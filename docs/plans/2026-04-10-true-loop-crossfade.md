# True Loop Crossfade Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the current fade-out/fade-in loop crossfade with a true simultaneous dual-read crossfade that blends loop-end and loop-start audio together.

**Architecture:** When the main cache read head is within `crossfadeLengthBytes` of the loop end, a second read head starts at the loop start. Both write to the output buffer — main fading out, crossfade fading in. When the main head reaches the loop end, the crossfade head becomes the new main head. Same concept for the uncached (time-stretch) path.

**Tech Stack:** C++, Q31 fixed-point DSP, Deluge sample cache system

---

### Task 1: Add crossfade state to VoiceSample

**Files:**
- Modify: `src/deluge/model/voice/voice_sample.h:89-91`
- Modify: `src/deluge/model/voice/voice_sample.cpp:43-57` (noteOn reset)

**Step 1: Add new fields to voice_sample.h**

After line 91 (`loopFadeInSamplesTotal`), add:

```cpp
int32_t crossfadeCacheBytePos{0};
bool crossfadeActive{false};
```

**Step 2: Reset new fields in noteOn()**

In `voice_sample.cpp` around line 55, after `loopFadeInSamplesRemaining = 0;`, add:

```cpp
crossfadeActive = false;
crossfadeCacheBytePos = 0;
```

**Step 3: Build to verify no regressions**

Run: `timeout 300 ./dbt build`
Expected: Clean build, no errors.

**Step 4: Commit**

```
feat: add crossfade state fields to VoiceSample
```

---

### Task 2: Convert cached crossfade length from samples to cache bytes

We need `crossfadeLengthCacheBytes` to know when to enter the crossfade region in cache space. This is derived from `loopFadeInSamplesTotal` — each output sample corresponds to `kCacheByteDepth * numChannels` cache bytes.

**Files:**
- Modify: `src/deluge/model/voice/voice_sample.cpp:649-688` (cached read path, loop boundary detection)

**Step 1: Compute crossfade length in cache bytes**

After line 650 (`bool pingpongCacheMode = ...`), add:

```cpp
int32_t frameSizeBytes = kCacheByteDepth * sampleSourceNumChannels;
int32_t crossfadeLengthCacheBytes = loopFadeInSamplesTotal * frameSizeBytes;
// Clamp crossfade to half the loop length
if (crossfadeLengthCacheBytes > 0 && (int32_t)cacheLoopLengthBytes > 0) {
    crossfadeLengthCacheBytes = std::min(crossfadeLengthCacheBytes, (int32_t)(cacheLoopLengthBytes / 2));
}
```

Note: `frameSizeBytes` is already declared at line 662 inside the pingpong block — move its declaration up and remove the duplicate.

**Step 2: Build**

Run: `timeout 300 ./dbt build`
Expected: Clean build.

**Step 3: Commit**

```
refactor: compute crossfade length in cache bytes
```

---

### Task 3: Enter crossfade region and initialize second read head (cached path)

When forward-reading and within `crossfadeLengthCacheBytes` of the loop end, activate the crossfade and position the second reader at the loop start.

**Files:**
- Modify: `src/deluge/model/voice/voice_sample.cpp:654-673` (forward loop boundary handling)

**Step 1: Add crossfade activation**

In the forward direction block (after computing `bytesTilLoopEndPoint` at ~line 656), before the existing `if (bytesTilLoopEndPoint <= 0)` check, add crossfade entry logic:

```cpp
// Enter crossfade region when approaching loop end (forward only, non-pingpong)
if (!crossfadeActive && !pingpongCacheMode && crossfadeLengthCacheBytes > 0
    && bytesTilLoopEndPoint > 0 && bytesTilLoopEndPoint <= crossfadeLengthCacheBytes) {
    crossfadeActive = true;
    crossfadeCacheBytePos = cacheLoopStartPointBytes;
    loopFadeInSamplesRemaining = loopFadeInSamplesTotal;
}
```

**Step 2: Modify loop-restart to hand off from crossfade**

In the existing `if (bytesTilLoopEndPoint <= 0)` block for the non-pingpong case (~line 666), change the loop restart to use the crossfade head position if active:

```cpp
else {
    // Normal loop: if crossfade was active, take over from crossfade head
    if (crossfadeActive) {
        cacheBytePos = crossfadeCacheBytePos;
        crossfadeActive = false;
    }
    else {
        cacheBytePos -= cacheLoopLengthBytes;
    }
}
```

Remove the old `loopFadeInSamplesRemaining = loopFadeInSamplesTotal` from this block since crossfade entry now handles it.

**Step 3: Build**

Run: `timeout 300 ./dbt build`
Expected: Clean build.

**Step 4: Commit**

```
feat: activate crossfade region and init second read head
```

---

