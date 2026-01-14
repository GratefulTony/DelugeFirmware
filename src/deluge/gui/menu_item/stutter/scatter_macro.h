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

#include "gui/menu_item/menu_item_with_cc_learning.h"
#include "gui/menu_item/patched_param/integer.h"
#include "gui/menu_item/source_selection/regular.h"
#include "gui/ui/sound_editor.h"
#include "model/model_stack.h"
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"

namespace params = deluge::modulation::params;

namespace deluge::gui::menu_item::stutter {

/// Scatter macro parameter - dual patched/unpatched param for macro control
/// Uses GLOBAL_SCATTER_MACRO when in Sound context, UNPATCHED_SCATTER_MACRO for GlobalEffectable
class ScatterMacro final : public patched_param::Integer {
public:
	using patched_param::Integer::Integer;

	// Override to use GLOBAL_SCATTER_MACRO
	[[nodiscard]] int32_t getP() const { return params::GLOBAL_SCATTER_MACRO; }

	void readCurrentValue() override {
		q31_t value;
		if (soundEditor.currentParamManager->containsPatchedParamSetCollection()) {
			value = soundEditor.currentParamManager->getPatchedParamSet()->getValue(params::GLOBAL_SCATTER_MACRO);
		}
		else {
			value = soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_SCATTER_MACRO);
		}
		this->setValue(value >> 24); // Convert q31 to 0-127 range
	}

	ModelStackWithAutoParam* getModelStack(void* memory) override {
		ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(memory);
		if (soundEditor.currentParamManager->containsPatchedParamSetCollection()) {
			return modelStack->getPatchedAutoParamFromId(params::GLOBAL_SCATTER_MACRO);
		}
		return modelStack->getUnpatchedAutoParamFromId(params::UNPATCHED_SCATTER_MACRO);
	}

	int32_t getFinalValue() override {
		int32_t value = this->getValue();
		if (value >= 128) {
			return 2147483647; // INT32_MAX
		}
		return value << 24;
	}

	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return 50; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }

	void getColumnLabel(StringBuf& label) override { label.append("Macro"); }
};

} // namespace deluge::gui::menu_item::stutter
