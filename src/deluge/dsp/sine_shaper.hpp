/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

// Sine shaper DSP - extracted from util.hpp for maintainability
// This file contains all sine shaper waveshaping algorithms, zone logic,
// harmonic extraction, and buffer processing functions.

#include "dsp/fast_math.h" // For fastSinHalfPi (rect2 optimization)
#include "dsp/util.hpp"    // For smoothing helpers, polynomial primitives

namespace deluge::dsp {

// ============================================================================
// Sine Shaper Distortion
// ============================================================================
// A soft-clipping waveshaper using polynomial saturation with additional harmonics.
// - Drive: Input gain before shaping (controls saturation amount)
// - Harmonic: Adds odd harmonics via polynomial shaping
// - Symmetry: DC bias before shaping (adds even harmonics via asymmetry)
// - Mix: Wet/dry blend (0 = bypass processing entirely)
//
// Future Patched Param Design:
// ----------------------------
// Twist and Harmonic could become patched params with mod-matrix routing.
// Modulation should ADD to menu setting (not multiply), with full-scale spanning:
// - Twist (5 zones): 1 sub-zone width = 1/5 of q31 range (~429M)
// - Harmonic (8 zones, modulation targets 4-7): 4 zones = 1/2 of q31 range (~1.07B)
// This allows LFO/envelope modulation to sweep through a musically useful subset
// while the menu position establishes the base zone.

// Forward declaration for cached weights
struct ShaperWeights;

/// Sine shaper parameters and DSP state for one sound instance
/// Note: harmonic uses UNPATCHED_SINE_SHAPER_HARMONIC, twist uses UNPATCHED_SINE_SHAPER_TWIST
struct SineShaperParams {
	// User-facing parameters (0-127, converted to q31_t for DSP)
	uint8_t drive{0};     // Input gain / saturation amount
	uint8_t symmetry{64}; // DEPRECATED: kept for XML backwards compat, use Twist param instead
	uint8_t mix{0};       // Wet/dry blend (0 = bypass)
	// Meta zone phase offsets (per-patch, secret menus)
	float metaPhase{0};         // Single offset for all Twist param triangles (push Twist encoder)
	float metaPhaseHarmonic{0}; // Harmonic zone phase offset (push Harmonic encoder)
	float gammaPhase{0};        // Additional offset = 100*gamma, accessed via Mix encoder push
	// DSP smoothing state (per-sound, shared across voices)
	q31_t smoothedDrive{0};    // Previous drive value for parameter smoothing
	q31_t smoothedHarmonic{0}; // Previous harmonic value for parameter smoothing
	q31_t smoothedTwist{0};    // Previous Twist param value for parameter smoothing
	// Smoothed zone coefficients - reused across zones, zone boundary glitches are acceptable
	float smoothedC1{1.0f}, smoothedC3L{0}, smoothedC5L{0}, smoothedC7L{0}, smoothedC9L{0};
	// R channel (c1R = c1, so only c3R/c5R/c7R/c9R needed)
	float smoothedC3R{0}, smoothedC5R{0}, smoothedC7R{0}, smoothedC9R{0};
};

/// Per-voice state for sine shaper DSP (must be separate from per-sound params)
/// Used in Voice (per-voice) and GlobalEffectableForClip (per-clip)
struct SineShaperVoiceState {
	// DC blocker state (removes DC from asymmetry)
	q31_t dcBlockerL{0};
	q31_t dcBlockerR{0};
	// Feedback recirculation state (Twist Zone 3)
	q31_t feedbackL{0};
	q31_t feedbackR{0};
	// Zone 0 (Width) stereo LFO phase accumulator (0.0 to 1.0, wraps)
	float stereoLfoPhase{0.0f};
};

/// Output HPF coefficient for ~100Hz cutoff at 44.1kHz
/// alpha = 2π * fc / fs = 2π * 100 / 44100 ≈ 0.01425
/// Replaces 5Hz DC blocker - removes sub-bass rumble from waveshaping
/// Feedback taps post-HPF so inherits the filtering
constexpr q31_t kOutputHpfAlpha = static_cast<q31_t>(0.01425 * ONE_Q31);

/// Subtractive mode boost amount (bits)
/// Subtractive runs at >> 4 attenuation vs FM's << 3 boost (7 bits difference)
/// Start with 4 bits (~24dB) as conservative estimate, tune empirically
/// Pre-boost input, post-attenuate wet to normalize waveshaper operating point
constexpr int32_t kSubtractiveBoostBits = 4;

// Number of harmonic zones (0-6 = various, 7 = Poly)
constexpr int32_t kNumHarmonicZones = 8;

// Zone 1 "357" Chebyshev Harmonic Extraction
// See docs/dev/sine_shaper_chebyshev.md for detailed design rationale

/**
 * Generic 4-weight blend for triangle-phased parameter morphing
 *
 * Used by both Zone 1/2 (Chebyshev harmonics) and Zone 3 (FM modes).
 * Weights are computed using log-scaled triangles with irrational frequencies
 * to create smooth, non-periodic transitions through parameter space.
 */
struct BlendWeights4 {
	float w0; // First mode weight (Zone 1/2: T3, Zone 3: Add)
	float w1; // Second mode weight (Zone 1/2: T5, Zone 3: Ring)
	float w2; // Third mode weight (Zone 1/2: T7, Zone 3: FM)
	float w3; // Fourth mode weight (Zone 1/2: T9, Zone 3: Fold)
};

/**
 * Compute 4 normalized blend weights using triangle-phased modulation
 *
 * Uses log-scaled triangles with irrational frequency ratios to create
 * smooth, non-periodic transitions. The 4th weight uses a bipolar triangle
 * that alternates between w2 and w3 (7th/9th for Zone 1/2, FM/Fold for Zone 3).
 *
 * @param posInZone Position 0.0 to 1.0 within the zone
 * @return Normalized weights (sum to 1.0)
 */
inline BlendWeights4 computeBlendWeights4(float posInZone) {
	posInZone = std::clamp(posInZone, 0.0f, 1.0f);

	constexpr float kMinWeight = 0.01f; // -40dB floor (prevents div by zero)
	constexpr float kLogScale = 4.605f; // 2 * ln(10) for 40dB range
	constexpr float kDuty = 0.8f;       // 80% duty cycle

	// Triangle frequencies: ~2 cycles/zone with irrational ratios to avoid periodicity
	constexpr float kFreqW0 = 2.019f;   // √29/2 * 0.75 (~2.0 cycles/zone)
	constexpr float kFreqW1 = 2.356f;   // π * 0.75     (~2.4 cycles/zone)
	constexpr float kFreqW2_3 = 2.771f; // e²/2 * 0.75  (~2.8 cycles/zone) - shared for w2/w3

	// Convert linear triangle (0-1) to log-scaled weight (0.01-1.0, 40dB range)
	auto linearToLog = [](float linear) {
		if (linear <= 0.0f) {
			return kMinWeight;
		}
		return std::fmax(kMinWeight, fastExp((linear - 1.0f) * kLogScale));
	};

	// Pure float triangles - no q31 conversion overhead
	// tri0/tri1: unipolar (0→1) for linearToLog input
	float tri0 = triangleSimpleUnipolar(posInZone * kFreqW0, kDuty);
	float tri1 = triangleSimpleUnipolar(posInZone * kFreqW1 + 0.25f, kDuty);
	float w0 = linearToLog(tri0);
	float w1 = linearToLog(tri1);

	// w2 and w3 from bipolar triangle: positive = w2, negative = w3
	// tri23: bipolar (0→+1→0→-1→0) for sign-based weight selection
	// Note: log scaling maps values near zero to kMinWeight, so the hard switch is smooth
	float tri23 = triangleFloat(posInZone * kFreqW2_3 + 0.5f, kDuty);
	float w23_log = linearToLog(std::abs(tri23));
	float w2 = (tri23 > 0.0f) ? w23_log : kMinWeight;
	float w3 = (tri23 < 0.0f) ? w23_log : kMinWeight;

	// Normalize weights to sum to 1.0
	float wSum = w0 + w1 + w2 + w3;
	return {w0 / wSum, w1 / wSum, w2 / wSum, w3 / wSum};
}

/**
 * Precomputed blended polynomial coefficients for Zone 1/2 "3579"
 * Computed once per buffer using Horner's method for efficient per-sample evaluation
 *
 * The blended polynomial is: P(x) = c1*x + c3*x³ + c5*x⁵ + c7*x⁷ + c9*x⁹
 * Using Horner's method: P(x) = x * (c1 + x² * (c3 + x² * (c5 + x² * (c7 + c9*x²))))
 *
 * This reduces Zone 1/2 from ~94 to ~30 cycles/sample (comparable to TanH+ADAA)
 *
 * Zone 1 "3579": Raw input, unbounded output (edgy, integer overflow wraps)
 * Zone 2 "3579wm": Sine-preprocessed input, bounded output (warm, FM-like)
 *
 * Bipolar harmonic: positive uses w7 (7th), negative uses w9 (9th) - never both.
 * H9 contributes to c7 via its x⁷ term, so c7 is always computed.
 */
struct ShaperWeights {
	float c1; // Coefficient for x (cancels fundamental from higher-order terms)
	float c3; // Coefficient for x³
	float c5; // Coefficient for x⁵
	float c7; // Coefficient for x⁷ (from w7 or w9's H9 contribution)
	float c9; // Coefficient for x⁹ (only when w9 active, else 0)
};

/**
 * Helper to compute polynomial coefficients from normalized weights
 *
 * c1 must equal Σw to cancel fundamental from higher-order terms.
 * When x = sin(θ), each x^n term produces fundamental energy.
 * The normalized Chebyshev design ensures these cancel exactly when c1 = Σw.
 *
 * Normalized Chebyshev polynomials (extract sin(nθ)/n from sin(θ)):
 *   H3(x) = x - (4/3)x³
 *   H5(x) = x - 4x³ + 3.2x⁵
 *   H7(x) = x - 8x³ + 16x⁵ - (64/7)x⁷
 *   H9(x) = x - (40/3)x³ + 48x⁵ - 64x⁷ + (256/9)x⁹
 *
 * @param w3 Weight for 3rd harmonic
 * @param w5 Weight for 5th harmonic
 * @param w7 Weight for 7th harmonic (0 when using 9th)
 * @param w9 Weight for 9th harmonic (0 when using 7th)
 */
inline ShaperWeights weightsToCoeffs(float w3, float w5, float w7, float w9) {
	// Coefficients derived from weighted sum of normalized Chebyshev polynomials
	float c1 = w3 + w5 + w7 + w9;
	float c3 = -(w3 * (4.0f / 3.0f) + w5 * 4.0f + w7 * 8.0f + w9 * (40.0f / 3.0f));
	float c5 = w5 * 3.2f + w7 * 16.0f + w9 * 48.0f;
	float c7 = -w7 * (64.0f / 7.0f) - w9 * 64.0f;
	float c9 = w9 * (256.0f / 9.0f);
	return {c1, c3, c5, c7, c9};
}

/**
 * Compute Zone 1/2 blended polynomial coefficients from position in zone
 *
 * Uses triangle-phased weight blending with irrational frequencies
 * to create smooth, non-periodic transitions through harmonic space.
 *
 * @param posInZone Position 0.0 to 1.0 within the zone
 * @return Precomputed blended coefficients for Horner's method evaluation
 */
inline ShaperWeights computeShaperWeightsFromPos(float posInZone) {
	posInZone = std::clamp(posInZone, 0.0f, 1.0f);
	BlendWeights4 weights = computeBlendWeights4(posInZone);
	return weightsToCoeffs(weights.w0, weights.w1, weights.w2, weights.w3);
}

/**
 * Evaluate blended 3579 polynomial using Horner's method
 * P(x) = x * (c1 + x² * (c3 + x² * (c5 + x² * (c7 + c9*x²))))
 *
 * Shared by Zone 1 (raw) and Zone 2 (sine-preprocessed) to ensure
 * coefficient/algorithm changes apply to both zones automatically.
 *
 * When c9=0 (7th mode): inner term is just c7, no extra cost
 * When c7 comes only from w9: still need x⁷ term from H9's contribution
 *
 * @param x Preprocessed input (raw for Zone 1, sin(rawX * π/2) for Zone 2)
 * @param weights Precomputed polynomial coefficients
 * @return Polynomial result (may exceed [-1, 1] for Zone 1)
 */
[[gnu::always_inline]] inline float evaluate3579Polynomial(float x, const ShaperWeights& weights) {
	float x2 = x * x;
	// Horner's method: x * (c1 + x² * (c3 + x² * (c5 + x² * (c7 + c9*x²))))
	return x * (weights.c1 + x2 * (weights.c3 + x2 * (weights.c5 + x2 * (weights.c7 + weights.c9 * x2))));
}

/**
 * Derived values from Twist parameter for sine shaper
 * Computed once per buffer at call site
 *
 * Zone 0: Width - Stereo spread with animated phase evolution
 * Zone 1: Evens - Asymmetric compression for even harmonics
 * Zone 2: Rect - Blended rectifier (rect + rect2 with overlap)
 * Zone 3: Feedback - Output→input recirculation
 * Zone 4: Twist - Phase modulator for Harmonic zones (meta-control)
 */
struct SineShaperTwistParams {
	float stereoWidth{0.0f};       // Width: stereo spread envelope
	float stereoFreqMult{1.0f};    // Width: oscillation frequency multiplier
	float stereoPhaseOffset{0.0f}; // Width: continuous phase evolution
	float evenAmount{0.0f};        // Evens: positive compression amount
	float evenDryBlend{0.0f};      // Evens: negative dry blend amount
	float rectAmount{0.0f};        // Rect: rectifier blend
	float rect2Amount{0.0f};       // Rect: sine compression
	float feedbackAmount{0.0f};    // Feedback: depth (0.0 to 0.25)
	float phaseHarmonic{0.0f};     // Twist zone: offset for Harmonic knob
};

/**
 * Derive all Twist-dependent parameters from smoothed Twist value
 * Zones 0-3: Individual effects, Zone 4+: Meta (all effects combined)
 * @param params Optional - provides per-patch phase offsets for meta zone
 */
inline SineShaperTwistParams computeSineShaperTwistParams(q31_t smoothedTwist,
                                                          const SineShaperParams* ssParams = nullptr) {
	constexpr q31_t kZone1 = ONE_Q31 / 8;       // 1/8
	constexpr q31_t kZone2 = ONE_Q31 / 4;       // 2/8
	constexpr q31_t kZone3 = (ONE_Q31 / 8) * 3; // 3/8
	constexpr q31_t kZone4 = ONE_Q31 / 2;       // 4/8

	SineShaperTwistParams result;

	// Always apply metaPhaseHarmonic offset (Harmonic secret menu) - works in all zones
	// This allows cycling through harmonic zones even when Twist is in zones 0-3
	float phH = ssParams ? ssParams->metaPhaseHarmonic : 0.0f;
	result.phaseHarmonic = phH;

	if (smoothedTwist < kZone1) {
		// Zone 0: Width - stereo spread with animated phase evolution
		float pos = static_cast<float>(smoothedTwist) / static_cast<float>(kZone1);
		result.stereoWidth = (pos < 0.25f) ? pos * 4.0f : (pos < 0.75f) ? 1.0f : (1.0f - pos) * 4.0f;
		result.stereoFreqMult = (pos <= 0.75f) ? 1.0f : 1.0f + (pos - 0.75f) * 12.0f;
		result.stereoPhaseOffset = pos;
	}
	else if (smoothedTwist < kZone2) {
		// Zone 1: Evens - bipolar rectified triangle (duty=1.0: dry→evens1→dry→evens2)
		float pos = static_cast<float>(smoothedTwist - kZone1) / static_cast<float>(kZone2 - kZone1);
		float tri = triangleFloat(pos, 1.0f);
		float absTri = std::abs(tri);
		result.evenAmount = (tri > 0.0f) ? absTri : 0.0f;
		result.evenDryBlend = (tri < 0.0f) ? absTri : 0.0f;
	}
	else if (smoothedTwist < kZone3) {
		// Zone 2: Rect - bipolar rectified triangle (duty=1.0: dry→rect1→dry→rect2)
		float pos = static_cast<float>(smoothedTwist - kZone2) / static_cast<float>(kZone3 - kZone2);
		float tri = triangleFloat(pos, 1.0f);
		float absTri = std::abs(tri);
		result.rectAmount = (tri > 0.0f) ? absTri : 0.0f;
		result.rect2Amount = (tri < 0.0f) ? absTri : 0.0f;
	}
	else if (smoothedTwist < kZone4) {
		// Zone 3: Feedback (capped at 25%)
		float pos = static_cast<float>(smoothedTwist - kZone3) / static_cast<float>(kZone4 - kZone3);
		result.feedbackAmount = pos * 0.25f;
	}
	else {
		// Zone 4+: Meta - unified triangle evolution, all shift with (pos + ph)
		// Fractional φ powers for tighter frequency spacing (all irrational, never align)
		constexpr float kPhi025 = 1.1271566f;  // φ^0.25
		constexpr float kPhi033 = 1.1746627f;  // φ^0.33
		constexpr float kPhi050 = 1.2720196f;  // φ^0.5
		constexpr float kPhi067 = 1.3871872f;  // φ^0.67
		constexpr float kPhi075 = 1.4352958f;  // φ^0.75
		constexpr float kPhi100 = 1.6180340f;  // φ^1.0
		constexpr float kPhiN025 = 0.8872984f; // φ^-0.25
		constexpr float kPhiN050 = 0.7861513f; // φ^-0.5

		// Use double for ph wrapping to maintain precision at large gamma values (gamma < 10^15 ok)
		double phRaw = ssParams ? static_cast<double>(ssParams->metaPhase) + 100.0 * ssParams->gammaPhase : 0.0;
		float pos = static_cast<float>(smoothedTwist - kZone4) / static_cast<float>(ONE_Q31 - kZone4);

		// Scale and wrap ph per-frequency to preserve irrational divergence with large ph values
		// pos * freq maintains full precision, ph * freq is wrapped to 0-1 after scaling
		auto wrapPh = [](double ph, double freq) {
			double scaled = ph * freq;
			return static_cast<float>(scaled - std::floor(scaled));
		};
		float ph025 = wrapPh(phRaw, kPhi025);
		float ph033 = wrapPh(phRaw, kPhi033);
		float ph050 = wrapPh(phRaw, kPhi050);
		float ph067 = wrapPh(phRaw, kPhi067);
		float ph075 = wrapPh(phRaw, kPhi075);
		float ph100 = wrapPh(phRaw, kPhi100);
		float phN025 = wrapPh(phRaw, kPhiN025);
		float phN050 = wrapPh(phRaw, kPhiN050);

		// Per-effect freqMult: ramps 1.0→(1.25-1.5), peaks at pos=1, phXXX varies the peak
		float fmW = 1.0f + pos * (0.25f + 0.25f * ph025);
		float fmE = 1.0f + pos * (0.25f + 0.25f * ph033);
		float fmR = 1.0f + pos * (0.25f + 0.25f * ph067);
		float fmF = 1.0f + pos * (0.25f + 0.25f * phN025);

		result.phaseHarmonic += pos * 5.0f;

		// Phase offsets calculated so all triangles peak at pos=0.5 (end of effective zone 5), phi=0
		// offset = duty - (freq * 0.5 * 1.125) mod 1, where 1.125 = fmX at pos=0.5, phXXX=0

		// Width: scale(φ^0.25)*2 clipped * param(φ^0.5), duty 0.8/0.7 for broad coverage
		float wS = std::min(triangleSimpleUnipolar(pos * kPhi025 * fmW + ph025 + 0.166f, 0.8f) * 2.0f, 1.0f);
		float wP = triangleSimpleUnipolar(pos * kPhi050 * fmW + ph050 + 0.984f, 0.7f);
		result.stereoWidth = wS * wP;
		result.stereoPhaseOffset = triangleSimpleUnipolar(pos * kPhi067 * fmW + ph067 + 0.720f, 0.5f);
		result.stereoFreqMult = 1.0f + 0.5f * triangleSimpleUnipolar(pos * kPhi100 * fmW + ph100 + 0.590f);

		// Evens: bipolar rectified, scale(φ^0.33*0.5) * param(φ^0.75*0.5), sign selects mode
		float eS = triangleSimpleUnipolar(pos * kPhi033 * 0.5f * fmE + ph033 + 0.970f, 0.3f);
		float eT = triangleFloat(pos * kPhi075 * 0.5f * fmE + ph075 + 0.896f, 0.3f);
		float eAbs = std::abs(eT);
		result.evenAmount = eS * ((eT > 0.0f) ? eAbs : 0.0f);
		result.evenDryBlend = eS * ((eT < 0.0f) ? eAbs : 0.0f);

		// Rect: bipolar rectified, scale(φ^0.67*0.5) * param(φ^1.0*0.5), sign selects mode
		float rS = triangleSimpleUnipolar(pos * kPhi067 * 0.5f * fmR + ph067 + 0.910f, 0.3f);
		float rT = triangleFloat(pos * kPhi100 * 0.5f * fmR + ph100 + 0.845f, 0.3f);
		float rAbs = std::abs(rT);
		result.rectAmount = rS * ((rT > 0.0f) ? rAbs : 0.0f);
		result.rect2Amount = rS * ((rT < 0.0f) ? rAbs : 0.0f);

		// Feedback: scale(φ^-0.25) * min(param(φ^-0.5)² * 1.5, 1) * 0.25
		float fS = triangleSimpleUnipolar(pos * kPhiN025 * fmF + phN025 + 0.001f, 0.5f);
		float fP = triangleSimpleUnipolar(pos * kPhiN050 * fmF + phN050 + 0.058f, 0.5f);
		result.feedbackAmount = fS * std::min(fP * fP * 1.5f, 1.0f) * 0.25f;
	}

	return result;
}

/**
 * Core sine shaping without wet/dry mix (returns wet signal only)
 *
 * Drive follows saturator pattern with hybrid param (bipolar, additive modulation):
 * - Drive INT32_MIN = silence (minimum)
 * - Drive 0 (12 o'clock) = unity gain
 * - Drive INT32_MAX = 4x overdrive (maximum)
 *
 * Harmonic parameter selects zones:
 * - Zones 0-1 (Chebyshev): Triangle-modulated blend of T3, T5, T7, T9
 * - Zones 2-6 (FM): Various FM synthesis modes
 * - Zone 7 (Poly): Cascaded polynomial waveshaping
 *
 * Post-gain compensation ensures peak output matches wavefolder.
 *
 * @param zone1Weights Optional precomputed weights (hoisted from buffer loop)
 *                     All zones repurpose this struct (zone boundary glitches acceptable):
 *                       Zone 0/1: c1-c9 = Chebyshev polynomial coefficients
 *                       Zones 2-6: c1=inputGainMult, c3=w0, c5=w1, c7=w2, c9=w3
 *                       Zone 7: c1=cascadeBlend, c3=selfMulBlend
 *                     If nullptr, values are computed per sample
 * @param evenAmount Positive compression for even harmonics (Twist Zone 1, 0.0 to 1.0)
 * @param evenDryBlend Negative dry blend for even harmonics (Twist Zone 1, 0.0 to 1.0)
 * @param rectAmount Rectifier blend toward |result| (Twist Zone 2, 0.0 to 1.0)
 * @param rect2Amount Sine compression on positive (Twist Zone 2, 0.0 to 1.0)
 */
inline q31_t sineShapeCore(q31_t input, q31_t drive, q31_t harmonic, const ShaperWeights* zone1Weights = nullptr,
                           float evenAmount = 0.0f, float evenDryBlend = 0.0f, float rectAmount = 0.0f,
                           float rect2Amount = 0.0f, float phaseHarmonic = 0.0f) {

	// === Drive calculation for hybrid param (bipolar, additive modulation) ===
	// drive range: INT32_MIN to INT32_MAX (center=0 is unity)
	// Normalize to 0..1, then square to get volume-style curve
	// This gives: min→0, center→1, max→4 (same as old volume param behavior)
	float normalizedDrive = (static_cast<float>(drive) + 2147483648.0f) / 4294967296.0f; // 0 to 1
	float driveGain = normalizedDrive * normalizedDrive * 4.0f;                          // Square for volume curve

	// Apply drive to input
	float inputF = static_cast<float>(input) * driveGain;

	// Clamp and convert back to q31 for zones that need bounded input
	// Keep inputF unclamped for zone 3 which needs phase wrapping
	constexpr float kMaxQ31 = 2147483647.0f;
	float inputFClamped = std::clamp(inputF, -kMaxQ31, kMaxQ31);
	q31_t driven = static_cast<q31_t>(inputFClamped);

	// === Derive harmonic zone ===
	int32_t zone = computeZoneQ31(harmonic, kNumHarmonicZones).index;

	q31_t shaped;

	if (zone == 7) {
		// === Zone 7: Polynomial waveshaping with cascade blend + self-multiplication ===
		//
		// IMPORTANT: This must remain q31-based. Float conversion was attempted but failed.
		//
		// The polynomialOscillatorApproximation function applies P(x) = 6x - 8x³ twice
		// (internal double-cascade). It relies on 32-bit integer overflow/wrap behavior
		// for its characteristic sound. When input exceeds the linear region, the
		// polynomial blows up and wraps around, creating a specific wavefolding-like
		// saturation. Float doesn't have this natural saturation - values grow unbounded,
		// causing premature clipping and different harmonic content.
		//
		// A float version would need to explicitly model the q31 overflow behavior,
		// which adds complexity without benefit. Keep this as q31.

		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 8);

		// zone1Weights repurposed for Zone 7 (Poly): c1=cascadeBlend, c3=selfMulBlend
		// Zone boundary glitches are acceptable - smoothing is per-zone, not cross-zone
		float selfMulBlend = zone1Weights->c3;

		// Self-mul = input * |input| adds even harmonics before polynomial
		if (selfMulBlend > 0.01f) {
			q31_t absInput = (scaledInput < 0) ? -scaledInput : scaledInput;
			q31_t selfMulInput = multiply_32x32_rshift32_rounded(scaledInput, absInput) << 1;
			q31_t blendQ = static_cast<q31_t>(selfMulBlend * static_cast<float>(ONE_Q31));
			scaledInput = scaledInput + multiply_32x32_rshift32_rounded(selfMulInput - scaledInput, blendQ) * 2;
		}

		shaped = polynomialOscillatorApproximation(scaledInput) >> 8;

		// zone1Weights->c1 = cascadeBlend (0-1 float)
		q31_t cascadeBlend = static_cast<q31_t>(zone1Weights->c1 * static_cast<float>(ONE_Q31));

		if (cascadeBlend > 0) {
			q31_t moreHarmonics = polynomialOscillatorApproximation(shaped << 7) >> 7;
			shaped = shaped + multiply_32x32_rshift32_rounded(moreHarmonics - shaped, cascadeBlend) * 2;
		}

		// -3dB to match other zones (poly is naturally hotter)
		constexpr q31_t kMinus3dB = static_cast<q31_t>(0.707 * ONE_Q31);
		shaped = multiply_32x32_rshift32(shaped, kMinus3dB) << 1;
	}
	else if (zone == 0 || zone == 1) {
		// === Zone 0 "3579" / Zone 1 "3579wm": Blended Chebyshev T3, T5, T7, T9 ===
		// Zone 0: Raw input - clean, precise Chebyshev harmonics
		// Zone 1: Sine-preprocessed input - warmer, FM-like character
		//
		// Gain staging design:
		// - Chebyshev polynomials produce clean harmonics when |x| ≤ 1.0
		// - At |x| = 1.0: maximum harmonic energy, still mathematically clean
		// - At |x| > 1.0: polynomial explodes exponentially (not usable)
		// - Hard saturation keeps rawX bounded to ±1.0, preserving full clean range
		// - Soft clipping would compress the signal before reaching |x| = 1.0,
		//   reducing clean headroom and preventing maximum harmonic energy
		// - The "harsh" sound past threshold is flat-topped input, not polynomial
		//   explosion - it's feedback that you've exceeded the operating point
		//
		// << 9 puts sweet spot at ~80% drive knob (+8dB), giving room to
		// back off for subtle harmonics while harsh feedback indicates overdrive.
		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 9);
		float rawX = static_cast<float>(scaledInput) / static_cast<float>(ONE_Q31);

