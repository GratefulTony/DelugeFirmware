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

#include "definitions_cxx.hpp"
#include "dsp/compressor/multiband.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/decimal.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/oled.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/model_stack.h"
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"

namespace params = deluge::modulation::params;

namespace deluge::gui::menu_item::audio_compressor {

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
		return {
		    l10n::getView(l10n::String::STRING_FOR_COMPRESSOR_MODE_SINGLE),
		    l10n::getView(l10n::String::STRING_FOR_COMPRESSOR_MODE_MULTIBAND),
		};
	}

	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }
};

/// Menu item for low crossover frequency (Hz)
/// Range: 50Hz to 2000Hz. Clamped to stay below high crossover.
class LowCrossover final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	static constexpr float kMinFreq = 50.0f;
	static constexpr float kMaxFreq = 2000.0f;
	static constexpr float kMinGap = 100.0f; // Minimum gap between low and high crossovers

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(
		    params::UNPATCHED_MB_COMPRESSOR_LOW_CROSSOVER);
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_LOW_CROSSOVER);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
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
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Menu item for high crossover frequency (Hz)
/// Range: 200Hz to 8000Hz. Clamped to stay above low crossover.
class HighCrossover final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	static constexpr float kMinFreq = 200.0f; // Fixed minimum for consistent knob feel
	static constexpr float kMaxFreq = 8000.0f;
	static constexpr float kMinGap = 100.0f; // Minimum gap between low and high crossovers

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(
		    params::UNPATCHED_MB_COMPRESSOR_HIGH_CROSSOVER);
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_HIGH_CROSSOVER);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
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
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Linked threshold control - sets threshold for all bands simultaneously
class LinkedThreshold final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(
		    params::UNPATCHED_MB_COMPRESSOR_THRESHOLD);
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(modelStackMemory);
		ModelStackWithAutoParam* modelStackWithParam =
		    modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_MB_COMPRESSOR_THRESHOLD);
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(value, modelStackWithParam);
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
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Linked ratio control - sets ratio for all bands simultaneously
class LinkedRatio final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getLinkedRatio();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.setAllRatios(value);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(0).getRatioForDisplay();
	}

	const char* getUnit() override { return " : 1"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Linked attack control - sets attack for all bands simultaneously
class LinkedAttack final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getLinkedAttack();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.setAllAttacks(value);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(0).getAttackMS();
	}

	const char* getUnit() override { return "MS"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return ATTACK; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Linked release control - sets release for all bands simultaneously
class LinkedRelease final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getLinkedRelease();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.setAllReleases(value);
	}

	[[nodiscard]] float getDisplayValue() override {
		return soundEditor.currentModControllable->multibandCompressor.getBand(0).getReleaseMS();
	}

	const char* getUnit() override { return "MS"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 1; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return RELEASE; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Character control (replaces knee) - controls width, knee, timing, skew via zones
/// Zones: Width, Timing, Skew, Punch, Air, Rich, OTT, OWLTT
class Character final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getCharacter();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.setCharacter(value);
	}

	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}

protected:
	void drawPixelsForOled() override {
		using namespace deluge::hid::display;
		auto zone = soundEditor.currentModControllable->multibandCompressor.getCharacterZone();
		const char* zoneName = getZoneName(zone);

		// Draw zone name at top (smaller font)
		OLED::main.drawStringCentred(zoneName, 8 + OLED_MAIN_TOPMOST_PIXEL, kTextSpacingX, kTextSpacingY);

		// Draw numeric value below (larger font)
		char buffer[8];
		intToString(this->getValue(), buffer);
		OLED::main.drawStringCentred(buffer, 24 + OLED_MAIN_TOPMOST_PIXEL, kTextHugeSpacingX, kTextHugeSizeY);
	}

private:
	static const char* getZoneName(deluge::dsp::CharacterZone zone) {
		switch (zone) {
		case deluge::dsp::CharacterZone::Width:
			return "Width";
		case deluge::dsp::CharacterZone::Timing:
			return "Timing";
		case deluge::dsp::CharacterZone::Skew:
			return "Skew";
		case deluge::dsp::CharacterZone::Punch:
			return "Punch";
		case deluge::dsp::CharacterZone::Air:
			return "Air";
		case deluge::dsp::CharacterZone::Rich:
			return "Rich";
		case deluge::dsp::CharacterZone::OTT:
			return "OTT";
		case deluge::dsp::CharacterZone::OWLTT:
			return "OWLTT";
		default:
			return "?";
		}
	}
};

