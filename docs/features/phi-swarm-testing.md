# PHI_SWARM Oscillator Testing Guide

## TL;DR

PHI_SWARM is an injection-locking oscillator (Adler 1946 / Kuramoto 1975 — see
[docs/dev/phi-swarm-sync.md](../dev/phi-swarm-sync.md)): a master phase at the note
pitch drives two coupled slave oscillators. Zones move the system between locked
consonance, injection-pulling "sirens", quasiperiodic shimmer, and chaos. Temperature
(phase noise) runs from crystalline to melted. **Sweeping the wave index heats the
network; parking it lets you hear it anneal back into lock.**

**Quick Start:**
1. Synth → OSC1 Type → **PhiSwarm** (7-seg `SWRM`)
2. Zones: Still, Drift, Pull, Swarm, Flock, Surge, Fray, Chaos
3. Hold a note in Still: pure locked interval. Move to Drift: slow beating appears
4. Zone A = Still, Zone B = Chaos: sweep the wave index and listen for the melt;
   snap it back and listen for the re-crystallization

## Walkthrough

### 1. Locking (Still / Drift / Pull)
- Still: should sound like a stable organ-ish interval (unison + octave partials)
- Drift: very slow beating — the slaves are just outside lock with weak coupling
- Pull: the signature sound — a beat that slows, hesitates, then snaps around; sweep
  within the zone (1024 positions) to find edge-of-lock positions
- Transpose across the keyboard: the locking CHARACTER should be pitch-invariant
  (couplings scale with pitch)

### 2. Quasiperiodic (Swarm / Flock)
- Swarm: phi-ratio slaves — inharmonic shimmer that never repeats
- Flock: the slaves chase each other more than the master — internal secondary locks

### 3. Entropy (Surge / Fray / Chaos)
- Surge/Fray: increasing temperature — analog drift into breathy instability
- Chaos: coupling overshoot + high temperature — pitched noise, growl
- Expect Chaos zones to be LOUD in character but bounded in level (sine-based)

### 4. Annealing (the party trick)
1. A = Still, B = Fray or Chaos; hold a note, wave position parked at A
2. Slow sweep toward B: progressive de-sync (heat + changing landscape)
3. Park at B, then snap back to A: a burst of heat, then — listen — the slaves cool
   and re-lock over a few hundred ms. That settling IS the instrument
4. LFO on wave index: periodic heat = breathing, boiling textures

### 5. Regression checks
- Preset round-trip (zones, phase offsets, gamma)
- Ring-mod with PHI_SWARM as either oscillator
- TRIANGLE_PW unaffected (PHI_SWARM borrows its per-voice field — switch between the
  two types and verify TRPW still sounds correct; type switches kill voices)
- Unison: each unison voice has independent slave phases — expect thick ensembles
- FX benchmark: `phi_swarm`/`render`; estimate ~18-22 cycles/sample (wavetable-class)

## Known Characteristics (not bugs)
- Sine-based: spectrally sparse by design; richness is temporal (locking dynamics) and
  the ring cross-term. It will not replace a saw
- First moments of a note may include a settling gesture (slave phases start free) —
  each note anneals in; this is intended
- Locked zones are nearly static by design — that's what a lock is

## Tuning Status
Anchors and wander banks are v1, not ear-tuned. Likeliest tweaks: coupling anchor
levels (lock vs pull balance per zone), temperature scaling, anneal gain/cooling rate.
