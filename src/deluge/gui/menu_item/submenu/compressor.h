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

#include "definitions_cxx.hpp"
#include "deluge/gui/l10n/l10n.h"
#include "deluge/gui/menu_item/horizontal_menu.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui_timer_manager.h"
#include "hid/display/oled.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/settings/runtime_feature_settings.h"

namespace deluge::gui::menu_item::submenu {

/// Specialized HorizontalMenu for DOTT that displays a GR meter in the header.
/// The meter shows:
/// - 3 bars for Low/Mid/High band gain reduction
/// - 1 bar for output level
/// - 1 dot for clip indicator
/// The meter updates automatically when audio passes through the compressor.
/// Visibility is gated by the DynamicsSoundDesign community feature.
class CompressorHorizontalMenu final : public HorizontalMenu {
public:
	using HorizontalMenu::HorizontalMenu;

	static constexpr int32_t kMeterRefreshMs = 100; // 10 fps for meter animation

	/// Gate the entire DOTT menu based on DynamicsSoundDesign feature toggle.
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DynamicsSoundDesign);
	}

	void beginSession(MenuItem* navigatedBackwardFrom = nullptr) override {
		HorizontalMenu::beginSession(navigatedBackwardFrom);

		// Always start meter refresh timer - it will check conditions on each tick
		if (isMeterFeatureEnabled()) {
			uiTimerManager.setTimer(TimerName::UI_SPECIFIC, kMeterRefreshMs);
		}
	}

	void endSession() override {
		// Stop the meter refresh timer
		uiTimerManager.unsetTimer(TimerName::UI_SPECIFIC);
		HorizontalMenu::endSession();
	}

	ActionResult timerCallback() override {
		// Always reschedule to keep animation running
		uiTimerManager.setTimer(TimerName::UI_SPECIFIC, kMeterRefreshMs);

		// Timer owns the decision: check ALL conditions before triggering render
		if (shouldShowMeter()) {
			// Clear the audio loop's refresh flag
			(void)soundEditor.currentModControllable->multibandCompressor.checkAndClearMeterRefresh();

			// Request full UI redraw - this triggers the standard OLED render path
			renderUIsForOled();
		}
		return ActionResult::DEALT_WITH;
	}

	void renderOLED() override {
		// Call base implementation for standard rendering
		HorizontalMenu::renderOLED();

		// Draw meter if conditions are met
		// Note: This is also called from non-timer sources (navigation, etc.)
		// so we still need the full condition check here
		if (shouldShowMeter()) {
			renderGRMeter();
		}
	}

	/// Override selectEncoderAction - don't draw meter during value changes
	/// The popup notification takes priority for readability
	void selectEncoderAction(int32_t offset) override {
		// Call base implementation (updates value and shows notification)
		HorizontalMenu::selectEncoderAction(offset);
		// Don't draw meter here - popup is showing and needs to be readable
	}

