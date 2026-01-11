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

#include "OSLikeStuff/scheduler_api.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/fx/sine_shaper.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/velocity_encoder.h"
#include "gui/ui/sound_editor.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item::fx {

// Deferred shaper regeneration - avoids blocking audio thread
namespace shaper_regen {
inline ModControllableAudio* pending{nullptr};
inline TaskID taskId{-1};

inline void regenerateCallback() {
	if (pending) {
		pending->shaperDsp.regenerateIfDirty();
		pending = nullptr;
	}
	taskId = -1;
}

inline void scheduleRegeneration(ModControllableAudio* mca) {
	// Pre-allocate buffers NOW on UI thread (safe time) - never allocate during deferred task
	mca->shaperDsp.ensureBuffersAllocated();
	pending = mca;
	if (taskId < 0) {
		// Priority 40 = idle time only, won't preempt other tasks
		taskId = addOnceTask(regenerateCallback, 40, 0.1, "shaper_regen", RESOURCE_NONE);
	}
}
} // namespace shaper_regen

// Drive: Bipolar patched param for shaper input gain
// Gold knob press toggles AA via Sound::modEncoderButtonAction
using TableShaperDrive = DynamicsPatchedParam;

constexpr int32_t kShaperHighResSteps = 1024;
constexpr int32_t kShaperNumZones = 8;

// Shape X: Soft→Hard axis (0-127)
// Controls knee/clipping aggressiveness
class TableShaperShapeX final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->shaper.shapeX); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		auto* mca = soundEditor.currentModControllable;

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->shaper.shapeX = current_value;
					soundDrum->shaperDsp.regenerateTable(current_value, soundDrum->shaper.shapeY,
					                                     soundDrum->shaper.phaseOffset);
					soundDrum->shaperDsp.regenerateIfDirty(); // Direct for bulk kit operation
				}
			}
		}
		else {
			mca->shaper.shapeX = current_value;
			mca->shaperDsp.regenerateTable(current_value, mca->shaper.shapeY, mca->shaper.phaseOffset);
			shaper_regen::scheduleRegeneration(mca);
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }

	/// Click encoder to toggle subharmonic effect (octave-down modulation)
	/// Drift is always on when phaseOffset != 0; this toggles the sub-octave effect
	MenuItem* selectButtonPress() override {
		auto* mca = soundEditor.currentModControllable;
		mca->shaper.subEnabled = !mca->shaper.subEnabled;
		display->displayPopup(mca->shaper.subEnabled ? "SUB" : "OFF");
		return NO_NAVIGATION;
	}

	void selectEncoderAction(int32_t offset) override {
		IntegerWithOff::selectEncoderAction(offset);
		if (this->getValue() == 0) {
			display->displayPopup("OFF");
		}
	}

	// Show "OFF" in horizontal menu when X=0
	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		if (this->getValue() == 0) {
			deluge::hid::display::OLED::main.drawStringCentered("OFF", slot.start_x,
			                                                    slot.start_y + kHorizontalMenuSlotYOffset,
			                                                    kTextSpacingX, kTextSpacingY, slot.width);
			return;
		}
		IntegerWithOff::renderInHorizontalMenu(slot);
	}
};

// Shape Y (UI: "Color"): Sweeps through saturation characters
// Secret menu: Push+twist to adjust shaper.phaseOffset
class TableShaperShapeY final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->shaper.shapeY); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		auto* mca = soundEditor.currentModControllable;

		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					soundDrum->shaper.shapeY = current_value;
					soundDrum->shaperDsp.regenerateTable(soundDrum->shaper.shapeX, current_value,
					                                     soundDrum->shaper.phaseOffset);
					soundDrum->shaperDsp.regenerateIfDirty(); // Direct for bulk kit operation
				}
			}
		}
		else {
			mca->shaper.shapeY = current_value;
			mca->shaperDsp.regenerateTable(mca->shaper.shapeX, current_value, mca->shaper.phaseOffset);
			shaper_regen::scheduleRegeneration(mca);
		}
	}
	[[nodiscard]] int32_t getMaxValue() const override { return kShaperHighResSteps - 1; } // 0-1023
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }

	void selectEncoderAction(int32_t offset) override {
		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			// Secret: push+twist adjusts shaper.phaseOffset
			Buttons::selectButtonPressUsedUp = true;
			float& phaseOffset = soundEditor.currentModControllable->shaper.phaseOffset;
			phaseOffset += static_cast<float>(velocity_.getScaledOffset(offset)) * 0.1f;
			// Regenerate table with new phaseOffset
			auto* mca = soundEditor.currentModControllable;
			mca->shaperDsp.regenerateTable(mca->shaper.shapeX, mca->shaper.shapeY, phaseOffset);
			shaper_regen::scheduleRegeneration(mca);
			// Show current value on display
			char buffer[12];
			intToString(static_cast<int32_t>(phaseOffset * 10.0f), buffer);
			display->displayPopup(buffer);
			suppressNotification_ = true;
		}
		else {
			IntegerWithOff::selectEncoderAction(velocity_.getScaledOffset(offset));
		}
	}

	[[nodiscard]] bool showNotification() const override {
		if (suppressNotification_) {
			suppressNotification_ = false;
			return false;
		}
		return true;
	}

	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		renderZoneInHorizontalMenu(slot, this->getValue(), kShaperHighResSteps, kShaperNumZones, getZoneName);
	}

