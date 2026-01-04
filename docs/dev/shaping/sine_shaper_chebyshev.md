# Zone 1 "357" Chebyshev Harmonic Extraction

## Fundamental Cancellation - Critical Design Constraint

The normalized Chebyshev polynomials Hn(x) are designed to extract pure
harmonics from a sinusoidal input. For x = sin(θ):

```
H3(sin(θ)) = sin(3θ)/3   (pure 3rd harmonic)
H5(sin(θ)) = sin(5θ)/5   (pure 5th harmonic)
H7(sin(θ)) = sin(7θ)/7   (pure 7th harmonic)
```

The polynomial form H3(x) = x - (4/3)x³ achieves this by having the linear
term (x) exactly cancel the fundamental energy produced by the cubic term.
This cancellation is mathematically precise when evaluated at sin(θ).

## Low Input Level Problem

**At low input levels, fundamental cancellation FAILS.**

The polynomial P(x) = c1*x + c3*x³ + c5*x⁵ + c7*x⁷ behaves differently
depending on input amplitude:

| Input |x| | Behavior |
|---------|----------|
| ≈ 1.0 | All terms contribute, cancellation works |
| = 0.5 | c3*x³ = c3*0.125, much smaller than c1*x = c1*0.5 |
| = 0.1 | c3*x³ = c3*0.001, negligible vs c1*x = c1*0.1 |

For small x, P(x) ≈ c1*x (pure fundamental pass-through).

**Numerical example with pure H3 (c1=1, c3=-4/3):**
- x=1.0: P(x) = 1 - 1.33 = -0.33 (harmonics dominate)
- x=0.5: P(x) = 0.5 - 0.17 = 0.33 (66% fundamental)
- x=0.1: P(x) = 0.1 - 0.0013 ≈ 0.1 (99% fundamental!)

This is a fundamental limitation of polynomial waveshapers - they become
LINEAR at low input levels. The Drive parameter must push the signal into
the polynomial's nonlinear region for harmonic extraction to work.

## High Input Level Solution (Output Wavefolding)

When the polynomial OUTPUT exceeds [-1, 1], we use wavefolding to map it
back. Unlike input folding, output folding preserves the Chebyshev harmonic
extraction because the polynomial sees the actual (overdriven) sine wave.

**Key insight:** Input folding distorts the waveform BEFORE the polynomial,
creating harmonics similar to hard clipping. Output folding lets the
polynomial do its harmonic extraction first, then bounds the result.

**Formula:** `phase = fmod(result + 1, 4); if (phase < 0) phase += 4; result = 1 - fabs(phase - 2)`

- At moderate drive: polynomial output stays in [-1, 1], no folding occurs
- At high drive: polynomial blows up (T7(2) ≈ 5000), folding kicks in

**Benefits vs input folding:**
- Preserves Chebyshev harmonic extraction at moderate drives
- Folding only activates when polynomial exceeds bounds
- Musical character when overdriven (Buchla/Serge wavefolder sound)

## Why Not Just Skip Fundamental Cancellation?

Without the c1 linear term (using only c3*x³ + c5*x⁵ + c7*x⁷), each power
of x produces BOTH fundamental and harmonics when applied to sin(θ):

```
sin³(θ) = (3sin(θ) - sin(3θ))/4    → 75% fundamental + 25% 3rd harmonic
sin⁵(θ) = (10sin(θ) - 5sin(3θ) + sin(5θ))/16  → 62.5% fundamental + ...
```

The fundamental components from x³, x⁵, x⁷ are IN-PHASE with the input.
This creates a mix knob problem:

| Mix | Result |
|-----|--------|
| 100% wet | fundamental + harmonics (from polynomial) |
| 50% mix | 0.5*dry + 0.5*(fundamental + harmonics) = 1.0× fundamental + 0.5× harmonics |
| 0% dry | fundamental only |

The constructive interference at mid-mix values causes non-linear output
level swings. **Mix 50% can be LOUDER than either 0% or 100%.**

With fundamental cancellation (c1 = w3+w5+w7), the wet signal contains
only harmonics (at optimal drive), making the mix knob behavior predictable:

| Mix | Result |
|-----|--------|
| 100% wet | harmonics only |
| 50% mix | 0.5*dry + 0.5*harmonics (linear blend) |
| 0% dry | fundamental only |

## Design Decision

We KEEP fundamental cancellation despite its limitations because:

1. Predictable mix behavior (no output level surprises at mid-mix values)
2. Clean harmonic extraction at moderate drive levels
3. The "sweet spot" drive requirement is acceptable for a sound design tool

**Accepted limitations:**

1. **LOW DRIVE:** Polynomial becomes linear, outputs scaled fundamental.
   At 100% mix this produces quiet fundamental instead of silence.
   → Accepted as inherent to polynomial waveshaping physics.

2. **HIGH DRIVE:** Wavefolding adds its own harmonic character.
   → Accepted. The classic Buchla/Serge wavefolder sound adds musical
   complexity rather than harsh clipping artifacts when overdriven.

3. **OPTIMAL RANGE:** Effect works best when input amplitude is near ±1.0.
   → Users should adjust Drive to find the harmonic extraction sweet spot.

**Future enhancement:** Per-buffer envelope follower to normalize input,
ensuring polynomial always operates in optimal region regardless of
input level. This would eliminate the low-drive fundamental leakage
at the cost of ~10 additional cycles per sample.
