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
#include "deluge/util/waves.h"
#include "dsp/saturator.h"
#include "dsp_ng/core/types.hpp"
#include <cmath>
#include <span>

namespace deluge::dsp {
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
	if (level <= 0 && *smoothedLevel <= 0) {
		return;
	}
	if (buffer.empty()) {
		return;
	}

	// Exponential smoothing (~100ms at 44.1kHz with 128 sample buffers)
	constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);

	q31_t targetSmoothed = *smoothedLevel + multiply_32x32_rshift32(level - *smoothedLevel, smoothingAlpha) * 2;
	int32_t levelIncrement = (targetSmoothed - *smoothedLevel) / static_cast<int32_t>(buffer.size());
	q31_t currentLevel = *smoothedLevel;

	for (auto& sample : buffer) {
		currentLevel += levelIncrement;
		if (currentLevel > 0) {
			q31_t fold_level = add_saturate(currentLevel, FOLD_MIN);
			q31_t x = lshiftAndSaturateUnknown(multiply_32x32_rshift32(fold_level, sample), 8);
			sample = polynomialOscillatorApproximation(x) >> 7;
		}
	}

	*smoothedLevel = targetSmoothed;
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

/// Sine shaper parameters and DSP state for one sound instance
/// Note: harmonic param uses UNPATCHED_SINE_SHAPER_HARMONIC (1024 resolution)
struct SineShaperParams {
	// User-facing parameters (0-127, converted to q31_t for DSP)
	uint8_t drive{0};     // Input gain / saturation amount
	uint8_t symmetry{64}; // DC bias for asymmetry (64 = center/symmetric)
	uint8_t mix{0};       // Wet/dry blend (0 = bypass)
	// DSP smoothing state
	q31_t smoothedDrive{0};    // Previous drive value for parameter smoothing
	q31_t smoothedHarmonic{0}; // Previous harmonic value for parameter smoothing
	q31_t filterL{0};          // Lowpass state for left channel (unused, kept for API compat)
	q31_t filterR{0};          // Lowpass state for right channel (unused, kept for API compat)
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
	// Duty cycles for each harmonic (lower = more gaps = more compute savings)
	static constexpr float kDutyT3 = 0.5f;  // T3 active 50% of time
	static constexpr float kDutyT5 = 0.5f;  // T5 active 50% of time
	static constexpr float kDutyT7 = 0.4f;  // T7 active 40% of time (higher = rarer)
	static constexpr float kDutyT9 = 0.35f; // T9 active 35% of time (highest = rarest)

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

	/// Triangle wave with duty cycle (creates dead zones)
	/// @param phase Oscillator phase (will be wrapped to 0-1)
	/// @param duty Active portion of cycle (0-1), rest is dead zone at 0
	static float triangle(float phase, float duty) {
		// Wrap phase to 0-1
		phase = phase - static_cast<int32_t>(phase);
		if (phase < 0.0f) {
			phase += 1.0f;
		}

		// Bipolar triangle: -1 at phase 0, +1 at phase 0.5, -1 at phase 1.0
		float bipolar = (phase < 0.5f) ? (-1.0f + phase * 4.0f) : (3.0f - phase * 4.0f);

		// Offset based on duty cycle: duty 0.5 → offset 0, duty 1.0 → offset +1
		float offset = duty * 2.0f - 1.0f;
		float shifted = bipolar + offset;

		// Clamp negatives to 0 (creates dead zones)
		if (shifted < 0.0f) {
			return 0.0f;
		}

		// Renormalize so peak is 1.0
		float peak = 1.0f + offset;
		return (peak > 0.001f) ? (shifted / peak) : 0.0f;
	}

	/// Derive harmonic weights from zone parameter
	/// @param harmonicParam Raw harmonic parameter (0 to ONE_Q31)
	/// @param outZone Output zone number (0-7)
	/// @param outT3 Output T3 weight (0-1, or q31)
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

		// Compute weights with triangle oscillators
		float t3w = triangle(chebyPos * kPeriodT3 * freqMult + kPhaseT3, kDutyT3);
		float t5w = triangle(chebyPos * kPeriodT5 * freqMult + kPhaseT5, kDutyT5);
		float t7w = triangle(chebyPos * kPeriodT7 * freqMult + kPhaseT7, kDutyT7);
		float t9w = triangle(chebyPos * kPeriodT9 * freqMult + kPhaseT9, kDutyT9);

