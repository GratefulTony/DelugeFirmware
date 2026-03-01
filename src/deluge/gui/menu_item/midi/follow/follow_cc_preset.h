/*
 * Copyright (c) 2024 Owlet Records
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
#include "ff.h"
#include "gui/menu_item/selection.h"
#include "hid/display/display.h"
#include "io/midi/midi_follow.h"
#include "util/d_string.h"
#include <cstring>

namespace deluge::gui::menu_item::midi {

/// Menu item for selecting a CC mapping preset from SETTINGS/MIDIFollow/ directory.
/// Scans directory on entry, encoder scrolls through available presets,
/// select button loads the chosen preset.
class FollowCCPreset final : public Selection {
public:
	using Selection::Selection;

	static constexpr int32_t kMaxPresets = 20;
	static constexpr char const* kPresetDir = "SETTINGS/MIDIFollow";

	void beginSession(MenuItem* navigatedBackwardFrom) override {
		scanDirectory();
		this->setValue(0);
		Selection::beginSession(navigatedBackwardFrom);
	}

	void readCurrentValue() override {}
	void writeCurrentValue() override {}

	deluge::vector<std::string_view> getOptions(OptType optType = OptType::FULL) override {
		deluge::vector<std::string_view> options;
		for (int32_t i = 0; i < numPresets_; i++) {
			options.push_back(std::string_view(presetNames_[i].get()));
		}
		if (options.empty()) {
			options.push_back("(none)");
		}
		return options;
	}

	size_t size() override { return numPresets_ > 0 ? numPresets_ : 1; }

	MenuItem* selectButtonPress() override {
		int32_t idx = this->getValue();
		if (idx >= 0 && idx < numPresets_) {
			midiFollow.loadPresetFromFile(presetPaths_[idx].get());
			display->displayPopup(presetNames_[idx].get());
		}
		return nullptr; // go back up
	}

private:
	String presetNames_[kMaxPresets]; // display names (without .XML)
	String presetPaths_[kMaxPresets]; // full paths
	int32_t numPresets_ = 0;

	void scanDirectory() {
		// Clear previous entries
		for (int32_t i = 0; i < numPresets_; i++) {
			presetNames_[i].clear();
			presetPaths_[i].clear();
		}
		numPresets_ = 0;

		DIR dir;
		FILINFO fno;
		FRESULT result = f_opendir(&dir, kPresetDir);
		if (result != FR_OK) {
			return;
		}

		while (numPresets_ < kMaxPresets) {
			result = f_readdir(&dir, &fno);
			if (result != FR_OK || fno.fname[0] == 0) {
				break;
			}
			if (fno.fattrib & AM_DIR) {
				continue; // skip subdirectories
			}

			// Filter for .XML files
			char* dot = strrchr(fno.fname, '.');
			if (!dot || strcasecmp(dot, ".XML") != 0) {
				continue;
			}

			// Store display name (filename without extension)
			*dot = '\0';
			presetNames_[numPresets_].set(fno.fname);
			*dot = '.'; // restore

			// Store full path
			presetPaths_[numPresets_].set(kPresetDir);
			presetPaths_[numPresets_].concatenate("/");
			presetPaths_[numPresets_].concatenate(fno.fname);

			numPresets_++;
		}

		f_closedir(&dir);
	}
};
} // namespace deluge::gui::menu_item::midi
