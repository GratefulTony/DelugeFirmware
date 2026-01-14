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

/// Scatter gate parameter - controls duty cycle/gate amount
class ScatterGate final : public UnpatchedParam {
public:
	ScatterGate(l10n::String newName, l10n::String title)
	    : UnpatchedParam(newName, title, deluge::modulation::params::UNPATCHED_SCATTER_GATE) {}

	void getColumnLabel(StringBuf& label) override { label.append("Gate"); }
};

} // namespace deluge::gui::menu_item::stutter
