/*
 * Copyright © 2024 Synthstrom Audible Limited
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

#include "dynamics_sound_design.h"
#include "gui/l10n/l10n.h"

namespace deluge::gui::menu_item::runtime_feature {

// Menu items for the submenu
static SettingToggle menuDynamicsEnabled(RuntimeFeatureSettingType::DynamicsSoundDesign);
static SettingToggle menuDynamicsFineGoldKnobCharacter(RuntimeFeatureSettingType::DynamicsFineGoldKnobCharacter);
static SettingToggle menuDynamicsFineGoldKnobVibe(RuntimeFeatureSettingType::DynamicsFineGoldKnobVibe);
static SettingToggle menuDOTTAnalyzer(RuntimeFeatureSettingType::DOTTAnalyzer);
static SettingToggle menuDisperserHiCPU(RuntimeFeatureSettingType::DisperserHiCPU);

DynamicsSoundDesignMenu::DynamicsSoundDesignMenu()
    : Submenu(l10n::String::STRING_FOR_COMMUNITY_FEATURE_DYNAMICS_SOUND_DESIGN,
              {&menuDynamicsEnabled, &menuDynamicsFineGoldKnobCharacter, &menuDynamicsFineGoldKnobVibe,
               &menuDOTTAnalyzer, &menuDisperserHiCPU}) {
}

std::string_view DynamicsSoundDesignMenu::getTitle() const {
	return l10n::getView(l10n::String::STRING_FOR_COMMUNITY_FEATURE_DYNAMICS_SOUND_DESIGN);
}

// Global instance
DynamicsSoundDesignMenu menuDynamicsSoundDesignSubmenu;

} // namespace deluge::gui::menu_item::runtime_feature
