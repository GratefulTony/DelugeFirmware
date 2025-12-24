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

// Disperser Freq: Center frequency for allpass cascade (0-127, maps to 50Hz-8kHz)
class DisperserFreq final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->disperserFreq); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->disperserFreq = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->disperserFreq = current_value;
		}
	}
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

// Disperser Spread: Frequency spread across stages (0-127, 0=all same, 127=±4 octaves)
class DisperserSpread final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->disperserSpread); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->disperserSpread = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->disperserSpread = current_value;
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

// Disperser Feedback: Output fed back to input (0-127, 64=none, 0=negative, 127=positive)
class DisperserFeedback final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->disperserFeedback); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->disperserFeedback = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->disperserFeedback = current_value;
		}
	}
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

// Disperser Stages: Number of active allpass stages (0-16, acts as on/off and intensity)
class DisperserStages final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->disperserStages); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->disperserStages = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->disperserStages = current_value;
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 16; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

} // namespace deluge::gui::menu_item::fx
