# Memory Architecture and Optimization Analysis

**"AFAICT"**

This document covers the Deluge's memory architecture, allocation strategies, and performance tradeoffs discovered during Featherverb development.

## Memory Regions Overview

The Deluge RZ/A1 SoC has multiple memory regions with different performance characteristics:

| Region | Size | Speed | Use Case |
|--------|------|-------|----------|
| **Internal SRAM** | ~1 MB | Fastest | L1 cache-resident, filter states, hot paths |
| **External SRAM** | ~2.1 MB | Fast | Medium allocations, less latency-sensitive |
| **SDRAM (Stealable)** | ~64 MB | Slowest | Large buffers, samples, reclaimable data |

### Memory Addresses

```
0x20000000 - 0x200FFFFF : Internal SRAM (~1MB, BSS lives here)
0x0C000000 - 0x0C0FFFFF : External SRAM (memory-mapped)
0x60000000+            : SDRAM (main memory)
```

## Why Text Section Doesn't Affect Runtime Performance

The `.text` section contains executable code (firmware instructions). It does **not** compete with data for SRAM because:

1. **Code lives in Flash/ROM**: The `.text` section is stored in non-volatile memory and loaded into the instruction cache (I-cache), not SRAM.

2. **Separate cache hierarchies**: ARM Cortex-A9 has separate L1 instruction and data caches:
   - **L1 I-cache**: 32 KB for instructions
   - **L1 D-cache**: 32 KB for data

   Code execution uses I-cache; data access uses D-cache. They don't compete.

3. **No SRAM pressure**: Larger firmware doesn't reduce available SRAM for runtime data. The only constraint is Flash capacity (~4 MB on Deluge OLED).

4. **Startup time, not runtime**: Larger text sections may marginally increase boot time (loading from Flash), but have no impact on audio processing performance.

**Key insight**: A 200 KB increase in code size has zero impact on CPU cycles during audio rendering. Only data placement matters for performance.

## BSS Section: The Critical Memory

BSS (Block Started by Symbol) is zero-initialized static data. On Deluge:
- Lives in **Internal SRAM** (fastest memory)
- Limited to ~350-400 KB available
- **Every byte counts** for performance-critical data

### Current BSS Breakdown (owlet-firmware)

| Size (KB) | Symbol | Purpose |
|-----------|--------|---------|
| 25.3 | `connectedUSBMIDIDevices` | USB MIDI device state |
| 16.0 | `ssiRxBuffer` | Audio DMA receive buffer |
| 12.5 | `english` (l10n) | English language strings |
| 12.5 | `seven_segment` (l10n) | 7-segment display strings |
| 8.0 | `emptySpacesMemoryInternal` | Memory allocator bookkeeping |
| 4.7 | `sampleForPreviewMemory` | Sample preview buffer |
| 3.5 | `FxBenchGlobal::pendingResults` | Debug benchmarking (dev only*) |
| ... | (many smaller) | Various system state |

### BSS Changes: owlet-firmware vs community

| Change | Impact | Reason |
|--------|--------|--------|
| -128 KB | Mutable reverb buffer | Moved to dynamic allocation |
| -93 KB | Freeverb buffers | Moved to dynamic allocation |
| +77 KB | Featherverb buffer | Dynamic allocation from start |
| +3.5 KB | `FxBenchGlobal` | Debug instrumentation (disable for release) |
| **Net** | **~-200 KB** | owlet-firmware uses much less BSS (with benchmark disabled) |

## Reverb Memory Architecture

### The Cache Contention Problem

During Featherverb development, we discovered that CPU cycles varied dramatically based on which synth fed the reverb:

| Synth Type | Featherverb Cycles | Mutable Cycles |
|------------|-------------------|----------------|
| Subtractive | ~10,000 | ~9,500 |
| DX7 (FM) | ~18,000 | ~17,500 |

Both reverbs showed the same ~1.8× penalty with complex synths. This is **L1 D-cache contention**.

#### Why L1 D-cache (not allocation) Determines Performance

The ARM Cortex-A9 has two separate L1 caches:
- **L1 I-cache (32KB)**: Instructions/code - not contested
- **L1 D-cache (32KB)**: Data - this is where synth and reverb fight

Neither synth working set nor reverb buffer (77KB) fits in the 32KB D-cache. During each audio buffer:
1. DX7 runs → fills cache with operator state, envelopes, sine LUTs
2. Reverb runs → evicts DX7 data, loads delay buffer chunks
3. Next buffer → repeat, everything gets evicted

With subtractive synth, the working set is small enough that reverb data stays cached longer → fewer misses → ~10k cycles instead of ~19k.

**Key insight**: The ~1.8× penalty is from the *synth's* cache footprint, not the reverb's memory location. BSS vs dynamic allocation makes <1% difference because both go through the same D-cache

### Allocation Strategy Comparison

Featherverb benchmarked under DX7/FM synth (worst-case cache pressure):

| Strategy | Memory Used | Avg Cycles | Notes |
|----------|-------------|------------|-------|
| Static BSS | 77 KB BSS | 18,856 | Best but permanently reserves memory |
| `allocMaxSpeed()` | 0 KB BSS | 18,993 | **Recommended** - only +0.7% CPU |
| SDRAM-only | 0 KB BSS | ~24,000 | Significantly worse cache behavior |

