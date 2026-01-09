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

#include "dsp/fast_math.h"
#include "dsp/phi_triangle.hpp"
#include "dsp/util.hpp"
#include "util/fixedpoint.h"
#include "util/functions.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace deluge::dsp {

/**
 * Parameters for table-based shaper - consolidated for efficient passing
 *
 * All parameters are normalized 0-1 range:
 * - drive: Overall intensity (0 = bypass)
 * - tanhWeight: Tanh basis weight (warm, smooth)
 * - polyWeight: Polynomial basis weight (bright, edgy)
 * - hardKneeWeight: Hard knee basis weight (crisp, aggressive)
 * - chebyWeight: Chebyshev T5 basis weight (fold, synthy)
 * - sineFoldWeight: Sine folder basis weight (harmonic-rich)
 * - rectifierWeight: Rectifier basis weight (diode, asymmetric)
 * - threshold: Linear zone size (1 = all linear, 0 = always saturate)
 * - asymmetry: Even harmonics (0.5 = symmetric)
 */
struct TableShaperParams {
	float drive{0.0f};
	float tanhWeight{1.0f};
	float polyWeight{0.0f};
	float hardKneeWeight{0.0f};
	float chebyWeight{0.0f};
	float sineFoldWeight{0.0f};
	float rectifierWeight{0.0f};
	float threshold{1.0f};
	float asymmetry{0.5f};
	float deadzoneWidth{0.0f}; // 0 = no deadzone, 1 = 90% deadzone (10% passthrough)
	float deadzonePhase{0.5f}; // Center of passthrough window: 0.5 = x=0 (zero crossing)

	/// Clamp all parameters to valid 0-1 range
	void clamp() {
		drive = std::clamp(drive, 0.0f, 1.0f);
		tanhWeight = std::clamp(tanhWeight, 0.0f, 1.0f);
		polyWeight = std::clamp(polyWeight, 0.0f, 1.0f);
		hardKneeWeight = std::clamp(hardKneeWeight, 0.0f, 1.0f);
		chebyWeight = std::clamp(chebyWeight, 0.0f, 1.0f);
		sineFoldWeight = std::clamp(sineFoldWeight, 0.0f, 1.0f);
		rectifierWeight = std::clamp(rectifierWeight, 0.0f, 1.0f);
		threshold = std::clamp(threshold, 0.0f, 1.0f);
		asymmetry = std::clamp(asymmetry, 0.0f, 1.0f);
		deadzoneWidth = std::clamp(deadzoneWidth, 0.0f, 1.0f);
		deadzonePhase = std::clamp(deadzonePhase, 0.0f, 1.0f);
	}

	bool operator!=(const TableShaperParams& o) const {
		return drive != o.drive || tanhWeight != o.tanhWeight || polyWeight != o.polyWeight
		       || hardKneeWeight != o.hardKneeWeight || chebyWeight != o.chebyWeight
		       || sineFoldWeight != o.sineFoldWeight || rectifierWeight != o.rectifierWeight || threshold != o.threshold
		       || asymmetry != o.asymmetry || deadzoneWidth != o.deadzoneWidth || deadzonePhase != o.deadzonePhase;
	}
};

/**
 * Table-based Parametric Shaper with ADAA (Antiderivative Antialiasing)
 *
 * Features:
 * - 6 basis functions for rich harmonic exploration
 * - Drive parameter where 0 = linear bypass (transparent)
 * - Separate weights for each basis function
 * - Cached f(x) and F(x) tables for fast lookup
 * - First-order ADAA using cached antiderivative table
 */
class TableShaperCore {
public:
	// =============================================================================
	// A/B TEST CONFIGURATION - Change these for testing different modes
	// =============================================================================
	// Table size: 128 (smallest, ~1KB), 256 (~2KB), 512 (~4KB), 2048 (~16KB)
	static constexpr size_t kTableSize = 2048;

	// Interpolation mode for function f(x) lookup:
	// false = linear (fast, ~10 ops)
	// true = cubic Catmull-Rom (slow, ~25 ops, smoother waveshaping)
	static constexpr bool kUseCubicFunction = false;

	// Interpolation mode for antiderivative F(x) lookup:
	// false = linear (fast, ~10 ops)
	// true = cubic Catmull-Rom (slow, ~25 ops, smoother ADAA)
	static constexpr bool kUseCubicAntiderivative = true;

	// Generate ADAA tables (float F(x) antiderivative):
	// false = int32 path only, saves ~16KB and speeds up regeneration
	// true = enables float ADAA path (process() with prevX state)
	static constexpr bool kGenerateADAA = false;

	// Energy-balanced phase compression:
	// false = midpoint centering only (full amplitude, DC handled downstream)
	// true = compress dominant-energy half to balance mean while keeping full amplitude
	// This warps the x-axis, subtly changing harmonic content but achieving both goals
	static constexpr bool kEnergyBalancedPhase = true;
	// =============================================================================

	static constexpr size_t kTableMask = kTableSize - 1;
	static constexpr float kTableScale = static_cast<float>(kTableSize) / 2.0f;

	TableShaperCore() = default; // Tables start empty, allocated on first non-linear use

	/// Set all parameters at once using struct
	/// Regenerates tables immediately (not deferred to audio thread)
	void setParameters(const TableShaperParams& p) {
		TableShaperParams clamped = p;
		clamped.clamp();
		if (clamped != params_) {
			params_ = clamped;
			regenerateTables();
		}
	}

	/// Get current parameters
	[[nodiscard]] const TableShaperParams& getParameters() const { return params_; }

	/// Check if effect is effectively bypassed (transparent)
	/// Only checks drive (X axis) - threshold shouldn't cause bypass since user explicitly set X > 0
	[[nodiscard]] bool isLinear() const { return params_.drive < 0.001f; }

	// TODO: Remove float ADAA path - not used, int32 path is the intended signal path
	/// Process a single sample with ADAA (uses internal state)
	/// @param x Input sample in range [-1, 1]
	/// @return Processed sample (peak-normalized)
	[[gnu::always_inline]] float process(float x) { return process(x, &prevX_); }

	/// ADAA state: stores both previous X and cached F(prevX) to avoid redundant lookup
	struct AdaaState {
		float prevX{0.0f};
		float prevF{0.0f}; // Cached F(prevX) - saves one table lookup per sample
	};

