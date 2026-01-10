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
 *
 * Design note: shapeX and shapeY are intentionally NOT patched/unpatched params.
 * Unlike sine shaper zones, changing X/Y triggers expensive table regeneration
 * (recomputing all 6 basis function lookup tables). This makes them unsuitable
 * for real-time modulation or automation. Drive and mix ARE patched params since
 * they only affect per-sample gain, not the lookup tables.
 */
struct TableShaperState {
	// User-facing knob values (NOT params - changes trigger expensive table regeneration)
	uint8_t shapeX{0};  // Soft→Hard axis (0-127, "Knee")
	uint16_t shapeY{0}; // Clean→Weird axis (0-1023, high-res multi-zone, "Color")
	bool aa{false};     // Anti-aliasing enabled (default off, reserved for future use)
	float phase{0.0f};  // Phase offset for triangle modulation (secret knob)

	// DSP smoothing state
	q31_t driveLast{0};         // Previous drive value for smoothing
	int32_t mixNormLast_Q16{0}; // Previous mixNorm value for smoothing (Q16.16 format)

	/// Check if effect is enabled (non-zero X)
	/// Note: mix is now a patched param (LOCAL_TABLE_SHAPER_MIX), checked separately at render time
	[[nodiscard]] bool isEnabled() const { return shapeX > 0; }

	/// Reset DSP state (call when starting new audio stream)
	void resetDspState() {
		driveLast = 0;
		mixNormLast_Q16 = 0;
	}

	/// Write shaper state to file (only non-default values)
	void writeToFile(Serializer& writer) const {
		WRITE_FIELD(writer, shapeX, "tableShaperShapeX");
		WRITE_FIELD(writer, shapeY, "tableShaperShapeY");
		if (aa) {
			storage::writeAttributeInt(writer, "tableShaperAA", 1);
		}
		WRITE_FLOAT(writer, phase, "tableShaperPhase", 10.0f);
	}

	/// Read a tag into shaper state, returns true if tag was handled
	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, shapeX, "tableShaperShapeX");
		READ_FIELD(reader, tagName, shapeY, "tableShaperShapeY");
		if (std::strcmp(tagName, "tableShaperAA") == 0) {
			aa = storage::readAndExitTag(reader, "tableShaperAA") != 0;
			return true;
		}
		READ_FLOAT(reader, tagName, phase, "tableShaperPhase", 10.0f);
		return false;
	}
};

/**
 * Table Shaper using table-based waveshaping with XY control
 *
 * Features:
 * - X/Y shape control for creative distortion curves
 * - Integer-only processing path for efficiency
 * - Double-buffered tables with IIR crossfade for click-free updates
 *
 * Shape X (0-127): Soft→hard clipping curve (UI: "Knee")
 * Shape Y (0-1023): Saturation character/color (UI: "Color")
 */
class TableShaper {
public:
	TableShaper() { regenerateTable(0, 0); }

	/**
	 * Regenerate the waveshaping tables based on shape parameters
	 * Call this when shapeX or shapeY changes (not during audio processing)
	 * @param shapeX Controls waveshaping intensity (0-127)
	 * @param shapeY Sweeps through combinatoric blend (0-1023, high-res)
	 * @param phaseOffset Phase offset for triangle modulation (from secret knob)
	 */
	void regenerateTable(uint8_t shapeX, uint16_t shapeY, float phaseOffset = 0.0f) {
		if (phaseOffset != 0.0f) {
			tableSat_.setParameters(TableShaperXYMapper::deriveParametersWithPhase(shapeX, shapeY, phaseOffset, 1.0f));
		}
		else {
			tableSat_.setParameters(TableShaperXYMapper::deriveParameters(shapeX, shapeY));
		}
	}

	/// Call from non-audio context to regenerate tables if params changed
	void regenerateIfDirty() { tableSat_.regenerateIfDirty(); }

