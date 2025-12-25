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
#include "dsp/fast_math.h"
#include "dsp/filter/ladder_components.h"
#include "util/fixedpoint.h"
#include <array>
#include <cmath>
#include <type_traits>

namespace deluge::dsp::filter {

/// 3-band Linkwitz-Riley crossover filter (LR2 = 12dB/oct slopes).
/// Uses cascaded first-order Butterworth filters.
///
/// LR2 characteristics:
/// - 12dB/oct (40dB/decade) slopes
/// - -6dB at crossover frequency (power-complementary)
/// - Flat summed magnitude response
///
/// Template parameter PHASE_COMPENSATED:
/// - true (default): Adds allpass to LOW band to match MID/HIGH phase.
///   Cost: 6 filter ops. Perfect phase alignment.
/// - false: Skips phase compensation for CPU efficiency.
///   Cost: 4 filter ops. ~90° phase lead in LOW band at high crossover freq.
///   Inaudible for dynamics processing.
template <bool PHASE_COMPENSATED = true>
class LR2Crossover {
public:
	using Bands = CrossoverBands;

	LR2Crossover() = default;

	/// Set the low-mid crossover frequency
	void setLowCrossover(float freqHz) {
		lowCrossoverHz_ = freqHz;
		lowCoeff_ = calculateCoefficient(freqHz);
	}

	/// Set the mid-high crossover frequency
	void setHighCrossover(float freqHz) {
		highCrossoverHz_ = freqHz;
		highCoeff_ = calculateCoefficient(freqHz);
	}

	[[nodiscard]] float getLowCrossoverHz() const { return lowCrossoverHz_; }
	[[nodiscard]] float getHighCrossoverHz() const { return highCrossoverHz_; }

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
	/// Per-channel filter state
	struct ChannelState {
		// Low crossover: two cascaded first-order for LR2
		BasicFilterComponent lpLow1, lpLow2;
		// High crossover: two cascaded first-order for LR2
		BasicFilterComponent lpHigh1, lpHigh2;
		// Phase compensation allpass for LOW band (only if PHASE_COMPENSATED)
		// Conditionally included to save memory when not used
		std::conditional_t<PHASE_COMPENSATED, BasicFilterComponent, char[0]> apComp1;
		std::conditional_t<PHASE_COMPENSATED, BasicFilterComponent, char[0]> apComp2;
	};

	/// Calculate coefficient for first-order Butterworth
	/// Same as allpass: coeff = tan(pi*fc/fs) / (1 + tan(pi*fc/fs))
	[[nodiscard]] static q31_t calculateCoefficient(float freqHz) {
		float fc = freqHz / static_cast<float>(kSampleRate);
		fc = std::clamp(fc, 0.001f, 0.49f);
		float wc = fastTan(3.14159265358979f * fc);
		float coeff = wc / (1.0f + wc);
		return static_cast<q31_t>(coeff * ONE_Q31);
	}

	[[gnu::always_inline]] inline Bands process(q31_t input, ChannelState& state) const {
		// === Low crossover: split into LOW and REST ===
		// LR2 lowpass = cascade of two first-order lowpasses
		q31_t lp1 = state.lpLow1.doFilter(input, lowCoeff_);
		q31_t lowRaw = state.lpLow2.doFilter(lp1, lowCoeff_);

		// LR2 highpass = input - LR2_lowpass
		// Note: This gives exact reconstruction: LOW + REST = input
		q31_t rest = input - lowRaw;

		// === High crossover: split REST into MID and HIGH ===
		q31_t lp3 = state.lpHigh1.doFilter(rest, highCoeff_);
		q31_t mid = state.lpHigh2.doFilter(lp3, highCoeff_);

		// Highpass of REST
		q31_t high = rest - mid;

		// === Phase compensation for LOW band (conditional) ===
		q31_t low;
		if constexpr (PHASE_COMPENSATED) {
			// The MID+HIGH path has phase shift from the high crossover filters.
			// Apply matching allpass to LOW so all bands stay aligned.
			q31_t lowComp1 = state.apComp1.doAPF(lowRaw, highCoeff_);
			low = state.apComp2.doAPF(lowComp1, highCoeff_);
		}
		else {
			// Skip phase compensation for CPU efficiency.
			// LOW has ~90° phase lead vs MID/HIGH at high crossover frequency.
			low = lowRaw;
		}

		return {low, mid, high};
	}

	ChannelState stateL_{};
	ChannelState stateR_{};

	q31_t lowCoeff_ = calculateCoefficient(200.0f);
	q31_t highCoeff_ = calculateCoefficient(2000.0f);

	float lowCrossoverHz_ = 200.0f;
	float highCrossoverHz_ = 2000.0f;
};

// Type aliases for convenience
using LR2CrossoverFull = LR2Crossover<true>;  // With phase compensation (6 filter ops)
using LR2CrossoverFast = LR2Crossover<false>; // Without phase compensation (4 filter ops)

} // namespace deluge::dsp::filter