	/// Process a single sample with ADAA using external state
	/// This allows one TableShaper (one set of tables) to serve multiple channels
	/// @param x Input sample in range [-1, 1]
	/// @param prevXState Pointer to previous sample state (updated in place)
	/// @return Processed sample (peak-normalized)
	[[gnu::always_inline]] float process(float x, float* prevXState) {
		// Headroom: normalizationGain_ already handles peak limiting, minimal extra needed
		constexpr float kHeadroom = 0.8f;

		// Fast path: bypass when linear
		if (isLinear_) {
			*prevXState = x;
			return x * kHeadroom;
		}

		// First-order ADAA: output = (F(x) - F(x_prev)) / (x - x_prev)
		// Where F(x) is the antiderivative of f(x)
		float F_curr = lookupAntiderivative(x);
		float F_prev = lookupAntiderivative(*prevXState);

		float dx = x - *prevXState;
		float absDx = std::fabs(dx);
		float output;

		// Threshold for numerical stability - below this, blend with direct lookup
		// At 44.1kHz, a 1kHz full-scale sine changes by ~0.14 per sample
		constexpr float kMinDx = 0.01f;
		constexpr float kInvMinDx = 1.0f / kMinDx; // Precomputed reciprocal
		// Maximum dx for ADAA - above this, the antiderivative lookup becomes unreliable
		constexpr float kMaxDx = 0.5f;

		if (absDx < 1e-7f || absDx > kMaxDx) {
			// Essentially static OR large jump - use direct lookup
			output = lookupFunction(x);
		}
		else {
			// Fast reciprocal using NEON vrecpe + Newton-Raphson (~5 cycles vs ~15 for division)
			float invDx = fastReciprocal(dx);
			float adaaOutput = (F_curr - F_prev) * invDx;

			if (absDx >= kMinDx) {
				// Normal ADAA range - use ADAA output directly
				output = adaaOutput;
			}
			else {
				// Transition zone - smoothstep blend between ADAA and direct
				float directOutput = lookupFunction(x);

				// Smoothstep: t² * (3 - 2t) gives smooth S-curve from 0 to 1
				float t = absDx * kInvMinDx; // Multiply by precomputed reciprocal
				float blend = t * t * (3.0f - 2.0f * t);

				output = adaaOutput * blend + directOutput * (1.0f - blend);
			}
		}

		*prevXState = x;

		// Apply combined normalization and headroom
		float result = output * (normalizationGain_ * kHeadroom);

		// Safety clamp - prevent any extreme values that could cause clicks
		return std::clamp(result, -1.0f, 1.0f);
	}

	/// Process a single sample with ADAA using cached state (optimized - one lookup per sample)
	/// @param x Input sample in range [-1, 1]
	/// @param state Pointer to ADAA state (prevX and cached F(prevX))
	/// @return Processed sample (peak-normalized)
	[[gnu::always_inline]] float processWithCache(float x, AdaaState* state) {
		constexpr float kHeadroom = 0.8f;

		if (isLinear_) {
			state->prevX = x;
			state->prevF = x * x * 0.5f; // F(x) = x²/2 for linear
			return x * kHeadroom;
		}

		// Only one lookup - use cached F(prevX)
		float F_curr = lookupAntiderivative(x);
		float F_prev = state->prevF;

		float dx = x - state->prevX;
		float absDx = std::fabs(dx);
		float output;

		constexpr float kMinDx = 0.01f;
		constexpr float kInvMinDx = 1.0f / kMinDx;
		constexpr float kMaxDx = 0.5f;

		if (absDx < 1e-7f || absDx > kMaxDx) {
			output = lookupFunction(x);
		}
		else {
			float invDx = fastReciprocal(dx);
			float adaaOutput = (F_curr - F_prev) * invDx;

			if (absDx >= kMinDx) {
				output = adaaOutput;
			}
			else {
				float directOutput = lookupFunction(x);
				float t = absDx * kInvMinDx;
				float blend = t * t * (3.0f - 2.0f * t);
				output = adaaOutput * blend + directOutput * (1.0f - blend);
			}
		}

		// Update state with cached F for next sample
		state->prevX = x;
		state->prevF = F_curr;

		float result = output * (normalizationGain_ * kHeadroom);
		return std::clamp(result, -1.0f, 1.0f);
	}

	/// Process with ADAA using cubic interpolation (smoother, more expensive)
	[[gnu::always_inline]] float processCubic(float x, float* prevXState) {
		constexpr float kHeadroom = 0.8f;

		if (isLinear_) {
			*prevXState = x;
			return x * kHeadroom;
		}

		float F_curr = lookupAntiderivativeCubic(x);
		float F_prev = lookupAntiderivativeCubic(*prevXState);

		float dx = x - *prevXState;
		float absDx = std::fabs(dx);
		float output;

		constexpr float kMinDx = 0.01f;
		constexpr float kInvMinDx = 1.0f / kMinDx; // Precomputed reciprocal
		constexpr float kMaxDx = 0.5f;

		if (absDx < 1e-7f || absDx > kMaxDx) {
			output = lookupFunction(x);
		}
		else {
			// Fast reciprocal using NEON vrecpe + Newton-Raphson
			float invDx = fastReciprocal(dx);
			float adaaOutput = (F_curr - F_prev) * invDx;

			if (absDx >= kMinDx) {
				output = adaaOutput;
			}
			else {
				float directOutput = lookupFunction(x);
				float t = absDx * kInvMinDx; // Multiply by precomputed reciprocal
				float blend = t * t * (3.0f - 2.0f * t);
				output = adaaOutput * blend + directOutput * (1.0f - blend);
			}
		}

		*prevXState = x;
		float result = output * (normalizationGain_ * kHeadroom);
		return std::clamp(result, -1.0f, 1.0f);
	}

	// TODO: Remove float processNoAA - not used, int32 path is the intended signal path
	/// Process a single sample (direct table lookup, no ADAA)
	/// Note: Uses same headroom as process() for consistent levels when toggling AA
	[[gnu::always_inline]] float processNoAA(float x) {
		// Headroom: must match process() for level consistency
		constexpr float kHeadroom = 0.8f;

		if (isLinear_) {
			return x * kHeadroom;
		}

		// Apply peak normalization and headroom so levels match process() with ADAA
		return lookupFunction(x) * normalizationGain_ * kHeadroom;
	}

