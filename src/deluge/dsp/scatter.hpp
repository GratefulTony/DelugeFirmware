/*
 * Copyright © 2024-2025 Owlet Records
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
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "dsp/phi_triangle.hpp"
#include <array>
#include <cstdint>

namespace deluge::dsp::scatter {

/**
 * Pre-configured phi triangle bank for scatter effects
 *
 * Bank indices:
 *   [0] sliceSelect  - Which slice to play (for shuffle/rearrange)
 *   [1] gateModulate - Gate duty cycle modulation
 *   [2] reverseProb  - Probability of reversing a slice
 *   [3] pitchVar     - Pitch/speed variance
 *
 * Phase offsets spread by 0.25 for decorrelation.
 * Uses φ^n frequencies for quasi-periodic evolution.
 */
constexpr std::array<phi::PhiTriConfig, 4> kScatterBank = {{
    {phi::kPhi100, 1.00f, 0.00f, false}, // [0] slice selection
    {phi::kPhi150, 1.00f, 0.25f, false}, // [1] gate modulation
    {phi::kPhi175, 0.80f, 0.50f, false}, // [2] reverse probability
    {phi::kPhi200, 1.00f, 0.75f, false}, // [3] pitch/speed variance
}};

/**
 * Map triangle value [0,1] to slice index within a zone
 * @param tri Triangle value in [0,1]
 * @param numSlices Total number of slices in the zone
 * @return Slice index [0, numSlices-1]
 */
[[gnu::always_inline]] inline int32_t sliceIndexFromTriangle(float tri, int32_t numSlices) {
	if (numSlices <= 1) {
		return 0;
	}
	int32_t idx = static_cast<int32_t>(tri * static_cast<float>(numSlices));
	return std::clamp(idx, int32_t{0}, numSlices - 1);
}

/**
 * Map triangle value [0,1] to gate duty cycle
 * Never fully mutes - minimum gate is 12.5%
 * @param tri Triangle value in [0,1]
 * @return Gate ratio [0.125, 1.0]
 */
[[gnu::always_inline]] inline float gateFromTriangle(float tri) {
	return 0.125f + tri * 0.875f;
}

/**
 * Map triangle value [0,1] to reverse decision
 * @param tri Triangle value in [0,1]
 * @param threshold Probability threshold for reversing
 * @return true if slice should be reversed
 */
[[gnu::always_inline]] inline bool reverseFromTriangle(float tri, float threshold) {
	return tri > threshold;
}

/**
 * Map triangle value [0,1] to pitch ratio
 * Center is 1.0 (no pitch change), range depends on depth
 * @param tri Triangle value in [0,1]
 * @param semitoneRange Maximum semitone deviation (e.g., 12 for octave)
 * @return Pitch ratio
 */
[[gnu::always_inline]] inline float pitchFromTriangle(float tri, float semitoneRange) {
	// Map [0,1] to [-semitoneRange, +semitoneRange] semitones
	float semitones = (tri * 2.0f - 1.0f) * semitoneRange;
	// Convert semitones to pitch ratio: 2^(semitones/12)
	// Use fast approximation: exp2f(semitones / 12.0f)
	return std::exp2(semitones / 12.0f);
}

/**
 * Map triangle value [0,1] to tape speed ratio
 * @param tri Triangle value in [0,1]
 * @param minSpeed Minimum speed (e.g., 0.5 for half speed)
 * @param maxSpeed Maximum speed (e.g., 2.0 for double speed)
 * @return Speed ratio
 */
[[gnu::always_inline]] inline float tapeSpeedFromTriangle(float tri, float minSpeed = 0.5f, float maxSpeed = 2.0f) {
	return minSpeed + tri * (maxSpeed - minSpeed);
}

/**
 * Map triangle value [0,1] to filter frequency
 * Uses exponential mapping for perceptually linear sweep
 * @param tri Triangle value in [0,1]
 * @param minFreq Minimum frequency in Hz
 * @param maxFreq Maximum frequency in Hz
 * @return Frequency in Hz
 */
[[gnu::always_inline]] inline float filterFreqFromTriangle(float tri, float minFreq = 200.0f, float maxFreq = 8000.0f) {
	// Exponential mapping for perceptually linear sweep
	float logMin = std::log2(minFreq);
	float logMax = std::log2(maxFreq);
	return std::exp2(logMin + tri * (logMax - logMin));
}

/**
 * Compute grain envelope multiplier using parabolic approximation of Hanning
 * Very cheap: 3 multiplies, 1 subtract, no trig
 *
 * @param positionInSlice Current position within slice [0, sliceLength)
 * @param sliceLength Total length of slice in samples
 * @param gateRatio Gate duty cycle [0,1] - audio plays during this portion
 * @param depth Envelope depth [0,1] - 0=hard cut, 1=full smooth envelope
 * @return Amplitude multiplier [0,1]
 */
[[gnu::always_inline]] inline float grainEnvelope(int32_t positionInSlice, int32_t sliceLength, float gateRatio,
                                                  float depth) {
	if (sliceLength <= 0) {
		return 1.0f;
	}

	// Normalized position within slice [0,1]
	float pos = static_cast<float>(positionInSlice) / static_cast<float>(sliceLength);

	// If past gate threshold, output is silent
	if (pos > gateRatio) {
		return 0.0f;
	}

	// Rescale position to [0,1] within the gated portion
	float gatedPos = pos / gateRatio;

	// Parabolic envelope: peaks at 0.5, zero at 0 and 1
	// This approximates Hanning window closely
	float envelope = gatedPos * (1.0f - gatedPos) * 4.0f;

	// Blend between hard gate (1.0) and envelope based on depth
	// depth=0: return 1.0 (hard gate, no fade)
	// depth=1: return envelope (full smooth grain)
	return 1.0f + depth * (envelope - 1.0f);
}

