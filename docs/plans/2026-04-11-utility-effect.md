# Utility Effect Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a utility channel-strip effect (volume, pan L, pan R, stereo width) placed right before DOTT in the signal chain.

**Architecture:** Lightweight inline DSP params struct on ModControllableAudio. Processing is a simple render function called in both Sound and GlobalEffectable paths. Menu items follow the eroder/disperser pattern (direct struct field access, no patched/unpatched params). Serialization uses the WRITE_FIELD/READ_FIELD macro pattern.

**Tech Stack:** C++, Q31 fixed-point DSP, Deluge menu system, XML serialization

---

### Task 1: Create DSP params struct and processing function

**Files:**
- Create: `src/deluge/dsp/utility.h`

**Step 1: Create the utility DSP header**

Use the Owlet Records copyright header (reference: `src/deluge/dsp/eroder.h` lines 1-20).

```cpp
#pragma once

#include "dsp/stereo_sample.h"
#include "storage/field_serialization.h"
#include "storage/storage_manager.h"
#include "util/fixedpoint.h"
#include <cstdint>
#include <span>

namespace deluge::dsp {

struct UtilityParams {
	uint8_t volume{64};  // 0=off(-inf), 64=unity, 127=+12dB
	uint8_t panL{64};    // 0=silence, 64=center, 127=hard right
	uint8_t panR{64};    // 0=silence, 64=center, 127=hard right
	uint8_t width{64};   // 0=mono, 64=normal(100%), 127=wide(~200%)

	[[nodiscard]] bool isEnabled() const {
		return volume != 64 || panL != 64 || panR != 64 || width != 64;
	}

	void render(std::span<StereoSample> buffer) const {
		if (!isEnabled()) {
			return;
		}

		// Volume: 0=silence, 64=unity(0dB), 127=+12dB
		// Map 0-127 to gain multiplier in Q31
		q31_t gainQ31;
		if (volume == 0) {
			// -inf: silence the buffer
			for (auto& sample : buffer) {
				sample.l = 0;
				sample.r = 0;
			}
			return;
		}
		else if (volume <= 64) {
			// 0dB at 64, linear fade to silence at 0
			// gain = volume / 64.0 in Q31
			gainQ31 = static_cast<q31_t>((static_cast<int64_t>(volume) * ONE_Q31) / 64);
		}
		else {
			// +12dB at 127, linear from 0dB at 64
			// gain = 1.0 + (volume - 64) / 63.0 * 3.0  (4x = +12dB)
			// In Q31: ONE_Q31 + (volume-64) * (3 * ONE_Q31) / 63
			int32_t excess = volume - 64;
			gainQ31 = ONE_Q31 + static_cast<q31_t>((static_cast<int64_t>(excess) * 3 * ONE_Q31) / 63);
		}

		// Pan L/R: 0=silence, 64=center(unity), 127=hard opposite
		// Convert to per-channel L/R gain multipliers
		// panL controls where the left input goes: 64=center, 0=silence, 127=full right
		// panR controls where the right input goes: 64=center, 0=silence, 127=full left
		q31_t panL_left, panL_right, panR_left, panR_right;

		if (panL <= 64) {
			// Left channel: pan from silence(0) to center(64)
			panL_left = static_cast<q31_t>((static_cast<int64_t>(panL) * ONE_Q31) / 64);
			panL_right = 0;
		}
		else {
			// Left channel: pan from center(64) to hard right(127)
			int32_t rightAmount = panL - 64;
			panL_left = static_cast<q31_t>((static_cast<int64_t>(64 - rightAmount) * ONE_Q31) / 64);
			panL_right = static_cast<q31_t>((static_cast<int64_t>(rightAmount) * ONE_Q31) / 64);
		}

		if (panR <= 64) {
			// Right channel: pan from silence(0) to center(64)
			panR_right = static_cast<q31_t>((static_cast<int64_t>(panR) * ONE_Q31) / 64);
			panR_left = 0;
		}
		else {
			// Right channel: pan from center(64) to hard left(127)
			int32_t leftAmount = panR - 64;
			panR_right = static_cast<q31_t>((static_cast<int64_t>(64 - leftAmount) * ONE_Q31) / 64);
			panR_left = static_cast<q31_t>((static_cast<int64_t>(leftAmount) * ONE_Q31) / 64);
		}

		// Width: mid/side processing
		// 0=mono(0%), 64=normal(100%), 127=wide(~200%)
		// widthFactor: 0.0 at knob=0, 1.0 at knob=64, ~2.0 at knob=127
		// In Q31: 0 at 0, ONE_Q31 at 64, ~2*ONE_Q31 at 127
		q31_t widthQ31;
		if (width <= 64) {
			widthQ31 = static_cast<q31_t>((static_cast<int64_t>(width) * ONE_Q31) / 64);
		}
		else {
			int32_t excess = width - 64;
			// Can exceed ONE_Q31 — that's fine, we handle the math carefully
			widthQ31 = ONE_Q31 + static_cast<q31_t>((static_cast<int64_t>(excess) * ONE_Q31) / 63);
		}

		for (auto& sample : buffer) {
			int32_t inL = sample.l;
			int32_t inR = sample.r;

			// Mid/side width processing
			int32_t mid = (inL >> 1) + (inR >> 1);
			int32_t side = (inL >> 1) - (inR >> 1);

			// Apply width to side signal
			int32_t wideSide = multiply_32x32_rshift32(side, widthQ31) << 1;

			int32_t widthL = mid + wideSide;
			int32_t widthR = mid - wideSide;

			// Apply pan (L input contributes to both channels, R input contributes to both)
			int32_t outL = multiply_32x32_rshift32(widthL, panL_left) + multiply_32x32_rshift32(widthR, panR_left);
			int32_t outR = multiply_32x32_rshift32(widthL, panL_right) + multiply_32x32_rshift32(widthR, panR_right);
			outL <<= 1;
			outR <<= 1;

			// Apply volume gain
			sample.l = multiply_32x32_rshift32(outL, gainQ31) << 1;
			sample.r = multiply_32x32_rshift32(outR, gainQ31) << 1;
		}
	}

	// Serialization
	void writeToFile(Serializer& writer) const {
		WRITE_FIELD_DEFAULT(writer, volume, "utilityVolume", 64);
		WRITE_FIELD_DEFAULT(writer, panL, "utilityPanL", 64);
		WRITE_FIELD_DEFAULT(writer, panR, "utilityPanR", 64);
		WRITE_FIELD_DEFAULT(writer, width, "utilityWidth", 64);
	}

	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, volume, "utilityVolume");
		READ_FIELD(reader, tagName, panL, "utilityPanL");
		READ_FIELD(reader, tagName, panR, "utilityPanR");
		READ_FIELD(reader, tagName, width, "utilityWidth");
		return false;
	}
};

} // namespace deluge::dsp
```

