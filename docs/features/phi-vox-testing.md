# PHI_VOX Oscillator Testing Guide

## TL;DR

PHI_VOX is a VOSIM voice oscillator (Kaegi & Tempelaars, JAES 1978 — see
[docs/dev/phi-vox-vosim.md](../dev/phi-vox-vosim.md)): formant pulse bursts that read as
vowels, with zones anchored on vocal characters and phi-triangle wander pushing far past
physiology. Crossfading A→B glides the formants (a diphthong) and knob MOTION injects
consonant bursts — it speaks when you play the wave index.

**Quick Start:**
1. Synth → OSC1 Type → **PhiVox** (after PhiWeave; 7-seg shows `VOX`)
2. Zone A/B knobs now show voice characters: Breath, Hum, Round, Open, Bright, Nasal, Growl, Rasp
3. Hold a note ~2 octaves below middle C (formant synthesis loves low fundamentals)
4. Sweep **Wave Position** slowly: vowel glide; snap it: consonant articulation
5. Push+twist Wave Position for gamma; push+twist Zone knobs for per-zone phase offset

## Step-by-Step Walkthrough

### 1. Vowel identity
Hold a low note in Open (zone 3, /a/-region), then transpose up an octave: it should still
read as the *same voice*, higher — formants are fixed in Hz. Compare against a wavetable,
which shifts its whole spectrum with pitch.

### 2. The zone map
| Zone | Name | Anchor character |
|------|------|-----------------|
| 0 | Breath | /i/-ish, breathy, airy noise in the pulses |
| 1 | Hum | closed /u/, dark |
| 2 | Round | /o/ roundness |
| 3 | Open | /a/, the classic open vowel |
| 4 | Bright | /e/, forward |
| 5 | Nasal | pinched, /æ/-nasal |
| 6 | Growl | sub-vocal cluster, low F1+F2 |
| 7 | Rasp | harsh spread, F2 high |

Positions within a zone wander widely (deliberately beyond vocal realism). Report
coordinates (P:G display) that sound broken rather than merely strange.

### 3. Articulation (the party trick)
1. Zone A = Hum, Zone B = Open, hold a note at wave position 0
2. Slow sweep → "oooo-aaaa" glide, clean
3. Quick snap → a consonant-like burst articulates the transition
4. LFO on wave index → babble; square LFO → hard syllables
5. Park the knob: the burst decays in well under a second

### 4. Character extremes
- Find positions where pulses GROW across the burst (decay > 1): edgy rasp, almost brass
- Alternating-polarity positions: hollow, clarinet-adjacent
- Growl zone at very low fundamentals: sub-vocal texture
- High fundamentals (above ~500 Hz): the burst truncates naturally — thinner, chirpy;
  physically authentic behavior, not a bug

### 5. Regression checks
- Preset round-trip: save with non-default zones/gamma, reload, verify
- Ring-mod with PHI_VOX as either oscillator
- Pulse width: gates the cycle tail (extends the silent gap — tightens the voice)
- Osc sync and retrigger phase: PHI_VOX is stateless, both should behave perfectly
- PHI_MORPH / PHI_WEAVE fields unaffected when switching between the three
- FX benchmark: `phi_vox`/`render`, expected between PHI_MORPH (~1.0k) and wavetable (~2.2k)

## Known Characteristics (not bugs)
- Unipolar-pulse DC is compensated per buffer; a tiny residual step can occur at extreme
  pitch-bend rates (compensation updates per buffer)
- Very high notes lose vowel identity (burst shorter than one pulse) — sopranos too
- Breath noise lives INSIDE the pulses (voiced-gated); gaps are silent by design

## Tuning Status
The phi-bank constants and vowel anchors in `dsp/phi_vox.hpp/.cpp` are v1, not ear-tuned.
Likeliest tweaks: breath range, articulation gain/decay, F2 wander width, balance range.
