# True Loop Crossfade Design

## Problem

The current loop crossfade implementation does a sequential fade-out then fade-in at
loop boundaries. This creates a volume dip rather than a true crossfade. We need
simultaneous dual reads that blend loop-end and loop-start audio together.

## Approach: Pre-loop-end crossfade with second cache read

When the main read head is within `crossfadeMs` of the loop end, a second read head
starts playing from the loop start. The main head fades out while the crossfade head
fades in. By the time we reach the loop boundary, we're 100% on the crossfade head —
it becomes the new main head and playback continues with zero discontinuity.

## New state in VoiceSample

```cpp
int32_t crossfadeCacheBytePos{0};    // second reader position in cache
bool crossfadeActive{false};          // currently in crossfade region
```

Existing `loopFadeInSamplesRemaining` and `loopFadeInSamplesTotal` are reused to track
crossfade progress.

## Cached read path (forward loop)

1. **Enter crossfade:** When `bytesTilLoopEndPoint <= crossfadeLengthBytes`, set
   `crossfadeActive = true`, initialize `crossfadeCacheBytePos = cacheLoopStartPointBytes`,
   set `loopFadeInSamplesRemaining = loopFadeInSamplesTotal`.

2. **During crossfade:** Each render cycle reads from both positions into the output buffer:
   - Main read: amplitude scaled by `(1 - fadeProgress)` — fading out
   - Crossfade read: amplitude scaled by `fadeProgress` — fading in
   - `crossfadeCacheBytePos` advances in sync with `cacheBytePos`

3. **Exit crossfade:** When main head reaches loop end:
   - `cacheBytePos = crossfadeCacheBytePos` (crossfade head becomes main)
   - `crossfadeActive = false`
   - No discontinuity — already 100% on the crossfade audio

4. **Render loop change:** The inner loop gets a second cache cluster lookup and blended
   write when `crossfadeActive`. Uses the same `multiply_accumulate_32x32_rshift32_rounded`
   pattern with complementary amplitude envelope.

## Time-stretch path

Same concept applied to the non-cached render path. When within crossfade distance of the
loop end, start a secondary read from loop start. Handled separately from TimeStretcher's
own dual-head infrastructure (which manages time-stretch hops, not loop boundaries).

## Edge cases

- **Crossfade longer than loop:** Clamp crossfade length to half the loop length.
- **Note release during crossfade:** Amplitude envelope applies to both reads.
- **Cache not ready:** Fall back to current fade-out/fade-in behavior.
- **Pingpong:** Not supported initially — crossfade only for forward loop mode.

## What doesn't change

- `loopCrossfadeMs` parameter, menu item, serialization
- `loopFadeInSamplesTotal` calculation in voice.cpp
- No new memory allocations — both reads hit the existing cache

## Key files

- `src/deluge/model/voice/voice_sample.h` — new state fields
- `src/deluge/model/voice/voice_sample.cpp` — dual-read logic in cached and uncached paths
- `src/deluge/model/voice/voice.cpp` — crossfade setup during voice render
