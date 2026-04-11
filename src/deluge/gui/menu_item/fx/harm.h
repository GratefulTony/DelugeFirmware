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

#include "dsp/harm.h"
#include "gui/menu_item/integer.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound_drum.h"
#include <cstdio>

namespace deluge::gui::menu_item::fx {

class HarmHarmonic final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->harm.harmonic); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->harm.harmonic = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->harm.harmonic = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return deluge::dsp::HarmParams::kNumHarmonics; }

private:
	// Format ratio as "n:d" string into buffer, returns buffer
	static const char* formatRatio(int32_t val, char* buf, size_t bufSize) {
		if (val == 0) {
			return "OFF";
		}
		int32_t idx = std::min(val - 1, deluge::dsp::HarmParams::kNumHarmonics - 1);
		auto& entry = deluge::dsp::HarmParams::kHarmonicTable[idx];
		if (entry.den == 1) {
			snprintf(buf, bufSize, "%d", entry.num);
		}
		else {
			snprintf(buf, bufSize, "%d:%d", entry.num, entry.den);
		}
		return buf;
	}

	void drawPixelsForOled() override {
		char buf[8];
		const char* text = formatRatio(this->getValue(), buf, sizeof(buf));
		deluge::hid::display::OLED::main.drawStringCentered(text, 0, 20, kTextSpacingX, kTextSpacingY,
		                                                    OLED_MAIN_WIDTH_PIXELS);
	}

	void renderInHorizontalMenu(const SlotPosition& slot) override {
		char buf[8];
		const char* text = formatRatio(this->getValue(), buf, sizeof(buf));
		deluge::hid::display::OLED::main.drawStringCentered(
		    text, slot.start_x, slot.start_y + kHorizontalMenuSlotYOffset, kTextSpacingX, kTextSpacingY, slot.width);
	}
};

class HarmLevel final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->harm.level); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->harm.level = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->harm.level = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

class HarmPhase final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->harm.phase); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->harm.phase = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->harm.phase = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

class HarmAttack final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->harm.attack); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->harm.attack = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->harm.attack = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

class HarmRelease final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->harm.release); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->harm.release = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->harm.release = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

class HarmHpf final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->harm.hpf); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->harm.hpf = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->harm.hpf = current_value;
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

class HarmPorta final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->harm.porta); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->harm.porta = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->harm.porta = current_value;
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

} // namespace deluge::gui::menu_item::fx
