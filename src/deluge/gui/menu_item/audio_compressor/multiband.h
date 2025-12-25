/*
 * Copyright © 2024 Synthstrom Audible Limited
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

#include <cstring>

#include "definitions_cxx.hpp"
#include "dsp/compressor/multiband.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/decimal.h"
#include "gui/menu_item/menu_item_with_cc_learning.h"
#include "gui/menu_item/momentum_encoder.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/value_scaling.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/oled.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/model_stack.h"
#include "model/settings/runtime_feature_settings.h"
#include "modulation/params/param.h"
#include "modulation/params/param_descriptor.h"
#include "modulation/params/param_set.h"

namespace params = deluge::modulation::params;

namespace deluge::gui::menu_item::audio_compressor {

// Helper to convert param value (0 to INT32_MAX) to menu value (0 to 128)
// Handles overflow that would occur when adding rounding term to values near INT32_MAX
inline int32_t paramToMenuValue128(q31_t value) {
	// Values above this threshold would overflow when adding (1 << 23)
	constexpr q31_t kOverflowThreshold = 2147483647 - (1 << 23); // INT32_MAX - rounding term
	if (value > kOverflowThreshold) {
		return 128;
	}
	return (value + (1 << 23)) >> 24;
}

// High resolution: 1024 steps (8x more precision than 128)
// Used for params that display as percentages where finer control is beneficial
constexpr int32_t kHighResSteps = 1024;
constexpr int32_t kHighResShift = 21; // 31 - 10 = 21 (2^10 = 1024)

inline int32_t paramToMenuValueHighRes(q31_t value) {
	constexpr q31_t kOverflowThreshold = 2147483647 - (1 << 20); // INT32_MAX - rounding term
	if (value > kOverflowThreshold) {
		return kHighResSteps;
	}
	return (value + (1 << 20)) >> kHighResShift;
}

inline q31_t menuValueToParamHighRes(int32_t menuValue) {
	if (menuValue >= kHighResSteps) {
		return 2147483647; // INT32_MAX
	}
	return menuValue << kHighResShift;
}

/// Menu item to select compressor mode (Single or Multiband)
class CompressorModeSelection final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override {
		this->setValue(static_cast<int32_t>(soundEditor.currentModControllable->compressorMode));
	}

	void writeCurrentValue() override {
		soundEditor.currentModControllable->compressorMode = static_cast<CompressorMode>(this->getValue());
	}

	deluge::vector<std::string_view> getOptions(OptType optType = OptType::FULL) override {
		if (runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)) {
			return {
			    l10n::getView(l10n::String::STRING_FOR_COMPRESSOR_MODE_SINGLE),
			    l10n::getView(l10n::String::STRING_FOR_COMPRESSOR_MODE_MULTIBAND),
			};
		}
		return {
		    l10n::getView(l10n::String::STRING_FOR_COMPRESSOR_MODE_SINGLE),
		};
	}

	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }
};

/// Menu item for low crossover frequency (Hz)
/// Range: 50Hz to 2000Hz. Clamped to stay below high crossover.
class LowCrossover final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	static constexpr float kMinFreq = 50.0f;
	static constexpr float kMaxFreq = 2000.0f;
	static constexpr float kMinGap = 100.0f; // Minimum gap between low and high crossovers

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(
		    params::UNPATCHED_MB_COMPRESSOR_LOW_CROSSOVER);
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_LOW_CROSSOVER);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_LOW_CROSSOVER + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getLowCrossoverHz();
	}

	const char* getUnit() override { return "HZ"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Menu item for high crossover frequency (Hz)
/// Range: 200Hz to 8000Hz. Clamped to stay above low crossover.
class HighCrossover final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	static constexpr float kMinFreq = 200.0f; // Fixed minimum for consistent knob feel
	static constexpr float kMaxFreq = 8000.0f;
	static constexpr float kMinGap = 100.0f; // Minimum gap between low and high crossovers

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(
		    params::UNPATCHED_MB_COMPRESSOR_HIGH_CROSSOVER);
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_HIGH_CROSSOVER);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_HIGH_CROSSOVER + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getHighCrossoverHz();
	}

	const char* getUnit() override { return "HZ"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Linked threshold control - sets threshold for all bands simultaneously
class LinkedThreshold final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(
		    params::UNPATCHED_MB_COMPRESSOR_THRESHOLD);
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_THRESHOLD);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_THRESHOLD + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(0).getThresholdForDisplay();
	}

	const char* getUnit() override { return "DB"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Linked ratio control - sets ratio for all bands simultaneously
class LinkedRatio final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value =
		    soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_MB_COMPRESSOR_RATIO);
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_RATIO);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(0).getRatioForDisplay();
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_RATIO + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	const char* getUnit() override { return " : 1"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Linked attack control - sets attack for all bands simultaneously
class LinkedAttack final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value =
		    soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_MB_COMPRESSOR_ATTACK);
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_ATTACK);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(0).getAttackMS();
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_ATTACK + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	const char* getUnit() override { return "MS"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return ATTACK; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Linked release control - sets release for all bands simultaneously
class LinkedRelease final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value =
		    soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_MB_COMPRESSOR_RELEASE);
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_RELEASE);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(0).getReleaseMS();
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_RELEASE + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	const char* getUnit() override { return "MS"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return RELEASE; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Character control (replaces knee) - controls width, knee, timing, skew via zones
/// Zones: Width, Timing, Skew, Punch, Air, Rich, OTT, OWLTT
class Character final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(
		    params::UNPATCHED_MB_COMPRESSOR_CHARACTER);
		this->setValue(paramToMenuValueHighRes(value));
	}

	void writeCurrentValue() override {
		q31_t value = menuValueToParamHighRes(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_CHARACTER);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_CHARACTER + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	[[nodiscard]] int32_t getMaxValue() const override { return kHighResSteps; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	// Scale 0-1024 to 0-50 for display (matches gold knob popup range)
	// 1 decimal place since we have finer internal resolution
	[[nodiscard]] float getDisplayValue() override { return (this->getValue() * 50.0f) / kHighResSteps; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}

	void selectEncoderAction(int32_t offset) override {
		DecimalWithoutScrolling::selectEncoderAction(momentum_.getScaledOffset(offset));
	}

	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		renderZoneInHorizontalMenu(slot, this->getValue(), kHighResSteps, 8, getZoneName);
	}

protected:
	void drawPixelsForOled() override { drawZoneForOled(this->getValue(), kHighResSteps, 8, getZoneName); }

private:
	static const char* getZoneName(int32_t zoneIndex) {
		switch (zoneIndex) {
		case 0:
			return "Width";
		case 1:
			return "Timing";
		case 2:
			return "Skew";
		case 3:
			return "Punch";
		case 4:
			return "Air";
		case 5:
			return "Rich";
		case 6:
			return "OTT";
		case 7:
			return "OWLTT";
		default:
			return "?";
		}
	}

	mutable MomentumEncoder momentum_;
};

/// Up/Down ratio skew control (balance between upward and downward compression)
class UpDownSkew final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value =
		    soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_MB_COMPRESSOR_SKEW);
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_SKEW);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_SKEW + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Vibe control - controls phase relationships between oscillations in Feel
/// Zones: Sync, Spread, Pairs, Cascade, Invert, Pulse, Drift, Chaos
class Vibe final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value =
		    soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_MB_COMPRESSOR_VIBE);
		this->setValue(paramToMenuValueHighRes(value));
	}

	void writeCurrentValue() override {
		q31_t value = menuValueToParamHighRes(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_VIBE);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_VIBE + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	[[nodiscard]] int32_t getMaxValue() const override { return kHighResSteps; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	// Scale 0-1024 to 0-50 for display (matches gold knob popup range)
	// 1 decimal place since we have finer internal resolution
	[[nodiscard]] float getDisplayValue() override { return (this->getValue() * 50.0f) / kHighResSteps; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}

	void selectEncoderAction(int32_t offset) override {
		DecimalWithoutScrolling::selectEncoderAction(momentum_.getScaledOffset(offset));
	}

	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		renderZoneInHorizontalMenu(slot, this->getValue(), kHighResSteps, 8, getZoneName);
	}

protected:
	void drawPixelsForOled() override { drawZoneForOled(this->getValue(), kHighResSteps, 8, getZoneName); }

private:
	static const char* getZoneName(int32_t zoneIndex) {
		switch (zoneIndex) {
		case 0:
			return "Sync";
		case 1:
			return "Spread";
		case 2:
			return "Pairs";
		case 3:
			return "Cascade";
		case 4:
			return "Invert";
		case 5:
			return "Pulse";
		case 6:
			return "Drift";
		case 7:
			return "Chaos";
		default:
			return "?";
		}
	}

	mutable MomentumEncoder momentum_;
};

/// Global output gain control
class OutputGain final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(
		    params::UNPATCHED_MB_COMPRESSOR_OUTPUT_GAIN);
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_OUTPUT_GAIN);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_OUTPUT_GAIN + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	[[nodiscard]] float getDisplayValue() override {
		float linear = soundEditor.currentModControllable->multibandCompressor.getOutputGainLinear();
		// Convert to dB for display
		return 20.0f * std::log10(linear);
	}

	const char* getUnit() override { return "DB"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Per-band threshold control (parameterized by band index)
/// Displays NET value (linked + offset), stores offset when adjusted
template <size_t BAND_INDEX>
class BandThreshold final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		// Display NET value (linked + offset)
		q31_t netValue = soundEditor.currentModControllable->multibandCompressor.getNetThreshold(BAND_INDEX);
		this->setValue(paramToMenuValue128(netValue));
	}

	void writeCurrentValue() override {
		// Calculate offset = newNetValue - linkedValue
		q31_t newNetValue = lshiftAndSaturate<24>(this->getValue());
		q31_t linkedValue = soundEditor.currentModControllable->multibandCompressor.getLinkedThreshold();
		q31_t offset = newNetValue - linkedValue;
		soundEditor.currentModControllable->multibandCompressor.setThresholdOffset(BAND_INDEX, offset);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).getThresholdForDisplay();
	}

	const char* getUnit() override { return "DB"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Per-band ratio control (parameterized by band index)
/// Displays NET value (linked + offset), stores offset when adjusted
template <size_t BAND_INDEX>
class BandRatio final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		// Display NET value (linked + offset)
		q31_t netValue = soundEditor.currentModControllable->multibandCompressor.getNetRatio(BAND_INDEX);
		this->setValue(paramToMenuValue128(netValue));
	}

	void writeCurrentValue() override {
		// Calculate offset = newNetValue - linkedValue
		q31_t newNetValue = lshiftAndSaturate<24>(this->getValue());
		q31_t linkedValue = soundEditor.currentModControllable->multibandCompressor.getLinkedRatio();
		q31_t offset = newNetValue - linkedValue;
		soundEditor.currentModControllable->multibandCompressor.setRatioOffset(BAND_INDEX, offset);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).getRatioForDisplay();
	}

	const char* getUnit() override { return " : 1"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Per-band bandwidth control (gap between up/down thresholds)
/// Displays NET value (linked + offset), stores offset when adjusted
template <size_t BAND_INDEX>
class BandBandwidth final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		// Display NET value (linked + offset)
		q31_t netValue = soundEditor.currentModControllable->multibandCompressor.getNetBandwidth(BAND_INDEX);
		this->setValue(paramToMenuValue128(netValue));
	}

	void writeCurrentValue() override {
		// Calculate offset = newNetValue - linkedValue
		q31_t newNetValue = lshiftAndSaturate<24>(this->getValue());
		q31_t linkedValue = soundEditor.currentModControllable->multibandCompressor.getLinkedBandwidth();
		q31_t offset = newNetValue - linkedValue;
		soundEditor.currentModControllable->multibandCompressor.setBandwidthOffset(BAND_INDEX, offset);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).getBandwidthForDisplay();
	}

	const char* getUnit() override { return "DB"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Per-band output level control (post-compression, like OTT's L/M/H sliders)
/// CCW = -inf, 12:00 = 0dB, CW = +16dB
template <size_t BAND_INDEX>
class BandOutputLevel final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	static constexpr int32_t getParamId() {
		if constexpr (BAND_INDEX == 0) {
			return params::UNPATCHED_MB_COMPRESSOR_LOW_LEVEL;
		}
		else if constexpr (BAND_INDEX == 1) {
			return params::UNPATCHED_MB_COMPRESSOR_MID_LEVEL;
		}
		else {
			return params::UNPATCHED_MB_COMPRESSOR_HIGH_LEVEL;
		}
	}

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(getParamId());
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam = modelStack->getUnpatchedAutoParamFromId(getParamId());
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(getParamId() + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	[[nodiscard]] float getDisplayValue() override {
		float linear =
		    soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).getOutputLevelLinear();
		// Convert to dB for display
		return 20.0f * std::log10(linear + 1e-10f);
	}

	const char* getUnit() override { return "DB"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

/// Mode zone control - first item in DOTT menu
/// Zone 0 = disabled, Zone 1 = enabled
class ModeZone final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getEnabledZone();
		// Map 0-ONE_Q31 to 0-128 range
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.setEnabledZone(value);
	}

	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}

	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		renderZoneInHorizontalMenu(slot, this->getValue(), kMaxKnobPos, 2, getZoneName);
	}

protected:
	void drawPixelsForOled() override { drawZoneForOled(this->getValue(), kMaxKnobPos, 2, getZoneName); }

private:
	static const char* getZoneName(int32_t zoneIndex) {
		switch (zoneIndex) {
		case 0:
			return "Off";
		case 1:
			return "On";
		default:
			return "?";
		}
	}
};

/// Multiband wet/dry blend control
class MultibandBlend final : public DecimalWithoutScrolling, public MenuItemWithCCLearning {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value =
		    soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_MB_COMPRESSOR_BLEND);
		this->setValue(paramToMenuValue128(value));
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_BLEND);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
	}

	ParamDescriptor getLearningThing() override {
		ParamDescriptor paramDescriptor;
		paramDescriptor.setToHaveParamOnly(params::UNPATCHED_MB_COMPRESSOR_BLEND + params::UNPATCHED_START);
		return paramDescriptor;
	}

	void unlearnAction() final { MenuItemWithCCLearning::unlearnAction(); }
	bool allowsLearnMode() final { return MenuItemWithCCLearning::allowsLearnMode(); }
	void learnKnob(MIDICable* cable, int32_t whichKnob, int32_t modKnobMode, int32_t midiChannel) final {
		MenuItemWithCCLearning::learnKnob(cable, whichKnob, modKnobMode, midiChannel);
	}

	[[nodiscard]] float getDisplayValue() override {
		q31_t value =
		    soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_MB_COMPRESSOR_BLEND);
		return (static_cast<float>(value) / ONE_Q31f) * 100.0f;
	}

	const char* getUnit() override { return "%"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign)
		       && modControllable->multibandCompressor.isEnabled();
	}
};

} // namespace deluge::gui::menu_item::audio_compressor
