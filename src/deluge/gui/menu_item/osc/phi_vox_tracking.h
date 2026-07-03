/*
 * Copyright © 2026 Owlet Records
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
#include "gui/menu_item/integer.h"
#include "gui/ui/sound_editor.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::osc {

// PHI_VOX formant tracking: 0 = formants fixed in Hz (vocal behavior - identity
// survives transposition), 50 = formants locked to note-relative ratios
// (harmonic overtone behavior - timbre transposes with pitch). Blendable.
class PhiVoxTracking final : public Integer {
public:
	PhiVoxTracking(l10n::String name, uint8_t source_id) : Integer(name), sourceId_{source_id} {}

	void readCurrentValue() override { this->setValue(soundEditor.currentSound->sources[sourceId_].phiVoxTracking); }

	void writeCurrentValue() override {
		soundEditor.currentSound->sources[sourceId_].phiVoxTracking = static_cast<uint8_t>(this->getValue());
	}

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 50; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		const auto sound = static_cast<Sound*>(modControllable);
		return sound->sources[sourceId_].oscType == OscType::PHI_VOX;
	}

private:
	uint8_t sourceId_;
};

} // namespace deluge::gui::menu_item::osc
