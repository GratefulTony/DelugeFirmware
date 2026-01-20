# Reverb SDRAM Migration Baseline

**Date:** 2026-01-20
**Branch:** feat/featherverb
**Build:** relwithdebinfo with ENABLE_FX_BENCHMARK + ENABLE_MEMORY_PROFILE

## Summary

Moved reverb buffers from BSS (static SRAM) to dynamically allocated SDRAM. Added Featherverb as a new lightweight FDN reverb model.

**BSS Savings:** ~127.5 KB (364,936 -> 234,376 bytes in release build)

## Benchmark Results (cycles per audio buffer)

| Model | Avg Cycles | Min | Max | Samples | Notes |
|-------|-----------|-----|-----|---------|-------|
| **Featherverb** | 8,951 | 4,898 | 13,646 | 70 | New default, fastest, lightest memory |
| Mutable | 13,779 | 8,225 | 23,253 | 52 | 1.5x slower than Featherverb |
| Digital | 26,358 | 15,928 | 33,436 | 21 | 2.9x slower than Featherverb |
| Freeverb | 27,157 | 14,280 | 39,469 | 38 | 3.0x slower, occasional glitches |

### Observations

1. **Featherverb** is the new default and fastest model - 4-tap FDN with Hadamard mixing, uses ~68KB buffer
2. **Mutable** performs well with SDRAM - uses sequential buffer access via FxEngine
3. **Freeverb** shows higher latency and variance - accesses 24 separate delay lines (8 combs + 4 allpasses x 2 channels) with scattered memory access patterns
4. **Digital** inherits Mutable's buffer but has more complex topology
5. Minor audio glitches observed on Freeverb and Digital during testing - correlates with high-cycle spikes

## Memory Profile

| Region | Used | Total | % Used |
|--------|------|-------|--------|
| SDRAM (stealable) | 207 KB | 64 MB | 0.3% |
| Internal | 179 KB | 1.1 MB | 16.3% |
| External | 172 KB | 2.1 MB | 8.2% |

Reverb buffer allocation (~68-128 KB depending on model) confirmed in SDRAM stealable region.

### Stealable Region During Model Switching

Observed during a session switching between all 4 reverb models:
- Featherverb → Freeverb: +24,584 bytes used (freeverb larger buffer)
- Sample cache eviction mid-session reduced used memory from ~5.4MB to ~1.5MB
- Allocation count increased from 3 to 12 over session (normal fragmentation)

## Buffer Sizes

- **Featherverb:** ~68 KB (17,432 bytes delay buffer + predelay + diffuser)
  - 4 FDN delay lines: 887 + 1021 + 1153 + 1297 samples
  - 3-zone predelay buffer: up to ~50KB depending on zone sizes
  - Hadamard feedback matrix (no extra memory)

- **Freeverb:** 93,800 bytes (23,450 samples x 4 bytes)
  - 8 comb filters L/R: 10,024 + 10,208 samples
  - 4 allpass filters L/R: 1,563 + 1,655 samples

- **Mutable/Digital:** ~128 KB float buffer via FxEngine

## Known Issues

- Very rapid model switching can still cause crashes (TODO in reverb.hpp)
- Freeverb glitches may be inherent to algorithm's scattered memory access

## Files Modified

- `src/deluge/dsp/reverb/reverb.hpp` - Deferred model switching, std::visit for thread safety, Featherverb param persistence
- `src/deluge/dsp/reverb/featherverb.hpp` - New lightweight 4-tap FDN reverb with Hadamard matrix
- `src/deluge/dsp/reverb/mutable.hpp` - Dynamic allocation via allocSdram()
- `src/deluge/dsp/reverb/digital.hpp` - Added hasBuffer() safety check
- `src/deluge/dsp/reverb/freeverb/freeverb.hpp` - Dynamic allocation interface
- `src/deluge/dsp/reverb/freeverb/freeverb.cpp` - Buffer allocation implementation
- `src/deluge/dsp/reverb/freeverb/comb.hpp` - Empty buffer safety, bufidx reset
- `src/deluge/dsp/reverb/freeverb/allpass.hpp` - Empty buffer safety, bufidx reset
- `src/deluge/processing/engines/audio_engine.cpp` - Reverb allocation in init(), benchmark tags
- `src/deluge/gui/l10n/strings.h` - Added STRING_FOR_FEATHERVERB
- `src/deluge/gui/l10n/built_in/g_english.cpp` - Added "Feather" string
- `src/deluge/gui/menu_item/reverb/model.h` - Added Featherverb to model selection

## Raw Data

See `reverb_benchmark.csv` and `memory_stats.csv` in this directory.