		// Zone 1 applies sine waveshaping via lookup table to bound input to [-1, 1]
		// Factor increased by 50%: sin(x * 3π/4) instead of sin(x * π/2) for more coloration
		float x;
		if (zone == 1) {
			// Convert q31 input to phase for sin(x * 3π/4) using lookup table
			// Phase = scaledInput * 0.75 (3π/4 is 3/8 of full cycle, scaled for q31 input)
			int32_t phaseInt = (scaledInput >> 1) + (scaledInput >> 2); // * 0.75
			uint32_t phase = static_cast<uint32_t>(phaseInt);

			// getSine returns ~q30 from 16-bit table interpolation
			int32_t sinValue = getSine(phase);

			// Convert to float with unity gain (getSine peak is ~ONE_Q31)
			constexpr float kInvQ31 = 1.0f / static_cast<float>(ONE_Q31);
			x = static_cast<float>(sinValue) * kInvQ31;
		}
		else {
			// Zone 0: use raw input (allows overdrive for intended harmonic character)
			x = rawX;
		}

		// Use precomputed blended coefficients (hoisted from buffer loop)
		// Shared polynomial evaluation - changes here apply to both zones
		float result = evaluate3579Polynomial(x, *zone1Weights);

		// Gain staging: match other zones' output level
		// Chebyshev peak varies by weight blend - tune empirically
		// Previous: >> 7 too quiet, 3.0 too loud, 0.5 still loud, 0.1 9dB hot, 0.035 3.5dB hot, 0.023 1.5dB quiet
		constexpr float kChebyBoost = 0.027f; // Tune empirically (+1.5dB from 0.023)
		constexpr float kMinus3dB = 0.707f;   // Match other zones
		constexpr float kChebyGain = kChebyBoost * kMinus3dB;

