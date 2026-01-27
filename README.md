# Deluge Owlet Firmware

This is **owlet-firmware**, a personal fork of the Deluge Community Firmware maintained at [owlet-labs/DelugeFirmware](https://github.com/owlet-labs/DelugeFirmware). It serves as a playground for experimental sound design features that may be too specialized or CPU-intensive for the main community branch.

### NOTICE
this repo is in the process of migrating from community to 1.3 base for stability. Current builds are on comunity until the rest of tthe features are migrated. In testing, 1.3 base has been stable.

### Features (vs Community)

| Feature | Description |
|---------|-------------|
| **Scatter (Bird Brain)** | Beat-repeat and slice manipulation effect. Double-buffer system for glitch-free triggering. Rate knob controls slice length. Future modes: beat reordering, fractional positions, random patterns. |
| **Featherverb** | Lightweight 4-tap FDN reverb. 77 KB buffer vs Mutable's 128 KB (40% smaller). ~19k cycles under heavy load (DX7), ~10k with simple synths. Three zone parameters for matrix/size/decay character. |
| **Multiband OTT Compressor (OWLTT/DOTT)** | 3-band upward/downward compressor with "Feel" modulation system. 8 vibe zones for dynamic character control and creative crossover options. Realistically its the worlds best creative multiband compressor. Also can serve utillity duty and cleans up nice when not acting nuts. Can funciton as full downward or full upward compression or anywhere in between. Aggressively optimized and surprisingly light on cpu for what it does. Minimal metering also included so you know when and what is clipping and when up or downn compression is being applied.|
| **Sine Shaper (HOOT)** | 4-knob Harmonic waveshaping with width, evolution, recursion, and feedback zones. 8 Different algorithms with complex routing capabilities. |
| **Disperser (OWLPASS)** | 4-knob Allpass filter cascade for frequency-dependent phase smearing. 8 algorithms and various Twist zones with punch, curve, chirp, and Q-tilt. Classic and exotic phase dispersion sounds with some Karplus Strong capabilitites in certain configurations. Unlike any dispersion you have ever heard. 8 stages max in lo-cpu mode. up to 32 in cpu heavy "shoot myself in the foot" mode (sounds amazing). (benchmarks at 8 stages, scales linearly with stages. very cheap at 1,2 stages still sounds disperserey) In a few configurations, KS feedback can get out of control so its "fun" to tune.|
| **Table Shaper (PELLET)** | 4-knob Wavetable-based waveshaping but its the best one ever with pre slew and post hysteresis along with other creative modifiers. Tables are parametrically designed from basis functions including various saturations clippers and wavefolders. |
| **Retrospective Sampler** | Lookback buffer for capturing audio after the fact. Sources: Input, Master mix, or Focused Track (captures whichever clip is selected). Duration modes: time-based (5/15/30/60 sec) or bar-synced (1/2/4 bars). Bar modes wait for next downbeat to save and tag filenames with BPM (e.g., `RETR0000_4BAR_120BPM.WAV`). Very light on CPU, configurable memory footprint. |
| **Zone Menu Items** | Horizontal menu UI component for zone-based parameters with visual feedback. |
| **FX Benchmarking Framework** | Performance profiling tools for DSP development. |
| **Pulse Width Triangle** | Triangle oscillator with pulse width control. Creates compressed triangle cycles with dead zones—at 0% you get a full triangle, at 100% narrow spikes. Useful for timbral variety and rhythmic pulsing effects. |

### Random Fixes

| Fix | Description |
|-----|-------------|
| **SIMD oscillator remainder handling** | Fixed tempo-synced clicking on sine and analog waveforms. The SIMD-vectorized `renderWave` only processed samples in groups of 4, leaving remainder samples unwritten when buffer sizes weren't multiples of 4 (common due to tempo tick alignment). Added scalar fallback loops for remainder samples. |

### Changelog

**Latest**
- Reverb memory optimization: all models now use dynamic allocation, freeing ~220 KB when switching models
- Added Featherverb reverb with zone-based parameter design
- Table Shaper and Sine Shaper now available for audio clips via DOTT menu
- Bar-synced retrospective sampler with BPM-tagged filenames
- Disperser delay buffers now allocate on-demand (saves up to 2.2 MB in complex songs)



### For Developers

This fork is open source under the same GPL-3.0 license as the community firmware. Feel free to:
- Fork this repo for your own experiments
- Cherry-pick individual features into your own builds
- Submit PRs to the main community repo if you refine something worth sharing

No guarantees of stability or compatibility. Use at your own risk.

---

## Dear Deluge owners
If you want to start using the community firmware please visit https://synthstromaudible.github.io/DelugeFirmware/ for all relevant details.

## About
The [Deluge](https://synthstrom.com/product/deluge/) from [Synthstrom Audible](https://synthstrom.com/) is a portable sequencer, synthesizer and sampler. Synthstrom Audible decided to [open source](https://synthstrom.com/open/) the firmware application that runs on the Deluge to allow the community to customize and extend the capabilities of the device. 

The hardware is built around a Renesas RZ/A1L processor with an Arm® Cortex®-A9 core running at 400MHz and 3MB of on-chip SRAM. Connected to that is a 64MB SDRAM chip, a PIC for button handling and either a 7-Segment array or an OLED screen. The application is written in C and C++ and some occasional assembly in bare metal style without an operating system.

## Important links
* For acquiring and using the firmware visit the [community firmware website](https://synthstromaudible.github.io/DelugeFirmware)
* Once you have the firmware, consult our detailed [update guide](https://github.com/SynthstromAudible/DelugeFirmware/wiki/Update-guide) for instructions on how to install it
* To contribute to the project with code, bug reports, suggestions, and feedback see: [How to contribute to Deluge Firmware](https://github.com/SynthstromAudible/DelugeFirmware/blob/community/docs/CONTRIBUTING.md)
* To learn about the toolchain, application, and to start working on the code continue with: [Getting Started with Deluge Development](https://delugecommunity.com/development/deluge/getting_started/)
* Please also visit the [Synthstrom Forum](https://forums.synthstrom.com/) and the community [Discord](https://discord.gg/BnRcyFSgaT) to interact with other users and the developers
* You can donate to the project using the [Synthstrom Patreon](https://www.patreon.com/Synthstrom)