### Task 4: Dual-read render loop for cached path

This is the core change. When `crossfadeActive`, the inner cache render loop reads from both `cacheBytePos` (fading out) and `crossfadeCacheBytePos` (fading in), blending into the output buffer.

**Files:**
- Modify: `src/deluge/model/voice/voice_sample.cpp:819-916` (cache render amplitude + inner loop)

**Step 1: Replace the old fade-in/fade-out envelope with crossfade blending**

Replace the entire crossfade envelope block (lines ~823-874) with:

```cpp
if (crossfadeActive && loopFadeInSamplesRemaining > 0) {
    // True crossfade: compute fade progress (0 = start of xfade, 1 = end)
    int32_t fadeProgress = loopFadeInSamplesTotal - loopFadeInSamplesRemaining;

    int32_t fadeInStart = static_cast<int32_t>(
        std::min(((int64_t)fadeProgress << 31) / loopFadeInSamplesTotal, (int64_t)0x7FFFFFFF));
    int32_t fadeInEnd = static_cast<int32_t>(
        std::min(((int64_t)(fadeProgress + numSamplesThisCacheRead) << 31) / loopFadeInSamplesTotal,
                 (int64_t)0x7FFFFFFF));

    // Main read fades OUT: amplitude * (1 - fade)
    int32_t fadeOutStart = 0x7FFFFFFF - fadeInStart;
    int32_t fadeOutEnd = 0x7FFFFFFF - fadeInEnd;

    int32_t mainAmpStart = multiply_32x32_rshift32(amplitude, fadeOutStart) << 1;
    int32_t mainAmpEnd = multiply_32x32_rshift32(
        amplitude + amplitudeIncrement * numSamplesThisCacheRead, fadeOutEnd) << 1;

    cacheRenderAmplitude = mainAmpStart;
    cacheRenderAmplitudeIncrement = (mainAmpEnd - mainAmpStart) / numSamplesThisCacheRead;

    // Crossfade read fades IN — computed here, applied in render loop below
    int32_t xfadeAmpStart = multiply_32x32_rshift32(amplitude, fadeInStart) << 1;
    int32_t xfadeAmpEnd = multiply_32x32_rshift32(
        amplitude + amplitudeIncrement * numSamplesThisCacheRead, fadeInEnd) << 1;

    // Store for the render loop (local variables used below)
    xfadeAmplitude = xfadeAmpStart;
    xfadeAmplitudeIncrement = (xfadeAmpEnd - xfadeAmpStart) / numSamplesThisCacheRead;

    loopFadeInSamplesRemaining -= numSamplesThisCacheRead;
    if (loopFadeInSamplesRemaining < 0) {
        loopFadeInSamplesRemaining = 0;
    }
}
```

Declare `int32_t xfadeAmplitude = 0;` and `int32_t xfadeAmplitudeIncrement = 0;` alongside `cacheRenderAmplitude`/`cacheRenderAmplitudeIncrement` at line ~820.

**Step 2: Add second cache read inside the forward render loop**

In the forward render loop (~line 882-916), after each main sample is written to the output buffer, add the crossfade read when active. The crossfade reader needs its own cluster lookup since it may be in a different cluster than the main reader.

After the main `multiply_accumulate_32x32_rshift32_rounded` writes, add:

```cpp
if (crossfadeActive) {
    // Read from crossfade position
    int32_t xfadeCachedClusterIndex = crossfadeCacheBytePos >> Cluster::size_magnitude;
    int32_t xfadeBytePosWithinCluster = crossfadeCacheBytePos & (Cluster::size - 1);
    Cluster* xfadeCluster = cache->getCluster(xfadeCachedClusterIndex);
    if (xfadeCluster) {
        int32_t* xfadeReadPos = (int32_t*)&xfadeCluster->data[xfadeBytePosWithinCluster];
        int32_t xfadeSampleL = *xfadeReadPos;

        xfadeAmplitude += xfadeAmplitudeIncrement;

        // Write L to output (accumulate)
        *(outputBufferWritePos - (numChannelsInOutputBuffer == 2 ? 2 : 1)) +=
            multiply_32x32_rshift32_rounded(xfadeSampleL, xfadeAmplitude);

        if (sampleSourceNumChannels == 2) {
            int32_t xfadeSampleR = *(int32_t*)((char*)xfadeReadPos + kCacheByteDepth);
            if (numChannelsInOutputBuffer == 2) {
                *(outputBufferWritePos - 1) +=
                    multiply_32x32_rshift32_rounded(xfadeSampleR, xfadeAmplitude);
            }
        }

        crossfadeCacheBytePos += frameSizeBytes;
    }
}
```

