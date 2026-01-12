# Table Shaper - Continuation Prompt

## Context

Working on Zone 4 of the XY Shaper - a table-based parametric shaper with ADAA.

## Current State

**Commit**: `612414b0` on `feat/multiband_compressor` branch

**Key Files**:
- `src/deluge/dsp/table_shaper.h` - Core implementation
- `src/deluge/dsp/shaper.h` - Integration wrapper
- `src/deluge/dsp/fast_math.h` - Fast math (fastTanh)
- `docs/dev/table_shaper.md` - Design document

## Bugs to Debug

### 1. Sound is Harsh/Broken (not smooth saturation)

At X > 0, the output sounds harsh and broken instead of smoothly saturated.

**Suspected causes**:

a) **External drive pre-boost is too aggressive** - In `Shaper::process()`:
```cpp
float driveScale = 1.0f + (static_cast<float>(drive) / 2147483648.0f + 1.0f) * 2.0f;
inputF *= driveScale;
inputF = std::clamp(inputF, -1.0f, 1.0f);
```
At menu center (drive=0), this applies 3x boost BEFORE clamping, causing hard clipping before saturation even happens.

**Fix**: Reduce the pre-boost range or apply it more subtly.

b) **Basis functions not scaled by drive** - In `regenerateTables()`:
- Tanh IS scaled: `fastTanh(kEff * norm)`
- Poly is NOT: just uses `norm` directly
- Cheby is NOT: just uses `norm` directly

So drive (X axis) primarily affects tanh steepness but poly/cheby are invariant.

**Fix**: Scale all basis functions by some factor of k.

### 2. X/Y Parameters Have Little Effect

Changing X and Y values doesn't produce obvious timbral differences (except X=0 = bypass).

**Suspected causes**:

a) **Threshold from Y sweep can be high** - When `triangle(yNorm * 2.5f + 0.25f)` returns values near 1.0, almost all input is in the linear zone.

b) **Weight normalization smooths differences** - Blending 3 similar-shaped basis functions and normalizing may reduce perceivable difference.

c) **Peak normalization may mask dynamics** - Always scaling output to match peak could reduce the character of soft clipping.

### 3. Behavior at X=0

X=0 correctly bypasses (transparent) because `drive_ = 0` triggers `isLinear()` check. This is working as intended.

## Debug Steps

1. **Add debug logging** to print derived parameters at various X/Y values
2. **Test each basis function in isolation** - set one weight to 1.0, others to 0
3. **Reduce external drive scaling** - try `driveScale = 1.0f + (drive_norm + 0.5f)` for 0.5x to 1.5x range
4. **Scale poly/cheby by k** - apply `k * norm` or similar to all basis functions
5. **Check ADAA correctness** - compare output with `processNoAA()` to verify antialiasing works

## Relevant Code Sections

### External drive in Shaper::process() (shaper.h ~line 100-116)
```cpp
if (useTable_) {
    float inputF = static_cast<float>(input) / 2147483648.0f;
    float driveScale = 1.0f + (static_cast<float>(drive) / 2147483648.0f + 1.0f) * 2.0f;
    inputF *= driveScale;
    inputF = std::clamp(inputF, -1.0f, 1.0f);
    float* prevXState = (channel == 0) ? &tablePrevXL_ : &tablePrevXR_;
    float outputF = tableSat_.process(inputF, prevXState);
    return static_cast<q31_t>(outputF * 2147483647.0f);
}
```

### Basis function computation (table_shaper.h ~line 268-291)
```cpp
// Basis 1: Tanh - SCALED by kEff
float tanh_out = fastTanh(kEff * norm) * invTanhNorm;

// Basis 2: Polynomial - NOT scaled
float norm3 = norm * norm * norm;
float poly_out = (norm - norm3 * 0.333333f) * 1.5f;

// Basis 3: Chebyshev T5 - NOT scaled
float n2 = norm * norm;
float cheby_out = norm * (5.0f + n2 * (-20.0f + n2 * 16.0f));
```

### Parameter derivation (table_shaper.h ~line 395-418)
```cpp
outDrive = static_cast<float>(x) / 127.0f;  // X -> drive
outTanhWeight = triangle(yNorm * 3.0f);
outPolyWeight = triangle(yNorm * 2.718f + 0.333f);
outChebyWeight = triangle(yNorm * 3.14159f + 0.666f);
outThreshold = triangle(yNorm * 2.5f + 0.25f);
outAsymmetry = 0.3f + triangle(yNorm * 1.618f) * 0.4f;
```

## Goal

Make Zone 4 produce smooth, characterful saturation where:
- X smoothly increases intensity from transparent to heavily saturated
- Y produces audibly different timbres as you sweep through parameter combinations
- No harsh/broken sound artifacts
