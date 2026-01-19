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
#include "dsp/reverb/base.hpp"
#include "memory/general_memory_allocator.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace deluge::dsp::reverb {

/// Featherverb: A lightweight 4-tap FDN reverb with continuous matrix morphing
/// - Memory: ~48KB (vs Freeverb 93KB, Mutable 128KB)
/// - CPU: ~5-6K cycles (vs Mutable 13K, Freeverb 28K)
/// - Uses phi triangle bank for orthogonal matrix morphing via Gram-Schmidt
/// - Asymmetric delays: short D0-D2 for texture, long D3 for tail/scale
/// - Multi-tap output from D3 for increased reflection density
class Featherverb : public Base {
	static constexpr size_t kNumDelays = 4;
	static constexpr size_t kNumPlanes = 6; // Rotation planes in SO(4)

	// Asymmetric delays: D0-D1 fixed for early texture, D2-D3 variable for size
	// D0-D1 use phi-related ratios for non-resonant diffusion
	// D2 scales slowly (texture), D3 scales fast (tail) - both controlled by Zone 2
	// D3 is 50% larger than needed to provide headroom for multi-tap output
	// Total ~48KB at max size
	static constexpr size_t kD0Length = 397;     // ~9ms - fixed, small prime
	static constexpr size_t kD1Length = 643;     // ~14.6ms - fixed, φ ratio from D0
	static constexpr size_t kD2MinLength = 650;  // ~14.7ms - min, just above D1
	static constexpr size_t kD2MaxLength = 1039; // ~23.5ms - max, φ ratio from D1
	static constexpr size_t kD3MinLength = 1297; // ~29ms - min, tight room
	static constexpr size_t kD3MaxLength = 9830; // ~223ms - max, cathedral (50% larger for multi-tap density)

	// Fixed delays (D0-D1) + variable (D2-D3)
	static constexpr size_t kFixedSamples = kD0Length + kD1Length;                          // 1040
	static constexpr size_t kMaxTotalSamples = kFixedSamples + kD2MaxLength + kD3MaxLength; // 11909

	// Multi-tap output offsets into D3 for increased density (always < kD3MinLength)
	static constexpr size_t kTap1Offset = 311;                               // ~7ms - early reflection
	static constexpr size_t kTap2Offset = 643;                               // ~14.6ms - mid reflection (φ related)
	static constexpr size_t kBufferBytes = kMaxTotalSamples * sizeof(float); // ~48KB

	static constexpr std::array<size_t, kNumDelays> kDelayLengths = {kD0Length, kD1Length, kD2MaxLength, kD3MaxLength};

public:
	Featherverb();
	~Featherverb() override { deallocate(); }

	/// Allocate delay buffer from SDRAM
	[[nodiscard]] bool allocate();
	void deallocate();
	[[nodiscard]] bool isAllocated() const { return buffer_ != nullptr; }

	/// Process audio through FDN reverb
	void process(std::span<int32_t> input, StereoBuffer<q31_t> output) override;

	/// RoomSize maps to feedback amount (decay time)
	void setRoomSize(float value) override;
	[[nodiscard]] float getRoomSize() const override { return roomSize_; }

	/// Damping controls lowpass filter cutoff in feedback path
	void setDamping(float value) override;
	[[nodiscard]] float getDamping() const override { return damping_; }

	/// Width controls stereo spread of output
	void setWidth(float value) override;
	[[nodiscard]] float getWidth() const override { return width_; }

	/// High-pass filter on input (removes DC/rumble)
	void setHPF(float f) override;
	[[nodiscard]] float getHPF() const override { return hpCutoff_; }

	/// Low-pass filter on output
	void setLPF(float f) override;
	[[nodiscard]] float getLPF() const override { return lpCutoff_; }

	// === Featherverb-specific zone parameters ===

	/// Zone 1 - Matrix: Controls feedback matrix rotation through orthogonal space
	/// 8 zones select rotation planes, position within zone = rotation angle
	/// Uses Givens rotations for continuous morphing while preserving energy
	void setZone1(int32_t value);
	[[nodiscard]] int32_t getZone1() const { return zone1_; }

	/// Zone 2 - Size: Controls D2+D3 delay lengths for room size (small room → cathedral)
	/// D2 scales slowly (15-24ms), D3 scales fast (29-148ms) for proportional growth
	void setZone2(int32_t value);
	[[nodiscard]] int32_t getZone2() const { return zone2_; }