	/// Process a single sample using integer-only path with Q16 mix parameter
	/// @param input Input sample in raw signal format (e.g., ~23M for FM, ~1.4M for subtractive)
	/// @param driveGain_Q30 Pre-computed drive gain in Q30 format
	/// @param mixNorm_Q16 Normalized mix in Q16.16 (65536 = 1.0, 131072 = 2.0 full wet)
	/// @return Output sample at same level as input (unity gain when undriven)
	[[gnu::always_inline]] int32_t processInt32Q16(int32_t input, int32_t driveGain_Q30, int32_t mixNorm_Q16 = 131072) {
		// Linear drive with saturation
		// Range: ~0.25x at min drive to ~4x at max drive
		int32_t afterDrive = multiply_32x32_rshift32(input, driveGain_Q30) << 2;

		// Fast path: bypass when linear (tables may be deallocated)
		if (isLinear_) {
			return afterDrive; // Return driven signal (consistent with dry path)
		}

		// Scale to fill table range using bit shift (Fix 1: restored from float multiply)
		// Uses floor(log2(inputScale_)) for conservative boost
		int32_t scaledInput;
		if (afterDrive >= 0) {
			int32_t maxBeforeOverflow = INT32_MAX >> inputScaleShift_;
			scaledInput = (afterDrive > maxBeforeOverflow) ? INT32_MAX : (afterDrive << inputScaleShift_);
		}
		else {
			int32_t minBeforeOverflow = INT32_MIN >> inputScaleShift_;
			scaledInput = (afterDrive < minBeforeOverflow) ? INT32_MIN : (afterDrive << inputScaleShift_);
		}

		// Amplitude-dependent blend: threshold determines which amplitudes get wet
		// absInput is in [0, INT32_MAX] after scaling
		int32_t clampedInput = std::max(scaledInput, static_cast<int32_t>(-2147483647));
		int32_t absInput = clampedInput < 0 ? -clampedInput : clampedInput;

		// Threshold calculation for amplitude-dependent blend
		// At mixNorm=0: threshold = INT32_MAX (always dry)
		// At mixNorm=131072: threshold = kThresholdForFullWet (negative, ensures blend=1.0 for all amplitudes)
		constexpr int32_t kInt32Max = 2147483647;
		constexpr int64_t kInt32Max64 = 2147483647;
		constexpr int32_t kOne_Q16 = 65536;
		constexpr int32_t kMaxMix = 131072;

		// Quadratic slope: steeper at high mix for sharper transitions
		// At mixNorm=0: slope = kBaseSlope (Q8.8, = 1.0)
		// At mixNorm=kMaxMix: slope = kBaseSlope + (kMaxMix² >> kSlopeShift)
		// Then scaled by blendAggressionMult_Q8_ (from X): [0.5x, 2.0x]
		constexpr int32_t kBaseSlope = 256;
		constexpr int32_t kSlopeShift = 20;
		int64_t mixSquared = static_cast<int64_t>(mixNorm_Q16) * mixNorm_Q16;
		int32_t baseSlope = kBaseSlope + static_cast<int32_t>(mixSquared >> kSlopeShift);
		int32_t blendSlope_Q8 = (baseSlope * blendAggressionMult_Q8_) >> 8;

		// Derive threshold from slope formula to ensure blend=1.0 at any amplitude when mix is max
		// For blend_Q16 = 65536: diff_Q16 * slope >= 65536 << 8 = 16777216
		// At max slope: diff_Q16 >= 16777216 / maxSlope, round up
		// In Q31: diff >= requiredDiffQ16 << 15
		// For input=0 to get full blend: threshold = -diff
		constexpr int32_t kMaxSlope = kBaseSlope + ((static_cast<int64_t>(kMaxMix) * kMaxMix) >> kSlopeShift);
		constexpr int32_t kBlendTarget = kOne_Q16 << 8;                                  // 16777216
		constexpr int32_t kRequiredDiffQ16 = (kBlendTarget + kMaxSlope - 1) / kMaxSlope; // ceiling division
		constexpr int64_t kThresholdForFullWet = -(static_cast<int64_t>(kRequiredDiffQ16) << 15);

		// Linear threshold mapping: [INT32_MAX, kThresholdForFullWet]
		constexpr int64_t kThresholdRange = kInt32Max64 - kThresholdForFullWet; // 2,180,546,559
		int64_t threshold64 = kInt32Max64 - ((kThresholdRange * mixNorm_Q16) >> 17);

		// diff = absInput - threshold
		int64_t diff64 = static_cast<int64_t>(absInput) - threshold64;
		if (diff64 <= 0) {
			return afterDrive; // Return driven (but unshaped) signal
		}

		// Convert diff to Q16 for blend calculation
		int32_t diff_clamped = static_cast<int32_t>(std::min(diff64, static_cast<int64_t>(kInt32Max)));
		int32_t diff_Q16 = diff_clamped >> 15;

		// Linear blend calculation: blend = diff * slope
		int32_t blend_Q16 = (diff_Q16 * blendSlope_Q8) >> 8;

		// Clamp to [0, 65536] (0 to 1.0)
		if (blend_Q16 > kOne_Q16) {
			blend_Q16 = kOne_Q16;
		}

		// Table lookup (both scaledInput and lookup are clipped to INT32_MAX)
		uint32_t tableInput = static_cast<uint32_t>(scaledInput) + 2147483648u;
		int32_t lookup = lookupFunctionInt(tableInput);

		// Blend at scaled level (both signals clipped to same peak)
		int32_t blend_Q30 = blend_Q16 << 14;
		int32_t oneMinusBlend_Q30 = (kOne_Q16 << 14) - blend_Q30;

		int32_t dryPart = multiply_32x32_rshift32(scaledInput, oneMinusBlend_Q30) << 2;
		int32_t wetPart = multiply_32x32_rshift32(lookup, blend_Q30) << 2;
		int32_t blended = dryPart + wetPart;

		// Scale back to original level using bit shift (Fix 1: restored from float divide)
		return blended >> inputScaleShift_;
	}

	/// Process a single sample using integer-only path (legacy float interface)
	/// Uses stored inputScaleShift_ set via setExpectedPeak()
	/// @param input Input sample in raw signal format (e.g., ~23M for FM, ~1.4M for subtractive)
	/// @param driveGain_Q30 Pre-computed drive gain in Q30 format
	/// @param mixNorm Normalized mix (0 = full dry, 2 = full wet), for amplitude-dependent blend
	/// @return Output sample at same level as input (unity gain when undriven)
	[[gnu::always_inline]] int32_t processInt32(int32_t input, int32_t driveGain_Q30, float mixNorm = 2.0f) {
		// Convert float mixNorm to Q16 and call the integer version
		int32_t mixNorm_Q16 = static_cast<int32_t>(mixNorm * 65536.0f);
		return processInt32Q16(input, driveGain_Q30, mixNorm_Q16);
	}

	/// Reset ADAA state (call when starting a new audio stream)
	void reset() {
		prevX_ = 0.0f;
		// Tables don't need regeneration - params haven't changed
	}

	[[nodiscard]] float getNormalizationGain() const { return normalizationGain_; }

	/// Deallocate tables to free memory
	/// ~4KB when kGenerateADAA=false (int16 only), ~20KB when true (+ float tables)
	/// Called automatically when X=0 (linear bypass)
	void deallocateTables() {
		fTableInt_.clear();
		fTableInt_.shrink_to_fit();
		if constexpr (kGenerateADAA) {
			fTable_.clear();
			fTable_.shrink_to_fit();
			FTable_.clear();
			FTable_.shrink_to_fit();
		}
	}

