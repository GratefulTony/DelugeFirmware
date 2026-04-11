# Harm Effect Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a post-FX "Harm" effect: note-tracking HPF (cleans muddy fundamental from FX'd signal) + mono sine sub oscillator (adds clean harmonic back). HPF and oscillator operate independently.

**Architecture:** Inline params struct on ModControllableAudio with standalone AR envelope, phase accumulator, and cascaded single-pole HPF. Two patched params (Level, Fine) for mod matrix; six direct params (Harmonic, Phase/Spread, Attack, Release, HPF, Porta). Split render: HPF runs pre-reverb (strips fundamental before reverb), sub oscillator mixes in post-reverb (adds clean harmonic back dry). Sound-only (needs voice/note info).

**Tech Stack:** C++, Q31 fixed-point, getSine() from waves.h, noteFrequencyTable[] for pitch, patched param system for Level+Fine.

---

### Task 1: Add patched param enum entries

**Files:**
- Modify: `src/deluge/modulation/params/param.h` (~line 149)

**Step 1: Add GLOBAL_HARM_LEVEL as a volume param**

In `param.h`, in the Global enum, add after `GLOBAL_MOD_FX_DEPTH` (line 149) and before `FIRST_GLOBAL_NON_VOLUME`:

```cpp
GLOBAL_HARM_LEVEL,  // Harm sub oscillator level (volume category)
```

**Step 2: Add GLOBAL_HARM_FINE as a hybrid param**

After `GLOBAL_MACRO_4` (line 160) and before `FIRST_GLOBAL_ZONE`:

```cpp
GLOBAL_HARM_FINE,  // Harm sub oscillator fine tune (hybrid: base +/- modulation)
```

Fine is hybrid because it's bipolar (base semitones +/- modulation offset). Range: -12 to +12 semitones.

**Step 3: Add serialization string names**

In `param.cpp`, in `paramNameForFileConst()` (~line 770), add cases before the `GLOBAL_NONE` case:

```cpp
case GLOBAL_HARM_LEVEL:
    return "globalHarmLevel";

case GLOBAL_HARM_FINE:
    return "globalHarmFine";
```

**Step 4: Add display names**

In `param.cpp`, in the `getPatchedParamDisplayName()` NAMES array (~line 297), add before the closing `};`:

```cpp
[GLOBAL_HARM_LEVEL] = STRING_FOR_HARM_LEVEL,
[GLOBAL_HARM_FINE] = STRING_FOR_HARM_FINE,
```

(These STRING_FOR_ constants will be added in Task 3.)

**Step 5: Verify PLACEHOLDER_RANGE static_assert still passes**

The static_assert at line 197 checks `PLACEHOLDER_RANGE (89) > GLOBAL_NONE`. Adding 2 params increases GLOBAL_NONE by 2. Verify the assert still holds — GLOBAL_NONE should still be well below 89 since FIRST_GLOBAL starts at LOCAL_LAST which is around 136. Wait — PLACEHOLDER_RANGE is 89 and must be > GLOBAL_NONE. But GLOBAL_NONE is ~189. So PLACEHOLDER_RANGE (89) < GLOBAL_NONE (~189). Re-read the assert: it says `PLACEHOLDER_RANGE > GLOBAL_NONE` — **this can't be right if GLOBAL_NONE > 89**. Check the actual enum values. LOCAL_LAST is likely much smaller. Read the Local enum carefully to determine actual values.

**Important:** Read the full param.h to understand the actual enum numbering before making changes. The implementer MUST verify enum values don't break static_asserts.

**Step 6: Build and verify**

Run: `timeout 300 ./dbt build`

**Step 7: Commit**

```bash
git add src/deluge/modulation/params/param.h src/deluge/modulation/params/param.cpp
git commit -m "feat: add GLOBAL_HARM_LEVEL and GLOBAL_HARM_FINE patched params"
```

---

### Task 2: Create DSP params struct and processing functions

**Files:**
- Create: `src/deluge/dsp/harm.h`

**Step 1: Read reference files**

