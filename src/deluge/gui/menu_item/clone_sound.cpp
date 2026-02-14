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

#include "gui/menu_item/clone_sound.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/session_view.h"
#include "hid/display/display.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item {

void CloneSound::beginSession(MenuItem* navigatedBackwardFrom) {
	Output* output = getCurrentOutput();
	Clip* clip = getCurrentClip();

	if (output->type == OutputType::KIT && !getRootUI()->getAffectEntire()) {
		// Clone the current drum to a new row at the bottom of the kit
		auto* sourceDrum = static_cast<SoundDrum*>(soundEditor.currentSound);
		auto* kit = static_cast<Kit*>(output);

		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack = currentSong->setupModelStackWithCurrentClip(modelStackMemory);

		instrumentClipView.cloneDrumToBottom(sourceDrum, kit, modelStack);
		soundEditor.exitCompletely();
	}
	else if (output->type == OutputType::SYNTH) {
		// Clone the synth track
		sessionView.cloneSynthTrack(clip);
		soundEditor.exitCompletely();
	}
	else {
		soundEditor.goUpOneLevel();
	}
}

} // namespace deluge::gui::menu_item
