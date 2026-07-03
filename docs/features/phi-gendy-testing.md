# PHI_GENDY Oscillator Testing Guide

## TL;DR

PHI_GENDY is Xenakis dynamic stochastic synthesis (GENDYN) with locked pitch — see
[docs/dev/phi-gendy-stochastic.md](../dev/phi-gendy-stochastic.md). A 16-breakpoint
polygon whose points random-walk between elastic barriers, once per buffer. Zones run an
exponential entropy arc from near-static warmth to reflection-driven grit. Rich, saw-like
spectra — this is the aggressive sibling.

**Quick Start:**
1. Synth → OSC1 Type → **PhiGendy** (7-seg `GNDY`)
2. Zones: Haze, Murmur, Wander, Ripple, Boil, Writhe, Snarl, Frenzy
3. Hold a bass note in Haze: a warm wave with barely-alive drift
4. Walk the zones upward and hear the polygon come to a boil
5. Wiggle the wave index: the walkers STARTLE, then settle back home

## Walkthrough

### 1. The entropy arc
- Haze/Murmur: near-static timbre with organic drift — compare against a parked
  PHI_MORPH to hear the aliveness
- Wander/Ripple: chorus-like motion without any chorus effect
- Boil/Writhe: the classic GENDY writhing snarl — timbre in constant negotiation
- Snarl/Frenzy: barrier reflections dominate — pitched grit, broken-machine textures
- Pitch must stay STABLE at all entropy levels (cycle length is locked — this is the
  key deviation from Xenakis; report any perceived pitch wander)

### 2. Spatial terrain
Within one zone, sweep positions: landscapes reassign which polygon regions are calm
vs boiling and how the cage is shaped. Asymmetric-cage positions sound skewed/reedy.

### 3. Startle (the morph gesture)
1. Park in Haze or Murmur (strong home pull)
2. Flick the wave index a small amount: a burst of chaos, then audible settling as the
   home spring reels the walkers in (~0.5s)
3. Note-on also startles — rapid retriggers should sound subtly "roused"
4. LFO on wave index: continuous agitation scaled by rate/depth

### 4. Morph behavior
A/B crossfade lerps walk LAWS (cage, step, home), never the polygon itself — sweeps must
be click-free even between extreme zones. The state carries through the morph.

### 5. Shared-state behavior (like PHI_WEAVE)
All voices/unison of a Sound scan the SAME polygon — chords are coherent (every note
writhes together). Two Sounds get independent walks.

### 6. Regression checks
- Preset round-trip (zones, phase offsets, gamma)
- Ring-mod with PHI_GENDY as either oscillator
- Pulse width, osc sync/retrigger phase, unison stereo spread
- Siblings unaffected (five-way zone-menu dispatch)
- FX benchmark: `phi_gendy`/`render` — expect PHI_MORPH-class (~1k cycles/buffer)

## Known Characteristics (not bugs)
- Breakpoints move mid-cycle (the walk ticks per buffer, not per waveform cycle);
  at high entropy this adds edge-grit — it IS the sound. At low entropy it's inaudible
- DC is removed by mean subtraction each tick; extreme asymmetric cages may still lean
  on transient content
- High notes traverse the polygon fast: entropy reads more as noisy sheen than motion

## Tuning Status
Anchors/banks v1, not ear-tuned. Likeliest tweaks: entropy range endpoints, home pull
strength, startle gain, barrier-width range.
