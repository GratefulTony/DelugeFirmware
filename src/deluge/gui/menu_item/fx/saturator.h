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
#include "gui/menu_item/momentum_encoder.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/oled.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include <cstdint>

namespace deluge::gui::menu_item::fx {

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
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}
};

// Shape Y: Clean→Weird axis (0-255)
// Controls harmonic character with 5 distinct zones
// Zone 4 (Aanalytic) uses analytic parametric saturator with ADAA
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
	[[nodiscard]] int32_t getMaxValue() const override { return 255; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}

	// High-resolution encoder with momentum acceleration
	void selectEncoderAction(int32_t offset) override {
		IntegerWithOff::selectEncoderAction(momentum_.getScaledOffset(offset));
	}

	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		// Custom zone rendering for unequal zone sizes
		// Zones 0-3: 32 steps each (0-127), Zone 4: 128 steps (128-255)
		int32_t value = this->getValue();
		int32_t zoneIndex = getZoneIndex(value);
		const char* zoneName = getZoneName(zoneIndex);

		using namespace deluge::hid::display;
		OLED::main.drawStringCentered(zoneName, slot.start_x, slot.start_y, kTextSmallSpacingX, kTextSmallSizeY,
		                              slot.width);

		// Draw position bar showing position within zone
		constexpr int32_t barWidth = 20;
		constexpr int32_t barHeight = 3;
		int32_t barX = slot.start_x + (slot.width - barWidth) / 2;
		int32_t barY = slot.start_y + kTextSmallSizeY + 2;

		float posInZone = getPositionInZone(value, zoneIndex);
		OLED::main.drawRectangle(barX, barY, barX + barWidth - 1, barY + barHeight - 1);
		int32_t fillWidth = static_cast<int32_t>(posInZone * (barWidth - 2));
		if (fillWidth > 0) {
			OLED::main.invertArea(barX + 1, fillWidth, barY + 1, barY + barHeight - 2);
		}
	}

protected:
	void drawPixelsForOled() override {
		int32_t value = this->getValue();
		int32_t zoneIndex = getZoneIndex(value);
		const char* zoneName = getZoneName(zoneIndex);

		using namespace deluge::hid::display;
		OLED::main.drawStringCentred(zoneName, 18 + OLED_MAIN_TOPMOST_PIXEL, kTextHugeSpacingX, kTextHugeSizeY);
	}

private:
	mutable MomentumEncoder momentum_;

	/// Get zone index from value (zones 0-3 are 32 steps each, zone 4 is 128 steps)
	static int32_t getZoneIndex(int32_t value) {
		if (value >= 128) {
			return 4; // Aanalytic zone
		}
		return value / 32; // Zones 0-3 (32 steps each)
	}

	/// Get position within zone (0.0 to 1.0)
	static float getPositionInZone(int32_t value, int32_t zoneIndex) {
		if (zoneIndex == 4) {
			// Zone 4: 128-255 (128 steps)
			return static_cast<float>(value - 128) / 127.0f;
		}
		// Zones 0-3: 32 steps each
		int32_t zoneStart = zoneIndex * 32;
		return static_cast<float>(value - zoneStart) / 31.0f;
	}

	static const char* getZoneName(int32_t zoneIndex) {
		switch (zoneIndex) {
		case 0:
			return "Satur";
		case 1:
			return "Tube";
		case 2:
			return "Clip";
		case 3:
			return "Xover";
		case 4:
			return "Aanalytic"; // Analytic ADAA saturator with combinatoric parameter sweep
		default:
			return "?";
		}
	}
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
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}
};

} // namespace deluge::gui::menu_item::fx