**Notes for implementer:**
- `ONE_Q31` is `0x7FFFFFFF` — verify this constant exists. If not, use the appropriate constant from `fixedpoint.h`.
- `multiply_32x32_rshift32` is the standard Q31 multiply used throughout the codebase.
- Pan at default (64) should be bit-exact pass-through. Volume at 64 should be unity. Width at 64 should be pass-through. Verify the math achieves this.
- The gain above +12dB means the max multiplier is 4x (volume=127). Since Q31 max is ~1.0, you may need to handle this differently — possibly apply gain as two multiply stages, or use a shift. **Review the eroder/compressor code for how gains >1.0 are handled in Q31.**

**Step 2: Verify it compiles**

Run: `timeout 300 ./dbt build`

**Step 3: Commit**

```bash
git add src/deluge/dsp/utility.h
git commit -m "feat: add utility effect DSP params struct"
```

---

### Task 2: Add utility member to ModControllableAudio and wire serialization

**Files:**
- Modify: `src/deluge/model/mod_controllable/mod_controllable_audio.h` (~line 140, near other effect members)
- Modify: `src/deluge/model/mod_controllable/mod_controllable_audio.cpp` (~line 570 writeParamAttributesToFile, ~line 1200 readTagFromFile)

**Step 1: Add member variable**

In `mod_controllable_audio.h`, add near the other DSP effect members (around line 140, after disperser):

```cpp
#include "dsp/utility.h"
```

And in the class body:

```cpp
deluge::dsp::UtilityParams utility;
```

**Step 2: Add serialization write**

