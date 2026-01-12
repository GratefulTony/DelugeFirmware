/*
 * Copyright © 2024-2025 Owlet Records
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "deluge/dsp/shaper.h"
#include "deluge/util/fixedpoint.h"
#include "deluge/util/functions.h"
#include "deluge/util/intrinsics.h"
#include "dsp_ng/core/types.hpp"
#include "io/debug/fx_benchmark.h"
#include <climits>
#include <cmath>
#include <span>

namespace deluge::dsp {

/// Per-sample IIR alpha for q31 parameter smoothing (~40ms time constant at 44.1kHz)
constexpr q31_t kShaperSmoothingAlpha = static_cast<q31_t>(0.0005 * ONE_Q31);

/// Subtractive gain staging analysis (from voice.cpp):
/// - FM: sourceAmplitude at full level → signal at ~23M peak
/// - Subtractive: oscillators scaled by >> 4 OR filterGain (both ~16x attenuation)
///
/// The shaper table is designed to handle varying input levels via the drive knob.
/// Subtractive signals use a smaller portion of the table at neutral drive.
/// FilterGain compensation only adjusts for resonance-induced level changes.
constexpr int32_t kShaperNeutralFilterGainInt = 1 << 28; // filterGain at neutral settings (integer)

/// Context for per-sample IIR parameter smoothing during buffer processing
struct ShaperSmoothingContext {
	q31_t current;
	q31_t alpha;
	q31_t target;
};

/// Prepare parameter smoothing for per-sample IIR processing
[[gnu::always_inline]] inline ShaperSmoothingContext prepareShaperSmoothing(q31_t state, q31_t target) {
	return {state, kShaperSmoothingAlpha, target};
}

/// Q16 smoothing alpha (~40ms time constant at 44.1kHz, matches q31 version)
/// 0.0005 * 65536 ≈ 33
constexpr int32_t kShaperSmoothingAlphaQ16 = 33;

/// Context for per-sample Q16 parameter smoothing (used for mixNorm)
struct ShaperSmoothingContextQ16 {
	int32_t current;
	int32_t alpha;
	int32_t target;
};

/// Prepare Q16 parameter smoothing for per-sample IIR processing
[[gnu::always_inline]] inline ShaperSmoothingContextQ16 prepareShaperSmoothingQ16(int32_t state, int32_t target) {
	return {state, kShaperSmoothingAlphaQ16, target};
}

/// Per-channel modulation state pointers (mono: 1 instance, stereo: L+R instances)
/// Groups all state that needs to persist between buffer calls
struct ShaperModState {
	int32_t* driftSlope;      ///< Rate of DC offset accumulation per sample
	int32_t* driftAccum;      ///< Accumulated drift (resets on zero crossings)
	int32_t* prevSample;      ///< Previous sample for zero-crossing detection
	int32_t* slewed;          ///< Slew rate limiter state (previous output)
	int32_t* prevScaledInput; ///< Hysteresis state for slope detection
	uint8_t* zcCount;         ///< Zero-crossing counter (for subharmonic)
	int8_t* subSign;          ///< Subharmonic sign (±1, toggles every 2nd ZC)
};

/// Per-buffer computed values hoisted out of sample loop
/// Computed once at buffer start, passed to per-sample processing
struct ShaperBufferContext {
	// Blend/table parameters
	int32_t blendSlope_Q8; ///< Pre-computed blend slope
	int64_t threshold64;   ///< Pre-computed amplitude threshold
	int8_t tableIdx;       ///< Target table index
	int32_t hystOffset;    ///< Hysteresis offset for slope detection

	// Modulator intensities (from phi triangles, gated by extrasEnabled)
	int32_t subBoost_Q16;           ///< Subharmonic boost amount (0 = disabled)
	int32_t driftMultIntensity_Q16; ///< Multiplicative drift intensity (bipolar)
	int32_t driftAddIntensity_Q16;  ///< Additive drift intensity (bipolar)
	int32_t lpfAlpha_Q16;           ///< Lowpass filter alpha (0 = bypass)

	// Gain staging
	int32_t attenGain_Q16; ///< Output attenuation (subtractive mode)

	// Flags
	bool isLinear;        ///< True if shaper is in linear bypass
	bool lpfActive;       ///< True if lowpass filter enabled (always on when gammaPhase != 0)
	bool driftActive;     ///< True if drift modulation enabled (requires extrasEnabled)
	bool needsGainAdjust; ///< True if subtractive gain compensation needed
	bool extrasEnabled;   ///< True if drift+sub extras enabled (X encoder toggle)
};

/// Drift slope range and evolution rate (50% increased from 70k/80k)
/// Slope is always positive (unipolar), oscillates between [min, max] via random walk
/// Phi triangle intensity (bipolar) scales and applies polarity (sag vs boost)
constexpr int32_t kDriftSlopeMin = 105000; // Tight range (~12% variation)
constexpr int32_t kDriftSlopeMax = 120000; // Max slope (scaled by phi triangle intensity)
constexpr int32_t kDriftSlopeStep = 8;     // Very slow evolution (~30+ sec to traverse range)

/// Subharmonic gain modulation: maximum cut/boost amount at full intensity (25% decreased from 26214)
/// 19660 Q16 = ~30% (0.7x when subSign=+1, 1.3x when subSign=-1)
constexpr int32_t kSubBoostMax_Q16 = 19660;

/// Lowpass filter for transient softening (replaces slew rate limiting)
/// One-pole IIR: y[n] = y[n-1] + alpha * (x[n] - y[n-1])
/// Cutoff is note-relative: min = 1 octave above root, max = 2 octaves above
/// alpha ≈ 2π * fc / fs, at 44.1kHz: alpha_Q16 ≈ fc * 9.33
constexpr float kLpfOctaveMin = 1.0f; // Min cutoff = 2^1 = 2x note freq (one octave above)
constexpr float kLpfOctaveMax = 2.0f; // Max cutoff = 2^2 = 4x note freq (two octaves above)
constexpr int32_t kLpfAlphaScale = 9; // 2π * 65536 / 44100 ≈ 9.33
constexpr float kLpfRefFreq = 110.0f; // Reference for audio tracks (A2, gives 220-440Hz range)

/// Compute lowpass alpha from intensity and note frequency (Q16 format)
/// intensity 0 → bypass, intensity > 0 → cutoff sweeps from 2 octaves down to 1 octave above note
/// At A4 (440Hz): min=880Hz, max=1760Hz. At A2 (110Hz): min=220Hz, max=440Hz
[[gnu::always_inline]] inline int32_t computeLpfAlpha_Q16(int32_t intensity_Q16, float noteFreqHz = kLpfRefFreq) {
	// Interpolate octave offset: intensity=0 → 2 octaves, intensity=max → 1 octave above note
	float octaveRange = kLpfOctaveMax - kLpfOctaveMin;
	float octaveOffset = kLpfOctaveMax - (octaveRange * static_cast<float>(intensity_Q16) / 65536.0f);
	// Cutoff = note * 2^octaveOffset (e.g., A4=440 → 880-1760Hz range)
	float cutoff = noteFreqHz * exp2f(octaveOffset);
	// alpha = 2π * cutoff / fs, in Q16
	return static_cast<int32_t>(cutoff) * kLpfAlphaScale;
}

/// Update LFSR with signal entropy and step random walk for drift slope
/// Slope is unipolar, oscillates between [minSlope, maxSlope] - never reaches zero
/// Returns new slope value (rate of DC offset accumulation per sample)
[[gnu::always_inline]] inline int32_t updateDriftSlopeRandomWalk(int32_t currentSlope, uint32_t& lfsr,
                                                                 int32_t signalEntropy, int32_t maxSlope,
                                                                 int32_t minSlope) {
	// Mix signal bits into LFSR for entropy
	lfsr ^= static_cast<uint32_t>(signalEntropy) & 0xFFFF;
	// Galois LFSR step (fast, good distribution)
	lfsr ^= lfsr >> 7;
	lfsr ^= lfsr << 9;
	lfsr ^= lfsr >> 13;

	// Safeguard: reseed LFSR if it degenerates to 0 (would cause stuck drift)
	if (lfsr == 0) {
		lfsr = 0xDEADBEEF;
	}

	// Random walk direction from LFSR
	int8_t dir = (lfsr & 1) ? 1 : -1;

	// Compute distance to the rail we're moving toward (unipolar: [minSlope, maxSlope])
	// max(0, ...) handles bootstrap case when slope starts below minSlope
	int32_t distanceToRail = (dir > 0) ? (maxSlope - currentSlope) : std::max(int32_t{0}, currentSlope - minSlope);

	// Step is min of fixed step and distance to rail - guaranteed in bounds
	int32_t stepMag = std::min(kDriftSlopeStep, distanceToRail);
	int32_t newSlope = currentSlope + stepMag * dir;

	// Clamp to valid range (bootstrap: if starting below min, push up to min)
	return std::clamp(newSlope, minSlope, maxSlope);
}

/// Per-sample shaper processing - shared by mono and stereo versions
/// Handles: drive → slew → combined drift+sub → additive → shaper
/// @param input Raw input sample
/// @param driveGain_Q26 Smoothed drive gain (includes boost if subtractive)
/// @param ctx Pre-computed buffer context (hoisted values)
/// @param slewed Pointer to slew state (updated)
/// @param prevScaledInput Pointer to hysteresis state (updated)
/// @param driftGain_Q16 Per-sample multiplicative drift gain (65536 = unity)
/// @param subSign Per-sample subharmonic sign (±1)
/// @param driftOffset_Q16 Per-sample additive drift offset
/// @param shaper Reference to shaper instance
/// @return Processed sample (before output attenuation)
[[gnu::always_inline]] inline q31_t processShaperSample(q31_t input, int32_t driveGain_Q26,
                                                        const ShaperBufferContext& ctx, int32_t* slewed,
                                                        int32_t* prevScaledInput, int32_t driftGain_Q16, int8_t subSign,
                                                        int32_t driftOffset_Q16, TableShaper& shaper) {
	// Apply drive (boost folded into driveGain_Q26)
	q31_t drivenInput = shift_left_saturate<6, 32>(multiply_32x32_rshift32(input, driveGain_Q26));

	// Build wet path: slew → mult → sub → additive
	q31_t wetInput = drivenInput;

	// 1. Lowpass filter: soften transients (one-pole IIR)
	if (ctx.lpfActive) {
		int64_t diff = static_cast<int64_t>(drivenInput) - *slewed;
		*slewed += static_cast<int32_t>((diff * ctx.lpfAlpha_Q16) >> 16);
		wetInput = *slewed;
	}

	// 2-3. Combined multiplicative modifiers: drift + subharmonic
	{
		int32_t wetModGain_Q16 = 65536; // unity
		bool hasDrift = (driftGain_Q16 != 65536 && driftGain_Q16 != 0);
		bool hasSub = (ctx.subBoost_Q16 != 0);

		if (hasDrift) {
			wetModGain_Q16 = driftGain_Q16;
		}
		if (hasSub) {
			int32_t subGain_Q16 = 65536 - subSign * ctx.subBoost_Q16;
			if (hasDrift) {
				wetModGain_Q16 = static_cast<int32_t>((static_cast<int64_t>(wetModGain_Q16) * subGain_Q16) >> 16);
			}
			else {
				wetModGain_Q16 = subGain_Q16;
			}
		}
		if (hasDrift || hasSub) {
			wetInput = static_cast<q31_t>((static_cast<int64_t>(wetInput) * wetModGain_Q16) >> 16);
		}
	}

	// 4. Additive drift: DC offset on transfer curve
	if (driftOffset_Q16 != 0) {
		int32_t offset = -driftOffset_Q16 << 8;
		wetInput = add_saturate(wetInput, offset);
	}

	// Shaper: wet path through table, dry path for blending
	return shaper.processWithGainHoisted(wetInput, drivenInput, ctx.blendSlope_Q8, ctx.threshold64, ctx.tableIdx,
	                                     ctx.hystOffset, prevScaledInput);
}

/**
 * Process a mono buffer through the TableShaper using integer-only path
 *
 * Table operates at FM signal levels. For subtractive synths, pass filterGain to
 * dynamically compute the boost needed to match FM operating levels.
 *
 * @param buffer Audio buffer to process in-place
 * @param shaper The Shaper instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDriveGain Previous driveGain_Q26 value for smoothing (updated)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param smoothedMixNorm_Q16 Previous mixNorm value for smoothing (Q16.16, updated)
 * @param filterGain For subtractive mode: filterGain from filter config (0 = FM mode)
 * @param hasFilters For subtractive mode: true if filters are active
 * @param state Per-channel modulation state (drift, sub, slew, hysteresis)
 * @param driftLfsr Pointer to LFSR state for random walk entropy (shared)
 * @param extrasEnabled Whether drift+sub extras are enabled (X encoder toggle)
 * @param gammaPhase Secret knob phase offset for phi triangles (0 = slew disabled)
 * @param noteFreqHz Note frequency in Hz for LPF cutoff scaling (default 440Hz = A4)
 */
