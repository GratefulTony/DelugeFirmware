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
#include <cmath>

namespace deluge::dsp::filter {

/// A 3-band allpass crossover filter using 12dB/oct Linkwitz-Riley slopes.
/// Uses cascaded first-order allpass filters for perfect phase-coherent reconstruction.
/// The bands sum back to the original signal with no phase distortion.
class AllpassCrossover {
public:
	/// Output structure for the three frequency bands
	struct Bands {
		q31_t low;
		q31_t mid;
		q31_t high;
	};

	/// Per-channel filter state for the cascaded allpass filters
	struct ChannelState {
		BasicFilterComponent apLow1;  // First stage low crossover
		BasicFilterComponent apLow2;  // Second stage low crossover (for LR2)
		BasicFilterComponent apHigh1; // First stage high crossover
		BasicFilterComponent apHigh2; // Second stage high crossover (for LR2)
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
	/// Uses 12dB/oct (LR2) slopes via cascaded allpass filters.
	///
	/// @param input The input sample
	/// @param state Reference to the filter state (use stateL_ or stateR_)
	/// @return The three frequency bands that sum to the original input
	[[gnu::always_inline]] inline Bands process(q31_t input, ChannelState& state) const {
		// First stage allpass for each crossover (6dB/oct)
		q31_t apLow1 = state.apLow1.doAPF(input, lowCoeff_);
		q31_t apHigh1 = state.apHigh1.doAPF(input, highCoeff_);

		// Second stage allpass for 12dB/oct LR2 response
		q31_t apLow = state.apLow2.doAPF(apLow1, lowCoeff_);
		q31_t apHigh = state.apHigh2.doAPF(apHigh1, highCoeff_);

		// Derive the three bands with perfect reconstruction:
		// low + mid + high = input (mathematically exact)
		Bands bands;
		bands.low = (input + apLow) >> 1;
		bands.high = (input - apHigh) >> 1;
		bands.mid = (apHigh - apLow) >> 1;

		return bands;
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
	/// Uses the formula: coeff = tan(pi * f / fs) / (1 + tan(pi * f / fs))
	/// which maps to the "moveability" parameter of BasicFilterComponent::doAPF
	[[nodiscard]] static q31_t calculateCoefficient(float freqHz) {
		float fc = freqHz / static_cast<float>(kSampleRate);
		// Clamp to valid range to prevent instability
		fc = std::clamp(fc, 0.001f, 0.49f);
		float wc = std::tan(3.14159265358979f * fc);
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

} // namespace deluge::dsp::filter
