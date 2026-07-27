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

#include "gui/ui/keyboard/layout/iris.h"
#include "gui/colour/colour.h"
#include "hid/display/display.h"
#include "util/functions.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace deluge::gui::ui::keyboard::layout {

// Grid rows (bottom to top)
constexpr int32_t kIrisRowRoot = 0;
constexpr int32_t kIrisRowBass = 1;
constexpr int32_t kIrisRowComplexity = 2;
constexpr int32_t kIrisRowQuality = 3;
constexpr int32_t kIrisRowVoicing = 4;

constexpr int32_t kIrisNumQualities = 6;
constexpr int32_t kIrisNumComplexities = 10;
constexpr int32_t kIrisOctavePadFirst = 12; // Root row, pads 12-15
constexpr int32_t kIrisNumOctaves = 4;
constexpr int32_t kIrisBaseOctaveNote = 36; // Octave pad 0 = C2 region

namespace {

const std::array<const Chord, 10>& chordsForQuality(int32_t quality) {
	switch (quality) {
	case 1:
		return minorChords;
	case 2:
		return dominantChords;
	case 3:
		return diminishedChords;
	case 4:
		return augmentedChords;
	case 5:
		return otherChords;
	default:
		return majorChords;
	}
}

const RGB kQualityColours[kIrisNumQualities] = {
    colours::yellow, colours::blue, colours::purple, colours::red, colours::green, colours::grey,
};

bool voicingValid(const Voicing& v) {
	for (int32_t i = 1; i < kMaxChordKeyboardSize; i++) {
		if (v.offsets[i] != 0 && v.offsets[i] != NONE) {
			return true;
		}
	}
	return false;
}

} // namespace

void KeyboardLayoutIris::evaluatePads(PressedPad presses[kMaxNumKeyboardPadPresses]) {
	currentNotesState = NotesState{}; // Erase active notes

	// First pass: latch control-region presses (quality, complexity, bass,
	// voicing, octave) so a root held simultaneously sees the new settings.
	// Edge-triggered per press slot: bass is a toggle and must fire once
	for (int32_t idxPress = 0; idxPress < kMaxNumKeyboardPadPresses; ++idxPress) {
		PressedPad pressed = presses[idxPress];
		uint32_t bit = 1u << idxPress;
		if (!pressed.active) {
			controlHandledMask &= ~bit;
			continue;
		}
		bool isControl = (pressed.y != kIrisRowRoot) || (pressed.x >= kIrisOctavePadFirst);
		if (!pressed.dead && pressed.x < kDisplayWidth && isControl && !(controlHandledMask & bit)) {
			controlHandledMask |= bit;
			handleControlPad(pressed.x, pressed.y);
		}
	}

	// Second pass: the most recent active root press sounds the chord
	int32_t rootPadX = -1;
	uint32_t bestTime = 0;
	for (int32_t idxPress = 0; idxPress < kMaxNumKeyboardPadPresses; ++idxPress) {
		PressedPad pressed = presses[idxPress];
		if (pressed.active && !pressed.dead && pressed.x < kIrisOctavePadFirst && pressed.y == kIrisRowRoot) {
			if (rootPadX < 0 || pressed.timeLastPadPress >= bestTime) {
				rootPadX = pressed.x;
				bestTime = pressed.timeLastPadPress;
			}
		}
	}

	if (rootPadX >= 0) {
		triggerChord(rootPadX);
		uint8_t velocity = getDefaultVelocity();
		for (int32_t i = 0; i < curCount; i++) {
			enableNote(curNotes[i], velocity);
		}
	}
	else {
		// Chord released: it stays in prevNotes as the voice-leading anchor,
		// but the next press must recompute
		curIdentity = 0xFFFFFFFF;
	}

	ColumnControlsKeyboard::evaluatePads(presses);
}

