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
 * Used by SineShaperParams and other zone-based effect parameters.
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
		return std::clamp(static_cast<int32_t>(value / kZoneWidth), static_cast<int32_t>(0), NUM_ZONES - 1);
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

	/// Implicit conversion to q31_t for direct field access
	operator q31_t() const { return value; }
	ZoneBasedParam& operator=(q31_t v) {
		value = v;
		return *this;
	}
};

} // namespace deluge::dsp
