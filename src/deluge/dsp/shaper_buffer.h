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

#include "deluge/dsp/shaper.h"
#include "deluge/util/fixedpoint.h"
#include "deluge/util/functions.h"
#include "dsp_ng/core/types.hpp"
#include "io/debug/fx_benchmark.h"
#include <climits>
#include <span>

namespace deluge::dsp {

/// Per-sample IIR alpha for q31 parameter smoothing (~40ms time constant at 44.1kHz)
constexpr q31_t kShaperSmoothingAlpha = static_cast<q31_t>(0.0005 * ONE_Q31);

/// DC blocker coefficient for ~5Hz cutoff at 44.1kHz (removes DC from asymmetric waveshaping)
/// alpha = fc / fs = 5 / 44100 ≈ 0.000113
/// 5Hz gives <-0.1dB at 30Hz - essentially transparent to bass content
/// Matches multiband compressor's kDCBlockCoeff
constexpr q31_t kShaperDcBlockCoeff = static_cast<q31_t>((5.0f / 44100.0f) * ONE_Q31);

/// Subtractive gain staging analysis (from voice.cpp):
/// - FM: LOCAL_VOLUME applied before shaper via sourceAmplitude → signal varies with velocity
/// - Subtractive: LOCAL_VOLUME applied AFTER shaper → signal always at full osc level
///
/// Empirically measured:
/// - FM at max velocity (v=127): inputScale=128 → notch near drive=-11
/// - Subtractive: similar saturation point, no base attenuation needed
///
/// FilterGain compensation adjusts when resonance changes filterGain from neutral
constexpr int32_t kShaperNeutralFilterGainInt = 1 << 28; // filterGain at neutral settings (integer)
constexpr float kShaperNeutralFilterGainInvF = 1.0f / static_cast<float>(1 << 28); // precomputed reciprocal

/// Context for per-sample IIR parameter smoothing during buffer processing
struct ShaperSmoothingContext {
	q31_t current;
	q31_t alpha;
	q31_t target;
};

/// Prepare parameter smoothing for per-sample IIR processing
[[gnu::always_inline]] inline ShaperSmoothingContext prepareShaperSmoothing(q31_t state, q31_t target) {
	return {state, kShaperSmoothingAlpha, target};
}

/// Q16 smoothing alpha (~40ms time constant at 44.1kHz, matches q31 version)
/// 0.0005 * 65536 ≈ 33
constexpr int32_t kShaperSmoothingAlphaQ16 = 33;

/// Context for per-sample Q16 parameter smoothing (used for mixNorm)
struct ShaperSmoothingContextQ16 {
	int32_t current;
	int32_t alpha;
	int32_t target;
};

/// Prepare Q16 parameter smoothing for per-sample IIR processing
[[gnu::always_inline]] inline ShaperSmoothingContextQ16 prepareShaperSmoothingQ16(int32_t state, int32_t target) {
	return {state, kShaperSmoothingAlphaQ16, target};
}

// TODO: Remove float shapeBuffer functions - not used, shapeBufferInt32 is the intended signal path
/**
 * Process a mono buffer through the TableShaper waveshaper
 *
 * @param buffer Audio buffer to process in-place
 * @param shaper The Shaper instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDrive Previous drive value for smoothing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param prevX ADAA state (previous input sample), nullptr if AA disabled
 */
inline void shapeBuffer(std::span<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDrive, q31_t mix,
                        float* prevX = nullptr) {
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	auto ctx = prepareShaperSmoothing(*smoothedDrive, drive);

	for (auto& sample : buffer) {
		ctx.current += multiply_32x32_rshift32(ctx.target - ctx.current, ctx.alpha) * 2;
		q31_t wet = shaper.process(sample, ctx.current, prevX);
		q31_t dry = multiply_32x32_rshift32(sample, ONE_Q31 - mix) << 1;
		wet = multiply_32x32_rshift32(wet, mix) << 1;
		sample = add_saturate(dry, wet);
	}

	*smoothedDrive = ctx.current;
}

/**
 * Process a stereo buffer through the TableShaper waveshaper
 *
 * @param buffer Stereo audio buffer to process in-place
 * @param shaper The Shaper instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDrive Previous drive value for smoothing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param prevXL Left channel ADAA state (previous input sample), nullptr if AA disabled
 * @param prevXR Right channel ADAA state (previous input sample), nullptr if AA disabled
 */
inline void shapeBuffer(StereoBuffer<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDrive, q31_t mix,
                        float* prevXL = nullptr, float* prevXR = nullptr) {
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	auto ctx = prepareShaperSmoothing(*smoothedDrive, drive);

	for (auto& sample : buffer) {
		ctx.current += multiply_32x32_rshift32(ctx.target - ctx.current, ctx.alpha) * 2;
		q31_t wetL = shaper.process(sample.l, ctx.current, prevXL);
		q31_t wetR = shaper.process(sample.r, ctx.current, prevXR);
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
 * Process a mono buffer through the TableShaper using integer-only path (no floats)
 *
 * Table operates at FM signal levels. For subtractive synths, pass filterGain to
 * dynamically compute the boost needed to match FM operating levels.
 *
 * @param buffer Audio buffer to process in-place
 * @param shaper The Shaper instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDrive Previous drive value for smoothing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param smoothedMixNorm_Q16 Previous mixNorm value for smoothing (Q16.16 format, updated)
 * @param filterGain For subtractive mode: pass the filterGain from filter config.
 *                   For FM mode or subtractive without filters: pass 0.
 *                   When 0, no boost is applied (FM mode) or fixed boost for no-filter case.
 * @param hasFilters For subtractive mode: true if filters are active
 * @param dcBlockState DC blocker state for removing DC offset from asymmetric waveshaping (per-voice)
 */
inline void shapeBufferInt32(std::span<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDrive, q31_t mix,
                             int32_t* smoothedMixNorm_Q16, q31_t filterGain, bool hasFilters, q31_t* dcBlockState) {
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	auto ctx = prepareShaperSmoothing(*smoothedDrive, drive);

	// Compute gain adjustment for subtractive mode
	// filterGain=0 means FM mode (no adjustment needed)
	// Only adjust when hasFilters and filterGain deviates from neutral
	bool needsGainAdjust = (filterGain > 0) && hasFilters;
	int32_t boostGain_Q28 = 1 << 28; // 1.0 in Q28
	int32_t attenGain_Q28 = 1 << 28;

	if (needsGainAdjust) {
		float filterGainF = static_cast<float>(filterGain);
		float boostGain = static_cast<float>(kShaperNeutralFilterGainInt) / filterGainF;
		float attenGain = filterGainF * kShaperNeutralFilterGainInvF;
		// Convert to Q28 for integer multiply in loop
		boostGain_Q28 = static_cast<int32_t>(boostGain * (1 << 28));
		attenGain_Q28 = static_cast<int32_t>(attenGain * (1 << 28));
		// Skip per-sample multiply if effectively unity
		needsGainAdjust = (filterGain != kShaperNeutralFilterGainInt);
	}

	// Convert mix param to Q16 normalized value and setup smoothing (fastmath called once)
	int32_t targetMixNorm_Q16 = TableShaper::mixParamToNormQ16(mix);
	auto mixCtx = prepareShaperSmoothingQ16(*smoothedMixNorm_Q16, targetMixNorm_Q16);

	for (auto& sample : buffer) {
		ctx.current += multiply_32x32_rshift32(ctx.target - ctx.current, ctx.alpha) * 2;
		// Q16 IIR smoothing: current += (target - current) * alpha >> 16
		mixCtx.current += ((mixCtx.target - mixCtx.current) * mixCtx.alpha) >> 16;

		q31_t input = sample;
		if (needsGainAdjust) {
			input = static_cast<q31_t>((static_cast<int64_t>(sample) * boostGain_Q28) >> 28);
		}

		// processInt32 handles: drive gain, table lookup, and amplitude-dependent blend
		q31_t out = shaper.processInt32(input, ctx.current, mixCtx.current);

		if (needsGainAdjust) {
			out = static_cast<q31_t>((static_cast<int64_t>(out) * attenGain_Q28) >> 28);
		}

		// DC blocker: HPF by subtracting lowpassed signal (removes DC from asymmetric waveshaping)
		if (dcBlockState) {
			q31_t lpf = *dcBlockState;
			q31_t delta = multiply_32x32_rshift32(out - lpf, kShaperDcBlockCoeff) << 1;
			lpf += delta;
			*dcBlockState = lpf;
			out = out - lpf;
		}
		sample = out;
	}

	*smoothedDrive = ctx.current;
	*smoothedMixNorm_Q16 = mixCtx.current;
}

/**
 * Process a stereo buffer through the TableShaper using integer-only path (no floats)
 * Includes subtractive mixing: >50% mix subtracts dry from wet to isolate harmonics
 *
 * Table operates at FM signal levels. For subtractive synths, pass filterGain to
 * dynamically compute the boost needed to match FM operating levels.
 *
 * @param buffer Stereo audio buffer to process in-place
 * @param shaper The Shaper instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDrive Previous drive value for smoothing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param smoothedMixNorm_Q16 Previous mixNorm value for smoothing (Q16.16 format, updated)
 * @param filterGain For subtractive mode: pass the filterGain from filter config.
 *                   For FM mode: pass 0 (no boost needed).
 * @param hasFilters For subtractive mode: true if filters are active
 * @param dcBlockStateL Left DC blocker state (per-voice)
 * @param dcBlockStateR Right DC blocker state (per-voice)
 */
inline void shapeBufferInt32(StereoBuffer<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDrive,
                             q31_t mix, int32_t* smoothedMixNorm_Q16, q31_t filterGain, bool hasFilters,
                             q31_t* dcBlockStateL, q31_t* dcBlockStateR) {
	if (mix <= 0 || buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	auto ctx = prepareShaperSmoothing(*smoothedDrive, drive);

	// Compute gain adjustment for subtractive mode
	// filterGain=0 means FM mode (no adjustment needed)
	// Only adjust when hasFilters and filterGain deviates from neutral
	bool needsGainAdjust = (filterGain > 0) && hasFilters;
	int32_t boostGain_Q28 = 1 << 28; // 1.0 in Q28
	int32_t attenGain_Q28 = 1 << 28;

	if (needsGainAdjust) {
		float filterGainF = static_cast<float>(filterGain);
		float boostGain = static_cast<float>(kShaperNeutralFilterGainInt) / filterGainF;
		float attenGain = filterGainF * kShaperNeutralFilterGainInvF;
		// Convert to Q28 for integer multiply in loop
		boostGain_Q28 = static_cast<int32_t>(boostGain * (1 << 28));
		attenGain_Q28 = static_cast<int32_t>(attenGain * (1 << 28));
		// Skip per-sample multiply if effectively unity
		needsGainAdjust = (filterGain != kShaperNeutralFilterGainInt);
	}

	// Convert mix param to Q16 normalized value and setup smoothing (fastmath called once)
	int32_t targetMixNorm_Q16 = TableShaper::mixParamToNormQ16(mix);
	auto mixCtx = prepareShaperSmoothingQ16(*smoothedMixNorm_Q16, targetMixNorm_Q16);

	for (auto& sample : buffer) {
		ctx.current += multiply_32x32_rshift32(ctx.target - ctx.current, ctx.alpha) * 2;
		// Q16 IIR smoothing: current += (target - current) * alpha >> 16
		mixCtx.current += ((mixCtx.target - mixCtx.current) * mixCtx.alpha) >> 16;

		q31_t inputL = sample.l;
		q31_t inputR = sample.r;
		if (needsGainAdjust) {
			inputL = static_cast<q31_t>((static_cast<int64_t>(sample.l) * boostGain_Q28) >> 28);
			inputR = static_cast<q31_t>((static_cast<int64_t>(sample.r) * boostGain_Q28) >> 28);
		}

		// processInt32 handles: drive gain, table lookup, and amplitude-dependent blend
		q31_t outL = shaper.processInt32(inputL, ctx.current, mixCtx.current);
		q31_t outR = shaper.processInt32(inputR, ctx.current, mixCtx.current);

		if (needsGainAdjust) {
			outL = static_cast<q31_t>((static_cast<int64_t>(outL) * attenGain_Q28) >> 28);
			outR = static_cast<q31_t>((static_cast<int64_t>(outR) * attenGain_Q28) >> 28);
		}

		// DC blocker: HPF by subtracting lowpassed signal (removes DC from asymmetric waveshaping)
		if (dcBlockStateL && dcBlockStateR) {
			q31_t lpfL = *dcBlockStateL;
			q31_t lpfR = *dcBlockStateR;
			q31_t deltaL = multiply_32x32_rshift32(outL - lpfL, kShaperDcBlockCoeff) << 1;
			q31_t deltaR = multiply_32x32_rshift32(outR - lpfR, kShaperDcBlockCoeff) << 1;
			lpfL += deltaL;
			lpfR += deltaR;
			*dcBlockStateL = lpfL;
			*dcBlockStateR = lpfR;
			outL = outL - lpfL;
			outR = outR - lpfR;
		}
		sample.l = outL;
		sample.r = outR;
	}

	*smoothedDrive = ctx.current;
	*smoothedMixNorm_Q16 = mixCtx.current;
}

} // namespace deluge::dsp
