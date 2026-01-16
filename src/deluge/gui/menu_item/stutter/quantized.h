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
#include "gui/menu_item/toggle.h"
#include "gui/ui/sound_editor.h"
#include "model/drum/drum.h"
#include "model/fx/stutterer.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item::stutter {

/// Dual-mode menu: shows pWrite slider for Leaky mode, Quantize toggle otherwise
/// pWrite controls the probability of writing processed output back to buffer (0=never, 50=always)
class QuantizedStutter final : public IntegerContinuous {
public:
	using IntegerContinuous::IntegerContinuous;

	/// Check if current mode is Leaky
	bool isLeakyMode() const {
		return soundEditor.currentModControllable->stutterConfig.scatterMode == ScatterMode::Leaky;
	}

	void readCurrentValue() override {
		if (isLeakyMode()) {
			// pWrite mode: read leakyWriteProb as 0-50 value
			float prob = soundEditor.currentModControllable->stutterConfig.leakyWriteProb;
			this->setValue(static_cast<int32_t>(prob * 50.0f));
		}
		else {
			// Quantize mode: read quantized as 0 or 1, scale to display as 0 or 50
			this->setValue(soundEditor.currentModControllable->stutterConfig.quantized ? 50 : 0);
		}
	}

	void writeCurrentValue() override {
		if (isLeakyMode()) {
			// pWrite mode: write leakyWriteProb
			float prob = static_cast<float>(this->getValue()) / 50.0f;

			// If affect-entire button held, do whole kit
			if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
				Kit* kit = getCurrentKit();
				for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
					if (thisDrum->type == DrumType::SOUND) {
						auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
						if (soundDrum->stutterConfig.scatterMode == ScatterMode::Leaky) {
							soundDrum->stutterConfig.leakyWriteProb = prob;
						}
					}
				}
			}
			else {
				soundEditor.currentModControllable->stutterConfig.leakyWriteProb = prob;
			}
		}
		else {
			// Quantize mode: write quantized bool
			bool current_value = (this->getValue() >= 25); // Threshold at midpoint

			if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
				Kit* kit = getCurrentKit();
				for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
					if (thisDrum->type == DrumType::SOUND) {
						auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
						if (!soundEditor.currentModControllable->stutterConfig.useSongStutter) {
							soundDrum->stutterConfig.quantized = current_value;
						}
					}
				}
			}
			else {
				soundEditor.currentModControllable->stutterConfig.quantized = current_value;
			}
		}
	}

	bool usesAffectEntire() override { return true; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		// Always relevant: shows pWrite for Leaky mode, Quantize otherwise
		// For Quantize, still need to check useSongStutter
		if (isLeakyMode()) {
			return true;
		}
		return soundEditor.currentModControllable->isSong()
		       || !soundEditor.currentModControllable->stutterConfig.useSongStutter;
	}

	// === Display configuration ===
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 50; }

	void getColumnLabel(StringBuf& label) override {
		if (isLeakyMode()) {
			label.append("pWrite");
		}
		else {
			label.append("Quant");
		}
	}

	// Override to show different title based on mode
	std::string_view getTitle() const override {
		if (soundEditor.currentModControllable->stutterConfig.scatterMode == ScatterMode::Leaky) {
			return l10n::getView(l10n::String::STRING_FOR_SCATTER_PWRITE);
		}
		return l10n::getView(l10n::String::STRING_FOR_QUANTIZE);
	}
};

} // namespace deluge::gui::menu_item::stutter
