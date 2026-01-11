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

/// Drift slope range and evolution rate (50% increased from 70k/80k)
/// Slope is always positive (unipolar), oscillates between [min, max] via random walk
/// Phi triangle intensity (bipolar) scales and applies polarity (sag vs boost)
constexpr int32_t kDriftSlopeMin = 105000; // Tight range (~12% variation)
constexpr int32_t kDriftSlopeMax = 120000; // Max slope (scaled by phi triangle intensity)
constexpr int32_t kDriftSlopeStep = 8;     // Very slow evolution (~30+ sec to traverse range)

/// Subharmonic gain modulation: maximum cut/boost amount at full intensity (25% decreased from 26214)
/// 19660 Q16 = ~30% (0.7x when subSign=+1, 1.3x when subSign=-1)
constexpr int32_t kSubBoostMax_Q16 = 19660;

/// Slew rate limiting: controls max signal change per sample
/// Creates trapezoid from square, softens transients in complex material
/// At intensity 0: maxSlew = kSlewMax (effectively disabled)
/// At intensity 1: maxSlew = kSlewMin (extreme limiting, ~500 samples for full swing)
/// Using exponential mapping for better control at subtle settings
constexpr int32_t kSlewMin = 4000000;   // Extreme: full swing in ~500 samples (2B/4M)
constexpr int32_t kSlewMax = 500000000; // Subtle: full swing in ~4 samples

