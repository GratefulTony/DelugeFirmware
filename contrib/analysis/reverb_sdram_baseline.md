# Reverb SDRAM Migration Baseline

**Date:** 2026-01-19
**Branch:** feat/featherverb
**Build:** relwithdebinfo with ENABLE_FX_BENCHMARK + ENABLE_MEMORY_PROFILE

## Summary

Moved reverb buffers from BSS (static SRAM) to dynamically allocated SDRAM.

**BSS Savings:** ~127.5 KB (364,936 -> 234,376 bytes in release build)

## Benchmark Results (cycles per audio buffer)

| Model | Avg Cycles | Min | Max | Samples | Notes |
|-------|-----------|-----|-----|---------|-------|
| Mutable | 12,977 | 5,366 | 28,192 | 53 | Fastest, most stable |
| Freeverb | 28,220 | 19,929 | 40,355 | 31 | 2.2x slower, occasional glitches |
| Digital | 26,568 | 23,605 | 29,532 | 2 | Similar to Freeverb |

### Observations

1. **Mutable** performs best with SDRAM - uses sequential buffer access via FxEngine
2. **Freeverb** shows higher latency and variance - accesses 24 separate delay lines (8 combs + 4 allpasses x 2 channels) with scattered memory access patterns
3. **Digital** inherits Mutable's buffer but has more complex topology
4. Minor audio glitches observed on Freeverb and Digital during testing - correlates with high-cycle spikes

## Memory Profile

| Region | Used | Total | % Used |
|--------|------|-------|--------|
| SDRAM (stealable) | 207 KB | 64 MB | 0.3% |
| Internal | 179 KB | 1.1 MB | 16.3% |
| External | 172 KB | 2.1 MB | 8.2% |

Reverb buffer allocation (~93-128 KB depending on model) confirmed in SDRAM stealable region.

## Buffer Sizes

- **Freeverb:** 93,800 bytes (23,450 samples x 4 bytes)
  - 8 comb filters L/R: 10,024 + 10,208 samples
  - 4 allpass filters L/R: 1,563 + 1,655 samples

- **Mutable/Digital:** ~128 KB float buffer via FxEngine

## Known Issues

- Very rapid model switching can still cause crashes (TODO in reverb.hpp)
- Freeverb glitches may be inherent to algorithm's scattered memory access

## Files Modified

- `src/deluge/dsp/reverb/reverb.hpp` - Deferred model switching, std::visit for thread safety
- `src/deluge/dsp/reverb/mutable.hpp` - Dynamic allocation via allocSdram()
- `src/deluge/dsp/reverb/digital.hpp` - Added hasBuffer() safety check
- `src/deluge/dsp/reverb/freeverb/freeverb.hpp` - Dynamic allocation interface
- `src/deluge/dsp/reverb/freeverb/freeverb.cpp` - Buffer allocation implementation
- `src/deluge/dsp/reverb/freeverb/comb.hpp` - Empty buffer safety, bufidx reset
- `src/deluge/dsp/reverb/freeverb/allpass.hpp` - Empty buffer safety, bufidx reset
- `src/deluge/processing/engines/audio_engine.cpp` - Reverb allocation in init(), benchmark tags

## Raw Data

See `reverb_benchmark.csv` and `memory_stats.csv` in this directory.
