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
#include "deluge/util/fixedpoint.h"
#include "deluge/util/functions.h"
#include "dsp/fast_math.h"
#include "dsp/saturator.h"
#include "dsp_ng/core/types.hpp"
#include "util/waves.h"
#include <cmath>
#include <span>

namespace deluge::dsp {

// ============================================================================
// Parameter Smoothing Helper
// ============================================================================

/// Smoothing time constant (~100ms at 44.1kHz with 128-sample buffers)
constexpr q31_t kSmoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);

/// DC blocker coefficient for 5Hz cutoff at 44.1kHz
/// alpha = 2 * pi * fc / fs = 2 * pi * 5 / 44100 ≈ 0.000712
constexpr q31_t kDcBlockerAlpha = static_cast<q31_t>(0.000712 * ONE_Q31);

/// Feedback LPF coefficient for ~6kHz cutoff at 44.1kHz
/// alpha = 1 - exp(-2π * fc / fs) ≈ 0.58 for fc=6kHz
/// Tames harsh high harmonics in feedback loop
constexpr q31_t kFeedbackLpfAlpha = static_cast<q31_t>(0.58 * ONE_Q31);

/// Context for per-sample parameter smoothing during buffer processing
/// Provides click-free parameter interpolation over one buffer
struct SmoothingContext {
	q31_t current;     // Current interpolated value (increment each sample)
	int32_t increment; // Per-sample increment
	q31_t target;      // Target smoothed value (write back to state after buffer)
};

/// Prepare parameter smoothing for buffer processing
/// @param state Current smoothed state value
/// @param target Target parameter value
/// @param bufferSize Number of samples in buffer
/// @return SmoothingContext for use during buffer processing
inline SmoothingContext prepareSmoothing(q31_t state, q31_t target, size_t bufferSize) {
	q31_t targetSmoothed = state + multiply_32x32_rshift32(target - state, kSmoothingAlpha) * 2;
	int32_t increment = (targetSmoothed - state) / static_cast<int32_t>(bufferSize);
	return {state, increment, targetSmoothed};
}

/// Simple buffer-rate smoothing (once per buffer, not per-sample)
/// Updates state in place and returns the smoothed value
/// @param state Pointer to smoothed state (updated in place)
/// @param target Target parameter value
/// @return Smoothed value for this buffer
[[gnu::always_inline]] inline q31_t smoothParam(q31_t* state, q31_t target) {
	*state = *state + multiply_32x32_rshift32(target - *state, kSmoothingAlpha) * 2;
	return *state;
}

/// Float version of SmoothingContext for per-sample coefficient interpolation
struct FloatSmoothingContext {
	float current;   // Current interpolated value (increment each sample)
	float increment; // Per-sample increment
	float target;    // Target value (write back to state after buffer)
};

/// Prepare float smoothing for per-sample coefficient interpolation
/// @param state Current smoothed state value
/// @param target Target coefficient value
/// @param bufferSize Number of samples in buffer
/// @return FloatSmoothingContext for use during buffer processing
inline FloatSmoothingContext prepareSmoothingFloat(float state, float target, size_t bufferSize) {
	// Use same alpha as buffer-rate smoothing to determine target
	constexpr float kAlpha = 0.03f; // Match kSmoothingAlpha
	float targetSmoothed = state + (target - state) * kAlpha;
	float increment = (targetSmoothed - state) / static_cast<float>(bufferSize);
	return {state, increment, targetSmoothed};
}

/// Float version of buffer-rate smoothing for coefficient smoothing
/// Uses slow rate to avoid zipper noise from triangle weight jumps
[[gnu::always_inline]] inline float smoothParamFloat(float* state, float target) {
	constexpr float kCoeffAlpha = 0.015f; // ~200ms to reach 95% at 128 samples/buffer
	*state = *state + (target - *state) * kCoeffAlpha;
	return *state;
}

// ============================================================================
// Wavefolder
// ============================================================================

/**
 * Fold reduces the input by the amount it's over the level
 */
constexpr q31_t FOLD_MIN = 0.1 * ONE_Q31;
constexpr q31_t THREE_FOURTHS = 0.75 * ONE_Q31;
inline q31_t fold(q31_t input, q31_t level) {
	// no folding occurs if max is 0 or if max is greater than input
	// to keep the knob range consistent fold starts from 0 and
	// increases, decreasing would lead to a large deadspace until
	// suddenly clipping occured
	// note 9db loss
	q31_t extra = 0;
	q31_t max = level >> 8;
	if (input > max) {
		extra = input - max;
	}
	else if (input < -max) {
		extra = input + max;
	}
	// this avoids inverting the wave
	return 2 * extra - input;
}
/**
 * Polynomial waveshaper with double-cascade for rich harmonic content
 *
 * Applies P(x) = 8 * (0.75*x - x³) = 6x - 8x³ twice (internal cascade).
 * This creates a wavefolding-like effect with specific harmonic character.
 *
 * CRITICAL: This function relies on 32-bit integer overflow behavior.
 * At high input levels, intermediate values exceed INT32_MAX and wrap around,
 * creating natural saturation/wavefolding. This is essential to the sound.
 *
 * Float equivalent (for reference - DO NOT USE, loses overflow behavior):
 *   auto P = [](float x) { return 6.0f * x - 8.0f * x * x * x; };
 *   float r1 = P(x);
 *   return P(r1);
 *
 * @param x Input in q31 format (ONE_Q31 = 1.0)
 * @return Shaped output in q31 format
 */
