/*
 * Copyright © 2024-2025 Owlet Records
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
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */
#pragma once
#include "definitions_cxx.hpp"
#include "gui/menu_item/formatted_title.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/sample/utils.h"
#include "gui/ui/sound_editor.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound_drum.h"
#include "storage/multi_range/multisample_range.h"

namespace deluge::gui::menu_item::sample {
class LoopCrossfade final : public IntegerWithOff, public FormattedTitle {
public:
	LoopCrossfade(l10n::String name, l10n::String title_format_str, uint8_t source_id)
	    : IntegerWithOff(name), FormattedTitle(title_format_str, source_id + 1), source_id_{source_id} {}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }

	bool usesAffectEntire() override { return true; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t) override {
		return isSampleModeSample(modControllable, source_id_);
	}

	void readCurrentValue() override {
		Source& source = soundEditor.currentSound->sources[source_id_];
		if (source.ranges.getNumElements() && source.oscType == OscType::SAMPLE) {
			auto* multiRange = static_cast<MultisampleRange*>(source.ranges.getElement(0));
			setValue(multiRange->sampleHolder.loopCrossfadeMs);
		}
		else {
			setValue(0);
		}
	}

	void writeCurrentValue() override {
		int32_t val = getValue();
		if (val < 0) {
			val = 0;
		}

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			const Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					Source* source = &soundDrum->sources[source_id_];
					if (source->ranges.getNumElements() && source->oscType == OscType::SAMPLE) {
						auto* multiRange = static_cast<MultisampleRange*>(source->ranges.getElement(0));
						multiRange->sampleHolder.loopCrossfadeMs = static_cast<uint16_t>(val);
					}
				}
			}
		}
		else {
			Source& source = soundEditor.currentSound->sources[source_id_];
			if (source.ranges.getNumElements() && source.oscType == OscType::SAMPLE) {
				auto* multiRange = static_cast<MultisampleRange*>(source.ranges.getElement(0));
				multiRange->sampleHolder.loopCrossfadeMs = static_cast<uint16_t>(val);
			}
		}
	}

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 1000; }

private:
	uint8_t source_id_;
};
} // namespace deluge::gui::menu_item::sample
