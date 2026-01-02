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
#include "dsp/fast_math.h"
#include "dsp/filter/ladder_components.h"
#include "util/fixedpoint.h"
#include <array>
#include <cmath>

namespace deluge::dsp::filter {

/// Output structure for the three frequency bands
struct CrossoverBands {
	q31_t low;
	q31_t mid;
	q31_t high;
};

/// Allpass Subtraction Crossover - 3-band filter with perfect phase-coherent reconstruction.
/// Also known as "Complementary Allpass Crossover".
///
/// The bands sum back EXACTLY to the original signal with no phase distortion.
/// This is the key advantage over Linkwitz-Riley crossovers which have phase shifts.
///
/// Method: LP = (input + allpass) / 2, HP = (input - allpass) / 2
/// At crossover: both bands are at -3dB (vs -6dB for Linkwitz-Riley)
///
/// Characteristics:
/// - Perfect reconstruction: LOW + MID + HIGH = input (exactly)
/// - 6dB/oct slopes (gentle, smooth band blending)
/// - -3dB at crossover frequency
/// - Minimal CPU cost (~2 first-order allpasses)
/// - Good for dynamics processing where band isolation isn't critical
///
/// For steeper slopes (12dB/oct), use LR2Crossover instead (see lr_crossover.h).
///
/// Template parameter ORDER (only ORDER=1 is recommended for 3-band: Higher orders dont work properly, but are
/// interesting for DOTT):
template <int ORDER>
class AllpassCrossover {
	static_assert(ORDER >= 1 && ORDER <= 5, "ORDER must be 1-5");

public:
	using Bands = CrossoverBands;

	AllpassCrossover() = default;

	/// Set the low-mid crossover frequency
	/// @param freqHz Crossover frequency in Hz (typically 100-500Hz)
	void setLowCrossover(float freqHz) {
		lowCrossoverHz_ = freqHz;
		lowCoeff_ = calculateCoefficient(freqHz);
	}

	/// Set the mid-high crossover frequency
	/// @param freqHz Crossover frequency in Hz (typically 1000-5000Hz)
	void setHighCrossover(float freqHz) {
		highCrossoverHz_ = freqHz;
		highCoeff_ = calculateCoefficient(freqHz);
	}

	/// Get the current low crossover frequency in Hz
	[[nodiscard]] float getLowCrossoverHz() const { return lowCrossoverHz_; }

	/// Get the current high crossover frequency in Hz
	[[nodiscard]] float getHighCrossoverHz() const { return highCrossoverHz_; }

	/// Process a stereo sample pair using NEON (parallel L/R processing)
	[[gnu::always_inline]] inline void processStereo(q31_t inputL, q31_t inputR, Bands& outL, Bands& outR) {
		// Pack L/R into NEON vector
		int32x2_t input = {inputL, inputR};

		// Step 1: Low crossover splits input into LOW and REST
		int32x2_t apLow = input;
		for (int i = 0; i < ORDER; ++i) {
			apLow = stereoState_.apLow[i].doAPF(apLow, lowCoeff_);
		}
		// low = (input + apLow) >> 1
		int32x2_t low = vhadd_s32(input, apLow);
		// rest = (input - apLow) >> 1
		int32x2_t rest = vhsub_s32(input, apLow);

		// Step 2: High crossover splits REST into MID and HIGH
		int32x2_t apHigh = rest;
		for (int i = 0; i < ORDER; ++i) {
			apHigh = stereoState_.apHigh[i].doAPF(apHigh, highCoeff_);
		}
		// mid = (rest + apHigh) >> 1
		int32x2_t mid = vhadd_s32(rest, apHigh);
		// high = (rest - apHigh) >> 1
		int32x2_t high = vhsub_s32(rest, apHigh);

		// Unpack results
		outL = {vget_lane_s32(low, 0), vget_lane_s32(mid, 0), vget_lane_s32(high, 0)};
		outR = {vget_lane_s32(low, 1), vget_lane_s32(mid, 1), vget_lane_s32(high, 1)};
	}

	/// Reset all filter states
	void reset() {
		for (int i = 0; i < ORDER; ++i) {
			stereoState_.apLow[i].reset();
			stereoState_.apHigh[i].reset();
		}
	}

private:
	/// NEON stereo filter state (processes L/R in parallel)
	struct StereoState {
		std::array<StereoFilterComponent, ORDER> apLow;  // Low crossover stages
		std::array<StereoFilterComponent, ORDER> apHigh; // High crossover stages
	};

	/// Calculate the allpass coefficient for a given frequency.
	/// For BasicFilterComponent::doAPF(): coeff = tan(pi * fc / fs) / (1 + tan(pi * fc / fs))
	[[nodiscard]] static q31_t calculateCoefficient(float freqHz) {
		float fc = freqHz / static_cast<float>(kSampleRate);
		// Clamp to valid range to prevent instability
		fc = std::clamp(fc, 0.001f, 0.49f);
		float wc = fastTan(3.14159265358979f * fc);
		// Coefficient for BasicFilterComponent::doAPF()
		float coeff = wc / (1.0f + wc);
		return static_cast<q31_t>(coeff * ONE_Q31);
	}

	StereoState stereoState_{};

	q31_t lowCoeff_ = calculateCoefficient(200.0f);   // Default 200Hz
	q31_t highCoeff_ = calculateCoefficient(2000.0f); // Default 2kHz

	float lowCrossoverHz_ = 200.0f;
	float highCrossoverHz_ = 2000.0f;
};

// Type aliases for convenience
using AllpassCrossoverLR1 = AllpassCrossover<1>; // 6dB/oct - cheapest, recommended
using AllpassCrossoverLR2 = AllpassCrossover<2>; // 12dB/oct - experimental
using AllpassCrossoverLR3 = AllpassCrossover<3>; // 18dB/oct - experimental
using AllpassCrossoverLR5 = AllpassCrossover<5>; // 30dB/oct - experimental

} // namespace deluge::dsp::filter
