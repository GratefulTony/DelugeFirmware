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

#include "retrospective_handler.h"
#include "definitions_cxx.hpp"
#include "extern.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/sample_marker_editor.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/instrument_clip_view.h"
#include "hid/display/display.h"
#include "hid/led/pad_leds.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/note/note_row.h"
#include "model/sample/sample.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "playback/playback_handler.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "processing/source.h"
#include "retrospective_buffer.h"
#include "storage/multi_range/multi_range.h"
#include "storage/multi_range/multisample_range.h"

/// Helper to create a new SoundDrum for a kit
static SoundDrum* createNewDrumForKit(Kit* kit) {
	// Create unique name for the drum
	String drum_name;
	drum_name.set("RETR");
	Error error = kit->makeDrumNameUnique(&drum_name, 1);
	if (error != Error::NONE) {
		return nullptr;
	}

	// Allocate memory for the drum
	void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(SoundDrum));
	if (!memory) {
		return nullptr;
	}

	// Set up param manager
	ParamManagerForTimeline param_manager;
	error = param_manager.setupWithPatching();
	if (error != Error::NONE) {
		delugeDealloc(memory);
		return nullptr;
	}

	// Initialize and create the drum
	Sound::initParams(&param_manager);
	SoundDrum* new_drum = new (memory) SoundDrum();
	new_drum->setupAsSample(&param_manager);

	// Back up param manager and set name
	currentSong->backUpParamManager(new_drum, currentSong->getCurrentClip(), &param_manager, true);
	new_drum->drumName = drum_name.get();
	new_drum->nameIsDiscardable = true;

	return new_drum;
}

void handleRetrospectiveSave() {
	bool bar_sync = retrospectiveBuffer.isBarMode() && playbackHandler.isEitherClockActive();

	// Show initial feedback
	if (bar_sync) {
		display->displayPopup("WAIT", 0);
	}
	else if (runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::RetrospectiveSamplerNormalize)) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_RETRO_NORMALIZING));
	}
	else {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_RETRO_SAVING));
	}

	// Save - bar-sync blocks until downbeat, time-based saves immediately
	String file_path;
	Error error =
	    bar_sync ? retrospectiveBuffer.requestBarSyncedSave(&file_path) : retrospectiveBuffer.saveToFile(&file_path);

	if (error != Error::NONE) {
		display->displayPopup("FAIL");
		return;
	}

	// Check if we're in a kit context
	bool loaded_to_pad = false;

	if (getCurrentOutputType() == OutputType::KIT) {
		Kit* kit = getCurrentKit();

		if (kit) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_RETRO_LOADING));

			InstrumentClip* clip = getCurrentInstrumentClip();
			if (clip) {
				char model_stack_memory[MODEL_STACK_MAX_SIZE];
				ModelStackWithTimelineCounter* model_stack =
				    currentSong->setupModelStackWithCurrentClip(model_stack_memory);

				SoundDrum* sound_drum = createNewDrumForKit(kit);
				if (sound_drum) {
					kit->addDrum(sound_drum);

					int32_t note_row_index = clip->noteRows.getNumElements();
					NoteRow* note_row = clip->noteRows.insertNoteRowAtIndex(note_row_index);

					if (note_row) {
						ModelStackWithNoteRow* model_stack_with_note_row =
						    model_stack->addNoteRow(note_row_index, note_row);
						note_row->setDrum(sound_drum, kit, model_stack_with_note_row);

						int32_t y_display = note_row_index - clip->yScroll;
						if (y_display < 0 || y_display >= kDisplayHeight) {
							clip->yScroll = note_row_index - (kDisplayHeight - 1);
							if (clip->yScroll < 0) {
								clip->yScroll = 0;
							}
						}
					}

					kit->beenEdited();
					instrumentClipView.setSelectedDrum(sound_drum, true);

					Source* source = &sound_drum->sources[0];
					MultiRange* range = source->getOrCreateFirstRange();
					if (range) {
						AudioFileHolder* holder = range->getAudioFileHolder();
						if (holder) {
							holder->setAudioFile(nullptr);
							holder->filePath.set(&file_path);
							Error load_error = holder->loadFile(false, true, true, CLUSTER_LOAD_IMMEDIATELY);

							if (load_error == Error::NONE) {
								Sample* sample = static_cast<Sample*>(holder->audioFile);
								if (sample) {
									source->repeatMode = (sample->getLengthInMSec() < 2002) ? SampleRepeatMode::ONCE
									                                                        : SampleRepeatMode::CUT;
								}
								loaded_to_pad = true;

								soundEditor.currentSound = sound_drum;
								soundEditor.currentSourceIndex = 0;
								soundEditor.currentSource = source;
								soundEditor.currentSampleControls = &source->sampleControls;
								soundEditor.currentMultiRange = static_cast<MultisampleRange*>(range);
								soundEditor.navigationDepth = 0;
								soundEditor.shouldGoUpOneLevelOnBegin = false;

								sampleMarkerEditor.markerType = MarkerType::START;
								display->setNextTransitionDirection(1);
								bool success = openUI(&sampleMarkerEditor);
								if (success) {
									PadLEDs::skipGreyoutFade();
									PadLEDs::sendOutSidebarColoursSoon();
								}

								uiNeedsRendering(&instrumentClipView);
							}
						}
					}
				}
			}
		}
	}

	if (!loaded_to_pad) {
		const char* full_path = file_path.get();
		const char* filename = full_path;
		for (const char* p = full_path; *p; p++) {
			if (*p == '/') {
				filename = p + 1;
			}
		}
		display->displayPopup(filename);
	}
}