/// Up/Down ratio skew control (balance between upward and downward compression)
class UpDownSkew final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getUpDownSkew();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.setUpDownSkew(value);
	}

	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Vibe control - controls phase relationships between oscillations in Feel
/// Zones: Sync, Spread, Pairs, Cascade, Invert, Pulse, Drift, Chaos
class Vibe final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getVibe();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.setVibe(value);
	}

	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}

protected:
	void drawPixelsForOled() override {
		using namespace deluge::hid::display;
		auto zone = soundEditor.currentModControllable->multibandCompressor.getVibeZone();
		const char* zoneName = getZoneName(zone);

		// Draw zone name at top (smaller font)
		OLED::main.drawStringCentred(zoneName, 8 + OLED_MAIN_TOPMOST_PIXEL, kTextSpacingX, kTextSpacingY);

		// Draw numeric value below (larger font)
		char buffer[8];
		intToString(this->getValue(), buffer);
		OLED::main.drawStringCentred(buffer, 24 + OLED_MAIN_TOPMOST_PIXEL, kTextHugeSpacingX, kTextHugeSizeY);
	}

private:
	static const char* getZoneName(deluge::dsp::VibeZone zone) {
		switch (zone) {
		case deluge::dsp::VibeZone::Sync:
			return "Sync";
		case deluge::dsp::VibeZone::Spread:
			return "Spread";
		case deluge::dsp::VibeZone::Pairs:
			return "Pairs";
		case deluge::dsp::VibeZone::Cascade:
			return "Cascade";
		case deluge::dsp::VibeZone::Invert:
			return "Invert";
		case deluge::dsp::VibeZone::Pulse:
			return "Pulse";
		case deluge::dsp::VibeZone::Drift:
			return "Drift";
		case deluge::dsp::VibeZone::Chaos:
			return "Chaos";
		default:
			return "?";
		}
	}
};

/// Global output gain control
class OutputGain final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getOutputGain();
		// Add 0.5 LSB before truncating to round instead of truncate (prevents jumps on first edit)
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.setOutputGain(value);
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
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Per-band threshold control (parameterized by band index)
template <size_t BAND_INDEX>
class BandThreshold final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).getThresholdDown();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).setThresholdDown(value);
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
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Per-band ratio control (parameterized by band index)
template <size_t BAND_INDEX>
class BandRatio final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).getRatioDown();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).setRatioDown(value);
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
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Per-band bandwidth control (gap between up/down thresholds)
template <size_t BAND_INDEX>
class BandBandwidth final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).getBandwidth();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).setBandwidth(value);
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
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Per-band output level control (post-compression, like OTT's L/M/H sliders)
/// CCW = -inf, 12:00 = 0dB, CW = +16dB
template <size_t BAND_INDEX>
class BandOutputLevel final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		q31_t value = soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).getOutputLevel();
		this->setValue((value + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		q31_t value = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.getBand(BAND_INDEX).setOutputLevel(value);
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
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

/// Multiband wet/dry blend control
class MultibandBlend final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		FixedPoint<31> blend = soundEditor.currentModControllable->multibandCompressor.getBlend();
		this->setValue((blend.raw() + (1 << 23)) >> 24);
	}

	void writeCurrentValue() override {
		FixedPoint<31> blend;
		blend.raw() = lshiftAndSaturate<24>(this->getValue());
		soundEditor.currentModControllable->multibandCompressor.setBlend(blend);
	}

	[[nodiscard]] float getDisplayValue() override {
		FixedPoint<31> blend = soundEditor.currentModControllable->multibandCompressor.getBlend();
		return static_cast<float>(blend) * 100.0f;
	}

	const char* getUnit() override { return "%"; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxKnobPos; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 0; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }
	[[nodiscard]] int32_t getColumnSpan() const override { return 1; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return modControllable->compressorMode == CompressorMode::MULTIBAND;
	}
};

} // namespace deluge::gui::menu_item::audio_compressor
