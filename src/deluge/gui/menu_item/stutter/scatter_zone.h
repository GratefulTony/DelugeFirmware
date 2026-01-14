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
#include "gui/menu_item/unpatched_param.h"
#include "gui/ui/sound_editor.h"
#include "model/fx/stutterer.h"
#include "modulation/params/param.h"

namespace deluge::gui::menu_item::stutter {

/// Zone A parameter with mode-dependent label
class ScatterZoneA final : public UnpatchedParam {
public:
	ScatterZoneA(l10n::String newName, l10n::String title)
	    : UnpatchedParam(newName, title, deluge::modulation::params::UNPATCHED_SCATTER_ZONE_A) {}

	void getColumnLabel(StringBuf& label) override {
		auto mode = soundEditor.currentModControllable->stutterConfig.scatterMode;
		switch (mode) {
		case ScatterMode::Classic:
			label.append("-");
			break;
		case ScatterMode::Repeat:
			label.append("Count");
			break;
		case ScatterMode::Reverse:
			label.append("Start");
			break;
		case ScatterMode::Chop:
			label.append("Slices");
			break;
		case ScatterMode::Shuffle:
			label.append("Range");
			break;
		case ScatterMode::Tape:
			label.append("Speed");
			break;
		case ScatterMode::Pitch:
			label.append("Semi");
			break;
		case ScatterMode::Filter:
			label.append("Freq");
			break;
		default:
			label.append("ZoneA");
			break;
		}
	}
};

/// Zone B parameter with mode-dependent label
class ScatterZoneB final : public UnpatchedParam {
public:
	ScatterZoneB(l10n::String newName, l10n::String title)
	    : UnpatchedParam(newName, title, deluge::modulation::params::UNPATCHED_SCATTER_ZONE_B) {}

	void getColumnLabel(StringBuf& label) override {
		auto mode = soundEditor.currentModControllable->stutterConfig.scatterMode;
		switch (mode) {
		case ScatterMode::Classic:
			label.append("-");
			break;
		case ScatterMode::Repeat:
			label.append("Decay");
			break;
		case ScatterMode::Reverse:
			label.append("End");
			break;
		case ScatterMode::Chop:
			label.append("Swing");
			break;
		case ScatterMode::Shuffle:
			label.append("Chaos");
			break;
		case ScatterMode::Tape:
			label.append("Accel");
			break;
		case ScatterMode::Pitch:
			label.append("Glide");
			break;
		case ScatterMode::Filter:
			label.append("Reso");
			break;
		default:
			label.append("ZoneB");
			break;
		}
	}
};

} // namespace deluge::gui::menu_item::stutter
