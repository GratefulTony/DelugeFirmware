# Bounce In Place — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a destructive "Bounce Clip" / "Bounce Track" option to the ClipSettings context menu that renders the source through its current FX chain and replaces it with an AudioClip on a new AudioOutput.

**Architecture:** Reuse `StemExport` with `includeSongFX=false` + `renderOffline=true` for rendering. After each WAV lands, create a new `AudioOutput` (reverb send copied from source), build an `AudioClip` referencing the WAV, and `swapClips` into the session. Works identically for synth, kit, and audio-clip sources.

**Tech Stack:** C++20 / C++23, Deluge firmware, `./dbt build`, tested on RZ/A1 hardware.

**Design doc:** `docs/plans/2026-04-21-bounce-in-place-design.md`

---

## Testing Philosophy

This is embedded audio DSP on a Deluge. The project's automated tests in `tests/32bit_unit_tests/` do **not** cover the audio rendering or UI flows we're touching. Per CLAUDE.md: "Test on actual hardware when possible. User will report issues — trust their observations. Don't assume code works just because it compiles."

So the rhythm is **not** red-green-refactor. Instead:

1. Implement a coherent slice
2. `./dbt build` (takes ~2 min)
3. `./dbt loadfw` to flash
4. Manual hardware test per the task's verification steps
5. Commit
6. Move on

Each phase ends on a flashable, hardware-verifiable checkpoint. Do not build inside every task — consolidate builds at the end of each phase.

---

## Phase 1 — UI plumbing (stub, no behavior)

End-of-phase hardware test: open ClipSettings menu on a synth clip, see "Bounce Clip" and "Bounce Track"; selecting either shows a popup saying which was clicked. Nothing else changes.

### Task 1.1: Add localization strings

**Files:**
- Modify: `src/deluge/gui/l10n/strings.h` — add two enum entries
- Modify: `src/deluge/gui/l10n/g_english.cpp` — add two string mappings
- Modify: `src/deluge/gui/l10n/english.json` — add two JSON entries
- Modify: `src/deluge/gui/l10n/g_seven_segment.cpp` — add two 4-char mappings
- Modify: `src/deluge/gui/l10n/seven_segment.json` — add two JSON entries

**Step 1: Add enum entries**

In `strings.h`, find `STRING_FOR_CONVERT_TO_AUDIO,` (line 1216) and add below it:

```cpp
	STRING_FOR_BOUNCE_CLIP,
	STRING_FOR_BOUNCE_TRACK,
```

**Step 2: Add English strings**

In `g_english.cpp`, find `{STRING_FOR_CONVERT_TO_AUDIO, "Convert to Audio"},` (line 1043) and add below it:

```cpp
        {STRING_FOR_BOUNCE_CLIP, "Bounce Clip"},
        {STRING_FOR_BOUNCE_TRACK, "Bounce Track"},
```

Mirror the same two lines in `english.json` near the existing `STRING_FOR_CONVERT_TO_AUDIO` entry.

**Step 3: Add seven-segment mappings**

In `g_seven_segment.cpp`, near `{STRING_FOR_CONVERT_TO_AUDIO, "CONV"},` add:

```cpp
        {STRING_FOR_BOUNCE_CLIP, "BNCC"},
        {STRING_FOR_BOUNCE_TRACK, "BNCT"},
```

Mirror in `seven_segment.json`.

**Step 4: Commit**

```bash
git add src/deluge/gui/l10n/
git commit -m "l10n: add Bounce Clip / Bounce Track strings"
```

No build yet — these are pure data additions.

---

### Task 1.2: Add `BounceScope` enum and stub `bounceInPlace` method

**Files:**
- Modify: `src/deluge/gui/views/session_view.h` — add enum + method declaration
- Modify: `src/deluge/gui/views/session_view.cpp` — add stub implementation

**Step 1: Declare the enum and method**

In `session_view.h`, near line 159 (just above `replaceInstrumentClipWithAudioClip`), add:

```cpp
	enum class BounceScope { CLIP, TRACK };

	// bounce synth/kit/audio clip to new audio clip through current FX chain
	void bounceInPlace(Clip* clip, BounceScope scope);
```

**Step 2: Add stub implementation**

In `session_view.cpp`, near line 1792 (just above `replaceInstrumentClipWithAudioClip`), add:

```cpp
void SessionView::bounceInPlace(Clip* clip, BounceScope scope) {
	if (!clip) {
		return;
	}
	char const* msg = (scope == BounceScope::CLIP) ? "Bounce clip (stub)" : "Bounce track (stub)";
	display->displayPopup(msg);
}
```

**Step 3: Commit**

```bash
git add src/deluge/gui/views/session_view.h src/deluge/gui/views/session_view.cpp
git commit -m "feat: add BounceScope enum and bounceInPlace stub"
```

---

### Task 1.3: Wire menu options in ClipSettingsMenu

**Files:**
- Modify: `src/deluge/gui/context_menu/clip_settings/clip_settings.cpp`

**Step 1: Add options to both branches of `getOptions()`**

Replace the current body of `getOptions()` (lines 38–55) with:

```cpp
std::span<char const*> ClipSettingsMenu::getOptions() {
	using enum l10n::String;
	if (clip->type == ClipType::AUDIO) {
		static const char* optionsls[] = {
		    l10n::get(STRING_FOR_BOUNCE_CLIP),
		    l10n::get(STRING_FOR_BOUNCE_TRACK),
		    l10n::get(STRING_FOR_CLIP_MODE),
		    l10n::get(STRING_FOR_CLIP_NAME),
		};
		return {optionsls, 4};
	}
	else {
		static const char* optionsls[] = {
		    l10n::get(STRING_FOR_CONVERT_TO_AUDIO),
		    l10n::get(STRING_FOR_BOUNCE_CLIP),
		    l10n::get(STRING_FOR_BOUNCE_TRACK),
		    l10n::get(STRING_FOR_CLIP_MODE),
		    l10n::get(STRING_FOR_CLIP_NAME),
		};
		return {optionsls, 5};
	}
}
```

**Step 2: Update `acceptCurrentOption()` dispatch**

Replace the body (lines 66–88) with:

```cpp
bool ClipSettingsMenu::acceptCurrentOption() {
	using Scope = SessionView::BounceScope;
	int32_t option = this->currentOption;

	if (clip->type == ClipType::INSTRUMENT) {
		if (option == 0) {
			sessionView.replaceInstrumentClipWithAudioClip(clip);
			return false;
		}
		option--; // normalise past Convert to Audio
	}

	// Now option is: 0=Bounce Clip, 1=Bounce Track, 2=Clip Mode, 3=Clip Name
	if (option == 0) {
		sessionView.bounceInPlace(clip, Scope::CLIP);
		return false;
	}
	if (option == 1) {
		sessionView.bounceInPlace(clip, Scope::TRACK);
		return false;
	}
	if (option == 2) {
		launchStyle.clip = clip;
		launchStyle.setupAndCheckAvailability();
		openUI(&launchStyle);
		return true;
	}
	currentUIMode = UI_MODE_NONE;
	renameClipUI.clip = clip;
	openUI(&renameClipUI);
	return true;
}
```

**Step 3: Add include**

Near the top of `clip_settings.cpp`, ensure `#include "gui/views/session_view.h"` is present (it already is at line 24 — verify).

**Step 4: Commit**

```bash
git add src/deluge/gui/context_menu/clip_settings/clip_settings.cpp
git commit -m "feat: wire Bounce Clip/Track menu options to bounceInPlace"
```

---

### Task 1.4: Build, flash, and smoke-test Phase 1

**Step 1: Build**

```bash
./dbt build release
```

Expected: clean build, no warnings added.

**Step 2: Flash**

```bash
./dbt loadfw
```

**Step 3: Hardware verification**

