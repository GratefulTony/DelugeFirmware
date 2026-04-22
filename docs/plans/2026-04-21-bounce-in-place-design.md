# Bounce In Place — Design

## Summary

Add a destructive "bounce in place" operation that renders a clip (or whole track) through its current FX chain and replaces the source with an AudioClip referencing the rendered WAV. Supports synth tracks, kit tracks, and audio-clip tracks. Reuses the existing StemExport infrastructure for rendering.

## Goals

- One-button bounce from the grid or rows song view, via the existing `ClipSettingsMenu`.
- Two scopes: single clip (one grid square) and whole track (one column / one output).
- Works identically for synth, kit, and audio-clip sources.
- Reverb send is preserved on the new AudioOutput so global reverb stays live/tweakable after bounce.
- POC keeps the patch small: hardcoded config, reuses stem export UI, no new Runtime Feature entries.

## Non-goals (for POC)

- A configurable "include song FX" / "include reverb tail" toggle (can be added later).
- Preserving source track's per-Sound FX settings onto the new AudioClip's clip-level FX for further tweaking (that's a "freeze track" workflow, different feature).
- Non-destructive bounce (keep source muted). Destructive replace is the name of the feature.
- A bespoke progress UI — stem export's existing "Exporting N of M" popup is fine.

## Architecture

### Entry point

```cpp
void SessionView::bounceInPlace(Clip* clip, BounceScope scope);
enum class BounceScope { CLIP, TRACK };
```

This is the only new public surface. All orchestration lives here.

### UX trigger

`gui/context_menu/clip_settings/clip_settings.cpp` adds two options to both branches of `getOptions()` (instrument and audio):

- "Bounce Clip"
- "Bounce Track"

In `acceptCurrentOption`, each dispatches to `sessionView.bounceInPlace(clip, BounceScope::CLIP|TRACK)`.

### Core reuse — StemExport

`bounceInPlace` configures `stemExport`:

- `includeSongFX = false`
- `includeKitFX = true` (default, and only matters for DRUM scope which we don't use)
- `allowNormalization = false`
- `renderOffline = true`
- Output folder: `AudioRecordingFolder::STEMS` (reuse the existing path; no new folder for POC)

It then iterates `startCurrentStemExport` + yield loop for each target clip, collects the produced WAV paths, and performs the post-render swap.

### Files touched (estimate)

- `gui/context_menu/clip_settings/clip_settings.{cpp,h}` — 2 new menu options in both branches
- `gui/views/session_view.{cpp,h}` — new `bounceInPlace` method (~150–200 lines)
- `gui/l10n/english.json`, `g_english.cpp`, `strings.h`, `g_seven_segment.cpp`, `seven_segment.json` — "Bounce Clip" / "Bounce Track" strings

No changes to `Song`, to the rendering engine, or to serialization.

## Data flow

### Single clip bounce (scope = CLIP)

```
User selects "Bounce Clip" from ClipSettingsMenu
  -> acceptCurrentOption calls sessionView.bounceInPlace(clip, CLIP)
  -> Save source reverb-send amount from clip->output
  -> Save prior values of: stemExport.includeSongFX, renderOffline, allowNormalization
  -> Configure stemExport (as above) with currentStemExportType = CLIP
  -> Mark only target clip with exportStem = true (reuses disarmAllClipsForStemExport pattern)
  -> startCurrentStemExport(CLIP, clip->output, mute state ref, clipIndex, true)
  -> yield() until render complete (same predicate stem export uses)
  -> Read wavFileNameForStemExport
  -> restoreAllClipMutes to revert the arming side-effects
  -> Restore stemExport prior values
  -> Create new AudioOutput via Song::createNewAudioOutput()
  -> Copy reverb-send amount from source onto new AudioOutput
  -> Create new AudioClip, set sampleHolder to the WAV path, loop length = source loopLength
  -> newClip->setOutput(newAudioOutput)
  -> swapClips(newAudioClip, sourceClip, clipIndex)
  -> If source SoundInstrument has no remaining clips, delete it via existing deleteOutput path
  -> view.setActiveModControllableTimelineCounter(newAudioClip)
  -> requestRendering(this)
```

### Whole-track bounce (scope = TRACK)

Same as above but:

- Collect every clip on `clip->output` (ordered by section).
- For each source clip: configure, `startCurrentStemExport`, yield, store `(sourceClip, wavPath)` pair.
- All-or-nothing: if any render fails, abort before any swap — user's song is unchanged. See Error handling.
- Once all WAVs are on disk, create ONE new AudioOutput via `createNewAudioOutput(replaceOutput = sourceInstrument)`. That lands it in the same position in the output list as the source.
- Copy reverb send once onto the new AudioOutput.
- For each `(sourceClip, wavPath)`: create AudioClip, assign to new AudioOutput, swap into the session at the source clip's index.
- When all source clips have been swapped, the source SoundInstrument/Kit/AudioOutput has no clips referencing it; `createNewAudioOutput(replaceOutput=…)` handles the output-list removal.

### Source-type specialization

None. The orchestration is identical for synth, kit, and audio-clip sources. Stem export already renders each correctly at `CLIP` / `TRACK` scope:

- Synth: renders voice chain → per-Sound FX → compressor → volume → pan. Reverb send path populates the reverb bus but `renderReverb()` is skipped (`includeSongFX=false`).
- Kit: renders per-drum voices through kit affect-entire FX (kit.cpp:691 — non-DRUM scopes always use the full render path).
- AudioClip: renders the sample through the clip-level FX chain (always on; no stem-export carve-out) and AudioOutput FX chain.

All three paths produce a WAV at the source's final per-output level, minus reverb tail.

## Gain staging

**Baked into the render** (via `renderAudioForStemExport` with `includeSongFX=false`, `allowNormalization=false`):

- Oscillator/voice mix (synth) or per-drum voices (kit) or sample playback (audio clip)
- Per-voice filter (synth)
- Sound-level FX (mod FX, delay, EQ, distortion)
- Per-track compressor (including makeup gain)
- Per-track volume
- Pan (as stereo level balance in the rendered L/R)
- Clip-level FX (audio clip source only — always applied)

**Stays "live" on the new AudioOutput:**

- Volume: unity (0 dB default). Bounced audio already carries source-volume, so unity here = source level in mix.
- Compressor: **must be explicitly bypassed** on the new output. Default is believed to be off/bypass but verify during implementation — if the default does anything non-transparent, the bounce would double-process the already-compressed audio (especially damaging for sources with aggressive makeup gain pushing peaks high).
- Pan: center. Stereo positioning is already in the rendered L/R.
- Reverb send: **copy from source**. One and only preserved parameter.
- ModFX, delay, EQ: off / flat defaults. No double-effects.

**Master bus FX** (global compressor/EQ/volume): neither baked nor preserved — they're applied live during main-mix playback as always, which is correct behavior (they should apply once, to the final mix).

## Error handling

### Preconditions (checked before any render)

- `clip->type == ClipType::INSTRUMENT` or `ClipType::AUDIO` (else menu option shouldn't have shown)
- `clip->output->type != OutputType::MIDI_OUT && != OutputType::CV` — popup "Can't bounce MIDI/CV"
- `playbackHandler.recording != RecordingMode::ARRANGEMENT` — reuse stem export's guard
- For TRACK scope: at least one non-empty clip on the output; else popup "Nothing to bounce"
- SD card present & writable (stem export already fails gracefully; we propagate)

### During render

- OOM creating AudioClip/AudioOutput: `displayError(Error::INSUFFICIENT_RAM)`, abort, song untouched (we haven't swapped yet).
- SD write failure: stem export surfaces it; treat as user-cancel.
- User presses BACK: stem export cancels via `exitUIMode(UI_MODE_STEM_EXPORT)`; we detect and skip the swap phase. Orphaned WAVs on SD are harmless — the STEMS folder can be cleaned manually.

### Partial-failure transactionality (whole-track)

All-or-nothing: buffer all `(sourceClip, wavPath)` pairs and only start the swap phase after every WAV is on disk. On any render failure, abort before any swap — song is unchanged.

### State to restore on abort

- `stemExport.includeSongFX`, `renderOffline`, `allowNormalization`: save prior values, restore on every exit path.
- All clips' `activeIfNoSolo` flags: already handled by `restoreAllClipMutes`.
- Source output's reverb send: not mutated during render (reverb is skipped via `includeSongFX=false`, not by zeroing the send), so nothing to restore.

## Testing

Hardware/manual (primary — audio DSP, level/timing issues not catchable in unit tests):

- Single-clip synth bounce, no FX: A/B against muted source — indistinguishable.
- Single-clip synth bounce, heavy FX + high reverb send: A/B — match including reverb tail (live reverb via preserved send).
- Whole-track synth bounce, 3 clips at different sections: verify all clips appear at correct sections on new AudioOutput, source SoundInstrument deleted.
- Gain-staging stress: aggressive compressor + high makeup gain + high volume knob, compare peak meters — levels match within noise floor.
- Release-tail: long release, bounce — WAV longer than loop length, loop marker at loop end, tail plays only on first iteration/song-stop.
- Cancel mid-bounce (track scope): press BACK partway — song untouched.
- Empty clip attempt: rejected with popup.
- MIDI/CV attempt: menu hidden or rejected with popup.
- Kit clip bounce, all drums firing with kit-level reverb: verify all drums present in WAV, live reverb still wet.
- Kit whole-track bounce: each kit clip → own bounced AudioClip on new AudioOutput.
- AudioClip source bounce with heavy clip-level FX (filter + delay): verify new AudioClip is flat, plays identically.

Regression:

- Run a normal stem export after a bounce: verify we restored all stemExport flags to their prior values.
- Song save/load round-trip after bounce: rendered WAV paths survive; new AudioOutput and AudioClips serialize correctly.

Automated:

- Hardware-dependent bits unsuitable for `tests/32bit_unit_tests/`.
- The orchestration logic (state save/restore, all-or-nothing transaction) could get a small unit test with a mocked StemExport; not worth the scaffolding for POC.

## Open questions to resolve during implementation

1. Verify `AudioOutput` compressor default is truly transparent. If not, explicitly bypass on the new output.
2. Verify `Song::createNewAudioOutput(replaceOutput = sourceInstrument)` correctly handles the transfer of output-list position for all three source types (synth, kit, audio output). Likely yes since it's already used for synth→audio in the classic Convert to Audio path.
3. Confirm `swapClips` correctly retargets `clip->output` across type-changing swaps (instrument clip on SoundInstrument → audio clip on AudioOutput). Again likely yes given existing usage.

## POC limitations (documented for follow-up)

- No normalization option — always `allowNormalization=false`.
- No "bake reverb tail" option — reverb always stays live via preserved send.
- No way to keep source track (non-destructive mode) — always destructive replace.
- No per-type configurability (e.g., different defaults for audio-clip source where one might want different behavior).
- Progress popup wording is stem-export's, not bounce-specific ("Exporting stem" rather than "Bouncing").

These are deliberate deferrals — each can be added without restructuring the POC.
