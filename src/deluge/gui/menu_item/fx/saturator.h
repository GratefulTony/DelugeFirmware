/*
 * Copyright (c) 2024 Synthstrom Audible Limited
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

#include "gui/menu_item/integer.h"
#include "gui/ui/sound_editor.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include <cstdint>

namespace deluge::gui::menu_item::fx {

// Drive: input gain / saturation amount (0-127)
class SaturatorDrive final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->saturatorDrive); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->saturatorDrive = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->saturatorDrive = current_value;
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

// Shape X: Soft→Hard axis (0-127)
// Controls knee/clipping aggressiveness
class SaturatorShapeX final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->saturatorShapeX); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->saturatorShapeX = current_value;
					soundDrum->saturator.regenerateTable(current_value, soundDrum->saturatorShapeY);
				}
			}
		}
		else {
			soundEditor.currentModControllable->saturatorShapeX = current_value;
			soundEditor.currentModControllable->saturator.regenerateTable(
			    current_value, soundEditor.currentModControllable->saturatorShapeY);
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

// Shape Y: Clean→Weird axis (0-127)
// Controls harmonic character
class SaturatorShapeY final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->saturatorShapeY); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->saturatorShapeY = current_value;
					soundDrum->saturator.regenerateTable(soundDrum->saturatorShapeX, current_value);
				}
			}
		}
		else {
			soundEditor.currentModControllable->saturatorShapeY = current_value;
			soundEditor.currentModControllable->saturator.regenerateTable(
			    soundEditor.currentModControllable->saturatorShapeX, current_value);
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

// Mix: Wet/dry blend (0 = bypass, 127 = full wet)
class SaturatorMix final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->saturatorMix); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->saturatorMix = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->saturatorMix = current_value;
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

} // namespace deluge::gui::menu_item::fx