Read these files to understand patterns:
- `src/deluge/dsp/eroder.h` — copyright header, serialization pattern
- `src/deluge/dsp/utility.h` — simple effect struct pattern (just created)
- `src/deluge/util/waves.h` — `getSine(uint32_t phase)` function
- `src/deluge/util/lookuptables/lookuptables.h` — `noteFrequencyTable[12]`
- `src/deluge/dsp/stereo_sample.h` — StereoSample struct
- `src/deluge/util/fixedpoint.h` — `multiply_32x32_rshift32`, `ONE_Q31`

**Step 2: Create harm.h**

Use Owlet Records copyright header. Namespace: `deluge::dsp`.

```cpp
struct HarmParams {
    // Direct params (stored on struct, serialized via WRITE_FIELD/READ_FIELD)
    uint8_t harmonic{0};    // Index into ratio table: 0=off, 1=1/4, 2=1/3, 3=1/2, 4=1, 5=2, ... 11=8
    uint8_t phase{0};       // 0-64: start phase 0-360 mono. 65-127: stereo spread 0-180
    uint8_t attack{0};      // AR envelope attack time (0=instant)
    uint8_t release{64};    // AR envelope release time
    uint8_t hpf{0};         // Note-tracking HPF amount (0=off/bypass)
    uint8_t porta{0};       // Portamento time (0=instant)

    // DSP state (not serialized)
    uint32_t phaseAccumL{0};
    uint32_t phaseAccumR{0};
    float envelope{0.0f};       // AR envelope state, 0.0-1.0
    float currentFreq{0.0f};    // Portamento smoothed frequency
    bool voicesWereActive{false}; // Previous frame voice state for trigger detection
    int32_t hpfStateL{0};       // Single-pole HPF state left
    int32_t hpfStateR{0};       // Single-pole HPF state right

    // Harmonic ratio table (frequency multipliers)
    static constexpr float kHarmonicRatios[] = {
        0.0f,       // 0: off
        0.25f,      // 1: 1/4 (-2 octaves)
        1.0f/3.0f,  // 2: 1/3 (-octave-fifth)
        0.5f,       // 3: 1/2 (-1 octave)
        1.0f,       // 4: 1 (fundamental)
        2.0f,       // 5: 2 (octave)
        3.0f,       // 6: 3 (fifth+octave)
        4.0f,       // 7: 4 (2 octaves)
        5.0f,       // 8: 5 (maj3+2oct)
        6.0f,       // 9: 6 (fifth+2oct)
        7.0f,       // 10: 7 (min7+2oct)
        8.0f,       // 11: 8 (3 octaves)
    };
    static constexpr int32_t kNumHarmonics = 12;

    [[nodiscard]] bool isOscEnabled() const { return harmonic > 0; }
    [[nodiscard]] bool isHpfEnabled() const { return hpf > 0; }
    [[nodiscard]] bool isEnabled() const { return isOscEnabled() || isHpfEnabled(); }
};
```

**Two separate render methods** (split around reverb send in Sound::render):

```cpp
// Called PRE-reverb: strips fundamental from signal before it hits reverb
void renderHpf(std::span<StereoSample> buffer, int32_t noteCode);

// Called POST-reverb: generates sub sine and mixes into buffer
void renderOsc(std::span<StereoSample> buffer, int32_t noteCode, bool voicesActive,
               int32_t levelFinalValue, int32_t fineFinalValue);
```

Where `levelFinalValue` and `fineFinalValue` come from `paramFinalValues[]` in Sound::render().

**renderHpf() — pre-reverb HPF:**
- 12dB note-tracking HPF (two cascaded single-pole)
- Cutoff frequency derived from noteCode, HPF knob controls offset (3 octave range)
- Bypass when hpf==0
- Strips the fundamental/low-end so reverb only processes harmonics
- Has its own on/off independent of oscillator

**renderOsc() — post-reverb sub oscillator:**

*Oscillator DSP:*
- Convert noteCode to base frequency using `noteFrequencyTable[]`
- Multiply by harmonic ratio
- Apply fine tune from `fineFinalValue` (convert semitones to frequency multiplier)
- Portamento: exponentially smooth toward target frequency
- Phase accumulator: `phaseAccumL += phaseIncrement` per sample
- Waveform: `getSine(phaseAccumL)` returns int32_t Q31
- Apply AR envelope and level
- Phase/Spread: if knob <= 64, set start phase on trigger. If > 64, offset L/R phase accumulators

