# PHI_STAIR Oscillator Testing Guide

## TL;DR

PHI_STAIR is a staircase wavetable oscillator (stepped-waveform lineage: Walsh-function
synthesis, Chamberlin 1980 — see [docs/dev/phi-stair-steps.md](../dev/phi-stair-steps.md)):
up to 16 steps with zone-pattern heights, asymmetric widths, and a slope dimension from
hard-edged (bright) to ramped (mellow). The family's digital/chip/organ voice.

**Quick Start:**
1. Synth → OSC1 Type → **PhiStair** (between PhiMorph and PhiWeave; 7-seg `STAI`)
2. Zones: Brick, Terrace, Ramp, Mesa, Pylon, Glyph, Shard, Teeth
3. Glyph and Teeth are the character showcases; Brick→Glyph wave morphs are very digital
4. The Stereo zone knob works out of the box (dual taps on the staircase)

## Walkthrough

### 1. Pattern families
Sweep the zone knob through all eight families - each 128-wide region is a different
step-height architecture. Within a zone, positions vary count (2..16), tilt, width
asymmetry, and slope continuously - no pops at any position (fractional step count).

### 2. Slope
Positions differ in riser slope: hard-edged positions are pulse-bright, high-slope
positions approach polygon mellowness. Ramp zone is deliberately smooth-biased.

### 3. Morphs
A/B zone + wave-index morphs crossfade the whole staircase architecture (heights,
widths, slope, count) with per-rebuild loudness normalization - levels should stay
even through any morph. Wave modulation (LFO/env) should be click-free (one-buffer
smoothstep de-zipper per rebuild).

### 4. Regression checks
- Preset round-trip (zones, phase offsets, gamma; stereo zone)
- Ring-mod with PHI_STAIR as either oscillator
- Pulse width, retrigger phase, unison (zero per-voice state - should be trivially clean)
- High notes: anti-alias mips engage ~700/1800 Hz (brightness steps down; report fizz)
- FX benchmark: `phi_stair`/`render` - expected among the cheapest (single table scan)

## Known Characteristics (not bugs)
- Loudness is normalized per morph position (static RMS targeting): extreme sparse
  patterns are lifted to saw-class energy by design
- Mip switches on big pitch bends step the brightness (same as WEAVE)

## Tuning Status
Banks v1: pattern shapes, slope mapping (squared), tilt/asymmetry ranges are the
likeliest ear-tune targets.