	/// Check if tables are currently allocated
	[[nodiscard]] bool hasAllocatedTables() const { return !fTableInt_.empty(); }

private:
	/// Regenerate both f(x) and F(x) tables based on current parameters
	/// Uses fast math approximations for speed during parameter automation.
	/// When linear (X=0), deallocates tables to save memory (~20KB per instance).
	/// IMPORTANT: isLinear_ is set AFTER tables are fully populated to prevent
	/// audio thread from reading partially-initialized data.
	void regenerateTables() {
		tablesDirty_ = false;
		bool willBeLinear = isLinear(); // Check params, but don't set isLinear_ yet

		// Compute blend aggression from drive (X axis)
		// Quadratic curve: very gentle at low X, snappy at high X
		// Range: [0.1, 2.0] → 20x dynamic range
		// At X=0: slope so gentle that full mix range is needed for full blend
		// At X=127: snappy onset, wet kicks in quickly
		float driveSquared = params_.drive * params_.drive;
		float aggression = 0.1f + driveSquared * 1.9f; // [0.1, 2.0]
		blendAggressionMult_Q8_ = static_cast<int32_t>(aggression * 256.0f);

		if (willBeLinear) {
			// Linear bypass: deallocate tables to save memory
			// Process functions have isLinear_ fast-path that doesn't use tables
			deallocateTables();
			normalizationGain_ = 1.0f;
			normalizationGainInt_ = 32767;
			isLinear_ = true; // Safe to set - no tables to read
			return;
		}

		// Keep isLinear_ = true until tables are FULLY populated
		// This prevents audio thread from reading partial data

		// Allocate tables if needed (first non-linear use or after deallocation)
		if (fTableInt_.size() != kTableSize + 1) {
			fTableInt_.resize(kTableSize + 1);
			if constexpr (kGenerateADAA) {
				fTable_.resize(kTableSize + 1);
				FTable_.resize(kTableSize + 1);
			}
		}

		// Compute effective parameters
		// Drive affects steepness (k) and threshold reduction
		float drive = params_.drive;
		float k = 1.0f + drive * 9.0f;                               // Steepness: 1 to 10
		float T = params_.threshold * (1.0f - params_.drive * 0.8f); // Threshold shrinks with drive
		T = std::fmax(T, 0.05f);                                     // Never fully zero

		// Asymmetry ratio for positive vs negative
		float asymRatio = 0.5f + params_.asymmetry; // 0.5 to 1.5

		// Precompute inverse normalization factors for tanh (using fast approximation)
		float invTanhNormPos = 1.0f / std::fmax(0.01f, fastTanh(k * asymRatio));
		float invTanhNormNeg = 1.0f / std::fmax(0.01f, fastTanh(k * (2.0f - asymRatio)));

		// Precompute weight normalization for all 6 basis functions
		float weightSum = params_.tanhWeight + params_.polyWeight + params_.hardKneeWeight + params_.chebyWeight
		                  + params_.sineFoldWeight + params_.rectifierWeight;
		float invWeightSum = (weightSum > 0.001f) ? (1.0f / weightSum) : 1.0f;
		bool hasWeights = (weightSum >= 0.001f);

		// Lambda to evaluate transfer function at a given x position
		// Returns f(x) in [-1, +1] range
		auto evaluateTransfer = [&](float x) -> float {
			float mag = std::fabs(x);
			float sign = (x >= 0.0f) ? 1.0f : -1.0f;

			float f_val;

			if (mag < T) {
				// Linear zone
				f_val = x;
			}
			else {
				// Saturation zone - blend between basis functions
				float norm = (mag - T) / (1.0f - T); // 0 at threshold, 1 at max
				norm = std::fmin(norm, 1.0f);

				// Intensity: overdrive for richer harmonics
				float intensity = 1.0f + params_.drive * 1.0f;
				float overdriven = norm * intensity;

				// Asymmetric k for positive/negative
				float kEff = k * ((x >= 0.0f) ? asymRatio : (2.0f - asymRatio));
				float invTanhNorm = (x >= 0.0f) ? invTanhNormPos : invTanhNormNeg;

				// BASIS 1: Tanh (warm, smooth)
				float tanh_out = fastTanh(overdriven * kEff) * invTanhNorm;

				// BASIS 2: Polynomial soft clip (bright, edgy)
				float od2 = overdriven * overdriven;
				float od3 = od2 * overdriven;
				float od5 = od3 * od2;
				float poly_out = overdriven - od3 * 0.333333f + od5 * 0.2f;
				poly_out = fastTanh(poly_out);

				// BASIS 3: Hard clip (crisp, aggressive)
				float hardClip_out = std::fmin(std::fmax(overdriven, -1.0f), 1.0f);

				// BASIS 4: Chebyshev T5 wavefolder (fold, synthy)
				float cheby_in = overdriven * 1.2f;
				float cheby_in2 = cheby_in * cheby_in;
				float cheby_in3 = cheby_in2 * cheby_in;
				float cheby_in5 = cheby_in3 * cheby_in2;
				float cheby_raw = 16.0f * cheby_in5 - 20.0f * cheby_in3 + 5.0f * cheby_in;
				float cheby_phase = std::fmod(cheby_raw + 1.0f, 4.0f);
				if (cheby_phase < 0.0f)
					cheby_phase += 4.0f;
				float cheby_out = (cheby_phase <= 2.0f) ? (cheby_phase - 1.0f) : (3.0f - cheby_phase);
				cheby_out = std::fabs(cheby_out);

				// BASIS 5: Sine folder (Gold)
				constexpr float kSineFoldA = 0.4f;
				float sineFoldB = 3.14159265f * (1.0f + drive * 1.0f);
				float sineFold_raw =
				    fastTanh(overdriven / kSineFoldA) * std::sin(sineFoldB * overdriven) + fastTanh(overdriven) * 0.3f;
				float sineFold_out = std::fabs(sineFold_raw);
				sineFold_out = std::fmin(sineFold_out, 1.0f);

				// BASIS 6: Rectifier (diode)
				float bias = 0.2f * drive;
				float rect_raw = std::fabs(overdriven + bias) - bias;
				float rect_out = fastTanh(rect_raw * 2.0f);

				// Blend using weights
				float basis_out;
				if (!hasWeights) {
					basis_out = norm;
				}
				else {
					basis_out = (tanh_out * params_.tanhWeight + poly_out * params_.polyWeight
					             + hardClip_out * params_.hardKneeWeight + cheby_out * params_.chebyWeight
					             + sineFold_out * params_.sineFoldWeight + rect_out * params_.rectifierWeight)
					            * invWeightSum;
				}

				// Drive-dependent blend toward linear
				basis_out = norm + (basis_out - norm) * drive;

				// Map back to output range
				f_val = sign * (T + (1.0f - T) * std::fabs(basis_out));
			}

			// Apply deadzone modifier: use tiny epsilon instead of hard zero
			// This preserves some signal for DC balancing while being effectively silent
			if (params_.deadzoneWidth > 0.001f) {
				// Max 80% deadzone (20% minimum passthrough) to avoid extreme DC imbalance
				float passthrough = 1.0f - 0.8f * params_.deadzoneWidth;
				float centerX = params_.deadzonePhase * 2.0f - 1.0f;
				float halfWindow = passthrough;
				float lowX = centerX - halfWindow;
				float highX = centerX + halfWindow;
				if (x < lowX || x > highX) {
					// ±4 bits at int16 output (avoids rounding to zero)
					constexpr float kDeadzoneEpsilon = 4.0f / 32767.0f;
					f_val = (f_val >= 0.0f) ? kDeadzoneEpsilon : -kDeadzoneEpsilon;
				}
			}

			return f_val;
		};

		// Temporary float table for processing
		std::vector<float> tempFloat(kTableSize + 1);

		// 128-bucket phase warp: find quiet zone and balance opposite pairs
		constexpr int kNumBuckets = 128;
		constexpr size_t kBucketSize = (kTableSize + 1) / kNumBuckets;
		float bucketSum[kNumBuckets] = {0};    // Signed sum (for energy balance)
		float bucketAbsSum[kNumBuckets] = {0}; // Absolute sum (for quiet zone)

		// Pass 1a: Generate raw transfer function, find min/max for centering
		float rawMax = -1e30f;
		float rawMin = 1e30f;
		for (size_t i = 0; i <= kTableSize; ++i) {
			float x = (static_cast<float>(i) / kTableScale) - 1.0f;
			float val = evaluateTransfer(x);
			tempFloat[i] = val;
			if (val > rawMax)
				rawMax = val;
			if (val < rawMin)
				rawMin = val;
		}

		// Compute midpoint for centering BEFORE bucket analysis
		float rawMidpoint = (rawMax + rawMin) * 0.5f;

		// Pass 1b: Collect bucket stats on CENTERED values
		if constexpr (kEnergyBalancedPhase) {
			for (size_t i = 0; i <= kTableSize; ++i) {
				float centeredVal = tempFloat[i] - rawMidpoint;
				int b = std::min(static_cast<int>(i / kBucketSize), kNumBuckets - 1);
				bucketSum[b] += centeredVal;
				bucketAbsSum[b] += std::fabs(centeredVal);
			}
		}

		// Compute warp parameters from bucket analysis
		float quietZoneX = 0.0f;
		float warpBoundaries[kNumBuckets + 1]; // Warped x positions for each bucket boundary
		bool applyPhaseWarp = false;

		if constexpr (kEnergyBalancedPhase) {
			// Find quiet zone using weighted absolute amplitude
			// For each proposed center, weight buckets by proximity (closer = higher weight)
			constexpr float kBucketWidth = 2.0f / kNumBuckets;

			int bestBoundary = kNumBuckets / 2;
			float minWeightedAbs = 1e30f;

			for (int center = 1; center < kNumBuckets; ++center) {
				float weightedAbs = 0.0f;
				float totalWeight = 0.0f;

				for (int b = 0; b < kNumBuckets; ++b) {
					// Distance from this bucket to proposed center (in bucket units)
					float dist = std::fabs(static_cast<float>(b) + 0.5f - static_cast<float>(center));

					// Weight: inverse square decay (inner buckets matter more)
					float weight = 1.0f / (1.0f + dist * dist);

					weightedAbs += weight * bucketAbsSum[b];
					totalWeight += weight;
				}

				weightedAbs /= totalWeight; // Normalize by total weight

				if (weightedAbs < minWeightedAbs) {
					minWeightedAbs = weightedAbs;
					bestBoundary = center;
				}
			}

			int minPairStart = std::clamp(bestBoundary - 1, 0, kNumBuckets - 2);

			// Origin at the detected boundary
			quietZoneX = (minPairStart + 1) * kBucketWidth - 1.0f;

			// Build cumulative energy from inner (adjacent to origin) to outer
			// Left side: buckets minPairStart, minPairStart-1, ... going outward
			// Right side: buckets minPairStart+1, minPairStart+2, ... going outward
			int numLeftBuckets = minPairStart + 1;
			int numRightBuckets = kNumBuckets - minPairStart - 1;
			int numPairs = std::min(numLeftBuckets, numRightBuckets);

			float cumLeft[kNumBuckets / 2] = {0};
			float cumRight[kNumBuckets / 2] = {0};

			for (int p = 0; p < numPairs; ++p) {
				int leftIdx = minPairStart - p;
				int rightIdx = minPairStart + 1 + p;
				cumLeft[p] = (p > 0 ? cumLeft[p - 1] : 0.0f) + (leftIdx >= 0 ? bucketSum[leftIdx] : 0.0f);
				cumRight[p] = (p > 0 ? cumRight[p - 1] : 0.0f) + (rightIdx < kNumBuckets ? bucketSum[rightIdx] : 0.0f);
			}

			// Initialize warp boundaries to unwarped positions (relative to quietZoneX as origin)
			for (int b = 0; b <= kNumBuckets; ++b) {
				warpBoundaries[b] = b * kBucketWidth - 1.0f;
			}

			// Compute warped boundaries to balance opposite pairs
			// For each pair level, shift boundary to equalize cumulative energy
			for (int p = 0; p < numPairs; ++p) {
				float imbalance = cumLeft[p] + cumRight[p]; // Should be ~0 for balance
				float totalMag = std::fabs(cumLeft[p]) + std::fabs(cumRight[p]);

				if (totalMag > 0.01f) {
					// Shift proportional to imbalance: positive imbalance means right is heavier
					float shiftFraction = imbalance / totalMag * 0.5f;
					// Limit shift to avoid extreme warping
					shiftFraction = std::clamp(shiftFraction, -0.4f, 0.4f);

					// Apply shift to boundaries at this level (from origin)
					int leftBoundaryIdx = minPairStart - p;      // Left boundary moving outward
					int rightBoundaryIdx = minPairStart + 2 + p; // Right boundary moving outward

					if (leftBoundaryIdx >= 0 && leftBoundaryIdx <= kNumBuckets) {
						warpBoundaries[leftBoundaryIdx] -= shiftFraction * kBucketWidth;
					}
					if (rightBoundaryIdx >= 0 && rightBoundaryIdx <= kNumBuckets) {
						warpBoundaries[rightBoundaryIdx] -= shiftFraction * kBucketWidth;
					}

					applyPhaseWarp = true;
				}
			}

			// Also apply if quiet zone is significantly off-center
			if (std::fabs(quietZoneX) > 0.1f) {
				applyPhaseWarp = true;
			}
		}

		// Lambda to apply piecewise linear warp
		constexpr float kBucketWidthWarp = 2.0f / kNumBuckets;
		auto applyWarp = [&](float x) -> float {
			if (!applyPhaseWarp) {
				return x;
			}

			// First apply quiet-zone centering (shift so quietZoneX → 0)
			float xCentered = x + quietZoneX;

			// Then apply piecewise linear warp based on bucket boundaries
			// Find which bucket segment x falls into and interpolate
			float xNorm = (xCentered + 1.0f) / kBucketWidthWarp; // [0, kNumBuckets]
			int segment = static_cast<int>(std::floor(xNorm));
			segment = std::clamp(segment, 0, kNumBuckets - 1);

			float segFrac = xNorm - segment;

			// Interpolate between warped boundaries
			float warpedX = warpBoundaries[segment] + segFrac * (warpBoundaries[segment + 1] - warpBoundaries[segment]);

			return std::clamp(warpedX, -1.0f, 1.0f);
		};

		// Pass 2: Generate final table with phase warp (analytical - no resampling artifacts)
		float fMax = -1e30f;
		float fMin = 1e30f;

		for (size_t i = 0; i <= kTableSize; ++i) {
			float x = (static_cast<float>(i) / kTableScale) - 1.0f;

			float val;
			if (applyPhaseWarp) {
				float xWarped = applyWarp(x);
				val = evaluateTransfer(xWarped);
			}
			else {
				val = tempFloat[i]; // Use pre-computed value
			}

			tempFloat[i] = val;
			if (val > fMax)
				fMax = val;
			if (val < fMin)
				fMin = val;
		}

		// Midpoint centering in float
		float midpoint = (fMax + fMin) * 0.5f;
		float peakToPeak = fMax - fMin;
		normalizationGain_ = (peakToPeak > 0.001f) ? (2.0f / peakToPeak) : 1.0f;
		normalizationGainInt_ = static_cast<int32_t>(normalizationGain_);

		// Final pass: center, normalize, and convert to int16
		float dx = 2.0f / static_cast<float>(kTableSize);

		for (size_t i = 0; i <= kTableSize; ++i) {
			float val = (tempFloat[i] - midpoint) * normalizationGain_;

			// Store to ADAA tables if enabled
			if constexpr (kGenerateADAA) {
				fTable_[i] = val;

				if (i == 0) {
					FTable_[i] = 0.0f;
				}
				else {
					FTable_[i] = FTable_[i - 1] + (fTable_[i] + fTable_[i - 1]) * dx * 0.5f;
				}
			}

			// Convert to int16
			fTableInt_[i] = static_cast<int16_t>(std::clamp(val * 32767.0f, -32767.0f, 32767.0f));
		}

		// NOW safe to tell audio thread tables are ready
		// Must be AFTER all table data is written
		isLinear_ = false;
	}