inline q31_t polynomialOscillatorApproximation(q31_t x) {
	// Uses rounded multiply to avoid asymmetric truncation bias
	// (truncation rounds positive toward 0, negative toward -inf, causing DC offset)

	// First pass: P(x) = 8 * (0.75*x - x³) = 6x - 8x³
	q31_t x2 = 2 * multiply_32x32_rshift32_rounded(x, x);
	q31_t x3 = 2 * multiply_32x32_rshift32_rounded(x2, x);
	q31_t r1 = 8 * (multiply_32x32_rshift32_rounded(THREE_FOURTHS, x) - x3);

	// Second pass: P(r1) - applies polynomial again for richer harmonics
	q31_t r2 = 2 * multiply_32x32_rshift32_rounded(r1, r1);
	q31_t r3 = 2 * multiply_32x32_rshift32_rounded(r2, r1);
	q31_t out = 8 * (multiply_32x32_rshift32_rounded(THREE_FOURTHS, r1) - r3);

	return out;
}

inline void foldBufferPolyApproximation(std::span<q31_t> buffer, q31_t level) {
	q31_t fold_level = add_saturate(level, FOLD_MIN);
	for (auto& sample : buffer) {
		q31_t x = lshiftAndSaturateUnknown(multiply_32x32_rshift32(fold_level, sample), 8);
		// volume compensation
		sample = polynomialOscillatorApproximation(x) >> 7;
	}
}

inline void foldBufferPolyApproximation(StereoBuffer<q31_t> buffer, q31_t level) {
	foldBufferPolyApproximation(std::span<q31_t>{reinterpret_cast<q31_t*>(buffer.data()), buffer.size() * 2}, level);
}

/**
 * Smoothed wavefold with parameter interpolation to avoid zipper noise
 * @param buffer Audio buffer to process in place
 * @param level Current target fold level
 * @param smoothedLevel Pointer to smoothed level state (updated in place)
 */
inline void foldBufferPolyApproximationSmoothed(std::span<q31_t> buffer, q31_t level, q31_t* smoothedLevel) {
	if ((level <= 0 && *smoothedLevel <= 0) || buffer.empty()) {
		return;
	}

	auto ctx = prepareSmoothing(*smoothedLevel, level, buffer.size());

	for (auto& sample : buffer) {
		ctx.current += ctx.increment;
		if (ctx.current > 0) {
			q31_t fold_level = add_saturate(ctx.current, FOLD_MIN);
			q31_t x = lshiftAndSaturateUnknown(multiply_32x32_rshift32(fold_level, sample), 8);
			sample = polynomialOscillatorApproximation(x) >> 7;
		}
	}

	*smoothedLevel = ctx.target;
}

inline void foldBufferPolyApproximationSmoothed(StereoBuffer<q31_t> buffer, q31_t level, q31_t* smoothedLevel) {
	foldBufferPolyApproximationSmoothed(std::span<q31_t>{reinterpret_cast<q31_t*>(buffer.data()), buffer.size() * 2},
	                                    level, smoothedLevel);
}

/**
 * foldBuffer folds a whole buffer. Works for stereo too
 */
inline void foldBuffer(std::span<q31_t> buffer, q31_t foldLevel) {
	for (auto& sample : buffer) {
		auto out = fold(sample, foldLevel);
		// volume compensation
		sample = out + 4 * multiply_32x32_rshift32(out, foldLevel);
	}
}

// ============================================================================
// Enhanced Wavefolder with additional parameters
// ============================================================================

/**
 * Enhanced wavefold with drive and symmetry controls
 * @param input Sample to process
 * @param level Fold amount (original parameter)
 * @param drive Input gain multiplier (0 = 1x, max = ~4x)
 * @param symmetry DC bias for asymmetric folding (center = symmetric)
 * @return Folded sample
 */
inline q31_t wavefoldEnhanced(q31_t input, q31_t level, q31_t drive, q31_t symmetry) {
	// Early out if level is 0
	if (level <= 0) {
		return input;
	}

	// Apply symmetry (DC offset for asymmetric wavefolding)
	q31_t biased = add_saturate(input, symmetry >> 1);

	// Apply drive - scales input before folding
	// drive of 0 = 1x, max = ~4x
	q31_t driveMultiplier = add_saturate(ONE_Q31 >> 2, multiply_32x32_rshift32(drive, 3));
	q31_t driven = lshiftAndSaturate<2>(multiply_32x32_rshift32(biased, driveMultiplier));

	// Apply the polynomial folding
	q31_t fold_level = add_saturate(level, FOLD_MIN);
	q31_t x = lshiftAndSaturateUnknown(multiply_32x32_rshift32(fold_level, driven), 8);
	return polynomialOscillatorApproximation(x) >> 7;
}

