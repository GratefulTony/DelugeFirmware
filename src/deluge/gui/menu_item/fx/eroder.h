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

#include "dsp/eroder.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/zone_based.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/song/song.h"
#include "modulation/params/param.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include <cmath>
#include <cstdio>

namespace params = deluge::modulation::params;

namespace deluge::gui::menu_item::fx {

// Eroder Freq: Frequency zone for allpass center frequency (8 zones: Sub → Full)
class EroderFreq final : public ZoneBasedDualParam<params::GLOBAL_ERODER_FREQ> {
public:
	using ZoneBasedDualParam::ZoneBasedDualParam;

	[[nodiscard]] q31_t getFieldValue() const override {
		return soundEditor.currentModControllable->ensureEroder().freq.value;
	}
	void setFieldValue(q31_t value) override { soundEditor.currentModControllable->ensureEroder().freq.value = value; }

	// Auto-wrap support for phi triangle phase exploration
	[[nodiscard]] bool supportsAutoWrap() const override { return true; }
	[[nodiscard]] float getPhaseOffset() const override {
		return soundEditor.currentModControllable->ensureEroder().freqPhaseOffset;
	}
	void setPhaseOffset(float offset) override {
		soundEditor.currentModControllable->ensureEroder().freqPhaseOffset = offset;
	}

	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			Buttons::selectButtonPressUsedUp = true;
			float& phase = soundEditor.currentModControllable->ensureEroder().freqPhaseOffset;
			phase = std::max(0.0f, phase + static_cast<float>(offset) * 128.0f);
			char buffer[16];
			snprintf(buffer, sizeof(buffer), "F:%d", static_cast<int32_t>(phase / 128.0f));
			display->displayPopup(buffer);
			renderUIsForOled();
			suppressNotification_ = true;
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

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "Sub";
		case 1:
			return "Bass";
		case 2:
			return "Low";
		case 3:
			return "Mid";
		case 4:
			return "High";
		case 5:
			return "Air";
		case 6:
			return "Bright";
		case 7:
			return "Full";
		default:
			return "?";
		}
	}

	[[nodiscard]] const char* getShortZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "SB";
		case 1:
			return "BA";
		case 2:
			return "LO";
		case 3:
			return "MI";
		case 4:
			return "HI";
		case 5:
			return "AI";
		case 6:
			return "BR";
		case 7:
			return "FL";
		default:
			return "??";
		}
	}

	// P:Z coordinate display when phase offset is active
	void renderInHorizontalMenu(const SlotPosition& slot) override {
		double phaseOffset = soundEditor.currentModControllable->ensureEroder().effectiveFreq();
		if (phaseOffset != 0.0) {
			cacheCoordDisplay(phaseOffset, this->getValue());
			renderZoneInHorizontalMenu(slot, this->getValue(), 1024, dsp::kEroderNumZones, getCoordName);
		}
		else {
			renderZoneInHorizontalMenu(slot, this->getValue(), 1024, dsp::kEroderNumZones,
			                           [this](int32_t z) { return this->getZoneName(z); });
		}
	}

protected:
	void drawPixelsForOled() override {
		double phaseOffset = soundEditor.currentModControllable->ensureEroder().effectiveFreq();
		if (phaseOffset != 0.0) {
			cacheCoordDisplay(phaseOffset, this->getValue());
			drawZoneForOled(this->getValue(), 1024, dsp::kEroderNumZones, getCoordName);
		}
		else {
			drawZoneForOled(this->getValue(), 1024, dsp::kEroderNumZones,
			                [this](int32_t z) { return this->getZoneName(z); });
		}
	}

private:
	mutable bool suppressNotification_ = false;

	static inline char coordBuffer_[12] = {};
	static void cacheCoordDisplay(double phaseOffset, int32_t value) {
		int32_t p = static_cast<int32_t>(std::floor(phaseOffset));
		int32_t z = value >> 7; // 0-1023 → 0-7
		snprintf(coordBuffer_, sizeof(coordBuffer_), "%d:%d", p, z);
	}
	static const char* getCoordName([[maybe_unused]] int32_t zoneIndex) { return coordBuffer_; }
};

// Eroder Character: SVF-filtered noise spectrum (8 zones: Deep → Open)
class EroderCharacter final : public ZoneBasedDualParam<params::GLOBAL_ERODER_CHARACTER> {
public:
	using ZoneBasedDualParam::ZoneBasedDualParam;

	[[nodiscard]] q31_t getFieldValue() const override {
		return soundEditor.currentModControllable->ensureEroder().character.value;
	}
	void setFieldValue(q31_t value) override {
		soundEditor.currentModControllable->ensureEroder().character.value = value;
	}

