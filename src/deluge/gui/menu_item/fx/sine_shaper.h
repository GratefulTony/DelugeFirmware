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
#include "model/model_stack.h"
#include "model/song/song.h"
#include "modulation/params/param.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "util/d_string.h"
#include <cstdint>
#include <hid/buttons.h>
#include <hid/display/display.h>
#include <hid/display/oled.h>
#include <limits>

namespace params = deluge::modulation::params;

namespace deluge::gui::menu_item::fx {

// Helpers for dual patched/unpatched param access (Sound vs GlobalEffectable contexts)
inline q31_t getShapingParamValue(params::ParamType patched, params::ParamType unpatched) {
	if (soundEditor.currentParamManager->containsPatchedParamSetCollection()) {
		return soundEditor.currentParamManager->getPatchedParamSet()->getValue(patched);
	}
	return soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(unpatched);
}

inline ModelStackWithAutoParam* getShapingModelStack(void* memory, params::ParamType patched,
                                                     params::ParamType unpatched) {
	ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(memory);
	if (soundEditor.currentParamManager->containsPatchedParamSetCollection()) {
		return modelStack->getPatchedAutoParamFromId(patched);
	}
	return modelStack->getUnpatchedAutoParamFromId(unpatched);
}

/// UnpatchedParam for learnable drive parameters in the shaping submenu.
/// Used in menus.cpp for sineShaperDriveMenu and shaperDriveMenu.
/// Visibility is gated at the submenu level by submenu::Shaping.
class DynamicsUnpatchedParam : public UnpatchedParam {
public:
	using UnpatchedParam::UnpatchedParam;
};

/// PatchedParam for mod-matrix-routable drive parameters in the shaping submenu.
/// Used in menus.cpp for sineShaperDriveMenu and shaperDriveMenu.
/// Uses bipolar range (-128 to +128) where 0 = unity, negative = below unity, -128 = -inf.
/// Visibility is gated at the submenu level by submenu::Shaping.
class DynamicsPatchedParam : public patched_param::Integer {
public:
	static constexpr int32_t kDriveMenuHalfRange = 128;

	using patched_param::Integer::Integer;

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

	params::ParamType getUnpatchedP() {
		return (getP() == params::LOCAL_SINE_SHAPER_DRIVE) ? params::UNPATCHED_SINE_SHAPER_DRIVE
		                                                   : params::UNPATCHED_TABLE_SHAPER_DRIVE;
	}

	void readCurrentValue() override { this->setValue(getShapingParamValue(getP(), getUnpatchedP()) >> 24); }

	ModelStackWithAutoParam* getModelStack(void* memory) override {
		return getShapingModelStack(memory, getP(), getUnpatchedP());
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
/// Zone 0: 3579 - T3, T5, T7/T9 blend (edgy)
/// Zone 1: 3579wm - Same with sine input waveshaping (warm)
/// Zone 2: FM - Add, Ring, FM, Fold at 2x
/// Zone 3: Fold - Wavefolder k=1,2,3,4
/// Zone 4: Ring - Ring mod n=2,3,4,5
/// Zone 5: Add - Additive n=2,3,4,5
/// Zone 6: Mod - FM depths d=0.25,0.5,0.75,1.0
/// Zone 7: Poly - Cascaded polynomial waveshaping
/// Secret menu: Push+twist encoder to adjust harmonicPhaseOffset (per-patch phase offset)
/// Press encoder (no twist): Opens mod matrix source selection
class SineShaperHarmonic final : public ZoneBasedDualParam<params::LOCAL_SINE_SHAPER_HARMONIC> {
public:
	using ZoneBasedDualParam::ZoneBasedDualParam;

	[[nodiscard]] q31_t getFieldValue() const override {
		return soundEditor.currentModControllable->sineShaper.harmonic;
	}

	void setFieldValue(q31_t value) override { soundEditor.currentModControllable->sineShaper.harmonic = value; }

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "3579"; // T3, T5, T7/T9 blend with unbounded output (edgy)
		case 1:
			return "3579wm"; // T3, T5, T7/T9 with sine input waveshaping (warm)
		case 2:
			return "FM"; // Add, Ring, FM, Fold at 2x
		case 3:
			return "Fold"; // Wavefolder k=1,2,3,4
		case 4:
			return "Ring"; // Ring mod n=2,3,4,5
		case 5:
			return "Add"; // Additive n=2,3,4,5
		case 6:
			return "Mod"; // FM depths d=0.25,0.5,0.75,1.0
		case 7:
			return "Poly"; // Cascaded polynomial waveshaping (most expensive)
		default:
			return "?";
		}
	}

	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			// Secret menu: adjust harmonicPhaseOffset (gated ≥0 for fast floor optimization)
			Buttons::selectButtonPressUsedUp = true;
			float& phase = soundEditor.currentModControllable->sineShaper.harmonicPhaseOffset;
			phase = std::max(0.0f, phase + static_cast<float>(velocity_.getScaledOffset(offset)) * 0.1f);
			// Show current value on display
			char buffer[16];
			snprintf(buffer, sizeof(buffer), "offset:%d", static_cast<int32_t>(phase * 10.0f));
			display->displayPopup(buffer);
			renderUIsForOled();           // Refresh display for consistency
			suppressNotification_ = true; // Prevent horizontal menu from overwriting popup
		}
		else {
			ZoneBasedDualParam::selectEncoderAction(offset);
		}
	}

	[[nodiscard]] bool showNotification() const override {
		if (suppressNotification_) {
			suppressNotification_ = false;
			return false;
		}
		return true;
	}

