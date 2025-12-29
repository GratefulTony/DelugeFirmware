/*
 * Copyright (c) 2014-2023 Synthstrom Audible Limited
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
#include "gui/menu_item/patched_param/integer.h"
#include "gui/menu_item/unpatched_param.h"
#include "gui/menu_item/zone_based.h"
#include "gui/ui/sound_editor.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/params/param.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include <cstdint>
#include <hid/buttons.h>
#include <hid/display/display.h>
#include <limits>

namespace params = deluge::modulation::params;

namespace deluge::gui::menu_item::fx {

/// UnpatchedParam with DynamicsSoundDesign gating for learnable drive parameters.
/// Used in menus.cpp for sineShaperDriveMenu and saturatorDriveMenu.
class DynamicsUnpatchedParam : public UnpatchedParam {
public:
	using UnpatchedParam::UnpatchedParam;
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}
};

/// PatchedParam with DynamicsSoundDesign gating for mod-matrix-routable drive parameters.
/// Used in menus.cpp for sineShaperDriveMenu and saturatorDriveMenu.
/// Uses bipolar range (-128 to +128) where 0 = unity, negative = below unity, -128 = -inf.
class DynamicsPatchedParam : public patched_param::Integer {
public:
	static constexpr int32_t kDriveMenuHalfRange = 128;

	using patched_param::Integer::Integer;
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}

	/// Override selectEncoderAction to enforce our bipolar range (-128 to +128)
	/// We bypass Integer::selectEncoderAction to avoid its getMaxValue/getMinValue clamping
	void selectEncoderAction(int32_t offset) override {
		int32_t newValue = this->getValue() + offset;
		// Clamp to our bipolar range
		if (newValue > kDriveMenuHalfRange) {
			newValue = kDriveMenuHalfRange;
		}
		else if (newValue < -kDriveMenuHalfRange) {
			newValue = -kDriveMenuHalfRange;
		}
		this->setValue(newValue);

		// Trigger value write and display update (mimics Value::selectEncoderAction)
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			Buttons::selectButtonPressUsedUp = true;
		}
		writeCurrentValue();
		if (display->haveOLED()) {
			renderUIsForOled();
		}
		else {
			drawValue();
		}
	}

protected:
	[[nodiscard]] int32_t getMinValue() const override { return -kDriveMenuHalfRange; }
	[[nodiscard]] int32_t getMaxValue() const override { return kDriveMenuHalfRange; }

	void readCurrentValue() override {
		// Scale q31 (-2^31 to 2^31-1) to menu range (-128 to +128)
		// q31 0 = menu 0 (unity), q31 INT32_MIN = menu -128 (-inf)
		int32_t q31Value = soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP());
		// Shift right by 24 bits to get -128 to +127 range
		this->setValue(q31Value >> 24);
	}

	int32_t getFinalValue() override {
		int32_t value = this->getValue();
		if (value >= kDriveMenuHalfRange) {
			return std::numeric_limits<int32_t>::max();
		}
		else if (value <= -kDriveMenuHalfRange) {
			return std::numeric_limits<int32_t>::min();
		}
		else {
			// Scale -128..+128 back to q31 range: shift left by 24 bits
			return value << 24;
		}
	}
};

/// Harmonic zone control - 8 zones with triangle-modulated Chebyshev harmonics
/// Zone 0: Poly - Cascaded polynomial waveshaping
/// Zones 1-7: Chebyshev harmonics with triangle modulation
class SineShaperHarmonic final : public ZoneBasedUnpatchedParam<params::UNPATCHED_SINE_SHAPER_HARMONIC> {
public:
	using ZoneBasedUnpatchedParam::ZoneBasedUnpatchedParam;

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "Poly";
		case 1:
			return "3579"; // T3, T5, T7/T9 blend with unbounded output (edgy)
		case 2:
			return "3579wm"; // T3, T5, T7/T9 with sine input waveshaping (warm)
		case 3:
			return "FM"; // Cascaded sine waveshaping
		case 4:
			return "Cheby 5";
		case 5:
			return "Cheby 7";
		case 6:
			return "Chaos";
		case 7:
			return "---";
		default:
			return "?";
		}
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}
};

/// Twist zone control - 8 zones with different modifiers
/// Zone 0: Asym - DC bias for even harmonics (asymmetric clipping)
/// Zone 1: Wide - Stereo coefficient spread
/// Zone 2: Even - Self-mul for even harmonics (3→6, 5→10, 7→14)
/// Zone 3: Rect - Rectifier blend (pure octave up)
/// Zone 4: Reserved
/// Zone 5: Fdbk - Output→input feedback (thickening to chaos)
/// Zones 6-7: Reserved
class SineShaperTwist final : public ZoneBasedUnpatchedParam<params::UNPATCHED_SINE_SHAPER_TWIST> {
public:
	using ZoneBasedUnpatchedParam::ZoneBasedUnpatchedParam;

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "Asym";
		case 1:
			return "Wide";
		case 2:
			return "Even";
		case 3:
			return "Rect";
		case 4:
		case 6:
		case 7:
			return "---";
		case 5:
			return "Fdbk";
		default:
			return "?";
		}
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}
};

// Mix: wet/dry blend (0-127, 0 = bypass)
class SineShaperMix final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->sineShaper.mix); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->sineShaper.mix = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->sineShaper.mix = current_value;
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}
};

} // namespace deluge::gui::menu_item::fx
