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

/// Per-sample IIR alpha for q31 parameter smoothing (~40ms time constant at 44.1kHz)
/// Matches float kPerSampleAlpha for consistent behavior across q31/float params
constexpr q31_t kPerSampleAlphaQ31 = static_cast<q31_t>(0.0005 * ONE_Q31);

/// Context for per-sample IIR parameter smoothing during buffer processing
/// Provides click-free parameter interpolation without linear ramp discontinuities
struct SmoothingContext {
	q31_t current; // Current smoothed value (IIR update each sample)
	q31_t alpha;   // Per-sample IIR coefficient
	q31_t target;  // Target value (write back to state after buffer)
};

/// Prepare parameter smoothing for per-sample IIR processing
/// @param state Current smoothed state value
/// @param target Target parameter value
/// @param bufferSize Number of samples in buffer (unused, kept for API compat)
/// @return SmoothingContext for use during buffer processing
inline SmoothingContext prepareSmoothing(q31_t state, q31_t target, [[maybe_unused]] size_t bufferSize) {
	// Per-sample IIR smoothing - no intermediate target, smoother convergence
	return {state, kPerSampleAlphaQ31, target};
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

/// Float version of SmoothingContext for per-sample IIR coefficient interpolation
struct FloatSmoothingContext {
	float current; // Current smoothed value (IIR update each sample)
	float alpha;   // Per-sample IIR coefficient
	float target;  // Target value (write back to state after buffer)
};

/// Per-sample IIR alpha for coefficient smoothing (~40ms time constant at 44.1kHz)
/// Faster response for snappier knob feel while maintaining smooth transitions
/// τ = -1 / (fs × ln(1-α)) → α = 1 - exp(-1/(τ × fs))
/// With τ = 0.04s: α ≈ 0.0005
constexpr float kPerSampleAlpha = 0.0005f;

/// Prepare float smoothing for per-sample IIR coefficient interpolation
/// @param state Current smoothed state value
/// @param target Target coefficient value
/// @param bufferSize Number of samples in buffer (unused, kept for API compat)
/// @return FloatSmoothingContext for use during buffer processing
inline FloatSmoothingContext prepareSmoothingFloat(float state, float target, [[maybe_unused]] size_t bufferSize) {
	// Per-sample IIR smoothing - no intermediate target, smoother convergence
	return {state, kPerSampleAlpha, target};
}

/// Float version of buffer-rate smoothing for coefficient smoothing
/// Uses slow rate to avoid zipper noise from triangle weight jumps
[[gnu::always_inline]] inline float smoothParamFloat(float* state, float target) {
	constexpr float kCoeffAlpha = 0.015f; // ~200ms to reach 95% at 128 samples/buffer
	*state = *state + (target - *state) * kCoeffAlpha;
	return *state;
}

// ============================================================================
// Float Triangle Waveforms
// ============================================================================
// Two families of triangle waves:
//
// 1. Simple triangle (2 segments per cycle): -1→+1→-1 or 0→1→0
//    - triangleSimple: phase 0=-1, phase 0.5=+1, phase 1=-1
//    - triangleSimpleUnipolar: phase 0=0, phase 0.5=1, phase 1=0
//
// 2. Sine-like triangle (4 segments per cycle, matches sine zero crossings):
//    - triangleFloat: 0→+1→0→-1→0 (starts at 0, peak at 0.25)

/// Simple unipolar triangle - 2 segments: 0→1→0 (peak at phase=0.5)
/// @param phase Phase in cycles (wraps automatically via floor)
/// @param duty Active portion 0.0-1.0 (default 1.0 = full triangle, no deadzone)
/// @return Output 0.0 to 1.0
inline float triangleSimpleUnipolar(float phase, float duty = 1.0f) {
	phase = phase - std::floor(phase); // Wrap to 0-1
	float halfDuty = duty * 0.5f;

	if (phase < halfDuty) {
		return phase / halfDuty; // Rising: 0→1
	}
	else if (phase < duty) {
		return (duty - phase) / halfDuty; // Falling: 1→0
	}
	return 0.0f; // Deadzone
}

/// Simple bipolar triangle - 2 segments: -1→+1→-1 (peak at phase=0.5)
/// This is the shape multiband compressor uses for OWLTT oscillations.
/// @param phase Phase in cycles (wraps automatically)
/// @param duty Active portion 0.0-1.0 (default 1.0 = full triangle, no deadzone)
/// @return Output -1.0 to +1.0
[[gnu::always_inline]] inline float triangleSimple(float phase, float duty = 1.0f) {
	return triangleSimpleUnipolar(phase, duty) * 2.0f - 1.0f;
}

/// Sine-like bipolar triangle - 4 segments: 0→+1→0→-1→0
/// Matches sine wave zero crossings: starts at 0, peak at 0.25, zero at 0.5, trough at 0.75
/// @param phase Phase in cycles (wraps automatically)
/// @param duty Active portion 0.0-1.0 (default 1.0 = full wave, no deadzone)
/// @return Output -1.0 to +1.0
inline float triangleFloat(float phase, float duty = 1.0f) {
	phase = phase - std::floor(phase);
	float quarterDuty = duty * 0.25f;
	float halfDuty = duty * 0.5f;

	if (phase < quarterDuty) {
		return phase / quarterDuty; // Rising positive: 0→+1
	}
	else if (phase < halfDuty) {
		return (halfDuty - phase) / quarterDuty; // Falling positive: +1→0
	}
	else if (phase < halfDuty + quarterDuty) {
		return -(phase - halfDuty) / quarterDuty; // Falling negative: 0→-1
	}
	else if (phase < duty) {
		return -(duty - phase) / quarterDuty; // Rising negative: -1→0
	}
	return 0.0f; // Deadzone
}

// ============================================================================
// Multi-Zone Knob Helpers
// ============================================================================
// Utilities for parameters that divide their range into discrete zones,
// each with distinct behavior. Used by sine shaper, multiband compressor,
// analytic saturator, etc.

/// Result of zone calculation - index and position within zone
struct ZoneInfo {
	int32_t index;  ///< Zone index (0 to numZones-1)
	float position; ///< Position within zone (0.0 to 1.0)
};

/// Normalize q31 parameter to 0.0-1.0 float
[[gnu::always_inline]] inline float normalizeQ31(q31_t value) {
	return static_cast<float>(value) / static_cast<float>(ONE_Q31);
}

/// Compute zone index and position from normalized parameter (0.0-1.0)
/// @param normalized Parameter value normalized to 0.0-1.0 range
/// @param numZones Number of zones (typically 8)
/// @return ZoneInfo with index (0 to numZones-1) and position (0.0-1.0)
[[gnu::always_inline]] inline ZoneInfo computeZone(float normalized, int32_t numZones) {
	float zoneFloat = normalized * static_cast<float>(numZones);
	int32_t index = std::min(numZones - 1, static_cast<int32_t>(zoneFloat));
	float position = zoneFloat - static_cast<float>(index);
	return {index, position};
}

/// Compute zone index and position directly from q31 parameter
/// @param param Parameter value in q31 format (0 to ONE_Q31)
/// @param numZones Number of zones (typically 8)
/// @return ZoneInfo with index (0 to numZones-1) and position (0.0-1.0)
[[gnu::always_inline]] inline ZoneInfo computeZoneQ31(q31_t param, int32_t numZones) {
	return computeZone(normalizeQ31(param), numZones);
}

/// Convert zone position (0.0-1.0) to display value (0-127)
[[gnu::always_inline]] inline int32_t zonePositionToDisplay(float position) {
	return static_cast<int32_t>(position * 127.0f);
}

/// Compute position within a specific zone from global normalized position
/// @param normalized Global parameter value (0.0-1.0)
/// @param zoneIndex Target zone index
/// @param numZones Total number of zones
/// @return Position within zone (0.0-1.0), clamped
[[gnu::always_inline]] inline float positionInZone(float normalized, int32_t zoneIndex, int32_t numZones) {
	float zoneSize = 1.0f / static_cast<float>(numZones);
	float zoneStart = static_cast<float>(zoneIndex) * zoneSize;
	return std::clamp((normalized - zoneStart) / zoneSize, 0.0f, 1.0f);
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
		// Per-sample IIR update
		ctx.current += multiply_32x32_rshift32(ctx.target - ctx.current, ctx.alpha) * 2;
		if (ctx.current > 0) {
			q31_t fold_level = add_saturate(ctx.current, FOLD_MIN);
			q31_t x = lshiftAndSaturateUnknown(multiply_32x32_rshift32(fold_level, sample), 8);
			sample = polynomialOscillatorApproximation(x) >> 7;
		}
	}

	*smoothedLevel = ctx.current; // Write back final smoothed value
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

} // namespace deluge::dsp