protected:
	void drawPixelsForOled() override {
		drawZoneForOled(this->getValue(), kShaperHighResSteps, kShaperNumZones, getZoneName);
	}

private:
	mutable VelocityEncoder velocity_;
	mutable bool suppressNotification_ = false;

	static const char* getZoneName(int32_t zoneIndex) {
		switch (zoneIndex) {
		case 0:
			return "Warm"; // Tanh-dominant, smooth saturation
		case 1:
			return "Bright"; // Polynomial-dominant, upper harmonics
		case 2:
			return "Clip"; // Hard knee dominant, aggressive
		case 3:
			return "Fold"; // Chebyshev T5, wavefolder character
		case 4:
			return "Gold"; // Sine folder, rich harmonics
		case 5:
			return "Diode"; // Rectifier, asymmetric/even harmonics
		case 6:
			return "Blend"; // Mixed basis functions
		case 7:
			return "Morph"; // Complex combinations
		default:
			return "?";
		}
	}
};

// Mix: Patched param for amplitude-dependent wet/dry blend (0 = dry, max = full wet)
// Uses bipolar param range (INT32_MIN to INT32_MAX) displayed as 0-128
class TableShaperMix : public patched_param::Integer {
public:
	static constexpr int32_t kMixMenuRange = 128;

	using patched_param::Integer::Integer;

	void selectEncoderAction(int32_t offset) override {
		auto* mca = soundEditor.currentModControllable;
		bool wasZero = (this->getValue() == 0);
		// Auto-enable X when turning up mix from 0
		if (mca->shaper.shapeX == 0 && offset > 0) {
			mca->shaper.shapeX = 1;
			mca->shaperDsp.regenerateTable(1, mca->shaper.shapeY, mca->shaper.phaseOffset);
			shaper_regen::scheduleRegeneration(mca);
		}
		// Regenerate tables when mix goes from 0 to non-zero (may have been skipped at load)
		else if (wasZero && offset > 0 && mca->shaper.shapeX > 0) {
			mca->shaperDsp.regenerateTable(mca->shaper.shapeX, mca->shaper.shapeY, mca->shaper.phaseOffset);
			shaper_regen::scheduleRegeneration(mca);
		}

		int32_t newValue = this->getValue() + offset;
		if (newValue > kMixMenuRange) {
			newValue = kMixMenuRange;
		}
		else if (newValue < 0) {
			newValue = 0;
		}
		this->setValue(newValue);

		if (Buttons::isButtonPressed(hid::button::SELECT_ENC)) {
			Buttons::selectButtonPressUsedUp = true;
		}
		writeCurrentValue();
		if (display->haveOLED()) {
			renderUIsForOled();
		}
		else {
			drawValue();
		}
	}

protected:
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMixMenuRange; }

	void readCurrentValue() override {
		// Scale bipolar param (INT32_MIN to INT32_MAX) to menu range (0 to 128)
		int32_t paramValue = soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP());
		// Same formula as computeCurrentValueForStandardMenuItem but with 128 range
		int32_t menuValue = (((int64_t)paramValue + 2147483648) * kMixMenuRange + 2147483648) >> 32;
		this->setValue(menuValue);
	}

	int32_t getFinalValue() override {
		int32_t value = this->getValue();
		// Scale 0-128 back to bipolar param (INT32_MIN to INT32_MAX)
		if (value >= kMixMenuRange) {
			return 2147483647; // INT32_MAX
		}
		if (value <= 0) {
			return -2147483648; // INT32_MIN
		}
		// (2^32 / 128) = 33554432
		return static_cast<int32_t>(static_cast<uint32_t>(value) * 33554432) - 2147483648;
	}
};

} // namespace deluge::gui::menu_item::fx