private:
	mutable bool suppressNotification_ = false;
};

/// Twist zone control - 8 zones with different modifiers
/// Zone 0: Width - Stereo spread with animated phase evolution
/// Zone 1: Evens - Asymmetric compression for even harmonics
/// Zone 2: Rect - Blended rectifier (rect + rect2 with overlap)
/// Zone 3: Fdbk - Output→input feedback (thickening to chaos)
/// Zones 4-7: Meta - Combined modifiers with φ-ratio triangle modulation
/// Secret menu: Push+twist encoder to adjust twistPhaseOffset (per-patch phase offset for meta zone)
/// Press encoder (no twist): Opens mod matrix source selection
class SineShaperTwist final : public ZoneBasedDualParam<params::LOCAL_SINE_SHAPER_TWIST> {
public:
	using ZoneBasedDualParam::ZoneBasedDualParam;

	[[nodiscard]] q31_t getFieldValue() const override { return soundEditor.currentModControllable->sineShaper.twist; }

	void setFieldValue(q31_t value) override { soundEditor.currentModControllable->sineShaper.twist = value; }

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "Width";
		case 1:
			return "Evens";
		case 2:
			return "Rect";
		case 3:
			return "Fdbk";
		case 4:
			return "Twist1"; // Meta zone 1
		case 5:
			return "Twist2"; // Meta zone 2
		case 6:
			return "Twist3"; // Meta zone 3
		case 7:
			return "Twist4"; // Meta zone 4
		default:
			return "---";
		}
	}

	[[nodiscard]] const char* getShortZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "WD";
		case 1:
			return "EV";
		case 2:
			return "RC";
		case 3:
			return "FB";
		default:
			return "TW"; // Twist zones 4-7
		}
	}

	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			// Secret menu: adjust twistPhaseOffset (gated ≥0 for fast floor optimization)
			Buttons::selectButtonPressUsedUp = true;
			float& phase = soundEditor.currentModControllable->sineShaper.twistPhaseOffset;
			phase = std::max(0.0f, phase + static_cast<float>(velocity_.getScaledOffset(offset)) * 0.1f);
			// Show current value on display
			char buffer[16];
			snprintf(buffer, sizeof(buffer), "offset:%d", static_cast<int32_t>(phase * 10.0f));
			display->displayPopup(buffer);
			renderUIsForOled();           // Refresh display to show updated coordinate format
			suppressNotification_ = true; // Prevent horizontal menu from overwriting popup
		}
		else {
			ZoneBasedDualParam::selectEncoderAction(offset);
		}
	}

	[[nodiscard]] bool showNotification() const override {
		if (suppressNotification_) {
			suppressNotification_ = false;
			return false;
		}
		return true;
	}

	// Override rendering to show numeric coordinates when phaseOffset > 0
	void renderInHorizontalMenu(const SlotPosition& slot) override {
		double phaseOffset = effectivePhaseOffset();
		if (phaseOffset != 0.0) {
			// When secret knob is engaged, show "P:Z" (phase:zone) with visual indicator
			cacheCoordDisplay(phaseOffset, this->getValue());
			renderZoneInHorizontalMenu(slot, this->getValue(), kSineShaperResolution, kSineShaperNumZones,
			                           getCoordName);
		}
		else {
			renderZoneInHorizontalMenu(slot, this->getValue(), kSineShaperResolution, kSineShaperNumZones,
			                           [this](int32_t z) { return this->getZoneName(z); });
		}
	}

