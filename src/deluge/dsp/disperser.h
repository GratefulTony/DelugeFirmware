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

#include "definitions_cxx.hpp"
#include "dsp/filter/ladder_components.h"
#include "io/debug/fx_benchmark.h"
#include "util/fixedpoint.h"
#include <arm_neon.h>
#include <array>
#include <cmath>

namespace deluge::dsp {

/**
 * Disperser effect using cascaded first-order allpass filters
 *
 * Creates phase smearing and tonal coloration effects:
 * - Frequency: Center frequency for the allpass cascade (50Hz - 8kHz)
 * - Spread: 0 = all stages at center (classic disperser), 127 = stages spread ±2 octaves
 * - Feedback: Output fed back to input for resonant peaks (0-50%)
 * - Stages: Number of active allpass stages (0-8)
 *
 * All parameters except stages are modulatable (q31_t) with per-sample interpolation.
 * Coefficient recalculation happens per-buffer when freq/spread change.
 */
class Disperser {
public:
	static constexpr size_t kMaxStages = 16;

	Disperser() = default;

	/**
	 * Update coefficients with smoothing (call once per buffer)
	 * @param freq Target center frequency (q31)
	 * @param spread Target frequency spread (q31)
	 * @param smoothedFreq Previous smoothed freq value (updated)
	 * @param smoothedSpread Previous smoothed spread value (updated)
	 */
	void updateCoefficientsSmoothed(q31_t freq, q31_t spread, q31_t* smoothedFreq, q31_t* smoothedSpread) {
		constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31); // ~100ms at 44.1kHz
		*smoothedFreq = *smoothedFreq + (multiply_32x32_rshift32(freq - *smoothedFreq, smoothingAlpha) << 1);
		*smoothedSpread = *smoothedSpread + (multiply_32x32_rshift32(spread - *smoothedSpread, smoothingAlpha) << 1);

		uint32_t freqU = static_cast<uint32_t>(*smoothedFreq) + 0x80000000u;
		uint32_t spreadU = static_cast<uint32_t>(*smoothedSpread) + 0x80000000u;
		updateCoefficients(freqU >> 25, spreadU >> 25);
	}

	/**
	 * Update coefficients when parameters change (call per-buffer, not per-sample)
	 * @param freq Center frequency (0-127, maps to 50Hz-8kHz)
	 * @param spread Frequency spread (0-127, 0=all same, 127=±2 octaves)
	 */
	void updateCoefficients(uint8_t freq, uint8_t spread) {
		if (freq == lastFreq_ && spread == lastSpread_) {
			return; // No change, skip recalc
		}
		lastFreq_ = freq;
		lastSpread_ = spread;

		// freq: 0-127 -> 50Hz-8kHz (7.3 octaves), spread: 0-127 -> ±4 octaves
		float centerHz = 50.0f * std::pow(2.0f, (freq / 127.0f) * 7.3f);
		float spreadOctaves = (spread / 127.0f) * 4.0f;

		constexpr float halfStages = (kMaxStages - 1) / 2.0f;
		for (size_t i = 0; i < kMaxStages; ++i) {
			float stagePosition = (static_cast<float>(i) - halfStages) / halfStages;
			float stageHz = std::clamp(centerHz * std::pow(2.0f, stagePosition * spreadOctaves), 20.0f, 20000.0f);
			float fc = std::clamp(stageHz / static_cast<float>(kSampleRate), 0.001f, 0.49f);
			float wc = std::tan(3.14159265358979f * fc);
			coeffs_[i] = static_cast<q31_t>((wc / (1.0f + wc)) * ONE_Q31);
		}
	}

	/**
	 * Process a stereo sample pair through the disperser (NEON vectorized)
	 * Uses int32x2_t to process L/R channels in parallel for ~2x speedup.
	 * @param inL Left input sample
	 * @param inR Right input sample
	 * @param outL Left output sample (written)
	 * @param outR Right output sample (written)
	 * @param stages Number of active stages (0-16)
	 * @param feedback Bipolar feedback (q31): 0=none, positive=resonant peaks, negative=notches
	 */
	[[gnu::always_inline]] inline void process(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR, uint8_t stages,
	                                           q31_t feedback) {
		if (stages == 0) {
			outL = inL;
			outR = inR;
			return;
		}

		size_t numStages = std::min(static_cast<size_t>(stages), kMaxStages);
		int32x2_t input = {inL, inR};

		// Bipolar feedback scaled to ±90% (DC blocker prevents runaway)
		q31_t fbGain = (multiply_32x32_rshift32(feedback, 0x73333333) << 1);

		// DC Blocker: y[n] = x[n] - x[n-1] + 0.995 * y[n-1]
		constexpr q31_t dcAlpha = 0x7F5C28F5;
		int32x2_t dcIn = vld1_s32(feedback_);
		int32x2_t dcOut =
		    vadd_s32(vsub_s32(dcIn, vld1_s32(dcPrevIn_)), vqrdmulh_s32(vld1_s32(dcPrevOut_), vdup_n_s32(dcAlpha)));
		vst1_s32(dcPrevIn_, dcIn);
		vst1_s32(dcPrevOut_, dcOut);

		// Feedback + allpass cascade
		int32x2_t proc = vqadd_s32(input, vqrdmulh_s32(dcOut, vdup_n_s32(fbGain)));
		for (size_t i = 0; i < numStages; ++i) {
			proc = stages_[i].doAPFSaturating(proc, coeffs_[i]);
		}

		vst1_s32(feedback_, proc);
		outL = vget_lane_s32(proc, 0);
		outR = vget_lane_s32(proc, 1);
	}

	/**
	 * Process a stereo buffer through the disperser with parameter interpolation
	 * @param buffer Stereo audio buffer to process in-place
	 * @param stages Number of stages (0 = bypass)
	 * @param feedback Target feedback amount (q31)
	 * @param smoothedFeedback Previous smoothed feedback (updated)
	 */
	void processBuffer(StereoBuffer<q31_t> buffer, uint8_t stages, q31_t feedback, q31_t* smoothedFeedback) {
		if (stages == 0 || buffer.empty()) {
			return;
		}

		FX_BENCH_DECLARE(bench, "disperser");
		FX_BENCH_SCOPE(bench);

		constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);
		q31_t targetSmoothed =
		    *smoothedFeedback + (multiply_32x32_rshift32(feedback - *smoothedFeedback, smoothingAlpha) << 1);
		int32_t fbIncrement = (targetSmoothed - *smoothedFeedback) / static_cast<int32_t>(buffer.size());
		q31_t currentFb = *smoothedFeedback;

		for (auto& sample : buffer) {
			currentFb += fbIncrement;
			q31_t outL, outR;
			process(sample.l, sample.r, outL, outR, stages, currentFb);
			sample.l = outL;
			sample.r = outR;
		}

		*smoothedFeedback = targetSmoothed;
	}

	/// Reset all filter states
	void reset() {
		for (auto& stage : stages_) {
			stage.reset();
		}
		feedback_[0] = 0;
		feedback_[1] = 0;
		dcPrevIn_[0] = 0;
		dcPrevIn_[1] = 0;
		dcPrevOut_[0] = 0;
		dcPrevOut_[1] = 0;
	}

private:
	std::array<filter::StereoFilterComponent, kMaxStages> stages_{};
	std::array<q31_t, kMaxStages> coeffs_{};
	q31_t feedback_[2]{};
	q31_t dcPrevIn_[2]{};
	q31_t dcPrevOut_[2]{};
	uint8_t lastFreq_{255};
	uint8_t lastSpread_{255};
};

} // namespace deluge::dsp