	// Auto-wrap support for phi triangle phase exploration
	[[nodiscard]] bool supportsAutoWrap() const override { return true; }
	[[nodiscard]] float getPhaseOffset() const override {
		return soundEditor.currentModControllable->ensureEroder().charPhaseOffset;
	}
	void setPhaseOffset(float offset) override {
		soundEditor.currentModControllable->ensureEroder().charPhaseOffset = offset;
	}

	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			Buttons::selectButtonPressUsedUp = true;
			float& phase = soundEditor.currentModControllable->ensureEroder().charPhaseOffset;
			phase = std::max(0.0f, phase + static_cast<float>(offset) * 128.0f);
			char buffer[16];
			snprintf(buffer, sizeof(buffer), "C:%d", static_cast<int32_t>(phase / 128.0f));
			display->displayPopup(buffer);
			renderUIsForOled();
			suppressNotification_ = true;
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

	[[nodiscard]] const char* getZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "Deep";
		case 1:
			return "Dark";
		case 2:
			return "Warm";
		case 3:
			return "Mid";
		case 4:
			return "Bright";
		case 5:
			return "Crisp";
		case 6:
			return "Air";
		case 7:
			return "Open";
		default:
			return "?";
		}
	}

	[[nodiscard]] const char* getShortZoneName(int32_t zoneIndex) const override {
		switch (zoneIndex) {
		case 0:
			return "DP";
		case 1:
			return "DK";
		case 2:
			return "WM";
		case 3:
			return "MI";
		case 4:
			return "BR";
		case 5:
			return "CR";
		case 6:
			return "AI";
		case 7:
			return "OP";
		default:
			return "??";
		}
	}

	// P:Z coordinate display when phase offset is active
	void renderInHorizontalMenu(const SlotPosition& slot) override {
		double phaseOffset = soundEditor.currentModControllable->ensureEroder().effectiveChar();
		if (phaseOffset != 0.0) {
			cacheCoordDisplay(phaseOffset, this->getValue());
			renderZoneInHorizontalMenu(slot, this->getValue(), 1024, dsp::kEroderNumZones, getCoordName);
		}
		else {
			renderZoneInHorizontalMenu(slot, this->getValue(), 1024, dsp::kEroderNumZones,
			                           [this](int32_t z) { return this->getZoneName(z); });
		}
	}

protected:
	void drawPixelsForOled() override {
		double phaseOffset = soundEditor.currentModControllable->ensureEroder().effectiveChar();
		if (phaseOffset != 0.0) {
			cacheCoordDisplay(phaseOffset, this->getValue());
			drawZoneForOled(this->getValue(), 1024, dsp::kEroderNumZones, getCoordName);
		}
		else {
			drawZoneForOled(this->getValue(), 1024, dsp::kEroderNumZones,
			                [this](int32_t z) { return this->getZoneName(z); });
		}
	}

private:
	mutable bool suppressNotification_ = false;

	static inline char coordBuffer_[12] = {};
	static void cacheCoordDisplay(double phaseOffset, int32_t value) {
		int32_t p = static_cast<int32_t>(std::floor(phaseOffset));
		int32_t z = value >> 7; // 0-1023 → 0-7
		snprintf(coordBuffer_, sizeof(coordBuffer_), "%d:%d", p, z);
	}
	static const char* getCoordName([[maybe_unused]] int32_t zoneIndex) { return coordBuffer_; }
};

// Eroder Depth: Modulation intensity (0=off/bypass, 1-127)
class EroderDepth final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->ensureEroder().depth); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->ensureEroder().depth = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->ensureEroder().depth = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }

	void renderInHorizontalMenu(const SlotPosition& slot) override {
		if (this->getValue() == 0) {
			deluge::hid::display::OLED::main.drawStringCentered("OFF", slot.start_x,
			                                                    slot.start_y + kHorizontalMenuSlotYOffset,
			                                                    kTextSpacingX, kTextSpacingY, slot.width);
			return;
		}
		IntegerWithOff::renderInHorizontalMenu(slot);
	}
};

// Eroder Mix: Wet/dry blend (0=dry, 127=wet)
class EroderMix final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->ensureEroder().mix); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->ensureEroder().mix = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->ensureEroder().mix = current_value;
		}
	}

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }

	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			// Push+twist: adjust gammaPhase (multiplier for all zone phase offsets)
			Buttons::selectButtonPressUsedUp = true;
			float& gamma = soundEditor.currentModControllable->ensureEroder().gammaPhase;
			gamma = std::max(0.0f, gamma + static_cast<float>(offset));
			char buffer[16];
			snprintf(buffer, sizeof(buffer), "G:%d", static_cast<int32_t>(gamma));
			display->displayPopup(buffer);
			renderUIsForOled();
			suppressNotification_ = true;
		}
		else {
			Integer::selectEncoderAction(offset);
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

} // namespace deluge::gui::menu_item::fx