**Important:** This is pseudocode for the concept. The exact integration into the tight inner loop needs care — the main loop is performance-critical. Consider structuring as two separate loops (crossfade active vs not) to avoid the branch in the hot path. See the existing forward/backward split at line 882/918 as a pattern.

**Step 3: Build and test**

Run: `timeout 300 ./dbt build`
Expected: Clean build. Test on hardware with a looped sample and crossfade > 0.

**Step 4: Commit**

```
feat: true dual-read crossfade for cached sample loops
```

---

### Task 5: Uncached (time-stretch) crossfade path

Apply the same dual-read concept to the uncached render path at lines ~1276-1356. This path is used when time stretching is active or when the cache runs out.

**Files:**
- Modify: `src/deluge/model/voice/voice_sample.cpp:1276-1356`

**Step 1: Replace sequential fade with true crossfade**

The uncached path is more complex because reading from a second position requires setting up a second `SampleLowLevelReader` or pre-buffering. The approach here:

1. When entering the crossfade zone (detected via `distOutputSamples <= loopFadeInSamplesTotal`), save the current playback state.
2. For the main read: apply fade-out amplitude envelope (same as current).
3. For the crossfade read: we need audio from the loop start position.

**Design consideration:** The uncached path doesn't have easy random access like the cache does. Reading from a second position requires either:
- A second `SampleLowLevelReader` with its own cluster chain (like TimeStretcher's `olderPartReader`)
- Pre-buffering the loop start into a scratch buffer when starting crossfade

Given the complexity, the recommended approach for V1 is: **only do true crossfade in the cached path; keep the existing fade-out/fade-in for uncached.** The cache handles the vast majority of looped playback. If the sample falls back to uncached mid-crossfade, the existing behavior is acceptable.

**Step 2: Add a guard so cached crossfade state doesn't leak into uncached path**

In the `stopReadingFromCache()` function, reset crossfade state:

```cpp
crossfadeActive = false;
crossfadeCacheBytePos = 0;
```

**Step 3: Build**

Run: `timeout 300 ./dbt build`

**Step 4: Commit**

```
fix: reset crossfade state when falling back from cache to uncached
```

---

### Task 6: Clamp crossfade to loop length and clean up old code

**Files:**
- Modify: `src/deluge/model/voice/voice.cpp:2851-2858` (crossfade setup)
- Modify: `src/deluge/model/voice/voice_sample.cpp` (remove old fade-only code)

**Step 1: Clamp crossfade to half the loop length in voice.cpp**

At line 2854 where `loopFadeInSamplesTotal` is computed, add clamping. We need the loop length in output samples. The sample rate and loop point positions are available via the holder and sample.

```cpp
if (loopingType != LoopType::NONE) {
    auto* holder = static_cast<SampleHolderForVoice*>(guides[s].audioFileHolder);
    int32_t crossfadeSamples = (holder->loopCrossfadeMs * sample->sampleRate) / 1000;

    // Clamp to half the loop length to prevent crossfade exceeding loop
    if (holder->loopEndPos > 0 && holder->loopEndPos > holder->loopStartPos) {
        int32_t loopLengthSamples = holder->loopEndPos - holder->loopStartPos;
        crossfadeSamples = std::min(crossfadeSamples, loopLengthSamples / 2);
    }

    voiceSample->loopFadeInSamplesTotal = crossfadeSamples;
}
```

**Step 2: Remove old fade-out code from cached path**

The old `else if (loopFadeInSamplesTotal > 0 && loopingType != LoopType::NONE)` block at ~line 845 that did the pre-boundary fade-out is no longer needed — the true crossfade replaces it. Remove it.

Keep the old fade-in block as a fallback for when `crossfadeActive` is false (e.g., if a note starts mid-loop within the crossfade zone).

**Step 3: Build and test**

Run: `timeout 300 ./dbt build`

**Step 4: Commit**

```
fix: clamp crossfade to half loop length, remove old fade-out
```

---

### Task 7: Final cleanup and integration test

**Files:**
- All modified files

**Step 1: Run format**

Run: `./dbt format`

**Step 2: Full build**

Run: `timeout 300 ./dbt build`

**Step 3: Hardware test checklist**

- [ ] Load a sample with loop points, set crossfade to 0 — should loop with no crossfade (click at boundary is expected)
- [ ] Set crossfade to 100ms — loop should be smooth, no volume dip
- [ ] Set crossfade to 500ms — longer blend, should sound natural
- [ ] Set crossfade to 1000ms — maximum blend
- [ ] Try with very short loops (crossfade should auto-clamp)
- [ ] Try with pingpong mode — crossfade should NOT activate, falls back to old behavior
- [ ] Multisample instrument — crossfade should work on all note zones (fixed in earlier commit)
- [ ] Note release during crossfade — should fade out cleanly

**Step 4: Commit**

```
chore: format and finalize true loop crossfade
```
