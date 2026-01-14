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

#include "gui/menu_item/zone_based.h"
#include "gui/ui/sound_editor.h"
#include "model/fx/stutterer.h"
#include "modulation/params/param.h"

namespace params = deluge::modulation::params;

namespace deluge::gui::menu_item::stutter {

/**
 * Scatter Zone A - Structural control
 *
 * Zone 0: Drift - Sequential with slight offset
 * Zone 1: Swap - Adjacent pair swapping
 * Zone 2: Retro - Reverse order tendency
 * Zone 3: Leap - Interleaved skipping
 * Zones 4-7: Meta - All structural params via phi triangle evolution
 */
class ScatterZoneA final : public ZoneBasedDualParam<params::GLOBAL_SCATTER_ZONE_A> {
public:
	using ZoneBasedDualParam::ZoneBasedDualParam;

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "Drift";
		case 1:
			return "Swap";
		case 2:
			return "Retro";
		case 3:
			return "Leap";
		case 4:
			return "Meta1";
		case 5:
			return "Meta2";
		case 6:
			return "Meta3";
		case 7:
			return "Meta4";
		default:
			return "?";
		}
	}

	[[nodiscard]] const char* getShortZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "DR";
		case 1:
			return "SW";
		case 2:
			return "RE";
		case 3:
			return "LP";
		default:
			return "MT";
		}
	}
};

/**
 * Scatter Zone B - Timbral control
 *
 * Zone 0: Flip - Reverse probability
 * Zone 1: Filter - Bandpass sweep
 * Zone 2: Echo - Delay feedback
 * Zone 3: Shape - Envelope shaping
 * Zones 4-7: Meta - All timbral params via phi triangle evolution
 */
class ScatterZoneB final : public ZoneBasedDualParam<params::GLOBAL_SCATTER_ZONE_B> {
public:
	using ZoneBasedDualParam::ZoneBasedDualParam;

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "Flip";
		case 1:
			return "Filter";
		case 2:
			return "Echo";
		case 3:
			return "Shape";
		case 4:
			return "Meta1";
		case 5:
			return "Meta2";
		case 6:
			return "Meta3";
		case 7:
			return "Meta4";
		default:
			return "?";
		}
	}

	[[nodiscard]] const char* getShortZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "FL";
		case 1:
			return "FI";
		case 2:
			return "EC";
		case 3:
			return "SH";
		default:
			return "MT";
		}
	}
};

/**
 * Scatter Depth - Effect intensity control
 *
 * Zone 0-3: Subtle (depth=0 is static, no evolution)
 * Zone 4-7: Intense (full phi triangle evolution)
 */
class ScatterDepth final : public ZoneBasedDualParam<params::GLOBAL_SCATTER_DEPTH> {
public:
	using ZoneBasedDualParam::ZoneBasedDualParam;

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "Off";
		case 1:
			return "Light";
		case 2:
			return "Medium";
		case 3:
			return "Heavy";
		case 4:
			return "Deep";
		case 5:
			return "Intense";
		case 6:
			return "Wild";
		case 7:
			return "Chaos";
		default:
			return "?";
		}
	}

	[[nodiscard]] const char* getShortZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "OF";
		case 1:
			return "LT";
		case 2:
			return "MD";
		case 3:
			return "HV";
		case 4:
			return "DP";
		case 5:
			return "IN";
		case 6:
			return "WD";
		case 7:
			return "CH";
		default:
			return "??";
		}
	}
};

} // namespace deluge::gui::menu_item::stutter
