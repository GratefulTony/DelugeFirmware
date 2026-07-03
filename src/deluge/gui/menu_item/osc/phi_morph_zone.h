/*
 * Copyright © 2025 Owlet Records
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
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "processing/sound/sound.h"
#include <algorithm>
#include <cstdio>

namespace deluge::gui::menu_item::osc {

inline constexpr int32_t kPhiMorphNumZones = 8;
inline constexpr int32_t kPhiMorphZoneResolution = 1024;

class PhiMorphZone final : public ZoneBasedMenuItem<kPhiMorphNumZones, kPhiMorphZoneResolution>, public FormattedTitle {
public:
	PhiMorphZone(l10n::String name, l10n::String title_format_str, uint8_t source_id, uint8_t zone_id)
	    : ZoneBasedMenuItem(name), FormattedTitle(title_format_str, source_id + 1), sourceId_{source_id},
	      zoneId_{zone_id} {}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }

	void readCurrentValue() override {
		auto& source = soundEditor.currentSound->sources[sourceId_];
		uint16_t val;
		if (isWeave()) {
			val = (zoneId_ == 0) ? source.phiWeaveZoneA : source.phiWeaveZoneB;
		}
		else {
			val = (zoneId_ == 0) ? source.phiMorphZoneA : source.phiMorphZoneB;
		}
		this->setValue(static_cast<int32_t>(val));
	}

	void writeCurrentValue() override {
		auto& source = soundEditor.currentSound->sources[sourceId_];
		uint16_t val = static_cast<uint16_t>(this->getValue());
		if (isWeave()) {
			((zoneId_ == 0) ? source.phiWeaveZoneA : source.phiWeaveZoneB) = val;
		}
		else {
			((zoneId_ == 0) ? source.phiMorphZoneA : source.phiMorphZoneB) = val;
		}
	}

	[[nodiscard]] bool supportsAutoWrap() const override { return true; }

	[[nodiscard]] float getPhaseOffset() const override {
		auto& source = soundEditor.currentSound->sources[sourceId_];
		if (isWeave()) {
			return (zoneId_ == 0) ? source.phiWeavePhaseOffsetA : source.phiWeavePhaseOffsetB;
		}
		return (zoneId_ == 0) ? source.phiMorphPhaseOffsetA : source.phiMorphPhaseOffsetB;
	}

	void setPhaseOffset(float offset) override {
		auto& source = soundEditor.currentSound->sources[sourceId_];
		float& target = isWeave() ? ((zoneId_ == 0) ? source.phiWeavePhaseOffsetA : source.phiWeavePhaseOffsetB)
		                          : ((zoneId_ == 0) ? source.phiMorphPhaseOffsetA : source.phiMorphPhaseOffsetB);
		target = offset;
	}

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		static const char* const kMorphNames[8] = {"Ember", "Coral", "Prism", "Jade",
		                                           "Azure", "Ivory", "Slate", "Onyx"};
		static const char* const kWeaveNames[8] = {"Silk", "Wool", "Reed", "Vine", "Bone", "Glass", "Steel", "Storm"};
		if (zoneIndex < 0 || zoneIndex >= 8) {
			return "?";
		}
		return isWeave() ? kWeaveNames[zoneIndex] : kMorphNames[zoneIndex];
	}

	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			// Push+twist: manually adjust phi triangle phase offset
			Buttons::selectButtonPressUsedUp = true;
			auto& source = soundEditor.currentSound->sources[sourceId_];
			float& phase = isWeave() ? ((zoneId_ == 0) ? source.phiWeavePhaseOffsetA : source.phiWeavePhaseOffsetB)
			                         : ((zoneId_ == 0) ? source.phiMorphPhaseOffsetA : source.phiMorphPhaseOffsetB);
			phase = std::max(0.0f, phase + static_cast<float>(velocity_.getScaledOffset(offset)) * 1.0f);
			char buffer[16];
			snprintf(buffer, sizeof(buffer), "P:%d", static_cast<int32_t>(std::floor(effectivePhaseOffset())));
			display->displayPopup(buffer);
			renderUIsForOled();
			suppressNotification_ = true;
		}
		else {
			ZoneBasedMenuItem::selectEncoderAction(offset);
		}
	}

	[[nodiscard]] bool showNotification() const override {
		if (suppressNotification_) {
			suppressNotification_ = false;
			return false;
		}
		return true;
	}

	void renderInHorizontalMenu(const SlotPosition& slot) override {
		float effOffset = effectivePhaseOffset();
		if (effOffset != 0.0f) {
			cacheCoordDisplay(effOffset, this->getValue());
			renderZoneInHorizontalMenu(slot, this->getValue(), kPhiMorphZoneResolution, kPhiMorphNumZones,
			                           getCoordName);
		}
		else {
			ZoneBasedMenuItem::renderInHorizontalMenu(slot);
		}
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		const auto sound = static_cast<Sound*>(modControllable);
		OscType type = sound->sources[sourceId_].oscType;
		return type == OscType::PHI_MORPH || type == OscType::PHI_WEAVE;
	}

protected:
	void drawPixelsForOled() override {
		float effOffset = effectivePhaseOffset();
		if (effOffset != 0.0f) {
			cacheCoordDisplay(effOffset, this->getValue());
			drawZoneForOled(this->getValue(), kPhiMorphZoneResolution, kPhiMorphNumZones, getCoordName);
		}
		else {
			ZoneBasedMenuItem::drawPixelsForOled();
		}
	}

private:
	uint8_t sourceId_;
	uint8_t zoneId_; // 0 = Zone A, 1 = Zone B
	mutable bool suppressNotification_ = false;

	[[nodiscard]] bool isWeave() const {
		return soundEditor.currentSound->sources[sourceId_].oscType == OscType::PHI_WEAVE;
	}

	[[nodiscard]] float effectivePhaseOffset() const {
		auto& source = soundEditor.currentSound->sources[sourceId_];
		float knobOffset = getPhaseOffset();
		float gamma = isWeave() ? source.phiWeaveGamma : source.phiMorphGamma;
		return knobOffset + static_cast<float>(kPhiMorphZoneResolution) * gamma;
	}

	static inline char coordBuffer_[12] = {};
	static void cacheCoordDisplay(float phaseOffset, int32_t value) {
		int32_t p = static_cast<int32_t>(std::floor(phaseOffset));
		int32_t z = value >> 7; // 0-1023 -> 0-7 (zone index)
		snprintf(coordBuffer_, sizeof(coordBuffer_), "%d:%d", p, z);
	}
	static const char* getCoordName([[maybe_unused]] int32_t zoneIndex) { return coordBuffer_; }
};

} // namespace deluge::gui::menu_item::osc
