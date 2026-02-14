/*
 * Copyright © 2024 Synthstrom Audible Limited
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
 */

#pragma once

#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/selection.h"
#include "storage/flash_storage.h"

namespace deluge::gui::menu_item {

class SelectRecordSource final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override {
		auto current = FlashStorage::defaultRecordSource;
		for (int32_t i = 0; i < static_cast<int32_t>(std::size(kSourceOptions)); i++) {
			if (kSourceOptions[i] == current) {
				this->setValue(i);
				return;
			}
		}
		this->setValue(0);
	}

	void writeCurrentValue() override {
		int32_t idx = this->getValue();
		if (idx >= 0 && idx < static_cast<int32_t>(std::size(kSourceOptions))) {
			FlashStorage::defaultRecordSource = kSourceOptions[idx];
		}
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {
		    l10n::getView(STRING_FOR_LEFT_INPUT),     l10n::getView(STRING_FOR_STEREO_INPUT),
		    l10n::getView(STRING_FOR_BALANCED_INPUT), l10n::getView(STRING_FOR_MIX_PRE_FX),
		    l10n::getView(STRING_FOR_MIX_POST_FX),
		};
	}

private:
	static constexpr AudioInputChannel kSourceOptions[] = {
	    AudioInputChannel::LEFT, AudioInputChannel::STEREO, AudioInputChannel::BALANCED,
	    AudioInputChannel::MIX,  AudioInputChannel::OUTPUT,
	};
};

} // namespace deluge::gui::menu_item
