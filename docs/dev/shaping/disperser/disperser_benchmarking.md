# Disperser Benchmarking and Optimization

## Current State (2026-01)

### Baseline Performance (commit `33213922`)

At base twist setting (no punch/chirp):
```
Stage   Median Cycles   CPU%
  1         3,182       0.27%
  3         4,547       0.39%
  5         5,965       0.51%
  7         7,772       0.67%
  9         9,263       0.80%
 11        13,685       1.18%
 13        15,696       1.35%
 17        19,720       1.70%
 21        23,756       2.05%
 25        27,788       2.40%
 29        31,842       2.75%
 32        43,097       3.72%
```

With twist in meta zone (punch/chirp active):
```
Stage   Median Cycles   CPU%
 16        23,112       1.99%
 32        46,475       4.01%
```

### Architecture Changes from Original

| Feature | Original (pre-zone) | Current (zone system) |
|---------|--------------------|-----------------------|
| Allpass order | 1st order | 2nd order biquad |
| Max stages | 16 | 32 |
| L/R processing | Parallel NEON int32x2 | Separate L/R coefficients |
| Topologies | 1 (cascade) | 7 (cascade, pingpong, cross, etc.) |
| Delay line | None | Fractional multi-tap writes |
| Transient detection | None | Per-sample envelope followers |
| Per-stage gains | None | Smoothed emphasis gains |

### Cost Breakdown (estimated per sample, 32 stages)

| Component | Cycles | Notes |
|-----------|--------|-------|
| Biquad allpass (×32) | ~800 | ~25 cycles/stage |
| Per-stage emphasis | ~200 | NEON multiply + shift |
| Topology routing | ~50 | Switch dispatch |
| Transient detection | ~80 | Dual envelope followers |
| Delay fractional write | ~120 | When punch/chirp > 0 |
| Coefficient update | ~amortized | Once per buffer |

## Known Regression

**Good state:** `33213922` - ~46k cycles at 32 stages with meta zone
**Bad state:** `18826f74` - 100k+ cycles reported

Commits between:
- `8f7b1f59` refactor(shaper): remove unused ADAA and phase warp code
- `f0abb9d1` perf(shaper): restore integer-only hot path for table shaper
- `4b5820e2` fix(serialization): add missing param serialization for effects
- `5e8d8619` refactor(shaper): move DC correction to table generation
- `57fbd61e` fix(shaper): improve amplitude-dependent blend
- `acf75bc7` fix(shaper): convert mix param to hybrid
- `ad07e76c` perf(shaper): optimize table shaper hot path to integer-only
- `6b8429f2` refactor(shaper): externalize wet-path modifiers and add slew limiting
- `fd0adc31` refactor(shaper): extract per-sample helper
- `4c332adf` perf(shaper): fold boost into drive
- `63e6b748` perf(shaper): apply drive before wet/dry split
- `a107b500` refactor(params): add automatic patched→unpatched fallback
- `18826f74` refactor(shaper): add osc harmonic weight

**Investigation needed:** None of these directly modify `disperser.h`. Regression may be in:
- Call site changes (`voice.cpp`, `global_effectable.cpp`)
- Param handling changes affecting zone computation
- Indirect effects from shaper changes if shared code paths

## Optimization Opportunities

### High Impact

1. **Skip punch/chirp when disabled**
   - Currently checks `punch > 0.01f` per sample
   - Could have separate fast path for pure cascade

2. **NEON vectorize cascade loop**
   - Current: scalar processing with NEON per-stage
   - Potential: batch 4 samples through each stage

3. **Precompute crossGain outside sample loop**
   - Currently computed per-sample in Cross topology

### Medium Impact

4. **Reduce topology dispatch overhead**
   - Function pointer table vs switch
   - Template specialization for common topologies

5. **Simplify transient detection when chirp=0**
   - Skip envelope followers entirely

6. **Cache stage offset calculations**
   - Triangle fold math is expensive
   - Only recalc when freq changes significantly

### Low Impact

7. **Use NEON for envelope followers**
8. **Align delay buffer for cache efficiency**

## Benchmarking Commands

```bash
# Build with benchmarks enabled
./dbt configure -DENABLE_FX_BENCHMARK=ON -DENABLE_SYSEX_LOAD=YES
./dbt build relwithdebinfo

# Collect data
./dbt fx-benchmark --collect -o fx_benchmark.csv

# Quick test (30 seconds)
./dbt fx-benchmark --collect -d 30 -o fx_benchmark.csv
```

## Test Configurations

For consistent benchmarking, use these test patches:

| Config | Topo | Twist | Stages | Expected Cycles |
|--------|------|-------|--------|-----------------|
| Baseline | Zone 0 (Cascade) | Zone 0 (base) | 32 | ~43k |
| Meta | Zone 0 | Zone 5+ (Meta) | 32 | ~46k |
| Cross | Zone 3 (Cross) | Zone 0 | 32 | ~45k |
| Spring | Zone 7 (Spring) | Zone 5+ | 32 | ~50k |

## Historical Comparison

### Pre-zone system (commit `2b058cb6`)
- 16 stages max
- ~4.3k cycles at 16 stages
- ~228 cycles/stage + 650 base
- Simple 1st-order allpasses

### Post-zone system (commit `33213922`)
- 32 stages max
- ~43k cycles at 32 stages
- ~1,300 cycles/stage + base
- 2nd-order biquad allpasses
- Multiple topologies
- Punch/chirp delay system

**Expected overhead:** ~5-6x due to biquad + features (actual: ~10x at same stage count)