		double scaled = static_cast<double>(result * kChebyGain) * static_cast<double>(ONE_Q31);
		scaled = std::clamp(scaled, static_cast<double>(INT32_MIN), static_cast<double>(INT32_MAX));
		shaped = static_cast<q31_t>(scaled);
	}
	else if (zone >= 2 && zone <= 6) {
		// === Zones 2-6: Matrix-based FM synthesis (shared infrastructure) ===
		// Zone 2 (FM):   Add, Ring, FM, Fold - mixed character
		// Zone 3 (Fold): k=1,2,3,4 - wavefolder depths
		// Zone 4 (Ring): n=2,3,4,5 - ring mod frequency ratios
		// Zone 5 (Add):  n=2,3,4,5 - additive frequency ratios
		// Zone 6 (Mod):  FM depths d=0.25,0.5,0.75,1.0

		// Shared: extract weights (c3=w0, c5=w1, c7=w2, c9=w3)
		q31_t w0q = static_cast<q31_t>(zone1Weights->c3 * ONE_Q31);
		q31_t w1q = static_cast<q31_t>(zone1Weights->c5 * ONE_Q31);
		q31_t w2q = static_cast<q31_t>(zone1Weights->c7 * ONE_Q31);
		q31_t w3q = static_cast<q31_t>(zone1Weights->c9 * ONE_Q31);
		// inputGainMult (1.0-4.0) → q29 so 4.0 fits in q31
		q31_t inputGainQ = static_cast<q31_t>(zone1Weights->c1 * (ONE_Q31 >> 2));

		// Shared: apply input gain and compute base phase
		q31_t gained = multiply_32x32_rshift32(driven, inputGainQ) << 2;
		uint32_t phase1 = static_cast<uint32_t>(gained) << 8;
		q31_t sine1 = getSine(phase1);
		// Peak scaling: multiply_32x32_rshift32(ONE_Q31, ONE_Q31) = 0.5 peak
		// Ring mod naturally gives 0.5, others need kScale=1.0 to match
		constexpr q31_t kScale = ONE_Q31;

		// Zone-specific: compute 4 modes (each peaks at ~0.5)
		q31_t m0, m1, m2, m3;

		if (zone == 2) {
			// FM: Add(×2), Ring(×2), FM(×2), Fold(×2)
			uint32_t phase2 = phase1 << 1;
			q31_t sine2 = getSine(phase2);
			// Add: (s1+s2)/2 scaled to 0.5 peak
			m0 = multiply_32x32_rshift32((sine1 >> 1) + (sine2 >> 1), kScale);
			// Ring: s1 × s2(+90°) scaled to 0.5 peak
			m1 = multiply_32x32_rshift32(sine1, getSine(phase2 + 0x40000000u));
			// FM: sin(p1 + s2>>1) scaled to 0.5 peak
			m2 = multiply_32x32_rshift32(getSine(phase1 + static_cast<uint32_t>(sine2 >> 1)), kScale);
			// Fold: sin(s1<<1) scaled to 0.5 peak
			m3 = multiply_32x32_rshift32(getSine(static_cast<uint32_t>(sine1) << 1), kScale);
		}
		else if (zone == 3) {
			// Fold: k=1,2,3,4 (increasing fold depth) - all peak at 0.5
			m0 = multiply_32x32_rshift32(getSine(static_cast<uint32_t>(sine1)), kScale);
			m1 = multiply_32x32_rshift32(getSine(static_cast<uint32_t>(sine1) << 1), kScale);
			m2 = multiply_32x32_rshift32(getSine(static_cast<uint32_t>(sine1) + (static_cast<uint32_t>(sine1) << 1)),
			                             kScale);
			m3 = multiply_32x32_rshift32(getSine(static_cast<uint32_t>(sine1) << 2), kScale);
		}
		else if (zone == 4) {
			// Ring: sin(x) × sin(nx) for n=2,3,4,5 - ring mod peaks at 0.5
			m0 = multiply_32x32_rshift32(sine1, getSine(phase1 << 1));
			m1 = multiply_32x32_rshift32(sine1, getSine(phase1 + (phase1 << 1)));
			m2 = multiply_32x32_rshift32(sine1, getSine(phase1 << 2));
			m3 = multiply_32x32_rshift32(sine1, getSine((phase1 << 2) + phase1));
		}
		else if (zone == 5) {
			// Add: sin(x) + sin(nx) for n=2,3,4,5 - sum scaled to 0.5 peak
			q31_t s2 = getSine(phase1 << 1);
			q31_t s3 = getSine(phase1 + (phase1 << 1));
			q31_t s4 = getSine(phase1 << 2);
			q31_t s5 = getSine((phase1 << 2) + phase1);
			m0 = multiply_32x32_rshift32((sine1 >> 1) + (s2 >> 1), kScale);
			m1 = multiply_32x32_rshift32((sine1 >> 1) + (s3 >> 1), kScale);
			m2 = multiply_32x32_rshift32((sine1 >> 1) + (s4 >> 1), kScale);
			m3 = multiply_32x32_rshift32((sine1 >> 1) + (s5 >> 1), kScale);
		}
		else { // zone == 6
			// Mod: FM with depths d=0.25,0.5,0.75,1.0 (modulator at 2x) - all peak at 0.5
			q31_t mod = getSine(phase1 << 1);
			m0 = multiply_32x32_rshift32(getSine(phase1 + static_cast<uint32_t>(mod >> 2)), kScale);
			m1 = multiply_32x32_rshift32(getSine(phase1 + static_cast<uint32_t>(mod >> 1)), kScale);
			m2 = multiply_32x32_rshift32(getSine(phase1 + static_cast<uint32_t>((mod >> 1) + (mod >> 2))), kScale);
			m3 = multiply_32x32_rshift32(getSine(phase1 + static_cast<uint32_t>(mod)), kScale);
		}

		// Shared: blend with normalized weights and apply attenuation
		q31_t blended = 0;
		blended = add_saturate(blended, multiply_32x32_rshift32(m0, w0q) << 1);
		blended = add_saturate(blended, multiply_32x32_rshift32(m1, w1q) << 1);
		blended = add_saturate(blended, multiply_32x32_rshift32(m2, w2q) << 1);
		blended = add_saturate(blended, multiply_32x32_rshift32(m3, w3q) << 1);
		shaped = blended >> 7;
	}

	// === Even harmonics (Twist Zone 1) ===
	// Two phased effects like rect/rect2:
	// evenAmount: compress positive midrange → asymmetry → even harmonics
	// evenDryBlend: blend negative toward dry input → different harmonic content per half
	if (evenAmount > 0.0f || evenDryBlend > 0.0f) {
		float shapedF = static_cast<float>(shaped);
		float drivenF = static_cast<float>(driven); // Pre-zone input (dry harmonics)

		if (shapedF > 0.0f && evenAmount > 0.0f) {
			// Positive: compress midrange (peak-preserving quadratic blend)
			// Ratio = blendedX/x = 1 + (x-1)*evenAmount (eliminates division)
			constexpr float kNormFactor = 1.0f / static_cast<float>(ONE_Q31);
			constexpr float kBoost = 256.0f;
			float x = std::clamp(shapedF * kNormFactor * kBoost, 0.0f, 1.0f);

			// ratio = 1 - evenAmount + x*evenAmount = 1 + (x-1)*evenAmount
			// At x=1: ratio=1 (unchanged), at x=0: ratio=1-evenAmount (compressed)
			constexpr float kEvenGain = 0.85f;
			float ratio = 1.0f - evenAmount * (1.0f - x);
			shaped = static_cast<q31_t>(ratio * shapedF * kEvenGain);
		}
		else if (shapedF < 0.0f && evenDryBlend > 0.0f) {
			// Negative: blend magnitudes, preserve negative sign
			// Blend driven's magnitude into shaped's magnitude for different harmonic content
			float absShapedF = -shapedF; // We know shapedF < 0
			float absDrivenF = std::abs(drivenF);

			// Cap driven contribution to 2× shaped magnitude (prevents >> 7 mismatch issues)
			float cappedDrivenAbs = std::min(absDrivenF, absShapedF * 2.0f);
			float blendedAbs = absShapedF * (1.0f - evenDryBlend) + cappedDrivenAbs * evenDryBlend;
			shaped = static_cast<q31_t>(-blendedAbs); // Keep negative
		}
	}

	// === Rect + Rect2 (Twist Zone 2) - parallel processing ===
	// Both effects operate on original shaped, then combine additively
	// Rect: blend toward |shaped| for octave-up effect
	// Rect2: peak-cutting fold (subtracts peak² toward sign flip)
	if (rectAmount > 0.0f || rect2Amount > 0.0f) {
		float shapedF = static_cast<float>(shaped);
		float absVal = std::abs(shapedF);
		float result = shapedF;

		// Rect contribution: crossfade toward |shaped|
		if (rectAmount > 0.0f) {
			result = result + (absVal - shapedF) * rectAmount;
		}

		// Rect2 contribution: sine expansion on positive half (level-independent)
		// Applies sin(x*π/2)/x ratio which boosts midrange, preserves peaks
		// At x=1: ratio=1 (unchanged), at x<1: ratio>1 (boosted)
		// Boost compensates for zone attenuation (e.g., >> 7 in FM zones)
		if (rect2Amount > 0.0f && result > 0.0f) {
			constexpr float kNormFactor = 1.0f / static_cast<float>(ONE_Q31);
			constexpr float kBoost = 256.0f; // Compensate for attenuation
			float x = std::clamp(result * kNormFactor * kBoost, 0.0f, 1.0f);

			// Use fast polynomial sin approximation (~10x faster than std::sin)
			constexpr float kHalfPi = 1.5707963267949f;
			float sineX = fastSinHalfPi(x * kHalfPi);

			// Sine/linear ratio: preserves peaks (1 at x=1), boosts midrange
			// Blend between original and sine-scaled (transparent at rect2Amount=0)
			float sineRatio = (x > 0.01f) ? sineX / x : 1.0f;
			float sineResult = result * sineRatio;
			result = result * (1.0f - rect2Amount) + sineResult * rect2Amount;
		}

		shaped = static_cast<q31_t>(result);
	}

	return shaped;
}

