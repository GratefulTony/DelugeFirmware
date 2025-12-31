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

// Shape Y: High-res multi-zone sweep through analytic saturator parameter space (0-1023)
// Controls harmonic character via combinatoric blend of 6 basis functions:
// Tanh (warm), Polynomial (bright), Hard Knee (clip), Chebyshev T5 (fold), Sine Folder (gold), Rectifier (diode)
// Uses velocity-based encoder acceleration for smooth navigation through 1024 steps
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
	[[nodiscard]] int32_t getMaxValue() const override { return kSaturatorHighResSteps - 1; } // 0-1023
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}

	// Velocity-based encoder acceleration for navigating 1024 steps
	void selectEncoderAction(int32_t offset) override {
		IntegerWithOff::selectEncoderAction(velocity_.getScaledOffset(offset));
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
};

} // namespace deluge::gui::menu_item::fx
