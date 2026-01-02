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

#include "dsp/zone_param.hpp" // For ZoneBasedParam
#include "gui/menu_item/decimal.h"
#include "gui/menu_item/menu_item_with_cc_learning.h"
#include "gui/menu_item/momentum_encoder.h" // VelocityEncoder and zone render helpers
#include "gui/menu_item/source_selection/regular.h"
#include "gui/ui/sound_editor.h"
#include "hid/buttons.h"
#include "model/model_stack.h"
#include "modulation/params/param.h"
#include "modulation/params/param_descriptor.h"
#include "modulation/params/param_set.h"
#include <cstdint>

namespace params = deluge::modulation::params;

namespace deluge::gui::menu_item {

// Re-export ZoneBasedParam from dsp namespace for convenience
using dsp::ZoneBasedParam;

// High resolution constants for zone-based menus (1024 steps)
constexpr int32_t kZoneHighResSteps = 1024;
constexpr int32_t kZoneHighResShift = 21; // 31 - 10 = 21 (2^10 = 1024)

/// Convert q31 param value to menu value (0-1024)
inline int32_t zoneParamToMenuValue(q31_t value) {
	constexpr q31_t kOverflowThreshold = 2147483647 - (1 << 20); // INT32_MAX - rounding term
	if (value > kOverflowThreshold) {
		return kZoneHighResSteps;
	}
	return (value + (1 << 20)) >> kZoneHighResShift;
}

/// Convert menu value (0-1024) to q31 param value
inline q31_t zoneMenuValueToParam(int32_t menuValue) {
	if (menuValue >= kZoneHighResSteps) {
		return 2147483647; // INT32_MAX
	}
	return menuValue << kZoneHighResShift;
}

/**
 * Base class for zone-based high-resolution menu items
 *
 * Provides:
 * - 1024 resolution with velocity-sensitive encoder
 * - Zone name rendering (OLED and horizontal menu)
 * - Common display value scaling (0-50)
 *
 * Derived classes must implement:
 * - readCurrentValue() / writeCurrentValue() for storage
 * - getZoneName(int32_t) for zone labels
 * - isRelevant() if gating is needed
 *
 * @tparam NUM_ZONES Number of zones (typically 8)
 */
template <int32_t NUM_ZONES = 8>
class ZoneBasedMenuItem : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	/// Override to provide zone name for each index (0 to NUM_ZONES-1)
	[[nodiscard]] virtual const char* getZoneName(int32_t zoneIndex) const = 0;

	[[nodiscard]] int32_t getMaxValue() const override { return kZoneHighResSteps; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	// Scale 0-1024 to 0-50 for display (matches gold knob popup range)
	[[nodiscard]] float getDisplayValue() override { return (this->getValue() * 50.0f) / kZoneHighResSteps; }

	void selectEncoderAction(int32_t offset) override {
		DecimalWithoutScrolling::selectEncoderAction(velocity_.getScaledOffset(offset));
	}

	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		// Capture 'this' to call virtual getZoneName
		renderZoneInHorizontalMenu(slot, this->getValue(), kZoneHighResSteps, NUM_ZONES,
		                           [this](int32_t z) { return this->getZoneName(z); });
	}

protected:
	void drawPixelsForOled() override {
		drawZoneForOled(this->getValue(), kZoneHighResSteps, NUM_ZONES,
		                [this](int32_t z) { return this->getZoneName(z); });
	}

	mutable VelocityEncoder velocity_;
};

/**
 * Zone-based menu item backed by an unpatched param
 *
 * Provides CC learning and reads/writes via UnpatchedParamSet.
 *
 * @tparam PARAM_ID The UnpatchedShared param ID
 * @tparam NUM_ZONES Number of zones (typically 8)
 */
