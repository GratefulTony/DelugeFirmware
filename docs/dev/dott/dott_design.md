# DOTT Multiband Compressor - Design & Optimization Notes

## Overview

DOTT (Deluge OTT) is a 3-band multiband compressor with OTT-style upward/downward compression. It's designed to run efficiently on the Deluge's ARM Cortex-A9 processor while providing musical dynamics control.

## DSP Architecture

### Signal Flow

```
Input -> Crossover Filter -> [Low Band]  -> Compressor -> Gain ->
                            [Mid Band]  -> Compressor -> Gain -> Recombine -> Output
                            [High Band] -> Compressor -> Gain ->
```

### Crossover Filters

Five crossover types are available, ordered by CPU cost:

| Type | Name | Slope | Ops/Channel | Notes |
|------|------|-------|-------------|-------|
| 0 | AP 6dB | 6dB/oct | 2 | Cheapest, gentle slopes, perfect reconstruction |
| 1 | AP 12dB | 12dB/oct | 4 | Steeper slopes, still perfect reconstruction |
| 2 | AP 18dB | 18dB/oct | 6 | Steep slopes, experimental |
| 3 | LR2 Fast | 12dB/oct | 4 | Linkwitz-Riley, no phase compensation |
| 4 | LR2 | 12dB/oct | 6 | Linkwitz-Riley, with phase compensation |

#### Allpass Subtraction Crossover

The allpass crossover uses the formula:
- LP = (input + allpass) / 2
- HP = (input - allpass) / 2

**First Order (6dB/oct)**: Guarantees **perfect reconstruction**: LOW + MID + HIGH = input exactly, with zero phase distortion. The cost is gentle slopes.

**Higher Orders (12dB/18dB/oct)**: These are **experimental** and technically not true crossovers. Cascading allpass filters for steeper slopes introduces peaking and notching in the frequency response around crossover frequencies. The bands no longer sum to a flat response.

However, for a dynamics processor like DOTT, this can be **musically interesting**:
- The frequency response coloration adds character
- The phase relationships between bands become more complex
- Combined with the OWLTT zone's oscillating parameters, creates unique textures

Think of the higher-order allpass modes as "broken in a fun way" - they violate audio engineering best practices but can produce creative results for sound design.

#### Linkwitz-Riley Crossover (LR2)

LR2 uses cascaded first-order Butterworth filters for 12dB/oct slopes:
- Steeper band separation than allpass
- Power-complementary (-6dB at crossover)
- Flat summed magnitude response

**Phase Compensation**: The low band passes through fewer filter stages than mid/high, causing phase misalignment. The "LR2 Full" mode adds allpass filters to the low band to match. "LR2 Fast" skips this for ~33% CPU savings in the crossover stage.

### Band Compression

Each band uses an OTT-style compressor with:
- **Downward compression**: Reduces level above threshold
- **Upward compression**: Boosts level below threshold
- **Soft knee**: Smooth transition around thresholds
- **Attack/Release**: Envelope follower with per-band timing

The envelope follower uses IIR smoothing with response-based stride for efficiency:
- Response 0.0 (tight): ~2ms time constant, stride=2
- Response 1.0 (punchy): ~145ms time constant, stride=32

## Optimizations Applied

### 1. Gain + Recombine Loop Fusion

**Before**: Two separate loops
```cpp
// Loop 1: Apply gain to each band
for (each band) { bandBuffer *= gain; }
// Loop 2: Sum bands
for (each sample) { output = sum(bands); }
```

**After**: Single fused loop
```cpp
// Pre-compute combined gain
bandCombinedGain[b] = compression_gain * output_level;
// Single loop: sum bands with gain applied inline
for (each sample) { output = sum(band[b] * bandCombinedGain[b]); }
```

**Savings**: Eliminates one full pass over band buffers (~3 x 128 samples x 2 channels). Estimated 10-15% of render() time.

### 2. fastExp in Attack/Release Calculation

**Before**: `std::exp()` for envelope time constant
**After**: `fastExp()` polynomial approximation

