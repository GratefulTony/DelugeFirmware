# Analytic Saturator - Design Document

## Overview

The Analytic Saturator is an experimental Zone 4 addition to the XY Saturator, featuring:
- **3 Basis Functions**: Tanh (warm), Polynomial (bright), Chebyshev T5 (fold/synthy)
- **ADAA (Antiderivative Antialiasing)**: Reduces aliasing artifacts
- **Parametric XY Control**: X maps to drive, Y sweeps combinatorically through parameter space
- **Peak Normalization**: Volume-neutral saturation regardless of settings

## Architecture

### Files

- `src/deluge/dsp/analytic_saturator.h` - Core parametric saturator with ADAA
- `src/deluge/dsp/saturator.h` - Wrapper class, integrates Zone 4 with existing zones
- `src/deluge/dsp/fast_math.h` - Fast math approximations (fastTanh, fastExp, etc.)
- `src/deluge/gui/menu_item/fx/saturator.h` - Menu item with MomentumEncoder
- `src/deluge/gui/menu_item/momentum_encoder.h` - High-resolution encoder helper

### Key Classes

#### AnalyticSaturator
Core saturator with cached lookup tables:
- `fTable_[513]` - f(x) waveshaping function values
- `FTable_[513]` - F(x) antiderivative values for ADAA
- Tables regenerate only when parameters change (dirty flag)
- External state pointer pattern for multi-channel efficiency

#### AnalyticSaturatorXYMapper
Derives saturator parameters from XY position using triangle wave phasing:
- X (0-127): Maps to drive (0 = bypass, 127 = full saturation)
- Y (0-127): Combinatoric sweep through basis weights, threshold, asymmetry
- Triangle waves with irrational period ratios (3, e, pi) for dense parameter coverage

### Parameter Phasing

The Y axis uses triangle waves to sweep through parameter space:
```cpp
outTanhWeight = triangle(yNorm * 3.0f);                // 3 cycles
outPolyWeight = triangle(yNorm * 2.718f + 0.333f);     // e cycles
outChebyWeight = triangle(yNorm * 3.14159f + 0.666f);  // pi cycles
outThreshold = triangle(yNorm * 2.5f + 0.25f);
outAsymmetry = 0.3f + triangle(yNorm * 1.618f) * 0.4f; // golden ratio
```

### ADAA Implementation

First-order ADAA using cached antiderivative table:
```cpp
output = (F(x) - F(prev_x)) / (x - prev_x)  // Normal case
output = f(x)                                // When dx is too small
```

The antiderivative is computed via trapezoidal integration during table generation.

### Stereo Handling

One AnalyticSaturator instance (shared tables) with separate ADAA state per channel:
```cpp
// In Saturator class:
AnalyticSaturator analyticSat_;      // Shared tables
float analyticPrevXL_{0.0f};         // L channel ADAA state
float analyticPrevXR_{0.0f};         // R channel ADAA state

// In process():
float* prevXState = (channel == 0) ? &analyticPrevXL_ : &analyticPrevXR_;
float output = analyticSat_.process(input, prevXState);
```

## Zone Layout

- **Zone 0 (Y 0-31)**: Standard saturation
- **Zone 1 (Y 32-63)**: Asymmetric tube (even harmonics)
- **Zone 2 (Y 64-95)**: Hard clip with linear zone
- **Zone 3 (Y 96-127)**: Crossover distortion
- **Zone 4 (Y 128-255)**: Analytic ADAA saturator ("Aanalytic")

## Known Issues (WIP)

### Sound Quality
1. **Harsh/Broken Sound**: At X > 0, the output sounds harsh and broken rather than smoothly saturated
2. **Limited Parameter Response**: X and Y changes have minimal audible effect (except X=0 being transparent)

### Suspected Causes
1. **External Drive Pre-boost**: The Saturator::process() applies 3x boost at menu center position before clamping, causing hard clipping before saturation
2. **Basis Functions Not Scaled**: Polynomial and Chebyshev bases don't respond to drive/k parameter - only tanh is scaled by steepness
3. **Threshold Interaction**: High threshold values from Y sweep may mask saturation effect
4. **Peak Normalization**: May be amplifying artifacts

### Debug Checklist
- [ ] Verify parameter values are reasonable at various X/Y positions
- [ ] Check if external drive scaling is too aggressive
- [ ] Ensure all basis functions respond to drive parameter
- [ ] Test ADAA by comparing with non-ADAA output
- [ ] Verify peak normalization gain values

## Future Considerations

- Integration with DOTT multiband compressor (one saturator per band)
- Phase offset parameters from vibe/feel knobs for evolving textures
- Optimize table size vs quality tradeoff
