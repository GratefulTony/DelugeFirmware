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

/// Subtractive gain staging analysis (from voice.cpp):
/// - FM: sourceAmplitude at full level → signal at ~23M peak
/// - Subtractive: oscillators scaled by >> 4 OR filterGain (both ~16x attenuation)
///
/// The shaper table is designed to handle varying input levels via the drive knob.
/// Subtractive signals use a smaller portion of the table at neutral drive.
/// FilterGain compensation only adjusts for resonance-induced level changes.
constexpr int32_t kShaperNeutralFilterGainInt = 1 << 28; // filterGain at neutral settings (integer)

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

/**
 * Process a mono buffer through the TableShaper using integer-only path (no floats)
 *
 * Table operates at FM signal levels. For subtractive synths, pass filterGain to
 * dynamically compute the boost needed to match FM operating levels.
 *
 * Note: The p^5 drive curve is computed once per buffer (hoisted), then the resulting
 * Q26 gain is smoothed per-sample. This is more efficient than computing p^5 per sample.
 *
 * @param buffer Audio buffer to process in-place
 * @param shaper The Shaper instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDriveGain Previous driveGain_Q26 value for smoothing (updated, stores Q26 gain not raw drive)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param smoothedMixNorm_Q16 Previous mixNorm value for smoothing (Q16.16 format, updated)
 * @param filterGain For subtractive mode: pass the filterGain from filter config.
 *                   For FM mode or subtractive without filters: pass 0.
 *                   When 0, no boost is applied (FM mode) or fixed boost for no-filter case.
 * @param hasFilters For subtractive mode: true if filters are active
 */