*AR envelope:*
- On trigger (voicesActive transitions false->true): start attack ramp
- Attack: `envelope += attackRate` per sample, clamped to 1.0
- Sustain: hold at 1.0 while voicesActive
- Release: `envelope -= releaseRate` per sample, clamped to 0.0
- Zero-crossing shutoff: when `envelope < threshold && (phaseAccum crossed 0 or 0x80000000)`, snap to 0

**Enable/disable logic:**
- **Harmonic knob = 0**: oscillator truly OFF — no sine lookup, no phase, no envelope. Zero CPU. `isOscEnabled()` returns false. Menu shows "OFF" via `IntegerWithOff`.
- **Harmonic knob >= 1**: oscillator runs. Even if patched Level is modulated to 0, keep phase accumulator and envelope running (cheap), just skip the output mix. Avoids clicks when modulation crosses zero. The direct knob is the master enable gate, not the patched param.
- **HPF knob = 0**: filter truly OFF — no processing. `isHpfEnabled()` returns false. Menu shows "OFF" via `IntegerWithOff`.
- **HPF knob >= 1**: filter always runs regardless of other param values.

**Signal flow (in Sound::render):**
```
...DOTT -> harm.renderHpf(buffer) -> processReverbSendAndVolume() -> harm.renderOsc(buffer) -> compressor
```
HPF strips fundamental pre-reverb. Reverb only gets harmonics. Sub oscillator adds clean fundamental back dry post-reverb.

**Serialization:**

```cpp
void writeToFile(Serializer& writer) const {
    WRITE_FIELD(writer, harmonic, "harmHarmonic");
    WRITE_FIELD(writer, phase, "harmPhase");
    WRITE_FIELD(writer, attack, "harmAttack");
    WRITE_FIELD_DEFAULT(writer, release, "harmRelease", 64);
    WRITE_FIELD(writer, hpf, "harmHpf");
    WRITE_FIELD(writer, porta, "harmPorta");
}

bool readTag(Deserializer& reader, const char* tagName) {
    READ_FIELD(reader, tagName, harmonic, "harmHarmonic");
    READ_FIELD(reader, tagName, phase, "harmPhase");
    READ_FIELD(reader, tagName, attack, "harmAttack");
    READ_FIELD(reader, tagName, release, "harmRelease");
    READ_FIELD(reader, tagName, hpf, "harmHpf");
    READ_FIELD(reader, tagName, porta, "harmPorta");
    return false;
}
```

**Notes for implementer:**
- The `noteCode` to `phaseIncrement` conversion needs care. Read `src/deluge/model/voice/voice.cpp` lines 602-729 to see how the existing oscillator does it. The noteFrequencyTable is indexed by `noteCode % 12` (note within octave) and shifted right by `(noteCode / 12) - 2` (octave). For harmonic ratios, multiply the phase increment by the ratio.
- For sub-harmonics (ratios < 1.0), you'll get very large phase increments that need right-shifting. Handle the ratio as a multiply + shift.
- The HPF should be a simple cascaded single-pole for 12dB: `state += alpha * (input - state)` twice. The cutoff `alpha` is derived from frequency.
- `getSine(phase)` from `util/waves.h` takes uint32_t phase (full 32-bit range = 2pi) and returns int32_t.

**Step 3: Build**

Run: `timeout 300 ./dbt build`

**Step 4: Commit**

```bash
git add src/deluge/dsp/harm.h
git commit -m "feat: add Harm effect DSP struct with oscillator, envelope, and HPF"
```

---

### Task 3: Add localization strings

**Files:**
- Modify: `src/deluge/gui/l10n/strings.h`
- Modify: `src/deluge/gui/l10n/english.json`
- Modify: `src/deluge/gui/l10n/seven_segment.json`

**Step 1: Add string enum values in strings.h**

After the utility strings (added earlier), add:

```cpp
// Harm effect
STRING_FOR_HARM_MENU,
STRING_FOR_HARM_HARMONIC,
STRING_FOR_HARM_FINE,
STRING_FOR_HARM_LEVEL,
STRING_FOR_HARM_PHASE,
STRING_FOR_HARM_ATTACK,
STRING_FOR_HARM_RELEASE,
STRING_FOR_HARM_HPF,
STRING_FOR_HARM_PORTA,
// Harm patched param display names
STRING_FOR_PARAM_GLOBAL_HARM_LEVEL,
STRING_FOR_PARAM_GLOBAL_HARM_FINE,
```

**Step 2: english.json entries**

```json
"STRING_FOR_HARM_MENU": "Harm",
"STRING_FOR_HARM_HARMONIC": "Harmonic",
"STRING_FOR_HARM_FINE": "Fine",
"STRING_FOR_HARM_LEVEL": "Level",
"STRING_FOR_HARM_PHASE": "Phase",
"STRING_FOR_HARM_ATTACK": "Attack",
"STRING_FOR_HARM_RELEASE": "Release",
"STRING_FOR_HARM_HPF": "HPF",
"STRING_FOR_HARM_PORTA": "Porta",
"STRING_FOR_PARAM_GLOBAL_HARM_LEVEL": "Harm Level",
"STRING_FOR_PARAM_GLOBAL_HARM_FINE": "Harm Fine",
```

**Step 3: seven_segment.json entries**

```json
"STRING_FOR_HARM_MENU": "HARM",
"STRING_FOR_HARM_HARMONIC": "HRMC",
"STRING_FOR_HARM_FINE": "FINE",
"STRING_FOR_HARM_LEVEL": "LVL ",
"STRING_FOR_HARM_PHASE": "PHAS",
"STRING_FOR_HARM_ATTACK": "ATCK",
"STRING_FOR_HARM_RELEASE": "RLSE",
"STRING_FOR_HARM_HPF": "HPF ",
"STRING_FOR_HARM_PORTA": "PORT",
"STRING_FOR_PARAM_GLOBAL_HARM_LEVEL": "HLVL",
"STRING_FOR_PARAM_GLOBAL_HARM_FINE": "HFIN",
```

**Step 4: Regenerate**

Run: `cd src/deluge/gui/l10n && python3 generate.py`

**Step 5: Build**

Run: `timeout 300 ./dbt build`

**Step 6: Commit**

```bash
git add src/deluge/gui/l10n/
git commit -m "feat: add Harm effect localization strings"
```

---

### Task 4: Wire into ModControllableAudio with serialization

**Files:**
- Modify: `src/deluge/model/mod_controllable/mod_controllable_audio.h`
- Modify: `src/deluge/model/mod_controllable/mod_controllable_audio.cpp`

**Step 1: Add member**

In `mod_controllable_audio.h`, add include and member near the utility member:

```cpp
#include "dsp/harm.h"
```

```cpp
deluge::dsp::HarmParams harm;
```

**Step 2: Add serialization write**

In `writeParamAttributesToFile()`, after `utility.writeToFile(writer);`:

```cpp
harm.writeToFile(writer);
```

**Step 3: Add serialization read**

In `readTagFromFile()`, after the utility readTag block:

```cpp
else if (harm.readTag(reader, tagName)) {
    // Reading handled internally
}
```

**Step 4: Build and commit**

```bash
git commit -m "feat: wire Harm effect into ModControllableAudio with serialization"
```

---

### Task 5: Insert into signal chain (Sound only — split around reverb)

**Files:**
- Modify: `src/deluge/processing/sound/sound.cpp`

The Harm effect is split into two calls that straddle the reverb send. Read sound.cpp around line 2809 to see:

```cpp
// Current code (after DOTT and ModFX-post-DOTT):
processReverbSendAndVolume(sound_stereo, reverbBuffer, postFXVolume, postReverbVolume, reverbSendAmount, 0, true);

q31_t compThreshold = paramManager->getUnpatchedParamSet()->getValue(params::UNPATCHED_COMPRESSOR_THRESHOLD);
```

**Step 1: Add HPF call BEFORE processReverbSendAndVolume()**

Right before `processReverbSendAndVolume()` (line 2809), add:

```cpp
// Harm: compute lowest active note for pitch tracking (sub reinforces bass note)
int32_t harmNoteCode = lastNoteCode; // fallback when no voices active
if (harm.isEnabled() && !voices_.empty()) {
    harmNoteCode = std::numeric_limits<int32_t>::max();
    for (auto& voice : voices_) {
        harmNoteCode = std::min(harmNoteCode, voice->noteCodeAfterArpeggiation);
    }
}

// Harm HPF - strip fundamental pre-reverb so reverb only gets harmonics
if (harm.isHpfEnabled()) {
    harm.renderHpf(sound_stereo, harmNoteCode);
}
```

**Step 2: Add oscillator call AFTER processReverbSendAndVolume()**

Right after `processReverbSendAndVolume()` and before the compressor threshold code, add:

```cpp
// Harm oscillator - add clean sub harmonic back post-reverb (dry)
if (harm.isOscEnabled()) {
    int32_t harmLevel = paramFinalValues[params::GLOBAL_HARM_LEVEL - params::FIRST_GLOBAL];
    int32_t harmFine = paramFinalValues[params::GLOBAL_HARM_FINE - params::FIRST_GLOBAL];
    harm.renderOsc(sound_stereo, harmNoteCode, !voices_.empty(), harmLevel, harmFine);
}
```

The signal chain becomes:
```
...DOTT -> ModFX(post-DOTT) -> harm.renderHpf() -> processReverbSendAndVolume() -> harm.renderOsc() -> compressor
```

HPF strips fundamental before reverb send. Reverb only processes harmonics (clean tails). Sub oscillator adds clean fundamental back dry after reverb. Sub doesn't go through reverb or compressor sidechain.

**Note:** Harm is Sound-only (not in GlobalEffectable) because it needs `lastNoteCode` and `voices_` for pitch tracking and envelope triggering.

**Step 3: Build and commit**

```bash
git commit -m "feat: insert Harm effect into Sound signal chain (split around reverb)"
```

---

### Task 6: Create menu item classes

**Files:**
- Create: `src/deluge/gui/menu_item/fx/harm.h`

**Step 1: Read reference files**

- `src/deluge/gui/menu_item/fx/utility.h` — direct-access pattern
- `src/deluge/gui/menu_item/fx/eroder.h` — IntegerWithOff pattern
- `src/deluge/gui/menu_item/patched_param/integer.h` — patched param menu item

**Step 2: Create menu item classes**

Use Owlet Records copyright header. Namespace: `deluge::gui::menu_item::fx`.

Create these classes:

**Direct-access menu items (like utility pattern):**
- `HarmHarmonic` — extends `IntegerWithOff`, reads/writes `harm.harmonic`, max=11. Value 0 = OFF (no sine lookup). Display as ratio names: "OFF", "1/4", "1/3", "1/2", "1", "2", "3", "4", "5", "6", "7", "8". Override rendering to show ratio names instead of raw numbers.
- `HarmPhase` — extends `Integer`, reads/writes `harm.phase`, max=127, BAR style
- `HarmAttack` — extends `Integer`, reads/writes `harm.attack`, max=127, BAR style
- `HarmRelease` — extends `Integer`, reads/writes `harm.release`, max=127, BAR style
- `HarmHpf` — extends `IntegerWithOff`, reads/writes `harm.hpf`, max=127, BAR style (0=off)
- `HarmPorta` — extends `IntegerWithOff`, reads/writes `harm.porta`, max=127, BAR style (0=off)

**Patched param menu items** use existing `patched_param::Integer` — these are declared directly in menus.cpp, not in this header. They're wired via `params::GLOBAL_HARM_LEVEL` and `params::GLOBAL_HARM_FINE`.

All direct-access items need affect-entire support (same pattern as utility).

**Step 3: Build and commit**

```bash
git commit -m "feat: add Harm effect menu item classes"
```

---

### Task 7: Register in FX menu

**Files:**
- Modify: `src/deluge/gui/ui/menus.cpp`

**Step 1: Add include**

```cpp
#include "gui/menu_item/fx/harm.h"
```

**Step 2: Declare menu item instances**

After the utility menu declarations:

```cpp
// Harm - note-tracking HPF + clean sub oscillator
fx::HarmHarmonic harmHarmonicMenu{STRING_FOR_HARM_HARMONIC};
patched_param::Integer harmFineMenu{STRING_FOR_HARM_FINE, STRING_FOR_HARM_FINE, params::GLOBAL_HARM_FINE};
patched_param::Integer harmLevelMenu{STRING_FOR_HARM_LEVEL, STRING_FOR_HARM_LEVEL, params::GLOBAL_HARM_LEVEL};
fx::HarmPhase harmPhaseMenu{STRING_FOR_HARM_PHASE};
fx::HarmAttack harmAttackMenu{STRING_FOR_HARM_ATTACK};
fx::HarmRelease harmReleaseMenu{STRING_FOR_HARM_RELEASE};
fx::HarmHpf harmHpfMenu{STRING_FOR_HARM_HPF};
fx::HarmPorta harmPortaMenu{STRING_FOR_HARM_PORTA};

submenu::CompressorHorizontalMenu harmSubMenu{
    STRING_FOR_HARM_MENU,
    {&harmHarmonicMenu, &harmFineMenu, &harmLevelMenu, &harmPhaseMenu,
     &harmAttackMenu, &harmReleaseMenu, &harmHpfMenu, &harmPortaMenu},
    HorizontalMenu::Layout::DYNAMIC,
};
```

Note: Use `CompressorHorizontalMenu` or `HorizontalMenu` with `DYNAMIC` layout for >4 items (2 pages of 4).

**Step 3: Add to soundFXMenu only**

In soundFXMenu (Harm needs voice/note info, so Sound only — not globalFXMenu or audioClipFXMenu):

```cpp
Submenu soundFXMenu{
    STRING_FOR_FX,
    {
        &eqMenu,
        &delayMenu,
        &reverbMenu,
        &stutterMenu,
        &modFXMenu,
        &shapingMenu,
        &soundDistortionMenu,
        &noiseMenu,
        &dott_menu,
        &harmSubMenu,   // <-- add
        &utilitySubMenu,
    },
};
```

**Step 4: Build and commit**

```bash
git commit -m "feat: register Harm effect in Sound FX menu"
```

---

### Task 8: Format and final build

**Step 1:** `./dbt format`

**Step 2:** `timeout 300 ./dbt build`

**Step 3:** Commit any formatting changes

---

### Implementation Notes

**Param category choices:**
- `GLOBAL_HARM_LEVEL`: Volume category — these are linear multiplicative gains, applied as `Q31 * level`. Placing it in volume category means the patcher handles it like other volume params (squared at end for perceptual linearity).
- `GLOBAL_HARM_FINE`: Hybrid category — bipolar additive, base +-cents. The fine tune base value is 0 (no detune), modulation adds/subtracts cents.

**HPF 12dB implementation:**
- Two cascaded single-pole HPFs: `y = input - state; state += alpha * y;` applied twice
- Alpha from cutoff frequency: `alpha = 2.0 * pi * fc / sampleRate` (or fixed-point equivalent)
- Cutoff = oscillator frequency * HPF knob (3 octave range: knob=1 -> 1/8 osc freq, knob=127 -> osc freq)
- Filter state persists across buffers, reset only on first activation

**Harmonic ratio display:**
- The HarmHarmonic menu item should override rendering to display ratio names ("OFF", "1/4", "1/3", etc.) instead of raw 0-11 values. Look at how existing enum-style menu items display named values (e.g., ModFXType selection, CompressorMode).

**Phase/Spread knob:**
- 0-64: on trigger, set `phaseAccumL = phaseAccumR = (knob * 0xFFFFFFFF) / 64`
- 65-127: `spread = ((knob - 65) * 0x80000000) / 62`. On trigger: `phaseAccumL = +spread/2`, `phaseAccumR = -spread/2` (relative to 0). Both use same phaseIncrement.

**Fine tune range and snap:**
- Range: -12 to +12 semitones (bipolar, hybrid patched param, center = 0)
- Default behavior: snaps to whole semitones when turning encoder
- Encoder press: unlocks fine continuous tuning (cents resolution)
- Frequency multiplier: `2^(semitones/12)` — use `noteIntervalTable[]` for exact semitone ratios, interpolate for fine cents
- The snap/fine behavior is a menu item UX concern (HarmFine menu class), not a DSP concern — the param value itself is continuous, snap is just encoder step quantization