	/// Lookup f(x) from cached table with linear or cubic interpolation
	[[gnu::always_inline]] float lookupFunction(float x) const {
		// Map x from [-1, 1] to [0, kTableSize]
		float idx = (x + 1.0f) * kTableScale;
		idx = std::clamp(idx, 0.0f, static_cast<float>(kTableSize));

		size_t i0 = static_cast<size_t>(idx);
		float frac = idx - static_cast<float>(i0);

		if constexpr (kUseCubicFunction) {
			// Cubic Catmull-Rom: smoother, better for small tables
			if (i0 == 0 || i0 >= kTableSize - 1) {
				// Fall back to linear at boundaries
				size_t i1 = std::min(i0 + 1, kTableSize);
				return fTable_[i0] + (fTable_[i1] - fTable_[i0]) * frac;
			}

			size_t im1 = i0 - 1;
			size_t i1 = i0 + 1;
			size_t i2 = std::min(i0 + 2, kTableSize);

			float y0 = fTable_[im1];
			float y1 = fTable_[i0];
			float y2 = fTable_[i1];
			float y3 = fTable_[i2];

			float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
			float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
			float a2 = -0.5f * y0 + 0.5f * y2;
			float a3 = y1;

			return ((a0 * frac + a1) * frac + a2) * frac + a3;
		}
		else {
			// Linear interpolation: faster, may have artifacts with small tables
			size_t i1 = std::min(i0 + 1, kTableSize);
			return fTable_[i0] + (fTable_[i1] - fTable_[i0]) * frac;
		}
	}

