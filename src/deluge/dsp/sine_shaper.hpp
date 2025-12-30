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

#include "dsp/util.hpp" // For smoothing helpers, polynomial primitives

namespace deluge::dsp {

// ============================================================================
// Sine Shaper Distortion
// ============================================================================
// A soft-clipping waveshaper using polynomial saturation with additional harmonics.
// - Drive: Input gain before shaping (controls saturation amount)
// - Harmonic: Adds odd harmonics via polynomial shaping
// - Symmetry: DC bias before shaping (adds even harmonics via asymmetry)
// - Mix: Wet/dry blend (0 = bypass processing entirely)

// Forward declaration for cached weights
struct Zone1Weights;

/// Sine shaper parameters and DSP state for one sound instance
/// Note: harmonic uses UNPATCHED_SINE_SHAPER_HARMONIC, twist uses UNPATCHED_SINE_SHAPER_TWIST
struct SineShaperParams {
	// User-facing parameters (0-127, converted to q31_t for DSP)
	uint8_t drive{0};     // Input gain / saturation amount
	uint8_t symmetry{64}; // DEPRECATED: kept for XML backwards compat, use Twist param instead
	uint8_t mix{0};       // Wet/dry blend (0 = bypass)
	// DSP smoothing state (per-sound, shared across voices)
	q31_t smoothedDrive{0};    // Previous drive value for parameter smoothing
	q31_t smoothedHarmonic{0}; // Previous harmonic value for parameter smoothing
	q31_t smoothedTwist{0};    // Previous Twist param value for parameter smoothing
	// Smoothed zone coefficients (prevents clicks when triangle weights jump)
	// Reused by Zone 0 (phaseHarmonic), Zone 1/2 (c1,c3,c5,c7,c9), Zone 3 (w0,w1,w2,w3,inputGain)
	// L channel (c1/inputGain shared with R)
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
	// Feedback recirculation state (Twist Zone 5)
	q31_t feedbackL{0};
	q31_t feedbackR{0};
	// Feedback LPF state (tames harsh high harmonics in feedback loop)
	q31_t feedbackLpfL{0};
	q31_t feedbackLpfR{0};
	// Zone 1 stereo LFO phase accumulator (0.0 to 1.0, wraps)
	float stereoLfoPhase{0.0f};
};

// Number of harmonic zones (0 = Poly, 1-7 = Chebyshev with triangle modulation)
constexpr int32_t kNumHarmonicZones = 8;

/**
 * Harmonic zone mapper for sine shaper
 *
 * Zone 0: Poly - Uses cascaded polynomial (existing behavior)
 * Zones 1-7: Chebyshev - Triangle-modulated blend of T3, T5, T7, T9
 *
 * Triangle oscillators with irrational periods create non-repeating patterns.
 * Duty cycles create dead zones where harmonics are OFF (saves compute).
 * Higher zones emphasize higher harmonics via phase offsets.
 */
struct SineShaperHarmonicMapper {
	// Phase width for each harmonic (active region width)
	// 70% duty balances harmonic variety with smooth transitions
	static constexpr uint32_t kPhaseWidthT3 = 0xB3333333u; // 70% duty
	static constexpr uint32_t kPhaseWidthT5 = 0xB3333333u; // 70% duty
	static constexpr uint32_t kPhaseWidthT7 = 0x99999999u; // 60% duty
	static constexpr uint32_t kPhaseWidthT9 = 0x80000000u; // 50% duty

	// Irrational period ratios for non-repeating patterns (halved for smoother transitions)
	static constexpr float kPeriodT3 = 1.359f;  // e/2
	static constexpr float kPeriodT5 = 1.5708f; // π/2
	static constexpr float kPeriodT7 = 1.118f;  // √5/2
	static constexpr float kPeriodT9 = 0.809f;  // φ/2 (golden ratio)

	// Phase offsets to spread harmonics across the parameter range
	static constexpr float kPhaseT3 = 0.0f;
	static constexpr float kPhaseT5 = 0.25f;
	static constexpr float kPhaseT7 = 0.5f;
	static constexpr float kPhaseT9 = 0.75f;

	/// Derive harmonic weights from zone parameter
	/// @param harmonicParam Raw harmonic parameter (0 to ONE_Q31)
	/// @param outZone Output zone number (0-7)
	/// @param outT3 Output T3 weight (q31)
	/// @param outT5 Output T5 weight
	/// @param outT7 Output T7 weight
	/// @param outT9 Output T9 weight
	static void deriveWeights(q31_t harmonicParam, int32_t& outZone, q31_t& outT3, q31_t& outT5, q31_t& outT7,
	                          q31_t& outT9, float phaseOffset = 0.0f) {
		float paramNorm = static_cast<float>(harmonicParam) / static_cast<float>(ONE_Q31);
		outZone = std::min(static_cast<int32_t>(paramNorm * kNumHarmonicZones), kNumHarmonicZones - 1);

		if (outZone == 0) {
			outT3 = outT5 = outT7 = outT9 = 0;
			return;
		}

		float chebyPos = (paramNorm - 1.0f / kNumHarmonicZones) * (kNumHarmonicZones / (kNumHarmonicZones - 1.0f));
		chebyPos = std::clamp(chebyPos, 0.0f, 1.0f);
		float freqMult = (outZone == 7) ? 2.0f : 1.0f;

		constexpr float kPhaseScale = 4294967296.0f;
		auto toPhase = [](float f) {
			f = std::fmod(f, 1.0f);
			return static_cast<uint32_t>((f < 0.0f ? f + 1.0f : f) * kPhaseScale);
		};

		// phaseOffset shifts all triangle oscillators together
		outT3 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT3 * freqMult + kPhaseT3 + phaseOffset), kPhaseWidthT3);
		outT5 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT5 * freqMult + kPhaseT5 + phaseOffset), kPhaseWidthT5);
		outT7 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT7 * freqMult + kPhaseT7 + phaseOffset), kPhaseWidthT7);
		outT9 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT9 * freqMult + kPhaseT9 + phaseOffset), kPhaseWidthT9);
	}
};

// ============================================================================
// Zone 1 "357" Chebyshev Harmonic Extraction
// ============================================================================
//
// FUNDAMENTAL CANCELLATION - CRITICAL DESIGN CONSTRAINT
// =====================================================
//
// The normalized Chebyshev polynomials Hn(x) are designed to extract pure
// harmonics from a sinusoidal input. For x = sin(θ):
//
//   H3(sin(θ)) = sin(3θ)/3   (pure 3rd harmonic)
//   H5(sin(θ)) = sin(5θ)/5   (pure 5th harmonic)
//   H7(sin(θ)) = sin(7θ)/7   (pure 7th harmonic)
//
// The polynomial form H3(x) = x - (4/3)x³ achieves this by having the linear
// term (x) exactly cancel the fundamental energy produced by the cubic term.
// This cancellation is mathematically precise when evaluated at sin(θ).
//
// LOW INPUT LEVEL PROBLEM
// =======================
//
// **At low input levels, fundamental cancellation FAILS.**
//
// The polynomial P(x) = c1*x + c3*x³ + c5*x⁵ + c7*x⁷ behaves differently
// depending on input amplitude:
//
//   At |x| ≈ 1.0:  All terms contribute, cancellation works
//   At |x| = 0.5:  c3*x³ = c3*0.125, much smaller than c1*x = c1*0.5
//   At |x| = 0.1:  c3*x³ = c3*0.001, negligible vs c1*x = c1*0.1
//
// For small x, P(x) ≈ c1*x (pure fundamental pass-through).
//
// Numerical example with pure H3 (c1=1, c3=-4/3):
//   x=1.0:  P(x) = 1 - 1.33 = -0.33  (harmonics dominate)
//   x=0.5:  P(x) = 0.5 - 0.17 = 0.33 (66% fundamental)
//   x=0.1:  P(x) = 0.1 - 0.0013 ≈ 0.1 (99% fundamental!)
//
// This is a fundamental limitation of polynomial waveshapers - they become
// LINEAR at low input levels. The Drive parameter must push the signal into
// the polynomial's nonlinear region for harmonic extraction to work.
//
// HIGH INPUT LEVEL SOLUTION (OUTPUT WAVEFOLDING)
// ===============================================
//
// When the polynomial OUTPUT exceeds [-1, 1], we use wavefolding to map it
// back. Unlike input folding, output folding preserves the Chebyshev harmonic
// extraction because the polynomial sees the actual (overdriven) sine wave.
//
// Key insight: Input folding distorts the waveform BEFORE the polynomial,
// creating harmonics similar to hard clipping. Output folding lets the
// polynomial do its harmonic extraction first, then bounds the result.
//
// Formula: phase = fmod(result + 1, 4); if (phase < 0) phase += 4; result = 1 - fabs(phase - 2)
//
// At moderate drive: polynomial output stays in [-1, 1], no folding occurs
// At high drive: polynomial blows up (T7(2) ≈ 5000), folding kicks in
//
// Benefits vs input folding:
// - Preserves Chebyshev harmonic extraction at moderate drives
// - Folding only activates when polynomial exceeds bounds
// - Musical character when overdriven (Buchla/Serge wavefolder sound)
//
// WHY NOT JUST SKIP FUNDAMENTAL CANCELLATION?
// ============================================
//
// Without the c1 linear term (using only c3*x³ + c5*x⁵ + c7*x⁷), each power
// of x produces BOTH fundamental and harmonics when applied to sin(θ):
//
//   sin³(θ) = (3sin(θ) - sin(3θ))/4    → 75% fundamental + 25% 3rd harmonic
//   sin⁵(θ) = (10sin(θ) - 5sin(3θ) + sin(5θ))/16  → 62.5% fundamental + ...
//
// The fundamental components from x³, x⁵, x⁷ are IN-PHASE with the input.
// This creates a mix knob problem:
//
//   100% wet: fundamental + harmonics (from polynomial)
//   50% mix:  0.5*dry + 0.5*(fundamental + harmonics) = 1.0× fundamental + 0.5× harmonics
//   0% dry:   fundamental only
//
// The constructive interference at mid-mix values causes non-linear output
// level swings. Mix 50% can be LOUDER than either 0% or 100%.
//
// With fundamental cancellation (c1 = w3+w5+w7), the wet signal contains
// only harmonics (at optimal drive), making the mix knob behavior predictable:
//
//   100% wet: harmonics only
//   50% mix:  0.5*dry + 0.5*harmonics (linear blend)
//   0% dry:   fundamental only
//
// DESIGN DECISION
// ===============
//
// We KEEP fundamental cancellation despite its limitations because:
//
// 1. Predictable mix behavior (no output level surprises at mid-mix values)
// 2. Clean harmonic extraction at moderate drive levels
// 3. The "sweet spot" drive requirement is acceptable for a sound design tool
//
// Accepted limitations:
//
// 1. LOW DRIVE: Polynomial becomes linear, outputs scaled fundamental.
//    At 100% mix this produces quiet fundamental instead of silence.
//    → Accepted as inherent to polynomial waveshaping physics.
//
// 2. HIGH DRIVE: Wavefolding adds its own harmonic character.
//    → Accepted. The classic Buchla/Serge wavefolder sound adds musical
//      complexity rather than harsh clipping artifacts when overdriven.
//
// 3. OPTIMAL RANGE: Effect works best when input amplitude is near ±1.0.
//    → Users should adjust Drive to find the harmonic extraction sweet spot.
//
// Future enhancement: Per-buffer envelope follower to normalize input,
// ensuring polynomial always operates in optimal region regardless of
// input level. This would eliminate the low-drive fundamental leakage
// at the cost of ~10 additional cycles per sample.
//
// ============================================================================

