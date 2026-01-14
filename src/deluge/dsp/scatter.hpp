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
 * Phi triangle bank for structural scatter params (Zone A meta)
 *
 * Bank indices:
 *   [0] sliceOffset - Offset added to slice selection
 *   [1] lengthMult  - Slice length multiplier
 *   [2] skipProb    - Probability of skipping to non-adjacent slice
 *
 * Slower φ^n frequencies for gradual structural evolution.
 * Phase offsets spread for decorrelation.
 */
constexpr std::array<phi::PhiTriConfig, 3> kStructuralBank = {{
    {phi::kPhi100, 0.80f, 0.00f, false}, // [0] sliceOffset
    {phi::kPhi150, 0.60f, 0.33f, false}, // [1] lengthMult
    {phi::kPhi175, 0.70f, 0.67f, false}, // [2] skipProb
}};

/**
 * Phi triangle bank for timbral scatter params (Zone B meta)
 *
 * Bank indices:
 *   [0] reverseProb - Probability of reversing slice
 *   [1] filterFreq  - Bandpass center frequency
 *   [2] delayFeed   - Per-grain delay send amount
 *   [3] envShape    - Envelope shape (percussive to reversed)
 *
 * Mix of slower and faster φ^n for varied timbral movement.
 * Phase offsets spread by 0.25 for decorrelation.
 */
constexpr std::array<phi::PhiTriConfig, 4> kTimbraBank = {{
    {phi::kPhiN050, 0.50f, 0.00f, false}, // [0] reverseProb (slow)
    {phi::kPhi067, 0.70f, 0.25f, false},  // [1] filterFreq
    {phi::kPhi125, 0.60f, 0.50f, false},  // [2] delayFeed
    {phi::kPhi200, 0.80f, 0.75f, false},  // [3] envShape (faster)
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

/**
 * Computed grain parameters from zone knobs
 * All values normalized [0,1] unless noted
 */
struct GrainParams {
	// Structural (from Zone A)
	float sliceOffset{0};   ///< Offset to add to slice selection [0,1] maps to [0,numSlices)
	float lengthMult{1.0f}; ///< Slice length multiplier [0.25, 2.0]
	float skipProb{0};      ///< Probability of skipping to non-adjacent slice

	// Timbral (from Zone B)
	float reverseProb{0};   ///< Probability of reversing slice
	float filterFreq{0.5f}; ///< Bandpass center [0,1] maps to freq range
	float delayFeed{0};     ///< Per-grain delay send amount
	float envShape{0.5f};   ///< Envelope shape (0=percussive, 0.5=hanning, 1=reverse)

	// Combined
	float gateRatio{1.0f}; ///< Gate duty cycle [0.125, 1.0]
};

/**
 * Compute grain parameters from zone knobs via phi triangles
 *
 * Zone A (Structural): Controls grain selection, length, skip patterns
 *   Zones 0-3: Individual behaviors with position controlling intensity
 *   Zones 4-7: Meta - all structural params via phi evolution (uses kStructuralBank)
 *
 * Zone B (Timbral): Controls per-grain effects
 *   Zones 0-3: Individual effects (reverse, filter, delay, envelope)
 *   Zones 4-7: Meta - all timbral params via phi evolution (uses kTimbraBank)
 *
 * @param zoneA Zone A normalized [0,1]
 * @param zoneB Zone B normalized [0,1]
 * @param depth Depth/intensity [0,1] - scales phi evolution rate
 * @param sliceIndex Current slice index (converted to phi-based phase internally)
 */
inline GrainParams computeGrainParams(float zoneA, float zoneB, float depth, int32_t sliceIndex) {
	GrainParams p;

	// Phi triangle deadzone: when triangle output is low, sliceIndex contribution is zeroed
	// This creates sparse activation - many consecutive slices get identical params → cache hits
	// Uses slow φ^-0.5 frequency so deadzone spans multiple slices
	float sliceWeight = triangleSimpleUnipolar(phi::wrapPhase(static_cast<float>(sliceIndex) * phi::kPhiN050), 0.5f);

	// Quantize: only use sliceIndex when outside deadzone (weight > threshold)
	int32_t effectiveSlice = (sliceWeight > 0.1f) ? sliceIndex : 0;

	// Convert effective slice to phi-based phase for quasi-random distribution
	float slicePhase = phi::wrapPhase(static_cast<float>(effectiveSlice) * phi::kPhi);

	// Phase evolution scaled by depth (depth=0: static, depth=1: full evolution)
	float gammaPhase = slicePhase * depth;

	// PhiTriContext: yNorm from slice phase, freqMult=1, periodScale=1, gammaPhase
	phi::PhiTriContext ctx{slicePhase, 1.0f, 1.0f, gammaPhase};

	// === Zone A: Structural ===
	constexpr float kZoneBoundary = 0.5f; // Zones 0-3 vs 4-7

	if (zoneA < kZoneBoundary) {
		// Zones 0-3: Discrete behaviors
		float pos = zoneA * 2.0f; // Remap to [0,1] within lower half
		int32_t zone = static_cast<int32_t>(pos * 4.0f);
		float inZone = (pos * 4.0f) - static_cast<float>(zone);

		switch (zone) {
		case 0: // Sequential with drift
			p.sliceOffset = inZone * 0.25f;
			break;
		case 1: // Swap adjacent pairs
			p.sliceOffset = (inZone > 0.5f) ? 0.5f : 0.0f;
			p.skipProb = inZone * 0.5f;
			break;
		case 2: // Reverse order tendency
			p.sliceOffset = inZone * 0.5f;
			p.lengthMult = 1.0f - inZone * 0.5f;
			break;
		case 3: // Interleave
		default:
			p.sliceOffset = inZone * 0.5f;
			p.skipProb = inZone;
			break;
		}
	}
	else {
		// Zones 4-7: Meta - phi triangle evolution via bank
		float pos = (zoneA - kZoneBoundary) * 2.0f; // Intensity within meta zone

		auto structural = ctx.evalBank(kStructuralBank, pos);
		p.sliceOffset = structural[0];
		p.lengthMult = 0.5f + structural[1] * 0.5f; // Map [0,1] to [0.5, 1.0]
		p.skipProb = structural[2] * 0.8f;          // Cap at 80%
	}

	// === Zone B: Timbral ===
	if (zoneB < kZoneBoundary) {
		// Zones 0-3: Individual effects
		float pos = zoneB * 2.0f;
		int32_t zone = static_cast<int32_t>(pos * 4.0f);
		float inZone = (pos * 4.0f) - static_cast<float>(zone);

		switch (zone) {
		case 0: // Reverse probability
			p.reverseProb = inZone;
			break;
		case 1: // Bandpass sweep
			p.filterFreq = inZone;
			break;
		case 2: // Delay feed
			p.delayFeed = inZone * 0.8f;
			break;
		case 3: // Envelope shape
		default:
			p.envShape = inZone;
			break;
		}
	}
	else {
		// Zones 4-7: Meta - phi triangle evolution via bank
		float pos = (zoneB - kZoneBoundary) * 2.0f; // Intensity within meta zone

		auto timbral = ctx.evalBank(kTimbraBank, pos);
		p.reverseProb = timbral[0];
		p.filterFreq = timbral[1];
		p.delayFeed = timbral[2] * 0.8f; // Cap at 80%
		p.envShape = timbral[3];
	}

	// Gate from depth (lower depth = more gating for rhythmic effect)
	p.gateRatio = 0.25f + (1.0f - depth * 0.5f) * 0.75f;

	return p;
}

} // namespace deluge::dsp::scatter