	/// Lookup F(x) with linear interpolation (fast)
	[[gnu::always_inline]] float lookupAntiderivativeLinear(float x) const {
		float idx = (x + 1.0f) * kTableScale;
		idx = std::clamp(idx, 0.0f, static_cast<float>(kTableSize));
		size_t i0 = static_cast<size_t>(idx);
		size_t i1 = std::min(i0 + 1, kTableSize);
		float frac = idx - static_cast<float>(i0);
		return FTable_[i0] + (FTable_[i1] - FTable_[i0]) * frac;
	}

	/// Lookup F(x) with cubic interpolation (smooth, for better ADAA)
	[[gnu::always_inline]] float lookupAntiderivativeCubic(float x) const {
		float idx = (x + 1.0f) * kTableScale;
		idx = std::clamp(idx, 0.0f, static_cast<float>(kTableSize));
		size_t i0 = static_cast<size_t>(idx);
		float frac = idx - static_cast<float>(i0);

		// For proper boundary handling, we need indices that don't clamp
		// Use linear interpolation at boundaries where we don't have enough neighbors
		if (i0 == 0 || i0 >= kTableSize - 1) {
			// Fall back to linear at boundaries
			size_t i1 = std::min(i0 + 1, kTableSize);
			return FTable_[i0] + (FTable_[i1] - FTable_[i0]) * frac;
		}

		size_t im1 = i0 - 1;
		size_t i1 = i0 + 1;
		size_t i2 = std::min(i0 + 2, kTableSize);

		float y0 = FTable_[im1];
		float y1 = FTable_[i0];
		float y2 = FTable_[i1];
		float y3 = FTable_[i2];

		float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
		float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
		float a2 = -0.5f * y0 + 0.5f * y2;
		float a3 = y1;

		return ((a0 * frac + a1) * frac + a2) * frac + a3;
	}

	/// Integer table lookup - matches builtin interpolateTableSigned style
	/// Input: uint32_t in [0, 2^32) where 0 = -1.0, 2^32-1 = +1.0
	/// Output: int32_t scaled by 65536 (Q16.15 format, like builtin)
	/// Uses linear interpolation for speed
	[[gnu::always_inline]] int32_t lookupFunctionInt(uint32_t input) const {
		// Compute log2(kTableSize) at compile time
		constexpr int32_t kTableBits = (kTableSize == 128)    ? 7
		                               : (kTableSize == 256)  ? 8
		                               : (kTableSize == 512)  ? 9
		                               : (kTableSize == 1024) ? 10
		                               : (kTableSize == 2048) ? 11
		                                                      : 8; // default

		// Extract table index (upper kTableBits bits of input)
		int32_t whichValue = input >> (32 - kTableBits);

		// Extract fractional part (next 16 bits after index)
		constexpr int32_t rshiftAmount = 32 - 16 - kTableBits;
		uint32_t rshifted = input >> rshiftAmount;
		int32_t strength2 = rshifted & 65535;
		int32_t strength1 = 65536 - strength2;

		// Linear interpolation with int16 table entries
		// Result is scaled by 65536 (Q16.15)
		return static_cast<int32_t>(fTableInt_[whichValue]) * strength1
		       + static_cast<int32_t>(fTableInt_[whichValue + 1]) * strength2;
	}

