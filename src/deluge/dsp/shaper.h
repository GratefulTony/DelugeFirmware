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

#include "deluge/dsp/table_shaper.h"
#include "deluge/util/fixedpoint.h"
#include "dsp_ng/core/types.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace deluge::dsp {

/**
 * Table Shaper using table-based waveshaping with XY control
 *
 * Features:
 * - X/Y shape control for creative distortion curves
 * - Table-based parametric shaper with optional ADAA
 * - Gain-compensated drive for predictable unity at 12 o'clock
 *
 * Shape X (0-127): Soft→hard clipping curve (UI: "Knee")
 * Shape Y (0-1023): Saturation character/color (UI: "Color")
 */
class TableShaper {
public:
	// Precomputed constants - eliminates per-sample divisions
	// EFFECTIVE_0DBFS_Q31 is ~23.7 million - the expected signal level
	static constexpr float kEffective0dBFS = 23726566.0f;
	static constexpr float kInv0dBFS = 1.0f / kEffective0dBFS;

	// Pre-gain to push signal into saturation range earlier (+9dB = 2 * sqrt(2))
	static constexpr float kPreGain = 2.0f * 1.41421356f;
	// Post-gain: compensate for pre-gain and add 2x for LPF removal (-3dB = 1 / sqrt(2))
	static constexpr float kPostGain = 0.70710678f;

	// Drive: hybrid param output range is [-1073741824, 1073741823] (half of INT32)
	// Linear mapping: min → 0 (silence), center (0) → 1 (unity), max → 2 (+6dB)
	// driveGain = (drive + kHybridParamMax) / (2 * kHybridParamMax) * 2
	//           = drive / kHybridParamMax + 1.0
	static constexpr float kHybridParamMax = 1073741824.0f;
	static constexpr float kInvHybridParamMax = 1.0f / kHybridParamMax;

	// Combined scales for efficient per-sample processing
	static constexpr float kInputScale = kInv0dBFS * kPreGain;         // Input: q31 → normalized with pre-gain
	static constexpr float kOutputScale = kPostGain * kEffective0dBFS; // Output: normalized → q31

	TableShaper() { regenerateTable(0, 0); }

	/**
	 * Regenerate the waveshaping tables based on shape parameters
	 * Call this when shapeX or shapeY changes (not during audio processing)
	 * @param shapeX Controls waveshaping intensity (0-127)
	 * @param shapeY Sweeps through combinatoric blend (0-1023, high-res)
	 * @param phaseOffset Phase offset for triangle modulation (from secret knob)
	 */
	void regenerateTable(uint8_t shapeX, uint16_t shapeY, float phaseOffset = 0.0f) {
		shapeX_ = shapeX;
		shapeY_ = shapeY;
		if (phaseOffset != 0.0f) {
			tableSat_.setParameters(TableShaperXYMapper::deriveParametersWithPhase(shapeX, shapeY, phaseOffset, 1.0f));
		}
		else {
			tableSat_.setParameters(TableShaperXYMapper::deriveParameters(shapeX, shapeY));
		}
	}

	/**
	 * Process a single sample through the shaper (optimized, no divisions)
	 * @param input Sample to process (q31)
	 * @param drive Input gain (q31 from hybrid-type patched param, bipolar additive modulation)
	 * @param prevX Pointer to ADAA state (previous input sample), nullptr if AA disabled
	 * @return Shaped sample (q31)
	 */
	[[gnu::always_inline]] inline q31_t process(q31_t input, q31_t drive, float* prevX = nullptr) {
		float driveGain = static_cast<float>(drive) * kInvHybridParamMax + 1.0f;
		float inputF = std::clamp(static_cast<float>(input) * kInputScale * driveGain, -1.0f, 1.0f);
		float outputF = prevX ? tableSat_.process(inputF, prevX) : tableSat_.processNoAA(inputF);
		return static_cast<q31_t>(outputF * kOutputScale);
	}

	/**
	 * Process a single sample using integer-only path (like builtin, no floats)
	 * @param input Sample to process (q31)
	 * @param drive Input gain (q31 from hybrid-type patched param)
	 * @return Shaped sample (q31)
	 */
	[[gnu::always_inline]] inline q31_t processInt32(q31_t input, q31_t drive) {
		// Asymmetric drive: 0.0625x at min, 1.0x at center, 2.0x at max
		constexpr int32_t kOne_Q30 = 1 << 30;
		int32_t driveGain_Q30 = (drive < 0) ? kOne_Q30 + drive - (drive >> 4) : kOne_Q30 + drive;

		int32_t afterDrive = multiply_32x32_rshift32(input, driveGain_Q30) << 2;
		int32_t scaledInput = lshiftAndSaturate<8>(afterDrive);
		uint32_t tableInput = static_cast<uint32_t>(scaledInput) + 2147483648u;
		return tableSat_.processNoAAInt32(tableInput) >> 8;
	}

	/// Check if effect is transparent (zero drive in waveshaper)
	[[nodiscard]] bool isTransparent() const { return tableSat_.isLinear(); }

	/// Get the table shaper core for direct parameter access
	[[nodiscard]] TableShaperCore& getTableShaperCore() { return tableSat_; }
	[[nodiscard]] const TableShaperCore& getTableShaperCore() const { return tableSat_; }

	/// Reset table shaper state (call when shape parameters change)
	void resetTableState() { tableSat_.reset(); }

	[[nodiscard]] uint8_t getShapeX() const { return shapeX_; }
	[[nodiscard]] uint16_t getShapeY() const { return shapeY_; }

private:
	uint8_t shapeX_{0};
	uint16_t shapeY_{0};

	// Table-based shaper with cached waveshaping (shared for L/R)
	TableShaperCore tableSat_;
};

} // namespace deluge::dsp
