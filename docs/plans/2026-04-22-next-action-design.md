# Next Action — Design

## Summary

Add per-clip "next action" behavior: when a session-mode clip finishes a user-defined number of loops, it automatically transitions to another clip in the same track (or stops). Five finish modes: `stop`, `next`, `prev`, `random`, `random_walk`. Repeat counts: ∞ (default), 1, 2, 4, 8, 16. Subsumes the existing `LaunchStyle::ONCE` (= `repeats=1, nextAction=stop`).

## Goals

- Two orthogonal per-clip controls: **Clip Repeats** (how many loops before the action) and **Next Action** (what the action is).
- Navigation modes (`next`, `prev`, `random`, `random_walk`) are **scoped to the current track** and **confined to the current contiguous block** of clips in that column — gaps on the grid are hard walls that only a manual launch crosses.
- Manual launches always override any pending next-action arm.
- Session mode only. Arrangement mode ignores these fields.
- Backward-compatible song files: legacy `launchStyle="once"` clips load as `(repeats=1, nextAction=stop)`.

## Non-goals

- Per-clip scope override (same-track scope is fixed).
- Cross-track chaining (e.g. "advance to a clip on a different track").
- Weighted or Markov-style random selection — uniform random within the block is enough.
- Chain definition by explicit targets (user-authored clip-to-clip chain). Ordering is grid-derived.
- New arrangement-mode behavior.
- Unit-test harness for launch semantics — validated on hardware.

## Architecture

### Data model

On `Clip` (applies to both `InstrumentClip` and `AudioClip`):

```cpp
enum class NextAction : uint8_t {
    STOP,
    NEXT,
    PREV,
    RANDOM,
    RANDOM_WALK,
};

// New persistent fields:
uint8_t    clipRepeats = 0;   // 0 == ∞; else one of {1,2,4,8,16}
NextAction nextAction  = NextAction::STOP;

// New runtime-only field (not serialized):
uint8_t    clipRepeatCount = 0; // reset on every launch
```

`LaunchStyle` enum loses `ONCE`; becomes `{DEFAULT, FILL}`. `FILL` is orthogonal (it governs *when* a clip launches, not *what happens after*), so fill clips ignore `nextAction`/`clipRepeats`.

### Runtime hook

The existing `ONCE` re-arm block in `Session::doLaunch` (around `session.cpp:634`) is generalized. On each launch event, for each playing clip:

1. If `clipRepeats == 0` (∞) → skip (today's default).
2. Increment `clipRepeatCount`.
3. If `clipRepeatCount >= clipRepeats`:
   - If `nextAction == STOP`: arm self off — same as today's ONCE re-arm (`armState = ON_NORMAL`, align `distanceTilLaunchEvent` to `loopLength`).
   - Else: resolve the target via the block-aware walker, arm self off, arm target on (`target->armState = ON_NORMAL`), align `distanceTilLaunchEvent`.
   - If no valid target (degenerate: block of size 1 and mode ≠ STOP): no-op — clip re-arms to loop itself another N times (counter resets on re-launch).

Manual overrides: any clip the user has explicitly armed (via pad press, section arm, etc.) already has `armState != OFF` by the time `doLaunch` iterates. The next-action arming logic only touches clips whose `armState == OFF` (matches today's ONCE gate), so a user arm is never clobbered.

The loop counter resets whenever a clip (re-)launches — i.e., in whatever code path today sets `activeIfNoSolo = true` on launch (`Session::doLaunch` and `Clip::beginInstance`/`setupAsNewPlayback`). A small helper `Clip::onLaunch()` does the reset.

### Block-aware walker

New helper on `View` (same file as `findNextClipForOutput`, `view.cpp:2960`):

```cpp
// Returns target for nextAction resolution. Stays within the contiguous
// section block containing `source` on `output`. Returns nullptr only if
// the block has size 1 and mode ≠ STOP (caller treats as no-op).
Clip* View::findNextActionTarget(Clip* source, NextAction mode);
```

Algorithm:

1. Collect all clips on `source->output` into a local list, sorted ascending by `section`.
2. Walk from `source` outward to find the block: contiguous section numbers (no gap ≥ 1 row).
3. Resolve by mode:
   - `NEXT` → index+1 in block, wrap to first.
   - `PREV` → index-1 in block, wrap to last.
   - `RANDOM` → uniform random over block (may return `source` itself on small blocks — acceptable; stay-on-self is a valid jukebox outcome).
   - `RANDOM_WALK` → 50/50 pick between the `NEXT` and `PREV` results.

Random source: existing `random()` helper (same PRNG used elsewhere in the firmware). No determinism required.

Allocation: the local sorted list is stack-bounded — cap at a small constant (e.g. 64 clips per output; today's songs don't approach this) and bail to `STOP` semantics if exceeded. Avoids heap allocation in the audio path.

### UI

`gui/context_menu/clip_settings/` gains two new menu entries, beside the existing Launch Style entry:

- **Clip Repeats** — picker: `∞ / 1 / 2 / 4 / 8 / 16`
- **Next Action** — picker: `Stop / Next / Prev / Random / Random Walk`

Existing Launch Style menu loses the `ONCE` option (both options list `DEFAULT` and `FILL` only).

Display conventions are matched to the existing Launch Style menu (OLED full name, seven-segment abbreviations).

Localization: new strings added to `strings.h`, `g_english.cpp`, `g_seven_segment.cpp`, `english.json`, `seven_segment.json`.

### Serialization

Per-clip XML attributes (both `InstrumentClip` and `AudioClip`):

- `launchStyle` — unchanged semantics, but emitter never writes `"once"` (always translates to the two new fields).
- `clipRepeats` — `"0"` (= ∞, skipped when default) / `"1"` / `"2"` / `"4"` / `"8"` / `"16"`.
- `nextAction` — `"stop"` / `"next"` / `"prev"` / `"random"` / `"randomWalk"`. Defaults to `"stop"` (skipped when default).

Read-path compat:

- `launchStyle="once"` → set `launchStyle = DEFAULT`, `clipRepeats = 1`, `nextAction = STOP`.
- Missing new attributes → defaults (`∞` + `STOP`).

Write-path omits attributes equal to defaults to keep diffs clean.

### Files touched (estimate)

- `src/definitions_cxx.hpp` — `LaunchStyle` drops `ONCE`; new `NextAction` enum.
- `src/deluge/model/clip/clip.{h,cpp}` — new fields, `onLaunch()` helper, read/write compat.
- `src/deluge/playback/mode/session.cpp` — generalize the `ONCE` re-arm block (~30 lines changed).
- `src/deluge/gui/views/view.{cpp,h}` — `findNextActionTarget` (~60 lines).
- `src/deluge/gui/context_menu/clip_settings/` — two new menu items; remove `ONCE` from Launch Style menu.
- Localization: `strings.h`, `g_english.cpp`, `g_seven_segment.cpp`, `english.json`, `seven_segment.json`.

No changes to the audio engine, rendering path, or Arrangement view.

## Data flow

### Normal case: `(repeats=4, nextAction=next)` in a 3-clip block

```
Launch boundary 1: clip A starts, clipRepeatCount=1, 1<4, no action.
Launch boundary 2: loopCount=2, 2<4, no action.
Launch boundary 3: loopCount=3, 3<4, no action.
Launch boundary 4: loopCount=4, 4>=4:
  -> findNextActionTarget(A, NEXT) returns B (the next clip in block)
  -> A.armState = ON_NORMAL  (toggle off at next event)
  -> B.armState = ON_NORMAL  (toggle on at next event)
  -> distanceTilLaunchEvent = max(current, A.loopLength)
Launch boundary 5: A stops, B starts; B.clipRepeatCount reset to 0 by onLaunch().
```

### Manual override mid-chain

```
Clip A playing, loopCount=2/4.
User presses clip-pad for clip C -> C.armState set by section launch logic.
Launch boundary: doLaunch iterates; A still has armState=OFF at this point's
  ONCE-style check, so loopCount increments to 3/4. No next-action fires.
  Section-launch logic toggles A off and C on (separate code path, user-arm).
Next boundary: C active, A inactive. A's loopCount not incremented since it's
  not playing. When user re-launches A later, onLaunch() resets it to 0.
```

### End-of-block wrap

```
Block = [A, B, C] on same output. Playing C with (4, next).
Boundary N: loopCount=4/4. findNextActionTarget(C, NEXT):
  - sorted-by-section: [A, B, C]
  - C is last in block, wrap to first -> returns A.
```

### Gap isolation

```
Track has clips at sections {0,1,2} (block 1) and {5,6} (block 2).
Clip at section 2 with (2, next):
  Boundary N: loopCount=2, findNextActionTarget(clip2, NEXT):
    - sorted: [clip0, clip1, clip2, clip5, clip6]
    - walk from clip2: clip1 at section 1 is contiguous; clip5 at section 5 is
      NOT (gap at 3,4) -> block = [clip0, clip1, clip2].
    - NEXT wraps within block -> returns clip0.
```

## Error handling / edge cases

- **Block of size 1**: `NEXT`/`PREV`/`RANDOM`/`RANDOM_WALK` all resolve to self. Arming self-off-then-self-on is a no-op; clip just continues looping and loopCount resets. Effectively "loop forever" when the track has no siblings in the block.
- **Clip deleted mid-transition**: resolved target must still be valid at launch time. Since we arm by pointer and `doLaunch` runs per-boundary, and clip deletion goes through the UI (not mid-render), this is the same failure mode as any stale clip pointer — caught by existing checks.
- **FILL clips**: `nextAction` / `clipRepeats` fields exist but are ignored. UI does not show the menu entries for fill-style clips (or shows them grayed — pick in implementation).
- **Arrangement mode**: session-mode fields are ignored by `Arrangement::doLaunch` equivalents. No change there.
- **Solo**: resolved target inherits arm as normal. If the soloed clip's target is not currently soloed, behavior matches current ONCE solo semantics (the next clip plays subject to solo rules). Edge case documented; not a v1 concern.
- **Repeat count of 1 with nextAction=stop**: byte-for-byte identical to today's ONCE. Covered by load-path translation.
- **User arms target clip while pending next-action arm**: the user arm happens first (it's an explicit UI action); the next-action arming code only touches clips with `armState == OFF`, so we don't clobber the user's arm on the target.

## Testing

No unit-test harness for this layer on the Deluge — validated on hardware.

**Smoke checklist**:

- `(∞, stop)` (default): identical to today's default loop.
- `(1, stop)`: identical to today's `ONCE`.
- `(4, next)` on a 3-clip block: A→A→A→A→B observed on each 4-loop boundary.
- `(2, prev)` on a 3-clip block: C→C→B→B→A→A→C…
- `(1, random)` on a 5-clip block: each launch picks a fresh clip; visually uniform over ~50 launches.
- `(1, random_walk)` on a 4-clip block: each launch stays adjacent to prior.
- Gap isolation: clips separated by an empty row never cross.
- Manual override: hitting any pad mid-countdown wins.
- Legacy song: a song saved under pre-feature firmware with a `launchStyle="once"` clip loads as `(1, stop)` and behaves identically.
- Bounce interaction: bouncing a track containing clips with next-action set does not corrupt the feature (this lands on `dev` which already has bounce-in-place).

## MVP scope

Ship all five finish modes together. The mechanism is shared; per-mode cost is only in the walker resolution and the menu strings. Splitting would double the review + hardware-test cycles without meaningful risk reduction.