In `mod_controllable_audio.cpp` `writeParamAttributesToFile()`, after the existing effect writes (~line 572, after `multibandCompressor.writeToFile(writer);`):

```cpp
utility.writeToFile(writer);
```

**Step 3: Add serialization read**

In `mod_controllable_audio.cpp` `readTagFromFile()`, after the multiband compressor readTag block (~line 1202):

```cpp
else if (utility.readTag(reader, tagName)) {
	// Reading handled internally
}
```

**Step 4: Build and verify**

Run: `timeout 300 ./dbt build`

**Step 5: Commit**

```bash
git add src/deluge/model/mod_controllable/mod_controllable_audio.h src/deluge/model/mod_controllable/mod_controllable_audio.cpp
git commit -m "feat: wire utility effect into ModControllableAudio with serialization"
```

---

### Task 3: Insert utility processing into signal chain

**Files:**
- Modify: `src/deluge/processing/sound/sound.cpp` (~line 2791, after processStutter)
- Modify: `src/deluge/model/global_effectable/global_effectable_for_clip.cpp` (~line 194, after processStutter)

**Step 1: Add utility processing in Sound path**

In `sound.cpp`, after line 2791 (`processStutter(sound_stereo, paramManager, modulatedScatterValues);`) and before the DOTT block:

```cpp
// Utility (volume, pan, stereo width) - gain staging before DOTT
utility.render(sound_stereo);
```

**Step 2: Add utility processing in GlobalEffectable path**

In `global_effectable_for_clip.cpp`, after line 194 (`processStutter(global_effectable_audio, paramManagerForClip);`) and before the DOTT block:

```cpp
// Utility (volume, pan, stereo width) - gain staging before DOTT
utility.render(global_effectable_audio);
```

**Step 3: Build and verify**

Run: `timeout 300 ./dbt build`

**Step 4: Commit**

```bash
git add src/deluge/processing/sound/sound.cpp src/deluge/model/global_effectable/global_effectable_for_clip.cpp
git commit -m "feat: insert utility effect before DOTT in signal chain"
```

---

### Task 4: Add localization strings

**Files:**
- Modify: `src/deluge/gui/l10n/strings.h` (~line 873, after eroder strings)
- Modify: `src/deluge/gui/l10n/english.json`
- Modify: `src/deluge/gui/l10n/seven_segment.json`

**Step 1: Add string enum values**

In `strings.h`, after the eroder string entries (around line 873):

```cpp
// Utility effect
STRING_FOR_UTILITY_MENU,
STRING_FOR_UTILITY_VOLUME,
STRING_FOR_UTILITY_PAN_L,
STRING_FOR_UTILITY_PAN_R,
STRING_FOR_UTILITY_WIDTH,
```

**Step 2: Add English display strings**

In `english.json`, add entries (alphabetically or near eroder entries):

```json
"STRING_FOR_UTILITY_MENU": "Utility",
"STRING_FOR_UTILITY_VOLUME": "Volume",
"STRING_FOR_UTILITY_PAN_L": "Pan L",
"STRING_FOR_UTILITY_PAN_R": "Pan R",
"STRING_FOR_UTILITY_WIDTH": "Width",
```

**Step 3: Add 7-segment display strings**

In `seven_segment.json`, add entries (4-char format):

```json
"STRING_FOR_UTILITY_MENU": "UTIL",
"STRING_FOR_UTILITY_VOLUME": "VOL ",
"STRING_FOR_UTILITY_PAN_L": "PANL",
"STRING_FOR_UTILITY_PAN_R": "PANR",
"STRING_FOR_UTILITY_WIDTH": "WIDT",
```

**Step 4: Regenerate C++ from JSON**

Run: `cd src/deluge/gui/l10n && python3 generate.py`

**Step 5: Build and verify**

Run: `timeout 300 ./dbt build`

**Step 6: Commit**

```bash
git add src/deluge/gui/l10n/
git commit -m "feat: add utility effect localization strings"
```

---

### Task 5: Create menu item classes

**Files:**
- Create: `src/deluge/gui/menu_item/fx/utility.h`

**Step 1: Create the menu item header**

Use Owlet Records copyright header. Follow the eroder menu pattern (`src/deluge/gui/menu_item/fx/eroder.h` lines 396-459).

