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
#include "storage/field_serialization.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace deluge::dsp {

/**
 * User-facing parameters and DSP state for the table shaper
 *
 * Consolidates all shaper-related fields that were scattered in ModControllableAudio.
 * The TableShaper class handles the actual DSP processing with lookup tables.
 */
struct ShaperState {
	// User-facing knob values
	uint8_t drive{0};   // Input gain / saturation amount (0-127)
	uint8_t shapeX{0};  // Soft→Hard axis (0-127, "Knee")
	uint16_t shapeY{0}; // Clean→Weird axis (0-1023, high-res multi-zone, "Color")
	uint8_t mix{0};     // Wet/dry blend (0 = bypass)
	bool aa{false};     // Anti-aliasing enabled (default off, reserved for future use)
	float phase{0.0f};  // Phase offset for triangle modulation (secret knob)

	// DSP smoothing/filter state
	q31_t driveLast{0};           // Previous drive value for smoothing
	int32_t mixNormLast_Q16{0};   // Previous mixNorm value for smoothing (Q16.16 format)
	q31_t filterL{0};             // Post-saturation lowpass state L
	q31_t filterR{0};             // Post-saturation lowpass state R
	float prevXL{0.0f};           // ADAA state L (previous input sample)
	float prevXR{0.0f};           // ADAA state R (previous input sample)
	float smoothedNormGain{1.0f}; // Smoothed normalization gain (tracks table's normalizationGain_)

	/// Check if effect is enabled (non-zero X and mix)
	[[nodiscard]] bool isEnabled() const { return shapeX > 0 && mix > 0; }

	/// Reset DSP state (call when starting new audio stream)
	void resetDspState() {
		driveLast = 0;
		mixNormLast_Q16 = 0;
		filterL = 0;
		filterR = 0;
		prevXL = 0.0f;
		prevXR = 0.0f;
	}

	/// Write shaper state to file (only non-default values)
	void writeToFile(Serializer& writer) const {
		WRITE_FIELD(writer, shapeX, "shaperShapeX");
		WRITE_FIELD(writer, shapeY, "shaperShapeY");
		WRITE_FIELD(writer, mix, "shaperMix");
		if (aa) {
			storage::writeAttributeInt(writer, "shaperAA", 1);
		}
		WRITE_FLOAT(writer, phase, "shaperPhase", 10.0f);
	}

	/// Read a tag into shaper state, returns true if tag was handled
	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, shapeX, "shaperShapeX");
		READ_FIELD(reader, tagName, shapeY, "shaperShapeY");
		READ_FIELD(reader, tagName, mix, "shaperMix");
		if (std::strcmp(tagName, "shaperAA") == 0) {
			aa = storage::readAndExitTag(reader, "shaperAA") != 0;
			return true;
		}
		READ_FLOAT(reader, tagName, phase, "shaperPhase", 10.0f);
		return false;
	}
};

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

	// TODO: Remove float process() - not used, processInt32 is the intended signal path
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
	 * @param input Sample to process (q31) - should be at FM operating level
	 *              For subtractive mode, caller should pre-boost input and post-attenuate output
	 * @param drive Input gain (q31 from hybrid-type patched param)
	 * @param mix Volume param output (0 = full dry, INT32_MAX = full wet)
	 * @return Shaped sample (q31)
	 *
	 * Table is normalized for FM signal levels (~23M peak). Subtractive signals
	 * should be boosted before processing to ensure full table utilization.
	 */
	/// Convert volume param output to mixNorm - call once per buffer, not per sample
	/// Returns Q16.16 fixed-point: 65536 = 1.0, max ~131072 for mixNorm=2
	[[gnu::always_inline]] static inline int32_t mixParamToNormQ16(int32_t mix) {
		if (mix <= 0) {
			return 0;
		}
		// pow(x, 0.7) = exp(0.7 * log(x)) using fast math
		float normalized = static_cast<float>(mix) / static_cast<float>(1 << 30);
		float mixNorm = fastExp(0.7f * fastLog(normalized)) * 2.0f;
		return static_cast<int32_t>(mixNorm * 65536.0f);
	}

	/// Process with integer mixNorm (Q16.16 format: 65536 = 1.0)
	[[gnu::always_inline]] inline q31_t processInt32(q31_t input, q31_t drive, int32_t mixNorm_Q16 = 131072) {
		// Symmetric drive: 0.25x at min, 1.0x at center, 1.75x at max
		// drive range [-2^30, 2^30] maps to gain [0.25x, 1.75x]
		// Scale factor: 0.75 = 3/4 = 1/2 + 1/4
		constexpr int32_t kOne_Q30 = 1 << 30;
		int32_t scaledDrive = (drive >> 1) + (drive >> 2);
		int32_t driveGain_Q30 = kOne_Q30 + scaledDrive;

		return tableSat_.processInt32Q16(input, driveGain_Q30, mixNorm_Q16);
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