		// Convert to q31
		outT3 = static_cast<q31_t>(t3w * ONE_Q31);
		outT5 = static_cast<q31_t>(t5w * ONE_Q31);
		outT7 = static_cast<q31_t>(t7w * ONE_Q31);
		outT9 = static_cast<q31_t>(t9w * ONE_Q31);
	}
};

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
 */
inline q31_t sineShapeCore(q31_t input, q31_t drive, q31_t harmonic, q31_t symmetry) {
	// Apply symmetry (DC offset for even harmonics)
	q31_t biased = add_saturate(input, symmetry >> 3);

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
		// === Zone 1 "357": Blended T3, T5, T7 Chebyshev polynomials ===
		// Position within zone controls blend via phased triangles

		// Scale up input
		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 9);
		float x = static_cast<float>(scaledInput) / static_cast<float>(ONE_Q31);

		// Precompute powers
		float x2 = x * x;
		float x3 = x2 * x;
		float x5 = x3 * x2;
		float x7 = x5 * x2;

		// Chebyshev polynomials (odd harmonics only)
		// T3(x) = 4x³ - 3x
		// T5(x) = 16x⁵ - 20x³ + 5x
		// T7(x) = 64x⁷ - 112x⁵ + 56x³ - 7x
		float T3 = 4.0f * x3 - 3.0f * x;
		float T5 = 16.0f * x5 - 20.0f * x3 + 5.0f * x;
		float T7 = 64.0f * x7 - 112.0f * x5 + 56.0f * x3 - 7.0f * x;

		// Calculate position within zone 1 (0.0 to 1.0)
		// Zone 1 spans harmonic values from 1/8 to 2/8 of full range
		constexpr float zoneStart = 1.0f / 8.0f;
		constexpr float zoneEnd = 2.0f / 8.0f;
		float paramNorm = static_cast<float>(harmonic) / static_cast<float>(ONE_Q31);
		float posInZone = (paramNorm - zoneStart) / (zoneEnd - zoneStart);
		posInZone = std::clamp(posInZone, 0.0f, 1.0f);

		// Phased triangle weights (120° apart for smooth cycling)
		// Each harmonic fades in and out as you sweep through the zone
		auto triangle = [](float phase) {
			phase = phase - static_cast<int>(phase); // wrap to 0-1
			return (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
		};

		float w3 = triangle(posInZone * 1.5f + 0.0f);   // T3 weight
		float w5 = triangle(posInZone * 1.5f + 0.333f); // T5 weight (120° offset)
		float w7 = triangle(posInZone * 1.5f + 0.666f); // T7 weight (240° offset)

		// Normalize weights so they sum to 1
		float wSum = w3 + w5 + w7;
		if (wSum > 0.001f) {
			w3 /= wSum;
			w5 /= wSum;
			w7 /= wSum;
		}

		// Blend the polynomials
		float result = w3 * T3 + w5 * T5 + w7 * T7;
		result = std::clamp(result, -1.0f, 1.0f);
		shaped = static_cast<q31_t>(result * static_cast<float>(ONE_Q31)) >> 8;
	}
	else {
		// === Zones 2-7: Chebyshev T3 waveshaping (placeholder) ===
		q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32_rounded(ONE_Q31, driven), 9);
		float x = static_cast<float>(scaledInput) / static_cast<float>(ONE_Q31);

		float x3 = x * x * x;
		float T3 = 4.0f * x3 - 3.0f * x;

		T3 = std::clamp(T3, -1.0f, 1.0f);
		shaped = static_cast<q31_t>(T3 * static_cast<float>(ONE_Q31)) >> 8;
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

	// Wet/dry crossfade
	q31_t invMix = ONE_Q31 - mix;
	q31_t dryPart = multiply_32x32_rshift32(input, invMix) << 1;
	q31_t wetPart = multiply_32x32_rshift32(shaped, mix) << 1;

	return add_saturate(dryPart, wetPart);
}

