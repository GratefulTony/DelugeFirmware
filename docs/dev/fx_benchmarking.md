# FX Benchmarking System

A compile-time optional system for measuring CPU cycle costs of audio effects on the Deluge hardware.

## Overview

The FX benchmarking system provides:
- **Zero-cost when disabled**: All benchmarking code compiles away completely
- **JSON output**: Machine-parseable format for automated analysis
- **Downsampling**: Only samples every Nth buffer to minimize overhead
- **Dynamic tagging**: Runtime tags for zone/mode-based aggregation

## How It Works

1. **ARM PMU Cycle Counter**: Uses the Cortex-A9 Performance Monitoring Unit to read CPU cycles with single-cycle precision
2. **Global Sampling Flag**: A single global counter advances once per audio buffer; when it reaches N, all benchmarks sample that buffer
3. **Minimal Per-Call Overhead**: Each benchmark call only checks a single boolean flag (not a counter per benchmark)
4. **MIDI Sysex Output**: Benchmark data is sent via the existing debug sysex infrastructure
5. **JSON Format**: Each sample outputs `{"fx":"name","cycles":N,"ts":T,"tags":["tag1","tag2"]}`

## Runtime Performance

When **disabled** (`ENABLE_FX_BENCHMARK=OFF`, the default):
- **Zero overhead**: All macros expand to nothing
- No code generated, no memory used

When **enabled** (`ENABLE_FX_BENCHMARK=ON`):
- **Per-call overhead**: ~1-2 cycles (single boolean check)
- **Global counter**: ~3 cycles per buffer (once, in audio engine)
- **Timing overhead**: ~50 cycles per benchmark when actively sampling
- **JSON output**: ~2,000 cycles per benchmark when outputting
- **Memory**: ~24 bytes per benchmark declaration (static)

**Effective overhead**: Negligible when not sampling, ~0.1% CPU during sampling buffer.

## Usage

### Building with Benchmarking

```bash
./dbt configure -DENABLE_FX_BENCHMARK=ON -DENABLE_SYSEX_LOAD=YES
./dbt build relwithdebinfo
```

**Note:** Must use `relwithdebinfo` (not `release`) because `Debug::println` requires `ENABLE_TEXT_OUTPUT` which is only defined in debug builds.

### Collecting Data

```bash
# Full workflow (build, upload, collect until Ctrl+C/Enter)
./dbt fx-benchmark

# Or step by step
./dbt fx-benchmark --build
./dbt fx-benchmark --upload
./dbt fx-benchmark --collect -o fx_benchmark.csv

# With options
./dbt fx-benchmark --collect -d 120 -o fx_benchmark.csv  # Fixed 120s duration
./dbt fx-benchmark --collect -v                          # Verbose: show all debug messages
```

### Analysis

Open the Jupyter notebook at `contrib/analysis/fx_benchmark_analysis.ipynb` or use pandas directly:

```python
import pandas as pd
df = pd.read_csv('fx_benchmark.csv')

# Summary by effect
print(df.groupby('fx')['cycles'].agg(['count', 'mean', 'median']))

# Summary by effect + zone/mode tag
print(df.groupby(['fx', 'tags'])['cycles'].mean())
```

## Instrumented Effects

| Effect | Benchmark Name | Dynamic Tags | Location |
|--------|----------------|--------------|----------|
| Sine Shaper | `sine_shaper` | stage (`total`, `setup`, `loop`) + harmonic zone (`z0_cheby`, `z1_cheby`, `z2_fm`-`z6_fm`, `z7_poly`) | Voice + Clip |
| Table Saturator | `saturator_table` | - | Voice + Clip |
| Builtin Saturator | `saturator_builtin` | - | Voice + Clip |
| Disperser | `disperser` | - | Clip |
| ModFX | `modfx` | type (`flanger`, `chorus`, `phaser`, `stereo_ch`, `warble`, `dimen`, `grain`) | Clip |
| Filters | `filters` | LPF mode + HPF mode (`lp12`, `lp24`, `lp24drv`, `svf_band`, `svf_notch`, `hpladder`, `off`) | Voice + Clip |

**Location key:** Voice = synth patches, Clip = audio clips

### Sine Shaper Sub-Aggregations

The sine shaper uses sub-aggregation tags to break down CPU usage:
- `total`: Overall time for the entire buffer
- `setup`: Zone calculation, weight computation, smoothing prep
- `loop`: Per-sample DSP processing

Example output:
```
B,sine_shaper,12000,1234567890,total,z0_cheby
B,sine_shaper,500,1234567890,setup,z0_cheby
B,sine_shaper,11500,1234567890,loop,z0_cheby
```

## Adding New Benchmarks

### Basic Usage

```cpp
#include "io/debug/fx_benchmark.h"

void MyEffect::process(StereoBuffer<q31_t> buffer) {
    FX_BENCH_DECLARE(bench, "my_effect");
    FX_BENCH_SCOPE(bench);  // RAII: times until end of scope

    // ... effect processing ...
}
```

### Manual Start/Stop

```cpp
FX_BENCH_DECLARE(bench, "my_effect", "tag1");
FX_BENCH_START(bench);
// ... code to benchmark ...
FX_BENCH_STOP(bench);
```

### Dynamic Tags

```cpp
static const char* kModeNames[] = {"mode_a", "mode_b", "mode_c"};
int mode = getCurrentMode();

FX_BENCH_DECLARE(bench, "my_effect");
FX_BENCH_SET_TAG(bench, 0, kModeNames[mode]);  // Set tag before start
FX_BENCH_START(bench);
// ... processing ...
FX_BENCH_STOP(bench);
```

## Output Format

Each benchmark outputs a compact CSV line via sysex:

```
B,multiband,45230,1234567890,crossover,ap1_6dB
B,sine_shaper,15000,1234567900,loop,z2_fm
```

Format: `B,fx,cycles,ts,tag1,tag2,tag3`

Fields:
- `B`: Marker to identify benchmark lines
- `fx`: Effect name
- `cycles`: CPU cycles for this call
- `ts`: Timestamp (cycle counter value at start)
- `tag1,tag2,tag3`: Optional tags for aggregation

**Note:** Results are queued during audio processing and output at buffer end to minimize overhead.

## Files

- `src/deluge/io/debug/fx_benchmark.h` - Header with macros and class
- `src/deluge/io/debug/fx_benchmark.cpp` - JSON output implementation
- `scripts/tasks/task-fx-benchmark.py` - Collection script
- `contrib/analysis/fx_benchmark_analysis.ipynb` - Analysis notebook

## Interpreting Results

At 400 MHz CPU and 44.1 kHz / 128 samples:
- **Cycles per buffer at 100% CPU**: ~1,160,000
- **1% CPU load**: ~11,600 cycles per buffer

Example interpretation:
- Multiband crossover at 50,000 cycles = ~4.3% CPU
- Sine shaper zone 7 at 15,000 cycles = ~1.3% CPU
