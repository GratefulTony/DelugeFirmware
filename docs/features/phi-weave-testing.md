# PHI_WEAVE Oscillator Testing Guide

## TL;DR

PHI_WEAVE is a scanned-physics oscillator: a ring of 32 masses on springs, simulated slowly
and scanned as a wavetable (Mathews/Verplank scanned synthesis — see
[docs/dev/phi-weave-physics.md](../dev/phi-weave-physics.md) for the physics and original
literature). Zone knobs select the *physics* — stiffness/damping terrain, rest shape,
bowing — not a waveform. One string is shared per Sound; the sound has inertia and
remembers what you just played.

**Quick Start:**
1. Load a synth, set OSC1 Type → **PhiWeave** (after PhiMorph; 7-seg shows `WEAV`)
2. Open **Zone A**, sweep through the zones: Silk, Wool, Reed, Vine, Bone, Glass, Steel, Storm
3. Set **Zone B** somewhere contrasting
4. Hold a note and slowly turn **Wave Position** — the morph itself bows the string
5. Push+twist Wave Position for gamma (the shared secret-knob phase multiplier);
   push+twist a Zone knob for its per-zone phase offset

**Suggested Test Patch:**
- Plain init synth, ~440 Hz, medium-long amp envelope release
- No FX at first — the oscillator's own motion is the point
- Later: add reverb and hold chords for the full bowed-glass experience

## Step-by-Step Walkthrough

### 1. A held note settles

Hold one note in any zone. Expected: the note-on pluck rings through the physics
(traveling ripples), decays over ~a second, and settles into that zone's baseline tone
with a zone-characteristic restlessness from the continuous bow. Nothing should click,
buzz digitally, or blow up — the failure mode of extreme zones is *saturation*, which
should sound like an overdriven string, not glitches.

### 2. The zones

| Zone | Name | Intended direction of travel* |
|------|------|-------------------------------|
| 0 | Silk | soft, slack, slow ripples |
| 1 | Wool | muffled, damped, breathy |
| 2 | Reed | nasal midrange, moderate coupling |
| 3 | Vine | uneven terrain, organic movement |
| 4 | Bone | harder, faster wave travel |
| 5 | Glass | bright, ringing, low damping |
| 6 | Steel | taut, dispersive, metallic |
| 7 | Storm | agitated, heavy bow, near-chaotic |

*Approximate — every one of the 1024 positions inside a zone differs (φ-triangle
quasi-periodicity), and the boundary names are vibes, not guarantees. Report positions
that sound broken (silent, stuck, harsh in a bad way) with their coordinate (P:G display).

### 3. The morph-bow (the party trick)

1. Zone A = something calm (Silk/Wool region), Zone B = something hard (Steel/Storm)
2. Hold a note, wave position at 0 — let it settle
3. **Slow sweep** toward B: the transition should *audibly sing* — energy enters as you move
4. **Stop moving**: the sound settles into the interpolated physics within ~a second
5. **Snap** the knob quickly: a hard bow — a struck transient, then ring-down
6. Map an LFO to Wave Position: it should sound like rhythmic bowing, tempo-synced if synced

### 4. The shared string

1. Play a fast run of notes: each note-on plucks the same string — later notes should
   sound subtly different depending on what's still ringing (this is intended!)
2. Play a chord: all voices scan the same ring at different speeds — expect coherent,
   organ-like fusion rather than independent voices
3. Unison + detune: scan phases spread; expect thickening without phasing artifacts

### 5. Regression checks

- **Preset round-trip**: save a patch with non-default zones/gamma, reload, confirm
  zones + P:G coordinates and the sound come back
- **Ring-mod mode**: PHI_WEAVE as either ring-mod oscillator should work (it's the last
  ring-moddable type)
- **Pulse width param**: nonzero PW gates the tail of the cycle to silence (parity with
  PHI_MORPH's deadzone behavior)
- **PHI_MORPH unaffected**: switch a PhiMorph patch back and forth; its zones/gamma are
  separate fields and must persist independently
- **CPU**: run the FX benchmark (`phi_weave`/`render`); expected at or below PHI_MORPH's
  ~1,048 cycles/buffer. Watch voice counts with big unison stacks

## Known Characteristics (not bugs)

- The first note after switching to PHI_WEAVE (or after a zone change) starts from
  whatever state the string was in — by design; the string is a physical object
- Zones with heavy damping + soft home shapes are *quiet by nature*; the AGC compensates
  slowly (~1.5 s), so an abrupt zone change can take a moment to reach full loudness
- Very high bow-rate zones have audible periodic wobble — that's the bow, not an LFO leak

## Tuning Status

The ~15 φ-bank constants in `dsp/phi_weave.hpp` are **v1, not yet ear-tuned**. Likeliest
candidates for adjustment after listening: bow depth range, AGC release time, stiffness
range extremes. Report findings against specific zone coordinates.
