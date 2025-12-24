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

	q31_t x2 = 2 * multiply_32x32_rshift32(x, x);
	q31_t x3 = 2 * multiply_32x32_rshift32(x2, x);
	// this is 4(3*x/4 - x^3) which is a nice shape
	q31_t r1 = 8 * (multiply_32x32_rshift32(THREE_FOURTHS, x) - x3);

	q31_t r2 = 2 * multiply_32x32_rshift32(r1, r1);
	q31_t r3 = 2 * multiply_32x32_rshift32(r2, r1);
	// at this point we've applied the polynomial twice
	q31_t out = 8 * (multiply_32x32_rshift32(THREE_FOURTHS, r1) - r3);

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
// Includes a 12kHz lowpass filter on the wet signal to reduce aliasing artifacts.

// 12kHz lowpass filter coefficients at 44.1kHz: alpha = 1 - exp(-2*pi*12000/44100) ≈ 0.82
// Using both alpha and (1-alpha) to avoid overflow in filter calculation
constexpr q31_t kSineShaperLpfAlpha = static_cast<q31_t>(0.82 * ONE_Q31);
constexpr q31_t kSineShaperLpfOneMinusAlpha = ONE_Q31 - kSineShaperLpfAlpha;

/**
 * Core sine shaping without wet/dry mix (returns wet signal only)
 */