/**
 * Generic 4-weight blend for triangle-phased parameter morphing
 *
 * Used by both Zone 1/2 (Chebyshev harmonics) and Zone 3 (FM modes).
 * Weights are computed using log-scaled triangles with irrational frequencies
 * to create smooth, non-periodic transitions through parameter space.
 */
struct BlendWeights4 {
	float w0;                  // First mode weight (Zone 1/2: T3, Zone 3: Add)
	float w1;                  // Second mode weight (Zone 1/2: T5, Zone 3: Ring)
	float w2;                  // Third mode weight (Zone 1/2: T7, Zone 3: FM)
	float w3;                  // Fourth mode weight (Zone 1/2: T9, Zone 3: Fold)
	float inputGainMult{1.0f}; // Zone 3 input gain multiplier (unused in Zone 1/2)
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

	// Convert float phase to uint32_t for triangle functions
	constexpr float kPhaseScale = 4294967296.0f;
	auto toPhase = [](float f) {
		f = std::fmod(f, 1.0f);
		if (f < 0.0f) {
			f += 1.0f;
		}
		return static_cast<uint32_t>(f * kPhaseScale);
	};

	constexpr float kInvQ31 = 1.0f / static_cast<float>(ONE_Q31);
	constexpr float kMinWeight = 0.01f; // -40dB floor (prevents div by zero)

	// Log scaling: each encoder step produces roughly equal dB change
	// 40dB dynamic range: weight = 10^((linearTri - 1) * 2) = fastExp((linearTri - 1) * 4.605)
	constexpr float kLogScale = 4.605f;           // 2 * ln(10) for 40dB range
	constexpr uint32_t kPhaseWidth = 0xCCCCCCCCu; // 80% duty

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

	float tri0 = static_cast<float>(triangleWithDeadzone(toPhase(posInZone * kFreqW0 + 0.0f), kPhaseWidth)) * kInvQ31;
	float tri1 = static_cast<float>(triangleWithDeadzone(toPhase(posInZone * kFreqW1 + 0.25f), kPhaseWidth)) * kInvQ31;
	float w0 = linearToLog(tri0);
	float w1 = linearToLog(tri1);

	// w2 and w3 from bipolar triangle with dead zone: positive = w2, negative = w3
	// Same 70% duty, split: 35% w2, 35% w3, 30% silent
	int32_t modeTriangle = triangleWithDeadzoneBipolar(toPhase(posInZone * kFreqW2_3 + 0.5f), kPhaseWidth);
	float tri23 = static_cast<float>(std::abs(modeTriangle)) * kInvQ31;
	float w23_log = linearToLog(tri23);
	float w2 = (modeTriangle > 0) ? w23_log : kMinWeight;
	float w3 = (modeTriangle < 0) ? w23_log : kMinWeight;

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
struct Zone1Weights {
	float c1; // Coefficient for x (cancels fundamental from higher-order terms)
	float c3; // Coefficient for x³
	float c5; // Coefficient for x⁵
	float c7; // Coefficient for x⁷ (from w7 or w9's H9 contribution)
	float c9; // Coefficient for x⁹ (only when w9 active, else 0)
};

/**
 * Zone 1 coefficients with stereo offset deltas
 * Symmetric stereo: L = center - delta, R = center + delta
 *
 * Uses Jacobian to avoid R normalization. Zero-sum weight deltas preserve c1.
 */
struct Zone1CoeffsWithOffsets {
	Zone1Weights coeffs; // Center coefficients (mono reference)
	float dc3;           // Coefficient delta for c3
	float dc5;           // Coefficient delta for c5
	float dc7;           // Coefficient delta for c7
	float dc9;           // Coefficient delta for c9

	/// Valid coefficient ranges for normalized Chebyshev blend (weights sum to 1)
	static constexpr float kMinC3 = -13.33f; // pure w9: -40/3
	static constexpr float kMaxC3 = -1.33f;  // pure w3: -4/3
	static constexpr float kMinC5 = 0.0f;    // pure w3
	static constexpr float kMaxC5 = 48.0f;   // pure w9
	static constexpr float kMinC7 = -64.0f;  // pure w9
	static constexpr float kMaxC7 = 0.0f;    // no w7/w9
	static constexpr float kMinC9 = 0.0f;    // no w9
	static constexpr float kMaxC9 = 28.44f;  // pure w9: 256/9

	/// Get L-channel coefficients with attenuation-only stereo
	/// Stereo attenuates harmonics differently per channel - never boosts above center
	/// This ensures L output ≤ center output, preventing clipping surprises
	/// @param stereoWidth Spread envelope (0-1, peaks at 25-75% of Wide zone)
	/// @param freqMult Oscillation frequency multiplier (1× normal, up to 4× on down slope)
	/// @param phaseOffset Continuous phase evolution (0-1 across Wide zone)
	[[nodiscard]] Zone1Weights getL(float stereoWidth, float freqMult = 1.0f, float phaseOffset = 0.0f) const {
		// Attenuation-only stereo: scale coefficients toward zero, never away from zero
		// L attenuates odd harmonics (3, 7) when mod > 0, even (5, 9) when mod < 0
		// R does opposite, creating complementary stereo image

		// 100% duty triangle: linear ramp -1 to +1 to -1 (no dead zone)
		uint32_t phase = static_cast<uint32_t>(phaseOffset * freqMult * 4294967296.0f);
		float mod = static_cast<float>(getTriangle(phase)) / static_cast<float>(ONE_Q31);

		// Attenuation factors: 1.0 = full strength, lower = attenuated
		// max(0, mod) attenuates when mod positive, max(0, -mod) when negative
		float atten3 = 1.0f - stereoWidth * 0.5f * std::fmax(0.0f, mod);
		float atten5 = 1.0f - stereoWidth * 0.5f * std::fmax(0.0f, -mod);
		float atten7 = atten3; // 7 pairs with 3
		float atten9 = atten5; // 9 pairs with 5

		return {coeffs.c1, // c1 unchanged (fundamental)
		        coeffs.c3 * atten3, coeffs.c5 * atten5, coeffs.c7 * atten7, coeffs.c9 * atten9};
	}

