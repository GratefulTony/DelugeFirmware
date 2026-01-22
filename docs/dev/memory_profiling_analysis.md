# Memory and CPU Profiling Analysis

This document summarizes findings from memory and CPU profiling of Deluge effects, with focus on identifying optimization opportunities for the `feat/featherverb` branch.

## Methodology

### Profiling Infrastructure

Memory profiling was performed using a custom SYSEX-based system:

1. **Memory Stats Query**: SYSEX command `F0 00 21 7B 01 03 03 00 F7` triggers memory region stats output
2. **Collection Tool**: `scripts/tasks/task-memory-profile.py` polls the Deluge every 5 seconds
3. **Build Requirement**: Requires `ENABLE_MEMORY_PROFILE=YES` and `ENABLE_TEXT_OUTPUT` (use `relwithdebinfo` or `debug` build)

### Memory Regions

The Deluge has several memory regions with different characteristics:

| Region | Size | Speed | Purpose |
|--------|------|-------|---------|
| **internal** | ~976 KB | Fast | CPU-local SRAM, filter states, small buffers |
| **external** | ~2.1 MB | Medium | External SRAM, medium allocations |
| **stealable** | ~64 MB | Slower | Main SDRAM, large buffers, sample data |
| **ext_small** | ~131 KB | Medium | Small external allocations |
| **int_small** | ~110 KB | Fast | Small internal allocations |

### Allocation Functions

```cpp
allocMaxSpeed()   // internal → external → stealable (prefers fast memory)
allocLowSpeed()   // external → stealable (avoids internal)
allocStealable()  // stealable only, marked as stealable (can be reclaimed)
allocSdram()      // stealable only, marked as allocated (won't be reclaimed)
```

The `allocSdram()` function was added to support large effect buffers that:
- Are too big for internal/external regions
- Should not be stealable (effect state must persist)
- Benefit from going directly to SDRAM without trying smaller regions first

### Test Setup

- Single synth voice playing a repeating pattern
- Effects enabled one at a time, cumulatively
- Memory delta measured between each effect enable
- CPU measured via task scheduler stats (working time %)

## Memory Findings

### Dynamic Allocators (Memory Used Only When Enabled)

| Effect | SDRAM Used | Allocations | Status |
|--------|-----------|-------------|--------|
| **Grain (Mod FX)** | **4.2 MB** | 2 | Embedded - always allocated |
| **Stutter** | **3.0 MB** | 3 | Uses `allocSdram()` |
| **Disperser** | **72 KB** | 2 | Uses `allocSdram()` (new) |
| **Table Shaper** | 12 KB | 3 | External region |

### Embedded State (Always Allocated Per Sound)

These effects have state embedded in the `ModControllableAudio` class, meaning memory is allocated for every sound whether the effect is enabled or not:

| Effect | Estimated Size | Notes |
|--------|---------------|-------|
| Saturator | < 1 KB | Filter coefficients |
| Filters (LPF/HPF) | < 1 KB | Filter state |
| Disperser (before fix) | **70 KB** | Delay buffers - now dynamic |
| Grain buffers | **4.2 MB** | Currently embedded |

### Song-Level Effects

| Effect | Location | Notes |
|--------|----------|-------|
| **Reverb** | Song | Buffer allocated via allocMaxSpeed(), only active model allocated |
| Delay | Song | Send-based, always running |

## CPU Findings

### Important: L1 D-Cache Contention

**CPU measurements vary dramatically based on what synth feeds an effect, not the effect itself.**

The ARM Cortex-A9 has a 32 KB L1 data cache. Complex synths (DX7/FM) have large working sets that evict effect data from cache, causing ~1.8× more CPU usage for the *same* effect code.

See [memory_architecture.md](memory_architecture.md) for detailed analysis.

### Task Scheduler Estimates (Rough)

These percentages are from task scheduler working time stats, which are approximate and affected by cache contention:

Baseline: Simple synth @ **28% CPU**

| Effect | CPU Delta | Cumulative | Notes |
|--------|-----------|------------|-------|
| Sine Shaper | +12% | 40% | Pure DSP, no memory |
| Table Shaper | ~0% | 40% | Lookup table, minimal CPU |
| Disperser (8 stages) | +6% | 46% | Allpass cascade |
| Saturator | +6% | 52% | Filter processing |
| Reverb send | ~0% | 52% | Already running at song level |
| Grain | +6.5% | 60% | Granular processing |
| Stutter | +0.6% | 61% | Standby mode, minimal |
| Dott | +3% | 64% | Pure DSP |

