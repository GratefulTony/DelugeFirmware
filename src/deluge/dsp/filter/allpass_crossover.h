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
/// Template parameter ORDER (only ORDER=1 is recommended for 3-band):
template <int ORDER>
class AllpassCrossover {
	static_assert(ORDER == 1 || ORDER == 3 || ORDER == 5, "ORDER must be odd (1, 3, or 5)");

public:
	using Bands = CrossoverBands;

	/// Per-channel filter state for the cascaded allpass filters
	struct ChannelState {
		std::array<BasicFilterComponent, ORDER> apLow;  // Low crossover stages
		std::array<BasicFilterComponent, ORDER> apHigh; // High crossover stages
	};

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

	/// Process a single mono sample and return the three frequency bands.
	///
	/// @param input The input sample
	/// @param state Reference to the filter state (use stateL_ or stateR_)
	/// @return The three frequency bands
	[[gnu::always_inline]] inline Bands process(q31_t input, ChannelState& state) const {
		// Step 1: Low crossover splits input into LOW and REST
		q31_t apLow = input;
		for (int i = 0; i < ORDER; ++i) {
			apLow = state.apLow[i].doAPF(apLow, lowCoeff_);
		}
		q31_t low = (input + apLow) >> 1;  // Lowpass
		q31_t rest = (input - apLow) >> 1; // Highpass (everything above low crossover)

		// Step 2: High crossover splits REST into MID and HIGH
		q31_t apHigh = rest;
		for (int i = 0; i < ORDER; ++i) {
			apHigh = state.apHigh[i].doAPF(apHigh, highCoeff_);
		}
		q31_t mid = (rest + apHigh) >> 1;  // Lowpass of REST = bandpass overall
		q31_t high = (rest - apHigh) >> 1; // Highpass of REST = highpass overall

		return {low, mid, high};
	}

	/// Process a stereo sample pair
	[[gnu::always_inline]] inline void processStereo(q31_t inputL, q31_t inputR, Bands& outL, Bands& outR) {
		outL = process(inputL, stateL_);
		outR = process(inputR, stateR_);
	}

	/// Reset all filter states
	void reset() {
		stateL_ = {};
		stateR_ = {};
	}

private:
	/// Calculate the allpass coefficient for a given frequency.
	/// For BasicFilterComponent::doAPF(): coeff = tan(pi * fc / fs) / (1 + tan(pi * fc / fs))
	[[nodiscard]] static q31_t calculateCoefficient(float freqHz) {
		float fc = freqHz / static_cast<float>(kSampleRate);
		// Clamp to valid range to prevent instability
		fc = std::clamp(fc, 0.001f, 0.49f);
		float wc = std::tan(3.14159265358979f * fc);
		// Coefficient for BasicFilterComponent::doAPF()
		float coeff = wc / (1.0f + wc);
		return static_cast<q31_t>(coeff * ONE_Q31);
	}

	ChannelState stateL_{};
	ChannelState stateR_{};

	q31_t lowCoeff_ = calculateCoefficient(200.0f);   // Default 200Hz
	q31_t highCoeff_ = calculateCoefficient(2000.0f); // Default 2kHz

	float lowCrossoverHz_ = 200.0f;
	float highCrossoverHz_ = 2000.0f;
};

// Type aliases for convenience (odd orders only - see class documentation)
using AllpassCrossoverLR1 = AllpassCrossover<1>; // 6dB/oct
using AllpassCrossoverLR3 = AllpassCrossover<3>; // 18dB/oct
using AllpassCrossoverLR5 = AllpassCrossover<5>; // 30dB/oct

} // namespace deluge::dsp::filter
