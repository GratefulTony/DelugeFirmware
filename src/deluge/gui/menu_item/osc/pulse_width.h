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
#include "definitions_cxx.hpp"
#include "gui/menu_item/formatted_title.h"
#include "gui/menu_item/source/patched_param.h"
#include "gui/ui/sound_editor.h"
#include "modulation/params/param_set.h"
#include "processing/sound/sound.h"
#include "util/waves.h"

namespace deluge::gui::menu_item::osc {
class PulseWidth final : public source::PatchedParam, public FormattedTitle {
public:
	PulseWidth(l10n::String name, l10n::String title_format_str, int32_t newP, uint8_t source_id)
	    : PatchedParam(name, newP, source_id), FormattedTitle(title_format_str, source_id + 1) {}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }

	/// Check if current oscillator is TRIANGLE_PW
	[[nodiscard]] bool isTrianglePW() const {
		if (soundEditor.currentSound == nullptr) {
			return false;
		}
		return soundEditor.currentSound->sources[source_id_].oscType == OscType::TRIANGLE_PW;
	}

	int32_t getFinalValue() override { return computeFinalValueForHalfPrecisionMenuItem(this->getValue()); }

	void readCurrentValue() override {
		int32_t paramValue = soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP());
		this->setValue(computeCurrentValueForHalfPrecisionMenuItem(paramValue));
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		const auto sound = static_cast<Sound*>(modControllable);
		if (sound->getSynthMode() == SynthMode::FM) {
			return false;
		}

		const OscType oscType = sound->sources[source_id_].oscType;
		if (oscType == OscType::WAVETABLE) {
			auto& source = sound->sources[source_id_];
			return source.hasAtLeastOneAudioFileLoaded();
		}

		return oscType != OscType::SAMPLE && oscType != OscType::INPUT_L && oscType != OscType::INPUT_R
		       && oscType != OscType::INPUT_STEREO;
	}

	void renderInHorizontalMenu(const HorizontalMenuSlotParams& slot) override {
		oled_canvas::Canvas& image = OLED::main;

		const float norm = getValue() / 50.0f;

		constexpr int32_t x_padding = 4;
		const uint8_t width = slot.width - x_padding * 2;

		int32_t start_x = slot.start_x + x_padding;
		int32_t end_x = start_x + width - 1;
		int32_t start_y = slot.start_y + kHorizontalMenuSlotYOffset;
		int32_t end_y = slot.start_y + slot.height - 5;

		if (isTrianglePW()) {
			// TRIANGLE_PW: Render actual waveform using triangleWithDeadzoneBipolar
			int32_t mid_y = (start_y + end_y) / 2;
			int32_t half_height = (end_y - start_y) / 2;
			int32_t total_width = end_x - start_x;

			// Match oscillator: inverted so norm 0 = full cycle, norm 1 = narrow pulses (like square wave)
			// Clamp before shift to prevent overflow (float 1.0 * 0x7FFFFFFF can round to 0x80000000)
			uint32_t pulseWidthEquiv = static_cast<uint32_t>(norm * 0x7FFFFFFF);
			if (pulseWidthEquiv > 0x7FFFFFFFu) {
				pulseWidthEquiv = 0x7FFFFFFFu;
			}
			uint32_t phaseWidth = 0xFFFFFFFF - (pulseWidthEquiv << 1);

			// Minimum display width to ensure visibility (at least 4 pixels worth)
			constexpr uint32_t kMinPhaseWidth = 0x00800000;
			uint32_t minDisplayWidth = (0xFFFFFFFF / total_width) * 4;
			uint32_t displayPhaseWidth = (phaseWidth < minDisplayWidth)  ? minDisplayWidth
			                             : (phaseWidth < kMinPhaseWidth) ? kMinPhaseWidth
			                                                             : phaseWidth;

			// Phase increment per pixel
			uint32_t phaseInc = 0xFFFFFFFF / total_width;

			uint32_t phase = 0;
			int32_t prevY = mid_y;
			for (int32_t x = start_x; x <= end_x; x++) {
				int32_t value = triangleWithDeadzoneBipolar(phase, displayPhaseWidth);
				// Map value (-0x3FFFFFFF to +0x3FFFFFFF) to y (end_y to start_y)
				// getTriangleSmall outputs half-scale, so shift by 30 total instead of 31
				int32_t y = mid_y - ((value >> 23) * half_height >> 7);
				image.drawLine(x - 1, prevY, x, y);
				prevY = y;
				phase += phaseInc;
			}
		}
		else {
			// Standard square wave pulse width visualization
			int32_t pw_min_x = start_x + 2;
			int32_t pw_max_x = start_x + width / 2;
			int32_t pw_width = pw_max_x - pw_min_x;
			int32_t pw_x = pw_max_x - pw_width * norm;

			image.drawVerticalLine(start_x, start_y, end_y);
			image.drawHorizontalLine(start_y, start_x, pw_x);
			image.drawVerticalLine(pw_x, start_y, end_y);
			image.drawHorizontalLine(end_y, pw_x, end_x);
		}
	}
};

} // namespace deluge::gui::menu_item::osc