- Open a song, enter grid song view
- Hold a synth clip pad, press SELECT → ClipSettings menu opens
- Scroll: you should see `Convert to Audio`, `Bounce Clip`, `Bounce Track`, `Clip Mode`, `Clip Name`
- Select "Bounce Clip" → popup "Bounce clip (stub)"
- Select "Bounce Track" → popup "Bounce track (stub)"
- Repeat on an audio clip: menu should show `Bounce Clip`, `Bounce Track`, `Clip Mode`, `Clip Name` (no Convert)

If all four dispatch routes behave correctly, proceed. Otherwise fix dispatch before continuing — a broken menu wiring will confuse later phases.

---

## Phase 2 — Single-clip bounce (CLIP scope end-to-end)

End-of-phase hardware test: bounce a single non-empty synth clip in a track with only that one clip; confirm source clip is replaced by a new audio clip on a new audio track, playback is audibly identical except reverb (still applied via live send).

### Task 2.1: Implement render phase — stem export orchestration

**Files:**
- Modify: `src/deluge/gui/views/session_view.cpp` — flesh out `bounceInPlace` CLIP path

**Step 1: Add required includes**

At the top of `session_view.cpp`, after existing includes, ensure these are present:

```cpp
#include "processing/stem_export/stem_export.h"
#include "gui/ui/audio_recorder.h"
#include "model/clip/audio_clip.h"
#include "processing/audio_output.h"
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"
```

**Step 2: Replace the stub with the CLIP-scope render phase**

Replace the `bounceInPlace` body:

```cpp
void SessionView::bounceInPlace(Clip* clip, BounceScope scope) {
	if (!clip || clip->type == ClipType::NONE) {
		return;
	}

	// Precondition: MIDI/CV can't be bounced
	OutputType outType = clip->output->type;
	if (outType == OutputType::MIDI_OUT || outType == OutputType::CV) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_CANT_CONVERT_TYPE));
		return;
	}

	// Precondition: not already recording arrangement
	if (playbackHandler.recording == RecordingMode::ARRANGEMENT) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_RECORDING_TO_ARRANGEMENT));
		return;
	}

	// Save stemExport state for restoration
	bool savedIncludeSongFX = stemExport.includeSongFX;
	bool savedRenderOffline = stemExport.renderOffline;
	bool savedAllowNormalization = stemExport.allowNormalization;

	// Configure stem export for bounce
	stemExport.includeSongFX = false;
	stemExport.renderOffline = true;
	stemExport.allowNormalization = false;
	stemExport.currentStemExportType = StemExportType::CLIP;

	if (scope == BounceScope::CLIP) {
		int32_t clipIndex = currentSong->sessionClips.getIndexForClip(clip);
		if (clipIndex < 0) {
			goto restore;
		}

		// Prepare all clips: disarm + mark only target with exportStem
		int32_t totalClips = stemExport.disarmAllClipsForStemExport();
		for (int32_t i = 0; i < totalClips; i++) {
			Clip* c = currentSong->sessionClips.getClipAtIndex(i);
			if (c) {
				c->exportStem = (c == clip);
			}
		}

		// Set loop length + marker for the target clip
		stemExport.getLoopEndPointInSamplesForAudioFile(clip->loopLength);

		// Kick off the stem render
		bool started = stemExport.startCurrentStemExport(
		    StemExportType::CLIP, clip->output, clip->activeIfNoSolo, clipIndex, true);

		if (!started) {
			stemExport.restoreAllClipMutes(totalClips);
			goto restore;
		}

		// Yield until render completes (same predicate stem export uses internally)
		yield([]() {
			if (stemExport.stopRecording) {
				stemExport.stopOutputRecording();
			}
			return !(playbackHandler.recording != RecordingMode::OFF
			         || audioRecorder.recordingSource > AudioInputChannel::NONE
			         || playbackHandler.isEitherClockActive());
		});

		// TODO (next task): create AudioOutput + AudioClip + swap
		display->displayPopup(stemExport.wavFileNameForStemExport.get());

		stemExport.restoreAllClipMutes(totalClips);
	}
	else {
		display->displayPopup("Track bounce not yet impl");
	}

restore:
	stemExport.includeSongFX = savedIncludeSongFX;
	stemExport.renderOffline = savedRenderOffline;
	stemExport.allowNormalization = savedAllowNormalization;
}
```

