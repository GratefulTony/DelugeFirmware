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

#include "gui/l10n/l10n.h"
#include "gui/menu_item/fx/sine_shaper.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/momentum_encoder.h"
#include "gui/ui/sound_editor.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item::fx {

// Drive: Bipolar patched param for shaper input gain
// Gold knob press toggles AA via Sound::modEncoderButtonAction
using TableShaperDrive = DynamicsPatchedParam;

constexpr int32_t kShaperHighResSteps = 1024;
constexpr int32_t kShaperNumZones = 8;

// Shape X: Soft→Hard axis (0-127)
// Controls knee/clipping aggressiveness
class TableShaperShapeX final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->shaperShapeX); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		auto* mca = soundEditor.currentModControllable;

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->shaperShapeX = current_value;
					soundDrum->shaper.regenerateTable(current_value, soundDrum->shaperShapeY, soundDrum->shaperPhase);
				}
			}
		}
		else {
			mca->shaperShapeX = current_value;
			mca->shaper.regenerateTable(current_value, mca->shaperShapeY, mca->shaperPhase);
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }

	void selectEncoderAction(int32_t offset) override {
		auto* mca = soundEditor.currentModControllable;
		bool wasOff = (this->getValue() == 0);
		IntegerWithOff::selectEncoderAction(offset);
		if (wasOff && this->getValue() > 0 && mca->shaperMix == 0) {
			mca->shaperMix = 64; // Auto-enable mix
		}
		if (this->getValue() == 0 || mca->shaperMix == 0) {
			display->displayPopup("OFF");
		}
	}

	// Show "OFF" in horizontal menu when effect is bypassed (X=0 or mix=0)
	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		if (this->getValue() == 0 || soundEditor.currentModControllable->shaperMix == 0) {
			deluge::hid::display::OLED::main.drawStringCentered("OFF", slot.start_x,
			                                                    slot.start_y + kHorizontalMenuSlotYOffset,
			                                                    kTextSpacingX, kTextSpacingY, slot.width);
			return;
		}
		IntegerWithOff::renderInHorizontalMenu(slot);
	}
};

// Shape Y (UI: "Color"): Sweeps through saturation characters
// Secret menu: Push+twist to adjust shaperPhase
class TableShaperShapeY final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->shaperShapeY); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		auto* mca = soundEditor.currentModControllable;

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->shaperShapeY = current_value;
					soundDrum->shaper.regenerateTable(soundDrum->shaperShapeX, current_value, soundDrum->shaperPhase);
				}
			}
		}
		else {
			mca->shaperShapeY = current_value;
			mca->shaper.regenerateTable(mca->shaperShapeX, current_value, mca->shaperPhase);
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return kShaperHighResSteps - 1; } // 0-1023
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }

	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			// Secret: push+twist adjusts shaperPhase
			Buttons::selectButtonPressUsedUp = true;
			float& phase = soundEditor.currentModControllable->shaperPhase;
			phase += static_cast<float>(velocity_.getScaledOffset(offset)) * 0.1f;
			// Regenerate table with new phase
			auto* mca = soundEditor.currentModControllable;
			mca->shaper.regenerateTable(mca->shaperShapeX, mca->shaperShapeY, phase);
			// Show current value on display
			char buffer[12];
			intToString(static_cast<int32_t>(phase * 10.0f), buffer);
			display->displayPopup(buffer);
			suppressNotification_ = true;
		}
		else {
			IntegerWithOff::selectEncoderAction(velocity_.getScaledOffset(offset));
		}
	}

	[[nodiscard]] bool showNotification() const override {
		if (suppressNotification_) {
			suppressNotification_ = false;
			return false;
		}
		return true;
	}

	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		renderZoneInHorizontalMenu(slot, this->getValue(), kShaperHighResSteps, kShaperNumZones, getZoneName);
	}

protected:
	void drawPixelsForOled() override {
		drawZoneForOled(this->getValue(), kShaperHighResSteps, kShaperNumZones, getZoneName);
	}

private:
	mutable VelocityEncoder velocity_;
	mutable bool suppressNotification_ = false;

	static const char* getZoneName(int32_t zoneIndex) {
		switch (zoneIndex) {
		case 0:
			return "Warm"; // Tanh-dominant, smooth saturation
		case 1:
			return "Bright"; // Polynomial-dominant, upper harmonics
		case 2:
			return "Clip"; // Hard knee dominant, aggressive
		case 3:
			return "Fold"; // Chebyshev T5, wavefolder character
		case 4:
			return "Gold"; // Sine folder, rich harmonics
		case 5:
			return "Diode"; // Rectifier, asymmetric/even harmonics
		case 6:
			return "Blend"; // Mixed basis functions
		case 7:
			return "Morph"; // Complex combinations
		default:
			return "?";
		}
	}
};

// Mix: Wet/dry blend (0 = bypass, 127 = full wet)
class TableShaperMix final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->shaperMix); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->shaperMix = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->shaperMix = current_value;
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }

	// Show "OFF" when X=0, "DRY" when mix=0
	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		if (soundEditor.currentModControllable->shaperShapeX == 0) {
			deluge::hid::display::OLED::main.drawStringCentered("OFF", slot.start_x,
			                                                    slot.start_y + kHorizontalMenuSlotYOffset,
			                                                    kTextSpacingX, kTextSpacingY, slot.width);
			return;
		}
		if (this->getValue() == 0) {
			deluge::hid::display::OLED::main.drawStringCentered("DRY", slot.start_x,
			                                                    slot.start_y + kHorizontalMenuSlotYOffset,
			                                                    kTextSpacingX, kTextSpacingY, slot.width);
			return;
		}
		IntegerWithOff::renderInHorizontalMenu(slot);
	}

	void selectEncoderAction(int32_t offset) override {
		auto* mca = soundEditor.currentModControllable;
		if (mca->shaperShapeX == 0) {
			if (offset > 0) {
				mca->shaperShapeX = 1; // Auto-enable Knee
				mca->shaper.regenerateTable(1, mca->shaperShapeY, mca->shaperPhase);
			}
			else {
				display->displayPopup("OFF");
				return;
			}
		}
		IntegerWithOff::selectEncoderAction(offset);
		if (this->getValue() == 0) {
			display->displayPopup("DRY");
		}
	}
};

} // namespace deluge::gui::menu_item::fx
