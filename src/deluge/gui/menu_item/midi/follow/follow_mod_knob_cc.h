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
#include "definitions_cxx.hpp"
#include "gui/menu_item/integer.h"
#include "gui/ui/sound_editor.h"
#include "io/midi/midi_engine.h"

namespace deluge::gui::menu_item::midi {

/// Menu item for setting the base CC number for mod knob MIDI control.
/// When enabled, CCs from baseCC to baseCC+15 map to the 16 mod knob slots.
/// Value 0 = OFF (disabled), 1-112 = base CC number.
class FollowModKnobCC final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override {
		uint8_t stored = midiEngine.midiFollowModKnobBaseCC;
		this->setValue(stored == MIDI_CC_NONE ? 0 : stored);
	}

	void writeCurrentValue() override {
		int32_t val = this->getValue();
		midiEngine.midiFollowModKnobBaseCC = (val == 0) ? MIDI_CC_NONE : static_cast<uint8_t>(val);
	}

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxMIDIValue - 15; }

	bool allowsLearnMode() override { return true; }

	void learnCC(MIDICable& cable, int32_t channel, int32_t ccNumber, int32_t value) override {
		if (ccNumber < 1 || ccNumber > kMaxMIDIValue - 15) {
			return;
		}
		this->setValue(ccNumber);
		midiEngine.midiFollowModKnobBaseCC = ccNumber;

		if (soundEditor.getCurrentMenuItem() == this) {
			if (display->haveOLED()) {
				renderUIsForOled();
			}
			else {
				drawValue();
			}
		}
		else {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_LEARNED));
		}
	}
};
} // namespace deluge::gui::menu_item::midi