**Step 3: Commit**

```bash
git add src/deluge/gui/views/session_view.cpp
git commit -m "feat: bounce CLIP scope — render phase via StemExport"
```

---

### Task 2.2: Create new AudioOutput with reverb send copied from source

**Files:**
- Modify: `src/deluge/gui/views/session_view.cpp`

**Step 1: Replace the `TODO` block with AudioOutput creation**

Replace the line `display->displayPopup(stemExport.wavFileNameForStemExport.get());` (and the adjacent TODO comment) with:

```cpp
		// Capture WAV path before state bleeds elsewhere
		String wavPath;
		wavPath.set(&stemExport.wavFileNameForStemExport);

		// Snapshot source reverb-send amount before creating new output
		int32_t sourceReverbSend = 0;
		{
			ParamManagerForTimeline* pm = currentSong->getBackedUpParamManagerForExactClip(
			    (ModControllableAudio*)clip->output->toModControllable(), nullptr);
			if (pm && pm->containsAnyParamCollectionsIncludingExpression()) {
				UnpatchedParamSet* ups = pm->getUnpatchedParamSet();
				sourceReverbSend = ups->getValue(deluge::modulation::params::UNPATCHED_REVERB_SEND_AMOUNT);
			}
		}

		// Create new AudioOutput (standalone for CLIP scope — source output kept for any other clips)
		AudioOutput* newOutput = currentSong->createNewAudioOutput();
		if (!newOutput) {
			display->displayError(Error::INSUFFICIENT_RAM);
			stemExport.restoreAllClipMutes(totalClips);
			goto restore;
		}
		newOutput->colour = clip->output->colour;

		// Copy reverb send onto new AudioOutput's param manager
		{
			ParamManagerForTimeline* pmNew = currentSong->getBackedUpParamManagerForExactClip(
			    (ModControllableAudio*)newOutput->toModControllable(), nullptr);
			if (pmNew && pmNew->containsAnyParamCollectionsIncludingExpression()) {
				UnpatchedParamSet* upsNew = pmNew->getUnpatchedParamSet();
				upsNew->params[deluge::modulation::params::UNPATCHED_REVERB_SEND_AMOUNT]
				    .setCurrentValueBasicForSetup(sourceReverbSend);
			}
		}

		// TODO (next task): create AudioClip and swap into session
		display->displayPopup(wavPath.get());

		stemExport.restoreAllClipMutes(totalClips);
```

Notes for the implementer:
- If `getBackedUpParamManagerForExactClip` isn't the right accessor, check how `View::displayOutputName` / existing audio-clip creation paths reach the unpatched param set for a newly created `AudioOutput`. The exact API name varies; what you want is the `UnpatchedParamSet*` for the new output's ModControllable.
- If the new output's param manager is zero-initialised, `setCurrentValueBasicForSetup` may be the wrong setter — compare against how `createNewAudioOutput` + `GlobalEffectableForClip::initParamsForAudioClip` set other unpatched params, and follow that pattern.

**Step 2: Commit**

```bash
git add src/deluge/gui/views/session_view.cpp
git commit -m "feat: bounce CLIP scope — create AudioOutput, copy reverb send"
```

---

### Task 2.3: Create AudioClip and swap into session

**Files:**
- Modify: `src/deluge/gui/views/session_view.cpp`

**Step 1: Replace the second TODO block with the swap**

Replace the lines from `// TODO (next task): create AudioClip…` through `display->displayPopup(wavPath.get());` with:

