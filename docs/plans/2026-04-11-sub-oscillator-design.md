# Sub Oscillator Effect Design

## Summary

A post-FX mono sine oscillator for clean harmonic reinforcement. Tracks pitch via `lastNoteCode` using harmonic ratios. Has its own standalone AR envelope triggered by voice activity, note-tracking HPF, portamento, and zero-crossing release logic to prevent clicks. Level and Fine are patched params (mod matrix targets). Placed post-reverb in the signal chain (after DOTT and reverb send, before per-clip compressor).

## Parameters (8 knobs, 2 menu pages)

| # | Knob | Type | Range | Purpose |
|---|------|------|-------|---------|
| 1 | Harmonic | direct | 1/4, 1/3, 1/2, 1, 2, 3, 4, 5, 6, 7, 8 | Harmonic ratio selection |
| 2 | Fine | patched | -12 to +12 semitones | Pitch offset, snaps to semitones. Encoder press unlocks fine continuous tuning |
| 3 | Level | patched | 0 to max | Mix amount |
| 4 | Phase/Spread | direct | 0-127 | Lower half: start phase 0-360. Upper half: stereo L/R phase spread 0-180 |
| 5 | Attack | direct | time | AR envelope attack |
| 6 | Release | direct | time | AR envelope release |
| 7 | HPF | direct | amount | Note-tracking high-pass cutoff |
| 8 | Porta | direct | time | Pitch glide time |

## Signal Chain Placement (split around reverb)

```
...DOTT -> ModFX(post-DOTT) -> harm.renderHpf() -> processReverbSendAndVolume() -> harm.renderOsc() -> compressor
```

- HPF runs pre-reverb: strips fundamental so reverb only processes harmonics (clean tails)
- Sub oscillator runs post-reverb: adds clean fundamental back dry
- HPF and oscillator operate independently (HPF has its own on/off)

Available on Sound (synth tracks and kit rows) only — not GlobalEffectable (needs voice/note information).

## Oscillator

- Mono sine using `getSine()` from `util/waves.h` (lookup table, cheap)
- Pitch from `lastNoteCode` * harmonic ratio, with fine tune cents offset
- Phase accumulator: `uint32_t`, incremented per sample by frequency-derived value
- Two phase accumulators when stereo spread is active (one per channel, same increment, different start offsets) — two sine lookups per sample in spread mode

### Harmonic Ratios

Sub-harmonics: 1/4 (-2 oct), 1/3 (-oct-fifth), 1/2 (-1 oct)
Fundamental: 1
Harmonics: 2 (oct), 3 (fifth+oct), 4 (2 oct), 5 (maj3+2oct), 6 (fifth+2oct), 7 (min7+2oct), 8 (3 oct)

These are true frequency ratios, not equal temperament semitones.

## Phase/Spread Knob (dual-function)

Single 0-127 knob with smooth transition:
- **0-64**: start phase 0-360 degrees, mono output (L=R)
- **65-127**: stereo spread 0-180 degrees (L/R phase offset), phase fixed at 0 degrees

At spread=0 (knob=65), output is mono at 0 degrees — smooth transition from the phase region.

## Envelope

Standalone mono AR envelope (not the per-voice Envelope class):
- Simple ramp: float or q31_t state variable
- Attack: ramp from 0 toward 1.0 at attack rate
- Sustain: holds at 1.0 while any voice is active
- Release: ramp from current toward 0.0 at release rate
- Trigger: `!voices_.empty()` transition false->true = attack, true->false = release

### Zero-Crossing Release

To prevent clicks when the envelope shuts off:
```
if (envelope < threshold && phase crossed zero):
    envelope = 0
    stop rendering
```
Phase zero crossings are at phase=0 and phase=0x80000000. One comparison per sample during release tail only.

## Portamento

Smoothly glide the current frequency toward the target frequency when `lastNoteCode` changes. Simple exponential approach:
```
currentFreq += (targetFreq - currentFreq) * portaRate
```

## Note-Tracking HPF

Single-pole or biquad high-pass filter whose cutoff tracks the oscillator's actual output frequency. Removes DC and sub-fundamental rumble. Cutoff = some multiple of the oscillator frequency, controlled by the HPF knob amount.

## Patched Params (Level + Fine)

Level and Fine are patched params, meaning they can be modulation targets in the mod matrix. This enables:
- LFO1 -> Level = tremolo
- LFO1 -> Fine = vibrato
- Envelope 1 -> Level = amplitude shaping
- Velocity -> Level = dynamic response

These need entries in the patched param enums, ParamManager integration, and proper menu items (patched_param::Integer style rather than direct-access Integer).

## Reusable Components

| Component | Source | Usage |
|-----------|--------|-------|
| Sine lookup | `util/waves.h` getSine() | Waveform generation |
| Phase accumulator pattern | `modulation/lfo.h` LFO class | Reference for phase inc math |
| Pitch conversion | existing noteCode->frequency utils | lastNoteCode to phase increment |
| globalSourceValues | Sound class | LFO1 and other global mod sources for patched params |

## Serialization

Direct params: use WRITE_FIELD/READ_FIELD macros (like utility effect).
Patched params (Level, Fine): go through ParamManager, serialized automatically.
XML tags: `subOscHarmonic`, `subOscPhase`, `subOscAttack`, `subOscRelease`, `subOscHpf`, `subOscPorta`.