/// Compute maxSlew from intensity using squared curve for better low-end control
/// intensity 0 → kSlewMax (no limiting), intensity 1 → kSlewMin (extreme)
[[gnu::always_inline]] inline int32_t computeMaxSlew(int32_t slewIntensity_Q16) {
	// Squared intensity for more control at subtle settings
	int64_t intensitySquared = (static_cast<int64_t>(slewIntensity_Q16) * slewIntensity_Q16) >> 16;
	// Interpolate from kSlewMax down to kSlewMin
	int64_t range = kSlewMax - kSlewMin;
	return kSlewMax - static_cast<int32_t>((range * intensitySquared) >> 16);
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

/**
 * Process a mono buffer through the TableShaper using integer-only path
 *
 * Table operates at FM signal levels. For subtractive synths, pass filterGain to
 * dynamically compute the boost needed to match FM operating levels.
 *
 * Optimizations:
 * - p^5 drive curve computed once per buffer, Q26 gain smoothed per-sample
 * - Filter gain adjustment uses Q16 fixed-point (one float divide per buffer)
 *
 * @param buffer Audio buffer to process in-place
 * @param shaper The Shaper instance (with pre-generated table)
 * @param drive Patched drive parameter (q31)
 * @param smoothedDriveGain Previous driveGain_Q26 value for smoothing (updated, stores Q26 gain not raw drive)
 * @param mix Wet/dry blend (q31, 0 = bypass)
 * @param smoothedMixNorm_Q16 Previous mixNorm value for smoothing (Q16.16 format, updated)
 * @param filterGain For subtractive mode: pass the filterGain from filter config.
 *                   For FM mode or subtractive without filters: pass 0.
 *                   When 0, no boost is applied (FM mode) or fixed boost for no-filter case.
 * @param hasFilters For subtractive mode: true if filters are active
 * @param prevScaledInput Pointer to previous scaled input for hysteresis (updated, can be null to disable)
 * @param driftSlope Pointer to drift slope state (rate of DC offset accumulation per sample)
 * @param driftAccum Pointer to accumulated DC offset (resets on zero crossings)
 * @param driftLfsr Pointer to LFSR state for random walk entropy
 * @param prevSample Pointer to previous sample for zero-crossing detection
 * @param zcCount Pointer to zero-crossing counter (for subharmonic)
 * @param subSign Pointer to subharmonic sign (±1)
 * @param subEnabled Whether subharmonic effect is enabled
 * @param phaseOffset Secret knob phase offset for phi triangles (0 = drift disabled)
 * @param slewed Pointer to slew rate limiter state (previous output)
 */
inline void shapeBufferInt32(std::span<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDriveGain,
                             q31_t mix, int32_t* smoothedMixNorm_Q16, q31_t filterGain, bool hasFilters,
                             int32_t* prevScaledInput, int32_t* driftSlope, int32_t* driftAccum, uint32_t* driftLfsr,
                             int32_t* prevSample, uint8_t* zcCount, int8_t* subSign, bool subEnabled, float phaseOffset,
                             int32_t* slewed) {
	if (buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	// Compute target driveGain ONCE (hoisted p^5 calculation)
	int32_t targetGain_Q26 = TableShaper::driveToGainQ26(drive);
	// Smooth driveGain_Q26 (not raw drive) - stored value is Q26 gain
	auto gainCtx = prepareShaperSmoothing(*smoothedDriveGain, targetGain_Q26);

	// Compute gain adjustment for subtractive mode (fixed-point, computed once per buffer)
	// filterGain=0 means FM mode (no adjustment needed)
	// filterGain>0 means subtractive: compensate for resonance-induced level changes
	// At neutral filterGain (2^28), gains = 1.0 (no adjustment)
	// High resonance (low filterGain) → boost; low resonance (high filterGain) → attenuate
	bool needsGainAdjust = (filterGain > 0) && hasFilters && (filterGain != kShaperNeutralFilterGainInt);
	int32_t boostGain_Q16 = 65536; // 1.0 in Q16
	int32_t attenGain_Q16 = 65536; // 1.0 in Q16

	if (needsGainAdjust) {
		// One float divide per buffer, then convert to Q16 for per-sample integer math
		float ratio = static_cast<float>(kShaperNeutralFilterGainInt) / static_cast<float>(filterGain);
		boostGain_Q16 = static_cast<int32_t>(ratio * 65536.0f);
		attenGain_Q16 = static_cast<int32_t>((1.0f / ratio) * 65536.0f);
	}

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

	// Hoist hysteresis offset (always enabled when state pointer provided)
	int32_t hystOffset = prevScaledInput ? shaper.getHystOffset() : 0;

	// Hoist subharmonic intensity (for gain modulation based on subSign)
	int32_t subBoost_Q16 = 0;
	if (subEnabled && zcCount && subSign && phaseOffset != 0.0f) {
		int32_t subIntensity_Q16 = shaper.getSubIntensity_Q16();
		// Pre-compute boost amount: subIntensity * maxBoost >> 16
		subBoost_Q16 = static_cast<int32_t>((static_cast<int64_t>(subIntensity_Q16) * kSubBoostMax_Q16) >> 16);
	}

	// Drift slope setup: random walk controls rate of DC offset accumulation
	// Drift is always active when phaseOffset != 0 (secret knob enables it)
	// Two drift modes with bipolar intensities at uncorrelated phi frequencies:
	// - Multiplicative: positive=sag toward zero, negative=boost away from zero
	// - Additive: positive=pull toward center, negative=push from center
	bool driftActive = driftSlope && driftAccum && driftLfsr && prevSample && phaseOffset != 0.0f;
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
		*driftSlope = updateDriftSlopeRandomWalk(*driftSlope, *driftLfsr, entropy, maxSlope, minSlope);
	}

	// Slew rate limiting setup: compute maxSlew from intensity
	// Active when phaseOffset != 0 and slew pointer provided
	bool slewActive = slewed && phaseOffset != 0.0f;
	int32_t maxSlew = kSlewMax; // Effectively disabled by default
	if (slewActive) {
		int32_t slewIntensity_Q16 = shaper.getSlewIntensity_Q16();
		if (slewIntensity_Q16 > 0) {
			maxSlew = computeMaxSlew(slewIntensity_Q16);
		}
		else {
			slewActive = false; // Intensity 0 = disabled
		}
	}

	// Fast path: linear bypass (X=0 or table not ready)
	if (isLinear) {
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
			int32_t prev = *prevSample;
			*prevSample = input;
			// Zero crossing detection: reset accumulator
			bool zc = (input ^ prev) < 0;
			if (zc) {
				*driftAccum = 0;
				// Subharmonic: toggle sign every 2nd ZC (full wave cycle modulation)
				// Toggle on EVEN counts so both halves of each cycle get same treatment
				if (subEnabled && zcCount && subSign) {
					(*zcCount)++;
					if ((*zcCount & 1) == 0) {
						*subSign = -*subSign;
					}
				}
			}
			else {
				// Accumulate: grows linearly within each half-cycle
				*driftAccum += *driftSlope;
			}

			// Compute drift effects from accumulator based on bipolar intensities
			// Shift 11 gives ~30% max effect at low frequencies, ~10% at mid frequencies
			int32_t baseEffect = *driftAccum >> 11;

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

		if (needsGainAdjust) {
			// Saturating Q16 multiply using SSAT with LSL (single ARM instruction)
			input = shift_left_saturate<16, 32>(multiply_32x32_rshift32(input, boostGain_Q16));
		}

		// Apply drive ONCE before splitting wet/dry paths
		// This saves one SMMUL+saturate per sample vs applying inside shaper
		q31_t drivenInput = shift_left_saturate<6, 32>(multiply_32x32_rshift32(input, gainCtx.current));

		// === Build wet path: all modifiers applied to driven signal ===
		// Order: slew → mult → sub → additive (additive last = pure DC offset to transfer curve)
		q31_t wetInput = drivenInput;

		// 1. Slew: input conditioning, soften transients
		if (slewActive) {
			int32_t delta = drivenInput - *slewed;
			delta = std::clamp(delta, -maxSlew, maxSlew);
			*slewed += delta;
			wetInput = *slewed;
		}

		// 2. Multiplicative drift: amplitude sag/boost (capacitor/power supply effects)
		if (driftGain_Q16 != 65536 && driftGain_Q16 != 0) {
			wetInput = static_cast<q31_t>((static_cast<int64_t>(wetInput) * driftGain_Q16) >> 16);
		}

		// 3. Subharmonic: octave-down gain modulation
		if (subBoost_Q16 != 0) {
			int32_t subGain_Q16 = 65536 - (*subSign) * subBoost_Q16;
			wetInput = static_cast<q31_t>((static_cast<int64_t>(wetInput) * subGain_Q16) >> 16);
		}

		// 4. Additive drift: DC offset determines operating point on transfer curve
		// Applied last so offset is drive-independent (fixed curve shift regardless of drive level)
		if (driftOffset_Q16 != 0) {
			int32_t offset = -driftOffset_Q16 << 8;
			wetInput = add_saturate(wetInput, offset);
		}

		// Shaper: both paths already driven, shaper only does table scaling
		q31_t out = shaper.processWithGainHoisted(wetInput, drivenInput, blendSlope_Q8, threshold64, tableIdx,
		                                          hystOffset, prevScaledInput);

		if (needsGainAdjust) {
			// Q16 multiply for attenuation (no overflow possible since attenGain <= 1 when boost >= 1)
			out = static_cast<q31_t>((static_cast<int64_t>(out) * attenGain_Q16) >> 16);
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
 * @param subEnabled Whether subharmonic effect is enabled
 * @param phaseOffset Secret knob phase offset for phi triangles (0 = drift disabled)
 * @param slewedL Pointer to L channel slew rate limiter state (previous output)
 * @param slewedR Pointer to R channel slew rate limiter state (previous output)
 */
inline void shapeBufferInt32(StereoBuffer<q31_t> buffer, TableShaper& shaper, q31_t drive, q31_t* smoothedDriveGain,
                             q31_t mix, int32_t* smoothedMixNorm_Q16, q31_t filterGain, bool hasFilters,
                             int32_t* prevScaledInputL, int32_t* prevScaledInputR, int32_t* driftSlopeL,
                             int32_t* driftSlopeR, int32_t* driftAccumL, int32_t* driftAccumR, uint32_t* driftLfsr,
                             int32_t* prevSampleL, int32_t* prevSampleR, uint8_t* zcCountL, uint8_t* zcCountR,
                             int8_t* subSignL, int8_t* subSignR, bool subEnabled, float phaseOffset, int32_t* slewedL,
                             int32_t* slewedR) {
	if (buffer.empty()) {
		return;
	}

	FX_BENCH_DECLARE(bench, "shaper_table");
	FX_BENCH_SCOPE(bench);

	// Compute target driveGain ONCE (hoisted p^5 calculation)
	int32_t targetGain_Q26 = TableShaper::driveToGainQ26(drive);
	// Smooth driveGain_Q26 (not raw drive) - stored value is Q26 gain
	auto gainCtx = prepareShaperSmoothing(*smoothedDriveGain, targetGain_Q26);

	// Compute gain adjustment for subtractive mode (fixed-point, computed once per buffer)
	// filterGain=0 means FM mode (no adjustment needed)
	// filterGain>0 means subtractive: compensate for resonance-induced level changes
	// At neutral filterGain (2^28), gains = 1.0 (no adjustment)
	// High resonance (low filterGain) → boost; low resonance (high filterGain) → attenuate
	bool needsGainAdjust = (filterGain > 0) && hasFilters && (filterGain != kShaperNeutralFilterGainInt);
	int32_t boostGain_Q16 = 65536; // 1.0 in Q16
	int32_t attenGain_Q16 = 65536; // 1.0 in Q16

	if (needsGainAdjust) {
		// One float divide per buffer, then convert to Q16 for per-sample integer math
		float ratio = static_cast<float>(kShaperNeutralFilterGainInt) / static_cast<float>(filterGain);
		boostGain_Q16 = static_cast<int32_t>(ratio * 65536.0f);
		attenGain_Q16 = static_cast<int32_t>((1.0f / ratio) * 65536.0f);
	}

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

	// Hoist hysteresis offset (always enabled when state pointers provided)
	int32_t hystOffset = (prevScaledInputL && prevScaledInputR) ? shaper.getHystOffset() : 0;

	// Hoist subharmonic intensity (for gain modulation based on subSign)
	int32_t subBoost_Q16 = 0;
	if (subEnabled && zcCountL && zcCountR && subSignL && subSignR && phaseOffset != 0.0f) {
		int32_t subIntensity_Q16 = shaper.getSubIntensity_Q16();
		// Pre-compute boost amount: subIntensity * maxBoost >> 16
		subBoost_Q16 = static_cast<int32_t>((static_cast<int64_t>(subIntensity_Q16) * kSubBoostMax_Q16) >> 16);
	}

	// Drift slope setup: separate random walks per channel with phi-controlled correlation
	// Drift is always active when phaseOffset != 0 (secret knob enables it)
	// Two drift modes with bipolar intensities at uncorrelated phi frequencies:
	// - Multiplicative: positive=sag toward zero, negative=boost away from zero
	// - Additive: positive=pull toward center, negative=push from center
	bool driftActive = driftSlopeL && driftSlopeR && driftAccumL && driftAccumR && driftLfsr && prevSampleL
	                   && prevSampleR && phaseOffset != 0.0f;
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

	// Slew rate limiting setup: compute maxSlew from intensity
	// Active when phaseOffset != 0 and slew pointers provided
	bool slewActive = slewedL && slewedR && phaseOffset != 0.0f;
	int32_t maxSlew = kSlewMax; // Effectively disabled by default
	if (slewActive) {
		int32_t slewIntensity_Q16 = shaper.getSlewIntensity_Q16();
		if (slewIntensity_Q16 > 0) {
			maxSlew = computeMaxSlew(slewIntensity_Q16);
		}
		else {
			slewActive = false; // Intensity 0 = disabled
		}
	}

	// Fast path: linear bypass (X=0 or table not ready)
	if (isLinear) {
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
				if (subEnabled && zcCountL && subSignL) {
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
				if (subEnabled && zcCountR && subSignR) {
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

		if (needsGainAdjust) {
			// Saturating Q16 multiply using SSAT with LSL (single ARM instruction)
			inputL = shift_left_saturate<16, 32>(multiply_32x32_rshift32(inputL, boostGain_Q16));
			inputR = shift_left_saturate<16, 32>(multiply_32x32_rshift32(inputR, boostGain_Q16));
		}

		// Apply drive ONCE before splitting wet/dry paths
		// This saves two SMMUL+saturate per stereo sample vs applying inside shaper
		q31_t drivenInputL = shift_left_saturate<6, 32>(multiply_32x32_rshift32(inputL, gainCtx.current));
		q31_t drivenInputR = shift_left_saturate<6, 32>(multiply_32x32_rshift32(inputR, gainCtx.current));

		// === Build wet paths: all modifiers applied to driven signals ===
		// Order: slew → mult → sub → additive (additive last = pure DC offset to transfer curve)
		q31_t wetInputL = drivenInputL;
		q31_t wetInputR = drivenInputR;

		// 1. Slew: input conditioning, soften transients
		if (slewActive) {
			int32_t deltaL = drivenInputL - *slewedL;
			int32_t deltaR = drivenInputR - *slewedR;
			deltaL = std::clamp(deltaL, -maxSlew, maxSlew);
			deltaR = std::clamp(deltaR, -maxSlew, maxSlew);
			*slewedL += deltaL;
			*slewedR += deltaR;
			wetInputL = *slewedL;
			wetInputR = *slewedR;
		}

		// 2. Multiplicative drift: amplitude sag/boost (capacitor/power supply effects)
		if (driftGainL_Q16 != 65536 && driftGainL_Q16 != 0) {
			wetInputL = static_cast<q31_t>((static_cast<int64_t>(wetInputL) * driftGainL_Q16) >> 16);
		}
		if (driftGainR_Q16 != 65536 && driftGainR_Q16 != 0) {
			wetInputR = static_cast<q31_t>((static_cast<int64_t>(wetInputR) * driftGainR_Q16) >> 16);
		}

		// 3. Subharmonic: octave-down gain modulation
		if (subBoost_Q16 != 0) {
			int32_t subGainL_Q16 = 65536 - (*subSignL) * subBoost_Q16;
			int32_t subGainR_Q16 = 65536 - (*subSignR) * subBoost_Q16;
			wetInputL = static_cast<q31_t>((static_cast<int64_t>(wetInputL) * subGainL_Q16) >> 16);
			wetInputR = static_cast<q31_t>((static_cast<int64_t>(wetInputR) * subGainR_Q16) >> 16);
		}

		// 4. Additive drift: DC offset determines operating point on transfer curve
		// Applied last so offset is drive-independent (fixed curve shift regardless of drive level)
		if (driftOffsetL_Q16 != 0 || driftOffsetR_Q16 != 0) {
			int32_t offsetL = -driftOffsetL_Q16 << 8;
			int32_t offsetR = -driftOffsetR_Q16 << 8;
			wetInputL = add_saturate(wetInputL, offsetL);
			wetInputR = add_saturate(wetInputR, offsetR);
		}

		// Shaper: both paths already driven, shaper only does table scaling
		q31_t outL = shaper.processWithGainHoisted(wetInputL, drivenInputL, blendSlope_Q8, threshold64, tableIdx,
		                                           hystOffset, prevScaledInputL);
		q31_t outR = shaper.processWithGainHoisted(wetInputR, drivenInputR, blendSlope_Q8, threshold64, tableIdx,
		                                           hystOffset, prevScaledInputR);

		if (needsGainAdjust) {
			// Q16 multiply for attenuation (no overflow possible since attenGain <= 1 when boost >= 1)
			outL = static_cast<q31_t>((static_cast<int64_t>(outL) * attenGain_Q16) >> 16);
			outR = static_cast<q31_t>((static_cast<int64_t>(outR) * attenGain_Q16) >> 16);
		}

		sample.l = outL;
		sample.r = outR;
	}

	*smoothedDriveGain = gainCtx.current;
	*smoothedMixNorm_Q16 = mixCtx.current;
}

} // namespace deluge::dsp