template <params::UnpatchedShared PARAM_ID, int32_t NUM_ZONES = 8>
class ZoneBasedUnpatchedParam : public ZoneBasedMenuItem<NUM_ZONES>, public MenuItemWithCCLearning {
public:
	using ZoneBasedMenuItem<NUM_ZONES>::ZoneBasedMenuItem;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(PARAM_ID);
		this->setValue(zoneParamToMenuValue(value));
	}

	void writeCurrentValue() override {
		q31_t value = zoneMenuValueToParam(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam = modelStack->getUnpatchedAutoParamFromId(PARAM_ID);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(PARAM_ID + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}
};

/**
 * Zone-based menu item backed by a field on ModControllableAudio
 *
 * No CC learning by default. Derived class implements read/write
 * to access the specific field.
 *
 * @tparam NUM_ZONES Number of zones (typically 8)
 */
template <int32_t NUM_ZONES = 8>
class ZoneBasedFieldItem : public ZoneBasedMenuItem<NUM_ZONES> {
public:
	using ZoneBasedMenuItem<NUM_ZONES>::ZoneBasedMenuItem;
	// Derived class must implement readCurrentValue/writeCurrentValue
};

/**
 * Zone-based menu item with patched param for mod matrix routing
 *
 * Design: Menu controls a FIELD on the sound (base value), patched param provides
 * pure modulation (neutral = 0). DSP combines: field + scaledMod.
 *
 * Provides:
 * - Zone-based high-resolution display (1024 steps, 8 zones)
 * - Mod matrix routing via PatchedParam (press encoder to access sources)
 * - CC learning for the patched param
 *
 * Derived class must implement:
 * - getZoneName(int32_t) for zone labels
 * - getFieldValue() / setFieldValue() for the sound's field
 *
 * @tparam PARAM_ID The Local param ID for mod routing (e.g., LOCAL_SINE_SHAPER_TWIST)
 * @tparam NUM_ZONES Number of zones (typically 8)
 */
template <params::Local PARAM_ID, int32_t NUM_ZONES = 8>
class ZoneBasedPatchedParam : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	/// Override to provide zone name for each index (0 to NUM_ZONES-1)
	[[nodiscard]] virtual const char* getZoneName(int32_t zoneIndex) const = 0;

	/// Override to get the field value from the sound (q31_t, 0 to ONE_Q31)
	[[nodiscard]] virtual q31_t getFieldValue() const = 0;

	/// Override to set the field value on the sound
	virtual void setFieldValue(q31_t value) = 0;

	[[nodiscard]] int32_t getMaxValue() const override { return kZoneHighResSteps; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	// Scale 0-1024 to 0-50 for display (matches gold knob popup range)
	[[nodiscard]] float getDisplayValue() override { return (this->getValue() * 50.0f) / kZoneHighResSteps; }

	void selectEncoderAction(int32_t offset) override {
		DecimalWithoutScrolling::selectEncoderAction(velocity_.getScaledOffset(offset));
	}

	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		renderZoneInHorizontalMenu(slot, this->getValue(), kZoneHighResSteps, NUM_ZONES,
		                           [this](int32_t z) { return this->getZoneName(z); });
	}

	// === PatchedParam interface for mod matrix routing ===
	[[nodiscard]] int32_t getP() const { return PARAM_ID; }

	MenuItem* selectButtonPress() override {
		// If shift held down, user wants to delete automation
		if (Buttons::isShiftButtonPressed()) {
			return DecimalWithoutScrolling::selectButtonPress();
		}
		// Press encoder (without twist) opens mod matrix source selection
		soundEditor.patchingParamSelected = PARAM_ID;
		return &source_selection::regularMenu;
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(PARAM_ID);
		return paramDescriptor;
	}

	[[nodiscard]] deluge::modulation::params::Kind getParamKind() const {
		return deluge::modulation::params::Kind::PATCHED;
	}

	// Read from sound field (not patched param)
	void readCurrentValue() override { this->setValue(zoneParamToMenuValue(getFieldValue())); }

	// Write to sound field (not patched param)
	void writeCurrentValue() override { setFieldValue(zoneMenuValueToParam(this->getValue())); }

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

protected:
	void drawPixelsForOled() override {
		drawZoneForOled(this->getValue(), kZoneHighResSteps, NUM_ZONES,
		                [this](int32_t z) { return this->getZoneName(z); });
	}

	mutable VelocityEncoder velocity_;
};

} // namespace deluge::gui::menu_item
