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
#include <algorithm>
#include <cstdint>

namespace deluge::dsp {

/**
 * Zone-based parameter with configurable behavior
 *
 * Encapsulates a q31 field value with zone semantics. Knows how to combine
 * with modulation according to its configuration (zone count, clipping).
 *
 * Used by SineTableShaperParams and other zone-based effect parameters.
 * Menu items wrap these params for UI, DSP calls combineWithMod directly.
 *
 * @tparam NUM_ZONES Number of zones (e.g., 8)
 * @tparam CLIP_TO_ZONE If true, modulation clips to zone boundaries
 */
template <int32_t NUM_ZONES = 8, bool CLIP_TO_ZONE = false>
struct ZoneBasedParam {
	q31_t value{0};

	static constexpr int32_t kNumZones = NUM_ZONES;
	static constexpr bool kClipToZone = CLIP_TO_ZONE;
	static constexpr q31_t kZoneWidth = ONE_Q31 / NUM_ZONES;

	/// Get zone index (0 to NUM_ZONES-1)
	[[nodiscard]] int32_t getZoneIndex() const {
		return std::clamp(static_cast<int32_t>(value / kZoneWidth), static_cast<int32_t>(0),
		                  static_cast<int32_t>(NUM_ZONES - 1));
	}

	/// Get position within current zone (0.0 to 1.0)
	[[nodiscard]] float getPosInZone() const {
		int32_t zone = getZoneIndex();
		q31_t zoneStart = zone * kZoneWidth;
		return static_cast<float>(value - zoneStart) / static_cast<float>(kZoneWidth);
	}

	/// Get global position across all zones (0.0 to 1.0)
	[[nodiscard]] float getGlobalPos() const { return static_cast<float>(value) / static_cast<float>(ONE_Q31); }

	/// Combine field value with scaled modulation
	/// @param modulation Raw modulation value (full bipolar range)
	/// @param scaleZones Divide modulation by this (default = kNumZones, so full mod = 1 zone)
	///                   Use 1 to disable scaling (full mod = full range)
	[[nodiscard]] q31_t combineWithMod(q31_t modulation, int32_t scaleZones = kNumZones) const {
		q31_t scaledMod = (scaleZones > 1) ? (modulation / scaleZones) : modulation;
		if constexpr (CLIP_TO_ZONE) {
			int32_t baseZone = getZoneIndex();
			q31_t zoneLower = baseZone * kZoneWidth;
			q31_t zoneUpper = (baseZone == NUM_ZONES - 1) ? ONE_Q31 : (baseZone + 1) * kZoneWidth - 1;
			return std::clamp(value + scaledMod, zoneLower, zoneUpper);
		}
		else {
			return std::clamp(value + scaledMod, static_cast<q31_t>(0), ONE_Q31);
		}
	}

	/// Combine field with preset (gold knob) and cable modulation separately
	/// Preset is scaled but never clipped to zone boundaries (allows automation to cross zones)
	/// Cables are scaled and clipped if CLIP_TO_ZONE is true (prevents LFO glitches)
	/// @param preset Gold knob automation value (scaled, not clipped)
	/// @param cables Mod matrix cable combination (scaled, clipped if CLIP_TO_ZONE)
	/// @param scaleZones Divide both by this (default = kNumZones)
	[[nodiscard]] q31_t combineWithModSeparate(q31_t preset, q31_t cables, int32_t scaleZones = kNumZones) const {
		q31_t scaledPreset = (scaleZones > 1) ? (preset / scaleZones) : preset;
		q31_t scaledCables = (scaleZones > 1) ? (cables / scaleZones) : cables;

		// Base = field + preset (preset can cross zones freely)
		q31_t base = std::clamp(value + scaledPreset, static_cast<q31_t>(0), ONE_Q31);

		if constexpr (CLIP_TO_ZONE) {
			// Clip cables to zone boundaries of the new base position
			int32_t baseZone = std::clamp(static_cast<int32_t>(base / kZoneWidth), static_cast<int32_t>(0),
			                              static_cast<int32_t>(NUM_ZONES - 1));
			q31_t zoneLower = baseZone * kZoneWidth;
			q31_t zoneUpper = (baseZone == NUM_ZONES - 1) ? ONE_Q31 : (baseZone + 1) * kZoneWidth - 1;
			return std::clamp(base + scaledCables, zoneLower, zoneUpper);
		}
		else {
			return std::clamp(base + scaledCables, static_cast<q31_t>(0), ONE_Q31);
		}
	}

	/// Combine preset (base position) with modulation cables
	/// Used by patcher/DSP when preset comes from patched param, not field
	/// Cables are scaled so full modulation = 1 zone, preset is not scaled
	/// If CLIP_TO_ZONE, cables are clipped to zone boundaries (prevents LFO glitches)
	/// @param preset Base position from patched param preset (gold knob/automation)
	/// @param cables Mod matrix cable combination (from patcher, rshift32 scaled)
	/// @return Combined value: preset + scaled cables, with appropriate clipping
	[[nodiscard]] q31_t combinePresetAndCables(q31_t preset, q31_t cables) const {
		// Patcher outputs cables with rshift32 scaling (full mod ≈ 2^30, not 2^31)
		// Divide by (kNumZones/2) so full modulation = 1 zone width
		q31_t scaledCables = cables / (kNumZones / 2);
		// Use 64-bit to avoid overflow when preset + scaledCables exceeds INT32_MAX
		int64_t combined = static_cast<int64_t>(preset) + static_cast<int64_t>(scaledCables);
		if constexpr (CLIP_TO_ZONE) {
			// Clip cables to zone boundaries of the preset position
			int32_t baseZone = std::clamp(static_cast<int32_t>(preset / kZoneWidth), static_cast<int32_t>(0),
			                              static_cast<int32_t>(NUM_ZONES - 1));
			int64_t zoneLower = static_cast<int64_t>(baseZone) * kZoneWidth;
			int64_t zoneUpper = (baseZone == NUM_ZONES - 1) ? ONE_Q31 : (baseZone + 1) * kZoneWidth - 1;
			return static_cast<q31_t>(std::clamp(combined, zoneLower, zoneUpper));
		}
		else {
			return static_cast<q31_t>(std::clamp(combined, static_cast<int64_t>(0), static_cast<int64_t>(ONE_Q31)));
		}
	}

	/// Implicit conversion to q31_t for direct field access
	operator q31_t() const { return value; }
	ZoneBasedParam& operator=(q31_t v) {
		value = v;
		return *this;
	}
};

} // namespace deluge::dsp