protected:
	void drawPixelsForOled() override {
		double phaseOffset = effectivePhaseOffset();
		if (phaseOffset != 0.0) {
			// When secret knob is engaged, show numeric coordinates
			cacheCoordDisplay(phaseOffset, this->getValue());
			drawZoneForOled(this->getValue(), kSineShaperResolution, kSineShaperNumZones, getCoordName);
		}
		else {
			drawZoneForOled(this->getValue(), kSineShaperResolution, kSineShaperNumZones,
			                [this](int32_t z) { return this->getZoneName(z); });
		}
	}

private:
	mutable bool suppressNotification_ = false;

	// Resolution and zone count for sine shaper (1024 steps, 8 zones)
	static constexpr int32_t kSineShaperResolution = 1024;
	static constexpr int32_t kSineShaperNumZones = 8;

	// Compute effective phase offset (twistPhaseOffset + 1024 * gammaPhase)
	[[nodiscard]] double effectivePhaseOffset() const {
		auto& ss = soundEditor.currentModControllable->sineShaper;
		return static_cast<double>(ss.twistPhaseOffset) + 1024.0 * static_cast<double>(ss.gammaPhase);
	}

	// Static storage for coordinate display (used by getCoordName callback)
	static inline char coordBuffer_[12] = {};
	static void cacheCoordDisplay(double phaseOffset, int32_t value) {
		// Format: "P:Z" where P=phaseOffset (int), Z=zone index (0-7)
		// 128 encoder clicks = 1 zone, so Z increments once per zone traversal
		int32_t p = static_cast<int32_t>(phaseOffset);
		int32_t z = value >> 7; // 0-1023 → 0-7 (zone index)
		snprintf(coordBuffer_, sizeof(coordBuffer_), "%d:%d", p, z);
	}
	static const char* getCoordName([[maybe_unused]] int32_t zoneIndex) { return coordBuffer_; }
};

// Mix: wet/dry blend (0-127, 0 = bypass)
// Secret menu: Push encoder to adjust gammaPhase (offsets twistPhaseOffset by 1024*gamma)
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

	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			// Secret menu: adjust gammaPhase (adds 1024*gamma to twistPhaseOffset in DSP)
			Buttons::selectButtonPressUsedUp = true;
			float& gamma = soundEditor.currentModControllable->sineShaper.gammaPhase;
			gamma = std::max(0.0f, gamma + static_cast<float>(offset) * 0.1f);
			// Show current value on display
			char buffer[16];
			snprintf(buffer, sizeof(buffer), "G:%d", static_cast<int32_t>(gamma * 10.0f));
			display->displayPopup(buffer);
			renderUIsForOled();           // Refresh display for consistency
			suppressNotification_ = true; // Prevent horizontal menu from overwriting popup
		}
		else {
			IntegerWithOff::selectEncoderAction(offset);
		}
	}

	[[nodiscard]] bool showNotification() const override {
		if (suppressNotification_) {
			suppressNotification_ = false;
			return false;
		}
		return true;
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }

	// Show "OFF" when mix=0 (effect bypassed)
	void renderInHorizontalMenu(const SlotPosition& slot) override {
		if (this->getValue() == 0) {
			deluge::hid::display::OLED::main.drawStringCentered("OFF", slot.start_x,
			                                                    slot.start_y + kHorizontalMenuSlotYOffset,
			                                                    kTextSpacingX, kTextSpacingY, slot.width);
			return;
		}
		IntegerWithOff::renderInHorizontalMenu(slot);
	}

private:
	mutable bool suppressNotification_ = false;
};

} // namespace deluge::gui::menu_item::fx
