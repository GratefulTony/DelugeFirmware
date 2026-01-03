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

/// Stereo filter component - processes L/R channels in parallel using NEON
/// Note: Do NOT add alignas() - it causes static initialization crashes
struct StereoFilterComponent {
	int32_t memory_[2]{};

	void reset() {
		memory_[0] = 0;
		memory_[1] = 0;
	}

	/// NEON-vectorized allpass filter for stereo (L/R in parallel)
	[[gnu::always_inline]] int32x2_t doAPF(int32x2_t input, int32_t moveability) {
		int32x2_t memory = vld1_s32(memory_);
		int32x2_t coeff = vdup_n_s32(moveability);
		int32x2_t diff = vsub_s32(input, memory);
		int32x2_t a = vqrdmulh_s32(diff, coeff);
		int32x2_t b = vadd_s32(a, memory);
		memory = vadd_s32(a, b);
		vst1_s32(memory_, memory);
		int32x2_t b2 = vshl_n_s32(b, 1);
		return vsub_s32(b2, input);
	}

	/// NEON-vectorized saturating allpass filter for stereo (L/R in parallel)
	/// Uses saturating add to prevent overflow artifacts (bitcrushing)
	[[gnu::always_inline]] int32x2_t doAPFSaturating(int32x2_t input, int32_t moveability) {
		int32x2_t memory = vld1_s32(memory_);
		int32x2_t coeff = vdup_n_s32(moveability);
		int32x2_t diff = vsub_s32(input, memory);
		int32x2_t a = vqrdmulh_s32(diff, coeff);
		int32x2_t b = vadd_s32(a, memory);
		memory = vadd_s32(a, b);
		vst1_s32(memory_, memory);
		// Saturating: b + (b - input) instead of b * 2 - input
		return vqadd_s32(b, vsub_s32(b, input));
	}

	/// NEON-vectorized lowpass filter for stereo (L/R in parallel)
	[[gnu::always_inline]] int32x2_t doFilter(int32x2_t input, int32_t moveability) {
		int32x2_t memory = vld1_s32(memory_);
		int32x2_t coeff = vdup_n_s32(moveability);
		int32x2_t diff = vsub_s32(input, memory);
		int32x2_t a = vqrdmulh_s32(diff, coeff);
		int32x2_t b = vadd_s32(a, memory);
		memory = vadd_s32(b, a);
		vst1_s32(memory_, memory);
		return b;
	}
};

/// Quad filter component - processes 4 channels in parallel using NEON
/// Note: Do NOT add alignas() - it causes static initialization crashes
struct QuadFilterComponent {
	int32_t memory_[4]{};

	void reset() {
		memory_[0] = 0;
		memory_[1] = 0;
		memory_[2] = 0;
		memory_[3] = 0;
	}

	/// NEON-vectorized allpass filter for 4 channels in parallel
	[[gnu::always_inline]] int32x4_t doAPF(int32x4_t input, int32_t moveability) {
		int32x4_t memory = vld1q_s32(memory_);
		int32x4_t coeff = vdupq_n_s32(moveability);
		int32x4_t diff = vsubq_s32(input, memory);
		int32x4_t a = vqrdmulhq_s32(diff, coeff);
		int32x4_t b = vaddq_s32(a, memory);
		memory = vaddq_s32(a, b);
		vst1q_s32(memory_, memory);
		int32x4_t b2 = vshlq_n_s32(b, 1);
		return vsubq_s32(b2, input);
	}

	/// NEON-vectorized lowpass filter for 4 channels in parallel
	[[gnu::always_inline]] int32x4_t doFilter(int32x4_t input, int32_t moveability) {
		int32x4_t memory = vld1q_s32(memory_);
		int32x4_t coeff = vdupq_n_s32(moveability);
		int32x4_t diff = vsubq_s32(input, memory);
		int32x4_t a = vqrdmulhq_s32(diff, coeff);
		int32x4_t b = vaddq_s32(a, memory);
		memory = vaddq_s32(b, a);
		vst1q_s32(memory_, memory);
		return b;
	}
};

} // namespace deluge::dsp::filter
