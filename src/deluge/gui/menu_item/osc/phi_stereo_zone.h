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
#include "gui/menu_item/formatted_title.h"
#include "gui/menu_item/zone_based.h"
#include "gui/ui/sound_editor.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::osc {

// Zone-style stereo knob for the phi family: position WITHIN each 128-wide
// zone is the stereo AMOUNT (every zone starts mono - knob 0 is always mono),
// the zone index selects the stereo CHARACTER, interpreted per oscillator
// (WEAVE/GENDY: dual pickup taps with distance/tilt/counter-scan; SWARM:
// slave separation; VOX: formant separation).
class PhiStereoZone final : public ZoneBasedMenuItem<8, 1024>, public FormattedTitle {
public:
	PhiStereoZone(l10n::String name, l10n::String title_format_str, uint8_t source_id)
	    : ZoneBasedMenuItem(name), FormattedTitle(title_format_str, source_id + 1), sourceId_{source_id} {}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }

	void readCurrentValue() override {
		this->setValue(static_cast<int32_t>(soundEditor.currentSound->sources[sourceId_].phiStereoZone));
	}

	void writeCurrentValue() override {
		soundEditor.currentSound->sources[sourceId_].phiStereoZone = static_cast<uint16_t>(this->getValue());
	}

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		static const char* const kNames[8] = {"Slim", "Near", "Open", "Tilt", "Wide", "Sway", "Split", "Vast"};
		if (zoneIndex < 0 || zoneIndex >= 8) {
			return "?";
		}
		return kNames[zoneIndex];
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		const auto sound = static_cast<Sound*>(modControllable);
		return sound->sources[sourceId_].phiStereoCapable();
	}

private:
	uint8_t sourceId_;
};

} // namespace deluge::gui::menu_item::osc