	/// Lookup F(x) (antiderivative) from cached table
	/// Uses linear or cubic interpolation based on kUseCubicAntiderivative flag
	[[gnu::always_inline]] float lookupAntiderivative(float x) const {
		// Map x from [-1, 1] to [0, kTableSize]
		float idx = (x + 1.0f) * kTableScale;
		idx = std::clamp(idx, 0.0f, static_cast<float>(kTableSize));

		size_t i0 = static_cast<size_t>(idx);
		float frac = idx - static_cast<float>(i0);

		if constexpr (kUseCubicAntiderivative) {
			// Use linear at boundaries where we don't have enough neighbors
			if (i0 == 0 || i0 >= kTableSize - 1) {
				size_t i1 = std::min(i0 + 1, kTableSize);
				return FTable_[i0] + (FTable_[i1] - FTable_[i0]) * frac;
			}

			// Cubic Catmull-Rom: smoother derivatives, better for ADAA
			size_t im1 = i0 - 1;
			size_t i1 = i0 + 1;
			size_t i2 = std::min(i0 + 2, kTableSize);

			float y0 = FTable_[im1];
			float y1 = FTable_[i0];
			float y2 = FTable_[i1];
			float y3 = FTable_[i2];

			float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
			float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
			float a2 = -0.5f * y0 + 0.5f * y2;
			float a3 = y1;

			return ((a0 * frac + a1) * frac + a2) * frac + a3;
		}
		else {
			// Linear interpolation: faster, may have more artifacts
			size_t i1 = std::min(i0 + 1, kTableSize);
			return FTable_[i0] + (FTable_[i1] - FTable_[i0]) * frac;
		}
	}

	// Cached tables (regenerated when parameters change)
	// Uses vectors for dynamic allocation - deallocated when X=0 to save ~20KB per instance
	std::vector<float> fTable_; // f(x) values (float)
	std::vector<float> FTable_; // F(x) antiderivative values (float)

	// Integer tables for fast integer-only processing (like builtin)
	// Scale: float [-1,1] → int16_t [-32767,32767]
	std::vector<int16_t> fTableInt_; // f(x) values (int16)

	// Parameters (consolidated struct)
	TableShaperParams params_;

	// State
	bool tablesDirty_{true};
	bool isLinear_{true};
	float prevX_{0.0f};
	float normalizationGain_{1.0f};       // Peak normalization (float)
	int32_t normalizationGainInt_{32767}; // Peak normalization (fixed-point, Q15 format)

	// Blend aggression: derived from X (drive), affects mix curve sharpness
	// Q8 format: 256 = 1.0x, range [128, 512] for [0.5x, 2.0x]
	// Low X = gentle transitions, high X = snappy onset
	int32_t blendAggressionMult_Q8_{256};

	// Expected peak level for int32 path (set at table generation time)
	// Used to normalize input/output so the table "expects" signals at this level
	// FM at max LOCAL_VOLUME + max OSC_VOLUME = 2^26 (~67M)
	// Calculation: sine(2^31) * sourceAmplitude(2^27) / 2^32 = 2^26
	// FM signal calibration (empirically determined):
	// - Theoretical max: 2^26 = 67M (sine * sourceAmplitude / 2^32, sourceAmplitude capped at 2^27)
	// - inputScale=128 (2^7) puts saturation onset near center drive for FM at max velocity
	// - At lower velocities: need positive drive to reach saturation (natural velocity response)
	// - Output = lookup >> inputScaleShift_ (unity gain: boost in, attenuate out)
	int32_t expectedPeak_{1 << 26};      // 67,108,864 - theoretical FM max (reference only)
	float inputScale_{128.0f};           // Calibrated: FM v=127 saturates near center drive
	int32_t inputScaleShift_{7};         // floor(log2(inputScale_)) for bit-shift scaling (128 = 2^7)
	float invInputScale_{1.0f / 128.0f}; // Precomputed reciprocal for non-power-of-2 fallback

public:
	/// Set the expected peak level for int32 processing
	/// Call this when synth mode changes (FM vs subtractive)
	void setExpectedPeak(int32_t peak) {
		expectedPeak_ = peak;
		// Input scale: maps expectedPeak to INT32_MAX (fills table proportionally)
		constexpr float kInt32Max = 2147483647.0f;
		inputScale_ = kInt32Max / static_cast<float>(peak);
		// Compute shift for bit-shift scaling - floor() for less boost (safer)
		int32_t shift = static_cast<int32_t>(std::floor(std::log2(inputScale_)));
		inputScaleShift_ = (shift < 0) ? 0 : ((shift > 31) ? 31 : shift); // Safety bounds
		// Precompute reciprocal for fallback (multiply faster than divide)
		invInputScale_ = 1.0f / inputScale_;
	}

	[[nodiscard]] int32_t getExpectedPeak() const { return expectedPeak_; }
	[[nodiscard]] float getInputScale() const { return inputScale_; }
};

/**
 * Helper to derive shaper parameters from XY position with combinatoric sweep
 *
 * X axis maps to drive (0 = linear bypass, 127 = full saturation)
 * Y axis creates a combinatoric sweep through basis weights, threshold, asymmetry
 * using half-rectified triangle waves with different periods
 *
 * Key behaviors:
 * - Half-rectified oscillators: each basis is OFF for ~50% of its cycle, creating
 *   gaps where only a subset of the 6 bases are active (sparse combinations)
 * - Accelerating frequency: oscillations are slow at Y=0 (easy to find sweet spots)
 *   and fast at Y=1023 (chaotic exploration with more gaps)
 * - φ-power frequency ratios: using powers of the golden ratio ensures frequencies
 *   never align, producing quasi-periodic patterns with no exact repetition
 *
 * Result: distinct character zones at low Y, fragmented/chaotic at high Y
 */
struct TableShaperXYMapper {
	// =================================================================
	// Duty cycle controls the active/gap ratio of basis oscillators
	// 0.25 = 25% active, 75% gap (very sparse, distinct characters)
	// 0.5  = 50% active, 50% gap (balanced, default)
	// 0.75 = 75% active, 25% gap (more blending, smoother)
	// 1.0  = 100% active, no gaps (original continuous triangle)
	// =================================================================
	static constexpr float kPhaseWidth = 0.5f;

	/// Derive parameters from X (0-127) and Y (0-1023) with combinatoric sweep
	/// @param x X position (0-127), maps to drive (0 = linear bypass)
	/// @param y Y position (0-1023), creates high-res combinatoric parameter sweep
	/// @return TableShaperParams with all derived values
	static TableShaperParams deriveParameters(uint8_t x, uint16_t y) {
		TableShaperParams p;
		p.drive = static_cast<float>(x) / 127.0f;

		float yNorm = static_cast<float>(y) / 1023.0f;

		// Accelerating interference: slow at Y=0, fast at Y=1023
		constexpr float kAccelFactor = 3.0f;
		float freqMult = 1.0f + yNorm * yNorm * kAccelFactor;

		// 6 Basis weights with φ-power frequency ratios for quasi-periodic coverage
		p.tanhWeight = 0.2f + triangleSimpleUnipolar(yNorm * phi::kPhi225 * freqMult, kPhaseWidth) * 0.8f;
		p.polyWeight = triangleSimpleUnipolar(yNorm * phi::kPhi200 * freqMult + 0.167f, kPhaseWidth);
		p.hardKneeWeight = triangleSimpleUnipolar(yNorm * phi::kPhi175 * freqMult + 0.333f, kPhaseWidth);
		p.chebyWeight = triangleSimpleUnipolar(yNorm * phi::kPhi250 * freqMult + 0.5f, kPhaseWidth);
		p.sineFoldWeight = triangleSimpleUnipolar(yNorm * phi::kPhi150 * freqMult + 0.667f, kPhaseWidth);
		p.rectifierWeight = triangleSimpleUnipolar(yNorm * phi::kPhi125 * freqMult + 0.833f, kPhaseWidth);

		p.threshold = triangleSimpleUnipolar(yNorm * phi::kPhi275 * freqMult + 0.25f, kPhaseWidth);

		float asymFreqMult = 1.0f + yNorm * yNorm * (kAccelFactor * 0.5f);
		p.asymmetry = 0.3f + triangleSimpleUnipolar(yNorm * phi::kPhi100 * asymFreqMult, kPhaseWidth) * 0.4f;

		return p;
	}