```cpp
		// Allocate AudioClip
		void* clipMem = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(AudioClip));
		if (!clipMem) {
			// Roll back: delete the new AudioOutput we just created
			currentSong->deleteOutput(newOutput);
			display->displayError(Error::INSUFFICIENT_RAM);
			stemExport.restoreAllClipMutes(totalClips);
			goto restore;
		}
		AudioClip* newClip = new (clipMem) AudioClip();
		newClip->cloneFrom(clip);           // gets loopLength, section, colour offset etc.
		newClip->colourOffset = clip->colourOffset;

		// Hook to timeline + new output
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStack* modelStack = setupModelStackWithSong(modelStackMemory, currentSong);
		newClip->setOutput(modelStack->addTimelineCounter(newClip), newOutput);

		// Load the rendered WAV as this clip's sample
		Error sampleErr = newClip->sampleHolder.loadFileRead(wavPath.get(), currentSong);
		if (sampleErr != Error::NONE) {
			newClip->~AudioClip();
			delugeDealloc(clipMem);
			currentSong->deleteOutput(newOutput);
			display->displayError(sampleErr);
			stemExport.restoreAllClipMutes(totalClips);
			goto restore;
		}

		// If playback is active and source was active, keep audio clip playing
		if (playbackHandler.playbackState && currentSong->isClipActive(clip)) {
			newClip->activeIfNoSolo = clip->activeIfNoSolo;
			if (clip->soloingInSessionMode) {
				session.unsoloClip(clip);
			}
		}

		// Swap in the session
		currentSong->swapClips(newClip, clip, clipIndex);

		// If source instrument has no remaining clips, delete it
		if (currentSong->getClipWithOutput(clip->output) == nullptr) {
			currentSong->deleteOutput(clip->output);
		}

		view.setActiveModControllableTimelineCounter(newClip);
		view.displayOutputName(newClip->output, true, newClip);
		requestRendering(this, 1 << selectedClipYDisplay, 1 << selectedClipYDisplay);

		stemExport.restoreAllClipMutes(totalClips);
```

