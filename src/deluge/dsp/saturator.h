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

#include "deluge/dsp/analytic_saturator.h"
#include "deluge/util/fixedpoint.h"
#include "dsp_ng/core/types.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace deluge::dsp {

/**
 * XY Saturator using analytic waveshaping with table-based lookup
 *
 * Features:
 * - X/Y shape control for creative distortion curves
 * - Analytic parametric saturator with optional ADAA
 * - Gain-compensated drive for predictable unity at 12 o'clock
 *
 * Shape X (0-127): Controls waveshaping intensity/drive
 * Shape Y (0-1023): Sweeps through combinatoric blend of basis functions (high-res multi-zone)
 */
class Saturator {
public:
	Saturator() { regenerateTable(0, 0); }

	/**
	 * Regenerate the waveshaping tables based on shape parameters
	 * Call this when shapeX or shapeY changes (not during audio processing)
	 * @param shapeX Controls waveshaping intensity (0-127)
	 * @param shapeY Sweeps through combinatoric blend (0-1023, high-res)
	 */
	void regenerateTable(uint8_t shapeX, uint16_t shapeY) {
		shapeX_ = shapeX;
		shapeY_ = shapeY;

		// Derive analytic parameters from X and Y
		float drive, tanhWeight, polyWeight, hardKneeWeight, chebyWeight, sineFoldWeight, rectifierWeight;
		float threshold, asymmetry;
		AnalyticSaturatorXYMapper::deriveParameters(shapeX, shapeY, drive, tanhWeight, polyWeight, hardKneeWeight,
		                                            chebyWeight, sineFoldWeight, rectifierWeight, threshold, asymmetry);

		// Set parameters on saturator (tables shared, state is per-channel)
		analyticSat_.setParameters(drive, tanhWeight, polyWeight, hardKneeWeight, chebyWeight, sineFoldWeight,
		                           rectifierWeight, threshold, asymmetry);
	}

	/**
	 * Process a single sample through the saturator
	 * @param input Sample to process (q31)
	 * @param drive Input gain (q31 from hybrid-type patched param, bipolar additive modulation)
	 * @param prevX Pointer to ADAA state (previous input sample), nullptr if AA disabled
	 * @return Shaped sample (q31)
	 */
	[[gnu::always_inline]] inline q31_t process(q31_t input, q31_t drive, float* prevX = nullptr) {
		// EFFECTIVE_0DBFS_Q31 is ~23.7 million - this is the expected signal level
		constexpr float kEffective0dBFS = 23726566.0f;

		// Pre-gain to push signal into saturation range earlier (+9dB = 2 * sqrt(2))
		constexpr float kPreGain = 2.0f * 1.41421356f;
		// Post-gain: compensate for pre-gain and add 2x for LPF removal (-3dB = 1 / sqrt(2))
		constexpr float kPostGain = 1.0f * 0.70710678f;

		// Drive: hybrid param output range is [-1073741824, 1073741823] (half of INT32)
		// due to getFinalParameterValueHybrid dividing by 4 and saturating to 29 bits
		// Linear mapping: min → 0 (silence), center (0) → 1 (unity), max → 2 (+6dB)
		constexpr float kHybridParamMax = 1073741824.0f;
		float normalizedDrive = (static_cast<float>(drive) + kHybridParamMax) / (2.0f * kHybridParamMax);
		float driveGain = normalizedDrive * 2.0f;

		// Convert q31 to float, normalized to 0dBFS reference
		float inputF = static_cast<float>(input) / kEffective0dBFS;

		// Apply pre-gain and drive to INPUT before waveshaping
		inputF *= kPreGain * driveGain;

		// Clamp input to [-1, 1] for the saturator
		inputF = std::clamp(inputF, -1.0f, 1.0f);

		// Process with optional ADAA (anti-derivative anti-aliasing)
		float outputF = prevX ? analyticSat_.process(inputF, prevX) : analyticSat_.processNoAA(inputF);

		// Apply post-attenuation and scale back to 0dBFS reference level
		outputF *= kPostGain * kEffective0dBFS;

		// Convert back to q31
		return static_cast<q31_t>(outputF);
	}

	/// Check if effect is transparent (zero drive in waveshaper)
	[[nodiscard]] bool isTransparent() const { return analyticSat_.isLinear(); }

	/// Get the analytic saturator for direct parameter access
	[[nodiscard]] AnalyticSaturator& getAnalyticSaturator() { return analyticSat_; }
	[[nodiscard]] const AnalyticSaturator& getAnalyticSaturator() const { return analyticSat_; }

	/// Reset analytic saturator tables (call when shape parameters change)
	void resetAnalyticState() { analyticSat_.reset(); }

	[[nodiscard]] uint8_t getShapeX() const { return shapeX_; }
	[[nodiscard]] uint16_t getShapeY() const { return shapeY_; }

private:
	uint8_t shapeX_{0};
	uint16_t shapeY_{0};

	// Analytic saturator with table-based waveshaping (shared for L/R)
	AnalyticSaturator analyticSat_;
};

} // namespace deluge::dsp