/**
 * Process a mono buffer through the sine shaper with parameter smoothing
 *
 * @param buffer Audio buffer to process in place
 * @param drive Current target drive value
 * @param smoothedDrive Pointer to smoothed drive state (updated in place)
 * @param voiceState Pointer to per-voice state (DC blocker, feedback, etc.)
 * @param harmonic Raw harmonic value (smoothed internally)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param twist Twist parameters (evens, rect, feedback, phaseHarmonic)
 * @param params Pointer to SineShaperParams for coefficient smoothing
 * @param wasBypassed Pointer to bypass state flag (updated in place)
 * @param boostSubtractive If true, pre-boost input and post-attenuate wet to normalize
 *                         subtractive mode signal levels to match FM mode operating point
 */
inline void sineShapeBuffer(std::span<q31_t> buffer, q31_t drive, q31_t* smoothedDrive,
                            SineShaperVoiceState* voiceState, q31_t harmonic, q31_t mix,
                            const SineShaperTwistParams& twist, SineShaperParams* params, bool* wasBypassed = nullptr,
                            bool boostSubtractive = false) {
	// Early out - if mix is 0, do nothing (important CPU optimization)
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Per-sample drive interpolation for zipper-free parameter changes
	auto driveCtx = prepareSmoothing(*smoothedDrive, drive, buffer.size());
	// Write-back happens at end of function after per-sample IIR updates

	// Buffer-rate harmonic smoothing commented out - now relying only on per-sample IIR
	// for coefficients. This eliminates the "chasing" behavior where two IIR stages fight.
	// q31_t smoothedHarmonic = smoothParam(&params->smoothedHarmonic, harmonic);
	q31_t smoothedHarmonic = harmonic; // Use raw harmonic, let coefficient IIR do smoothing

	// Mark as no longer bypassed if transitioning
	if (wasBypassed && *wasBypassed) {
		*wasBypassed = false;
	}

	// Determine zone from raw harmonic (coefficients will smooth the transition)
	auto zoneInfo = computeZoneQ31(smoothedHarmonic, kNumHarmonicZones);
	int32_t zone = zoneInfo.index;
	float posInZone = zoneInfo.position;
	// Poly (zone 7) uses different crossfade, all others use Chebyshev-style
	bool useChebyshevCrossfade = (zone != 7);

	// Hoist zone weight calculation with per-sample coefficient interpolation
	// Per-sample smoothing eliminates zipper noise from triangle weight changes
	// Zone boundary glitches are acceptable - smoothing is per-zone, not cross-zone
	// All zones use same smoothing path and repurpose ShaperWeights:
	//   Zone 7 (Poly): c1=cascadeBlend, c3=selfMulBlend
	//   Zone 0/1 (Chebyshev): c1-c9 = Chebyshev coefficients
	//   Zones 2-6 (FM): c1=inputGainMult, c3=w0, c5=w1, c7=w2, c9=w3
	ShaperWeights shaperWeights{0, 0, 0, 0, 0};
	ShaperWeights targetWeights{0, 0, 0, 0, 0};

	if (zone == 7) {
		// Zone 7 (Poly): c1=cascadeBlend, c3=selfMulBlend
		targetWeights.c1 = posInZone;
		targetWeights.c3 = std::fmod(twist.phaseHarmonic + 0.5f + 1.0f, 1.0f);
	}
	else if (zone == 0 || zone == 1) {
		float pos = std::fmod(posInZone + twist.phaseHarmonic + 1.0f, 1.0f);
		targetWeights = computeShaperWeightsFromPos(pos);
	}
	else if (zone >= 2 && zone <= 6) {
		// Zones 2-6 (FM): c1=inputGainMult, c3=w0, c5=w1, c7=w2, c9=w3
		// All use same weight structure, differ only in DSP applied in sineShapeCore
		float pos = std::fmod(posInZone + twist.phaseHarmonic + 1.0f, 1.0f);
		BlendWeights4 w = computeBlendWeights4(pos);
		targetWeights.c1 = 1.0f + pos * 3.0f; // inputGainMult 1.0-4.0
		targetWeights.c3 = w.w0;
		targetWeights.c5 = w.w1;
		targetWeights.c7 = w.w2;
		targetWeights.c9 = w.w3;
	}

	// Single smoothing path for all zones
	FloatSmoothingContext c1Ctx = prepareSmoothingFloat(params->smoothedC1, targetWeights.c1, buffer.size());
	FloatSmoothingContext c3Ctx = prepareSmoothingFloat(params->smoothedC3L, targetWeights.c3, buffer.size());
	FloatSmoothingContext c5Ctx = prepareSmoothingFloat(params->smoothedC5L, targetWeights.c5, buffer.size());
	FloatSmoothingContext c7Ctx = prepareSmoothingFloat(params->smoothedC7L, targetWeights.c7, buffer.size());
	FloatSmoothingContext c9Ctx = prepareSmoothingFloat(params->smoothedC9L, targetWeights.c9, buffer.size());

	shaperWeights = {c1Ctx.current, c3Ctx.current, c5Ctx.current, c7Ctx.current, c9Ctx.current};

	// Local copy of state for efficient per-sample update
	// If voiceState is null, use local zeros (no persistent state across buffers)
	q31_t dcState = voiceState ? voiceState->dcBlockerL : 0;
	q31_t fbState = voiceState ? voiceState->feedbackL : 0;
	q31_t currentDrive = driveCtx.current;

	// Scale feedback amount for moderate self-oscillation range
	// At max (1.0), feedback is ~0.9 of output (just below self-oscillation)
	// In Zone 7 (Poly), feedback is inversely proportional to harmonic position (more cascade = less feedback)
	// Also reduce feedback by up to 10% as drive increases (tames high-drive feedback)
	constexpr float kFeedbackScale = 0.25f;
	float fbScale = kFeedbackScale;
	constexpr q31_t kZone7Start = ONE_Q31 - (ONE_Q31 / 8); // 7/8 without overflow
	if (smoothedHarmonic >= kZone7Start) {
		float zone7Pos = static_cast<float>(smoothedHarmonic - kZone7Start) / static_cast<float>(ONE_Q31 - kZone7Start);
		fbScale *= (1.0f - zone7Pos); // 100% at start, 0% at end of Zone 7
		fbScale *= 0.707f;            // -3dB for Zone 7 (poly is hotter)
	}
	// Drive-based reduction: up to 10% at max drive (tames high-drive feedback)
	float driveNorm = static_cast<float>(drive) / static_cast<float>(ONE_Q31);
	fbScale *= (1.0f - 0.1f * driveNorm);
	q31_t fbAmount = static_cast<q31_t>(twist.feedbackAmount * fbScale * static_cast<float>(ONE_Q31));

	for (auto& sample : buffer) {
		// Apply feedback to input (before shaping)
		// Pre-boost for subtractive mode to normalize operating point with FM
		q31_t inputWithFb = boostSubtractive ? lshiftAndSaturate<kSubtractiveBoostBits>(sample) : sample;
		if (fbAmount > 0) {
			// Apply feedback directly (HPF on tap prevents fundamental buildup)
			q31_t fb = signed_saturate<22>(multiply_32x32_rshift32(fbState, fbAmount) << 1);
			inputWithFb = add_saturate(inputWithFb, fb);
			// Clamp combined signal to q31 range
			inputWithFb = std::clamp(inputWithFb, -ONE_Q31, ONE_Q31);
		}

		// Get shaped (wet) signal with all modifiers (drive + weights interpolated per-sample)
		// All zones use shaperWeights (repurposed per zone, see comment above)
		q31_t shaped = sineShapeCore(inputWithFb, currentDrive, smoothedHarmonic, &shaperWeights, twist.evenAmount,
		                             twist.evenDryBlend, twist.rectAmount, twist.rect2Amount, twist.phaseHarmonic);
		// Per-sample IIR update for drive
		currentDrive += multiply_32x32_rshift32(driveCtx.target - currentDrive, driveCtx.alpha) * 2;

		// Update weights for next sample (per-sample IIR, unified path)
		c1Ctx.current += (c1Ctx.target - c1Ctx.current) * c1Ctx.alpha;
		c3Ctx.current += (c3Ctx.target - c3Ctx.current) * c3Ctx.alpha;
		c5Ctx.current += (c5Ctx.target - c5Ctx.current) * c5Ctx.alpha;
		c7Ctx.current += (c7Ctx.target - c7Ctx.current) * c7Ctx.alpha;
		c9Ctx.current += (c9Ctx.target - c9Ctx.current) * c9Ctx.alpha;
		shaperWeights.c1 = c1Ctx.current;
		shaperWeights.c3 = c3Ctx.current;
		shaperWeights.c5 = c5Ctx.current;
		shaperWeights.c7 = c7Ctx.current;
		shaperWeights.c9 = c9Ctx.current;

		// 100Hz HPF on wet signal only - removes sub-bass rumble without affecting dry
		dcState += multiply_32x32_rshift32(shaped - dcState, kOutputHpfAlpha) * 2;
		q31_t hpfWet = shaped - dcState;

		// Feedback tap with 1-pole LPF (~4.9kHz) to tame harsh harmonics
		// Uses shifts for efficiency: α=0.5 → fbState = 0.5*old + 0.5*new
		fbState = (fbState + hpfWet) >> 1;

		// Post-attenuate wet signal for subtractive mode (inverse of pre-boost)
		// This normalizes the waveshaper operating point without changing output level
		// Applied AFTER feedback tap so feedback operates at internal waveshaper level
		if (boostSubtractive) {
			hpfWet >>= kSubtractiveBoostBits;
		}

		q31_t mixed;
		if (useChebyshevCrossfade) {
			// Chebyshev zones: traditional wet/dry crossfade
			// At mix=0: pure dry, mix=1: pure wet
			q31_t dryCoeff = ONE_Q31 - multiply_32x32_rshift32(mix, ONE_Q31) * 2; // 1 - mix
			q31_t dryPart = multiply_32x32_rshift32(sample, dryCoeff) << 1;
			q31_t wetPart = multiply_32x32_rshift32(hpfWet, mix) << 1;
			mixed = add_saturate(dryPart, wetPart);
		}
		else {
			// Zone 7 (Poly): Traditional wet/dry crossfade
			q31_t invMix = ONE_Q31 - mix;
			q31_t dryPart = multiply_32x32_rshift32(sample, invMix) << 1;
			q31_t wetPart = multiply_32x32_rshift32(hpfWet, mix) << 1;
			mixed = add_saturate(dryPart, wetPart);
		}

		sample = mixed;
	}

	// Write back state if voiceState is not null
	if (voiceState) {
		voiceState->dcBlockerL = dcState;
		voiceState->feedbackL = fbState;
	}
	// Write back smoothed params (shared across zones, zone boundary glitches acceptable)
	// Per-sample IIR: write final current, not target (current is where we actually got to)
	params->smoothedC1 = c1Ctx.current;
	params->smoothedC3L = c3Ctx.current;
	params->smoothedC5L = c5Ctx.current;
	params->smoothedC7L = c7Ctx.current;
	params->smoothedC9L = c9Ctx.current;
	*smoothedDrive = currentDrive; // Write back final drive value
}

