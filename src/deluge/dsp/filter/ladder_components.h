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

#include "util/fixedpoint.h"
#include <arm_neon.h>
#include <cstdint>
namespace deluge::dsp::filter {
class BasicFilterComponent {
public:
	BasicFilterComponent() { reset(); }
	// moveability is tan(f)/(1+tan(f))
	[[gnu::always_inline]] q31_t doFilter(q31_t input, q31_t moveability) {
		q31_t a = q31_mult_rounded(input - memory, moveability);
		q31_t b = a + memory;
		memory = b + a;
		return b;
	}
	[[gnu::always_inline]] int32_t doAPF(q31_t input, int32_t moveability) {
		q31_t a = q31_mult_rounded(input - memory, moveability);
		q31_t b = a + memory;
		memory = a + b;
		return b * 2 - input;
	}
	[[gnu::always_inline]] void affectFilter(q31_t input, int32_t moveability) {
		memory += multiply_32x32_rshift32_rounded(input - memory, moveability) << 2;
	}
	[[gnu::always_inline]] void reset() { memory = 0; }
	[[gnu::always_inline]] q31_t getFeedbackOutput(int32_t feedbackAmount) {
		return multiply_32x32_rshift32_rounded(memory, feedbackAmount) << 2;
	}
	[[gnu::always_inline]] q31_t getFeedbackOutputWithoutLshift(int32_t feedbackAmount) {
		return multiply_32x32_rshift32_rounded(memory, feedbackAmount);
	}

	q31_t memory = 0;
};

/// Stereo filter component using NEON for parallel L/R processing
/// Processes both channels simultaneously with int32x2_t vectors
/// Uses scalar storage to avoid NEON instructions at static initialization
class StereoFilterComponent {
public:
	StereoFilterComponent() = default;

	/// NEON-vectorized allpass filter for stereo (L/R in parallel)
	/// @param input Stereo input as int32x2_t (lane 0 = L, lane 1 = R)
	/// @param moveability Filter coefficient (same for both channels)
	/// @return Stereo allpass output
	[[gnu::always_inline]] int32x2_t doAPF(int32x2_t input, int32_t moveability) {
		// Load memory from scalar storage (NEON at runtime, not static init)
		int32x2_t memory = vld1_s32(memory_);
		int32x2_t coeff = vdup_n_s32(moveability);

		// a = ((input - memory) * moveability) >> 31, with rounding
		int32x2_t diff = vsub_s32(input, memory);
		// vqrdmulh gives (a*b*2) >> 32 with rounding, equivalent to q31_mult_rounded
		int32x2_t a = vqrdmulh_s32(diff, coeff);

		// b = a + memory
		int32x2_t b = vadd_s32(a, memory);

		// memory = a + b
		memory = vadd_s32(a, b);

		// Store memory back to scalar storage
		vst1_s32(memory_, memory);

		// return b * 2 - input
		int32x2_t b2 = vshl_n_s32(b, 1);
		return vsub_s32(b2, input);
	}

	/// NEON-vectorized lowpass filter for stereo (L/R in parallel)
	/// Used by LR2Crossover for Butterworth cascade
	/// @param input Stereo input as int32x2_t (lane 0 = L, lane 1 = R)
	/// @param moveability Filter coefficient (same for both channels)
	/// @return Stereo lowpass output
	[[gnu::always_inline]] int32x2_t doFilter(int32x2_t input, int32_t moveability) {
		// Load memory from scalar storage
		int32x2_t memory = vld1_s32(memory_);
		int32x2_t coeff = vdup_n_s32(moveability);

		// a = ((input - memory) * moveability) >> 31, with rounding
		int32x2_t diff = vsub_s32(input, memory);
		int32x2_t a = vqrdmulh_s32(diff, coeff);

		// b = a + memory
		int32x2_t b = vadd_s32(a, memory);

		// memory = b + a
		memory = vadd_s32(b, a);

		// Store memory back to scalar storage
		vst1_s32(memory_, memory);

		return b;
	}

	[[gnu::always_inline]] void reset() {
		memory_[0] = 0;
		memory_[1] = 0;
	}

	// Scalar storage aligned for NEON load/store - safe for static initialization
	alignas(8) int32_t memory_[2]{};
};

/// Quad filter component using NEON for processing 4 channels in parallel
/// Useful for: 4 mono channels, 2 stereo pairs, or batch processing
/// Uses scalar storage to avoid NEON instructions at static initialization
class QuadFilterComponent {
public:
	QuadFilterComponent() = default;

	/// NEON-vectorized allpass filter for 4 channels in parallel
	/// @param input 4 input samples as int32x4_t
	/// @param moveability Filter coefficient (same for all channels)
	/// @return 4 allpass outputs
	[[gnu::always_inline]] int32x4_t doAPF(int32x4_t input, int32_t moveability) {
		// Load memory from scalar storage
		int32x4_t memory = vld1q_s32(memory_);
		int32x4_t coeff = vdupq_n_s32(moveability);
		int32x4_t diff = vsubq_s32(input, memory);
		int32x4_t a = vqrdmulhq_s32(diff, coeff);
		int32x4_t b = vaddq_s32(a, memory);
		memory = vaddq_s32(a, b);
		// Store memory back to scalar storage
		vst1q_s32(memory_, memory);
		int32x4_t b2 = vshlq_n_s32(b, 1);
		return vsubq_s32(b2, input);
	}

	/// NEON-vectorized lowpass filter for 4 channels in parallel
	/// @param input 4 input samples as int32x4_t
	/// @param moveability Filter coefficient (same for all channels)
	/// @return 4 lowpass outputs
	[[gnu::always_inline]] int32x4_t doFilter(int32x4_t input, int32_t moveability) {
		// Load memory from scalar storage
		int32x4_t memory = vld1q_s32(memory_);
		int32x4_t coeff = vdupq_n_s32(moveability);
		int32x4_t diff = vsubq_s32(input, memory);
		int32x4_t a = vqrdmulhq_s32(diff, coeff);
		int32x4_t b = vaddq_s32(a, memory);
		memory = vaddq_s32(b, a);
		// Store memory back to scalar storage
		vst1q_s32(memory_, memory);
		return b;
	}

	[[gnu::always_inline]] void reset() {
		memory_[0] = 0;
		memory_[1] = 0;
		memory_[2] = 0;
		memory_[3] = 0;
	}

	// Scalar storage aligned for NEON load/store - safe for static initialization
	alignas(16) int32_t memory_[4]{};
};

} // namespace deluge::dsp::filter
