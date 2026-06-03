# Next Action — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add per-clip "next action" so a session clip can auto-stop or transition to next/prev/random/random_walk after N loops, staying within its track's contiguous section block. Subsumes `LaunchStyle::ONCE`.

**Architecture:** Two persistent `Clip` fields (`clipRepeats: uint8`, `nextAction: NextAction`) plus one runtime counter (`clipRepeatCount`). Generalize the existing `LaunchStyle::ONCE` re-arm block in `Session::doLaunch` to increment the counter each loop and, on reaching `clipRepeats`, arm the clip off plus arm a target (resolved by a new block-aware walker on `View`). Legacy `launchStyle="once"` XML translates to `(clipRepeats=1, nextAction=STOP)` on load; `LaunchStyle::ONCE` is removed from the enum.

**Tech Stack:** C++20/23, Deluge firmware, `./dbt build` (~2 min), RZ/A1 hardware validation.

**Design doc:** `docs/plans/2026-04-22-next-action-design.md`

---

## Testing Philosophy

Same rhythm as bounce-in-place (`docs/plans/2026-04-21-bounce-in-place.md`): this firmware layer has no unit-test coverage for launch semantics or clip UI. Per `CLAUDE.md`: test on hardware; trust user observation; don't assume code works because it compiles. Each phase ends on a flashable, hardware-verifiable checkpoint. Do not rebuild inside every task — consolidate builds at phase ends.

---

## Phase 1 — Data plumbing (no behavior change)