/**
 * Process a mono buffer through the sine shaper with parameter smoothing
 * @param buffer Audio buffer to process in place
 * @param drive Current target drive value
 * @param smoothedDrive Pointer to smoothed drive state (updated in place)
 * @param filterState Unused (kept for API compatibility)
 * @param harmonic Harmonic zone selection (0-1024 mapped to 8 zones)
 * @param smoothedHarmonic Pointer to smoothed harmonic state (updated in place)
 * @param symmetry DC bias for asymmetry (even harmonics)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void sineShapeBuffer(std::span<q31_t> buffer, q31_t drive, q31_t* smoothedDrive, q31_t* filterState,
                            q31_t harmonic, q31_t* smoothedHarmonic, q31_t symmetry, q31_t mix,
                            bool* wasBypassed = nullptr) {
	// Early out - if mix is 0, do nothing (important CPU optimization)
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Exponential smoothing coefficient for drive - lower = smoother
	// At 44.1kHz with 128 sample buffers (~344 buffers/sec), 0.03 gives ~100ms smoothing
	constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);

	// Calculate per-sample drive increment
	q31_t targetDriveSmoothed = *smoothedDrive + multiply_32x32_rshift32(drive - *smoothedDrive, smoothingAlpha) * 2;
	int32_t driveIncrement = (targetDriveSmoothed - *smoothedDrive) / static_cast<int32_t>(buffer.size());
	q31_t currentDrive = *smoothedDrive;

	// Calculate per-sample harmonic increment (same smoothing for zone transitions)
	q31_t targetHarmonicSmoothed =
	    *smoothedHarmonic + multiply_32x32_rshift32(harmonic - *smoothedHarmonic, smoothingAlpha) * 2;
	int32_t harmonicIncrement = (targetHarmonicSmoothed - *smoothedHarmonic) / static_cast<int32_t>(buffer.size());
	q31_t currentHarmonic = *smoothedHarmonic;

	// Mark as no longer bypassed if transitioning
	if (wasBypassed && *wasBypassed) {
		*wasBypassed = false;
	}

	for (auto& sample : buffer) {
		currentDrive += driveIncrement;
		currentHarmonic += harmonicIncrement;

		// Get shaped (wet) signal
		q31_t shaped = sineShapeCore(sample, currentDrive, currentHarmonic, symmetry);

		// Wet/dry crossfade (no lowpass filtering - filter state was per-clip, not per-voice)
		q31_t invMix = ONE_Q31 - mix;
		q31_t dryPart = multiply_32x32_rshift32(sample, invMix) << 1;
		q31_t wetPart = multiply_32x32_rshift32(shaped, mix) << 1;

		sample = add_saturate(dryPart, wetPart);
	}

	// Update state for next buffer
	*smoothedDrive = targetDriveSmoothed;
	*smoothedHarmonic = targetHarmonicSmoothed;
}

/**
 * Process a stereo buffer through the sine shaper with parameter smoothing
 * @param buffer Stereo audio buffer to process in place
 * @param drive Current target drive value
 * @param smoothedDrive Pointer to smoothed drive state (updated in place)
 * @param filterL Unused (kept for API compatibility)
 * @param filterR Unused (kept for API compatibility)
 * @param harmonic Harmonic zone selection (0-1024 mapped to 8 zones)
 * @param smoothedHarmonic Pointer to smoothed harmonic state (updated in place)
 * @param symmetry DC bias for asymmetry (even harmonics)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void sineShapeBuffer(StereoBuffer<q31_t> buffer, q31_t drive, q31_t* smoothedDrive, q31_t* filterL,
                            q31_t* filterR, q31_t harmonic, q31_t* smoothedHarmonic, q31_t symmetry, q31_t mix,
                            bool* wasBypassed = nullptr) {
	// Early out - if mix is 0, do nothing (important CPU optimization)
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Exponential smoothing coefficient for drive (~100ms smoothing at 44.1kHz)
	constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);

	// Calculate per-sample drive increment
	q31_t targetDriveSmoothed = *smoothedDrive + multiply_32x32_rshift32(drive - *smoothedDrive, smoothingAlpha) * 2;
	int32_t driveIncrement = (targetDriveSmoothed - *smoothedDrive) / static_cast<int32_t>(buffer.size());
	q31_t currentDrive = *smoothedDrive;

	// Calculate per-sample harmonic increment (same smoothing for zone transitions)
	q31_t targetHarmonicSmoothed =
	    *smoothedHarmonic + multiply_32x32_rshift32(harmonic - *smoothedHarmonic, smoothingAlpha) * 2;
	int32_t harmonicIncrement = (targetHarmonicSmoothed - *smoothedHarmonic) / static_cast<int32_t>(buffer.size());
	q31_t currentHarmonic = *smoothedHarmonic;

	// Mark as no longer bypassed if transitioning
	if (wasBypassed && *wasBypassed) {
		*wasBypassed = false;
	}

	for (auto& sample : buffer) {
		currentDrive += driveIncrement;
		currentHarmonic += harmonicIncrement;

		// Process left channel
		q31_t shapedL = sineShapeCore(sample.l, currentDrive, currentHarmonic, symmetry);

		// Process right channel
		q31_t shapedR = sineShapeCore(sample.r, currentDrive, currentHarmonic, symmetry);

		// Wet/dry crossfade (no lowpass filtering - filter state was per-clip, not per-voice)
		q31_t invMix = ONE_Q31 - mix;
		sample.l =
		    add_saturate(multiply_32x32_rshift32(sample.l, invMix) << 1, multiply_32x32_rshift32(shapedL, mix) << 1);
		sample.r =
		    add_saturate(multiply_32x32_rshift32(sample.r, invMix) << 1, multiply_32x32_rshift32(shapedR, mix) << 1);
	}

	// Update state for next buffer
	*smoothedDrive = targetDriveSmoothed;
	*smoothedHarmonic = targetHarmonicSmoothed;
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

	// Exponential smoothing coefficient for drive (~100ms smoothing at 44.1kHz)
	constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);

	// Calculate per-sample drive increment for smooth modulation
	q31_t targetSmoothed = *smoothedDrive + multiply_32x32_rshift32(drive - *smoothedDrive, smoothingAlpha) * 2;
	int32_t driveIncrement = (targetSmoothed - *smoothedDrive) / static_cast<int32_t>(buffer.size());
	q31_t currentDrive = *smoothedDrive;

	for (auto& sample : buffer) {
		currentDrive += driveIncrement;

		// Get saturated (wet) signal - prevX enables ADAA when non-null
		q31_t wet = saturator.process(sample, currentDrive, prevX);

		// Wet/dry crossfade
		q31_t dry = multiply_32x32_rshift32(sample, ONE_Q31 - mix) << 1;
		wet = multiply_32x32_rshift32(wet, mix) << 1;
		sample = add_saturate(dry, wet);
	}

	*smoothedDrive = targetSmoothed;
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

	// Exponential smoothing coefficient for drive (~100ms smoothing at 44.1kHz)
	constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);

	// Calculate per-sample drive increment
	q31_t targetSmoothed = *smoothedDrive + multiply_32x32_rshift32(drive - *smoothedDrive, smoothingAlpha) * 2;
	int32_t driveIncrement = (targetSmoothed - *smoothedDrive) / static_cast<int32_t>(buffer.size());
	q31_t currentDrive = *smoothedDrive;

	for (auto& sample : buffer) {
		currentDrive += driveIncrement;

		// Process left channel - prevXL enables ADAA when non-null
		q31_t wetL = saturator.process(sample.l, currentDrive, prevXL);

		// Process right channel - prevXR enables ADAA when non-null
		q31_t wetR = saturator.process(sample.r, currentDrive, prevXR);

		// Wet/dry crossfade
		q31_t dryL = multiply_32x32_rshift32(sample.l, ONE_Q31 - mix) << 1;
		q31_t dryR = multiply_32x32_rshift32(sample.r, ONE_Q31 - mix) << 1;
		wetL = multiply_32x32_rshift32(wetL, mix) << 1;
		wetR = multiply_32x32_rshift32(wetR, mix) << 1;

		sample.l = add_saturate(dryL, wetL);
		sample.r = add_saturate(dryR, wetR);
	}

	*smoothedDrive = targetSmoothed;
}

} // namespace deluge::dsp