### Microbenchmark Data (FX Benchmark System)

Cycle counts from the `FxBenchmark` system under different load conditions. **Note**: These figures vary significantly based on system load due to L1 D-cache contention—the 32KB data cache is shared between synths and effects. Expect ~2-3× higher cycles under heavy load compared to quiet conditions.

| Effect | Quiet (DX7 only) | Full Load (all FX) | Ratio |
|--------|------------------|-------------------|-------|
| **Mutable** | 10.7k | 27.5k | 2.6× |
| **Featherverb** | 11.4k | 27.7k | 2.4× |
| **Digital** | 14.3k | 38.9k | 2.7× |
| **Freeverb** | 15.4k | 43.4k | 2.8× |

**Key findings**:
- Featherverb and Mutable are essentially tied across all load conditions
- All reverbs scale ~2.5× from quiet to full load (cache pressure from other effects)
- Freeverb is 44% more expensive than Mutable/Featherverb (8-comb architecture)
- BSS vs dynamic allocation makes <1% difference because both go through the same D-cache

## Cost Comparison: New vs Traditional Effects

Understanding the relative cost of new community effects versus traditional Deluge effects helps contextualize optimization priorities.

### Memory Cost Comparison

| Effect | Memory | Type | Era | Context |
|--------|--------|------|-----|---------|
| **Grain** | 4.2 MB | Per-sound | New | ~33× larger than reverb buffer |
| **Stutter** | 3.0 MB | Per-sound | New | Looper buffers for beat repeat |
| **Mutable Reverb** | ~128 KB | Song-level | Original | Now dynamic via allocMaxSpeed() |
| **Freeverb** | ~93 KB | Song-level | Original | Now dynamic via allocMaxSpeed() |
| **Featherverb** | ~77 KB | Song-level | New | Dynamic via allocMaxSpeed() |
| **Disperser** | 72 KB | Per-sound | New | Comparable to reverb, now dynamic |
| **Table Shaper** | 12 KB | Per-sound | New | Lookup table only |
| **Filters/Saturator** | < 1 KB | Per-sound | Original | Minimal state |
| **Sine Shaper** | 0 | Per-sound | New | Pure CPU, no buffers |

**Key insight**: The new granular/looping effects (Grain, Stutter) require 25-35× more memory than traditional effects like reverb. This is expected—they buffer audio for manipulation—but highlights why dynamic allocation matters.

**Reverb note**: All reverb models now use `allocMaxSpeed()` for dynamic allocation. Only the active model's buffer is allocated at any time, saving ~298 KB total when models are switched.

### CPU Cost Comparison

| Effect | Cycles (Quiet) | Cycles (Full Load) | Scaling |
|--------|----------------|-------------------|---------|
| **Freeverb** | 15.4k | 43.4k | 2.8× |
| **Digital** | 14.3k | 38.9k | 2.7× |
| **Featherverb** | 11.4k | 27.7k | 2.4× |
| **Mutable Reverb** | 10.7k | 27.5k | 2.6× |

Other effects (full load only):
- Disperser (8 stages): 25.7k
- Sine Shaper: 8.8k
- Multiband (DOTT): 4.9k
- Scatter: 3.9k
- Table Shaper: 2.0k

**Key insight**: Reverb CPU scales ~2.5× from quiet to full load due to L1 D-cache contention. Featherverb matches Mutable performance while using 40% less memory (77 KB vs 128 KB).

### Memory per Sound: The Hidden Cost

When effects embed buffers in `ModControllableAudio`, **every sound pays the cost** whether the effect is used or not:

| Scenario | Sounds | Disperser (old) | Disperser (new) | Savings |
|----------|--------|-----------------|-----------------|---------|
| Kit with 16 drums | 16 | 1.1 MB | 0 - 72 KB* | Up to 1.1 MB |
| Song with 8 synths | 8 | 560 KB | 0 - 72 KB* | Up to 560 KB |
| Complex song (32 sounds) | 32 | 2.2 MB | 0 - 72 KB* | Up to 2.2 MB |

*Only allocated for sounds actually using disperser.