/**
 * Process a buffer through the enhanced wavefolder
 * @param buffer Audio buffer to process in place
 * @param level Fold amount
 * @param drive Input gain (0 = bypass drive)
 * @param symmetry DC bias for asymmetry
 */
inline void wavefoldBufferEnhanced(std::span<q31_t> buffer, q31_t level, q31_t drive, q31_t symmetry) {
	// Early out if level is 0
	if (level <= 0) {
		return;
	}

	for (auto& sample : buffer) {
		sample = wavefoldEnhanced(sample, level, drive, symmetry);
	}
}

/**
 * Process a stereo buffer through the enhanced wavefolder
 */
inline void wavefoldBufferEnhanced(StereoBuffer<q31_t> buffer, q31_t level, q31_t drive, q31_t symmetry) {
	wavefoldBufferEnhanced(std::span<q31_t>{reinterpret_cast<q31_t*>(buffer.data()), buffer.size() * 2}, level, drive,
	                       symmetry);
}

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
	// Note: DC blocker state is per-voice (in Voice) or per-clip (in GlobalEffectableForClip)
	// Smoothed Zone 1/2 coefficients (prevents clicks when triangle weights jump)
	// L channel (c1 shared with R since zero-sum deltas preserve it)
	float smoothedC1{1.0f}, smoothedC3L{0}, smoothedC5L{0}, smoothedC7L{0}, smoothedC9L{0};
	// R channel (c1R = c1, so only c3R/c5R/c7R/c9R needed)
	float smoothedC3R{0}, smoothedC5R{0}, smoothedC7R{0}, smoothedC9R{0};
	// Feedback LPF state (tames harsh high harmonics in feedback loop)
	q31_t feedbackLpfL{0}, feedbackLpfR{0};
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
	                          q31_t& outT9) {
		// Normalize to 0-1 range
		float paramNorm = static_cast<float>(harmonicParam) / static_cast<float>(ONE_Q31);

		// Determine zone (0-7)
		outZone = static_cast<int32_t>(paramNorm * kNumHarmonicZones);
		if (outZone >= kNumHarmonicZones) {
			outZone = kNumHarmonicZones - 1;
		}

		// Zone 0 = Poly, no Chebyshev weights needed
		if (outZone == 0) {
			outT3 = outT5 = outT7 = outT9 = 0;
			return;
		}

		// For zones 1-7, compute triangle-modulated weights
		// Use position across zones 1-7 (not just within current zone)
		float chebyPos = (paramNorm - 1.0f / kNumHarmonicZones) * (kNumHarmonicZones / (kNumHarmonicZones - 1.0f));
		chebyPos = std::clamp(chebyPos, 0.0f, 1.0f);

		// Zone 7 gets faster oscillation for extra chaos
		float freqMult = (outZone == 7) ? 2.0f : 1.0f;

		// Convert float phase (0-N cycles) to uint32_t phase
		// Must wrap to [0,1) before scaling to avoid UB from float->uint32 overflow
		constexpr float kPhaseScale = 4294967296.0f;
		auto toPhase = [](float f) {
			f = std::fmod(f, 1.0f);
			if (f < 0.0f) {
				f += 1.0f;
			}
			return static_cast<uint32_t>(f * kPhaseScale);
		};

		// Compute weights with triangle oscillators (returns q31 directly)
		outT3 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT3 * freqMult + kPhaseT3), kPhaseWidthT3);
		outT5 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT5 * freqMult + kPhaseT5), kPhaseWidthT5);
		outT7 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT7 * freqMult + kPhaseT7), kPhaseWidthT7);
		outT9 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT9 * freqMult + kPhaseT9), kPhaseWidthT9);
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
	constexpr float kEpsilon = 1e-6f;

	// === Base weights: unipolar triangles with log scaling for perceptual uniformity ===
	// Log scaling: each encoder step produces roughly equal dB change in harmonic balance
	// 40dB dynamic range: weight = 10^((linearTri - 1) * 2) = fastExp((linearTri - 1) * 4.605)
	constexpr float kLogScale = 4.605f;           // 2 * ln(10) for 40dB range
	constexpr float kMinWeight = 0.01f;           // -40dB floor (prevents div by zero)
	constexpr uint32_t kPhaseWidth = 0xB3333333u; // 70% duty
	// Triangle frequencies: ~2 cycles/zone with irrational ratios to avoid periodicity
	constexpr float kFreqW3 = 2.019f;   // √29/2 * 0.75 (~2.0 cycles/zone)
	constexpr float kFreqW5 = 2.356f;   // π * 0.75     (~2.4 cycles/zone)
	constexpr float kFreqW7_9 = 2.771f; // e²/2 * 0.75  (~2.8 cycles/zone) - shared for w7/w9

	// Convert linear triangle (0-1) to log-scaled weight (0.01-1.0, 40dB range)
	auto linearToLog = [](float linear) {
		if (linear <= 0.0f) {
			return kMinWeight;
		}
		return std::fmax(kMinWeight, fastExp((linear - 1.0f) * kLogScale));
	};

	float tri3 = static_cast<float>(triangleWithDeadzone(toPhase(posInZone * kFreqW3 + 0.0f), kPhaseWidth)) * kInvQ31;
	float tri5 = static_cast<float>(triangleWithDeadzone(toPhase(posInZone * kFreqW5 + 0.25f), kPhaseWidth)) * kInvQ31;
	float w3 = linearToLog(tri3);
	float w5 = linearToLog(tri5);

	// 7th and 9th from bipolar triangle with dead zone: positive = 7th, negative = 9th
	// Same 70% duty as w3/w5, split: 35% w7, 35% w9, 30% silent
	int32_t modeTriangle = triangleWithDeadzoneBipolar(toPhase(posInZone * kFreqW7_9 + 0.5f), kPhaseWidth);
	float tri79 = static_cast<float>(std::abs(modeTriangle)) * kInvQ31;
	float w79_log = linearToLog(tri79);
	float w7 = (modeTriangle > 0) ? w79_log : kMinWeight;
	float w9 = (modeTriangle < 0) ? w79_log : kMinWeight;

	// Normalize base weights
	float wSum = w3 + w5 + w7 + w9;
	w3 /= wSum;
	w5 /= wSum;
	w7 /= wSum;
	w9 /= wSum;

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
 * Zone 0: Asym - DC offset for even harmonics
 * Zone 1: Wide - Stereo spread with animated phase evolution
 *         Width envelope: 0→1 (0-25%), plateau (25-75%), 1→0 (75-100%)
 *         Freq multiplier: 1× (0-75%), ramps to 4× (75-100%)
 *         Phase offset: continuous evolution throughout
 * Zone 2: Even - Self-mul for even harmonics (3→6, 5→10, 7→14)
 * Zone 3: Rect - Rectifier blend (pure octave up)
 * Zone 4: Reserved
 * Zone 5: Feedback - Output→input recirculation
 */