	/// Derive parameters with phase offsets for secret knob integration
	/// @param x X position (0-127)
	/// @param y Y position (0-1023)
	/// @param phaseOffset Phase offset for parameter interference (from secret knob)
	/// @param periodScale Period scaling for parameter sweep rate
	/// @return TableShaperParams with all derived values
	///
	/// DESIGN NOTE: Phase Offset Scope
	/// ===============================
	/// Currently, phaseOffset rotates TWO levels of parameters:
	///   1. Algorithm superposition weights (which basis functions are active)
	///      - polyWeight, hardKneeWeight, chebyWeight, sineFoldWeight, rectifierWeight
	///      - phMult values: 0.167, 0.333, 0.5, 0.667, 0.833
	///   2. Internal algorithm parameters (how each basis behaves)
	///      - threshold (phMult: 0.25), asymmetry (phMult: 0.618)
	///
	/// Note: tanhWeight has phMult=0 so it serves as an anchor (always present)
	///
	/// ALTERNATIVE: Only rotate internal parameters
	/// If zone names should remain semantically stable (Y=0 always "Warm", Y=512 always "Fold"),
	/// we could set phMult=0 for all 6 basis weights. This would make:
	///   - Y axis: determines WHICH algorithms are blended (zone identity)
	///   - Secret knob: tunes HOW those algorithms behave (character within zone)
	///
	/// Current behavior: Secret knob morphs both identity AND character, creating
	/// continuous exploration where zone names are approximate guides rather than
	/// fixed definitions. This is more "sound design-y" but less predictable.
	///
	static TableShaperParams deriveParametersWithPhase(uint8_t x, uint16_t y, float phaseOffset, float periodScale) {
		TableShaperParams p;
		p.drive = static_cast<float>(x) / 127.0f;

		float yNorm = static_cast<float>(y) / 1023.0f;

		constexpr float kAccelFactor = 3.0f;
		float freqMult = 1.0f + yNorm * yNorm * kAccelFactor;

		// Use double precision to preserve phase accuracy at large phaseOffset values (< 10^15 ok)
		// Pre-wrap phase offsets at different φ frequencies (like disperser/sine_shaper/multiband)
		double ph = static_cast<double>(phaseOffset);
		float ph225 = phi::wrapPhase(ph * phi::kPhi225);
		float ph200 = phi::wrapPhase(ph * phi::kPhi200);
		float ph175 = phi::wrapPhase(ph * phi::kPhi175);
		float ph250 = phi::wrapPhase(ph * phi::kPhi250);
		float ph150 = phi::wrapPhase(ph * phi::kPhi150);
		float ph125 = phi::wrapPhase(ph * phi::kPhi125);
		float ph275 = phi::wrapPhase(ph * phi::kPhi275);
		float ph100 = phi::wrapPhase(ph * phi::kPhi100);

		// Deadzone phi triangles - use slower frequencies for smooth evolution
		// Initial offset 0.75 places width in dead region [0.5,1) at phaseOffset=0
		float phDzWidth = phi::wrapPhase(ph * phi::kPhiN050); // φ^-0.5 (slower)
		float phDzPhase = phi::wrapPhase(ph * phi::kPhi033);  // φ^0.33

		// Apply phase offsets and period scaling for interference patterns (φ-power frequencies)
		// Each parameter rotates at its own irrational rate - no alignments possible
		auto base = [yNorm, freqMult, periodScale](float freq) {
			return static_cast<float>(static_cast<double>(yNorm) * freq * freqMult * periodScale);
		};

		p.tanhWeight = 0.2f + triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi225) + ph225), kPhaseWidth) * 0.8f;
		p.polyWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi200) + ph200), kPhaseWidth);
		p.hardKneeWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi175) + ph175), kPhaseWidth);
		p.chebyWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi250) + ph250), kPhaseWidth);
		p.sineFoldWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi150) + ph150), kPhaseWidth);
		p.rectifierWeight = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi125) + ph125), kPhaseWidth);

		p.threshold = triangleSimpleUnipolar(phi::wrapPhase(base(phi::kPhi275) + ph275), kPhaseWidth);

		float asymFreqMult = 1.0f + yNorm * yNorm * (kAccelFactor * 0.5f);
		float asymBase = static_cast<float>(static_cast<double>(yNorm) * phi::kPhi100 * asymFreqMult * periodScale);
		p.asymmetry = 0.3f + triangleSimpleUnipolar(phi::wrapPhase(asymBase + ph100), kPhaseWidth) * 0.4f;

		// Deadzone modifier: inactive at phaseOffset=0, oscillates as secret knob increases
		// Width uses duty=0.5 with initial offset 0.75 to start in dead region
		// At phaseOffset=0: phDzWidth=0, offset 0.75 → phase=0.75 → in dead region → output=0
		constexpr float kDeadzoneDuty = 0.5f;
		constexpr float kDeadzoneWidthOffset = 0.75f; // Places phase in dead region [0.5,1) at start
		float dzWidthBase = static_cast<float>(static_cast<double>(yNorm) * phi::kPhiN050 * freqMult * periodScale);
		p.deadzoneWidth =
		    triangleSimpleUnipolar(phi::wrapPhase(dzWidthBase + phDzWidth + kDeadzoneWidthOffset), kDeadzoneDuty);

		// Phase center oscillates freely (doesn't need to start inactive)
		// 30% duty cycle + squared result → spends most time near 0, occasional excursions to 1
		constexpr float kDeadzonePhaseDuty = 0.30f;
		float dzPhaseBase = static_cast<float>(static_cast<double>(yNorm) * phi::kPhi033 * freqMult * periodScale);
		float dzPhaseRaw = triangleSimpleUnipolar(phi::wrapPhase(dzPhaseBase + phDzPhase), kDeadzonePhaseDuty);
		p.deadzonePhase = dzPhaseRaw * dzPhaseRaw; // Square to spend less time near 1

		return p;
	}
};

} // namespace deluge::dsp