This is why the disperser optimization matters: a 70 KB buffer embedded in every sound becomes megabytes of waste in complex songs.

## Key Insights

### 1. Disperser Dynamic Allocation (Implemented)

**Before**: 70 KB embedded in every `ModControllableAudio` instance
**After**: 72 KB allocated from SDRAM only when `stages > 0`

Implementation pattern:
```cpp
// In DisperserDelayState:
q31_t* bufferL{nullptr};  // Was: std::array<q31_t, 8820>
q31_t* bufferR{nullptr};  // Was: std::array<q31_t, 8820>

bool allocate() {
    bufferL = static_cast<q31_t*>(allocSdram(kBufferSizeBytes));
    // ... error handling, allocate bufferR
}

void deallocate() {
    if (bufferL) { delugeDealloc(bufferL); bufferL = nullptr; }
    // ... deallocate bufferR
}

// In DisperserParams:
bool setStages(uint8_t newStages) {
    if (newStages > 0 && !delay.isAllocated()) {
        if (!delay.allocate()) return false;
    } else if (newStages == 0 && delay.isAllocated()) {
        delay.deallocate();
    }
    stages_ = newStages;
    return true;
}
```

### 2. Stutter Uses allocSdram() (Previously Implemented)

Stutter's 3 MB buffers now go directly to SDRAM via `allocSdram()`, avoiding the "FULL external" warnings that occurred when `allocLowSpeed()` tried to fit 3 MB into the 2.1 MB external region.

### 3. Reverb Dynamic Allocation (Implemented)

**Before**: Static BSS buffers (128 KB Mutable, 93 KB Freeverb)
**After**: Dynamic allocation via `allocMaxSpeed()` - prefers fast SRAM, falls back to SDRAM

The reverb uses a `std::variant<Freeverb, Mutable, Featherverb>` where:
- Freeverb: ~93 KB (comb + allpass buffers)
- Mutable: ~128 KB (32768 float FxEngine buffer)
- Featherverb: ~77 KB (4-tap FDN delay lines)

**Allocation strategy**: All reverbs now use `allocMaxSpeed()`:
```cpp
// In all reverb models (Featherverb, Mutable, Freeverb):
bool allocate() {
    buffer_ = static_cast<float*>(
        GeneralMemoryAllocator::get().allocMaxSpeed(kBufferSizeBytes));
    // ... setup engine/filters
}
```

**Benchmarking showed <1% performance difference** between static BSS and dynamic allocation:
- Static BSS: 18,856 cycles (DX7 synth)
- Dynamic allocMaxSpeed: 18,993 cycles (DX7 synth)
- Difference: +0.7%

The ~1.8× CPU variation (10k vs 19k cycles) comes from L1 D-cache contention with the synth, not memory allocation strategy. See [memory_architecture.md](memory_architecture.md) for full analysis.

This frees ~298 KB (77+128+93) when reverbs are switched or disabled.

### 4. Grain is the Largest Per-Sound Allocator

At 4.2 MB per sound when enabled, Grain is larger than Stutter. However, Grain appears to allocate dynamically (memory increased when enabled), so it's already following the dynamic pattern. The buffers may benefit from using `allocSdram()` if they're currently trying external first.

## Recommendations

### Priority 1: Verify Grain Allocation Path

- Confirm Grain uses appropriate allocation function
- If using `allocLowSpeed()`, may cause "FULL external" on enable
- Consider `allocSdram()` for the 4.2 MB buffer

### Priority 2: Profile Additional Mod FX

- Chorus, Flanger, Phaser - unknown memory footprint
- May have similar embedded buffer issues

## References

- **[Memory Architecture](memory_architecture.md)** - Detailed analysis of L1 D-cache contention, BSS vs dynamic allocation benchmarks
- Memory profiler: `scripts/tasks/task-memory-profile.py`
- allocMaxSpeed/allocSdram: `src/deluge/memory/memory_allocator_interface.cpp`
- Disperser dynamic allocation: `src/deluge/dsp/disperser.h`
- Reverb allocation: `src/deluge/dsp/reverb/mutable.hpp`, `src/deluge/dsp/reverb/freeverb/freeverb.hpp`, `src/deluge/dsp/reverb/featherverb.hpp`
- Stutter allocation: `src/deluge/model/fx/stutterer.cpp`
- FX Benchmark system: `src/deluge/io/debug/fx_benchmark.h`