**Key finding**: Dynamic allocation via `allocMaxSpeed()` has nearly identical performance to static BSS (+0.7%), but saves 77 KB when other reverb models are active.

### Why Dynamic Allocation (allocMaxSpeed) Wins

1. **Negligible performance difference**: Only 137 cycles more than static BSS (0.7%)
2. **Memory efficiency**: Buffer freed when switching to other reverb models
3. **Falls back gracefully**: Tries Internal SRAM → External SRAM → SDRAM
4. **Consistency**: All reverb models now use the same allocation pattern

### Caveat: Model Switching Under Load

The benchmark above was performed under typical conditions. When switching reverb models under heavy load (many sounds, effects active), `allocMaxSpeed()` may fall back to slower memory tiers:

| Scenario | Likely Allocation | Expected Performance |
|----------|-------------------|---------------------|
| Startup / light load | External SRAM | ~19k cycles (benchmarked) |
| Heavy load, fragmented | SDRAM fallback | ~24k cycles (baseline) |

**Mitigation strategies** (not currently implemented):
- Reserve fast memory at boot for reverb buffers
- Allocate reverb buffer once at startup, reuse across model switches
- Priority allocation system for song-level effects

**Current behavior**: Reverb deallocates and reallocates on every model switch. This is acceptable because:
1. Model switching is rare (user preference, not per-note)
2. Performance degradation is temporary (until next switch or restart)
3. Worst case is still the original SDRAM baseline performance

### The Previous Static BSS Approach

Static BSS allocation permanently reserves memory:
- **Pro**: Marginally faster (0.7% in benchmarks)
- **Pro**: No allocation failures possible
- **Con**: Memory unavailable even when reverb model changes
- **Con**: Increases firmware's static memory footprint

**Decision**: Given the negligible performance difference, all reverbs now use dynamic allocation via `allocMaxSpeed()`. This frees ~298 KB (77+128+93) when reverbs are switched or disabled.

## Recommendations

### For Song-Level Effects (Reverb, Master Compressor)

Use **`allocMaxSpeed()`** dynamic allocation:
- Tries fast memory first (Internal → External SRAM), falls back to SDRAM
- Frees memory when effect/model changes
- Benchmarking shows negligible performance difference vs static BSS (<1%)
- All three reverb models (Featherverb, Mutable, Freeverb) use this pattern

### For Per-Sound Effects (Disperser, Stutter, Grain)

Use **`allocSdram()`** directly:
- Only allocate when effect is enabled
- Many instances possible (per-sound × sounds in song)
- Don't attempt fast SRAM - it will likely fail for large buffers anyway
- Memory waste multiplies: 70 KB × 32 sounds = 2.2 MB

### Allocation Function Guide

```cpp
// For song-level effects (reverb, master compressor)
// Dynamic allocation with fast memory preference - recommended for all reverbs
buffer_ = static_cast<float*>(GeneralMemoryAllocator::get().allocMaxSpeed(size));

// For large, per-sound effects (disperser, grain, stutter)
// Use SDRAM directly - don't waste fast memory attempts
buffer_ = static_cast<float*>(GeneralMemoryAllocator::get().allocSdram(size));

// For small, per-sound state (filter coefficients)
// Internal SRAM via embedded struct members

// Static BSS - only if benchmarks prove significant benefit (unlikely)
// static inline std::array<float, kBufferSize> buffer_{};
```

## Build Configuration Notes

### Debug BSS in Release Builds

The `ENABLE_FX_BENCHMARK` flag adds ~3.5 KB BSS for cycle counting infrastructure. Check your CMake cache:

```bash
grep ENABLE_FX_BENCHMARK build/CMakeCache.txt
# Should show: ENABLE_FX_BENCHMARK:BOOL=OFF
```

To disable for clean release builds:
```bash
./dbt configure -DENABLE_FX_BENCHMARK=OFF
```

*Note: The BSS breakdown above was measured with benchmarking enabled. Production builds should have ~3.5 KB less BSS.

## Profiling Tools

### Memory Profiling
- SYSEX command: `F0 00 21 7B 01 03 03 00 F7`
- Script: `scripts/tasks/task-memory-profile.py`
- Build: `ENABLE_MEMORY_PROFILE=YES` (debug/relwithdebinfo)

### CPU Benchmarking
- FX benchmark system in `Debug::FxBenchGlobal`
- Measures per-effect cycles during audio rendering
- Analysis: `contrib/analysis/fx_benchmark_analysis.ipynb`

### Binary Analysis
```bash
# Section sizes
arm-none-eabi-size build/Release/deluge.elf

# Largest BSS symbols
arm-none-eabi-nm --size-sort -S build/Release/deluge.elf | grep ' [bBvV] ' | tail -30 | c++filt
```

## References

- [Memory Profiling Analysis](memory_profiling_analysis.md) - Detailed effect memory measurements
- [Reverb SDRAM Baseline](../../contrib/analysis/reverb_sdram_baseline.md) - Benchmark data
- ARM Cortex-A9 TRM - Cache architecture details