```cpp
attackMS_ = 0.5f + (fastExp(2.0f * float(attack) / ONE_Q31f) - 1.0f) * 15.0f;
```

**Savings**: ~20 cycles per setAttack/setRelease call (rare, only on parameter change).

### 3. Character Zone Caching

**Problem**: `setCharacter()` does expensive zone calculations even when value unchanged.

**Solution**: Cache flag with early return
```cpp
void setCharacter(q31_t c) {
    if (c == characterKnob_ && characterComputed_) {
        return;  // Skip recalculation
    }
    characterComputed_ = true;
    // ... expensive calculations
}
```

**Note**: Cache is invalidated by `setVibe()` since OWLTT zone depends on vibe phases.

**Savings**: Significant when parameters are static (most of playback time).

### 4. Metering Uses fastLog

**Before**: `20.0f * std::log10(peak)` for dB conversion
**After**: `8.686f * fastLog(peak)` (since 20/ln(10) ≈ 8.686)

**Savings**: Minor per-call, but metering runs every buffer when analyzer is on.

### 5. LR2 Fast Mode (No Phase Compensation)

**Before**: LR2 crossover with 6 filter ops per channel
**After**: LR2 Fast with 4 filter ops per channel (skip phase compensation allpasses)

```cpp
template <bool PHASE_COMPENSATED = true>
class LR2Crossover {
    // Conditional state - saves memory when not used
    std::conditional_t<PHASE_COMPENSATED, BasicFilterComponent, char[0]> apComp1;

    // Zero-overhead compile-time branching
    if constexpr (PHASE_COMPENSATED) {
        low = apComp2.doAPF(apComp1.doAPF(lowRaw, coeff), coeff);
    } else {
        low = lowRaw;
    }
};
```

**Trade-off**: ~90 degree phase lead in low band at high crossover frequency. Inaudible for dynamics processing.

**Savings**: 33% reduction in crossover CPU cost.

### 6. Response-Based Stride (Level Detection)

**Before**: Process every sample for level detection
**After**: Adaptive stride based on response setting

```cpp
const size_t stride = 2 + static_cast<size_t>(response * 30.0f);
for (size_t i = 0; i < numSamples; i += stride) {
    // Level detection
}
```

**Rationale**: Punchy settings (~145ms time constant) don't need sample-accurate detection. Stride up to 32 samples (~0.73ms at 44.1kHz) is sufficient.

**Savings**: 10-15% of level detection time at high response settings.

## Fixed-Point Arithmetic

The DSP uses Q31 fixed-point for audio samples:
- Range: -2147483648 to 2147483647 representing -1.0 to ~1.0
- Multiplication uses ARM SMMUL instruction via `multiply_32x32_rshift32()`
- Gain application uses 4.28 format for headroom

```cpp
// 4.28 format: 4 bits integer, 28 bits fraction
q31_t gainFixed = static_cast<q31_t>(linearGain * (1 << 28));
q31_t result = multiply_32x32_rshift32(sample, gainFixed) << 4;
```

## CPU Cost Estimates

Per 128-sample buffer at 44.1kHz (2.9ms). These are **rough estimates** - actual profiling recommended.

| Stage | Notes |
|-------|-------|
| Crossover (AP 6dB) | Cheapest: 2 filter ops/ch x 2ch x 128 = 512 filter operations |
| Crossover (LR2 Full) | Most expensive: 6 filter ops/ch x 2ch x 128 = 1536 filter operations |
| Level Detection | Loops over samples with abs/max. Stride reduces iterations but still touches memory |
| Gain Calculation | 3x fastExp + envelope math per band |
| Recombine + Gain | Memory-bound: 6 band buffer reads + 2 output writes per sample |
| Metering (optional) | Peak tracking + fastLog for dB conversion |

**Key insight**: The recombine loop is memory-bound, not compute-bound. It reads from 6 band buffers (3 bands x 2 channels) and writes to the output buffer. At 128 samples, that's ~1KB of data touched per buffer.

**Level detection cost**: Despite stride optimization, it still iterates over band buffers to find peaks. The cost scales with buffer size and stride setting.