```cpp
#pragma once

#include "gui/menu_item/integer.h"
#include "gui/ui/sound_editor.h"
#include "model/drum/drum.h"
#include "model/drum/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item::fx {

// Volume: 0=off(-inf), 64=unity, 127=+12dB
class UtilityVolume final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->utility.volume); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->utility.volume = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->utility.volume = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

// Pan L: 0=silence, 64=center, 127=hard right
class UtilityPanL final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->utility.panL); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->utility.panL = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->utility.panL = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

// Pan R: 0=silence, 64=center, 127=hard left
class UtilityPanR final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->utility.panR); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->utility.panR = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->utility.panR = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

// Width: 0=mono(0%), 64=normal(100%), 127=wide(~200%)
class UtilityWidth final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override { this->setValue(soundEditor.currentModControllable->utility.width); }
	bool usesAffectEntire() override { return true; }

	void writeCurrentValue() override {
		int32_t current_value = this->getValue();
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->utility.width = current_value;
				}
			}
		}
		else {
			soundEditor.currentModControllable->utility.width = current_value;
		}
	}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return BAR; }
};

} // namespace deluge::gui::menu_item::fx
```

**Step 2: Build and verify**

Run: `timeout 300 ./dbt build`

**Step 3: Commit**

```bash
git add src/deluge/gui/menu_item/fx/utility.h
git commit -m "feat: add utility effect menu item classes"
```

---

### Task 6: Register menu items and add to FX menu

**Files:**
- Modify: `src/deluge/gui/ui/menus.cpp` (add menu item instances + add to soundFXMenu)

**Step 1: Add menu item instances**

In `menus.cpp`, after the eroder menu declarations (around line 803), add:

```cpp
// Utility - channel strip (volume, pan, width)
fx::UtilityVolume utilityVolumeMenu{STRING_FOR_UTILITY_VOLUME};
fx::UtilityPanL utilityPanLMenu{STRING_FOR_UTILITY_PAN_L};
fx::UtilityPanR utilityPanRMenu{STRING_FOR_UTILITY_PAN_R};
fx::UtilityWidth utilityWidthMenu{STRING_FOR_UTILITY_WIDTH};

HorizontalMenu utilitySubMenu{
    STRING_FOR_UTILITY_MENU,
    {&utilityVolumeMenu, &utilityPanLMenu, &utilityPanRMenu, &utilityWidthMenu},
};
```

Add the include at the top of menus.cpp:

```cpp
#include "gui/menu_item/fx/utility.h"
```

**Step 2: Add to soundFXMenu**

In the `soundFXMenu` definition (around line 1587-1599), add `&utilitySubMenu` to the list. Place it after `&dott_menu` or wherever appropriate:

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
        &utilitySubMenu,  // <-- add here
    },
};
```

**Step 3: Check if there's a separate globalFXMenu or clipFXMenu**

Search for other FX menu definitions that need the utility entry. The GlobalEffectable path may have its own menu — check and add there too.

**Step 4: Build and verify**

Run: `timeout 300 ./dbt build`

**Step 5: Commit**

```bash
git add src/deluge/gui/ui/menus.cpp
git commit -m "feat: register utility effect in FX menu"
```

---

### Task 7: Format, build, and final verification

**Step 1: Format code**

Run: `./dbt format`

**Step 2: Final build**

Run: `timeout 300 ./dbt build`

**Step 3: Commit any formatting changes**

```bash
git add -u
git commit -m "style: format utility effect code"
```

---

### Implementation Notes

- **Q31 gain >1.0**: The volume knob goes to +12dB (4x gain). Since Q31 max represents ~1.0, gains above unity need special handling. Options: (a) apply gain in two stages (e.g., 2x via left-shift + remaining via multiply), (b) use the approach from `RMSFeedbackCompressor::render()` which also applies gains. Check how existing code handles this before implementing.
- **Default values**: All params default to 64 (pass-through). The `isEnabled()` check skips processing entirely when all at default — zero CPU cost when not in use.
- **Pan semantics**: Pan L=64, Pan R=64 means both channels centered (normal stereo). Pan L=0 silences left input. Pan L=127 moves left input fully to right output. This gives independent per-channel routing.
- **No param system**: All values are stored directly on the struct. No UnpatchedParamSet, no modulation routing. This keeps it simple.
