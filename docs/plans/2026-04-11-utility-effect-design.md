# Utility Effect Design

## Summary

A channel-strip utility effect with volume, stereo pan (L/R), and stereo width controls. Placed right before DOTT in the signal chain for both Sound and GlobalEffectable paths. No patched/unpatched params — direct knob control only.

## Parameters

| Knob | Range | Behavior |
|------|-------|----------|
| Volume | -inf to +12dB | Gain stage into DOTT |
| Pan L | Left channel pan | |
| Pan R | Right channel pan | |
| Stereo Width | 0–200% | Mid/side balance |

## Stereo Width

Mid/side processing:
- mid = (L + R) / 2
- side = (L - R) / 2
- output L = mid + side * width
- output R = mid - side * width

Where width: 0% = mono (mid only), 100% = pass-through, 200% = double side signal.

## Signal Chain Placement

### Sound (sound.cpp)
`...Eroder -> ModFX -> EQ -> Stutter -> **Utility** -> DOTT -> Reverb Send -> Compressor`

### GlobalEffectable (global_effectable_for_clip.cpp)
`...Eroder -> ModFX -> Stutter -> **Utility** -> DOTT -> Reverb Send -> Compressor`

## Implementation Approach

- Add `UTILITY` to an appropriate effect type enum or use standalone member
- Create lightweight DSP processor class in `src/deluge/dsp/utility/`
- Add as member variable to `ModControllableAudio`
- Insert processing call before DOTT in both Sound and GlobalEffectable render paths
- Store parameter values directly on the processor (no param system integration)
