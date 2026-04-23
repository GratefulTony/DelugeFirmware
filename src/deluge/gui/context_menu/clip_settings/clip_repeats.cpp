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

#include "gui/context_menu/clip_settings/clip_repeats.h"
#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/ui/root_ui.h"
#include "hid/display/display.h"
#include "model/clip/clip.h"
#include <cstddef>

namespace deluge::gui::context_menu::clip_settings {

constexpr size_t kNumValues = 6;

ClipRepeatsMenu clipRepeats{};

char const* ClipRepeatsMenu::getTitle() {
	using enum l10n::String;
	return l10n::get(STRING_FOR_CLIP_REPEATS);
}

std::span<char const*> ClipRepeatsMenu::getOptions() {
	static const char* optionsls[] = {
	    "Inf", "1", "2", "4", "8", "16",
	};
	return {optionsls, kNumValues};
}

bool ClipRepeatsMenu::setupAndCheckAvailability() {
	currentUIMode = UI_MODE_NONE;

	switch (clip->clipRepeats) {
	case 0:
		this->currentOption = 0;
		break;
	case 1:
		this->currentOption = 1;
		break;
	case 2:
		this->currentOption = 2;
		break;
	case 4:
		this->currentOption = 3;
		break;
	case 8:
		this->currentOption = 4;
		break;
	case 16:
		this->currentOption = 5;
		break;
	default:
		this->currentOption = 0;
		break;
	}

	if (display->haveOLED()) {
		scrollPos = this->currentOption;
	}

	return true;
}

void ClipRepeatsMenu::selectEncoderAction(int8_t offset) {
	ContextMenu::selectEncoderAction(offset);

	static constexpr uint8_t kIndexToRepeats[] = {0, 1, 2, 4, 8, 16};
	clip->clipRepeats = kIndexToRepeats[this->currentOption];
}

} // namespace deluge::gui::context_menu::clip_settings