**End-of-phase hardware check:** save a song with default clips, reload — no diff. Legacy `launchStyle="once"` clips still work exactly as before (because we haven't touched the ONCE codepath yet).

### Task 1.1: Add `NextAction` enum

**Files:**
- Modify: `src/definitions_cxx.hpp` — add enum near `LaunchStyle` (around line 1101)

**Step 1: Add enum**

Find `enum class LaunchStyle { DEFAULT, FILL, ONCE };` (line 1101) and add immediately after it:

```cpp
enum class NextAction : uint8_t {
	STOP,
	NEXT,
	PREV,
	RANDOM,
	RANDOM_WALK,
};
constexpr uint8_t kNumNextActions = 5;
```

**Step 2: Commit**

```bash
git add src/definitions_cxx.hpp
git commit -m "next-action: add NextAction enum"
```

---

### Task 1.2: Add `clipRepeats`, `nextAction`, `clipRepeatCount` to `Clip`

**Files:**
- Modify: `src/deluge/model/clip/clip.h` — add fields + `onLaunch()` declaration
- Modify: `src/deluge/model/clip/clip.cpp` — initialize defaults; copy in the copy-ctor paths

**Step 1: Add fields in `clip.h`**

Find the existing `LaunchStyle launchStyle;` declaration and add immediately after it:

```cpp
	// Next-action behavior (subsumes old LaunchStyle::ONCE).
	// clipRepeats == 0 means infinite loop (today's DEFAULT behavior).
	// Otherwise valid values are {1, 2, 4, 8, 16}.
	uint8_t clipRepeats = 0;
	NextAction nextAction = NextAction::STOP;
	// Runtime-only: incremented at each loop-boundary launch event while
	// this clip is playing; compared against clipRepeats.
	uint8_t clipRepeatCount = 0;

	// Reset counter; call when this clip becomes active on a launch event.
	void onLaunch();
```

**Step 2: Initialize in `clip.cpp:59`**

Find `launchStyle = LaunchStyle::DEFAULT;` in the Clip constructor (line 59) and add immediately after:

```cpp
	clipRepeats = 0;
	nextAction = NextAction::STOP;
	clipRepeatCount = 0;
```

**Step 3: Copy in the two copy-paths at lines 96 and 104**

After each `launchStyle = otherClip->launchStyle;` add:

```cpp
	clipRepeats = otherClip->clipRepeats;
	nextAction = otherClip->nextAction;
	// clipRepeatCount is runtime-only; don't copy.
```

**Step 4: Implement `onLaunch` in `clip.cpp`**

Near the existing simple accessors (search for a short member like `lengthChanged` or `getLoopLength`) add:

```cpp
void Clip::onLaunch() {
	clipRepeatCount = 0;
}
```

**Step 5: Commit**

```bash
git add src/deluge/model/clip/clip.h src/deluge/model/clip/clip.cpp
git commit -m "next-action: add clipRepeats/nextAction fields on Clip"
```

---

### Task 1.3: Serialize new fields in XML write path

**Files:**
- Modify: `src/deluge/model/clip/clip.cpp` around line 688 (the launchStyle write block)

**Step 1: Extend the write block**

Find lines 688–690:

```cpp
	if (launchStyle != LaunchStyle::DEFAULT) {
		writer.writeAttribute("launchStyle", launchStyleToString(launchStyle));
	}
```

Replace with:

```cpp
	if (launchStyle != LaunchStyle::DEFAULT) {
		writer.writeAttribute("launchStyle", launchStyleToString(launchStyle));
	}
	if (clipRepeats != 0) {
		writer.writeAttribute("clipRepeats", clipRepeats);
	}
	if (nextAction != NextAction::STOP) {
		writer.writeAttribute("nextAction", nextActionToString(nextAction));
	}
```

`nextActionToString` is added in Task 1.4. (This file will not yet compile — next task fixes that.)

**Step 2: No commit yet** — build is broken until Task 1.4 lands. Stage the change:

```bash
git add src/deluge/model/clip/clip.cpp
```

---

### Task 1.4: Add `nextActionToString` / `stringToNextAction`

**Files:**
- Modify: `src/deluge/util/functions.h` — declarations
- Modify: `src/deluge/util/functions.cpp` — definitions, near `launchStyleToString` (line 1397)

**Step 1: Declarations in `functions.h`**

Find `LaunchStyle stringToLaunchStyle(char const* string);` and add below:

```cpp
char const* nextActionToString(NextAction action);
NextAction stringToNextAction(char const* string);
```

**Step 2: Definitions in `functions.cpp`**

After the closing brace of `stringToLaunchStyle` (line 1424) add:

```cpp
char const* nextActionToString(NextAction action) {
	switch (action) {
	case NextAction::STOP:
		return "stop";
	case NextAction::NEXT:
		return "next";
	case NextAction::PREV:
		return "prev";
	case NextAction::RANDOM:
		return "random";
	case NextAction::RANDOM_WALK:
		return "randomWalk";
	default:
		__builtin_unreachable();
		return "";
	}
}

NextAction stringToNextAction(char const* string) {
	if (!strcmp(string, "next")) {
		return NextAction::NEXT;
	}
	if (!strcmp(string, "prev")) {
		return NextAction::PREV;
	}
	if (!strcmp(string, "random")) {
		return NextAction::RANDOM;
	}
	if (!strcmp(string, "randomWalk")) {
		return NextAction::RANDOM_WALK;
	}
	return NextAction::STOP;
}
```

**Step 3: Commit (closes Task 1.3 + 1.4 together)**

```bash
git add src/deluge/util/functions.h src/deluge/util/functions.cpp src/deluge/model/clip/clip.cpp
git commit -m "next-action: serialize clipRepeats/nextAction in clip XML"
```

---

### Task 1.5: Read new fields (plus legacy `launchStyle="once"` translation)

**Files:**
- Modify: `src/deluge/model/clip/clip.cpp` around line 765 (the launchStyle read block)

**Step 1: Extend the read block**

Find lines 765–767:

```cpp
	else if (!strcmp(tagName, "launchStyle")) {
		launchStyle = stringToLaunchStyle(reader.readTagOrAttributeValue());
	}
```

Replace with:

```cpp
	else if (!strcmp(tagName, "launchStyle")) {
		char const* value = reader.readTagOrAttributeValue();
		// Legacy translation: "once" maps to the new-model (1, STOP) and
		// leaves launchStyle at DEFAULT. Done here (not in stringToLaunchStyle)
		// so the enum can drop ONCE in Phase 2.
		if (!strcmp(value, "once")) {
			launchStyle = LaunchStyle::DEFAULT;
			clipRepeats = 1;
			nextAction = NextAction::STOP;
		}
		else {
			launchStyle = stringToLaunchStyle(value);
		}
	}
	else if (!strcmp(tagName, "clipRepeats")) {
		clipRepeats = reader.readTagOrAttributeValueInt();
	}
	else if (!strcmp(tagName, "nextAction")) {
		nextAction = stringToNextAction(reader.readTagOrAttributeValue());
	}
```

**Step 2: Commit**

```bash
git add src/deluge/model/clip/clip.cpp
git commit -m "next-action: read clipRepeats/nextAction (with legacy 'once' translation)"
```

---

### Task 1.6: Build + hardware smoke for Phase 1

**Step 1: Build**

Run: `./dbt build`
Expected: success, no new warnings.

**Step 2: Flash**

Run: `./dbt loadfw` (or the raw Python fallback per memory `reference_loadfw_workaround.md`).

**Step 3: Hardware check — default clip roundtrip**

- Load an existing song (no ONCE clips).
- Save to a new name.
- Diff the XML: no new `clipRepeats`/`nextAction` attributes anywhere (defaults are suppressed).
- Reload, verify clips play normally.

**Step 4: Hardware check — legacy ONCE still works**

- Open a song with a ONCE clip from before this branch.
- Play it; verify it still plays once and stops (via the unchanged ONCE codepath — we haven't touched `session.cpp:635` yet).
- Save and reload; verify the clip plays once and stops again (it should now be `(1, STOP)` on re-save — inspect the XML to confirm no `launchStyle="once"` remains and that `clipRepeats="1"` is present).

**Step 5: No code change; Phase 1 complete.**

---

## Phase 2 — Subsume ONCE

**End-of-phase hardware check:** the ONCE codepath no longer exists; a legacy ONCE clip loaded from XML and a fresh `(clipRepeats=1, nextAction=STOP)` clip both behave identically to the pre-branch ONCE. `LaunchStyle` enum has only `DEFAULT` and `FILL`.

### Task 2.1: Wire `Clip::onLaunch()` resets

Goal: reset `clipRepeatCount = 0` when a clip transitions inactive → active (user arm, section arm, or arrival via a future next-action). The simplest reliable hook is in `Session::doLaunch` where `activeIfNoSolo` is set to true.

**Files:**
- Modify: `src/deluge/playback/mode/session.cpp`

**Step 1: Find the activation points**

Grep:
```bash
grep -n "activeIfNoSolo = true\|activeIfNoSolo=true" src/deluge/playback/mode/session.cpp
```

Expected: 2–3 hits within `doLaunch` and helpers. At each site where a clip transitions from inactive to active in `doLaunch`, add `thisClip->onLaunch();` immediately after (or alongside) the `activeIfNoSolo = true` assignment. Similarly for `soloingInSessionMode = true`.

**Step 2: Also hook the "late start" path**

At `session.cpp:1622` (the `launchStyle == DEFAULT` branch in the late-start code), immediately before or after the subsequent activation assignment, call `thisClip->onLaunch()`.

**Step 3: Commit**

```bash
git add src/deluge/playback/mode/session.cpp
git commit -m "next-action: reset clipRepeatCount on launch"
```

---

### Task 2.2: Generalize the re-arm block

**Files:**
- Modify: `src/deluge/playback/mode/session.cpp:633-638`

**Step 1: Replace the ONCE re-arm block**

Today at lines 633–638:

```cpp
		// Arm it again if a ONCE clip, so it stops at the launchEvent
		if (!isFillLaunch && (clip->activeIfNoSolo || clip->soloingInSessionMode)
		    && clip->launchStyle == LaunchStyle::ONCE && clip->armState == ArmState::OFF) {
			clip->armState = ArmState::ON_NORMAL;
			distanceTilLaunchEvent = std::max(distanceTilLaunchEvent, clip->loopLength);
		}
```

Replace with:

```cpp
		// Finite-repeat clips: tick the loop counter and, when the repeat count is
		// reached, arm self-off and (for non-STOP modes) arm the target on. Always
		// bump distanceTilLaunchEvent to the clip's loopLength so we get called
		// again at its next loop boundary.
		if (!isFillLaunch && (clip->activeIfNoSolo || clip->soloingInSessionMode)
		    && clip->launchStyle != LaunchStyle::FILL && clip->clipRepeats != 0
		    && clip->armState == ArmState::OFF) {

			clip->clipRepeatCount++;
			distanceTilLaunchEvent = std::max(distanceTilLaunchEvent, clip->loopLength);

			if (clip->clipRepeatCount >= clip->clipRepeats) {
				clip->armState = ArmState::ON_NORMAL;  // toggle self off at next event
				if (clip->nextAction != NextAction::STOP) {
					Clip* target = view.findNextActionTarget(clip, clip->nextAction);
					if (target && target != clip) {
						target->armState = ArmState::ON_NORMAL;  // toggle target on
					}
				}
			}
		}
```

Note: `view.findNextActionTarget` is stubbed in Task 2.3 (returns `nullptr`) and fully implemented in Phase 3. The `target != clip` guard is redundant for now but keeps the contract tidy when the walker might return the source on small blocks.

**Step 2: Commit (partial — still stubbed)**

Do not commit yet — build will fail until 2.3 adds the stub. Stage:

```bash
git add src/deluge/playback/mode/session.cpp
```

---

### Task 2.3: Stub `View::findNextActionTarget`

**Files:**
- Modify: `src/deluge/gui/views/view.h`
- Modify: `src/deluge/gui/views/view.cpp`

**Step 1: Declaration in `view.h`**

Near the existing `Clip* findNextClipForOutput(Output* output);`, add:

```cpp
	Clip* findNextActionTarget(Clip* source, NextAction mode);
```

**Step 2: Stub implementation in `view.cpp`**

Immediately after `findNextClipForOutput` (around line 2960), add:

```cpp
Clip* View::findNextActionTarget(Clip* source, NextAction mode) {
	// Phase 2 stub — real walker arrives in Phase 3.
	(void)source;
	(void)mode;
	return nullptr;
}
```

**Step 3: Commit (closes 2.2 + 2.3)**

```bash
git add src/deluge/gui/views/view.h src/deluge/gui/views/view.cpp src/deluge/playback/mode/session.cpp
git commit -m "next-action: generalize ONCE re-arm block (target walker stubbed)"
```

---

### Task 2.4: Update the "cancel arming" checks

There are two sites that exempt ONCE from arm-clearing sweeps. Replace the `launchStyle != ONCE` check with the equivalent new-model predicate: "this clip has a pending finite-repeat action pending."

**Files:**
- Modify: `src/deluge/playback/mode/session.cpp:1689`
- Modify: `src/deluge/model/song/song.cpp:3263`

**Step 1: Add a helper on `Clip`**

In `clip.h` near `onLaunch()`:

```cpp
	// True if this clip has a finite-repeat action that is actively arming
	// itself to toggle at a future launch event — used to exempt it from
	// global arm-clearing sweeps (replaces the old `launchStyle == ONCE` check).
	inline bool hasFiniteRepeatArming() const {
		return clipRepeats != 0 && clipRepeatCount >= clipRepeats;
	}
```

**Step 2: Update `session.cpp:1689`**

```cpp
							if (thisClip->armState != ArmState::OFF && thisClip->launchStyle != LaunchStyle::ONCE) {
```

becomes:

```cpp
							if (thisClip->armState != ArmState::OFF && !thisClip->hasFiniteRepeatArming()) {
```

**Step 3: Update `song.cpp:3263`**

```cpp
			if (clip->launchStyle != LaunchStyle::FILL && clip->launchStyle != LaunchStyle::ONCE) {
				clip->armState = ArmState::OFF;
			}
```

becomes:

```cpp
			if (clip->launchStyle != LaunchStyle::FILL && !clip->hasFiniteRepeatArming()) {
				clip->armState = ArmState::OFF;
			}
```

**Step 4: Commit**

```bash
git add src/deluge/model/clip/clip.h src/deluge/playback/mode/session.cpp src/deluge/model/song/song.cpp
git commit -m "next-action: replace ONCE-exemption checks with finite-repeat predicate"
```

---

### Task 2.5: Drop `LaunchStyle::ONCE` from the enum

**Files:**
- Modify: `src/definitions_cxx.hpp:1101`
- Modify: `src/deluge/util/functions.cpp:1397` and `:1414`
- Modify: `src/deluge/gui/views/view.cpp:2729-2739` (pad coloring)
- Modify: `src/deluge/gui/context_menu/clip_settings/launch_style.cpp:22-29` (menu options)

**Step 1: Shrink the enum**

`definitions_cxx.hpp:1101`:

```cpp
enum class LaunchStyle { DEFAULT, FILL };
```

**Step 2: Drop `case ONCE` from `launchStyleToString`/`stringToLaunchStyle`**

In `functions.cpp` remove the `case LaunchStyle::ONCE:` branch at line 1405 and the `if (!strcmp(string, "once"))` branch at lines 1418–1420. Legacy "once" XML is handled by the translation in `clip.cpp:765` (Task 1.5), so the string function never sees it.

**Step 3: Update view coloring (`view.cpp:2729-2739`)**

Replace:

```cpp
				switch (clip->launchStyle) {
				case LaunchStyle::FILL:
					thisColour = menu_item::fillColourMenu.getRGB();
					break;
				case LaunchStyle::ONCE:
					thisColour = menu_item::onceColourMenu.getRGB();
					break;
				default:
					thisColour = menu_item::stoppedColourMenu.getRGB();
				}
```

With:

```cpp
				if (clip->launchStyle == LaunchStyle::FILL) {
					thisColour = menu_item::fillColourMenu.getRGB();
				}
				// Any clip that will transition after a finite number of loops gets
				// the old "once" colour — visual cue that this clip has a plan.
				else if (clip->clipRepeats != 0) {
					thisColour = menu_item::onceColourMenu.getRGB();
				}
				else {
					thisColour = menu_item::stoppedColourMenu.getRGB();
				}
```

`onceColourMenu` stays named as-is (the name is fine; renaming it is churn for no benefit).

**Step 4: Remove ONCE from launch_style menu options**

In `launch_style.cpp:22-29`, find:

```cpp
std::span<char const*> LaunchStyleMenu::getOptions() {
	static const char* optionsls[] = {
	    "Default",
	    "Fill",
	    "Once",
	};
	return {optionsls, kNumValues};
}
```

Replace with:

```cpp
std::span<char const*> LaunchStyleMenu::getOptions() {
	static const char* optionsls[] = {
	    "Default",
	    "Fill",
	};
	return {optionsls, 2};
}
```

Also check `launch_style.h` for `kNumValues` — if it's `3`, change to `2`.

**Step 5: Build**

```bash
./dbt build
```

Expected: compiles cleanly. If any ONCE reference is left, the compiler catches it — fix the call site.

**Step 6: Commit**

```bash
git add src/definitions_cxx.hpp src/deluge/util/functions.cpp src/deluge/gui/views/view.cpp src/deluge/gui/context_menu/clip_settings/launch_style.cpp src/deluge/gui/context_menu/clip_settings/launch_style.h
git commit -m "next-action: remove LaunchStyle::ONCE (subsumed by clipRepeats+nextAction)"
```

---

### Task 2.6: Build + hardware test — ONCE compat

**Step 1: Flash**

```bash
./dbt loadfw
```

**Step 2: Legacy-ONCE roundtrip**

- Load a pre-branch song that contains a ONCE clip.
- Confirm the clip plays once and stops — identical to pre-branch.
- Save the song. Inspect the resulting XML — the clip should now have `clipRepeats="1"` and no `launchStyle="once"` attribute.
- Reload the saved XML; confirm behavior is still play-once-stop.

**Step 3: Fresh (1, STOP) clip**

- In Launch Style menu, note that ONCE is no longer an option.
- Create a clip and (via hex-edit of the saved XML, since UI doesn't exist yet) set `clipRepeats="1"`.
- Reload; confirm it plays once and stops.

**Step 4: Section/pad interaction regressions**

- Regular DEFAULT clips still loop forever.
- FILL clips still behave normally (fill-launch).
- Section arming + stopping still works.

If all green: Phase 2 complete.

---

## Phase 3 — Non-STOP next-action modes

**End-of-phase hardware check:** hex-edit a clip's XML to `(clipRepeats=1, nextAction=next)` in a 3-clip block; confirm the clip advances to the next on each loop. Repeat for PREV, RANDOM, RANDOM_WALK. Verify gap isolation.

### Task 3.1: Implement `View::findNextActionTarget`

**Files:**
- Modify: `src/deluge/gui/views/view.cpp` — replace the Phase 2 stub

**Step 1: Replace the stub**

```cpp
Clip* View::findNextActionTarget(Clip* source, NextAction mode) {
	if (!source || !source->output) {
		return nullptr;
	}
	if (mode == NextAction::STOP) {
		return nullptr;  // caller doesn't need a target for STOP
	}

	// Collect all clips on the same Output, indexed with their section number.
	// Stack-bounded: cap at kMaxPerOutput to avoid heap and to bound the sort.
	// If exceeded, fall back to STOP semantics by returning nullptr.
	constexpr int32_t kMaxPerOutput = 64;
	struct Entry {
		uint8_t section;
		Clip* clip;
	};
	Entry entries[kMaxPerOutput];
	int32_t count = 0;

	for (int32_t i = 0; i < currentSong->sessionClips.getNumElements(); i++) {
		Clip* c = currentSong->sessionClips.getClipAtIndex(i);
		if (!c || c->output != source->output) {
			continue;
		}
		if (count >= kMaxPerOutput) {
			return nullptr;
		}
		entries[count++] = {c->section, c};
	}
	if (count == 0) {
		return nullptr;
	}

	// Insertion sort by section ascending (count is tiny; std::sort would drag in deps).
	for (int32_t i = 1; i < count; i++) {
		Entry tmp = entries[i];
		int32_t j = i;
		while (j > 0 && entries[j - 1].section > tmp.section) {
			entries[j] = entries[j - 1];
			j--;
		}
		entries[j] = tmp;
	}

	// Locate source in the sorted list.
	int32_t sourceIdx = -1;
	for (int32_t i = 0; i < count; i++) {
		if (entries[i].clip == source) {
			sourceIdx = i;
			break;
		}
	}
	if (sourceIdx < 0) {
		return nullptr;
	}

	// Find the contiguous block bounds (inclusive). "Contiguous" = consecutive
	// section numbers with no gap >= 1.
	int32_t blockStart = sourceIdx;
	while (blockStart > 0
	       && entries[blockStart - 1].section + 1 == entries[blockStart].section) {
		blockStart--;
	}
	int32_t blockEnd = sourceIdx;
	while (blockEnd < count - 1
	       && entries[blockEnd + 1].section == entries[blockEnd].section + 1) {
		blockEnd++;
	}
	int32_t blockLen = blockEnd - blockStart + 1;

	auto atBlockIdx = [&](int32_t idxInBlock) -> Clip* {
		return entries[blockStart + idxInBlock].clip;
	};
	int32_t srcInBlock = sourceIdx - blockStart;

	switch (mode) {
	case NextAction::NEXT:
		return atBlockIdx((srcInBlock + 1) % blockLen);
	case NextAction::PREV:
		return atBlockIdx((srcInBlock - 1 + blockLen) % blockLen);
	case NextAction::RANDOM:
		if (blockLen == 1) {
			return source;
		}
		return atBlockIdx(random(blockLen - 1) % blockLen);
	case NextAction::RANDOM_WALK: {
		if (blockLen == 1) {
			return source;
		}
		bool forward = (random(1) & 1u) != 0;
		int32_t step = forward ? 1 : (blockLen - 1);
		return atBlockIdx((srcInBlock + step) % blockLen);
	}
	case NextAction::STOP:
	default:
		return nullptr;
	}
}
```

Note: `random(x)` is the existing firmware RNG — see `src/deluge/util/functions.h`. Confirm the arg semantics; adjust if the helper is `getRandom255()` or similar. Whatever the API, use a single-source PRNG to keep this deterministic within a run.

**Step 2: Build + format**

```bash
./dbt build
./dbt format
```

**Step 3: Commit**

```bash
git add src/deluge/gui/views/view.cpp
git commit -m "next-action: implement block-aware findNextActionTarget"
```

---

### Task 3.2: Hardware test — NEXT/PREV/RANDOM/RANDOM_WALK

No UI yet — use hex-edit of saved XML to configure test clips.

**Step 1: Set up a 3-clip block on one track**

- In Session view, create one synth track.
- Add 3 clips to that track in sections 0, 1, 2 (contiguous — no gaps).
- Give them distinguishable content (three different notes).

**Step 2: Test NEXT**

- Save song, edit XML: on the clip in section 0, add `clipRepeats="1" nextAction="next"`. Save and load.
- Launch the section 0 clip. Expected: plays one loop, advances to section 1 clip, plays one loop, advances to section 2, advances back to section 0 (wrap).
- Repeat with `clipRepeats="2"` — expect 2 loops per clip.

**Step 3: Test PREV**

- Edit XML: `clipRepeats="1" nextAction="prev"` on section 2 clip. Launch; expect 2→1→0→2 cycle.

**Step 4: Test RANDOM**

- `clipRepeats="1" nextAction="random"` on section 0. Launch. Expect each launch picks any of the 3 clips (including possibly staying on self).

**Step 5: Test RANDOM_WALK**

- `clipRepeats="1" nextAction="randomWalk"`. Expect each transition goes to an adjacent clip in the block.

**Step 6: Test gap isolation**

- Extend the track to clips at sections 0, 1, 2 and 4, 5 (gap at 3).
- `nextAction="next"` on section 2 with `clipRepeats="1"`. Expect it to wrap to section 0 (NOT jump to 4).
- `nextAction="next"` on section 4. Expect 4→5→4→5 only.

**Step 7: Test manual override**

- `(1, next)` on section 0. Launch it, then before the first loop ends, manually press the pad for section 5. Expect: section 5 plays at next boundary (user wins, not the auto-next to section 1).

**Step 8: Cleanup — no commit for tests** (XML hex-edits are disposable)

If issues surface: hypothesize, fix, rerun matrix. When all pass: Phase 3 complete.

---

## Phase 4 — UI menu entries

**End-of-phase hardware check:** from a clip's context menu, set `Clip Repeats` and `Next Action`. Values persist across save/load. Behavior matches Phase 3 tests.

### Task 4.1: Localization strings

**Files:**
- Modify: `src/deluge/gui/l10n/strings.h`
- Modify: `src/deluge/gui/l10n/g_english.cpp`
- Modify: `src/deluge/gui/l10n/english.json`
- Modify: `src/deluge/gui/l10n/g_seven_segment.cpp`
- Modify: `src/deluge/gui/l10n/seven_segment.json`

**Step 1: Add enum entries in `strings.h`**

Near the existing `STRING_FOR_LAUNCH_STYLE` (or the nearest analogous entry), add:

```cpp
	STRING_FOR_CLIP_REPEATS,
	STRING_FOR_NEXT_ACTION,
	STRING_FOR_NEXT_ACTION_STOP,
	STRING_FOR_NEXT_ACTION_NEXT,
	STRING_FOR_NEXT_ACTION_PREV,
	STRING_FOR_NEXT_ACTION_RANDOM,
	STRING_FOR_NEXT_ACTION_RANDOM_WALK,
```

**Step 2: Add strings in `g_english.cpp` / `english.json`**

```cpp
	{STRING_FOR_CLIP_REPEATS, "Clip Repeats"},
	{STRING_FOR_NEXT_ACTION, "Next Action"},
	{STRING_FOR_NEXT_ACTION_STOP, "Stop"},
	{STRING_FOR_NEXT_ACTION_NEXT, "Next"},
	{STRING_FOR_NEXT_ACTION_PREV, "Prev"},
	{STRING_FOR_NEXT_ACTION_RANDOM, "Random"},
	{STRING_FOR_NEXT_ACTION_RANDOM_WALK, "Random Walk"},
```

Mirror the same entries in `english.json`.

**Step 3: Seven-segment mappings**

In `g_seven_segment.cpp` / `seven_segment.json`:

```cpp
	{STRING_FOR_CLIP_REPEATS, "REPT"},
	{STRING_FOR_NEXT_ACTION, "NACT"},
	{STRING_FOR_NEXT_ACTION_STOP, "STOP"},
	{STRING_FOR_NEXT_ACTION_NEXT, "NEXT"},
	{STRING_FOR_NEXT_ACTION_PREV, "PREV"},
	{STRING_FOR_NEXT_ACTION_RANDOM, "RAND"},
	{STRING_FOR_NEXT_ACTION_RANDOM_WALK, "RWLK"},
```

**Step 4: Commit**

```bash
git add src/deluge/gui/l10n/
git commit -m "l10n: add Clip Repeats / Next Action strings"
```

---

### Task 4.2: Add `ClipRepeatsMenu` class

**Files:**
- Create: `src/deluge/gui/context_menu/clip_settings/clip_repeats.h`
- Create: `src/deluge/gui/context_menu/clip_settings/clip_repeats.cpp`

Structure mirrors `launch_style.{h,cpp}` closely. Menu offers 6 values: ∞ / 1 / 2 / 4 / 8 / 16, stored as the raw uint8 (0 for ∞). Display strings "Inf", "1", "2", "4", "8", "16".

**Step 1: Write `clip_repeats.h`**

Copy `launch_style.h` and adapt: class `ClipRepeatsMenu`, `kNumValues = 6`. Declaration of `clipRepeats` instance.

**Step 2: Write `clip_repeats.cpp`**

Copy `launch_style.cpp`. Adapt:

- `getTitle` → returns `"Clip Repeats"` (via STRING_FOR_CLIP_REPEATS).
- `getOptions` → `{"Inf","1","2","4","8","16"}`.
- `selectEncoderAction` — map menu index to raw uint8: index 0 → 0, 1 → 1, 2 → 2, 3 → 4, 4 → 8, 5 → 16. Assign to `clip->clipRepeats`.
- `setupAndCheckAvailability` — set `currentOption` from current `clip->clipRepeats`: 0 → 0, 1 → 1, 2 → 2, 4 → 3, 8 → 4, 16 → 5.

**Step 3: Commit**

```bash
git add src/deluge/gui/context_menu/clip_settings/clip_repeats.{h,cpp}
git commit -m "next-action: add Clip Repeats context menu"
```

---

### Task 4.3: Add `NextActionMenu` class

**Files:**
- Create: `src/deluge/gui/context_menu/clip_settings/next_action.h`
- Create: `src/deluge/gui/context_menu/clip_settings/next_action.cpp`

Same shape as `ClipRepeatsMenu`. 5 options — Stop/Next/Prev/Random/Random Walk — each maps 1:1 to the `NextAction` enum.

**Step 1: Write the two files**

Mirror `clip_repeats.{h,cpp}` with the different title + option list. Store as `static_cast<NextAction>(currentOption)`.

**Step 2: Commit**

```bash
git add src/deluge/gui/context_menu/clip_settings/next_action.{h,cpp}
git commit -m "next-action: add Next Action context menu"
```

---

### Task 4.4: Wire into `ClipSettingsMenu`

**Files:**
- Modify: `src/deluge/gui/context_menu/clip_settings/clip_settings.cpp`

**Step 1: Add menu entries**

In `getOptions()` in `clip_settings.cpp`, locate the existing entries that open the `LaunchStyleMenu`. Add two entries (one for `ClipRepeatsMenu`, one for `NextActionMenu`) to both the instrument branch and the audio-clip branch.

**Step 2: Handle the selections in `acceptCurrentOption`**

For each new menu entry, open the corresponding sub-menu (same pattern as the existing LaunchStyle entry).

**Step 3: Commit**

```bash
git add src/deluge/gui/context_menu/clip_settings/clip_settings.cpp
git commit -m "next-action: wire Clip Repeats and Next Action into ClipSettingsMenu"
```

---

### Task 4.5: Build + hardware smoke

```bash
./dbt build && ./dbt loadfw
```

**Hardware checklist:**

- On any clip, open Clip Settings menu. See "Clip Repeats" and "Next Action" entries.
- Pick "Clip Repeats" → cycle through ∞ / 1 / 2 / 4 / 8 / 16. Pick 4.
- Pick "Next Action" → cycle through Stop/Next/Prev/Random/Random Walk. Pick Next.
- Save the song; open XML, confirm `clipRepeats="4" nextAction="next"`.
- Launch the clip; confirm it advances to next at end of loop 4.

If all green: Phase 4 complete.

---

## Phase 5 — Verification matrix

Run the full design-doc smoke matrix on hardware. No code changes expected; if any test fails, triage back to the relevant phase.

### Task 5.1: `(∞, stop)` default

Unchanged loop — identical to pre-branch DEFAULT behavior.

### Task 5.2: `(1, stop)` identity with old ONCE

Configure via UI; compare feel vs. a pre-branch ONCE song loaded on the new firmware.

### Task 5.3: `(4, next)` on a 3-clip block

Configure via UI. Verify 4 loops, advance, repeat, wrap.

### Task 5.4: `(2, prev)` on the same 3-clip block

Verify 2 loops, advance backwards, wrap.

### Task 5.5: `(1, random)` and `(1, random_walk)` visual check

Watch 30+ transitions. Random feels uniformly distributed. Random walk stays adjacent.

### Task 5.6: Gap isolation

Track with two separate blocks; configure a `next`-mode clip in each block. Confirm neither crosses the gap.

### Task 5.7: Manual override mid-countdown

Configure `(4, next)`. On loop 2 of 4, manually arm a different track's clip in a different column. Confirm the user arm wins at next boundary.

### Task 5.8: Legacy song load

Open a pre-branch song with a ONCE clip. Confirm it plays once and stops. Save; confirm XML uses the new `clipRepeats`/`nextAction` attributes.

### Task 5.9: Regression smoke

Basic checks that nothing else is broken:

- Section launch still arms a whole section.
- FILL clips still behave as fill launches.
- Bounce-in-place (the parent branch's feature) still works on a track containing next-action clips.
- Stem export still works.

If all green: ready for Phase 6.

---

## Phase 6 — Polish

### Task 6.1: Update changelog

**Files:**
- Modify: the repo's changelog (check repo root for `CHANGELOG.md` or similar — bounce-in-place added an entry there)

Add a bullet under the next version: "Per-clip Next Action: clips can auto-stop, advance, go back, or jump randomly within their track's block after a configurable number of loops."

**Commit:**

```bash
git add <changelog file>
git commit -m "changelog: next-action per-clip launch behavior"
```

### Task 6.2: Sanity-review the diff

Run `git log --oneline community..dev -- '*next-action*' src/deluge src/definitions_cxx.hpp` and skim each commit. Look for:

- Any `LaunchStyle::ONCE` reference remaining → bug.
- Any `clipRepeatCount` read from XML → bug (it's runtime-only).
- Any mutation of `clipRepeatCount` outside `Clip::onLaunch()` or the `doLaunch` re-arm block → likely a bug.

Open a scratch file noting anything worth a follow-up; no commit unless you change code.

### Task 6.3: Request code review

Use the `superpowers:requesting-code-review` skill to package the branch for review. Attach:

- Design doc: `docs/plans/2026-04-22-next-action-design.md`
- Plan doc: this file
- Verification matrix results from Phase 5
- Any follow-ups noted in Task 6.2

---

## Follow-ups (deferred)

- Extended repeat counts (e.g. arbitrary N rather than presets). Current set {1,2,4,8,16} covers musical cases; extending later is additive.
- Configurable per-clip *ordering* within the block (today: section-ascending). Expose as a per-clip flag if users want reverse-by-default blocks.
- Chain definitions by explicit targets (clip A → clip C skipping B). Out of scope; grid-derived ordering is what the design commits to.
- Runtime-toggleable random seed for reproducible live sets. YAGNI until asked.
- **RANDOM_NEAR / Lévy-flight mode** — random transitions biased toward adjacent clips with a heavy tail for occasional far jumps. Needs a design pass on the weighting distribution and the UX knob (if any).
- **Suppress "beats remaining" popup when a next-action is the event driver** — the countdown popup makes sense for user-armed launches but is visual clutter when transitions are automatic per-clip. Investigation target: wherever the popup is triggered by `scheduleLaunchTiming` / `armingChanged`; gate it on whether the pending launch was set up by a finite-repeat re-arm vs. a user action.
- **Blinking/armed-state visual during pending next-action countdown** — the classic arm-based transitions show a blinking pad while the countdown runs. Our deferred-transition mechanism doesn't set `armState`, so no blink appears. Audio + mid-playback render both work; only the "something is coming" visual cue is missing. Tractable polish: key a new pad color (or blink state) off `Clip::pendingNextActionTransition`, or mirror `armState = ON_NORMAL` at the wrap purely for visual purposes while ensuring no downstream handler acts on it.
- **Solo carry-over on next-action transitions** — if the source clip is `soloingInSessionMode` when its threshold hits, `Session::processPendingNextActionTransitions` deactivates source but never propagates the solo state to the target, and never clears `soloingInSessionMode` on source. After the swap, `Song::isClipActive(source)` still returns true (lingering solo flag) and `currentSong->anyClipsSoloing` is stale. Likely fix: carry solo across (`target->soloingInSessionMode = clip->soloingInSessionMode; clip->soloingInSessionMode = false;`) and call `currentSong->reassessWhetherAnyClipsSoloing()` for the STOP case. Phase-5 hardware testing did not exercise solo + next-action.
- **Vestigial popup-suppression flag and predicate** — `Session::launchEventIsFromNextAction` and `Clip::hasFiniteRepeatArming()` were introduced earlier to suppress the "Beats Remaining" popup and to exempt finite-repeat-armed clips from arm-clearing sweeps (in session.cpp:1773 and song.cpp:3263). After the deferred-transition decouple removed the original re-arm block, the predicate window is one tick and the user-driven sweep paths effectively never overlap. The two `anyNextActionArmed` scans in session.cpp (~lines 720, 2320) are dead in the common case. Cleanup: either delete the flag + predicate + their callers (~20 lines), or rewrite their comments to call out the rare-overlap-only contract. Harmless either way; pure hygiene.