	/// Get R-channel coefficients with attenuation-only stereo
	/// Complementary to L: when L attenuates 3/7, R attenuates 5/9 (and vice versa)
	/// @param stereoWidth Spread envelope (0-1, peaks at 25-75% of Wide zone)
	/// @param freqMult Oscillation frequency multiplier (1× normal, up to 4× on down slope)
	/// @param phaseOffset Continuous phase evolution (0-1 across Wide zone)
	[[nodiscard]] Zone1Weights getR(float stereoWidth, float freqMult = 1.0f, float phaseOffset = 0.0f) const {
		// R uses opposite attenuation pattern to L for stereo difference

		// 100% duty triangle: linear ramp -1 to +1 to -1 (no dead zone)
		uint32_t phase = static_cast<uint32_t>(phaseOffset * freqMult * 4294967296.0f);
		float mod = static_cast<float>(getTriangle(phase)) / static_cast<float>(ONE_Q31);

		// Opposite of L: attenuate 3/7 when mod < 0, attenuate 5/9 when mod > 0
		float atten3 = 1.0f - stereoWidth * 0.5f * std::fmax(0.0f, -mod);
		float atten5 = 1.0f - stereoWidth * 0.5f * std::fmax(0.0f, mod);
		float atten7 = atten3;
		float atten9 = atten5;

		return {coeffs.c1, // c1 unchanged (fundamental)
		        coeffs.c3 * atten3, coeffs.c5 * atten5, coeffs.c7 * atten7, coeffs.c9 * atten9};
	}
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
inline Zone1Weights weightsToCoeffs(float w3, float w5, float w7, float w9) {
	// Coefficients derived from weighted sum of normalized Chebyshev polynomials
	float c1 = w3 + w5 + w7 + w9;
	float c3 = -(w3 * (4.0f / 3.0f) + w5 * 4.0f + w7 * 8.0f + w9 * (40.0f / 3.0f));
	float c5 = w5 * 3.2f + w7 * 16.0f + w9 * 48.0f;
	float c7 = -w7 * (64.0f / 7.0f) - w9 * 64.0f;
	float c9 = w9 * (256.0f / 9.0f);
	return {c1, c3, c5, c7, c9};
}

/**
 * Compute Zone 1 coefficients with stereo offsets from harmonic parameter
 * Should be called once per block, not per sample
 *
 * Base weights: unipolar triangles + epsilon, normalized (safe division)
 * Deltas: continuous BIPOLAR triangles (no epsilon/floor needed, symmetric L/R)
 * Jacobian: converts weight deltas to coefficient deltas, avoids R normalization
 *
 * 7th vs 9th harmonic selection: determined by bipolar triangle at posInZone.
 * When the triangle is positive, use 7th; when negative, use 9th.
 * This naturally alternates as posInZone sweeps through the zone.
 *
 * @param posInZone Position 0.0 to 1.0 within the zone
 * @return L coefficients + coefficient deltas for R
 */
inline Zone1CoeffsWithOffsets computeZone1CoeffsWithOffsets(float posInZone) {
	posInZone = std::clamp(posInZone, 0.0f, 1.0f);

	// === Base weights: use shared 4-weight blend computation ===
	BlendWeights4 weights = computeBlendWeights4(posInZone);
	float w3 = weights.w0;
	float w5 = weights.w1;
	float w7 = weights.w2;
	float w9 = weights.w3;

	// Convert float phase to uint32_t for triangle functions (needed for stereo deltas)
	constexpr float kPhaseScale = 4294967296.0f;
	auto toPhase = [](float f) {
		f = std::fmod(f, 1.0f);
		if (f < 0.0f) {
			f += 1.0f;
		}
		return static_cast<uint32_t>(f * kPhaseScale);
	};

	constexpr float kInvQ31 = 1.0f / static_cast<float>(ONE_Q31);

	// Compute L coefficients
	Zone1Weights coeffsL = weightsToCoeffs(w3, w5, w7, w9);

	// === Stereo deltas: continuous BIPOLAR triangles (no dead zone, no epsilon) ===
	// These don't need epsilon because we never divide by them
	// Bipolar means sometimes L gets more, sometimes R gets more (symmetric)
	// Each harmonic group has independent triangle for varied stereo image
	constexpr float kDeltaFreq3 = 3.14159f;    // π
	constexpr float kDeltaFreq5 = 2.71828f;    // e
	constexpr float kDeltaFreqHigher = 1.618f; // φ (golden ratio) - independent from 3 and 5
	constexpr float kDeltaScale = 0.35f;       // Scale factor for stereo spread

	// getTriangle returns bipolar q31 (-2^31 to 2^31-1), continuous (no dead zone)
	float dw3_raw = static_cast<float>(getTriangle(toPhase(posInZone * kDeltaFreq3 + 0.1f))) * kInvQ31 * kDeltaScale;
	float dw5_raw = static_cast<float>(getTriangle(toPhase(posInZone * kDeltaFreq5 + 0.6f))) * kInvQ31 * kDeltaScale;
	float dwHigher_raw =
	    static_cast<float>(getTriangle(toPhase(posInZone * kDeltaFreqHigher + 0.3f))) * kInvQ31 * kDeltaScale;

	// Normalize to zero sum (ensures dc1 = 0, preserving c1)
	float dwSum = dw3_raw + dw5_raw + dwHigher_raw;
	float dwOffset = dwSum / 3.0f;
	float dw3 = dw3_raw - dwOffset;
	float dw5 = dw5_raw - dwOffset;
	float dwHigher = dwHigher_raw - dwOffset;

	// 7 and 9 share stereo offset (only one active at a time based on triangle sign)
	float dw7 = dwHigher;
	float dw9 = dwHigher;

	// Apply Jacobian to get coefficient deltas (using full formulas including w9 terms):
	// dc1 = dw3 + dw5 + dw7 + dw9 = 0 (by construction)
	// dc3 = -(4/3)*dw3 - 4*dw5 - 8*dw7 - (40/3)*dw9
	// dc5 = 3.2*dw5 + 16*dw7 + 48*dw9
	// dc7 = -(64/7)*dw7 - 64*dw9
	// dc9 = (256/9)*dw9
	float dc3 = -(4.0f / 3.0f) * dw3 - 4.0f * dw5 - 8.0f * dw7 - (40.0f / 3.0f) * dw9;
	float dc5 = 3.2f * dw5 + 16.0f * dw7 + 48.0f * dw9;
	float dc7 = -(64.0f / 7.0f) * dw7 - 64.0f * dw9;
	float dc9 = (256.0f / 9.0f) * dw9;

	// Clamp coefficient deltas to prevent polynomial output exceeding bounds
	// Limits are ~25% of each coefficient's valid range to allow stereo spread
	// without pushing R channel coefficients outside valid polynomial bounds
	constexpr float kMaxDc3 = 3.0f;  // c3 range: [-13.3, -1.3]
	constexpr float kMaxDc5 = 12.0f; // c5 range: [0, 48]
	constexpr float kMaxDc7 = 16.0f; // c7 range: [-64, 0]
	constexpr float kMaxDc9 = 7.0f;  // c9 range: [0, 28.4]
	dc3 = std::clamp(dc3, -kMaxDc3, kMaxDc3);
	dc5 = std::clamp(dc5, -kMaxDc5, kMaxDc5);
	dc7 = std::clamp(dc7, -kMaxDc7, kMaxDc7);
	dc9 = std::clamp(dc9, -kMaxDc9, kMaxDc9);

	return {coeffsL, dc3, dc5, dc7, dc9};
}

/**
 * Compute Zone 1 mono weights from harmonic parameter (legacy/mono path)
 * @param posInZone Position 0.0 to 1.0 within the zone
 * @return Precomputed blended coefficients for Horner's method evaluation
 */
inline Zone1Weights computeZone1WeightsFromPos(float posInZone) {
	// For mono, use center coefficients (no stereo offset)
	return computeZone1CoeffsWithOffsets(posInZone).coeffs;
}

/**
 * Helper to compute position within a zone from harmonic param value
 */
inline float computePosInZone(q31_t harmonic, int32_t zoneIndex) {
	float zoneStart = static_cast<float>(zoneIndex) / 8.0f;
	float zoneEnd = static_cast<float>(zoneIndex + 1) / 8.0f;
	float paramNorm = static_cast<float>(harmonic) / static_cast<float>(ONE_Q31);
	return std::clamp((paramNorm - zoneStart) / (zoneEnd - zoneStart), 0.0f, 1.0f);
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
[[gnu::always_inline]] inline float evaluate3579Polynomial(float x, const Zone1Weights& weights) {
	float x2 = x * x;
	// Horner's method: x * (c1 + x² * (c3 + x² * (c5 + x² * (c7 + c9*x²))))
	return x * (weights.c1 + x2 * (weights.c3 + x2 * (weights.c5 + x2 * (weights.c7 + weights.c9 * x2))));
}

/**
 * Compute Chebyshev polynomials using recurrence relation
 * Tn(x) = 2x·Tn-1(x) - Tn-2(x)
 *
 * @param x Input sample (q31, range -ONE_Q31 to ONE_Q31)
 * @param maxOrder Maximum order to compute (3, 5, 7, or 9)
 * @param T3 Output T3(x)
 * @param T5 Output T5(x)
 * @param T7 Output T7(x)
 * @param T9 Output T9(x)
 */
inline void chebyshevRecurrence(q31_t x, int32_t maxOrder, q31_t& T3, q31_t& T5, q31_t& T7, q31_t& T9) {
	// Chebyshev recurrence: Tn = 2x·Tn-1 - Tn-2
	// To compute 2x·Tn-1 in q31:
	//   multiply_32x32_rshift32_rounded(x, Tn-1) gives x·Tn-1
	//   Then << 1 gives 2x·Tn-1
	// We use a helper macro for clarity.
#define CHEBY_STEP(Tprev1, Tprev2) ((multiply_32x32_rshift32_rounded(x, Tprev1) << 2) - Tprev2)

	// T0 = 1, T1 = x
	q31_t Tprev2 = ONE_Q31; // T(n-2)
	q31_t Tprev1 = x;       // T(n-1)
	q31_t Tcurr;

	// T2 = 2x·T1 - T0 = 2x² - 1
	Tcurr = CHEBY_STEP(Tprev1, Tprev2);
	Tprev2 = Tprev1;
	Tprev1 = Tcurr;

	// T3 = 2x·T2 - T1
	Tcurr = CHEBY_STEP(Tprev1, Tprev2);
	T3 = Tcurr;
	if (maxOrder <= 3) {
		return;
	}
	Tprev2 = Tprev1;
	Tprev1 = Tcurr;

	// T4 = 2x·T3 - T2
	Tcurr = CHEBY_STEP(Tprev1, Tprev2);
	Tprev2 = Tprev1;
	Tprev1 = Tcurr;

	// T5 = 2x·T4 - T3
	Tcurr = CHEBY_STEP(Tprev1, Tprev2);
	T5 = Tcurr;
	if (maxOrder <= 5) {
		return;
	}
	Tprev2 = Tprev1;
	Tprev1 = Tcurr;

	// T6 = 2x·T5 - T4
	Tcurr = CHEBY_STEP(Tprev1, Tprev2);
	Tprev2 = Tprev1;
	Tprev1 = Tcurr;

	// T7 = 2x·T6 - T5
	Tcurr = CHEBY_STEP(Tprev1, Tprev2);
	T7 = Tcurr;
	if (maxOrder <= 7) {
		return;
	}
	Tprev2 = Tprev1;
	Tprev1 = Tcurr;

	// T8 = 2x·T7 - T6
	Tcurr = CHEBY_STEP(Tprev1, Tprev2);
	Tprev2 = Tprev1;
	Tprev1 = Tcurr;

	// T9 = 2x·T8 - T7
	Tcurr = CHEBY_STEP(Tprev1, Tprev2);
	T9 = Tcurr;

#undef CHEBY_STEP
}

/**
 * Derived values from Twist parameter for sine shaper
 * Computed once per buffer at call site
 *
 * Zone 0: Twist - Phase modulator for other zones (meta-control)
 * Zone 1: Width - Stereo spread with animated phase evolution
 * Zone 2: Evens - Self-mul for even harmonics (3→6, 5→10, 7→14)
 * Zone 3: Asym - DC offset for even harmonics
 * Zone 4: Rect - Blended rectifier (rect + rect2 with overlap)
 * Zone 5: Feedback - Output→input recirculation
 */
struct SineShaperTwistParams {
	float stereoWidth{0.0f};       // Width: stereo spread envelope
	float stereoFreqMult{1.0f};    // Width: oscillation frequency multiplier
	float stereoPhaseOffset{0.0f}; // Width: continuous phase evolution
	float evenAmount{0.0f};        // Evens: self-mul amount (0.0 to 4.0)
	q31_t symmetry{0};             // Asym: DC offset for asymmetry
	float rectAmount{0.0f};        // Rect: rectifier blend
	float rect2Amount{0.0f};       // Rect: peak-cutting rectifier
	float feedbackAmount{0.0f};    // Feedback: depth (0.0 to 0.25)
	float phaseHarmonic{0.0f};     // Twist zone: offset for Harmonic knob
};

/**
 * Derive all Twist-dependent parameters from smoothed Twist value
 * Zones: 0=Twist (meta), 1=Width, 2=Evens, 3=Asym, 4=Rect, 5=Feedback
 */
inline SineShaperTwistParams computeSineShaperTwistParams(q31_t smoothedTwist) {
	constexpr q31_t kZone1 = ONE_Q31 / 8;       // 1/8
	constexpr q31_t kZone2 = ONE_Q31 / 4;       // 2/8
	constexpr q31_t kZone3 = (ONE_Q31 / 8) * 3; // 3/8
	constexpr q31_t kZone4 = ONE_Q31 / 2;       // 4/8
	constexpr q31_t kZone5 = (ONE_Q31 / 8) * 5; // 5/8
	constexpr q31_t kZone6 = (ONE_Q31 / 4) * 3; // 6/8
	constexpr uint32_t kFullDuty = 0xFFFFFFFFu; // 100% duty triangle
	constexpr float kInvQ31 = 1.0f / static_cast<float>(ONE_Q31);

	// Triangle helper: normalized 0→1 from triangleWithDeadzone
	auto tri = [&](float phase) {
		uint32_t p = static_cast<uint32_t>(std::fmod(phase, 1.0f) * 4294967296.0f);
		return static_cast<float>(triangleWithDeadzone(p, kFullDuty)) * kInvQ31;
	};

	SineShaperTwistParams params;

	if (smoothedTwist < kZone1) {
		// Zone 0: Twist - phase modulator for Harmonic knob only
		float pos = static_cast<float>(smoothedTwist) / static_cast<float>(kZone1);
		params.phaseHarmonic = tri(pos * 2.0f) * 0.5f; // 2 cycles, ±0.5 offset
	}
	else if (smoothedTwist < kZone2) {
		// Zone 1: Width - stereo spread with animated phase evolution
		float pos = static_cast<float>(smoothedTwist - kZone1) / static_cast<float>(kZone2 - kZone1);
		params.stereoWidth = (pos < 0.25f) ? pos * 4.0f : (pos < 0.75f) ? 1.0f : (1.0f - pos) * 4.0f;
		params.stereoFreqMult = (pos <= 0.75f) ? 1.0f : 1.0f + (pos - 0.75f) * 12.0f;
		params.stereoPhaseOffset = pos;
	}
	else if (smoothedTwist < kZone3) {
		// Zone 2: Evens - self-mul for even harmonics (quadratic onset)
		float pos = static_cast<float>(smoothedTwist - kZone2) / static_cast<float>(kZone3 - kZone2);
		params.evenAmount = pos * pos * 4.0f;
	}
	else if (smoothedTwist < kZone4) {
		// Zone 3: Asym - DC offset for even harmonics
		float pos = static_cast<float>(smoothedTwist - kZone3) / static_cast<float>(kZone4 - kZone3);
		params.symmetry = static_cast<q31_t>(pos * ONE_Q31);
	}
	else if (smoothedTwist < kZone5) {
		// Zone 4: Rect - phased triangle blend of rect + rect2
		float pos = static_cast<float>(smoothedTwist - kZone4) / static_cast<float>(kZone5 - kZone4);
		float rectW = (pos < 0.4f) ? pos / 0.4f : (pos < 0.8f) ? 1.0f - (pos - 0.4f) / 0.4f : 0.0f;
		float rect2W = (pos < 0.25f) ? 0.0f : (pos < 0.75f) ? (pos - 0.25f) / 0.5f : 1.0f;
		params.rectAmount = rectW * std::min(pos / 0.1f, 1.0f);
		params.rect2Amount = rect2W;
	}
	else if (smoothedTwist < kZone6) {
		// Zone 5: Feedback (capped at 25%) - TODO: scale inversely with mode0 harmonic
		float pos = static_cast<float>(smoothedTwist - kZone5) / static_cast<float>(kZone6 - kZone5);
		params.feedbackAmount = pos * 0.25f;
	}

	return params;
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
 * - Zone 0 (Poly): Cascaded polynomial waveshaping
 * - Zones 1-7 (Chebyshev): Triangle-modulated blend of T3, T5, T7, T9
 *
 * Post-gain compensation ensures peak output matches wavefolder.
 *
 * @param zone1Weights Optional precomputed Zone 1 weights (hoisted from buffer loop)
 *                     If nullptr and in Zone 1, weights are computed per sample
 * @param zone3Weights Optional precomputed Zone 3 weights (BlendWeights4 with inputGainMult)
 *                     If nullptr and in Zone 3, weights are computed per sample (mono)
 * @param evenAmount Self-mul for even harmonics (Twist Zone 2, 0.0 to 4.0)
 * @param rectAmount Rectifier blend toward |result| (Twist Zone 3, 0.0 to 1.0)
 * @param rect2Amount Peak-cutting rectifier (Twist Zone 4, 0.0 to 1.0)
 */
inline q31_t sineShapeCore(q31_t input, q31_t drive, q31_t harmonic, q31_t symmetry,
                           const Zone1Weights* zone1Weights = nullptr, const BlendWeights4* zone3Weights = nullptr,
                           float evenAmount = 0.0f, float rectAmount = 0.0f, float rect2Amount = 0.0f,
                           float phaseHarmonic = 0.0f) {
	// Apply symmetry (DC offset for even harmonics)
	// >> 12 accounts for drive (up to 4x) and input scaling (<< 8 Zone 0, << 9 Zone 1/2)
	// At max: 0.024% DC → after 4x drive and 512x scaling = ~50% DC offset
	q31_t biased = add_saturate(input, symmetry >> 12);

	// === Drive calculation for hybrid param (bipolar, additive modulation) ===
	// drive range: INT32_MIN to INT32_MAX (center=0 is unity)
	// Normalize to 0..1, then square to get volume-style curve
	// This gives: min→0, center→1, max→4 (same as old volume param behavior)
	float normalizedDrive = (static_cast<float>(drive) + 2147483648.0f) / 4294967296.0f; // 0 to 1
	float driveGain = normalizedDrive * normalizedDrive * 4.0f;                          // Square for volume curve

	// Apply drive to input
	float inputF = static_cast<float>(biased) * driveGain;

	// Clamp and convert back to q31 for zones that need bounded input
	// Keep inputF unclamped for zone 3 which needs phase wrapping
	constexpr float kMaxQ31 = 2147483647.0f;
	float inputFClamped = std::clamp(inputF, -kMaxQ31, kMaxQ31);
	q31_t driven = static_cast<q31_t>(inputFClamped);

	// === Derive harmonic zone and weights ===
	int32_t zone;
	q31_t wT3, wT5, wT7, wT9;
	SineShaperHarmonicMapper::deriveWeights(harmonic, zone, wT3, wT5, wT7, wT9, phaseHarmonic);

	q31_t shaped;

	if (zone == 0) {
		// === Zone 0: Polynomial waveshaping with cascade blend + self-multiplication ===
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

		// Twist (phaseHarmonic) blends in self-multiplication pre-waveshaping
		// Self-mul = input * |input| adds even harmonics before polynomial
		// phaseHarmonic ±0.5 → blend 0-1
		if (phaseHarmonic > -0.49f) {
			float selfMulBlend = phaseHarmonic + 0.5f; // 0 to 1
			q31_t absInput = (scaledInput < 0) ? -scaledInput : scaledInput;
			q31_t selfMulInput = multiply_32x32_rshift32_rounded(scaledInput, absInput) << 1;
			q31_t blendQ = static_cast<q31_t>(selfMulBlend * static_cast<float>(ONE_Q31));
			scaledInput = scaledInput + multiply_32x32_rshift32_rounded(selfMulInput - scaledInput, blendQ) * 2;
		}

		shaped = polynomialOscillatorApproximation(scaledInput) >> 8;

		// Cascaded polynomial adds more harmonics via second pass
		// Scale so blend reaches maximum at end of zone 0 (1/8 of range)
		q31_t cascadeBlend = (harmonic >= ONE_Q31 / 8) ? ONE_Q31 : (harmonic << 3);

		if (cascadeBlend > 0) {
			q31_t moreHarmonics = polynomialOscillatorApproximation(shaped << 7) >> 7;
			shaped = shaped + multiply_32x32_rshift32_rounded(moreHarmonics - shaped, cascadeBlend) * 2;
		}
	}
	else if (zone == 1 || zone == 2) {
		// === Zone 1 "357" / Zone 2 "357wm": Blended Chebyshev T3, T5, T7 ===
		// Zone 1: Raw input, unbounded output (edgy, platform-dependent overflow)
		// Zone 2: Sine-preprocessed input, bounded output (warm, FM-like)

		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 9);
		float rawX = static_cast<float>(scaledInput) / static_cast<float>(ONE_Q31);

		// Zone 2 applies sine waveshaping via lookup table to bound input to [-1, 1]
		// Factor increased by 50%: sin(x * 3π/4) instead of sin(x * π/2) for more coloration
		float x;
		if (zone == 2) {
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
			x = rawX;
		}

		// Use precomputed blended coefficients if available (hoisted from buffer loop)
		Zone1Weights fallbackWeights;
		const Zone1Weights* weights = zone1Weights;
		if (!weights) {
			float posInZone = computePosInZone(harmonic, zone);
			fallbackWeights = computeZone1WeightsFromPos(posInZone);
			weights = &fallbackWeights;
		}

		// Shared polynomial evaluation - changes here apply to both zones
		float result = evaluate3579Polynomial(x, *weights);

		// Scale output back to q31 range (no post-gain)
		shaped = static_cast<q31_t>(result * static_cast<float>(ONE_Q31)) >> 6;
	}
	else if (zone == 3) {
		// === Zone 3 "FM": 4-mode FM synthesis blend ===
		// Uses same triangle-phased weights as Zone 1/2 for smooth morphing
		// Mode 0 (Add):  sin(x) + sin(ratio·x)     - layered octaves
		// Mode 1 (Ring): sin(x) × sin(ratio·x)     - metallic, bell-like
		// Mode 2 (FM):   sin(x + d·sin(ratio·x))   - classic DX7-style
		// Mode 3 (Fold): sin(k·sin(x))             - wavefolder

		// Get weights: precomputed (per-buffer) or fallback to per-sample
		float w0, w1, w2, w3;
		float inputGainMult = 1.0f;
		if (zone3Weights != nullptr) {
			// Use precomputed weights (stereo comes from separate L/R BlendWeights4)
			w0 = zone3Weights->w0;
			w1 = zone3Weights->w1;
			w2 = zone3Weights->w2;
			w3 = zone3Weights->w3;
			inputGainMult = zone3Weights->inputGainMult;
		}
		else {
			// Fallback: compute weights per sample with inline stereo
			float posInZone = computePosInZone(harmonic, zone);
			BlendWeights4 weights = computeBlendWeights4(posInZone);
			w0 = weights.w0;
			w1 = weights.w1;
			w2 = weights.w2;
			w3 = weights.w3;
			// Mono fallback: no stereo (stereo comes from precomputed L/R weights)
			inputGainMult = 1.0f + posInZone * 3.0f;
		}

		constexpr float kInvQ31 = 1.0f / static_cast<float>(ONE_Q31);

		// Apply additional input gain for Zone 3 (ramps up through zone)
		float inputFGained = inputF * inputGainMult;

		// Primary sine: drive-controlled phase depth
		float phase1F = inputFGained * 256.0f;
		uint32_t phase1 = static_cast<uint32_t>(static_cast<int64_t>(phase1F));
		int32_t sine1Raw = getSine(phase1);
		float sine1 = static_cast<float>(sine1Raw) * kInvQ31; // [-1, 1]

		// Secondary sine: 2x frequency (octave up) for harmonically-related content
		float phase2F = inputFGained * 2.0f * 256.0f;
		uint32_t phase2 = static_cast<uint32_t>(static_cast<int64_t>(phase2F));
		int32_t sine2Raw = getSine(phase2);
		float sine2 = static_cast<float>(sine2Raw) * kInvQ31; // [-1, 1]

		// Compute all 4 modes - all scaled to peak at ~0.75 for headroom
		// No clamping needed with proper gain staging

		// Mode 0: Add - sum of fundamental and octave
		// (1+1) * 0.375 = 0.75 max
		float modeAdd = (sine1 + sine2) * 0.375f;

		// Mode 1: Ring - product creates sum/difference frequencies
		// 90° phase offset on sine2 aligns Ring peaks with Add to prevent cancellation
		uint32_t phase2Ring = phase2 + 0x40000000u; // +90° (quarter cycle)
		int32_t sine2RingRaw = getSine(phase2Ring);
		float sine2Ring = static_cast<float>(sine2RingRaw) * kInvQ31;
		float modeRing = sine1 * sine2Ring * 0.75f;

		// Mode 2: FM - phase modulation with sine2 as modulator
		float fmDepth = 0.5f;
		uint32_t fmPhase = static_cast<uint32_t>(static_cast<int64_t>(phase1F + sine2 * fmDepth * 256.0f));
		int32_t fmSineRaw = getSine(fmPhase);
		float modeFM = static_cast<float>(fmSineRaw) * kInvQ31 * 0.75f;

		// Mode 3: Fold - cascaded sine creates dense harmonics
		float foldGain = 2.0f;
		uint32_t foldPhase = static_cast<uint32_t>(static_cast<int64_t>(sine1 * foldGain * 256.0f));
		int32_t foldSineRaw = getSine(foldPhase);
		float modeFold = static_cast<float>(foldSineRaw) * kInvQ31 * 0.75f;

		// Blend by triangle-phased weights (normalized to prevent clipping)
		float weightSum = w0 + w1 + w2 + w3;
		float invWeightSum = (weightSum > 0.0f) ? (1.0f / weightSum) : 1.0f;
		float result = (w0 * modeAdd + w1 * modeRing + w2 * modeFM + w3 * modeFold) * invWeightSum;

		// Scale output back to q31 range (matched to reference level)
		shaped = static_cast<q31_t>(result * static_cast<float>(ONE_Q31)) >> 7;
	}
	else {
		// === Zones 4-7: Placeholder (reserved) ===
		// Simple passthrough with gain matching (no post-gain)
		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 9);
		shaped = scaledInput >> 6;
	}

	// === Even harmonics (Twist Zone 2) ===
	// Self-multiplication adds even harmonics, applied uniformly to all zones
	if (evenAmount > 0.0f) {
		float shapedF = static_cast<float>(shaped);
		float evenContrib = shapedF * std::abs(shapedF) * evenAmount * 64.0f / static_cast<float>(ONE_Q31);
		shaped = static_cast<q31_t>(shapedF + evenContrib);
	}

	// === Rectifier (Twist Zone 3) ===
	// Blend toward |shaped| for octave-up effect, applied uniformly to all zones
	if (rectAmount > 0.0f) {
		float shapedF = static_cast<float>(shaped);
		float absVal = std::abs(shapedF);
		float wet = shapedF + (absVal - shapedF) * rectAmount;
		shaped = static_cast<q31_t>(wet);
	}

	// === Rect2 (Twist Zone 4) ===
	// Peak-cutting rectifier: subtracts peak² to cut into peaks while adding octave harmonics
	// Larger peaks get cut more, preserving headroom while adding even harmonic content
	if (rect2Amount > 0.0f) {
		float shapedF = static_cast<float>(shaped);
		float absVal = std::abs(shapedF);
		float cutAmount = absVal * absVal * rect2Amount * 512.0f / static_cast<float>(ONE_Q31);
		shaped = static_cast<q31_t>(shapedF - std::copysign(cutAmount, shapedF));
	}

	// === Asymmetry gain compensation ===
	// DC offset causes higher peaks on one side.
	// Only apply when symmetry is non-zero to preserve original behavior at center.
	if (symmetry != 0) {
		q31_t absSymmetry = symmetry >= 0 ? symmetry : -symmetry;
		// Scale: 1.0 at center, ~0.6 at max asymmetry
		q31_t compFactor = ONE_Q31 - multiply_32x32_rshift32_rounded(absSymmetry, ONE_Q31 >> 1);
		shaped = multiply_32x32_rshift32_rounded(shaped, compFactor) << 1;
	}

	return shaped;
}

/**
 * Full sine shaping with wet/dry mix (legacy single-sample version)
 */
inline q31_t sineShape(q31_t input, q31_t drive, q31_t harmonic, q31_t symmetry, q31_t mix) {
	if (mix <= 0) {
		return input;
	}

	q31_t shaped = sineShapeCore(input, drive, harmonic, symmetry);

	// Chebyshev zones (1+) use harmonic extraction crossfade
	constexpr q31_t kZone1Threshold = ONE_Q31 / 8;
	if (harmonic >= kZone1Threshold) {
		// dryCoeff = 1 - mix: traditional wet/dry crossfade for Chebyshev zones
		// At mix=0: pure dry, mix=1: pure wet
		q31_t dryCoeff = ONE_Q31 - multiply_32x32_rshift32(mix, ONE_Q31) * 2;
		q31_t dryPart = multiply_32x32_rshift32(input, dryCoeff) << 1;
		q31_t wetPart = multiply_32x32_rshift32(shaped, mix) << 1;
		return add_saturate(dryPart, wetPart);
	}
	else {
		// Zone 0: Traditional wet/dry crossfade
		q31_t invMix = ONE_Q31 - mix;
		q31_t dryPart = multiply_32x32_rshift32(input, invMix) << 1;
		q31_t wetPart = multiply_32x32_rshift32(shaped, mix) << 1;
		return add_saturate(dryPart, wetPart);
	}
}

/**
 * Process a mono buffer through the sine shaper with parameter smoothing
 *
 * Drive and Harmonic are smoothed internally.
 * Twist should be smoothed at the call site for consistent handling.
 *
 * @param buffer Audio buffer to process in place
 * @param drive Current target drive value
 * @param smoothedDrive Pointer to smoothed drive state (updated in place)
 * @param voiceState Pointer to per-voice state (DC blocker, feedback, etc.)
 * @param harmonic Raw harmonic value (smoothed internally via params->smoothedHarmonic)
 * @param symmetry DC bias for asymmetry (Twist Zone 3)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param evenAmount Self-mul for even harmonics (Twist Zone 2, 0.0 to 4.0)
 * @param rectAmount Rectifier blend (Twist Zone 4, 0.0 to 1.0)
 * @param feedbackAmount Feedback depth (Twist Zone 5, 0.0 to 0.25)
 * @param params Pointer to SineShaperParams for coefficient smoothing (required)
 * @param phaseHarmonic Twist zone 0 offset for Harmonic knob (default 0)
 * @param wasBypassed Pointer to bypass state flag (updated in place)
 */
inline void sineShapeBuffer(std::span<q31_t> buffer, q31_t drive, q31_t* smoothedDrive,
                            SineShaperVoiceState* voiceState, q31_t harmonic, q31_t symmetry, q31_t mix,
                            float evenAmount, float rectAmount, float rect2Amount, float feedbackAmount,
                            SineShaperParams* params, float phaseHarmonic = 0.0f, bool* wasBypassed = nullptr) {
	// Early out - if mix is 0, do nothing (important CPU optimization)
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Per-sample drive interpolation for zipper-free parameter changes
	auto driveCtx = prepareSmoothing(*smoothedDrive, drive, buffer.size());
	*smoothedDrive = driveCtx.target; // Write back smoothed state

	// Smooth harmonic internally (fixes Zone 0 clicking from cascadeBlend/selfMulBlend jumps)
	// Note: 7th vs 9th harmonic selection is now computed internally by computeZone1*
	// based on a bipolar triangle at the posInZone - no need for external flag
	q31_t smoothedHarmonic = smoothParam(&params->smoothedHarmonic, harmonic);

	// Mark as no longer bypassed if transitioning
	if (wasBypassed && *wasBypassed) {
		*wasBypassed = false;
	}

	// Determine crossfade mode based on smoothed harmonic
	constexpr q31_t kZone1Threshold = ONE_Q31 / 8; // Zone 1 starts at 1/8
	constexpr q31_t kZone2Threshold = ONE_Q31 / 4; // Zone 2 starts at 2/8
	bool inZone0 = (smoothedHarmonic < kZone1Threshold);
	bool useChebyshevCrossfade = !inZone0;
	bool inZone1 = (smoothedHarmonic >= kZone1Threshold && smoothedHarmonic < kZone2Threshold);
	bool inZone2 = (smoothedHarmonic >= kZone2Threshold);

	// Hoist zone weight calculation with per-sample coefficient interpolation
	// Per-sample smoothing eliminates zipper noise from triangle weight changes
	// Zone 0 reuses c1Ctx for phaseHarmonic smoothing (zones are orthogonal)
	Zone1Weights zone1Weights;
	FloatSmoothingContext c1Ctx{0, 0, 0}, c3Ctx{0, 0, 0}, c5Ctx{0, 0, 0}, c7Ctx{0, 0, 0}, c9Ctx{0, 0, 0};
	bool usePerSampleWeights = (inZone0 || inZone1 || inZone2);
	float currentPhaseHarmonic = phaseHarmonic;

	if (inZone0) {
		// Zone 0: smooth phaseHarmonic using c1Ctx (reused since Zone 0 doesn't use Chebyshev coeffs)
		c1Ctx = prepareSmoothingFloat(params->smoothedC1, phaseHarmonic, buffer.size());
		currentPhaseHarmonic = c1Ctx.current;
	}
	else if (inZone1 || inZone2) {
		int32_t zoneIndex = inZone1 ? 1 : 2;
		float posInZone = computePosInZone(smoothedHarmonic, zoneIndex);
		// Apply phaseHarmonic offset (from Twist zone) - wraps around [0,1]
		posInZone = std::fmod(posInZone + phaseHarmonic + 1.0f, 1.0f);
		Zone1Weights targetWeights = computeZone1WeightsFromPos(posInZone);

		// Prepare per-sample interpolation contexts
		c1Ctx = prepareSmoothingFloat(params->smoothedC1, targetWeights.c1, buffer.size());
		c3Ctx = prepareSmoothingFloat(params->smoothedC3L, targetWeights.c3, buffer.size());
		c5Ctx = prepareSmoothingFloat(params->smoothedC5L, targetWeights.c5, buffer.size());
		c7Ctx = prepareSmoothingFloat(params->smoothedC7L, targetWeights.c7, buffer.size());
		c9Ctx = prepareSmoothingFloat(params->smoothedC9L, targetWeights.c9, buffer.size());

		// Initialize weights for first sample
		zone1Weights.c1 = c1Ctx.current;
		zone1Weights.c3 = c3Ctx.current;
		zone1Weights.c5 = c5Ctx.current;
		zone1Weights.c7 = c7Ctx.current;
		zone1Weights.c9 = c9Ctx.current;
	}

	// Local copy of state for efficient per-sample update
	// If voiceState is null, use local zeros (no persistent state across buffers)
	q31_t dcState = voiceState ? voiceState->dcBlockerL : 0;
	q31_t fbState = voiceState ? voiceState->feedbackL : 0;
	q31_t fbLpfState = voiceState ? voiceState->feedbackLpfL : 0;
	q31_t currentDrive = driveCtx.current;

	// Scale feedback amount for moderate self-oscillation range
	// At max (1.0), feedback is ~0.9 of output (just below self-oscillation)
	// In Zone 0, feedback is inversely proportional to harmonic position (more cascade = less feedback)
	// Also reduce feedback by up to 20% as drive increases (tames high-drive feedback)
	constexpr float kFeedbackScale = 0.9f;
	float fbScale = kFeedbackScale;
	constexpr q31_t kZone0End = ONE_Q31 / 8;
	if (smoothedHarmonic < kZone0End) {
		float zone0Pos = static_cast<float>(smoothedHarmonic) / static_cast<float>(kZone0End);
		fbScale *= (1.0f - zone0Pos); // 100% at start, 0% at end of Zone 0
	}
	// Drive-based reduction: -10% at max drive
	float driveNorm = static_cast<float>(drive) / static_cast<float>(ONE_Q31);
	fbScale *= (1.0f - 0.1f * driveNorm);
	q31_t fbAmount = static_cast<q31_t>(feedbackAmount * fbScale * static_cast<float>(ONE_Q31));

	for (auto& sample : buffer) {
		// Apply feedback to input (before shaping)
		q31_t inputWithFb = sample;
		if (fbAmount > 0) {
			// Hard clip feedback to prevent runaway (~-6dB from full scale)
			q31_t clippedFb = signed_saturate<22>(multiply_32x32_rshift32(fbState, fbAmount) << 1);
			inputWithFb = add_saturate(sample, clippedFb);
		}

		// Get shaped (wet) signal with all modifiers (drive + weights interpolated per-sample)
		q31_t shaped = sineShapeCore(inputWithFb, currentDrive, smoothedHarmonic, symmetry,
		                             (inZone1 || inZone2) ? &zone1Weights : nullptr, nullptr, evenAmount, rectAmount,
		                             rect2Amount, currentPhaseHarmonic);
		currentDrive += driveCtx.increment;

		// Update weights for next sample (per-sample interpolation)
		if (inZone0) {
			// Zone 0: smooth phaseHarmonic
			currentPhaseHarmonic = (c1Ctx.current += c1Ctx.increment);
		}
		else if (inZone1 || inZone2) {
			zone1Weights.c1 = (c1Ctx.current += c1Ctx.increment);
			zone1Weights.c3 = (c3Ctx.current += c3Ctx.increment);
			zone1Weights.c5 = (c5Ctx.current += c5Ctx.increment);
			zone1Weights.c7 = (c7Ctx.current += c7Ctx.increment);
			zone1Weights.c9 = (c9Ctx.current += c9Ctx.increment);
		}

		// LPF the feedback tap to tame harsh high harmonics
		fbLpfState += multiply_32x32_rshift32(shaped - fbLpfState, kFeedbackLpfAlpha) << 1;
		fbState = fbLpfState;

		q31_t mixed;
		if (useChebyshevCrossfade) {
			// Chebyshev zones: traditional wet/dry crossfade
			// At mix=0: pure dry, mix=1: pure wet
			q31_t dryCoeff = ONE_Q31 - multiply_32x32_rshift32(mix, ONE_Q31) * 2; // 1 - mix
			q31_t dryPart = multiply_32x32_rshift32(sample, dryCoeff) << 1;
			q31_t wetPart = multiply_32x32_rshift32(shaped, mix) << 1;
			mixed = add_saturate(dryPart, wetPart);
		}
		else {
			// Zone 0: Traditional wet/dry crossfade
			q31_t invMix = ONE_Q31 - mix;
			q31_t dryPart = multiply_32x32_rshift32(sample, invMix) << 1;
			q31_t wetPart = multiply_32x32_rshift32(shaped, mix) << 1;
			mixed = add_saturate(dryPart, wetPart);
		}

		// DC blocker (5Hz highpass) - removes DC offset from asymmetry
		dcState += multiply_32x32_rshift32(mixed - dcState, kDcBlockerAlpha) * 2;
		sample = mixed - dcState;
	}

	// Write back state if voiceState is not null
	if (voiceState) {
		voiceState->dcBlockerL = dcState;
		voiceState->feedbackL = fbState;
		voiceState->feedbackLpfL = fbLpfState;
	}
	if (inZone0) {
		// Zone 0: write back smoothed phaseHarmonic (stored in smoothedC1)
		params->smoothedC1 = c1Ctx.target;
	}
	else if (inZone1 || inZone2) {
		// Zone 1/2: write back smoothed Chebyshev coefficients
		params->smoothedC1 = c1Ctx.target;
		params->smoothedC3L = c3Ctx.target;
		params->smoothedC5L = c5Ctx.target;
		params->smoothedC7L = c7Ctx.target;
		params->smoothedC9L = c9Ctx.target;
	}
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
 * @param symmetry DC bias for asymmetry (Twist Zone 3)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param stereoWidth Stereo coefficient spread envelope (peaks at 25-75% of Width zone)
 * @param stereoFreqMult Stereo oscillation frequency multiplier (1× to 4×)
 * @param stereoPhaseOffset Stereo phase evolution (0-1 across Width zone)
 * @param evenAmount Self-mul for even harmonics (Twist Zone 2, 0.0 to 4.0)
 * @param rectAmount Rectifier blend (Twist Zone 4, 0.0 to 1.0)
 * @param feedbackAmount Feedback depth (Twist Zone 5, 0.0 to 0.25)
 * @param params Pointer to SineShaperParams for coefficient smoothing (required)
 * @param phaseHarmonic Twist zone 0 offset for Harmonic knob (default 0)
 * @param wasBypassed Pointer to bypass state flag (updated in place)
 */
inline void sineShapeBuffer(StereoBuffer<q31_t> buffer, q31_t drive, q31_t* smoothedDrive,
                            SineShaperVoiceState* voiceState, q31_t harmonic, q31_t symmetry, q31_t mix,
                            float stereoWidth, float stereoFreqMult, float stereoPhaseOffset, float evenAmount,
                            float rectAmount, float rect2Amount, float feedbackAmount, SineShaperParams* params,
                            float phaseHarmonic = 0.0f, bool* wasBypassed = nullptr) {
	// Early out - if mix is 0, do nothing (important CPU optimization)
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Per-sample drive interpolation for zipper-free parameter changes
	auto driveCtx = prepareSmoothing(*smoothedDrive, drive, buffer.size());
	*smoothedDrive = driveCtx.target; // Write back smoothed state

	// Smooth harmonic internally (fixes Zone 0 clicking from cascadeBlend/selfMulBlend jumps)
	// Note: 7th vs 9th harmonic selection is now computed internally by computeZone1*
	// based on a bipolar triangle at the posInZone - no need for external flag
	q31_t smoothedHarmonic = smoothParam(&params->smoothedHarmonic, harmonic);

	// Mark as no longer bypassed if transitioning
	if (wasBypassed && *wasBypassed) {
		*wasBypassed = false;
	}

	// Determine crossfade mode based on smoothed harmonic
	constexpr q31_t kZone1Threshold = ONE_Q31 / 8;       // Zone 1 starts at 1/8
	constexpr q31_t kZone2Threshold = ONE_Q31 / 4;       // Zone 2 starts at 2/8
	constexpr q31_t kZone3Threshold = (ONE_Q31 / 8) * 3; // Zone 3 starts at 3/8
	constexpr q31_t kZone4Threshold = ONE_Q31 / 2;       // Zone 4 starts at 4/8
	bool inZone0 = (smoothedHarmonic < kZone1Threshold);
	bool useChebyshevCrossfade = !inZone0;
	bool inZone1 = (smoothedHarmonic >= kZone1Threshold && smoothedHarmonic < kZone2Threshold);
	bool inZone2 = (smoothedHarmonic >= kZone2Threshold && smoothedHarmonic < kZone3Threshold);
	bool inZone3 = (smoothedHarmonic >= kZone3Threshold && smoothedHarmonic < kZone4Threshold);

	// Hoist zone weight calculation with per-sample coefficient interpolation
	// Per-sample smoothing eliminates zipper noise from triangle weight changes
	// Zone 0 reuses c1Ctx for phaseHarmonic smoothing (zones are orthogonal)
	Zone1Weights zone1WeightsL;
	Zone1Weights zone1WeightsR;
	FloatSmoothingContext c1Ctx{0, 0, 0}, c3LCtx{0, 0, 0}, c5LCtx{0, 0, 0}, c7LCtx{0, 0, 0}, c9LCtx{0, 0, 0};
	FloatSmoothingContext c3RCtx{0, 0, 0}, c5RCtx{0, 0, 0}, c7RCtx{0, 0, 0}, c9RCtx{0, 0, 0};
	bool usePerSampleWeights = (inZone0 || inZone1 || inZone2);
	float currentPhaseHarmonic = phaseHarmonic;

	if (inZone0) {
		// Zone 0: smooth phaseHarmonic using c1Ctx (reused since Zone 0 doesn't use Chebyshev coeffs)
		c1Ctx = prepareSmoothingFloat(params->smoothedC1, phaseHarmonic, buffer.size());
		currentPhaseHarmonic = c1Ctx.current;
	}
	else if (inZone1 || inZone2) {
		int32_t zoneIndex = inZone1 ? 1 : 2;
		float posInZone = computePosInZone(smoothedHarmonic, zoneIndex);
		// Apply phaseHarmonic offset (from Twist zone) - wraps around [0,1]
		posInZone = std::fmod(posInZone + phaseHarmonic + 1.0f, 1.0f);

		// === Position-based stereo: L and R computed at different harmonic positions ===
		// Instead of attenuating coefficients (which can break polynomial cancellation),
		// we compute L and R as independent valid Chebyshev blends at offset positions.
		// This guarantees both channels produce outputs within prescient gain bounds.

		// Compute animated stereo offset using 100% duty triangle (no dead zone)
		uint32_t phase = static_cast<uint32_t>(stereoPhaseOffset * stereoFreqMult * 4294967296.0f);
		float mod = static_cast<float>(getTriangle(phase)) / static_cast<float>(ONE_Q31);

		// Position offset: ±stereoWidth * 0.06 (6% of zone at max stereo)
		// Small offset creates subtle L/R differences without shifting overall harmonic character
		constexpr float kMaxPosOffset = 0.06f;
		float posOffset = stereoWidth * mod * kMaxPosOffset;

		// Compute L and R at different "virtual" positions (clamped to [0,1])
		float posL = std::clamp(posInZone - posOffset, 0.0f, 1.0f);
		float posR = std::clamp(posInZone + posOffset, 0.0f, 1.0f);

		Zone1Weights targetL = computeZone1WeightsFromPos(posL);
		Zone1Weights targetR = computeZone1WeightsFromPos(posR);

		// Prepare per-sample interpolation for L coefficients (c1 shared)
		c1Ctx = prepareSmoothingFloat(params->smoothedC1, targetL.c1, buffer.size());
		c3LCtx = prepareSmoothingFloat(params->smoothedC3L, targetL.c3, buffer.size());
		c5LCtx = prepareSmoothingFloat(params->smoothedC5L, targetL.c5, buffer.size());
		c7LCtx = prepareSmoothingFloat(params->smoothedC7L, targetL.c7, buffer.size());
		c9LCtx = prepareSmoothingFloat(params->smoothedC9L, targetL.c9, buffer.size());

		// Prepare per-sample interpolation for R coefficients (c1R = c1)
		c3RCtx = prepareSmoothingFloat(params->smoothedC3R, targetR.c3, buffer.size());
		c5RCtx = prepareSmoothingFloat(params->smoothedC5R, targetR.c5, buffer.size());
		c7RCtx = prepareSmoothingFloat(params->smoothedC7R, targetR.c7, buffer.size());
		c9RCtx = prepareSmoothingFloat(params->smoothedC9R, targetR.c9, buffer.size());

		// Initialize weights for first sample
		zone1WeightsL = {c1Ctx.current, c3LCtx.current, c5LCtx.current, c7LCtx.current, c9LCtx.current};
		zone1WeightsR = {c1Ctx.current, c3RCtx.current, c5RCtx.current, c7RCtx.current, c9RCtx.current};
	}

	// Hoist Zone 3 weight calculation with per-sample interpolation
	// Reuses Zone 1/2 smoothing contexts: c3→w0, c5→w1, c7→w2, c9→w3, c1→inputGain
	BlendWeights4 zone3WeightsL, zone3WeightsR;
	bool useZone3Weights = inZone3;

	if (useZone3Weights) {
		float posInZone = computePosInZone(smoothedHarmonic, 3);
		// Apply phaseHarmonic offset (from Twist zone) - wraps around [0,1]
		posInZone = std::fmod(posInZone + phaseHarmonic + 1.0f, 1.0f);

		// === Position-based stereo: L and R at different positions ===
		uint32_t phase = static_cast<uint32_t>(stereoPhaseOffset * stereoFreqMult * 4294967296.0f);
		float mod = static_cast<float>(getTriangle(phase)) / static_cast<float>(ONE_Q31);

		// Position offset: ±stereoWidth * 0.08 (8% of zone for Zone 3)
		constexpr float kMaxPosOffset = 0.08f;
		float posOffset = stereoWidth * mod * kMaxPosOffset;

		float posL = std::clamp(posInZone - posOffset, 0.0f, 1.0f);
		float posR = std::clamp(posInZone + posOffset, 0.0f, 1.0f);

		// Compute target weights at offset positions
		BlendWeights4 targetWeightsL = computeBlendWeights4(posL);
		BlendWeights4 targetWeightsR = computeBlendWeights4(posR);
		float targetInputGain = 1.0f + posInZone * 3.0f; // Input gain ramps with position

		// Reuse Zone 1/2 smoothing contexts: c3→w0, c5→w1, c7→w2, c9→w3, c1→inputGain
		// Prepare per-sample interpolation for L weights
		c3LCtx = prepareSmoothingFloat(params->smoothedC3L, targetWeightsL.w0, buffer.size());
		c5LCtx = prepareSmoothingFloat(params->smoothedC5L, targetWeightsL.w1, buffer.size());
		c7LCtx = prepareSmoothingFloat(params->smoothedC7L, targetWeightsL.w2, buffer.size());
		c9LCtx = prepareSmoothingFloat(params->smoothedC9L, targetWeightsL.w3, buffer.size());

		// Prepare per-sample interpolation for R weights
		c3RCtx = prepareSmoothingFloat(params->smoothedC3R, targetWeightsR.w0, buffer.size());
		c5RCtx = prepareSmoothingFloat(params->smoothedC5R, targetWeightsR.w1, buffer.size());
		c7RCtx = prepareSmoothingFloat(params->smoothedC7R, targetWeightsR.w2, buffer.size());
		c9RCtx = prepareSmoothingFloat(params->smoothedC9R, targetWeightsR.w3, buffer.size());

		// Prepare per-sample interpolation for input gain (shared L/R, uses c1)
		c1Ctx = prepareSmoothingFloat(params->smoothedC1, targetInputGain, buffer.size());

		// Initialize weights for first sample
		zone3WeightsL = {c3LCtx.current, c5LCtx.current, c7LCtx.current, c9LCtx.current, c1Ctx.current};
		zone3WeightsR = {c3RCtx.current, c5RCtx.current, c7RCtx.current, c9RCtx.current, c1Ctx.current};
	}

	// Zone 0 stereo: modulate drive bipolar (L gets +, R gets -)
	// Time-based LFO with frequency modulated by stereoFreqMult
	constexpr q31_t kZone0StereoDriveScale = ONE_Q31 / 8; // ±12.5% drive modulation
	q31_t stereoDriveOffset = 0;

	if (!useChebyshevCrossfade && stereoWidth > 0.0f) {
		constexpr float kTwoPi = 6.283185f;
		// LFO frequency: 0.1Hz min to 2Hz max (linear ramp through Wide zone)
		constexpr float kMinLfoHz = 0.1f;
		constexpr float kMaxLfoHz = 2.0f;
		constexpr float kSampleRate = 44100.0f;
		// stereoFreqMult: 1.0 at zone start → 4.0 at zone end, map to 0.1-2Hz
		float lfoHz = kMinLfoHz + (stereoFreqMult - 1.0f) * (kMaxLfoHz - kMinLfoHz) / 3.0f;
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
		float stereoMod = std::sin(phase * kTwoPi);
		stereoDriveOffset = static_cast<q31_t>(stereoWidth * stereoMod * static_cast<float>(kZone0StereoDriveScale));
	}

	// Local copy of state for efficient per-sample update
	// If voiceState is null, use local zeros (no persistent state across buffers)
	q31_t dcStateL = voiceState ? voiceState->dcBlockerL : 0;
	q31_t dcStateR = voiceState ? voiceState->dcBlockerR : 0;
	q31_t fbStateL = voiceState ? voiceState->feedbackL : 0;
	q31_t fbStateR = voiceState ? voiceState->feedbackR : 0;
	q31_t fbLpfStateL = voiceState ? voiceState->feedbackLpfL : 0;
	q31_t fbLpfStateR = voiceState ? voiceState->feedbackLpfR : 0;
	q31_t currentDrive = driveCtx.current;
	q31_t driveL = currentDrive;
	q31_t driveR = currentDrive;

	// Scale feedback amount for moderate self-oscillation range
	// At max (1.0), feedback is ~0.9 of output (just below self-oscillation)
	// In Zone 0, feedback is inversely proportional to harmonic position (more cascade = less feedback)
	// Also reduce feedback by up to 20% as drive increases (tames high-drive feedback)
	constexpr float kFeedbackScale = 0.9f;
	float fbScale = kFeedbackScale;
	constexpr q31_t kZone0End = ONE_Q31 / 8;
	if (smoothedHarmonic < kZone0End) {
		float zone0Pos = static_cast<float>(smoothedHarmonic) / static_cast<float>(kZone0End);
		fbScale *= (1.0f - zone0Pos); // 100% at start, 0% at end of Zone 0
	}
	// Drive-based reduction: -10% at max drive
	float driveNorm = static_cast<float>(drive) / static_cast<float>(ONE_Q31);
	fbScale *= (1.0f - 0.1f * driveNorm);
	q31_t fbAmount = static_cast<q31_t>(feedbackAmount * fbScale * static_cast<float>(ONE_Q31));

	for (auto& sample : buffer) {
		// Apply bipolar stereo drive offset (L gets +, R gets -)
		driveL = add_saturate(currentDrive, stereoDriveOffset);
		driveR = add_saturate(currentDrive, -stereoDriveOffset);

		// Apply feedback to input (before shaping)
		q31_t inputL = sample.l;
		q31_t inputR = sample.r;
		if (fbAmount > 0) {
			// Hard clip feedback to prevent runaway (~-6dB from full scale)
			q31_t clippedFbL = signed_saturate<22>(multiply_32x32_rshift32(fbStateL, fbAmount) << 1);
			q31_t clippedFbR = signed_saturate<22>(multiply_32x32_rshift32(fbStateR, fbAmount) << 1);
			inputL = add_saturate(sample.l, clippedFbL);
			inputR = add_saturate(sample.r, clippedFbR);
		}

		// Process left channel with L drive
		q31_t shapedL = sineShapeCore(
		    inputL, driveL, smoothedHarmonic, symmetry, (inZone1 || inZone2) ? &zone1WeightsL : nullptr,
		    useZone3Weights ? &zone3WeightsL : nullptr, evenAmount, rectAmount, rect2Amount, currentPhaseHarmonic);

		// Process right channel with R drive
		q31_t shapedR = sineShapeCore(
		    inputR, driveR, smoothedHarmonic, symmetry, (inZone1 || inZone2) ? &zone1WeightsR : nullptr,
		    useZone3Weights ? &zone3WeightsR : nullptr, evenAmount, rectAmount, rect2Amount, currentPhaseHarmonic);
		currentDrive += driveCtx.increment;

		// Update per-sample weights/values
		if (inZone0) {
			// Zone 0: smooth phaseHarmonic
			currentPhaseHarmonic = (c1Ctx.current += c1Ctx.increment);
		}
		else if (inZone1 || inZone2) {
			// Increment L coefficients (c1 shared)
			float c1 = (c1Ctx.current += c1Ctx.increment);
			zone1WeightsL.c1 = c1;
			zone1WeightsL.c3 = (c3LCtx.current += c3LCtx.increment);
			zone1WeightsL.c5 = (c5LCtx.current += c5LCtx.increment);
			zone1WeightsL.c7 = (c7LCtx.current += c7LCtx.increment);
			zone1WeightsL.c9 = (c9LCtx.current += c9LCtx.increment);

			// Increment R coefficients (c1R = c1)
			zone1WeightsR.c1 = c1;
			zone1WeightsR.c3 = (c3RCtx.current += c3RCtx.increment);
			zone1WeightsR.c5 = (c5RCtx.current += c5RCtx.increment);
			zone1WeightsR.c7 = (c7RCtx.current += c7RCtx.increment);
			zone1WeightsR.c9 = (c9RCtx.current += c9RCtx.increment);
		}

		// Update Zone 3 weights for next sample (reuses Zone 1/2 contexts)
		if (useZone3Weights) {
			float gain = (c1Ctx.current += c1Ctx.increment);
			// Increment L weights (c3→w0, c5→w1, c7→w2, c9→w3)
			zone3WeightsL.w0 = (c3LCtx.current += c3LCtx.increment);
			zone3WeightsL.w1 = (c5LCtx.current += c5LCtx.increment);
			zone3WeightsL.w2 = (c7LCtx.current += c7LCtx.increment);
			zone3WeightsL.w3 = (c9LCtx.current += c9LCtx.increment);
			zone3WeightsL.inputGainMult = gain;

			// Increment R weights
			zone3WeightsR.w0 = (c3RCtx.current += c3RCtx.increment);
			zone3WeightsR.w1 = (c5RCtx.current += c5RCtx.increment);
			zone3WeightsR.w2 = (c7RCtx.current += c7RCtx.increment);
			zone3WeightsR.w3 = (c9RCtx.current += c9RCtx.increment);
			zone3WeightsR.inputGainMult = gain;
		}

		// LPF the feedback tap to tame harsh high harmonics
		fbLpfStateL += multiply_32x32_rshift32(shapedL - fbLpfStateL, kFeedbackLpfAlpha) << 1;
		fbLpfStateR += multiply_32x32_rshift32(shapedR - fbLpfStateR, kFeedbackLpfAlpha) << 1;
		fbStateL = fbLpfStateL;
		fbStateR = fbLpfStateR;

		q31_t mixedL, mixedR;
		if (useChebyshevCrossfade) {
			// Chebyshev zones: traditional wet/dry crossfade
			// At mix=0: pure dry, mix=1: pure wet
			q31_t dryCoeff = ONE_Q31 - multiply_32x32_rshift32(mix, ONE_Q31) * 2; // 1 - mix
			mixedL = add_saturate(multiply_32x32_rshift32(sample.l, dryCoeff) << 1,
			                      multiply_32x32_rshift32(shapedL, mix) << 1);
			mixedR = add_saturate(multiply_32x32_rshift32(sample.r, dryCoeff) << 1,
			                      multiply_32x32_rshift32(shapedR, mix) << 1);
		}
		else {
			// Zone 0: Traditional wet/dry crossfade
			q31_t invMix = ONE_Q31 - mix;
			mixedL = add_saturate(multiply_32x32_rshift32(sample.l, invMix) << 1, multiply_32x32_rshift32(shapedL, mix)
			                                                                          << 1);
			mixedR = add_saturate(multiply_32x32_rshift32(sample.r, invMix) << 1, multiply_32x32_rshift32(shapedR, mix)
			                                                                          << 1);
		}

		// DC blocker (5Hz highpass) - removes DC offset from asymmetry
		dcStateL += multiply_32x32_rshift32(mixedL - dcStateL, kDcBlockerAlpha) * 2;
		dcStateR += multiply_32x32_rshift32(mixedR - dcStateR, kDcBlockerAlpha) * 2;
		sample.l = mixedL - dcStateL;
		sample.r = mixedR - dcStateR;
	}

	// Write back state if voiceState is not null
	if (voiceState) {
		voiceState->dcBlockerL = dcStateL;
		voiceState->dcBlockerR = dcStateR;
		voiceState->feedbackL = fbStateL;
		voiceState->feedbackR = fbStateR;
		voiceState->feedbackLpfL = fbLpfStateL;
		voiceState->feedbackLpfR = fbLpfStateR;
	}
	if (inZone0) {
		// Zone 0: write back smoothed phaseHarmonic (stored in smoothedC1)
		params->smoothedC1 = c1Ctx.target;
	}
	else if (inZone1 || inZone2 || useZone3Weights) {
		// Zone 1/2/3: write back smoothed coefficients/weights
		// Zone 1/2 uses c1-c9 for Chebyshev coefficients
		// Zone 3 reuses same state: c3→w0, c5→w1, c7→w2, c9→w3, c1→inputGain
		params->smoothedC1 = c1Ctx.target;
		params->smoothedC3L = c3LCtx.target;
		params->smoothedC5L = c5LCtx.target;
		params->smoothedC7L = c7LCtx.target;
		params->smoothedC9L = c9LCtx.target;
		params->smoothedC3R = c3RCtx.target;
		params->smoothedC5R = c5RCtx.target;
		params->smoothedC7R = c7RCtx.target;
		params->smoothedC9R = c9RCtx.target;
	}
}

// ============================================================================
// Tanh-based Sine Shaper (uses antialiased lookup table)
// ============================================================================
// Alternative implementation using getTanHAntialiased for smoother saturation
// with built-in anti-aliasing. Requires state variables per channel.

/**
 * Single sample tanh-based shaping with antialiasing
 * @param input Sample to process
 * @param drive Saturation amount (higher = more harmonics)
 * @param lastWorkingValue State variable for antialiasing (per-channel)
 * @param mix Wet/dry blend
 */
inline q31_t sineShapeTanh(q31_t input, q31_t drive, uint32_t* lastWorkingValue, q31_t mix) {
	if (mix <= 0) {
		return input;
	}

	// Calculate saturation amount from drive (1-8 range for getTanHAntialiased)
	// drive is 0 to ~ONE_Q31, map to saturation 1-6
	uint32_t saturationAmount = 1 + static_cast<uint32_t>(multiply_32x32_rshift32(drive, 5));

	// Apply tanh saturation with antialiasing
	q31_t shaped = getTanHAntialiased(input, lastWorkingValue, saturationAmount);

	// Wet/dry crossfade
	q31_t invMix = ONE_Q31 - mix;
	q31_t dryPart = multiply_32x32_rshift32(input, invMix) << 1;
	q31_t wetPart = multiply_32x32_rshift32(shaped, mix) << 1;

	return add_saturate(dryPart, wetPart);
}

/**
 * Process a stereo buffer through tanh-based shaper
 * @param buffer Stereo audio buffer
 * @param drive Saturation amount
 * @param stateL Left channel antialiasing state
 * @param stateR Right channel antialiasing state
 * @param mix Wet/dry blend
 */
inline void sineShapeBufferTanh(StereoBuffer<q31_t> buffer, q31_t drive, uint32_t* stateL, uint32_t* stateR,
                                q31_t mix) {
	if (mix <= 0) {
		return;
	}

	for (auto& sample : buffer) {
		sample.l = sineShapeTanh(sample.l, drive, stateL, mix);
		sample.r = sineShapeTanh(sample.r, drive, stateR, mix);
	}
}

} // namespace deluge::dsp
