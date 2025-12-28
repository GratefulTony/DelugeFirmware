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
 * This approximates wavefolding by taking an input between -1 and 1
 * and producing output that flips around zero several times
 */
inline q31_t polynomialOscillatorApproximation(q31_t x) {
	// requires 1 to be ONE_Q31
	// Uses rounded multiply to avoid asymmetric truncation bias
	// (truncation rounds positive toward 0, negative toward -inf, causing DC offset)

	q31_t x2 = 2 * multiply_32x32_rshift32_rounded(x, x);
	q31_t x3 = 2 * multiply_32x32_rshift32_rounded(x2, x);
	// this is 4(3*x/4 - x^3) which is a nice shape
	q31_t r1 = 8 * (multiply_32x32_rshift32_rounded(THREE_FOURTHS, x) - x3);

	q31_t r2 = 2 * multiply_32x32_rshift32_rounded(r1, r1);
	q31_t r3 = 2 * multiply_32x32_rshift32_rounded(r2, r1);
	// at this point we've applied the polynomial twice
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
/// Note: harmonic uses UNPATCHED_SINE_SHAPER_HARMONIC, twist uses UNPATCHED_SINE_SHAPER_SYMMETRY
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
	// Weight caching for Zone 1 (hoisted computation)
	bool weightsNeedUpdate{true};                // Set true when harmonic changes
	q31_t cachedHarmonic{0};                     // Harmonic value weights were computed for
	float cachedW3{0}, cachedW5{0}, cachedW7{0}; // Cached normalized weights
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
	// phaseWidth for each harmonic (lower = narrower peaks, more dead zones)
	// Scale: 0x80000000 = 50% active, 0xFFFFFFFF = 100% active
	static constexpr uint32_t kDutyT3 = 0x80000000u; // T3 active 50%
	static constexpr uint32_t kDutyT5 = 0x80000000u; // T5 active 50%
	static constexpr uint32_t kDutyT7 = 0x66666666u; // T7 active 40%
	static constexpr uint32_t kDutyT9 = 0x5999999Au; // T9 active 35%

	// Irrational period ratios for non-repeating patterns
	static constexpr float kPeriodT3 = 2.718f;   // e
	static constexpr float kPeriodT5 = 3.14159f; // π
	static constexpr float kPeriodT7 = 2.236f;   // √5
	static constexpr float kPeriodT9 = 1.618f;   // φ (golden ratio)

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
		outT3 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT3 * freqMult + kPhaseT3), kDutyT3);
		outT5 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT5 * freqMult + kPhaseT5), kDutyT5);
		outT7 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT7 * freqMult + kPhaseT7), kDutyT7);
		outT9 = triangleWithDeadzone(toPhase(chebyPos * kPeriodT9 * freqMult + kPhaseT9), kDutyT9);
	}
};

/**
 * Precomputed blended polynomial coefficients for Zone 1 "357"
 * Computed once per buffer using Horner's method for efficient per-sample evaluation
 *
 * The blended polynomial is: P(x) = c1*x + c3*x³ + c5*x⁵ + c7*x⁷
 * Using Horner's method: P(x) = x * (c1 + x² * (c3 + x² * (c5 + c7*x²)))
 *
 * This reduces Zone 1 from ~94 to ~30 cycles/sample (comparable to TanH+ADAA)
 */
struct Zone1Weights {
	float c1; // Coefficient for x (always 1.0 since weights are normalized)
	float c3; // Coefficient for x³ = -(4w3/3 + 4w5 + 8w7)
	float c5; // Coefficient for x⁵ = 3.2w5 + 16w7
	float c7; // Coefficient for x⁷ = -64w7/7
};

/**
 * Compute Zone 1 blended polynomial coefficients from harmonic parameter
 * Should be called once per block, not per sample
 *
 * Individual normalized Chebyshev polynomials (unity fundamental):
 *   H3 = x - (4/3)x³
 *   H5 = x - 4x³ + 3.2x⁵
 *   H7 = x - 8x³ + 16x⁵ - (64/7)x⁷
 *
 * Blended polynomial P(x) = w3*H3 + w5*H5 + w7*H7 expands to:
 *   P(x) = c1*x + c3*x³ + c5*x⁵ + c7*x⁷
 *
 * Where (with normalized weights summing to 1):
 *   c1 = 1.0 (always, since w3 + w5 + w7 = 1)
 *   c3 = -(4w3/3 + 4w5 + 8w7)
 *   c5 = 3.2w5 + 16w7
 *   c7 = -64w7/7
 *
 * @param posInZone Position 0.0 to 1.0 within the zone
 * @return Precomputed blended coefficients for Horner's method evaluation
 */