	/// Pre-allocate buffers from UI thread (call before scheduling deferred regeneration)
	void ensureBuffersAllocated() { tableSat_.ensureBuffersAllocated(); }

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
	/// Convert hybrid param output to mixNorm - call once per buffer, not per sample
	/// Input: hybrid param after getFinalParameterValueHybrid() - range [-1073741824, +1073741824]
	/// Maps full range linearly to [0, 131072] (0 to 2.0 in Q16)
	/// Returns Q16.16 fixed-point: 65536 = 1.0, 131072 = 2.0 (full wet)
	[[gnu::always_inline]] static inline int32_t mixParamToNormQ16(int32_t mix) {
		constexpr int32_t kHybridMin = -1073741824;

		// Early exit for bypass (at or below minimum)
		if (mix <= kHybridMin) {
			return 0;
		}

		// Linear mapping: [-1073741824, +1073741824] → [0, 131072]
		// offset = mix + 1073741824, range [0, 2147483648]
		// mixNorm_Q16 = offset >> 14 (divides by 16384, maps to [0, 131072])
		uint32_t offset = static_cast<uint32_t>(mix - kHybridMin);
		int32_t mixNorm_Q16 = static_cast<int32_t>(offset >> 14);

		// Clamp to max (safety for modulation overflow)
		if (mixNorm_Q16 > 131072) {
			mixNorm_Q16 = 131072;
		}
		return mixNorm_Q16;
	}

	/// Convert drive parameter to Q26 gain using power curve (call once per buffer, not per sample)
	/// Power curve: gain = 32 * p^5 where p = normalized drive position [0, 1]
	/// - min (-2^30): 0x (silence)
	/// - center (0): 1.0x (unity)
	/// - max (+2^30): 32x (full Q26 range)
	[[gnu::always_inline]] static inline int32_t driveToGainQ26(q31_t drive) {
		// Convert bipolar drive to unipolar p ∈ [0, 2^30] (Q30)
		uint32_t p_Q30 = static_cast<uint32_t>((drive >> 1) + (1 << 29));
		// Compute p^5 using repeated squaring (all intermediate values in Q30)
		uint64_t p2 = (static_cast<uint64_t>(p_Q30) * p_Q30) >> 30;
		uint64_t p4 = (p2 * p2) >> 30;
		uint64_t p5 = (p4 * p_Q30) >> 30;
		// gain = 32 * p^5 in Q26 (clamp to INT32_MAX at exactly p=1)
		return (p5 >= (1ULL << 30)) ? INT32_MAX : static_cast<int32_t>(p5 << 1);
	}

	/// Process with pre-computed driveGain (preferred - hoist gain calculation out of sample loop)
	[[gnu::always_inline]] inline q31_t processWithGain(q31_t input, int32_t driveGain_Q26,
	                                                    int32_t mixNorm_Q16 = 131072) {
		return tableSat_.processInt32Q16(input, driveGain_Q26, mixNorm_Q16);
	}

	/// Process with integer mixNorm (Q16.16 format: 65536 = 1.0)
	/// Note: Prefer processWithGain() and driveToGainQ26() for buffer processing
	[[gnu::always_inline]] inline q31_t processInt32(q31_t input, q31_t drive, int32_t mixNorm_Q16 = 131072) {
		return tableSat_.processInt32Q16(input, driveToGainQ26(drive), mixNorm_Q16);
	}

	/// Check if effect is transparent (zero drive in waveshaper)
	[[nodiscard]] bool isTransparent() const { return tableSat_.isLinear(); }

	/// Get the table shaper core for direct parameter access
	[[nodiscard]] TableShaperCore& getTableShaperCore() { return tableSat_; }
	[[nodiscard]] const TableShaperCore& getTableShaperCore() const { return tableSat_; }

private:
	// Table-based shaper with cached waveshaping (shared for L/R)
	TableShaperCore tableSat_;
};

} // namespace deluge::dsp