/**
 * Scatter control parameters from menu/modulation
 * Values are normalized q31 param values from UnpatchedParamSet
 */
struct ScatterParams {
	int32_t zoneA{0}; ///< Zone A control (meaning depends on mode)
	int32_t zoneB{0}; ///< Zone B control (meaning depends on mode)
	int32_t depth{0}; ///< Effect depth/intensity
	int32_t gate{0};  ///< Gate duty cycle

	/// Convert param value [-2^31, 2^31) to normalized [0,1]
	[[gnu::always_inline]] static float paramToNormalized(int32_t param) {
		// Convert signed 32-bit to [0,1] range
		// param + 2^31 maps to [0, 2^32), divide by 2^32 to get [0,1)
		return (static_cast<float>(param) + 2147483648.0f) / 4294967296.0f;
	}

	/// Convert param value [-2^31, 2^31) to bipolar [-1,1]
	[[gnu::always_inline]] static float paramToBipolar(int32_t param) {
		return static_cast<float>(param) / 2147483647.0f;
	}

	/// Get zone A as normalized [0,1]
	[[gnu::always_inline]] float zoneANormalized() const { return paramToNormalized(zoneA); }
	/// Get zone B as normalized [0,1]
	[[gnu::always_inline]] float zoneBNormalized() const { return paramToNormalized(zoneB); }
	/// Get depth as normalized [0,1]
	[[gnu::always_inline]] float depthNormalized() const { return paramToNormalized(depth); }
	/// Get gate as normalized [0,1]
	[[gnu::always_inline]] float gateNormalized() const { return paramToNormalized(gate); }
};

/**
 * State for scatter DSP processing
 */
struct ScatterState {
	double phiPhase{0.0};       ///< Phase accumulator for phi triangle evolution
	int32_t currentSlice{0};    ///< Current slice index in playback sequence
	int32_t targetSlice{0};     ///< Target slice (remapped) for scatter playback
	int32_t positionInSlice{0}; ///< Current sample position within current slice
	int32_t sliceLength{0};     ///< Length of each slice in samples
	int32_t numSlices{8};       ///< Number of slices to divide buffer into
	int32_t bufferLength{0};    ///< Total buffer length for dynamic slice updates
	float tapeSpeed{1.0f};      ///< Current tape speed for Tape mode
	float pitchRatio{1.0f};     ///< Current pitch ratio for Pitch mode
	float filterFreq{4000.0f};  ///< Current filter frequency for Filter mode
	int32_t repeatCount{0};     ///< Current repeat count for Repeat mode
	bool sliceReversed{false};  ///< Whether current slice is playing reversed
	float gatePosition{0.0f};   ///< Current position within gate cycle
	float gateRatio{1.0f};      ///< Current gate duty cycle
	bool initialized{false};    ///< Whether scatter state has been initialized for current buffer

	/// Reset state for new stutter session
	void reset() {
		phiPhase = 0.0;
		currentSlice = 0;
		targetSlice = 0;
		positionInSlice = 0;
		sliceLength = 0;
		numSlices = 8;
		bufferLength = 0;
		tapeSpeed = 1.0f;
		pitchRatio = 1.0f;
		filterFreq = 4000.0f;
		repeatCount = 0;
		sliceReversed = false;
		gatePosition = 0.0f;
		gateRatio = 1.0f;
		initialized = false;
	}

	/// Initialize slice parameters based on buffer size
	void initSlices(int32_t bufferSize, int32_t sliceCount) {
		numSlices = std::clamp(sliceCount, int32_t{2}, int32_t{16});
		sliceLength = bufferSize / numSlices;
		positionInSlice = 0;
		currentSlice = 0;
		targetSlice = 0;
		bufferLength = bufferSize;
		initialized = true;
	}

	/// Update slice count dynamically during playback
	/// Preserves relative position within buffer when slice count changes
	/// @param newSliceCount Desired number of slices
	/// @return true if slice count changed
	bool updateSliceCount(int32_t newSliceCount) {
		newSliceCount = std::clamp(newSliceCount, int32_t{2}, int32_t{16});
		if (newSliceCount == numSlices || bufferLength <= 0) {
			return false;
		}

		// Calculate current absolute position in buffer
		int32_t absPos = targetSlice * sliceLength + positionInSlice;

		// Update slice parameters
		numSlices = newSliceCount;
		sliceLength = bufferLength / numSlices;

		// Recalculate slice and position from absolute position
		if (sliceLength > 0) {
			targetSlice = (absPos / sliceLength) % numSlices;
			positionInSlice = absPos % sliceLength;
			currentSlice = targetSlice;
		}
		return true;
	}

	/// Advance position within slice, returns true if slice boundary crossed
	bool advancePosition() {
		positionInSlice++;
		if (positionInSlice >= sliceLength) {
			positionInSlice = 0;
			currentSlice = (currentSlice + 1) % numSlices;
			return true; // Slice boundary crossed
		}
		return false;
	}

	/// Get buffer offset for current scatter position (targetSlice + positionInSlice)
	[[gnu::always_inline]] int32_t getBufferOffset() const { return targetSlice * sliceLength + positionInSlice; }

	/// Advance phi phase for quasi-periodic evolution
	void advancePhase(float rate = 0.001f) { phiPhase += rate; }
};

} // namespace deluge::dsp::scatter
