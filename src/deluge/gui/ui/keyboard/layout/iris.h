/*
 * Copyright © 2026 Owlet Records
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

#include "gui/ui/keyboard/chords.h"
#include "gui/ui/keyboard/layout/column_controls.h"

namespace deluge::gui::ui::keyboard::layout {

// ============================================================================
// IRIS layout - one-finger chords with automatic voice leading
//
// Inspired by Telepathic Instruments' Orchid: the left hand latches a chord
// QUALITY (major/minor/dominant/dim/aug/other) and a COMPLEXITY (which chord
// within that quality's family - triad, 6th, 7th, 9th...), and a single root
// pad plays the whole chord. The voicing is chosen automatically by a
// voice-leading engine: candidates (stored voicings x octave shifts) are
// scored against the previously sounding chord for minimal voice motion,
// biased by the VOICING slider from tight/close through open/spread.
// Changing quality, complexity, bass or voicing while a root is held
// re-voices the sounding chord live.
//
// Grid (bottom to top):
//   row 0: roots (12 chromatic pads from the song root) + octave selector
//   row 1: slash bass (latching; sounds below the chord)
//   row 2: complexity 0-9 within the current quality family
//   row 3: quality buttons
//   row 4: voicing slider (voice-leading bias: close .. spread)
// ============================================================================

class KeyboardLayoutIris : public ColumnControlsKeyboard {
public:
	KeyboardLayoutIris() = default;
	~KeyboardLayoutIris() override = default;

	void evaluatePads(PressedPad presses[kMaxNumKeyboardPadPresses]) override;
	void handleVerticalEncoder(int32_t offset) override;
	void handleHorizontalEncoder(int32_t offset, bool shiftEnabled, PressedPad presses[kMaxNumKeyboardPadPresses],
	                             bool encoderPressed) override;
	void precalculate() override;

	void renderPads(RGB image[][kDisplayWidth + kSideBarWidth]) override;

	l10n::String name() override { return l10n::String::STRING_FOR_KEYBOARD_LAYOUT_IRIS; }
	bool supportsInstrument() override { return true; }
	RequiredScaleMode requiredScaleMode() override { return RequiredScaleMode::Disabled; }

private:
	void handleControlPad(int32_t x, int32_t y);
	void triggerChord(int32_t rootPadX);
	void computeVoicing(int32_t rootNote, const Chord& chord);
	void announceChord(int32_t rootNote, const Chord& chord);

	// Voice-leading memory: the previously sounding chord (survives release -
	// the NEXT chord flows from it, which is what makes progressions connect)
	uint8_t prevNotes[kMaxChordKeyboardSize] = {0};
	int32_t prevCount = 0;

	// Currently computed chord (cached against identity so held-chord
	// re-evaluations don't re-voice unless a control actually changed)
	uint8_t curNotes[kMaxChordKeyboardSize] = {0};
	int32_t curCount = 0;
	uint32_t curIdentity = 0xFFFFFFFF;

	// Edge detection for control pads (bass is a toggle - it must latch once
	// per physical press, not on every evaluate while held)
	uint32_t controlHandledMask = 0;
};

} // namespace deluge::gui::ui::keyboard::layout
