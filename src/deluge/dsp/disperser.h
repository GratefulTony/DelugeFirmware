/*
 * Copyright © 2024 Synthstrom Audible Limited
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

#include "definitions_cxx.hpp"
#include "dsp/filter/ladder_components.h"
#include "util/fixedpoint.h"
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
		// Exponential smoothing (~100ms at 44.1kHz with 128-sample buffers)
		constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);

		// Smooth freq
		*smoothedFreq = *smoothedFreq + (multiply_32x32_rshift32(freq - *smoothedFreq, smoothingAlpha) << 1);

		// Smooth spread
		*smoothedSpread = *smoothedSpread + (multiply_32x32_rshift32(spread - *smoothedSpread, smoothingAlpha) << 1);

		// Convert q31 to 0-127 range for coefficient calculation
		// q31 range is -2^31 to 2^31-1, we map to 0-127
		uint32_t freqU = static_cast<uint32_t>(*smoothedFreq) + 0x80000000u;
		uint32_t spreadU = static_cast<uint32_t>(*smoothedSpread) + 0x80000000u;
		uint8_t freq8 = freqU >> 25; // Top 7 bits -> 0-127
		uint8_t spread8 = spreadU >> 25;

		updateCoefficients(freq8, spread8);
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

		// Map freq 0-127 to 50Hz-8kHz (logarithmic)
		// log2(8000/50) ≈ 7.3 octaves
		float freqNorm = freq / 127.0f;
		float centerHz = 50.0f * std::pow(2.0f, freqNorm * 7.3f);

		// Map spread 0-127 to 0-4 octaves spread per direction
		// At max spread, stages span 8 octaves total (centerHz/16 to centerHz*16)
		float spreadOctaves = (spread / 127.0f) * 4.0f;

		// Calculate per-stage frequency
		// At spread=0, all stages at centerHz
		// At spread=max, stages spread from centerHz/16 to centerHz*16
		constexpr float halfStages = (kMaxStages - 1) / 2.0f; // 7.5 for 16 stages
		for (size_t i = 0; i < kMaxStages; ++i) {
			float stagePosition = (static_cast<float>(i) - halfStages) / halfStages; // -1 to +1
			float stageHz = centerHz * std::pow(2.0f, stagePosition * spreadOctaves);

			// Clamp to valid range
			stageHz = std::clamp(stageHz, 20.0f, 20000.0f);

			// Calculate allpass coefficient: coeff = tan(pi * f / fs) / (1 + tan(pi * f / fs))
			float fc = stageHz / static_cast<float>(kSampleRate);
			fc = std::clamp(fc, 0.001f, 0.49f);
			float wc = std::tan(3.14159265358979f * fc);
			float coeff = wc / (1.0f + wc);
			coeffs_[i] = static_cast<q31_t>(coeff * ONE_Q31);
		}
	}

	/**
	 * Saturating allpass filter - prevents overflow that causes bitcrushing artifacts
	 * Uses the formula: output = 2*lowpass - input, with saturation on the final add
	 * @param input Input sample
	 * @param memory Filter state (updated in place)
	 * @param coeff Allpass coefficient (moveability = tan(f)/(1+tan(f)))
	 */
	[[gnu::always_inline]] static inline q31_t doAPFSaturating(q31_t input, q31_t& memory, q31_t coeff) {
		q31_t a = q31_mult_rounded(input - memory, coeff);
		q31_t b = a + memory;
		memory = a + b;
		// Original: return b * 2 - input; // This overflows!
		// Rewrite as: b + (b - input) with saturation
		return add_saturate(b, b - input);
	}

	/**
	 * Process a stereo sample pair through the disperser
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

		// Clamp stages
		size_t numStages = std::min(static_cast<size_t>(stages), kMaxStages);

		// Bipolar feedback: center (0) = no feedback, CW = positive, CCW = negative
		// Scale to ±90% max for strong resonance (DC blocker prevents runaway)
		// feedback is q31: -2^31 to +2^31-1, we want ±0.9 gain
		// 0.9 in q31 ≈ 0x73333333
		q31_t fbGain = (multiply_32x32_rshift32(feedback, 0x73333333) << 1); // ~90% of input

		// Apply DC blocker to feedback to prevent low-frequency runaway
		// Formula: y[n] = x[n] - x[n-1] + alpha * y[n-1], alpha ≈ 0.995 (~10Hz cutoff)
		// This removes DC and sub-bass buildup while preserving musical content
		constexpr q31_t dcAlpha = 0x7F5C28F5; // 0.995 in q31
		q31_t dcInL = feedbackL_;
		q31_t dcInR = feedbackR_;
		q31_t dcOutL = dcInL - dcPrevInL_ + (multiply_32x32_rshift32(dcPrevOutL_, dcAlpha) << 1);
		q31_t dcOutR = dcInR - dcPrevInR_ + (multiply_32x32_rshift32(dcPrevOutR_, dcAlpha) << 1);
		dcPrevInL_ = dcInL;
		dcPrevInR_ = dcInR;
		dcPrevOutL_ = dcOutL;
		dcPrevOutR_ = dcOutR;

		// Mix DC-blocked feedback into input
		q31_t procL = add_saturate(inL, multiply_32x32_rshift32(dcOutL, fbGain) << 1);
		q31_t procR = add_saturate(inR, multiply_32x32_rshift32(dcOutR, fbGain) << 1);

		// Process through allpass cascade using saturating allpass
		for (size_t i = 0; i < numStages; ++i) {
			procL = doAPFSaturating(procL, stagesL_[i].memory, coeffs_[i]);
			procR = doAPFSaturating(procR, stagesR_[i].memory, coeffs_[i]);
		}

		// Store for feedback
		feedbackL_ = procL;
		feedbackR_ = procR;

		outL = procL;
		outR = procR;
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

		// Calculate per-sample feedback increment for smooth modulation
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
		for (auto& stage : stagesL_) {
			stage.reset();
		}
		for (auto& stage : stagesR_) {
			stage.reset();
		}
		feedbackL_ = 0;
		feedbackR_ = 0;
		dcPrevInL_ = 0;
		dcPrevInR_ = 0;
		dcPrevOutL_ = 0;
		dcPrevOutR_ = 0;
	}

private:
	// Per-channel allpass filter states
	std::array<filter::BasicFilterComponent, kMaxStages> stagesL_{};
	std::array<filter::BasicFilterComponent, kMaxStages> stagesR_{};

	// Cached coefficients for each stage
	std::array<q31_t, kMaxStages> coeffs_{};

	// Feedback state
	q31_t feedbackL_{0};
	q31_t feedbackR_{0};

	// DC blocker state for feedback path
	q31_t dcPrevInL_{0};
	q31_t dcPrevInR_{0};
	q31_t dcPrevOutL_{0};
	q31_t dcPrevOutR_{0};

	// Cached parameters for change detection
	uint8_t lastFreq_{255}; // Invalid value to force initial calc
	uint8_t lastSpread_{255};
};

} // namespace deluge::dsp
