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
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item::fx {

// Drive: Bipolar patched param for saturator input gain
// Gold knob press toggles AA via Sound::modEncoderButtonAction
using SaturatorDrive = DynamicsPatchedParam;

// High resolution: 1024 steps for Y axis zone exploration
constexpr int32_t kSaturatorHighResSteps = 1024;
constexpr int32_t kSaturatorNumZones = 8; // Placeholder zone count

// Shape X: Soft→Hard axis (0-127)
// Controls knee/clipping aggressiveness
class SaturatorShapeX final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->saturatorShapeX); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		auto* mca = soundEditor.currentModControllable;

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->saturatorShapeX = current_value;
					soundDrum->saturator.regenerateTable(current_value, soundDrum->saturatorShapeY,
					                                     soundDrum->saturatorPhase);
				}
			}
		}
		else {
			mca->saturatorShapeX = current_value;
			mca->saturator.regenerateTable(current_value, mca->saturatorShapeY, mca->saturatorPhase);
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}

	// Auto-enable mix when user enables X from OFF state
	void selectEncoderAction(int32_t offset) override {
		auto* mca = soundEditor.currentModControllable;
		bool wasOff = (this->getValue() == 0);
		IntegerWithOff::selectEncoderAction(offset);
		// If X was 0 and user turned it up, auto-set mix to 50% so effect is audible
		if (wasOff && this->getValue() > 0 && mca->saturatorMix == 0) {
			mca->saturatorMix = 64; // 50% wet
		}
		if (this->getValue() == 0 || mca->saturatorMix == 0) {
			display->displayPopup("OFF");
		}
	}

	// Show "OFF" in horizontal menu when effect is bypassed (X=0 or mix=0)
	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		if (this->getValue() == 0 || soundEditor.currentModControllable->saturatorMix == 0) {
			deluge::hid::display::OLED::main.drawStringCentered("OFF", slot.start_x,
			                                                    slot.start_y + kHorizontalMenuSlotYOffset,
			                                                    kTextSpacingX, kTextSpacingY, slot.width);
			return;
		}
		IntegerWithOff::renderInHorizontalMenu(slot);
	}
};

// Shape Y: High-res multi-zone sweep through analytic saturator parameter space (0-1023)
// Controls harmonic character via combinatoric blend of 6 basis functions:
// Tanh (warm), Polynomial (bright), Hard Knee (clip), Chebyshev T5 (fold), Sine Folder (gold), Rectifier (diode)
// Uses velocity-based encoder acceleration for smooth navigation through 1024 steps
// Secret menu: Push+twist encoder to adjust saturatorPhase (per-patch phase offset for triangle modulation)
class SaturatorShapeY final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->saturatorShapeY); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		auto* mca = soundEditor.currentModControllable;

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->saturatorShapeY = current_value;
					soundDrum->saturator.regenerateTable(soundDrum->saturatorShapeX, current_value,
					                                     soundDrum->saturatorPhase);
				}
			}
		}
		else {
			mca->saturatorShapeY = current_value;
			mca->saturator.regenerateTable(mca->saturatorShapeX, current_value, mca->saturatorPhase);
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return kSaturatorHighResSteps - 1; } // 0-1023
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}

	// Velocity-based encoder acceleration for navigating 1024 steps
	// Secret menu: Push+twist to adjust saturatorPhase
	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			// Secret menu: adjust saturatorPhase (unbounded, wraps via fmod in DSP)
			Buttons::selectButtonPressUsedUp = true;
			float& phase = soundEditor.currentModControllable->saturatorPhase;
			phase += static_cast<float>(velocity_.getScaledOffset(offset)) * 0.1f;
			// Regenerate table with new phase
			auto* mca = soundEditor.currentModControllable;
			mca->saturator.regenerateTable(mca->saturatorShapeX, mca->saturatorShapeY, phase);
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
		renderZoneInHorizontalMenu(slot, this->getValue(), kSaturatorHighResSteps, kSaturatorNumZones, getZoneName);
	}

protected:
	void drawPixelsForOled() override {
		drawZoneForOled(this->getValue(), kSaturatorHighResSteps, kSaturatorNumZones, getZoneName);
	}

private:
	mutable VelocityEncoder velocity_;
	mutable bool suppressNotification_ = false;

	// Zone names reflecting the 6 basis functions explored across Y axis
	// Basis functions: Tanh(warm), Poly(bright), HardKnee(clip), Cheby(fold), SineFold(gold), Rect(diode)
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

	// Show "OFF" when X=0, "DRY" when mix=0
	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		if (soundEditor.currentModControllable->saturatorShapeX == 0) {
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

	// Auto-enable X when user tries to increase mix from OFF state
	void selectEncoderAction(int32_t offset) override {
		auto* mca = soundEditor.currentModControllable;
		if (mca->saturatorShapeX == 0) {
			if (offset > 0) {
				// User wants to enable the effect - auto-set X=1
				mca->saturatorShapeX = 1;
				mca->saturator.regenerateTable(1, mca->saturatorShapeY, mca->saturatorPhase);
			}
			else {
				display->displayPopup("OFF");
				return; // Can't go lower than OFF
			}
		}
		IntegerWithOff::selectEncoderAction(offset);
		if (this->getValue() == 0) {
			display->displayPopup("DRY");
		}
	}
};

} // namespace deluge::gui::menu_item::fx
