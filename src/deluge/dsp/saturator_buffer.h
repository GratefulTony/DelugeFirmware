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

#include "deluge/dsp/saturator.h"
#include "deluge/util/fixedpoint.h"
#include "dsp_ng/core/types.hpp"
#include "io/debug/fx_benchmark.h"
#include <span>

namespace deluge::dsp {

/// Per-sample IIR alpha for q31 parameter smoothing (~40ms time constant at 44.1kHz)
constexpr q31_t kSaturatorSmoothingAlpha = static_cast<q31_t>(0.0005 * ONE_Q31);

/// Context for per-sample IIR parameter smoothing during buffer processing
struct SaturatorSmoothingContext {
	q31_t current;
	q31_t alpha;
	q31_t target;
};

/// Prepare parameter smoothing for per-sample IIR processing
[[gnu::always_inline]] inline SaturatorSmoothingContext prepareSaturatorSmoothing(q31_t state, q31_t target) {
	return {state, kSaturatorSmoothingAlpha, target};
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
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "saturator_table");
	FX_BENCH_SCOPE(bench);

	auto ctx = prepareSaturatorSmoothing(*smoothedDrive, drive);

	for (auto& sample : buffer) {
		ctx.current += multiply_32x32_rshift32(ctx.target - ctx.current, ctx.alpha) * 2;
		q31_t wet = saturator.process(sample, ctx.current, prevX);
		q31_t dry = multiply_32x32_rshift32(sample, ONE_Q31 - mix) << 1;
		wet = multiply_32x32_rshift32(wet, mix) << 1;
		sample = add_saturate(dry, wet);
	}

	*smoothedDrive = ctx.current;
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
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "saturator_table");
	FX_BENCH_SCOPE(bench);

	auto ctx = prepareSaturatorSmoothing(*smoothedDrive, drive);

	for (auto& sample : buffer) {
		ctx.current += multiply_32x32_rshift32(ctx.target - ctx.current, ctx.alpha) * 2;
		q31_t wetL = saturator.process(sample.l, ctx.current, prevXL);
		q31_t wetR = saturator.process(sample.r, ctx.current, prevXR);
		q31_t dryL = multiply_32x32_rshift32(sample.l, ONE_Q31 - mix) << 1;
		q31_t dryR = multiply_32x32_rshift32(sample.r, ONE_Q31 - mix) << 1;
		wetL = multiply_32x32_rshift32(wetL, mix) << 1;
		wetR = multiply_32x32_rshift32(wetR, mix) << 1;
		sample.l = add_saturate(dryL, wetL);
		sample.r = add_saturate(dryR, wetR);
	}

	*smoothedDrive = ctx.current;
}

/**
 * Process a mono buffer through the Saturator using integer-only path (no floats)
 *
 * @param buffer Audio buffer to process in-place
 * @param saturator The Saturator instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDrive Previous drive value for smoothing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 */
inline void saturateBufferInt32(std::span<q31_t> buffer, Saturator& saturator, q31_t drive, q31_t* smoothedDrive,
                                q31_t mix) {
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "saturator_table");
	FX_BENCH_SCOPE(bench);

	auto ctx = prepareSaturatorSmoothing(*smoothedDrive, drive);

	for (auto& sample : buffer) {
		ctx.current += multiply_32x32_rshift32(ctx.target - ctx.current, ctx.alpha) * 2;
		q31_t wet = saturator.processInt32(sample, ctx.current);
		q31_t dry = multiply_32x32_rshift32(sample, ONE_Q31 - mix) << 1;
		wet = multiply_32x32_rshift32(wet, mix) << 1;
		sample = add_saturate(dry, wet);
	}

	*smoothedDrive = ctx.current;
}

/**
 * Process a stereo buffer through the Saturator using integer-only path (no floats)
 * Includes subtractive mixing: >50% mix subtracts dry from wet to isolate harmonics
 *
 * @param buffer Stereo audio buffer to process in-place
 * @param saturator The Saturator instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDrive Previous drive value for smoothing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 */
inline void saturateBufferInt32(StereoBuffer<q31_t> buffer, Saturator& saturator, q31_t drive, q31_t* smoothedDrive,
                                q31_t mix) {
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "saturator_table");
	FX_BENCH_SCOPE(bench);

	auto ctx = prepareSaturatorSmoothing(*smoothedDrive, drive);
	constexpr q31_t kMixHalf = ONE_Q31 >> 1;

	for (auto& sample : buffer) {
		ctx.current += multiply_32x32_rshift32(ctx.target - ctx.current, ctx.alpha) * 2;
		q31_t wetL = saturator.processInt32(sample.l, ctx.current);
		q31_t wetR = saturator.processInt32(sample.r, ctx.current);

		if (mix <= kMixHalf) {
			// Standard crossfade: 0% = full dry, 50% = full wet
			q31_t wetAmount = mix << 1;
			q31_t dryAmount = ONE_Q31 - wetAmount;
			q31_t dryL = multiply_32x32_rshift32(sample.l, dryAmount) << 1;
			q31_t dryR = multiply_32x32_rshift32(sample.r, dryAmount) << 1;
			wetL = multiply_32x32_rshift32(wetL, wetAmount) << 1;
			wetR = multiply_32x32_rshift32(wetR, wetAmount) << 1;
			sample.l = add_saturate(dryL, wetL);
			sample.r = add_saturate(dryR, wetR);
		}
		else {
			// Subtractive mode: 50% = full wet, 100% = wet - dry (harmonics only)
			q31_t subAmount = (mix - kMixHalf) << 1;
			q31_t subL = multiply_32x32_rshift32(sample.l, subAmount) << 1;
			q31_t subR = multiply_32x32_rshift32(sample.r, subAmount) << 1;
			sample.l = wetL - subL;
			sample.r = wetR - subR;
		}
	}

	*smoothedDrive = ctx.current;
}

} // namespace deluge::dsp
