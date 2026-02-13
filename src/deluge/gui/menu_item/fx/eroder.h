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

#include "dsp/eroder.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/zone_based.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/song/song.h"
#include "modulation/params/param.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace params = deluge::modulation::params;

namespace deluge::gui::menu_item::fx {

// Eroder Freq: Frequency zone for allpass center frequency (8 zones: Sub → Full)
class EroderFreq final : public ZoneBasedDualParam<params::GLOBAL_ERODER_FREQ> {
public:
	using ZoneBasedDualParam::ZoneBasedDualParam;

	[[nodiscard]] q31_t getFieldValue() const override { return soundEditor.currentModControllable->eroder.freq.value; }
	void setFieldValue(q31_t value) override { soundEditor.currentModControllable->eroder.freq.value = value; }

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "Sub";
		case 1:
			return "Bass";
		case 2:
			return "Low";
		case 3:
			return "Mid";
		case 4:
			return "High";
		case 5:
			return "Air";
		case 6:
			return "Bright";
		case 7:
			return "Full";
		default:
			return "?";
		}
	}

	[[nodiscard]] const char* getShortZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "SB";
		case 1:
			return "BA";
		case 2:
			return "LO";
		case 3:
			return "MI";
		case 4:
			return "HI";
		case 5:
			return "AI";
		case 6:
			return "BR";
		case 7:
			return "FL";
		default:
			return "??";
		}
	}
};

// Eroder Character: Noise/modulator type zone (8 zones: White → Smooth)
class EroderCharacter final : public ZoneBasedDualParam<params::GLOBAL_ERODER_CHARACTER> {
public:
	using ZoneBasedDualParam::ZoneBasedDualParam;

	[[nodiscard]] q31_t getFieldValue() const override {
		return soundEditor.currentModControllable->eroder.character.value;
	}
	void setFieldValue(q31_t value) override { soundEditor.currentModControllable->eroder.character.value = value; }

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "White";
		case 1:
			return "Pink";
		case 2:
			return "Brown";
		case 3:
			return "Blue";
		case 4:
			return "Sine";
		case 5:
			return "Ring";
		case 6:
			return "Sparse";
		case 7:
			return "Smooth";
		default:
			return "?";
		}
	}

	[[nodiscard]] const char* getShortZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "WH";
		case 1:
			return "PK";
		case 2:
			return "BN";
		case 3:
			return "BL";
		case 4:
			return "SN";
		case 5:
			return "RG";
		case 6:
			return "SP";
		case 7:
			return "SM";
		default:
			return "??";
		}
	}
};

// Eroder Depth: Modulation intensity (0=off/bypass, 1-127)
class EroderDepth final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->eroder.depth); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->eroder.depth = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->eroder.depth = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }

	void renderInHorizontalMenu(const SlotPosition& slot) override {
		if (this->getValue() == 0) {
			deluge::hid::display::OLED::main.drawStringCentered("OFF", slot.start_x,
			                                                    slot.start_y + kHorizontalMenuSlotYOffset,
			                                                    kTextSpacingX, kTextSpacingY, slot.width);
			return;
		}
		IntegerWithOff::renderInHorizontalMenu(slot);
	}
};

// Eroder Mix: Wet/dry blend (0=dry, 127=wet)
class EroderMix final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->eroder.mix); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->eroder.mix = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->eroder.mix = current_value;
		}
	}

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

} // namespace deluge::gui::menu_item::fx