	/// Zone 3 - Feedback: Per-delay feedback multipliers for decay character
	/// 8 zones select different feedback patterns (balanced, front-heavy, tail-heavy, etc.)
	void setZone3(int32_t value);
	[[nodiscard]] int32_t getZone3() const { return zone3_; }

	/// Pre-delay: Delay before reverb onset (0-50ms)
	void setPredelay(float value) { predelay_ = value; }
	[[nodiscard]] float getPredelay() const { return predelay_; }

private:
	float* buffer_{nullptr};

	// Write positions for each delay line (circular buffer indices)
	std::array<size_t, kNumDelays> writePos_{};

	// Offsets into contiguous buffer for each delay line
	std::array<size_t, kNumDelays> delayOffsets_{};

	// Actual delay lengths (D2 and D3 are variable based on Zone 2)
	std::array<size_t, kNumDelays> actualDelayLengths_{kD0Length, kD1Length, kD2MaxLength, kD3MaxLength};

	// Lowpass filter states for damping in feedback path
	std::array<float, kNumDelays> lpState_{};

	// Parameters
	float roomSize_{0.5f}; // 0-1, maps to feedback
	float damping_{0.5f};  // 0-1, maps to LP coefficient
	float width_{1.0f};    // 0-1, stereo spread
	float hpCutoff_{0.0f}; // 0-1, input HPF
	float lpCutoff_{1.0f}; // 0-1, output LPF

	// Zone parameters
	int32_t zone1_{0};     // 0-1023, matrix rotation
	int32_t zone2_{512};   // 0-1023, D3 size (default mid)
	int32_t zone3_{0};     // 0-1023, feedback pattern
	float predelay_{0.0f}; // 0-1, pre-delay time

	// Derived coefficients
	float feedback_{0.85f};     // Derived from roomSize
	float dampCoeffNear_{0.5f}; // Damping for D0-D1 (brighter early reflections)
	float dampCoeffFar_{0.4f};  // Damping for D2-D3 (darker tail, scales with size)

	// Matrix rotation state (from Zone 1)
	// 4x4 orthonormal rotation matrix stored as 4 rows of 4 elements
	// Default: normalized Hadamard (0.5 scaling for unit columns)
	std::array<std::array<float, 4>, 4> matrix_{{
	    {0.5f, 0.5f, 0.5f, 0.5f},   // Hadamard row 0 (normalized)
	    {0.5f, -0.5f, 0.5f, -0.5f}, // Hadamard row 1 (normalized)
	    {0.5f, 0.5f, -0.5f, -0.5f}, // Hadamard row 2 (normalized)
	    {0.5f, -0.5f, -0.5f, 0.5f}  // Hadamard row 3 (normalized)
	}};

	// Per-delay feedback multipliers (from Zone 3)
	std::array<float, kNumDelays> feedbackMult_{1.0f, 1.0f, 1.0f, 1.0f};

	// Filter states
	float hpState_{0.0f};  // Input HPF state
	float lpStateL_{0.0f}; // Output LPF state (left)
	float lpStateR_{0.0f}; // Output LPF state (right)

	/// Update matrix_ based on zone1_ value using Givens rotations
	void updateMatrix();

	/// Update feedbackMult_ based on zone3_ value
	void updateFeedbackPattern();

	/// Update D2 and D3 lengths based on zone2_ value
	void updateDelayLengths();

	/// Read from delay line at current position (oldest sample = full delay)
	[[gnu::always_inline]] float read(size_t line) const { return buffer_[delayOffsets_[line] + writePos_[line]]; }

	/// Read from delay line at offset samples more recent than full delay
	/// offset=0 is same as read(), offset>0 gives shorter delay (more recent)
	[[gnu::always_inline]] float readAt(size_t line, size_t offset) const {
		size_t pos = (writePos_[line] + offset) % actualDelayLengths_[line];
		return buffer_[delayOffsets_[line] + pos];
	}

	/// Write to delay line and advance position
	[[gnu::always_inline]] void write(size_t line, float value) {
		buffer_[delayOffsets_[line] + writePos_[line]] = value;
		if (++writePos_[line] >= actualDelayLengths_[line]) {
			writePos_[line] = 0;
		}
	}

	/// One-pole lowpass filter
	[[gnu::always_inline]] static float onepole(float input, float& state, float coeff) {
		state += coeff * (input - state);
		return state;
	}
};

} // namespace deluge::dsp::reverb