Exact API names to verify while implementing:
- `AudioClip::cloneFrom` / constructor — match what `Song::replaceInstrumentClipWithAudioClip` (song.cpp:5619) does for a newly created AudioClip.
- `sampleHolder.loadFileRead` — confirm the method name and signature; if the AudioClip's sample-loading API is different, mirror what the sample browser or audio-clip-from-recording path uses.
- `currentSong->swapClips` — confirm signature; this should exist (it's what `replaceInstrumentClipWithAudioClip` calls internally).
- `deleteOutput` — verify the right function to remove an empty source Output.

Do **not** guess — open `song.cpp` around line 5619 (`replaceInstrumentClipWithAudioClip`) and mirror its exact API calls.

**Step 2: Commit**

```bash
git add src/deluge/gui/views/session_view.cpp
git commit -m "feat: bounce CLIP scope — create AudioClip and swap into session"
```

---

### Task 2.4: Phase 2 build, format, flash, hardware test

**Step 1: Format**

```bash
./dbt format
```

**Step 2: Build**

```bash
./dbt build release
```

Expected: clean build. If anything fails, fix before proceeding (most likely culprits: wrong param-manager accessor, wrong clone method, missing include). Consult the open-questions list in the design doc.

**Step 3: Commit formatting changes (if any)**

```bash
git add -u && git commit -m "chore: format"   # only if format changed anything
```

**Step 4: Flash**

```bash
./dbt loadfw
```

**Step 5: Hardware verification**

- Create a new song. Default template has one synth track with one clip.
- Add some notes to the synth clip.
- Add a small reverb send on the synth track (mod encoder while holding clip).
- In grid song view, hold the synth clip pad + SELECT → select "Bounce Clip".
- Watch: "Exporting stem 1 of 1" popup should appear briefly, then the synth clip's square should become an audio clip.
- Press play. The audio should sound substantially identical to the source (including the live reverb tail, since reverb send was copied to the new AudioOutput).
- Press the new audio clip pad + SELECT → verify it's an AudioClip (menu shows `Bounce Clip`, `Bounce Track`, `Clip Mode`, `Clip Name`, no `Convert to Audio`).
- Verify the source synth instrument is gone from the output list (enter output menu / scroll, no synth track left).

If any of these fail, don't proceed to Phase 3 — diagnose first. Most likely issues:
- Reverb send not copied correctly → re-check the param manager accessor.
- Source instrument not deleted → `getClipWithOutput` might be returning the new clip; verify it iterates only real clips, not the freshly swapped one.
- Audio sounds at wrong level → check `AudioOutput` compressor default (open question #1 in design doc); may need explicit bypass.

**Step 6 (if issues): bypass AudioOutput compressor explicitly**

If level is wrong, after creating `newOutput` add:

```cpp
		// Explicit bypass to match source level (compressor default may not be transparent)
		if (pmNew && pmNew->containsAnyParamCollectionsIncludingExpression()) {
			UnpatchedParamSet* upsNew = pmNew->getUnpatchedParamSet();
			upsNew->params[deluge::modulation::params::UNPATCHED_COMPRESSOR_THRESHOLD]
			    .setCurrentValueBasicForSetup(0);
			// …and any other compressor params as needed
		}
```

Only add this if the hardware test reveals a gain mismatch.

---

## Phase 3 — Whole-track bounce (TRACK scope)

End-of-phase hardware test: bounce a synth track with 3 clips across different sections; all 3 become audio clips on one new audio track in the same grid column; source synth track is gone.

### Task 3.1: Collect source clips; render each; buffer WAV paths

**Files:**
- Modify: `src/deluge/gui/views/session_view.cpp`

**Step 1: Replace the `else` branch of `bounceInPlace` (the "Track bounce not yet impl" popup) with the render loop.**

Design:
- Build a `vector<pair<Clip*, String>>` of `(sourceClip, wavPath)`
- For each non-empty clip on `clip->output`, run the same stem-export render used in Task 2.1
- On any failure → abort, no swap, no song changes
- After all renders complete → popup `"Bounced N clips"` (swap comes in next task)

Pseudocode (implementer: choose an appropriate container — the Deluge uses `Vector<T>` / `FixedVector<T, N>` in various places; match existing patterns):

```cpp
	else {
		Output* sourceOutput = clip->output;

		// Collect all clips on this output, in session order
		struct TargetClip { Clip* clip; int32_t index; String wavPath; };
		// choose container — see existing Vector/FixedVector usage
		int32_t totalClips = currentSong->sessionClips.getNumElements();
		// …

		// Disarm and mark each source clip's exportStem = true (target), others = false
		stemExport.disarmAllClipsForStemExport();
		for (int32_t i = 0; i < totalClips; i++) {
			Clip* c = currentSong->sessionClips.getClipAtIndex(i);
			c->exportStem = (c && c->output == sourceOutput && !c->isEmpty(false));
		}

		// For each target clip: render, yield, capture WAV path
		for (each target in collected list) {
			stemExport.getLoopEndPointInSamplesForAudioFile(target->loopLength);
			bool started = stemExport.startCurrentStemExport(
			    StemExportType::CLIP, sourceOutput, target->activeIfNoSolo, target->index, true);
			if (!started) {
				stemExport.restoreAllClipMutes(totalClips);
				goto restore;
			}
			yield(/* same predicate */);
			// capture WAV path into target struct
			target->wavPath.set(&stemExport.wavFileNameForStemExport);
			// detect user cancel / error:
			if (!isUIModeActive(UI_MODE_STEM_EXPORT)) {
				stemExport.restoreAllClipMutes(totalClips);
				goto restore;
			}
		}

		stemExport.restoreAllClipMutes(totalClips);

		// TODO (next task): swap all clips + cleanup source output
		char buf[16];
		snprintf(buf, sizeof(buf), "Rendered %d", numTargets);
		display->displayPopup(buf);
	}
```

**Step 2: Commit**

```bash
git add src/deluge/gui/views/session_view.cpp
git commit -m "feat: bounce TRACK scope — render all clips, buffer WAV paths"
```

---

### Task 3.2: All-or-nothing swap for TRACK scope

**Files:**
- Modify: `src/deluge/gui/views/session_view.cpp`

**Step 1: Replace the `TODO (next task)` block with the swap loop.**

```cpp
		// Snapshot reverb send from source output
		int32_t sourceReverbSend = 0;
		/* (same accessor as Task 2.2) */

		// Create single new AudioOutput replacing the source's slot in the output list
		AudioOutput* newOutput = currentSong->createNewAudioOutput(sourceOutput);
		if (!newOutput) {
			display->displayError(Error::INSUFFICIENT_RAM);
			goto restore;
		}
		newOutput->colour = sourceOutput->colour;

		// Copy reverb send onto newOutput (same as Task 2.2)
		/* … */

		// For each (sourceClip, wavPath): create AudioClip, swap in
		bool swapFailed = false;
		for (each target in collected list) {
			void* clipMem = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(AudioClip));
			if (!clipMem) {
				swapFailed = true;
				break;
			}
			AudioClip* newClip = new (clipMem) AudioClip();
			newClip->cloneFrom(target->clip);
			newClip->colourOffset = target->clip->colourOffset;

			char modelStackMemory[MODEL_STACK_MAX_SIZE];
			ModelStack* modelStack = setupModelStackWithSong(modelStackMemory, currentSong);
			newClip->setOutput(modelStack->addTimelineCounter(newClip), newOutput);

			Error sampleErr = newClip->sampleHolder.loadFileRead(target->wavPath.get(), currentSong);
			if (sampleErr != Error::NONE) {
				newClip->~AudioClip();
				delugeDealloc(clipMem);
				swapFailed = true;
				break;
			}

			if (playbackHandler.playbackState && currentSong->isClipActive(target->clip)) {
				newClip->activeIfNoSolo = target->clip->activeIfNoSolo;
				if (target->clip->soloingInSessionMode) {
					session.unsoloClip(target->clip);
				}
			}

			currentSong->swapClips(newClip, target->clip, target->index);
		}

		if (swapFailed) {
			// Partial failure: caller must accept current state (some clips swapped, some not).
			// POC policy is all-or-nothing pre-swap, but in this in-swap path we cannot cleanly undo
			// because clips are shared with playback engine. Surface the error and stop.
			display->displayError(Error::INSUFFICIENT_RAM);
		}

		// `createNewAudioOutput(sourceOutput)` already handled the output-list swap.
		view.setActiveModControllableTimelineCounter(nullptr);
		requestRendering(this, 0xFFFFFFFF, 0xFFFFFFFF);
```

Implementer notes:
- The "swap failed mid-loop" case is messy; for POC, log and stop. The user's song will be in a partially-bounced state. Not ideal but document it — fixing requires a pre-swap dry-run that allocates all AudioClips first.
- If you can pre-allocate all AudioClips before the swap loop, do so — that restores the all-or-nothing guarantee. Ballpark cost: each AudioClip is ~1-2 KB; N=10 clips is 10-20 KB, fine.

**Step 2: Commit**

```bash
git add src/deluge/gui/views/session_view.cpp
git commit -m "feat: bounce TRACK scope — swap all AudioClips onto new output"
```

---

### Task 3.3: Phase 3 build, format, flash, hardware test

**Step 1: Format + build**

```bash
./dbt format && ./dbt build release
```

**Step 2: Commit formatting if changed**

```bash
git add -u && git commit -m "chore: format"   # only if format changed anything
```

**Step 3: Flash**

```bash
./dbt loadfw
```

**Step 4: Hardware verification**

- New song; add a synth track with 3 clips in sections 1, 2, 3 (different notes/loop lengths in each).
- Add a reverb send.
- Grid song view → hold any clip on that track + SELECT → select "Bounce Track".
- Watch: progress popup "Exporting stem 1 of 3", "2 of 3", "3 of 3".
- After completion: all 3 clips in that column should now be audio clips, all on one new AudioOutput.
- Press play in session mode: each clip plays as before; levels match; reverb is still wet (live send).
- Verify the source synth output is deleted from the output list.

If the "all 3 on one AudioOutput" test fails (each clip ends up on a separate AudioOutput), `createNewAudioOutput(sourceOutput)` is being called for each clip instead of once — recheck Task 3.2 structure.

---

## Phase 4 — Cross-type verification

No new code. Pure hardware-testing phase to confirm synth, kit, and audio-clip sources all work. If any type fails, create follow-up tasks as needed.

### Task 4.1: Synth source, various FX configurations

Hardware tests:
- Bare synth, no FX → bounce clip → A/B match
- Synth with mod FX + delay + EQ + distortion → bounce clip → match
- Synth with aggressive compressor + high makeup gain → bounce clip → peak levels match (meters within noise floor)
- Long release time → bounce clip → WAV longer than loop length; tail plays only on first pass

### Task 4.2: Kit source

Hardware tests:
- Kit with 4 drums firing → bounce clip → all drums in WAV
- Kit with per-drum FX (e.g., hi-hat with delay) → bounce clip → drum-level FX baked in
- Kit with kit-level reverb send → bounce clip → live reverb tail still present
- Kit whole-track bounce → multi-clip kit becomes multi-clip audio track

### Task 4.3: Audio-clip source

Hardware tests:
- AudioClip with simple sample, no FX → bounce clip → new AudioClip identical
- AudioClip with clip-level FX (filter sweep + delay) → bounce clip → new AudioClip flat, plays identically
- AudioClip track with 2 clips referencing different samples → bounce track → 2 flat AudioClips

### Task 4.4: Regression — stem export after bounce

Hardware test:
- Bounce a clip, then immediately run a normal stem export
- Verify stem export works normally (all flags restored)
- If it breaks, our save/restore logic missed a flag — check what else stem export sets in its `init` vs runtime

### Task 4.5: Cancel mid-bounce

Hardware test:
- Start a whole-track bounce on a track with many clips
- Press BACK midway
- Verify: song is unchanged; no orphan AudioOutputs; source synth still intact with all its clips

If cancel produces partial state, add explicit cancel-detection in the render loop (check `isUIModeActive(UI_MODE_STEM_EXPORT)` after each yield) and bail before any swap.

---

## Phase 5 — Final polish

### Task 5.1: Update changelog

**Files:**
- Modify: `CHANGELOG.md` (or the analogous file for this branch's docs site)

Add a one-liner under the "Features" section:

```
- **Bounce in place** — destructive render of synth, kit, or audio clip through current FX chain into a new audio clip. Single-clip or whole-track via ClipSettings context menu. Reverb send is preserved (live global reverb still applies).
```

Commit:

```bash
git add CHANGELOG.md
git commit -m "docs: changelog entry for bounce in place"
```

### Task 5.2: Sanity-review the diff

Run:

```bash
git log --oneline dev..HEAD
git diff dev...HEAD --stat
```

- Total diff should be in the ~250–400 line range (new SessionView method + ~10-line menu edits + ~10-line l10n additions).
- No unintended edits outside the listed files.
- If something looks bloated, decide whether to squash commits or leave the history granular.

### Task 5.3: Request code review

Per CLAUDE.md (test on hardware, trust user observations), the user does final review. Hand off with:

- Link to design doc: `docs/plans/2026-04-21-bounce-in-place-design.md`
- Link to this plan: `docs/plans/2026-04-21-bounce-in-place.md`
- Summary of what was hardware-tested vs not
- List of deliberately-deferred items (from design doc's "POC limitations")

---

## Open questions / risks tracked from design doc

1. **AudioOutput compressor default transparency** — verified during Task 2.4. If wrong, add explicit bypass.
2. **`createNewAudioOutput(replaceOutput=sourceInstrument)` behaviour for non-synth sources** — verified during Task 4.2 (kit) and Task 4.3 (audio-clip).
3. **`swapClips` across type-changing swaps** — exercised by the classic Convert to Audio path, so should be fine; verified by Task 2.4.

---

## Commit granularity rule

One commit per task. If a task produces a broken intermediate state (e.g., Task 2.1 adds render but no swap), the popup output makes it obvious — that's fine. Bisectability is preserved by not mixing multiple tasks into one commit.