inline q31_t sineShapeCore(q31_t input, q31_t drive, q31_t harmonic, q31_t symmetry) {
	// Apply symmetry (DC offset for even harmonics)
	q31_t biased = add_saturate(input, symmetry >> 3);

	// Use drive similar to how fold uses fold_level:
	// Base level ensures some effect even at drive=0
	// Add drive on top for more saturation
	q31_t driveLevel = add_saturate(FOLD_MIN, drive);

	// Scale input for the polynomial - same pattern as foldBufferPolyApproximation
	q31_t scaledInput = lshiftAndSaturateUnknown(multiply_32x32_rshift32(driveLevel, biased), 8);

	// Apply the polynomial waveshaper and scale output back down
	q31_t shaped = polynomialOscillatorApproximation(scaledInput) >> 7;

	// Add extra harmonics via cascaded waveshaping
	if (harmonic > 0) {
		// Second pass through polynomial adds more harmonics
		q31_t moreHarmonics = polynomialOscillatorApproximation(shaped << 7) >> 7;
		// Blend between single-pass and double-pass based on harmonic param
		shaped = shaped + multiply_32x32_rshift32(moreHarmonics - shaped, harmonic) * 2;
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

	// Wet/dry crossfade
	q31_t invMix = ONE_Q31 - mix;
	q31_t dryPart = multiply_32x32_rshift32(input, invMix) << 1;
	q31_t wetPart = multiply_32x32_rshift32(shaped, mix) << 1;

	return add_saturate(dryPart, wetPart);
}

/**
 * Process a mono buffer through the sine shaper with parameter smoothing and lowpass filter
 * @param buffer Audio buffer to process in place
 * @param drive Current target drive value
 * @param smoothedDrive Pointer to smoothed drive state (updated in place)
 * @param filterState Pointer to lowpass filter state (updated in place)
 * @param harmonic Blend between fundamental and 3rd harmonic
 * @param symmetry DC bias for asymmetry (even harmonics)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void sineShapeBuffer(std::span<q31_t> buffer, q31_t drive, q31_t* smoothedDrive, q31_t* filterState,
                            q31_t harmonic, q31_t symmetry, q31_t mix, bool* wasBypassed = nullptr) {
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
	q31_t targetSmoothed = *smoothedDrive + multiply_32x32_rshift32(drive - *smoothedDrive, smoothingAlpha) * 2;
	int32_t driveIncrement = (targetSmoothed - *smoothedDrive) / static_cast<int32_t>(buffer.size());
	q31_t currentDrive = *smoothedDrive;

	q31_t lpfState = *filterState;

	// Seed filter when transitioning from bypass to active, or on first use
	bool needsSeed = (wasBypassed && *wasBypassed) || lpfState == 0;
	if (needsSeed) {
		lpfState = sineShapeCore(buffer[0], currentDrive, harmonic, symmetry);
		if (wasBypassed) {
			*wasBypassed = false;
		}
	}

	for (auto& sample : buffer) {
		currentDrive += driveIncrement;

		// Get shaped (wet) signal
		q31_t shaped = sineShapeCore(sample, currentDrive, harmonic, symmetry);

		// Apply 12kHz lowpass to wet signal to reduce aliasing
		// Using y = (1-a)*y + a*x form to avoid overflow from (x-y) subtraction
		lpfState = add_saturate(multiply_32x32_rshift32(lpfState, kSineShaperLpfOneMinusAlpha) << 1,
		                        multiply_32x32_rshift32(shaped, kSineShaperLpfAlpha) << 1);

		// Wet/dry crossfade
		q31_t invMix = ONE_Q31 - mix;
		q31_t dryPart = multiply_32x32_rshift32(sample, invMix) << 1;
		q31_t wetPart = multiply_32x32_rshift32(lpfState, mix) << 1;

		sample = add_saturate(dryPart, wetPart);
	}

	// Update states for next buffer
	*smoothedDrive = targetSmoothed;
	*filterState = lpfState;
}

/**
 * Process a stereo buffer through the sine shaper with parameter smoothing and lowpass filter
 * @param buffer Stereo audio buffer to process in place
 * @param drive Current target drive value
 * @param smoothedDrive Pointer to smoothed drive state (updated in place)
 * @param filterL Pointer to left channel lowpass filter state
 * @param filterR Pointer to right channel lowpass filter state
 * @param harmonic Blend between fundamental and 3rd harmonic
 * @param symmetry DC bias for asymmetry (even harmonics)
 * @param mix Wet/dry blend - if 0, buffer is not modified (CPU optimization)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void sineShapeBuffer(StereoBuffer<q31_t> buffer, q31_t drive, q31_t* smoothedDrive, q31_t* filterL,
                            q31_t* filterR, q31_t harmonic, q31_t symmetry, q31_t mix, bool* wasBypassed = nullptr) {
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
	q31_t targetSmoothed = *smoothedDrive + multiply_32x32_rshift32(drive - *smoothedDrive, smoothingAlpha) * 2;
	int32_t driveIncrement = (targetSmoothed - *smoothedDrive) / static_cast<int32_t>(buffer.size());
	q31_t currentDrive = *smoothedDrive;

	q31_t lpfL = *filterL;
	q31_t lpfR = *filterR;

	// Seed filter when transitioning from bypass to active, or on first use
	bool needsSeed = (wasBypassed && *wasBypassed) || (lpfL == 0 && lpfR == 0);
	if (needsSeed) {
		lpfL = sineShapeCore(buffer[0].l, currentDrive, harmonic, symmetry);
		lpfR = sineShapeCore(buffer[0].r, currentDrive, harmonic, symmetry);
		if (wasBypassed) {
			*wasBypassed = false;
		}
	}

	for (auto& sample : buffer) {
		currentDrive += driveIncrement;

		// Process left channel
		q31_t shapedL = sineShapeCore(sample.l, currentDrive, harmonic, symmetry);
		// Using y = (1-a)*y + a*x form to avoid overflow from (x-y) subtraction
		lpfL = add_saturate(multiply_32x32_rshift32(lpfL, kSineShaperLpfOneMinusAlpha) << 1,
		                    multiply_32x32_rshift32(shapedL, kSineShaperLpfAlpha) << 1);

		// Process right channel
		q31_t shapedR = sineShapeCore(sample.r, currentDrive, harmonic, symmetry);
		lpfR = add_saturate(multiply_32x32_rshift32(lpfR, kSineShaperLpfOneMinusAlpha) << 1,
		                    multiply_32x32_rshift32(shapedR, kSineShaperLpfAlpha) << 1);

		// Wet/dry crossfade
		q31_t invMix = ONE_Q31 - mix;
		sample.l =
		    add_saturate(multiply_32x32_rshift32(sample.l, invMix) << 1, multiply_32x32_rshift32(lpfL, mix) << 1);
		sample.r =
		    add_saturate(multiply_32x32_rshift32(sample.r, invMix) << 1, multiply_32x32_rshift32(lpfR, mix) << 1);
	}

	// Update states for next buffer
	*smoothedDrive = targetSmoothed;
	*filterL = lpfL;
	*filterR = lpfR;
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
 * @param filterState LPF state for anti-aliasing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void saturateBuffer(std::span<q31_t> buffer, Saturator& saturator, q31_t drive, q31_t* smoothedDrive,
                           q31_t* filterState, q31_t mix, bool* wasBypassed = nullptr) {
	// Early out - if mix is 0, do nothing
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Exponential smoothing coefficient for drive (~100ms smoothing at 44.1kHz)
	constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);

	// Calculate per-sample drive increment for smooth modulation
	q31_t targetSmoothed = *smoothedDrive + multiply_32x32_rshift32(drive - *smoothedDrive, smoothingAlpha) * 2;
	int32_t driveIncrement = (targetSmoothed - *smoothedDrive) / static_cast<int32_t>(buffer.size());
	q31_t currentDrive = *smoothedDrive;

	q31_t lpfState = *filterState;

	// Seed filter when transitioning from bypass to active, or on first use
	bool needsSeed = (wasBypassed && *wasBypassed) || lpfState == 0;
	if (needsSeed && !buffer.empty()) {
		lpfState = saturator.process(buffer[0], currentDrive);
		if (wasBypassed) {
			*wasBypassed = false;
		}
	}

	// 12kHz lowpass filter coefficients (alpha = 0.82, 1-alpha = 0.18)
	constexpr q31_t lpfOneMinusAlpha = static_cast<q31_t>(0.18 * ONE_Q31);
	constexpr q31_t lpfAlpha = static_cast<q31_t>(0.82 * ONE_Q31);

	for (auto& sample : buffer) {
		currentDrive += driveIncrement;

		// Get saturated (wet) signal
		q31_t wet = saturator.process(sample, currentDrive);

		// Apply lowpass to reduce aliasing (~12kHz at 44.1kHz)
		// Using y = (1-a)*y + a*x form to avoid overflow from (x-y) subtraction
		lpfState = add_saturate(multiply_32x32_rshift32(lpfState, lpfOneMinusAlpha) << 1,
		                        multiply_32x32_rshift32(wet, lpfAlpha) << 1);
		wet = lpfState;

		// Wet/dry crossfade
		q31_t dry = multiply_32x32_rshift32(sample, ONE_Q31 - mix) << 1;
		wet = multiply_32x32_rshift32(wet, mix) << 1;
		sample = add_saturate(dry, wet);
	}

	*smoothedDrive = targetSmoothed;
	*filterState = lpfState;
}

/**
 * Process a stereo buffer through the Saturator waveshaper
 *
 * @param buffer Stereo audio buffer to process in-place
 * @param saturator The Saturator instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDrive Previous drive value for smoothing (updated)
 * @param filterL Left channel LPF state (updated)
 * @param filterR Right channel LPF state (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param wasBypassed Pointer to bypass state flag (updated in place) - set to true initially
 */
inline void saturateBuffer(StereoBuffer<q31_t> buffer, Saturator& saturator, q31_t drive, q31_t* smoothedDrive,
                           q31_t* filterL, q31_t* filterR, q31_t mix, bool* wasBypassed = nullptr) {
	// Early out - if mix is 0, do nothing
	if (mix <= 0 || buffer.empty()) {
		if (wasBypassed) {
			*wasBypassed = true;
		}
		return;
	}

	// Exponential smoothing coefficient for drive (~100ms smoothing at 44.1kHz)
	constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);

	// Calculate per-sample drive increment
	q31_t targetSmoothed = *smoothedDrive + multiply_32x32_rshift32(drive - *smoothedDrive, smoothingAlpha) * 2;
	int32_t driveIncrement = (targetSmoothed - *smoothedDrive) / static_cast<int32_t>(buffer.size());
	q31_t currentDrive = *smoothedDrive;

	q31_t lpfL = *filterL;
	q31_t lpfR = *filterR;

	// Seed filter when transitioning from bypass to active, or on first use
	bool needsSeed = (wasBypassed && *wasBypassed) || (lpfL == 0 && lpfR == 0);
	if (needsSeed && !buffer.empty()) {
		lpfL = saturator.process(buffer[0].l, currentDrive);
		lpfR = saturator.process(buffer[0].r, currentDrive);
		if (wasBypassed) {
			*wasBypassed = false;
		}
	}

	// 12kHz lowpass filter coefficients (alpha = 0.82, 1-alpha = 0.18)
	// Using y = (1-a)*y + a*x form to avoid overflow from (x-y) subtraction
	constexpr q31_t lpfOneMinusAlpha = static_cast<q31_t>(0.18 * ONE_Q31);
	constexpr q31_t lpfAlpha = static_cast<q31_t>(0.82 * ONE_Q31);

	for (auto& sample : buffer) {
		currentDrive += driveIncrement;

		// Process left channel
		q31_t wetL = saturator.process(sample.l, currentDrive);
		lpfL = add_saturate(multiply_32x32_rshift32(lpfL, lpfOneMinusAlpha) << 1,
		                    multiply_32x32_rshift32(wetL, lpfAlpha) << 1);
		wetL = lpfL;

		// Process right channel
		q31_t wetR = saturator.process(sample.r, currentDrive);
		lpfR = add_saturate(multiply_32x32_rshift32(lpfR, lpfOneMinusAlpha) << 1,
		                    multiply_32x32_rshift32(wetR, lpfAlpha) << 1);
		wetR = lpfR;

		// Wet/dry crossfade
		q31_t dryL = multiply_32x32_rshift32(sample.l, ONE_Q31 - mix) << 1;
		q31_t dryR = multiply_32x32_rshift32(sample.r, ONE_Q31 - mix) << 1;
		wetL = multiply_32x32_rshift32(wetL, mix) << 1;
		wetR = multiply_32x32_rshift32(wetR, mix) << 1;

		sample.l = add_saturate(dryL, wetL);
		sample.r = add_saturate(dryR, wetR);
	}

	*smoothedDrive = targetSmoothed;
	*filterL = lpfL;
	*filterR = lpfR;
}

} // namespace deluge::dsp