inline Zone1Weights computeZone1WeightsFromPos(float posInZone) {
	posInZone = std::clamp(posInZone, 0.0f, 1.0f);

	// Convert float phase to uint32_t
	// Must wrap to [0,1) before scaling to avoid UB from float->uint32 overflow
	constexpr float kPhaseScale = 4294967296.0f;
	auto toPhase = [](float f) {
		f = std::fmod(f, 1.0f);
		if (f < 0.0f) {
			f += 1.0f;
		}
		return static_cast<uint32_t>(f * kPhaseScale);
	};

	// phaseWidth: 50% active, 50% dead zone (more contrast between harmonics)
	constexpr uint32_t kDuty = 0x80000000u;

	// Irrational frequencies prevent dead zones from aligning (avoids all-zero weights)
	constexpr float kFreqW3 = 5.38516f; // √29  (~5.4 cycles)
	constexpr float kFreqW5 = 6.28318f; // 2π   (~6.3 cycles)
	constexpr float kFreqW7 = 7.38906f; // e²   (~7.4 cycles)

	// Get triangle weights as q31, convert to float for coefficient computation
	constexpr float kInvQ31 = 1.0f / static_cast<float>(ONE_Q31);
	float w3 = static_cast<float>(triangleWithDeadzone(toPhase(posInZone * kFreqW3 + 0.0f), kDuty)) * kInvQ31;
	float w5 = static_cast<float>(triangleWithDeadzone(toPhase(posInZone * kFreqW5 + 0.25f), kDuty)) * kInvQ31;
	float w7 = static_cast<float>(triangleWithDeadzone(toPhase(posInZone * kFreqW7 + 0.5f), kDuty)) * kInvQ31;

	// Minimum weight floor prevents discontinuity when all triangles are in dead zones
	constexpr float kMinWeight = 0.08f;
	w3 = std::max(w3, kMinWeight);
	w5 = std::max(w5, kMinWeight);
	w7 = std::max(w7, kMinWeight);

	// Normalize weights so they sum to 1
	float wSum = w3 + w5 + w7;
	w3 /= wSum;
	w5 /= wSum;
	w7 /= wSum;

	// Compute blended polynomial coefficients
	// c1 = w3 + w5 + w7 = 1.0 (by normalization)
	// c3 = -(4w3/3 + 4w5 + 8w7)
	// c5 = 3.2w5 + 16w7
	// c7 = -64w7/7
	float c1 = 1.0f;
	float c3 = -(w3 * (4.0f / 3.0f) + w5 * 4.0f + w7 * 8.0f);
	float c5 = w5 * 3.2f + w7 * 16.0f;
	float c7 = -w7 * (64.0f / 7.0f);

	return {c1, c3, c5, c7};
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
 * Compute Zone 2 "357s" stereo R-channel coefficient offsets
 * Uses triangle-based parameter phasing for L/R decorrelation
 *
 * @param baseWeights The L-channel weights (from computeZone1Weights)
 * @param posInZone Position within zone (0.0 to 1.0)
 * @param stereoWidth Stereo spread amount (0.0 to 1.0, from UI 0-127)
 * @return R-channel weights with phase-based offsets
 */
inline Zone1Weights computeZone2StereoOffsets(const Zone1Weights& baseWeights, float posInZone, float stereoWidth) {
	if (stereoWidth <= 0.001f) {
		return baseWeights; // No stereo spread, return identical weights
	}

	// Triangle-based offsets with irrational frequencies for L/R decorrelation
	// Low frequencies vary with posInZone (Harmonic knob position)
	constexpr float kFreqC3 = 3.14159f; // π
	constexpr float kFreqC5 = 2.23607f; // √5
	constexpr float kFreqC7 = 1.73205f; // √3

	// High frequencies vary with stereoWidth itself - adjacent width values
	// produce different offset patterns (like dott multiband vibe/feel interaction)
	constexpr float kWidthFreqC3 = 7.28318f; // 2π + small offset
	constexpr float kWidthFreqC5 = 11.0902f; // 5√5
	constexpr float kWidthFreqC7 = 8.66025f; // 5√3

	// Simple triangle: phase wraps, outputs -1 to +1
	auto triangle = [](float phase) {
		phase = phase - static_cast<int>(phase);
		if (phase < 0.0f) {
			phase += 1.0f;
		}
		return (phase < 0.5f) ? (phase * 4.0f - 1.0f) : (3.0f - phase * 4.0f);
	};

	// Compute offset amounts scaled by stereo width
	// Max offset ~30% of coefficient magnitude at full width (was 10%, too subtle)
	constexpr float kMaxOffset = 0.3f;
	float scale = stereoWidth * kMaxOffset;

	// Phase combines posInZone (slow) + stereoWidth (fast wiggle)
	float phaseC3 = posInZone * kFreqC3 + stereoWidth * kWidthFreqC3;
	float phaseC5 = posInZone * kFreqC5 + stereoWidth * kWidthFreqC5 + 0.33f;
	float phaseC7 = posInZone * kFreqC7 + stereoWidth * kWidthFreqC7 + 0.67f;

	float offsetC3 = triangle(phaseC3) * scale * std::abs(baseWeights.c3);
	float offsetC5 = triangle(phaseC5) * scale * std::abs(baseWeights.c5);
	float offsetC7 = triangle(phaseC7) * scale * std::abs(baseWeights.c7);

	return {baseWeights.c1, // c1 stays at 1.0
	        baseWeights.c3 + offsetC3, baseWeights.c5 + offsetC5, baseWeights.c7 + offsetC7};
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
 */
inline q31_t sineShapeCore(q31_t input, q31_t drive, q31_t harmonic, q31_t symmetry,
                           const Zone1Weights* zone1Weights = nullptr) {
	// Apply symmetry (DC offset for even harmonics)
	// >> 7 gives very subtle effect, builds gradually across zone
	q31_t biased = add_saturate(input, symmetry >> 7);

	// === Drive calculation for hybrid param (bipolar, additive modulation) ===
	// drive range: INT32_MIN to INT32_MAX (center=0 is unity)
	// Normalize to 0..1, then square to get volume-style curve
	// This gives: min→0, center→1, max→4 (same as old volume param behavior)
	float normalizedDrive = (static_cast<float>(drive) + 2147483648.0f) / 4294967296.0f; // 0 to 1
	float driveGain = normalizedDrive * normalizedDrive * 4.0f;                          // Square for volume curve

	// Apply drive to input
	float inputF = static_cast<float>(biased) * driveGain;

	// Clamp and convert back to q31
	constexpr float kMaxQ31 = 2147483647.0f;
	inputF = std::clamp(inputF, -kMaxQ31, kMaxQ31);
	q31_t driven = static_cast<q31_t>(inputF);

	// === Derive harmonic zone and weights ===
	int32_t zone;
	q31_t wT3, wT5, wT7, wT9;
	SineShaperHarmonicMapper::deriveWeights(harmonic, zone, wT3, wT5, wT7, wT9);

	q31_t shaped;

	if (zone == 0) {
		// === Zone 0: Polynomial waveshaping with cascade blend ===
		// Traditional wet/dry crossfade (affects both fundamental and harmonics)
		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 8);
		shaped = polynomialOscillatorApproximation(scaledInput) >> 7;

		// Cascaded polynomial adds more harmonics via second pass
		// Scale so blend reaches maximum at end of zone 0 (1/8 of range)
		q31_t cascadeBlend = (harmonic >= ONE_Q31 / 8) ? ONE_Q31 : (harmonic << 3);

		if (cascadeBlend > 0) {
			q31_t moreHarmonics = polynomialOscillatorApproximation(shaped << 7) >> 7;
			shaped = shaped + multiply_32x32_rshift32_rounded(moreHarmonics - shaped, cascadeBlend) * 2;
		}
	}
	else if (zone == 1) {
		// === Zone 1 "357": Blended Chebyshev T3, T5, T7 with Horner's method ===
		// Precomputed blended coefficients reduce per-sample cost to ~30 cycles
		// (comparable to TanH+ADAA instead of ~94 cycles computing 3 polynomials)

		// Scale up input with shift 9 for stronger harmonic saturation
		// Higher shift pushes more inputs into the polynomial's nonlinear region
		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 9);
		float x = static_cast<float>(scaledInput) / static_cast<float>(ONE_Q31);

		// Clamp x to [-1, 1] BEFORE polynomial evaluation
		// Chebyshev polynomials blow up outside this range
		x = std::clamp(x, -1.0f, 1.0f);

		// Use precomputed blended coefficients if available (hoisted from buffer loop)
		float c1, c3, c5, c7;
		if (zone1Weights) {
			c1 = zone1Weights->c1;
			c3 = zone1Weights->c3;
			c5 = zone1Weights->c5;
			c7 = zone1Weights->c7;
		}
		else {
			// Fallback: compute coefficients per sample (legacy single-sample path)
			constexpr float zoneStart = 1.0f / 8.0f;
			constexpr float zoneEnd = 2.0f / 8.0f;
			float paramNorm = static_cast<float>(harmonic) / static_cast<float>(ONE_Q31);
			float posInZone = (paramNorm - zoneStart) / (zoneEnd - zoneStart);
			posInZone = std::clamp(posInZone, 0.0f, 1.0f);

			auto triangle = [](float phase) {
				phase = phase - static_cast<int>(phase);
				return (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
			};

			float w3 = triangle(posInZone * 1.5f + 0.0f);
			float w5 = triangle(posInZone * 1.5f + 0.333f);
			float w7 = triangle(posInZone * 1.5f + 0.666f);

			float wSum = w3 + w5 + w7;
			if (wSum > 0.001f) {
				w3 /= wSum;
				w5 /= wSum;
				w7 /= wSum;
			}

			c1 = 1.0f;
			c3 = -(w3 * (4.0f / 3.0f) + w5 * 4.0f + w7 * 8.0f);
			c5 = w5 * 3.2f + w7 * 16.0f;
			c7 = -w7 * (64.0f / 7.0f);
		}

		// Evaluate blended polynomial using Horner's method:
		// P(x) = x * (c1 + x² * (c3 + x² * (c5 + c7*x²)))
		float x2 = x * x;
		float result = x * (c1 + x2 * (c3 + x2 * (c5 + c7 * x2)));

		// Output gain: >>6 here + >>1 later = >>7 total
		// +3dB boost vs zones 2-7 for more harmonic presence
		shaped = static_cast<q31_t>(result * static_cast<float>(ONE_Q31)) >> 6;
	}
	else {
		// === Zones 2-7: Normalized Chebyshev H3 waveshaping (placeholder) ===
		// Same gain staging as Zone 1 for proper harmonic extraction crossfade

		// Scale up input with shift 9 (same as Zone 1)
		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 9);
		float x = static_cast<float>(scaledInput) / static_cast<float>(ONE_Q31);

		// Clamp x to [-1, 1] BEFORE polynomial evaluation
		x = std::clamp(x, -1.0f, 1.0f);

		// Normalized H3 = x - (4/3)x³
		// Evaluated using Horner's method: x * (1 - (4/3)*x²)
		float x2 = x * x;
		float result = x * (1.0f - (4.0f / 3.0f) * x2);

		// Output gain: >>7 here + >>1 later = >>8 total (matches Zone 1)
		shaped = static_cast<q31_t>(result * static_cast<float>(ONE_Q31)) >> 7;
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
 * @param buffer Audio buffer to process in place
 * @param drive Current target drive value
 * @param smoothedDrive Pointer to smoothed drive state (updated in place)
 * @param dcBlocker Pointer to DC blocker state (updated in place)
 * @param harmonic Harmonic zone selection (0-1024 mapped to 8 zones)
 * @param smoothedHarmonic Pointer to smoothed harmonic state (updated in place)
 * @param symmetry DC bias for asymmetry (even harmonics)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void sineShapeBuffer(std::span<q31_t> buffer, q31_t drive, q31_t* smoothedDrive, q31_t* dcBlocker,
                            q31_t harmonic, q31_t* smoothedHarmonic, q31_t symmetry, q31_t mix,
                            bool* wasBypassed = nullptr) {
	// Early out - if mix is 0, do nothing (important CPU optimization)
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Buffer-rate smoothing for drive and harmonic
	q31_t smoothedDriveValue = smoothParam(smoothedDrive, drive);
	q31_t smoothedHarmonicValue = smoothParam(smoothedHarmonic, harmonic);

	// Mark as no longer bypassed if transitioning
	if (wasBypassed && *wasBypassed) {
		*wasBypassed = false;
	}

	// Determine crossfade mode based on smoothed harmonic
	constexpr q31_t kZone1Threshold = ONE_Q31 / 8;
	constexpr q31_t kZone2Threshold = ONE_Q31 / 4;
	bool useChebyshevCrossfade = (smoothedHarmonicValue >= kZone1Threshold);
	bool inZone1 = (smoothedHarmonicValue >= kZone1Threshold && smoothedHarmonicValue < kZone2Threshold);

	// Hoist Zone 1 weight calculation - computed once per buffer using smoothed harmonic
	const Zone1Weights* zone1WeightsPtr = nullptr;
	Zone1Weights zone1Weights;
	if (inZone1) {
		float posInZone = computePosInZone(smoothedHarmonicValue, 1);
		zone1Weights = computeZone1WeightsFromPos(posInZone);
		zone1WeightsPtr = &zone1Weights;
	}

	// Local copy of DC blocker state for efficient per-sample update
	q31_t dcState = *dcBlocker;

	for (auto& sample : buffer) {
		// Get shaped (wet) signal
		q31_t shaped = sineShapeCore(sample, smoothedDriveValue, smoothedHarmonicValue, symmetry, zone1WeightsPtr);

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
}

/**
 * Process a stereo buffer through the sine shaper with parameter smoothing
 *
 * Drive and harmonic are smoothed internally; symmetry and stereoWidth should
 * be computed from an already-smoothed Twist param at the call site.
 *
 * @param buffer Stereo audio buffer to process in place
 * @param drive Current target drive value
 * @param smoothedDrive Pointer to smoothed drive state (updated in place)
 * @param dcBlockerL Pointer to left channel DC blocker state (updated in place)
 * @param dcBlockerR Pointer to right channel DC blocker state (updated in place)
 * @param harmonic Harmonic zone selection (0-1024 mapped to 8 zones)
 * @param smoothedHarmonic Pointer to smoothed harmonic state (updated in place)
 * @param symmetry DC bias for asymmetry (computed from smoothed Twist)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param stereoWidth Stereo coefficient spread (0.0-1.0, 0 = mono, from smoothed Twist)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void sineShapeBuffer(StereoBuffer<q31_t> buffer, q31_t drive, q31_t* smoothedDrive, q31_t* dcBlockerL,
                            q31_t* dcBlockerR, q31_t harmonic, q31_t* smoothedHarmonic, q31_t symmetry, q31_t mix,
                            float stereoWidth, bool* wasBypassed = nullptr) {
	// Early out - if mix is 0, do nothing (important CPU optimization)
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Buffer-rate smoothing for drive and harmonic
	// Note: symmetry and stereoWidth are already derived from smoothed Twist at call site
	q31_t smoothedDriveValue = smoothParam(smoothedDrive, drive);
	q31_t smoothedHarmonicValue = smoothParam(smoothedHarmonic, harmonic);

	// Mark as no longer bypassed if transitioning
	if (wasBypassed && *wasBypassed) {
		*wasBypassed = false;
	}

	// Determine crossfade mode based on smoothed harmonic
	constexpr q31_t kZone1Threshold = ONE_Q31 / 8;
	constexpr q31_t kZone2Threshold = ONE_Q31 / 4;
	bool useChebyshevCrossfade = (smoothedHarmonicValue >= kZone1Threshold);
	bool inZone1 = (smoothedHarmonicValue >= kZone1Threshold && smoothedHarmonicValue < kZone2Threshold);

	// Hoist Zone 1 weight calculation - computed once per buffer using smoothed harmonic
	const Zone1Weights* zone1WeightsLPtr = nullptr;
	const Zone1Weights* zone1WeightsRPtr = nullptr;
	Zone1Weights zone1WeightsL;
	Zone1Weights zone1WeightsR;

	if (inZone1) {
		float posInZone = computePosInZone(smoothedHarmonicValue, 1);
		zone1WeightsL = computeZone1WeightsFromPos(posInZone);
		zone1WeightsLPtr = &zone1WeightsL;

		// Apply stereo offsets (stereoWidth is 0.0-1.0 from smoothed Twist)
		if (stereoWidth > 0.001f) {
			zone1WeightsR = computeZone2StereoOffsets(zone1WeightsL, posInZone, stereoWidth);
			zone1WeightsRPtr = &zone1WeightsR;
		}
		else {
			zone1WeightsRPtr = &zone1WeightsL; // Same for both channels
		}
	}

	// Local copy of DC blocker state for efficient per-sample update
	q31_t dcStateL = *dcBlockerL;
	q31_t dcStateR = *dcBlockerR;

	for (auto& sample : buffer) {
		// Process left channel with L weights
		q31_t shapedL = sineShapeCore(sample.l, smoothedDriveValue, smoothedHarmonicValue, symmetry, zone1WeightsLPtr);

		// Process right channel with R weights
		q31_t shapedR = sineShapeCore(sample.r, smoothedDriveValue, smoothedHarmonicValue, symmetry, zone1WeightsRPtr);

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
