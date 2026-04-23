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

#include "gui/context_menu/clip_settings/next_action.h"
#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/ui/root_ui.h"
#include "hid/display/display.h"
#include "model/clip/clip.h"
#include <cstddef>

namespace deluge::gui::context_menu::clip_settings {

constexpr size_t kNumValues = 5;

NextActionMenu nextAction{};

char const* NextActionMenu::getTitle() {
	using enum l10n::String;
	return l10n::get(STRING_FOR_NEXT_ACTION);
}

std::span<char const*> NextActionMenu::getOptions() {
	using enum l10n::String;
	static const char* optionsls[] = {
	    l10n::get(STRING_FOR_NEXT_ACTION_STOP),        l10n::get(STRING_FOR_NEXT_ACTION_NEXT),
	    l10n::get(STRING_FOR_NEXT_ACTION_PREV),        l10n::get(STRING_FOR_NEXT_ACTION_RANDOM),
	    l10n::get(STRING_FOR_NEXT_ACTION_RANDOM_WALK),
	};
	return {optionsls, kNumValues};
}

bool NextActionMenu::setupAndCheckAvailability() {
	currentUIMode = UI_MODE_NONE;
	this->currentOption = static_cast<int32_t>(clip->nextAction);

	if (display->haveOLED()) {
		scrollPos = this->currentOption;
	}

	return true;
}

void NextActionMenu::selectEncoderAction(int8_t offset) {
	ContextMenu::selectEncoderAction(offset);
	clip->nextAction = static_cast<NextAction>(this->currentOption);
}

} // namespace deluge::gui::context_menu::clip_settings
