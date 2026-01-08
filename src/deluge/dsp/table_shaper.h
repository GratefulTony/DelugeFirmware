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
	}

	bool operator!=(const TableShaperParams& o) const {
		return drive != o.drive || tanhWeight != o.tanhWeight || polyWeight != o.polyWeight
		       || hardKneeWeight != o.hardKneeWeight || chebyWeight != o.chebyWeight
		       || sineFoldWeight != o.sineFoldWeight || rectifierWeight != o.rectifierWeight || threshold != o.threshold
		       || asymmetry != o.asymmetry;
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
		// Input drive knob = input gain (original behavior)
		int32_t afterDrive = multiply_32x32_rshift32(input, driveGain_Q30) << 2;

		// Fast path: bypass when linear (tables may be deallocated)
		if (isLinear_) {
			return afterDrive;
		}

		// Scale to fill table range using bit shift (inputScale_ is power of 2, e.g. 128 = 2^7)
		// Input is bounded by drive stage, so overflow not expected
		int32_t scaledInput = afterDrive << inputScaleShift_;

		// Amplitude-dependent blend using Q16 fixed-point
		// absNorm_Q16: abs(scaledInput) >> 15 gives top 17 bits as Q16 [0, 65536]
		int32_t absInput = scaledInput < 0 ? -scaledInput : scaledInput;
		int32_t absNorm_Q16 = absInput >> 15; // Q31 to Q16.16

		// threshold_Q16 = (1 - mixNorm) in Q16: 65536 - mixNorm_Q16
		// Range: [-65536, 65536] for mixNorm in [0, 2]
		constexpr int32_t kOne_Q16 = 65536;
		int32_t threshold_Q16 = kOne_Q16 - mixNorm_Q16;

		// diff_Q16 = absNorm - threshold, early exit if <= 0 (fully dry)
		int32_t diff_Q16 = absNorm_Q16 - threshold_Q16;
		if (diff_Q16 <= 0) {
			return afterDrive;
		}

		// Fixed blend slope of 4 (shift instead of multiply)
		// Ramps from 0 to 1.0 over 25% of amplitude range above threshold
		int32_t blend_Q16 = diff_Q16 << 2;

		// Fully wet when blend >= 1.0
		if (blend_Q16 >= kOne_Q16) {
			// Fully wet path
			uint32_t tableInput = static_cast<uint32_t>(scaledInput) + 2147483648u;
			int32_t lookup = lookupFunctionInt(tableInput);
			return lookup >> inputScaleShift_;
		}

		// Table lookup for wet signal
		uint32_t tableInput = static_cast<uint32_t>(scaledInput) + 2147483648u;
		int32_t lookup = lookupFunctionInt(tableInput);
		int32_t wet = lookup >> inputScaleShift_;

		// Blend dry/wet using Q16: out = dry * (1-blend) + wet * blend
		// Use multiply_32x32_rshift32 for efficiency (treats args as Q31, result Q31)
		// Convert blend_Q16 to Q31: blend_Q31 = blend_Q16 << 15
		int32_t blend_Q31 = blend_Q16 << 15;
		int32_t oneMinusBlend_Q31 = (kOne_Q16 << 15) - blend_Q31;

		// dry * (1-blend) + wet * blend, all in Q31 multiply then sum
		int32_t dryPart = multiply_32x32_rshift32(afterDrive, oneMinusBlend_Q31) << 1;
		int32_t wetPart = multiply_32x32_rshift32(wet, blend_Q31) << 1;
		return dryPart + wetPart;
	}

	/// Process a single sample using integer-only path (legacy float interface)
	/// Uses stored inputScale_/outputScale_ set via setExpectedPeak()
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

		// Generate tables - track positive and negative peaks separately for asymmetric normalization
		float peakPos = 0.0f; // Maximum positive value
		float peakNeg = 0.0f; // Maximum negative value (stored as positive for easy comparison)
		float dx = 2.0f / static_cast<float>(kTableSize);

		for (size_t i = 0; i <= kTableSize; ++i) {
			float x = (static_cast<float>(i) / kTableScale) - 1.0f; // -1 to +1
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

				// ==========================================================
				// Intensity: overdrive the basis functions for richer harmonics
				// At drive=0: intensity=1 (gentle)
				// At drive=1: intensity=2 (2x overdrive, moderate saturation)
				// This pushes the input beyond 1.0 to create actual clipping/folding
				// ==========================================================
				float intensity = 1.0f + params_.drive * 1.0f;
				float overdriven = norm * intensity;

				// Asymmetric k for positive/negative
				float kEff = k * ((x >= 0.0f) ? asymRatio : (2.0f - asymRatio));
				float invTanhNorm = (x >= 0.0f) ? invTanhNormPos : invTanhNormNeg;

				// =========================================================
				// BASIS 1: Tanh (warm, smooth) - using fast approximation
				// Classic tube-like saturation, pure odd harmonics
				// Overdrive creates harder saturation
				// =========================================================
				float tanh_out = fastTanh(overdriven * kEff) * invTanhNorm;

				// =========================================================
				// BASIS 2: Polynomial soft clip (bright, edgy)
				// Higher order polynomial with overdrive for more harmonics
				// x - x³/3 + x⁵/5 creates 3rd and 5th harmonics
				// =========================================================
				float od2 = overdriven * overdriven;
				float od3 = od2 * overdriven;
				float od5 = od3 * od2;
				float poly_out = overdriven - od3 * 0.333333f + od5 * 0.2f;
				poly_out = fastTanh(poly_out); // Soft limit the result

				// =========================================================
				// BASIS 3: Hard clip (crisp, aggressive)
				// True hard clipping creates rich odd harmonics
				// =========================================================
				float hardClip_out = std::fmin(std::fmax(overdriven, -1.0f), 1.0f);

				// =========================================================
				// BASIS 4: Chebyshev T5 wavefolder (fold, synthy)
				// T5(x) = 16x⁵ - 20x³ + 5x, with overdrive creates multiple folds
				// Input scaled to create 2-3 folds at full drive
				// =========================================================
				float cheby_in = overdriven * 1.2f; // Moderate fold scaling
				float cheby_in2 = cheby_in * cheby_in;
				float cheby_in3 = cheby_in2 * cheby_in;
				float cheby_in5 = cheby_in3 * cheby_in2;
				float cheby_raw = 16.0f * cheby_in5 - 20.0f * cheby_in3 + 5.0f * cheby_in;
				// Closed-form triangle fold into [-1, 1] (period = 4)
				float cheby_phase = std::fmod(cheby_raw + 1.0f, 4.0f);
				if (cheby_phase < 0.0f)
					cheby_phase += 4.0f;
				float cheby_out = (cheby_phase <= 2.0f) ? (cheby_phase - 1.0f) : (3.0f - cheby_phase);
				cheby_out = std::fabs(cheby_out);

				// =========================================================
				// BASIS 5: Sine folder (Gold) - harmonic-rich folding
				// tanh(x/a)*sin(b*x)/y + tanh(x) with multiple folds
				// Higher frequency sine for richer harmonics
				// =========================================================
				constexpr float kSineFoldA = 0.4f;                     // Tanh envelope softness
				float sineFoldB = 3.14159265f * (1.0f + drive * 1.0f); // 1-2 folds
				float sineFold_raw =
				    fastTanh(overdriven / kSineFoldA) * std::sin(sineFoldB * overdriven) + fastTanh(overdriven) * 0.3f;
				float sineFold_out = std::fabs(sineFold_raw);
				sineFold_out = std::fmin(sineFold_out, 1.0f);

				// =========================================================
				// BASIS 6: Rectifier (diode) - asymmetric, even harmonics
				// Full-wave rectifier with variable bias for rich even harmonics
				// =========================================================
				float bias = 0.2f * drive; // Adds DC offset for even harmonics
				float rect_raw = std::fabs(overdriven + bias) - bias;
				float rect_out = fastTanh(rect_raw * 2.0f); // Soft limit

				// Blend using weights (normalized by sum)
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

				// ==========================================================
				// Drive-dependent blend toward linear (like tanh envelope)
				// At drive=0: output = norm (completely linear/transparent)
				// At drive=1: output = basis_out (full nonlinear character)
				// This unifies X-axis control across all basis functions
				// ==========================================================
				basis_out = norm + (basis_out - norm) * drive;

				// Map back to output range
				f_val = sign * (T + (1.0f - T) * std::fabs(basis_out));
			}

			// Track positive and negative peaks separately for asymmetric normalization
			if (f_val > peakPos) {
				peakPos = f_val;
			}
			else if (f_val < 0.0f && -f_val > peakNeg) {
				peakNeg = -f_val;
			}

			// Store to appropriate tables based on configuration
			if constexpr (kGenerateADAA) {
				fTable_[i] = f_val;

				// Compute antiderivative using trapezoidal integration
				// F(x) = ∫f(x)dx, approximated incrementally
				if (i == 0) {
					FTable_[i] = 0.0f;
				}
				else {
					// Trapezoidal rule: F(x) = F(x-dx) + (f(x) + f(x-dx)) * dx / 2
					FTable_[i] = FTable_[i - 1] + (fTable_[i] + fTable_[i - 1]) * dx * 0.5f;
				}
			}

			// Always populate int table (used by processNoAAInt32)
			// Bake in normalization incrementally to avoid second pass
			// Note: we'll rescale after finding true peak
			fTableInt_[i] = static_cast<int16_t>(std::clamp(f_val * 32767.0f, -32767.0f, 32767.0f));
		}

		// Compute asymmetric normalization gains
		// This maps positive values to [0, 1] and negative values to [-1, 0] independently
		// Ensures full dynamic range utilization for asymmetric wavefunctions (e.g., rectifier)
		float normGainPos = (peakPos > 0.01f) ? (1.0f / peakPos) : 1.0f;
		float normGainNeg = (peakNeg > 0.01f) ? (1.0f / peakNeg) : 1.0f;

		// For backward compatibility, normalizationGain_ uses the smaller gain (larger peak)
		normalizationGain_ = std::fmin(normGainPos, normGainNeg);

		// Integer normalization: Q15 format (32767 = 1.0)
		normalizationGainInt_ = static_cast<int32_t>(normalizationGain_ * 32767.0f);

		// Apply asymmetric normalization to integer table
		// Positive values scaled by normGainPos, negative by normGainNeg
		bool needsNormalization = (peakPos > 0.01f || peakNeg > 0.01f);
		if (needsNormalization) {
			for (size_t i = 0; i <= kTableSize; ++i) {
				float val = static_cast<float>(fTableInt_[i]);
				float normalized = (val >= 0.0f) ? (val * normGainPos) : (val * normGainNeg);
				fTableInt_[i] = static_cast<int16_t>(std::clamp(normalized, -32767.0f, 32767.0f));
			}
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

	// Expected peak level for int32 path (set at table generation time)
	// Used to normalize input/output so the table "expects" signals at this level
	// FM at max LOCAL_VOLUME + max OSC_VOLUME = 2^26 (~67M)
	// Calculation: sine(2^31) * sourceAmplitude(2^27) / 2^32 = 2^26
	// FM signal calibration (empirically determined):
	// - Theoretical max: 2^26 = 67M (sine * sourceAmplitude / 2^32, sourceAmplitude capped at 2^27)
	// - inputScale=128 (2^7) puts saturation onset near center drive for FM at max velocity
	// - At lower velocities: need positive drive to reach saturation (natural velocity response)
	// - Output = lookup / inputScale (unity gain: boost in, attenuate out)
	int32_t expectedPeak_{1 << 26}; // 67,108,864 - theoretical FM max (reference only)
	float inputScale_{128.0f};      // Calibrated: FM v=127 saturates near drive=0 (2^7 for bit-shift efficiency)
	int inputScaleShift_{7};        // log2(inputScale_) for bit-shift output scaling (128 = 2^7)
	float outputScale_{static_cast<float>(1 << 26) / (32767.0f * 65536.0f)}; // Float path only (deprecated)

public:
	/// Set the expected peak level for int32 processing
	/// Call this when synth mode changes (FM vs subtractive)
	void setExpectedPeak(int32_t peak) {
		expectedPeak_ = peak;
		// Input scale: maps expectedPeak to INT32_MAX (fills table proportionally)
		constexpr float kInt32Max = 2147483647.0f;
		inputScale_ = kInt32Max / static_cast<float>(peak);
		// Compute shift for bit-shift output scaling (assumes inputScale_ is ~power of 2)
		inputScaleShift_ = static_cast<int>(std::log2(inputScale_) + 0.5f);
		// Output scale for unity gain: peak / (32767 * 65536)
		// Lookup returns Q16.15 (table_int16 * 65536), this converts back to input level
		constexpr float kLookupFullScale = 32767.0f * 65536.0f;
		outputScale_ = static_cast<float>(peak) / kLookupFullScale;
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

		return p;
	}
};

} // namespace deluge::dsp
