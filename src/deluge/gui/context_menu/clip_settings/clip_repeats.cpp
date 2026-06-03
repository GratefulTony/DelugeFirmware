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
#include <iterator>

namespace deluge::gui::context_menu::clip_settings {

struct RepeatsOption {
	char const* label;
	uint8_t value;
};

constexpr RepeatsOption kRepeatsOptions[] = {
    {"Inf", 0}, {"1", 1}, {"2", 2}, {"4", 4}, {"8", 8}, {"16", 16},
};
constexpr size_t kNumValues = std::size(kRepeatsOptions);

ClipRepeatsMenu clipRepeats{};

char const* ClipRepeatsMenu::getTitle() {
	using enum l10n::String;
	return l10n::get(STRING_FOR_CLIP_REPEATS);
}

std::span<char const*> ClipRepeatsMenu::getOptions() {
	static const char* options[] = {
	    kRepeatsOptions[0].label, kRepeatsOptions[1].label, kRepeatsOptions[2].label,
	    kRepeatsOptions[3].label, kRepeatsOptions[4].label, kRepeatsOptions[5].label,
	};
	return {options, kNumValues};
}

bool ClipRepeatsMenu::setupAndCheckAvailability() {
	currentUIMode = UI_MODE_NONE;

	this->currentOption = 0; // default to Inf for unknown stored values
	for (size_t i = 0; i < kNumValues; i++) {
		if (kRepeatsOptions[i].value == clip->clipRepeats) {
			this->currentOption = static_cast<int32_t>(i);
			break;
		}
	}

	if (display->haveOLED()) {
		scrollPos = this->currentOption;
	}

	return true;
}

void ClipRepeatsMenu::selectEncoderAction(int8_t offset) {
	ContextMenu::selectEncoderAction(offset);
	clip->clipRepeats = kRepeatsOptions[this->currentOption].value;
}

} // namespace deluge::gui::context_menu::clip_settings