void KeyboardLayoutIris::handleControlPad(int32_t x, int32_t y) {
	KeyboardStateIris& state = getState().iris;

	switch (y) {
	case kIrisRowRoot: // Octave selector pads (x >= kIrisOctavePadFirst)
		if (x >= kIrisOctavePadFirst) {
			state.octave = x - kIrisOctavePadFirst;
			char longName[16];
			snprintf(longName, sizeof(longName), "Octave %d", state.octave + 1);
			char shortName[5];
			snprintf(shortName, sizeof(shortName), "OCT%d", state.octave + 1);
			char const* shortLong[2] = {shortName, longName};
			display->displayPopup(shortLong);
		}
		break;
	case kIrisRowBass:
		if (x < kOctaveSize) {
			// Latching toggle: press the active bass pad to clear it
			state.bassPc = (state.bassPc == x) ? -1 : x;
			if (state.bassPc < 0) {
				char const* shortLong[2] = {"OFF", "Bass: off"};
				display->displayPopup(shortLong);
			}
			else {
				char noteName[3] = {0};
				int32_t isNatural = 1;
				noteCodeToString(60 + ((getRootNote() + x) % kOctaveSize), noteName, &isNatural, false);
				char longName[16];
				snprintf(longName, sizeof(longName), "Bass: %s", noteName);
				char const* shortLong[2] = {noteName, longName};
				display->displayPopup(shortLong);
			}
		}
		break;
	case kIrisRowComplexity:
		if (x < kIrisNumComplexities) {
			state.complexity = x;
			const Chord& chord = chordsForQuality(state.quality)[state.complexity];
			char longName[24];
			snprintf(longName, sizeof(longName), "Chord: %s", chord.name);
			char const* shortLong[2] = {chord.name, longName};
			display->displayPopup(shortLong);
		}
		break;
	case kIrisRowQuality:
		if (x < kIrisNumQualities) {
			state.quality = x;
			static const char* const kQualityShort[kIrisNumQualities] = {"MAJ", "MIN", "DOM", "DIM", "AUG", "OTHR"};
			static const char* const kQualityLong[kIrisNumQualities] = {"Major",      "Minor",     "Dominant",
			                                                            "Diminished", "Augmented", "Other"};
			char const* shortLong[2] = {kQualityShort[x], kQualityLong[x]};
			display->displayPopup(shortLong);
		}
		break;
	case kIrisRowVoicing: {
		state.voicingBias = x;
		static const char* const kSpreadNames[4] = {"Close", "Mid", "Open", "Wide"};
		char longName[20];
		snprintf(longName, sizeof(longName), "Voicing: %s %d", kSpreadNames[x >> 2], x);
		char shortName[5];
		snprintf(shortName, sizeof(shortName), "VC%02d", x);
		char const* shortLong[2] = {shortName, longName};
		display->displayPopup(shortLong);
		break;
	}
	default:
		break;
	}
}

void KeyboardLayoutIris::triggerChord(int32_t rootPadX) {
	KeyboardStateIris& state = getState().iris;

	uint32_t identity = static_cast<uint32_t>(rootPadX) | (static_cast<uint32_t>(state.octave) << 4)
	                    | (static_cast<uint32_t>(state.quality) << 8) | (static_cast<uint32_t>(state.complexity) << 12)
	                    | (static_cast<uint32_t>(state.bassPc + 1) << 16)
	                    | (static_cast<uint32_t>(state.voicingBias) << 21);
	if (identity == curIdentity) {
		return; // Same chord still held - keep the voicing stable
	}

	int32_t baseNote = kIrisBaseOctaveNote + state.octave * kOctaveSize + getRootNote();
	int32_t rootNote = baseNote + rootPadX;
	const Chord& chord = chordsForQuality(state.quality)[state.complexity];

	// Re-voicing an already-sounding chord (control changed under a held
	// root): flow from what is sounding now, not from two chords ago
	if (curIdentity != 0xFFFFFFFF && curCount > 0) {
		std::copy(curNotes, curNotes + curCount, prevNotes);
		prevCount = curCount;
	}

	computeVoicing(rootNote, chord);

	// Slash bass below the chord
	if (state.bassPc >= 0 && curCount < kMaxChordKeyboardSize) {
		int32_t bassNote = baseNote - kOctaveSize + state.bassPc;
		if (bassNote >= 0) {
			// Insert at front (keep sorted)
			for (int32_t i = curCount; i > 0; i--) {
				curNotes[i] = curNotes[i - 1];
			}
			curNotes[0] = static_cast<uint8_t>(bassNote);
			curCount++;
		}
	}

	curIdentity = identity;
	std::copy(curNotes, curNotes + curCount, prevNotes);
	prevCount = curCount;

	announceChord(rootNote, chord);
}

void KeyboardLayoutIris::computeVoicing(int32_t rootNote, const Chord& chord) {
	KeyboardStateIris& state = getState().iris;

	int32_t bestScore = INT32_MAX;
	uint8_t bestNotes[kMaxChordKeyboardSize];
	int32_t bestCount = 0;

	// The voicing slider sets the spread the engine aims for: ~7 semitones
	// (close triad) up to ~22 (wide open)
	int32_t targetSpread = 7 + state.voicingBias;
	int32_t registerAnchor = rootNote + 7;

	for (int32_t v = 0; v < kUniqueVoicings; v++) {
		const Voicing& voicing = chord.voicings[v];
		if (v > 0 && !voicingValid(voicing)) {
			continue;
		}
		for (int32_t shift = -kOctaveSize; shift <= kOctaveSize; shift += kOctaveSize) {
			uint8_t notes[kMaxChordKeyboardSize];
			int32_t count = 0;
			for (int32_t i = 0; i < kMaxChordKeyboardSize; i++) {
				int32_t offset = voicing.offsets[i];
				if (offset == NONE || (i > 0 && offset == 0)) {
					continue;
				}
				int32_t note = rootNote + offset + shift;
				if (note < 0 || note > 127) {
					count = 0; // Out of range - reject the whole candidate
					break;
				}
				notes[count++] = static_cast<uint8_t>(note);
			}
			if (count == 0) {
				continue;
			}
			std::sort(notes, notes + count);

			// Voice-leading distance: pair sorted voices, penalize count changes
			int32_t vlDist = 0;
			if (prevCount > 0) {
				int32_t pairCount = std::min(count, prevCount);
				for (int32_t i = 0; i < pairCount; i++) {
					vlDist += std::abs(static_cast<int32_t>(notes[i]) - static_cast<int32_t>(prevNotes[i]));
				}
				vlDist += 3 * std::abs(count - prevCount);
			}

			int32_t spread = notes[count - 1] - notes[0];
			int32_t mean = 0;
			for (int32_t i = 0; i < count; i++) {
				mean += notes[i];
			}
			mean /= count;

			int32_t score = 4 * vlDist + 2 * std::abs(spread - targetSpread) + std::abs(mean - registerAnchor);

			if (score < bestScore) {
				bestScore = score;
				bestCount = count;
				std::copy(notes, notes + count, bestNotes);
			}
		}
	}

	curCount = bestCount;
	std::copy(bestNotes, bestNotes + bestCount, curNotes);
}