inline void shapeBufferInt32(std::span<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDriveGain,
                             q31_t mix, int32_t* smoothedMixNorm_Q16, q31_t filterGain, bool hasFilters,
                             ShaperModState& state, uint32_t* driftLfsr, bool extrasEnabled, float gammaPhase,
                             float noteFreqHz = kLpfRefFreq) {
	if (buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	// Compute gain adjustment for subtractive mode (fixed-point, computed once per buffer)
	// filterGain=0 means FM mode (no adjustment needed)
	// filterGain>0 means subtractive: compensate for resonance-induced level changes
	// At neutral filterGain (2^28), gains = 1.0 (no adjustment)
	// High resonance (low filterGain) → boost; low resonance (high filterGain) → attenuate
	bool needsGainAdjust = (filterGain > 0) && hasFilters && (filterGain != kShaperNeutralFilterGainInt);
	int32_t attenGain_Q16 = 65536; // 1.0 in Q16

	// Compute target driveGain ONCE (hoisted p^5 calculation)
	// Fold boost into drive target to save one multiply per sample
	int32_t targetGain_Q26 = TableShaper::driveToGainQ26(drive);
	if (needsGainAdjust) {
		// One float divide per buffer for attenuation
		float ratio = static_cast<float>(kShaperNeutralFilterGainInt) / static_cast<float>(filterGain);
		attenGain_Q16 = static_cast<int32_t>((1.0f / ratio) * 65536.0f);
		// Fold boost into drive: (boost_Q16 × drive_Q26) >> 16 → Q26
		// Uses 64-bit intermediate to handle large boost × drive products
		int64_t boosted64 = static_cast<int64_t>(ratio * 65536.0f) * targetGain_Q26;
		targetGain_Q26 = static_cast<int32_t>(std::min(boosted64 >> 16, static_cast<int64_t>(INT32_MAX)));
	}
	// Smooth driveGain_Q26 (includes boost if subtractive) - stored value is Q26 gain
	auto gainCtx = prepareShaperSmoothing(*smoothedDriveGain, targetGain_Q26);

	// Convert mix param to Q16 normalized value and setup smoothing (fastmath called once)
	int32_t targetMixNorm_Q16 = TableShaper::mixParamToNormQ16(mix);
	auto mixCtx = prepareShaperSmoothingQ16(*smoothedMixNorm_Q16, targetMixNorm_Q16);

	// Hoist atomic loads once per buffer (removes memory barriers from per-sample loop)
	bool isLinear = shaper.getIsLinear();
	int8_t tableIdx = shaper.getTargetTableIndex();

	// Pre-compute mix-dependent values once per buffer (hoisted from per-sample loop)
	// Uses start-of-buffer mix value; error is tiny (~3ms buffer vs ~40ms smoothing)
	int32_t blendSlope_Q8 = shaper.computeBlendSlope_Q8(mixCtx.current);
	int64_t threshold64 = TableShaper::computeThreshold64(mixCtx.current);

	// Hoist hysteresis offset - skip when intensity is 0 (phi triangle at zero)
	int32_t hystOffset = 0;
	int32_t* hystState = nullptr;
	if (state.prevScaledInput && gammaPhase != 0.0f) {
		hystOffset = shaper.getHystOffset();
		if (hystOffset != 0) {
			hystState = state.prevScaledInput; // Only track slope when offset active
		}
	}

	// Hoist subharmonic intensity (for gain modulation based on subSign)
	// Sub requires extrasEnabled + gammaPhase != 0
	int32_t subBoost_Q16 = 0;
	if (extrasEnabled && state.zcCount && state.subSign && gammaPhase != 0.0f) {
		int32_t subIntensity_Q16 = shaper.getSubIntensity_Q16();
		// Pre-compute boost amount: subIntensity * maxBoost >> 16
		subBoost_Q16 = static_cast<int32_t>((static_cast<int64_t>(subIntensity_Q16) * kSubBoostMax_Q16) >> 16);
	}

	// Drift slope setup: random walk controls rate of DC offset accumulation
	// Drift requires extrasEnabled + gammaPhase != 0 (X encoder toggle gates drift/sub)
	// Two drift modes with bipolar intensities at uncorrelated phi frequencies:
	// - Multiplicative: positive=sag toward zero, negative=boost away from zero
	// - Additive: positive=pull toward center, negative=push from center
	bool driftActive =
	    extrasEnabled && state.driftSlope && state.driftAccum && driftLfsr && state.prevSample && gammaPhase != 0.0f;
	int32_t driftMultIntensity_Q16 = 0;
	int32_t driftAddIntensity_Q16 = 0;
	if (driftActive) {
		driftMultIntensity_Q16 = shaper.getDriftMultIntensity_Q16();
		driftAddIntensity_Q16 = shaper.getDriftAddIntensity_Q16();
		// Scale slope range by absolute intensity (higher intensity = wider slope range)
		int32_t absMultIntensity = (driftMultIntensity_Q16 < 0) ? -driftMultIntensity_Q16 : driftMultIntensity_Q16;
		int32_t absAddIntensity = (driftAddIntensity_Q16 < 0) ? -driftAddIntensity_Q16 : driftAddIntensity_Q16;
		// Use max of both intensities for slope scaling (both need the slope walker)
		int32_t maxIntensity = std::max(absMultIntensity, absAddIntensity);
		int32_t maxSlope = static_cast<int32_t>((static_cast<int64_t>(kDriftSlopeMax) * maxIntensity) >> 16);
		int32_t minSlope = static_cast<int32_t>((static_cast<int64_t>(kDriftSlopeMin) * maxIntensity) >> 16);
		// Ensure min doesn't exceed max (clamp if intensity is very low)
		minSlope = std::min(minSlope, maxSlope);
		// Update random walk using signal entropy from buffer middle
		int32_t entropy = buffer[buffer.size() / 2];
		*state.driftSlope = updateDriftSlopeRandomWalk(*state.driftSlope, *driftLfsr, entropy, maxSlope, minSlope);
	}

	// Lowpass filter setup: compute alpha from intensity (scaled by note frequency)
	// Active when filter state pointer provided and slewIntensity > 0
	// (slewIntensity is set > 0 for gammaPhase > 0 OR square waves)
	bool lpfActive = state.slewed != nullptr;
	int32_t lpfAlpha_Q16 = 0;
	if (lpfActive) {
		int32_t slewIntensity_Q16 = shaper.getSlewIntensity_Q16();
		if (slewIntensity_Q16 > 0) {
			lpfAlpha_Q16 = computeLpfAlpha_Q16(slewIntensity_Q16, noteFreqHz);
		}
		else {
			lpfActive = false; // Intensity 0 = disabled
		}
	}

	// Build per-buffer context (hoisted values for per-sample helper)
	ShaperBufferContext ctx{
	    .blendSlope_Q8 = blendSlope_Q8,
	    .threshold64 = threshold64,
	    .tableIdx = tableIdx,
	    .hystOffset = hystOffset,
	    .subBoost_Q16 = subBoost_Q16,
	    .driftMultIntensity_Q16 = driftMultIntensity_Q16,
	    .driftAddIntensity_Q16 = driftAddIntensity_Q16,
	    .lpfAlpha_Q16 = lpfAlpha_Q16,
	    .attenGain_Q16 = attenGain_Q16,
	    .isLinear = isLinear,
	    .lpfActive = lpfActive,
	    .driftActive = driftActive,
	    .needsGainAdjust = needsGainAdjust,
	    .extrasEnabled = extrasEnabled,
	};

	// Fast path: linear bypass (X=0 or table not ready)
	if (ctx.isLinear) {
		for (auto& sample : buffer) {
			gainCtx.current += multiply_32x32_rshift32(gainCtx.target - gainCtx.current, gainCtx.alpha) * 2;
			mixCtx.current += ((mixCtx.target - mixCtx.current) * mixCtx.alpha) >> 16;
			// Apply drive only (consistent with shaped path)
			sample = multiply_32x32_rshift32(sample, gainCtx.current) << 6;
		}
		*smoothedDriveGain = gainCtx.current;
		*smoothedMixNorm_Q16 = mixCtx.current;
		return;
	}

	for (auto& sample : buffer) {
		gainCtx.current += multiply_32x32_rshift32(gainCtx.target - gainCtx.current, gainCtx.alpha) * 2;
		// Q16 IIR smoothing: current += (target - current) * alpha >> 16
		mixCtx.current += ((mixCtx.target - mixCtx.current) * mixCtx.alpha) >> 16;

		q31_t input = sample;

		// Drift: two complementary effects that reset on zero crossings
		// - Multiplicative: gain sag/boost (capacitor discharge / charge)
		// - Additive: DC offset pull/push (toward or from center)
		// Both use the same accumulator/slope but apply based on their bipolar intensities
		int32_t driftGain_Q16 = 0;   // 0 = disabled, >0 = apply multiplicative gain
		int32_t driftOffset_Q16 = 0; // 0 = disabled, additive DC offset
		if (driftActive) {
			int32_t prev = *state.prevSample;
			*state.prevSample = input;
			// Zero crossing detection: reset accumulator
			bool zc = (input ^ prev) < 0;
			if (zc) {
				*state.driftAccum = 0;
				// Subharmonic: toggle sign every 2nd ZC (full wave cycle modulation)
				// Toggle on EVEN counts so both halves of each cycle get same treatment
				// (extrasEnabled already checked via driftActive gate)
				if (state.zcCount && state.subSign) {
					(*state.zcCount)++;
					if ((*state.zcCount & 1) == 0) {
						*state.subSign = -*state.subSign;
					}
				}
			}
			else {
				// Accumulate: grows linearly within each half-cycle
				*state.driftAccum += *state.driftSlope;
			}

			// Compute drift effects from accumulator based on bipolar intensities
			// Shift 11 gives ~30% max effect at low frequencies, ~10% at mid frequencies
			int32_t baseEffect = *state.driftAccum >> 11;

			// Multiplicative drift: positive intensity = sag (toward zero), negative = boost (away)
			// Apply as gain: 65536 = unity, lower = sag, higher = boost
			if (driftMultIntensity_Q16 != 0) {
				// Scale effect by intensity magnitude, then apply polarity
				int32_t scaledMult =
				    static_cast<int32_t>((static_cast<int64_t>(baseEffect) * driftMultIntensity_Q16) >> 16);
				// Positive intensity: subtract from unity (sag), negative: add to unity (boost)
				driftGain_Q16 = std::clamp(65536 - scaledMult, int32_t{0}, int32_t{131072});
			}

			// Additive drift: positive intensity = pull toward center, negative = push from center
			// Apply as DC offset added to wet signal
			if (driftAddIntensity_Q16 != 0) {
				// Scale effect by intensity (polarity determines push vs pull direction)
				// Negative intensity pushes signal away from zero (toward rails)
				driftOffset_Q16 =
				    static_cast<int32_t>((static_cast<int64_t>(baseEffect) * driftAddIntensity_Q16) >> 16);
			}
		}

		// Process sample through shared helper (drive → slew → drift+sub → additive → shaper)
		int8_t currentSubSign = (state.subSign && ctx.subBoost_Q16 != 0) ? *state.subSign : 1;
		q31_t out = processShaperSample(input, gainCtx.current, ctx, state.slewed, hystState, driftGain_Q16,
		                                currentSubSign, driftOffset_Q16, shaper);

		if (ctx.needsGainAdjust) {
			out = static_cast<q31_t>((static_cast<int64_t>(out) * ctx.attenGain_Q16) >> 16);
		}

		sample = out;
	}

	*smoothedDriveGain = gainCtx.current;
	*smoothedMixNorm_Q16 = mixCtx.current;
}

/**
 * Process a stereo buffer through the TableShaper using integer-only path
 *
 * Table operates at FM signal levels. For subtractive synths, pass filterGain to
 * dynamically compute the boost needed to match FM operating levels.
 *
 * Optimizations:
 * - p^5 drive curve computed once per buffer, Q26 gain smoothed per-sample
 * - Filter gain adjustment uses Q16 fixed-point (one float divide per buffer)
 *
 * @param buffer Stereo audio buffer to process in-place
 * @param shaper The Shaper instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDriveGain Previous driveGain_Q26 value for smoothing (updated, stores Q26 gain not raw drive)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param smoothedMixNorm_Q16 Previous mixNorm value for smoothing (Q16.16 format, updated)
 * @param filterGain For subtractive mode: pass the filterGain from filter config.
 *                   For FM mode: pass 0 (no boost needed).
 * @param hasFilters For subtractive mode: true if filters are active
 * @param prevScaledInputL Pointer to previous scaled input for left channel hysteresis (updated, can be null)
 * @param prevScaledInputR Pointer to previous scaled input for right channel hysteresis (updated, can be null)
 * @param driftSlopeL Pointer to L channel drift slope state (rate of DC offset accumulation per sample)
 * @param driftSlopeR Pointer to R channel drift slope state (decorrelated from L by phi triangle)
 * @param driftAccumL Pointer to L channel accumulated DC offset (resets on zero crossings)
 * @param driftAccumR Pointer to R channel accumulated DC offset (resets on zero crossings)
 * @param driftLfsr Pointer to LFSR state for random walk entropy
 * @param prevSampleL Pointer to previous L sample for zero-crossing detection
 * @param prevSampleR Pointer to previous R sample for zero-crossing detection
 * @param zcCountL Pointer to L channel zero-crossing counter (for subharmonic)
 * @param zcCountR Pointer to R channel zero-crossing counter (for subharmonic)
 * @param subSignL Pointer to L channel subharmonic sign (±1)
 * @param subSignR Pointer to R channel subharmonic sign (±1)
 * @param extrasEnabled Whether drift+sub extras are enabled (X encoder toggle)
 * @param gammaPhase Secret knob phase offset for phi triangles (0 = slew disabled)
 * @param slewedL Pointer to L channel slew rate limiter state (previous output)
 * @param slewedR Pointer to R channel slew rate limiter state (previous output)
 * @param noteFreqHz Note frequency in Hz for LPF cutoff scaling (default 440Hz = A4)
 */
inline void shapeBufferInt32(StereoBuffer<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDriveGain,
                             q31_t mix, int32_t* smoothedMixNorm_Q16, q31_t filterGain, bool hasFilters,
                             int32_t* prevScaledInputL, int32_t* prevScaledInputR, int32_t* driftSlopeL,
                             int32_t* driftSlopeR, int32_t* driftAccumL, int32_t* driftAccumR, uint32_t* driftLfsr,
                             int32_t* prevSampleL, int32_t* prevSampleR, uint8_t* zcCountL, uint8_t* zcCountR,
                             int8_t* subSignL, int8_t* subSignR, bool extrasEnabled, float gammaPhase, int32_t* slewedL,
                             int32_t* slewedR, float noteFreqHz = kLpfRefFreq) {
	if (buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	// Compute gain adjustment for subtractive mode (fixed-point, computed once per buffer)
	// filterGain=0 means FM mode (no adjustment needed)
	// filterGain>0 means subtractive: compensate for resonance-induced level changes
	// At neutral filterGain (2^28), gains = 1.0 (no adjustment)
	// High resonance (low filterGain) → boost; low resonance (high filterGain) → attenuate
	bool needsGainAdjust = (filterGain > 0) && hasFilters && (filterGain != kShaperNeutralFilterGainInt);
	int32_t attenGain_Q16 = 65536; // 1.0 in Q16

	// Compute target driveGain ONCE (hoisted p^5 calculation)
	// Fold boost into drive target to save one multiply per sample
	int32_t targetGain_Q26 = TableShaper::driveToGainQ26(drive);
	if (needsGainAdjust) {
		// One float divide per buffer for attenuation
		float ratio = static_cast<float>(kShaperNeutralFilterGainInt) / static_cast<float>(filterGain);
		attenGain_Q16 = static_cast<int32_t>((1.0f / ratio) * 65536.0f);
		// Fold boost into drive: (boost_Q16 × drive_Q26) >> 16 → Q26
		// Uses 64-bit intermediate to handle large boost × drive products
		int64_t boosted64 = static_cast<int64_t>(ratio * 65536.0f) * targetGain_Q26;
		targetGain_Q26 = static_cast<int32_t>(std::min(boosted64 >> 16, static_cast<int64_t>(INT32_MAX)));
	}
	// Smooth driveGain_Q26 (includes boost if subtractive) - stored value is Q26 gain
	auto gainCtx = prepareShaperSmoothing(*smoothedDriveGain, targetGain_Q26);

	// Convert mix param to Q16 normalized value and setup smoothing (fastmath called once)
	int32_t targetMixNorm_Q16 = TableShaper::mixParamToNormQ16(mix);
	auto mixCtx = prepareShaperSmoothingQ16(*smoothedMixNorm_Q16, targetMixNorm_Q16);

	// Hoist atomic loads once per buffer (removes memory barriers from per-sample loop)
	bool isLinear = shaper.getIsLinear();
	int8_t tableIdx = shaper.getTargetTableIndex();

	// Pre-compute mix-dependent values once per buffer (hoisted from per-sample loop)
	// Uses start-of-buffer mix value; error is tiny (~3ms buffer vs ~40ms smoothing)
	int32_t blendSlope_Q8 = shaper.computeBlendSlope_Q8(mixCtx.current);
	int64_t threshold64 = TableShaper::computeThreshold64(mixCtx.current);

	// Hoist hysteresis offset - skip when intensity is 0 (phi triangle at zero)
	int32_t hystOffset = 0;
	int32_t* hystStateL = nullptr;
	int32_t* hystStateR = nullptr;
	if (prevScaledInputL && prevScaledInputR && gammaPhase != 0.0f) {
		hystOffset = shaper.getHystOffset();
		if (hystOffset != 0) {
			hystStateL = prevScaledInputL; // Only track slope when offset active
			hystStateR = prevScaledInputR;
		}
	}

	// Hoist subharmonic intensity (for gain modulation based on subSign)
	// Sub requires extrasEnabled + gammaPhase != 0
	int32_t subBoost_Q16 = 0;
	if (extrasEnabled && zcCountL && zcCountR && subSignL && subSignR && gammaPhase != 0.0f) {
		int32_t subIntensity_Q16 = shaper.getSubIntensity_Q16();
		// Pre-compute boost amount: subIntensity * maxBoost >> 16
		subBoost_Q16 = static_cast<int32_t>((static_cast<int64_t>(subIntensity_Q16) * kSubBoostMax_Q16) >> 16);
	}

	// Drift slope setup: separate random walks per channel with phi-controlled correlation
	// Drift requires extrasEnabled + gammaPhase != 0 (X encoder toggle gates drift/sub)
	// Two drift modes with bipolar intensities at uncorrelated phi frequencies:
	// - Multiplicative: positive=sag toward zero, negative=boost away from zero
	// - Additive: positive=pull toward center, negative=push from center
	bool driftActive = extrasEnabled && driftSlopeL && driftSlopeR && driftAccumL && driftAccumR && driftLfsr
	                   && prevSampleL && prevSampleR && gammaPhase != 0.0f;
	int32_t slopeL = 0;
	int32_t slopeR = 0;
	int32_t driftMultIntensity_Q16 = 0;
	int32_t driftAddIntensity_Q16 = 0;
	if (driftActive) {
		driftMultIntensity_Q16 = shaper.getDriftMultIntensity_Q16();
		driftAddIntensity_Q16 = shaper.getDriftAddIntensity_Q16();
		// Scale slope range by absolute intensity (higher intensity = wider slope range)
		int32_t absMultIntensity = (driftMultIntensity_Q16 < 0) ? -driftMultIntensity_Q16 : driftMultIntensity_Q16;
		int32_t absAddIntensity = (driftAddIntensity_Q16 < 0) ? -driftAddIntensity_Q16 : driftAddIntensity_Q16;
		// Use max of both intensities for slope scaling (both need the slope walker)
		int32_t maxIntensity = std::max(absMultIntensity, absAddIntensity);
		int32_t maxSlope = static_cast<int32_t>((static_cast<int64_t>(kDriftSlopeMax) * maxIntensity) >> 16);
		int32_t minSlope = static_cast<int32_t>((static_cast<int64_t>(kDriftSlopeMin) * maxIntensity) >> 16);
		// Ensure min doesn't exceed max (clamp if intensity is very low)
		minSlope = std::min(minSlope, maxSlope);

		// L channel: random walk using L signal entropy
		int32_t entropyL = buffer[buffer.size() / 2].l;
		*driftSlopeL = updateDriftSlopeRandomWalk(*driftSlopeL, *driftLfsr, entropyL, maxSlope, minSlope);
		slopeL = *driftSlopeL;

		// R channel: independent random walk using R signal entropy
		int32_t entropyR = buffer[buffer.size() / 2].r;
		int32_t rIndependent = updateDriftSlopeRandomWalk(*driftSlopeR, *driftLfsr, entropyR, maxSlope, minSlope);

		// Correlation control: |stereoOffset| = 0 means R follows L, |stereoOffset| = 1 means fully independent
		// Blend: R = L + (R_independent - L) * |stereoOffset|
		int32_t stereoOffset_Q16 = shaper.getDriftStereoOffset_Q16();
		int32_t absOffset = (stereoOffset_Q16 < 0) ? -stereoOffset_Q16 : stereoOffset_Q16;
		int32_t diff = rIndependent - slopeL;
		*driftSlopeR = slopeL + static_cast<int32_t>((static_cast<int64_t>(diff) * absOffset) >> 16);
		slopeR = *driftSlopeR;
	}

	// Lowpass filter setup: compute alpha from intensity (scaled by note frequency)
	// Active when filter state pointers provided and slewIntensity > 0
	// (slewIntensity is set > 0 for gammaPhase > 0 OR square waves)
	bool lpfActive = slewedL && slewedR;
	int32_t lpfAlpha_Q16 = 0;
	if (lpfActive) {
		int32_t slewIntensity_Q16 = shaper.getSlewIntensity_Q16();
		if (slewIntensity_Q16 > 0) {
			lpfAlpha_Q16 = computeLpfAlpha_Q16(slewIntensity_Q16, noteFreqHz);
		}
		else {
			lpfActive = false; // Intensity 0 = disabled
		}
	}

	// Build per-buffer context (hoisted values for per-sample helper)
	ShaperBufferContext ctx{
	    .blendSlope_Q8 = blendSlope_Q8,
	    .threshold64 = threshold64,
	    .tableIdx = tableIdx,
	    .hystOffset = hystOffset,
	    .subBoost_Q16 = subBoost_Q16,
	    .driftMultIntensity_Q16 = driftMultIntensity_Q16,
	    .driftAddIntensity_Q16 = driftAddIntensity_Q16,
	    .lpfAlpha_Q16 = lpfAlpha_Q16,
	    .attenGain_Q16 = attenGain_Q16,
	    .isLinear = isLinear,
	    .lpfActive = lpfActive,
	    .driftActive = driftActive,
	    .needsGainAdjust = needsGainAdjust,
	    .extrasEnabled = extrasEnabled,
	};

	// Fast path: linear bypass (X=0 or table not ready)
	if (ctx.isLinear) {
		for (auto& sample : buffer) {
			gainCtx.current += multiply_32x32_rshift32(gainCtx.target - gainCtx.current, gainCtx.alpha) * 2;
			mixCtx.current += ((mixCtx.target - mixCtx.current) * mixCtx.alpha) >> 16;
			// Apply drive only (consistent with shaped path)
			sample.l = multiply_32x32_rshift32(sample.l, gainCtx.current) << 6;
			sample.r = multiply_32x32_rshift32(sample.r, gainCtx.current) << 6;
		}
		*smoothedDriveGain = gainCtx.current;
		*smoothedMixNorm_Q16 = mixCtx.current;
		return;
	}

	for (auto& sample : buffer) {
		gainCtx.current += multiply_32x32_rshift32(gainCtx.target - gainCtx.current, gainCtx.alpha) * 2;
		// Q16 IIR smoothing: current += (target - current) * alpha >> 16
		mixCtx.current += ((mixCtx.target - mixCtx.current) * mixCtx.alpha) >> 16;

		q31_t inputL = sample.l;
		q31_t inputR = sample.r;

		// Drift: two complementary effects that reset on zero crossings
		// - Multiplicative: gain sag/boost (capacitor discharge / charge)
		// - Additive: DC offset pull/push (toward or from center)
		// Stereo: separate accumulators with phi-correlated slopes for analog character
		int32_t driftGainL_Q16 = 0; // 0 = disabled, >0 = apply multiplicative gain
		int32_t driftGainR_Q16 = 0;
		int32_t driftOffsetL_Q16 = 0; // 0 = disabled, additive DC offset
		int32_t driftOffsetR_Q16 = 0;
		if (driftActive) {
			int32_t prevL = *prevSampleL;
			int32_t prevR = *prevSampleR;
			*prevSampleL = inputL;
			*prevSampleR = inputR;

			// L channel: reset on L zero crossing, accumulate with L slope
			bool zcL = (inputL ^ prevL) < 0;
			if (zcL) {
				*driftAccumL = 0;
				// Subharmonic: toggle sign every 2nd ZC (full wave cycle modulation)
				// Toggle on EVEN counts so both halves of each cycle get same treatment
				// (extrasEnabled already checked via driftActive gate)
				if (zcCountL && subSignL) {
					(*zcCountL)++;
					if ((*zcCountL & 1) == 0) {
						*subSignL = -*subSignL;
					}
				}
			}
			else {
				*driftAccumL += slopeL;
			}

			// R channel: reset on R zero crossing, accumulate with R slope (phi-correlated)
			bool zcR = (inputR ^ prevR) < 0;
			if (zcR) {
				*driftAccumR = 0;
				// Subharmonic: toggle sign every 2nd ZC (full wave cycle modulation)
				// Toggle on EVEN counts so both halves of each cycle get same treatment
				// (extrasEnabled already checked via driftActive gate)
				if (zcCountR && subSignR) {
					(*zcCountR)++;
					if ((*zcCountR & 1) == 0) {
						*subSignR = -*subSignR;
					}
				}
			}
			else {
				*driftAccumR += slopeR;
			}

			// Compute drift effects from accumulators based on bipolar intensities
			// Shift 11 gives ~30% max effect at low frequencies, ~10% at mid frequencies
			int32_t baseEffectL = *driftAccumL >> 11;
			int32_t baseEffectR = *driftAccumR >> 11;

			// Multiplicative drift: positive intensity = sag (toward zero), negative = boost (away)
			if (driftMultIntensity_Q16 != 0) {
				int32_t scaledMultL =
				    static_cast<int32_t>((static_cast<int64_t>(baseEffectL) * driftMultIntensity_Q16) >> 16);
				int32_t scaledMultR =
				    static_cast<int32_t>((static_cast<int64_t>(baseEffectR) * driftMultIntensity_Q16) >> 16);
				driftGainL_Q16 = std::clamp(65536 - scaledMultL, int32_t{0}, int32_t{131072});
				driftGainR_Q16 = std::clamp(65536 - scaledMultR, int32_t{0}, int32_t{131072});
			}

			// Additive drift: positive intensity = pull toward center, negative = push from center
			if (driftAddIntensity_Q16 != 0) {
				driftOffsetL_Q16 =
				    static_cast<int32_t>((static_cast<int64_t>(baseEffectL) * driftAddIntensity_Q16) >> 16);
				driftOffsetR_Q16 =
				    static_cast<int32_t>((static_cast<int64_t>(baseEffectR) * driftAddIntensity_Q16) >> 16);
			}
		}

		// Process L/R samples through shared helper (drive → slew → drift+sub → additive → shaper)
		int8_t currentSubSignL = (subSignL && ctx.subBoost_Q16 != 0) ? *subSignL : 1;
		int8_t currentSubSignR = (subSignR && ctx.subBoost_Q16 != 0) ? *subSignR : 1;

		q31_t outL = processShaperSample(inputL, gainCtx.current, ctx, slewedL, hystStateL, driftGainL_Q16,
		                                 currentSubSignL, driftOffsetL_Q16, shaper);
		q31_t outR = processShaperSample(inputR, gainCtx.current, ctx, slewedR, hystStateR, driftGainR_Q16,
		                                 currentSubSignR, driftOffsetR_Q16, shaper);

		if (ctx.needsGainAdjust) {
			outL = static_cast<q31_t>((static_cast<int64_t>(outL) * ctx.attenGain_Q16) >> 16);
			outR = static_cast<q31_t>((static_cast<int64_t>(outR) * ctx.attenGain_Q16) >> 16);
		}

		sample.l = outL;
		sample.r = outR;
	}

	*smoothedDriveGain = gainCtx.current;
	*smoothedMixNorm_Q16 = mixCtx.current;
}

} // namespace deluge::dsp