struct SineShaperTwistParams {
	q31_t symmetry{0};             // Zone 0: DC offset for asymmetry
	float stereoWidth{0.0f};       // Zone 1: stereo spread envelope (peaks 25-75%)
	float stereoFreqMult{1.0f};    // Zone 1: oscillation frequency multiplier
	float stereoPhaseOffset{0.0f}; // Zone 1: continuous phase evolution (0-1)
	float evenAmount{0.0f};        // Zone 2: self-mul amount (0.0 to 4.0 for 12dB)
	float rectAmount{0.0f};        // Zone 3: rectifier blend (0.0 to 1.0)
	float feedbackAmount{0.0f};    // Zone 5: feedback depth (0.0 to 1.0)
};

/**
 * Derive all Twist-dependent parameters from smoothed Twist value
 * Twist zones: 0=Asym, 1=Wide, 2=Even, 3=Rect, 4=Fold, 5=Feedback
 *
 * @param smoothedTwist Already-smoothed Twist parameter value
 * @return SineShaperTwistParams with all derived values
 */
inline SineShaperTwistParams computeSineShaperTwistParams(q31_t smoothedTwist) {
	constexpr q31_t kZone1Threshold = ONE_Q31 / 8;
	constexpr q31_t kZone2Threshold = ONE_Q31 / 4;
	constexpr q31_t kZone3Threshold = (ONE_Q31 / 8) * 3; // 3/8
	constexpr q31_t kZone4Threshold = ONE_Q31 / 2;       // 4/8
	constexpr q31_t kZone5Threshold = (ONE_Q31 / 8) * 5; // 5/8
	constexpr q31_t kZone6Threshold = (ONE_Q31 / 4) * 3; // 6/8

	SineShaperTwistParams params;

	if (smoothedTwist < kZone1Threshold) {
		// Zone 0: Asymmetry - DC offset for even harmonics
		params.symmetry = smoothedTwist << 3;
	}
	else if (smoothedTwist < kZone2Threshold) {
		// Zone 1: Wide stereo with animated phase evolution
		q31_t posInZoneQ31 = (smoothedTwist - kZone1Threshold) << 3;
		float pos = static_cast<float>(posInZoneQ31) / static_cast<float>(ONE_Q31);

		// Width envelope: trapezoidal shape
		// 0-25%: ramp up 0→1, 25-75%: plateau at 1, 75-100%: ramp down 1→0
		if (pos < 0.25f) {
			params.stereoWidth = pos * 4.0f; // 0→1 over 25%
		}
		else if (pos < 0.75f) {
			params.stereoWidth = 1.0f; // Plateau
		}
		else {
			params.stereoWidth = (1.0f - pos) * 4.0f; // 1→0 over last 25%
		}

		// Frequency multiplier: ramps from 1× to 4× on the down slope (75-100%)
		if (pos <= 0.75f) {
			params.stereoFreqMult = 1.0f;
		}
		else {
			// Linear ramp from 1× at 75% to 4× at 100%
			params.stereoFreqMult = 1.0f + (pos - 0.75f) * 12.0f;
		}

		// Phase offset: continuous evolution throughout zone (0 to 1)
		params.stereoPhaseOffset = pos;
	}
	else if (smoothedTwist < kZone3Threshold) {
		// Zone 2: Even harmonics - self-mul amount (0.0 to 4.0 for 12dB peak gain)
		q31_t posInZone = (smoothedTwist - kZone2Threshold) << 3;
		params.evenAmount = static_cast<float>(posInZone) / static_cast<float>(ONE_Q31) * 4.0f;
	}
	else if (smoothedTwist < kZone4Threshold) {
		// Zone 3: Rectifier - blend toward |result| (0.0 to 1.0)
		q31_t posInZone = (smoothedTwist - kZone3Threshold) << 3;
		params.rectAmount = static_cast<float>(posInZone) / static_cast<float>(ONE_Q31);
	}
	else if (smoothedTwist < kZone5Threshold) {
		// Zone 4: Reserved (no effect)
	}
	else if (smoothedTwist < kZone6Threshold) {
		// Zone 5: Feedback - output→input recirculation (0.0 to 1.0)
		q31_t posInZone = (smoothedTwist - kZone5Threshold) << 3;
		params.feedbackAmount = static_cast<float>(posInZone) / static_cast<float>(ONE_Q31);
	}
	// Zones 6-7: Reserved, no effect

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
 * @param evenAmount Self-mul for even harmonics (Twist Zone 2, 0.0 to 4.0)
 * @param rectAmount Rectifier blend toward |result| (Twist Zone 3, 0.0 to 1.0)
 */
inline q31_t sineShapeCore(q31_t input, q31_t drive, q31_t harmonic, q31_t symmetry,
                           const Zone1Weights* zone1Weights = nullptr, float evenAmount = 0.0f,
                           float rectAmount = 0.0f) {
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
	SineShaperHarmonicMapper::deriveWeights(harmonic, zone, wT3, wT5, wT7, wT9);

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
		shaped = polynomialOscillatorApproximation(scaledInput) >> 7;

		// Cascaded polynomial adds more harmonics via second pass
		// Scale so blend reaches maximum at end of zone 0 (1/8 of range)
		q31_t cascadeBlend = (harmonic >= ONE_Q31 / 8) ? ONE_Q31 : (harmonic << 3);

		if (cascadeBlend > 0) {
			q31_t moreHarmonics = polynomialOscillatorApproximation(shaped << 7) >> 7;
			shaped = shaped + multiply_32x32_rshift32_rounded(moreHarmonics - shaped, cascadeBlend) * 2;
		}

		// Self-multiplication: x * |x| creates octave content while preserving sign
		// Blend increases through zone - adds richness before transition to Chebyshev
		if (cascadeBlend > (ONE_Q31 / 4)) {
			// selfMul = shaped * |shaped|, scaled to maintain level
			q31_t selfMul = multiply_32x32_rshift32_rounded(shaped, std::abs(shaped)) << 1;
			// Blend from 25% to 100% of zone maps to 0% to 50% self-mul
			q31_t selfMulBlend = (cascadeBlend - (ONE_Q31 / 4)) >> 1;
			shaped = shaped + multiply_32x32_rshift32_rounded(selfMul - shaped, selfMulBlend) * 2;
		}

		// === Post-polynomial modifiers (Twist zones 2-3) also apply to Zone 0 ===
		// Even: self-mul adds even harmonics (in q31)
		if (evenAmount > 0.0f) {
			q31_t selfMul = multiply_32x32_rshift32_rounded(shaped, std::abs(shaped)) << 1;
			shaped = add_saturate(shaped, static_cast<q31_t>(static_cast<float>(selfMul) * evenAmount));
		}

		// Rect: blend toward |shaped| (pure octave up, in q31)
		if (rectAmount > 0.0f) {
			q31_t absVal = std::abs(shaped);
			q31_t rectScale = static_cast<q31_t>(rectAmount * static_cast<float>(ONE_Q31));
			shaped = shaped + multiply_32x32_rshift32_rounded(absVal - shaped, rectScale) * 2;
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

		// === Post-polynomial modifiers (Twist zones 2-4) ===
		// Applied in order: Even → Rect → Fold

		// Even: self-mul adds even harmonics (3→6, 5→10, 7→14 + intermod)
		if (evenAmount > 0.0f) {
			result += result * std::abs(result) * evenAmount;
		}

		// Rect: blend toward |result| (pure octave up)
		if (rectAmount > 0.0f) {
			result = result + (std::abs(result) - result) * rectAmount;
		}

		// No output limiting - clipping reveals gain staging issues
		shaped = static_cast<q31_t>(result * static_cast<float>(ONE_Q31)) >> 6;
	}
	else if (zone == 3) {
		// === Zone 3 "FM": Dual sine with frequency offset ===
		// Primary path: drive controls phase depth (waveshaping intensity)
		// Parallel path: position controls frequency offset (harmonic relationship)

		float posInZone = computePosInZone(harmonic, zone);
		constexpr float kInvQ30 = 1.0f / static_cast<float>(ONE_Q31 >> 1);

		// Primary sine: drive-controlled phase (fundamental waveshaping)
		float phase1F = inputF * 256.0f;
		uint32_t phase1 = static_cast<uint32_t>(static_cast<int64_t>(phase1F));
		int32_t sine1 = getSine(phase1);
		float result1 = static_cast<float>(sine1) * kInvQ30;

		// Parallel sine: position-controlled frequency offset (2x to 4x = octave to 2 octaves)
		float ratio = 2.0f + posInZone * 2.0f;
		float phase2F = inputF * ratio * 256.0f;
		uint32_t phase2 = static_cast<uint32_t>(static_cast<int64_t>(phase2F));
		int32_t sine2 = getSine(phase2);
		float result2 = static_cast<float>(sine2) * kInvQ30;

		// Blend: position also controls mix (0% parallel at start, 50% at end)
		float parallelMix = posInZone * 0.5f;
		float result = result1 * (1.0f - parallelMix) + result2 * parallelMix;

		// Apply post-modifiers (even/rect) for consistency
		if (evenAmount > 0.0f) {
			result += result * std::abs(result) * evenAmount;
		}
		if (rectAmount > 0.0f) {
			result = result + (std::abs(result) - result) * rectAmount;
		}

		// Scale down output - use ONE_Q31 >> 1 to keep well within q31 range
		shaped = static_cast<q31_t>(result * static_cast<float>(ONE_Q31 >> 1)) >> 6;
	}
	else {
		// === Zones 4-7: Placeholder (reserved) ===
		// Simple passthrough with gain matching
		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 9);
		shaped = scaledInput >> 7;
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

	// === Output gain reduction ===
	// Waveshaping produces hot output; attenuate to match bypass level
	shaped = shaped >> 1; // -6dB

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
 * @param dcBlocker Pointer to DC blocker state (updated in place)
 * @param harmonic Raw harmonic value (smoothed internally via params->smoothedHarmonic)
 * @param symmetry DC bias for asymmetry (Twist Zone 0)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param evenAmount Self-mul for even harmonics (Twist Zone 2, 0.0 to 4.0)
 * @param rectAmount Rectifier blend (Twist Zone 3, 0.0 to 1.0)
 * @param feedbackAmount Feedback depth (Twist Zone 5, 0.0 to 1.0)
 * @param feedback Pointer to feedback state (updated in place)
 * @param params Pointer to SineShaperParams for coefficient smoothing (required)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void sineShapeBuffer(std::span<q31_t> buffer, q31_t drive, q31_t* smoothedDrive, q31_t* dcBlocker,
                            q31_t harmonic, q31_t symmetry, q31_t mix, float evenAmount, float rectAmount,
                            float feedbackAmount, q31_t* feedback, SineShaperParams* params,
                            bool* wasBypassed = nullptr) {
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
	bool useChebyshevCrossfade = (smoothedHarmonic >= kZone1Threshold);
	bool inZone1 = (smoothedHarmonic >= kZone1Threshold && smoothedHarmonic < kZone2Threshold);
	bool inZone2 = (smoothedHarmonic >= kZone2Threshold);

	// Hoist Zone 1/2 weight calculation with per-sample coefficient interpolation
	// Per-sample smoothing eliminates zipper noise from triangle weight changes
	Zone1Weights zone1Weights;
	FloatSmoothingContext c1Ctx{0, 0, 0}, c3Ctx{0, 0, 0}, c5Ctx{0, 0, 0}, c7Ctx{0, 0, 0}, c9Ctx{0, 0, 0};
	bool usePerSampleWeights = (inZone1 || inZone2);

	if (usePerSampleWeights) {
		int32_t zoneIndex = inZone1 ? 1 : 2;
		float posInZone = computePosInZone(smoothedHarmonic, zoneIndex);
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
	q31_t dcState = *dcBlocker;
	q31_t fbState = feedback ? *feedback : 0;
	q31_t fbLpfState = params->feedbackLpfL;
	q31_t currentDrive = driveCtx.current;

	// Scale feedback amount for moderate self-oscillation range
	// At max (1.0), feedback is ~0.9 of output (just below self-oscillation)
	constexpr float kFeedbackScale = 0.9f;
	q31_t fbAmount = static_cast<q31_t>(feedbackAmount * kFeedbackScale * static_cast<float>(ONE_Q31));

	for (auto& sample : buffer) {
		// Apply feedback to input (before shaping)
		q31_t inputWithFb = sample;
		if (fbAmount > 0) {
			// Hard clip feedback to prevent runaway (same as oscillator feedback)
			q31_t clippedFb = signed_saturate<22>(multiply_32x32_rshift32(fbState, fbAmount) << 1);
			inputWithFb = add_saturate(sample, clippedFb);
		}

		// Get shaped (wet) signal with all modifiers (drive + weights interpolated per-sample)
		q31_t shaped = sineShapeCore(inputWithFb, currentDrive, smoothedHarmonic, symmetry,
		                             usePerSampleWeights ? &zone1Weights : nullptr, evenAmount, rectAmount);
		currentDrive += driveCtx.increment;

		// Update weights for next sample (per-sample interpolation)
		if (usePerSampleWeights) {
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

	*dcBlocker = dcState;
	params->feedbackLpfL = fbLpfState;
	if (usePerSampleWeights) {
		// Write back smoothed weight state for next buffer
		params->smoothedC1 = c1Ctx.target;
		params->smoothedC3L = c3Ctx.target;
		params->smoothedC5L = c5Ctx.target;
		params->smoothedC7L = c7Ctx.target;
		params->smoothedC9L = c9Ctx.target;
	}
	if (feedback) {
		*feedback = fbState;
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
 * @param dcBlockerL Pointer to left channel DC blocker state (updated in place)
 * @param dcBlockerR Pointer to right channel DC blocker state (updated in place)
 * @param harmonic Raw harmonic value (smoothed internally via params->smoothedHarmonic)
 * @param symmetry DC bias for asymmetry (Twist Zone 0)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param stereoWidth Stereo coefficient spread envelope (peaks at 25-75% of Wide zone)
 * @param stereoFreqMult Stereo oscillation frequency multiplier (1× to 4×)
 * @param stereoPhaseOffset Stereo phase evolution (0-1 across Wide zone)
 * @param evenAmount Self-mul for even harmonics (Twist Zone 2, 0.0 to 4.0)
 * @param rectAmount Rectifier blend (Twist Zone 3, 0.0 to 1.0)
 * @param feedbackAmount Feedback depth (Twist Zone 5, 0.0 to 1.0)
 * @param feedbackL Pointer to left channel feedback state (updated in place)
 * @param feedbackR Pointer to right channel feedback state (updated in place)
 * @param params Pointer to SineShaperParams for coefficient smoothing (required)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void sineShapeBuffer(StereoBuffer<q31_t> buffer, q31_t drive, q31_t* smoothedDrive, q31_t* dcBlockerL,
                            q31_t* dcBlockerR, q31_t harmonic, q31_t symmetry, q31_t mix, float stereoWidth,
                            float stereoFreqMult, float stereoPhaseOffset, float evenAmount, float rectAmount,
                            float feedbackAmount, q31_t* feedbackL, q31_t* feedbackR, SineShaperParams* params,
                            bool* wasBypassed = nullptr) {
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
	bool useChebyshevCrossfade = (smoothedHarmonic >= kZone1Threshold);
	bool inZone1 = (smoothedHarmonic >= kZone1Threshold && smoothedHarmonic < kZone2Threshold);
	bool inZone2 = (smoothedHarmonic >= kZone2Threshold);

	// Hoist Zone 1/2 weight calculation with per-sample coefficient interpolation
	// Per-sample smoothing eliminates zipper noise from triangle weight changes
	// Smooth L and R directly (simpler than L + delta, avoids per-sample multiplies)
	Zone1Weights zone1WeightsL;
	Zone1Weights zone1WeightsR;
	FloatSmoothingContext c1Ctx{0, 0, 0}, c3LCtx{0, 0, 0}, c5LCtx{0, 0, 0}, c7LCtx{0, 0, 0}, c9LCtx{0, 0, 0};
	FloatSmoothingContext c3RCtx{0, 0, 0}, c5RCtx{0, 0, 0}, c7RCtx{0, 0, 0}, c9RCtx{0, 0, 0};
	bool usePerSampleWeights = (inZone1 || inZone2);

	if (usePerSampleWeights) {
		int32_t zoneIndex = inZone1 ? 1 : 2;
		float posInZone = computePosInZone(smoothedHarmonic, zoneIndex);

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

	// Zone 0 stereo: different symmetry (asymmetry) for L/R creates stereo via even harmonics
	// Uses same animation as Zone 1 (Wide) for consistent stereo behavior
	constexpr q31_t kZone0StereoScale = ONE_Q31 / 4; // 25% of full asymmetry range
	q31_t symmetryL = symmetry;
	q31_t symmetryR = symmetry;

	if (!useChebyshevCrossfade) {
		// Zone 0 stereo: animated symmetry offset creates different even harmonic content
		constexpr float kTwoPi = 6.283185f;
		float modPhase = stereoPhaseOffset * stereoFreqMult * kTwoPi;
		float stereoMod = std::sin(modPhase); // -1 to +1 animation
		q31_t stereoSymOffset = static_cast<q31_t>(stereoWidth * stereoMod * static_cast<float>(kZone0StereoScale));
		symmetryL = symmetry - stereoSymOffset;
		symmetryR = symmetry + stereoSymOffset;
	}

	// Local copy of state for efficient per-sample update
	q31_t dcStateL = *dcBlockerL;
	q31_t dcStateR = *dcBlockerR;
	q31_t fbStateL = feedbackL ? *feedbackL : 0;
	q31_t fbStateR = feedbackR ? *feedbackR : 0;
	q31_t fbLpfStateL = params->feedbackLpfL;
	q31_t fbLpfStateR = params->feedbackLpfR;
	q31_t currentDrive = driveCtx.current;

	// Scale feedback amount for moderate self-oscillation range
	// At max (1.0), feedback is ~0.9 of output (just below self-oscillation)
	constexpr float kFeedbackScale = 0.9f;
	q31_t fbAmount = static_cast<q31_t>(feedbackAmount * kFeedbackScale * static_cast<float>(ONE_Q31));

	for (auto& sample : buffer) {
		// Apply feedback to input (before shaping)
		q31_t inputL = sample.l;
		q31_t inputR = sample.r;
		if (fbAmount > 0) {
			// Hard clip feedback to prevent runaway (same as oscillator feedback)
			q31_t clippedFbL = signed_saturate<22>(multiply_32x32_rshift32(fbStateL, fbAmount) << 1);
			q31_t clippedFbR = signed_saturate<22>(multiply_32x32_rshift32(fbStateR, fbAmount) << 1);
			inputL = add_saturate(sample.l, clippedFbL);
			inputR = add_saturate(sample.r, clippedFbR);
		}

		// Process left channel with L symmetry
		q31_t shapedL = sineShapeCore(inputL, currentDrive, smoothedHarmonic, symmetryL,
		                              usePerSampleWeights ? &zone1WeightsL : nullptr, evenAmount, rectAmount);

		// Process right channel with R symmetry
		q31_t shapedR = sineShapeCore(inputR, currentDrive, smoothedHarmonic, symmetryR,
		                              usePerSampleWeights ? &zone1WeightsR : nullptr, evenAmount, rectAmount);
		currentDrive += driveCtx.increment;

		// Update weights for next sample (per-sample interpolation)
		if (usePerSampleWeights) {
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

	*dcBlockerL = dcStateL;
	*dcBlockerR = dcStateR;
	params->feedbackLpfL = fbLpfStateL;
	params->feedbackLpfR = fbLpfStateR;
	if (usePerSampleWeights) {
		// Write back smoothed weight state for next buffer
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
	if (feedbackL) {
		*feedbackL = fbStateL;
	}
	if (feedbackR) {
		*feedbackR = fbStateR;
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

/**
 * Process a mono buffer through the Saturator waveshaper
 *
 * @param buffer Audio buffer to process in-place
 * @param saturator The Saturator instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDrive Previous drive value for smoothing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param prevX ADAA state (previous input sample), nullptr if AA disabled
 */
inline void saturateBuffer(std::span<q31_t> buffer, Saturator& saturator, q31_t drive, q31_t* smoothedDrive, q31_t mix,
                           float* prevX = nullptr) {
	// Early out - if mix is 0, do nothing
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	auto ctx = prepareSmoothing(*smoothedDrive, drive, buffer.size());

	for (auto& sample : buffer) {
		ctx.current += ctx.increment;

		// Get saturated (wet) signal - prevX enables ADAA when non-null
		q31_t wet = saturator.process(sample, ctx.current, prevX);

		// Wet/dry crossfade
		q31_t dry = multiply_32x32_rshift32(sample, ONE_Q31 - mix) << 1;
		wet = multiply_32x32_rshift32(wet, mix) << 1;
		sample = add_saturate(dry, wet);
	}

	*smoothedDrive = ctx.target;
}

/**
 * Process a stereo buffer through the Saturator waveshaper
 *
 * @param buffer Stereo audio buffer to process in-place
 * @param saturator The Saturator instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDrive Previous drive value for smoothing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param prevXL Left channel ADAA state (previous input sample), nullptr if AA disabled
 * @param prevXR Right channel ADAA state (previous input sample), nullptr if AA disabled
 */
inline void saturateBuffer(StereoBuffer<q31_t> buffer, Saturator& saturator, q31_t drive, q31_t* smoothedDrive,
                           q31_t mix, float* prevXL = nullptr, float* prevXR = nullptr) {
	// Early out - if mix is 0, do nothing
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	auto ctx = prepareSmoothing(*smoothedDrive, drive, buffer.size());

	for (auto& sample : buffer) {
		ctx.current += ctx.increment;

		// Process left channel - prevXL enables ADAA when non-null
		q31_t wetL = saturator.process(sample.l, ctx.current, prevXL);

		// Process right channel - prevXR enables ADAA when non-null
		q31_t wetR = saturator.process(sample.r, ctx.current, prevXR);

		// Wet/dry crossfade
		q31_t dryL = multiply_32x32_rshift32(sample.l, ONE_Q31 - mix) << 1;
		q31_t dryR = multiply_32x32_rshift32(sample.r, ONE_Q31 - mix) << 1;
		wetL = multiply_32x32_rshift32(wetL, mix) << 1;
		wetR = multiply_32x32_rshift32(wetR, mix) << 1;

		sample.l = add_saturate(dryL, wetL);
		sample.r = add_saturate(dryR, wetR);
	}

	*smoothedDrive = ctx.target;
}

} // namespace deluge::dsp