/**
 * Process a stereo buffer through the sine shaper with parameter smoothing
 *
 * Drive and Harmonic are smoothed internally.
 * Twist should be smoothed at the call site for consistent handling.
 *
 * @param buffer Stereo audio buffer to process in place
 * @param drive Current target drive value
 * @param smoothedDrive Pointer to smoothed drive state (updated in place)
 * @param voiceState Pointer to per-voice state (DC blocker, feedback, LFO, etc.)
 * @param harmonic Raw harmonic value (smoothed internally via params->smoothedHarmonic)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param twist Twist parameters (stereo, evens, rect, feedback, phaseHarmonic)
 * @param params Pointer to SineShaperParams for coefficient smoothing (required)
 * @param wasBypassed Pointer to bypass state flag (updated in place)
 * @param boostSubtractive If true, pre-boost input and post-attenuate wet to normalize
 *                         subtractive mode signal levels to match FM mode operating point
 */
inline void sineShapeBuffer(StereoBuffer<q31_t> buffer, q31_t drive, q31_t* smoothedDrive,
                            SineShaperVoiceState* voiceState, q31_t harmonic, q31_t mix,
                            const SineShaperTwistParams& twist, SineShaperParams* params, bool* wasBypassed = nullptr,
                            bool boostSubtractive = false) {
	// Early out - if mix is 0, do nothing (important CPU optimization)
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Per-sample drive interpolation for zipper-free parameter changes
	auto driveCtx = prepareSmoothing(*smoothedDrive, drive, buffer.size());
	// Write-back happens at end of function after per-sample IIR updates

	// Buffer-rate harmonic smoothing commented out - now relying only on per-sample IIR
	// for coefficients. This eliminates the "chasing" behavior where two IIR stages fight.
	// q31_t smoothedHarmonic = smoothParam(&params->smoothedHarmonic, harmonic);
	q31_t smoothedHarmonic = harmonic; // Use raw harmonic, let coefficient IIR do smoothing

	// Mark as no longer bypassed if transitioning
	if (wasBypassed && *wasBypassed) {
		*wasBypassed = false;
	}

	// Determine zone from raw harmonic (coefficients will smooth the transition)
	auto zoneInfo = computeZoneQ31(smoothedHarmonic, kNumHarmonicZones);
	int32_t zone = zoneInfo.index;
	float posInZone = zoneInfo.position;
	// Poly (zone 7) uses different crossfade, all others use Chebyshev-style
	bool useChebyshevCrossfade = (zone != 7);

	// Hoist zone weight calculation with per-sample coefficient interpolation
	// Per-sample smoothing eliminates zipper noise from triangle weight changes
	// Zone boundary glitches are acceptable - smoothing is per-zone, not cross-zone
	// All zones use same smoothing path and repurpose ShaperWeights:
	//   Zone 7 (Poly): c1=cascadeBlend, c3=selfMulBlend
	//   Zone 0/1 (Chebyshev): c1-c9 = Chebyshev coefficients
	//   Zones 2-6 (FM): c1=inputGainMult, c3=w0, c5=w1, c7=w2, c9=w3
	ShaperWeights shaperWeightsL{0, 0, 0, 0, 0};
	ShaperWeights shaperWeightsR{0, 0, 0, 0, 0};
	ShaperWeights targetWeightsL{0, 0, 0, 0, 0};
	ShaperWeights targetWeightsR{0, 0, 0, 0, 0};

	// Compute stereo position offset for zones that use it
	float stereoMod = triangleFloat(twist.stereoPhaseOffset * twist.stereoFreqMult);

	if (zone == 7) {
		// Zone 7 (Poly): c1=cascadeBlend, c3=selfMulBlend (L/R identical)
		targetWeightsL.c1 = posInZone;
		targetWeightsL.c3 = std::fmod(twist.phaseHarmonic + 0.5f + 1.0f, 1.0f);
		targetWeightsR = targetWeightsL;
	}
	else if (zone == 0 || zone == 1) {
		float pos = std::fmod(posInZone + twist.phaseHarmonic + 1.0f, 1.0f);

		// Position-based stereo: L and R at different positions
		constexpr float kMaxPosOffset = 0.06f;
		float posOffset = twist.stereoWidth * stereoMod * kMaxPosOffset;
		float posL = std::clamp(pos - posOffset, 0.0f, 1.0f);
		float posR = std::clamp(pos + posOffset, 0.0f, 1.0f);

		targetWeightsL = computeShaperWeightsFromPos(posL);
		targetWeightsR = computeShaperWeightsFromPos(posR);
	}
	else if (zone >= 2 && zone <= 6) {
		// Zones 2-6 (FM): c1=inputGainMult, c3=w0, c5=w1, c7=w2, c9=w3
		// All use same weight structure, differ only in DSP applied in sineShapeCore
		float pos = std::fmod(posInZone + twist.phaseHarmonic + 1.0f, 1.0f);

		// Position-based stereo
		constexpr float kMaxPosOffset = 0.08f;
		float posOffset = twist.stereoWidth * stereoMod * kMaxPosOffset;
		float posL = std::clamp(pos - posOffset, 0.0f, 1.0f);
		float posR = std::clamp(pos + posOffset, 0.0f, 1.0f);

		BlendWeights4 wL = computeBlendWeights4(posL);
		BlendWeights4 wR = computeBlendWeights4(posR);
		float inputGain = 1.0f + pos * 3.0f;

		targetWeightsL = {inputGain, wL.w0, wL.w1, wL.w2, wL.w3};
		targetWeightsR = {inputGain, wR.w0, wR.w1, wR.w2, wR.w3};
	}

	// Single smoothing path for all zones (L channel)
	FloatSmoothingContext c1Ctx = prepareSmoothingFloat(params->smoothedC1, targetWeightsL.c1, buffer.size());
	FloatSmoothingContext c3LCtx = prepareSmoothingFloat(params->smoothedC3L, targetWeightsL.c3, buffer.size());
	FloatSmoothingContext c5LCtx = prepareSmoothingFloat(params->smoothedC5L, targetWeightsL.c5, buffer.size());
	FloatSmoothingContext c7LCtx = prepareSmoothingFloat(params->smoothedC7L, targetWeightsL.c7, buffer.size());
	FloatSmoothingContext c9LCtx = prepareSmoothingFloat(params->smoothedC9L, targetWeightsL.c9, buffer.size());

	// R channel (c1 shared)
	FloatSmoothingContext c3RCtx = prepareSmoothingFloat(params->smoothedC3R, targetWeightsR.c3, buffer.size());
	FloatSmoothingContext c5RCtx = prepareSmoothingFloat(params->smoothedC5R, targetWeightsR.c5, buffer.size());
	FloatSmoothingContext c7RCtx = prepareSmoothingFloat(params->smoothedC7R, targetWeightsR.c7, buffer.size());
	FloatSmoothingContext c9RCtx = prepareSmoothingFloat(params->smoothedC9R, targetWeightsR.c9, buffer.size());

	shaperWeightsL = {c1Ctx.current, c3LCtx.current, c5LCtx.current, c7LCtx.current, c9LCtx.current};
	shaperWeightsR = {c1Ctx.current, c3RCtx.current, c5RCtx.current, c7RCtx.current, c9RCtx.current};

	// Poly zone (7) stereo: modulate drive bipolar (L gets +, R gets -)
	// Time-based LFO with frequency modulated by stereoFreqMult
	constexpr q31_t kPolyStereoDriveScale = ONE_Q31 / 8; // ±12.5% drive modulation
	q31_t stereoDriveOffset = 0;

	if (!useChebyshevCrossfade && twist.stereoWidth > 0.0f) { // Poly zone only
		// LFO frequency: 0.1Hz min to 2Hz max (linear ramp through Wide zone)
		constexpr float kMinLfoHz = 0.1f;
		constexpr float kMaxLfoHz = 2.0f;
		constexpr float kSampleRate = 44100.0f;
		// stereoFreqMult: 1.0 at zone start → 4.0 at zone end, map to 0.1-2Hz
		float lfoHz = kMinLfoHz + (twist.stereoFreqMult - 1.0f) * (kMaxLfoHz - kMinLfoHz) / 3.0f;
		float phaseIncrement = (lfoHz * static_cast<float>(buffer.size())) / kSampleRate;

		// Advance and wrap phase
		float phase = voiceState ? voiceState->stereoLfoPhase : 0.0f;
		phase += phaseIncrement;
		if (phase >= 1.0f) {
			phase -= 1.0f;
		}
		if (voiceState) {
			voiceState->stereoLfoPhase = phase;
		}

		// LFO output modulates drive offset (bipolar: L+, R-)
		// Triangle for efficient CPU and consistent with other parameter modulation
		float stereoMod = triangleFloat(phase);
		stereoDriveOffset =
		    static_cast<q31_t>(twist.stereoWidth * stereoMod * static_cast<float>(kPolyStereoDriveScale));
	}

	// Local copy of state for efficient per-sample update
	// If voiceState is null, use local zeros (no persistent state across buffers)
	q31_t dcStateL = voiceState ? voiceState->dcBlockerL : 0;
	q31_t dcStateR = voiceState ? voiceState->dcBlockerR : 0;
	q31_t fbStateL = voiceState ? voiceState->feedbackL : 0;
	q31_t fbStateR = voiceState ? voiceState->feedbackR : 0;
	q31_t currentDrive = driveCtx.current;
	q31_t driveL = currentDrive;
	q31_t driveR = currentDrive;

	// Scale feedback amount for moderate self-oscillation range
	// At max (1.0), feedback is ~0.9 of output (just below self-oscillation)
	// In Zone 7 (Poly), feedback is inversely proportional to harmonic position (more cascade = less feedback)
	// Also reduce feedback by up to 10% as drive increases (tames high-drive feedback)
	constexpr float kFeedbackScale = 0.25f;
	float fbScale = kFeedbackScale;
	constexpr q31_t kZone7Start = ONE_Q31 - (ONE_Q31 / 8); // 7/8 without overflow
	if (smoothedHarmonic >= kZone7Start) {
		float zone7Pos = static_cast<float>(smoothedHarmonic - kZone7Start) / static_cast<float>(ONE_Q31 - kZone7Start);
		fbScale *= (1.0f - zone7Pos); // 100% at start, 0% at end of Zone 7
		fbScale *= 0.707f;            // -3dB for Zone 7 (poly is hotter)
	}
	// Drive-based reduction: -10% at max drive
	float driveNorm = static_cast<float>(drive) / static_cast<float>(ONE_Q31);
	fbScale *= (1.0f - 0.1f * driveNorm);
	q31_t fbAmount = static_cast<q31_t>(twist.feedbackAmount * fbScale * static_cast<float>(ONE_Q31));

	for (auto& sample : buffer) {
		// Apply bipolar stereo drive offset (L gets +, R gets -)
		driveL = add_saturate(currentDrive, stereoDriveOffset);
		driveR = add_saturate(currentDrive, -stereoDriveOffset);

		// Apply feedback to input (before shaping)
		// Pre-boost for subtractive mode to normalize operating point with FM
		q31_t inputL = boostSubtractive ? lshiftAndSaturate<kSubtractiveBoostBits>(sample.l) : sample.l;
		q31_t inputR = boostSubtractive ? lshiftAndSaturate<kSubtractiveBoostBits>(sample.r) : sample.r;
		if (fbAmount > 0) {
			// Apply feedback directly (HPF on tap prevents fundamental buildup)
			q31_t fbL = signed_saturate<22>(multiply_32x32_rshift32(fbStateL, fbAmount) << 1);
			q31_t fbR = signed_saturate<22>(multiply_32x32_rshift32(fbStateR, fbAmount) << 1);
			inputL = add_saturate(inputL, fbL);
			inputR = add_saturate(inputR, fbR);
			// Clamp combined signal to q31 range
			inputL = std::clamp(inputL, -ONE_Q31, ONE_Q31);
			inputR = std::clamp(inputR, -ONE_Q31, ONE_Q31);
		}

		// Process left channel with L drive
		// All zones use shaperWeights (repurposed per zone, see comment above)
		q31_t shapedL = sineShapeCore(inputL, driveL, smoothedHarmonic, &shaperWeightsL, twist.evenAmount,
		                              twist.evenDryBlend, twist.rectAmount, twist.rect2Amount, twist.phaseHarmonic);
		q31_t shapedR = sineShapeCore(inputR, driveR, smoothedHarmonic, &shaperWeightsR, twist.evenAmount,
		                              twist.evenDryBlend, twist.rectAmount, twist.rect2Amount, twist.phaseHarmonic);
		// Per-sample IIR update for drive
		currentDrive += multiply_32x32_rshift32(driveCtx.target - currentDrive, driveCtx.alpha) * 2;

		// Update weights for next sample (per-sample IIR, unified path)
		c1Ctx.current += (c1Ctx.target - c1Ctx.current) * c1Ctx.alpha;
		c3LCtx.current += (c3LCtx.target - c3LCtx.current) * c3LCtx.alpha;
		c5LCtx.current += (c5LCtx.target - c5LCtx.current) * c5LCtx.alpha;
		c7LCtx.current += (c7LCtx.target - c7LCtx.current) * c7LCtx.alpha;
		c9LCtx.current += (c9LCtx.target - c9LCtx.current) * c9LCtx.alpha;
		c3RCtx.current += (c3RCtx.target - c3RCtx.current) * c3RCtx.alpha;
		c5RCtx.current += (c5RCtx.target - c5RCtx.current) * c5RCtx.alpha;
		c7RCtx.current += (c7RCtx.target - c7RCtx.current) * c7RCtx.alpha;
		c9RCtx.current += (c9RCtx.target - c9RCtx.current) * c9RCtx.alpha;

		shaperWeightsL.c1 = c1Ctx.current;
		shaperWeightsL.c3 = c3LCtx.current;
		shaperWeightsL.c5 = c5LCtx.current;
		shaperWeightsL.c7 = c7LCtx.current;
		shaperWeightsL.c9 = c9LCtx.current;
		shaperWeightsR.c1 = c1Ctx.current;
		shaperWeightsR.c3 = c3RCtx.current;
		shaperWeightsR.c5 = c5RCtx.current;
		shaperWeightsR.c7 = c7RCtx.current;
		shaperWeightsR.c9 = c9RCtx.current;

		// 100Hz HPF on wet signal only - removes sub-bass rumble without affecting dry
		dcStateL += multiply_32x32_rshift32(shapedL - dcStateL, kOutputHpfAlpha) * 2;
		dcStateR += multiply_32x32_rshift32(shapedR - dcStateR, kOutputHpfAlpha) * 2;
		q31_t hpfWetL = shapedL - dcStateL;
		q31_t hpfWetR = shapedR - dcStateR;

		// Feedback tap with 1-pole LPF (~4.9kHz) to tame harsh harmonics
		// Uses shifts for efficiency: α=0.5 → fbState = 0.5*old + 0.5*new
		fbStateL = (fbStateL + hpfWetL) >> 1;
		fbStateR = (fbStateR + hpfWetR) >> 1;

		// Post-attenuate wet signal for subtractive mode (inverse of pre-boost)
		// This normalizes the waveshaper operating point without changing output level
		// Applied AFTER feedback tap so feedback operates at internal waveshaper level
		if (boostSubtractive) {
			hpfWetL >>= kSubtractiveBoostBits;
			hpfWetR >>= kSubtractiveBoostBits;
		}

		q31_t mixedL, mixedR;
		if (useChebyshevCrossfade) {
			// Chebyshev zones: traditional wet/dry crossfade
			// At mix=0: pure dry, mix=1: pure wet
			q31_t dryCoeff = ONE_Q31 - multiply_32x32_rshift32(mix, ONE_Q31) * 2; // 1 - mix
			mixedL = add_saturate(multiply_32x32_rshift32(sample.l, dryCoeff) << 1,
			                      multiply_32x32_rshift32(hpfWetL, mix) << 1);
			mixedR = add_saturate(multiply_32x32_rshift32(sample.r, dryCoeff) << 1,
			                      multiply_32x32_rshift32(hpfWetR, mix) << 1);
		}
		else {
			// Zone 7 (Poly): Traditional wet/dry crossfade
			q31_t invMix = ONE_Q31 - mix;
			mixedL = add_saturate(multiply_32x32_rshift32(sample.l, invMix) << 1, multiply_32x32_rshift32(hpfWetL, mix)
			                                                                          << 1);
			mixedR = add_saturate(multiply_32x32_rshift32(sample.r, invMix) << 1, multiply_32x32_rshift32(hpfWetR, mix)
			                                                                          << 1);
		}

		sample.l = mixedL;
		sample.r = mixedR;
	}

	// Write back state if voiceState is not null
	if (voiceState) {
		voiceState->dcBlockerL = dcStateL;
		voiceState->dcBlockerR = dcStateR;
		voiceState->feedbackL = fbStateL;
		voiceState->feedbackR = fbStateR;
	}
	// Write back smoothed params (shared across zones, zone boundary glitches acceptable)
	// Per-sample IIR: write final current, not target (current is where we actually got to)
	params->smoothedC1 = c1Ctx.current;
	params->smoothedC3L = c3LCtx.current;
	params->smoothedC5L = c5LCtx.current;
	params->smoothedC7L = c7LCtx.current;
	params->smoothedC9L = c9LCtx.current;
	params->smoothedC3R = c3RCtx.current;
	params->smoothedC5R = c5RCtx.current;
	params->smoothedC7R = c7RCtx.current;
	params->smoothedC9R = c9RCtx.current;
	*smoothedDrive = currentDrive; // Write back final drive value
}

} // namespace deluge::dsp
