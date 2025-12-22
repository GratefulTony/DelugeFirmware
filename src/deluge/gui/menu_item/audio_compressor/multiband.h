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
#include "model/mod_controllable/mod_controllable_audio.h"

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
class LowCrossover final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		float freqHz = soundEditor.currentModControllable->multibandCompressor.getLowCrossoverHz();
		// Map frequency to 0-127 range (50Hz to 500Hz)
		int32_t value = static_cast<int32_t>((freqHz - 50.0f) / (500.0f - 50.0f) * 127.0f);
		this->setValue(std::clamp<int32_t>(value, 0, 127));
	}

	void writeCurrentValue() override {
		// Map 0-127 to 50Hz-500Hz
		float freqHz = 50.0f + (static_cast<float>(this->getValue()) / 127.0f) * (500.0f - 50.0f);
		soundEditor.currentModControllable->multibandCompressor.setLowCrossover(freqHz);
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
class HighCrossover final : public DecimalWithoutScrolling {
public:
	using DecimalWithoutScrolling::DecimalWithoutScrolling;

	void readCurrentValue() override {
		float freqHz = soundEditor.currentModControllable->multibandCompressor.getHighCrossoverHz();
		// Map frequency to 0-127 range (1000Hz to 8000Hz)
		int32_t value = static_cast<int32_t>((freqHz - 1000.0f) / (8000.0f - 1000.0f) * 127.0f);
		this->setValue(std::clamp<int32_t>(value, 0, 127));
	}

	void writeCurrentValue() override {
		// Map 0-127 to 1000Hz-8000Hz
		float freqHz = 1000.0f + (static_cast<float>(this->getValue()) / 127.0f) * (8000.0f - 1000.0f);
		soundEditor.currentModControllable->multibandCompressor.setHighCrossover(freqHz);
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

} // namespace deluge::gui::menu_item::audio_compressor