Use `MULTIBAND_PROFILE 1` in multiband.h to get actual cycle counts on hardware.

## Planned: Single-Pass Architecture

The current architecture uses intermediate band buffers (~3KB per instance). A single-pass design eliminates these buffers by fusing crossover, gain application, and output accumulation into one loop.

### Design

```cpp
#ifndef DOTT_SUB_BUFFER_SIZE
#define DOTT_SUB_BUFFER_SIZE 44  // ~1ms at 44.1kHz
#endif

template <size_t SubBufferSize = DOTT_SUB_BUFFER_SIZE>
void render(std::span<StereoSample> buffer) {
    for (size_t seg = 0; seg < buffer.size(); seg += SubBufferSize) {
        // Calculate gain from previous segment's level
        calculateGains();  // Uses prevSegmentPeak_

        q31_t segmentPeak[3] = {0, 0, 0};

        for (size_t i = seg; i < segEnd; ++i) {
            // Crossover (same as before)
            auto [low, mid, high] = crossover.process(buffer[i]);

            // Apply gain and accumulate directly to output
            buffer[i].l = low.l * gain[0] + mid.l * gain[1] + high.l * gain[2];
            buffer[i].r = low.r * gain[0] + mid.r * gain[1] + high.r * gain[2];

            // Track peaks inline for next segment
            segmentPeak[0] = max(segmentPeak[0], abs(low));
            segmentPeak[1] = max(segmentPeak[1], abs(mid));
            segmentPeak[2] = max(segmentPeak[2], abs(high));
        }

        prevSegmentPeak_ = segmentPeak;
    }
}
```

### Trade-offs

| Sub-Buffer Size | Latency | Segments/Buffer | CPU Savings | Notes |
|-----------------|---------|-----------------|-------------|-------|
| 128 (full) | 2.9ms | 1 | ~30-40% | Maximum savings, highest latency |
| 64 | 1.45ms | 2 | ~25-35% | Good balance |
| **44 (default)** | **1.0ms** | **3** | **~20-30%** | **Sweet spot** |
| 32 | 0.73ms | 4 | ~15-25% | Sub-ms response |

### Why 44 samples (1ms) is the sweet spot

- **1ms latency** is imperceptible for dynamics processing (typical attack times: 1-100ms)
- **3 segments per buffer** = only 3x gain calculations, minimal overhead
- **Full memory savings**: eliminates 3KB band buffers per instance
- **10 instances** becomes comfortable with 30KB less L1 cache pressure
- **No need for transient detection** since 1ms is faster than most musical transients

### Memory Operations Eliminated

| Operation | Ops Saved |
|-----------|-----------|
| Band buffer writes (crossover) | 768 stores (6 × 128) |
| Band buffer reads (recombine) | 768 loads (6 × 128) |
| Band buffer reads (level detect) | ~384 loads (with stride) |
| **Total** | **~1920 memory ops/buffer** |

The ~1536 ALU ops added for inline peak tracking are single-cycle on ARM. The memory bandwidth savings dominate.

### Compile-Time Configuration

```bash
# Build with custom sub-buffer size
cmake -DDOTT_SUB_BUFFER_SIZE=32 ..  # 0.73ms latency
cmake -DDOTT_SUB_BUFFER_SIZE=64 ..  # 1.45ms latency
```

## Other Future Opportunities

1. **NEON SIMD**: Band buffers could use vector operations, but filter state dependency limits gains. Estimated 10-30% savings with high implementation effort.

2. **Wet/Dry Blend Optimization**: Currently uses float math; could use fixed-point.

3. **Level Detection**: Could potentially combine L/R channels earlier or use SIMD for peak finding.

Note: Non-rounded multiply was evaluated - both SMMUL and SMMULR are single-cycle on ARMv7, no savings. Metering now uses fastLog.

## References

- Linkwitz-Riley crossovers: https://www.linkwitzlab.com/crossovers.htm
- Allpass crossover theory: "Complementary Allpass Crossovers" by Lipshitz & Vanderkooy
- OTT compression: Xfer Records OTT plugin documentation