inline void shapeBufferInt32(std::span<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDriveGain,
                             q31_t mix, int32_t* smoothedMixNorm_Q16, q31_t filterGain, bool hasFilters) {
	if (buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	// Compute target driveGain ONCE (hoisted p^5 calculation)
	int32_t targetGain_Q26 = TableShaper::driveToGainQ26(drive);
	// Smooth driveGain_Q26 (not raw drive) - stored value is Q26 gain
	auto gainCtx = prepareShaperSmoothing(*smoothedDriveGain, targetGain_Q26);

	// Compute gain adjustment for subtractive mode
	// filterGain=0 means FM mode (no adjustment needed)
	// filterGain>0 means subtractive: compensate for resonance-induced level changes
	// At neutral filterGain (2^28), boostGain = 1.0 (no adjustment)
	// High resonance (low filterGain) → boost; low resonance (high filterGain) → attenuate
	bool needsGainAdjust = (filterGain > 0) && hasFilters;
	float boostGain = 1.0f;
	float attenGain = 1.0f;

	if (needsGainAdjust) {
		boostGain = static_cast<float>(kShaperNeutralFilterGainInt) / static_cast<float>(filterGain);
		attenGain = 1.0f / boostGain;
		// Skip per-sample multiply if effectively unity
		needsGainAdjust = (filterGain != kShaperNeutralFilterGainInt);
	}

	// Convert mix param to Q16 normalized value and setup smoothing (fastmath called once)
	int32_t targetMixNorm_Q16 = TableShaper::mixParamToNormQ16(mix);
	auto mixCtx = prepareShaperSmoothingQ16(*smoothedMixNorm_Q16, targetMixNorm_Q16);

	for (auto& sample : buffer) {
		gainCtx.current += multiply_32x32_rshift32(gainCtx.target - gainCtx.current, gainCtx.alpha) * 2;
		// Q16 IIR smoothing: current += (target - current) * alpha >> 16
		mixCtx.current += ((mixCtx.target - mixCtx.current) * mixCtx.alpha) >> 16;

		q31_t input = sample;
		if (needsGainAdjust) {
			// Clamp to prevent overflow when boostGain > 1 and sample is near INT32_MAX
			input =
			    static_cast<q31_t>(std::clamp(static_cast<float>(sample) * boostGain, -2147483648.0f, 2147483647.0f));
		}

		// processWithGain: table lookup with pre-computed driveGain (p^5 hoisted)
		q31_t out = shaper.processWithGain(input, gainCtx.current, mixCtx.current);

		if (needsGainAdjust) {
			out = static_cast<q31_t>(static_cast<float>(out) * attenGain);
		}

		sample = out;
	}

	*smoothedDriveGain = gainCtx.current;
	*smoothedMixNorm_Q16 = mixCtx.current;
}

/**
 * Process a stereo buffer through the TableShaper using integer-only path (no floats)
 *
 * Table operates at FM signal levels. For subtractive synths, pass filterGain to
 * dynamically compute the boost needed to match FM operating levels.
 *
 * Note: The p^5 drive curve is computed once per buffer (hoisted), then the resulting
 * Q26 gain is smoothed per-sample. This is more efficient than computing p^5 per sample.
 *
 * @param buffer Stereo audio buffer to process in-place
 * @param shaper The Shaper instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDriveGain Previous driveGain_Q26 value for smoothing (updated, stores Q26 gain not raw drive)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param smoothedMixNorm_Q16 Previous mixNorm value for smoothing (Q16.16 format, updated)
 * @param filterGain For subtractive mode: pass the filterGain from filter config.
 *                   For FM mode: pass 0 (no boost needed).
 * @param hasFilters For subtractive mode: true if filters are active
 */
inline void shapeBufferInt32(StereoBuffer<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDriveGain,
                             q31_t mix, int32_t* smoothedMixNorm_Q16, q31_t filterGain, bool hasFilters) {
	if (buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	// Compute target driveGain ONCE (hoisted p^5 calculation)
	int32_t targetGain_Q26 = TableShaper::driveToGainQ26(drive);
	// Smooth driveGain_Q26 (not raw drive) - stored value is Q26 gain
	auto gainCtx = prepareShaperSmoothing(*smoothedDriveGain, targetGain_Q26);

	// Compute gain adjustment for subtractive mode
	// filterGain=0 means FM mode (no adjustment needed)
	// filterGain>0 means subtractive: compensate for resonance-induced level changes
	// At neutral filterGain (2^28), boostGain = 1.0 (no adjustment)
	// High resonance (low filterGain) → boost; low resonance (high filterGain) → attenuate
	bool needsGainAdjust = (filterGain > 0) && hasFilters;
	float boostGain = 1.0f;
	float attenGain = 1.0f;

	if (needsGainAdjust) {
		boostGain = static_cast<float>(kShaperNeutralFilterGainInt) / static_cast<float>(filterGain);
		attenGain = 1.0f / boostGain;
		// Skip per-sample multiply if effectively unity
		needsGainAdjust = (filterGain != kShaperNeutralFilterGainInt);
	}

	// Convert mix param to Q16 normalized value and setup smoothing (fastmath called once)
	int32_t targetMixNorm_Q16 = TableShaper::mixParamToNormQ16(mix);
	auto mixCtx = prepareShaperSmoothingQ16(*smoothedMixNorm_Q16, targetMixNorm_Q16);

	for (auto& sample : buffer) {
		gainCtx.current += multiply_32x32_rshift32(gainCtx.target - gainCtx.current, gainCtx.alpha) * 2;
		// Q16 IIR smoothing: current += (target - current) * alpha >> 16
		mixCtx.current += ((mixCtx.target - mixCtx.current) * mixCtx.alpha) >> 16;

		q31_t inputL = sample.l;
		q31_t inputR = sample.r;
		if (needsGainAdjust) {
			// Clamp to prevent overflow when boostGain > 1 and sample is near INT32_MAX
			inputL =
			    static_cast<q31_t>(std::clamp(static_cast<float>(sample.l) * boostGain, -2147483648.0f, 2147483647.0f));
			inputR =
			    static_cast<q31_t>(std::clamp(static_cast<float>(sample.r) * boostGain, -2147483648.0f, 2147483647.0f));
		}

		// processWithGain: table lookup with pre-computed driveGain (p^5 hoisted)
		q31_t outL = shaper.processWithGain(inputL, gainCtx.current, mixCtx.current);
		q31_t outR = shaper.processWithGain(inputR, gainCtx.current, mixCtx.current);

		if (needsGainAdjust) {
			outL = static_cast<q31_t>(static_cast<float>(outL) * attenGain);
			outR = static_cast<q31_t>(static_cast<float>(outR) * attenGain);
		}

		sample.l = outL;
		sample.r = outR;
	}

	*smoothedDriveGain = gainCtx.current;
	*smoothedMixNorm_Q16 = mixCtx.current;
}

} // namespace deluge::dsp