private:
	/// Check if meter feature is enabled (used by beginSession to decide whether to start timer)
	[[nodiscard]] bool isMeterFeatureEnabled() const {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::DOTTAnalyzer);
	}

	/// Authoritative check for meter display - used by both timer and render
	/// Timer uses this to avoid unnecessary renderUIsForOled() calls
	/// Render uses this because renderOLED() can also be triggered by navigation/input
	[[nodiscard]] bool shouldShowMeter() const {
		return isMeterFeatureEnabled() && soundEditor.currentModControllable != nullptr
		       && soundEditor.currentModControllable->multibandCompressor.isEnabled() && !OLED::isPopupPresent();
	}

	// Meter layout constants (shared between clear and render)
	static constexpr int32_t kMeterHeight = 13;
	static constexpr int32_t kMeterX = 45;
	static constexpr int32_t kMeterWidth =
	    24; // 3 bands(2px each) + 3 gaps(2px) + separator(2px) + master(2px) + clip(4px)

	/// Clear the meter area before redrawing (needed for direct rendering in timer)
	void clearMeterArea() {
		OLED::main.clearAreaExact(kMeterX, OLED_MAIN_TOPMOST_PIXEL, kMeterX + kMeterWidth,
		                          OLED_MAIN_TOPMOST_PIXEL + kMeterHeight);
	}

	/// Render GR meter with dual bars per band (output level + GR)
	/// Layout: [L:out|gr] [M:out|gr] [H:out|gr] | [Master] [Clip]
	void renderGRMeter() {
		auto& compressor = soundEditor.currentModControllable->multibandCompressor;

		// Header height (13px to stay within title area)
		constexpr int32_t meterHeight = kMeterHeight;
		constexpr int32_t halfHeight = meterHeight / 2;
		constexpr int32_t bandGap = 2; // Gap between bands

		// Position - moved left since "DOTT" is short
		constexpr int32_t meterX = kMeterX;
		constexpr int32_t meterY = OLED_MAIN_TOPMOST_PIXEL;
		const int32_t centerY = meterY + halfHeight;
		const int32_t bottomY = meterY + meterHeight - 1;

		oled_canvas::Canvas& canvas = OLED::main;

		// Scale bipolar GR value to half-height pixels
		auto scaleBipolar = [](int8_t value, int32_t maxHalfHeight) -> int32_t {
			return (static_cast<int32_t>(value) * maxHalfHeight) / 127;
		};

		// Scale unipolar value (0-127) to full height
		auto scaleUnipolar = [](uint8_t value, int32_t maxHeight) -> int32_t {
			return (static_cast<int32_t>(value) * maxHeight) / 127;
		};

		// Draw a band meter: output level bar + GR bar + saturation indicator
		auto drawBandMeter = [&](int32_t xPos, size_t bandIndex) {
			uint8_t outputLevel = compressor.getBandOutputLevel(bandIndex);
			int8_t grValue = compressor.getBandGainReduction(bandIndex);
			bool saturating = compressor.isBandSaturating(bandIndex);

			// Bar 1: Output level (unipolar, grows upward from bottom)
			// Leave 3px at top for saturation indicator (2px) + 1px gap
			int32_t outH = scaleUnipolar(outputLevel, meterHeight - 3);
			for (int32_t dy = 0; dy < outH; dy++) {
				canvas.drawPixel(xPos, bottomY - dy);
			}

			// Saturation indicator at top (2px wide spanning both columns, 2px tall)
			if (saturating) {
				canvas.drawPixel(xPos, meterY);
				canvas.drawPixel(xPos + 1, meterY);
				canvas.drawPixel(xPos, meterY + 1);
				canvas.drawPixel(xPos + 1, meterY + 1);
			}

			// Bar 2: GR (bipolar, from center)
			xPos += 1;
			int32_t h = scaleBipolar(grValue, halfHeight);
			// Draw center tick
			canvas.drawPixel(xPos, centerY);
			if (h > 0) {
				// Upward compression (gain boost)
				for (int32_t dy = 1; dy <= h; dy++) {
					canvas.drawPixel(xPos, centerY - dy);
				}
			}
			else if (h < 0) {
				// Downward compression (gain reduction)
				for (int32_t dy = 1; dy <= -h; dy++) {
					canvas.drawPixel(xPos, centerY + dy);
				}
			}
		};

		int32_t x = meterX;

		// Draw L/M/H band meters (each is 2px wide: output + GR)
		drawBandMeter(x, 0); // Low
		x += 2 + bandGap;

		drawBandMeter(x, 1); // Mid
		x += 2 + bandGap;

		drawBandMeter(x, 2); // High
		x += 2 + bandGap;

		// Separator (vertical dots at center)
		canvas.drawPixel(x, centerY - 1);
		canvas.drawPixel(x, centerY);
		canvas.drawPixel(x, centerY + 1);
		x += 2;

		// Master output level bar (2px wide for visibility)
		uint8_t outLevel = compressor.getOutputLevel();
		int32_t outH = scaleUnipolar(outLevel, meterHeight);
		for (int32_t dy = 0; dy < outH; dy++) {
			canvas.drawPixel(x, bottomY - dy);
			canvas.drawPixel(x + 1, bottomY - dy);
		}

		// Master clip indicator - top-right of output meter (2x2 dot when clipping)
		if (compressor.isClipping()) {
			int32_t clipX = x + 3;
			canvas.drawPixel(clipX, meterY);
			canvas.drawPixel(clipX + 1, meterY);
			canvas.drawPixel(clipX, meterY + 1);
			canvas.drawPixel(clipX + 1, meterY + 1);
		}

		OLED::markChanged();
	}
};

} // namespace deluge::gui::menu_item::submenu