void KeyboardLayoutIris::announceChord(int32_t rootNote, const Chord& chord) {
	char noteName[3] = {0};
	int32_t isNatural = 1;
	noteCodeToString(rootNote, noteName, &isNatural, false);

	char fullName[32];
	snprintf(fullName, sizeof(fullName), "%s%s", noteName, chord.name);
	if (display->haveOLED()) {
		display->popupTextTemporary(fullName);
	}
	else {
		display->setScrollingText(fullName, 0);
	}
}

void KeyboardLayoutIris::handleVerticalEncoder(int32_t offset) {
	if (verticalEncoderHandledByColumns(offset)) {
		return;
	}
	KeyboardStateIris& state = getState().iris;
	state.octave =
	    static_cast<uint8_t>(std::clamp<int32_t>(static_cast<int32_t>(state.octave) + offset, 0, kIrisNumOctaves - 1));
}

void KeyboardLayoutIris::handleHorizontalEncoder(int32_t offset, bool shiftEnabled,
                                                 PressedPad presses[kMaxNumKeyboardPadPresses], bool encoderPressed) {
	if (horizontalEncoderHandledByColumns(offset, shiftEnabled)) {
		return;
	}
	KeyboardStateIris& state = getState().iris;
	state.voicingBias =
	    static_cast<uint8_t>(std::clamp<int32_t>(static_cast<int32_t>(state.voicingBias) + offset, 0, 15));
}

void KeyboardLayoutIris::precalculate() {
}

void KeyboardLayoutIris::renderPads(RGB image[][kDisplayWidth + kSideBarWidth]) {
	KeyboardStateIris& state = getState().iris;
	int32_t rootPc = getRootNote();

	for (int32_t y = 0; y < kDisplayHeight; ++y) {
		for (int32_t x = 0; x < kDisplayWidth; ++x) {
			image[y][x] = colours::black;
		}
	}

	// Root row: rainbow by pitch class, song root emphasized
	for (int32_t x = 0; x < kOctaveSize; ++x) {
		RGB colour = getNoteColour(((rootPc + x) % kOctaveSize) * 16);
		image[kIrisRowRoot][x] = (x == 0) ? colour : colour.dim(2);
	}
	// Octave selector
	for (int32_t i = 0; i < kIrisNumOctaves; ++i) {
		image[kIrisRowRoot][kIrisOctavePadFirst + i] = (i == state.octave) ? colours::cyan : colours::cyan.dim(5);
	}

	// Bass row
	for (int32_t x = 0; x < kOctaveSize; ++x) {
		RGB colour = getNoteColour(((rootPc + x) % kOctaveSize) * 16);
		image[kIrisRowBass][x] = (x == state.bassPc) ? colour : colour.dim(5);
	}

	// Complexity row: brightness names the chosen chord in the family
	for (int32_t x = 0; x < kIrisNumComplexities; ++x) {
		RGB colour = kQualityColours[state.quality];
		image[kIrisRowComplexity][x] = (x == state.complexity) ? colour : colour.dim(5);
	}

	// Quality row
	for (int32_t x = 0; x < kIrisNumQualities; ++x) {
		image[kIrisRowQuality][x] = (x == state.quality) ? kQualityColours[x] : kQualityColours[x].dim(4);
	}

	// Voicing slider: gradient close (green) .. spread (magenta)
	for (int32_t x = 0; x < kDisplayWidth; ++x) {
		RGB colour = RGB::blend(colours::green, colours::magenta, static_cast<uint8_t>(x * 16));
		image[kIrisRowVoicing][x] = (x == state.voicingBias) ? colour : colour.dim(5);
	}
}

} // namespace deluge::gui::ui::keyboard::layout
