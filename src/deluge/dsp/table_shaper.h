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
#include <array>
#include <atomic>
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
 * Table-based Parametric Shaper
 *
 * Features:
 * - 6 basis functions for rich harmonic exploration
 * - Drive parameter where 0 = linear bypass (transparent)
 * - Separate weights for each basis function
 * - Double-buffered lookup tables with IIR crossfade for click-free updates
 */
class TableShaperCore {
public:
	// =============================================================================
	// TABLE REGENERATION CONFIGURATION
	// =============================================================================
	// Table size vs click tradeoff:
	// - 2048: Best quality for wavefolders, but regeneration may cause minor clicks
	// - 1024: Click-free regeneration, minimal quality difference for most curves
	// - 512/256/128: Faster regeneration, noticeable smoothing on sharp features
	// Linear interpolation adds 16-bit fractional precision between entries.
	static constexpr size_t kTableSize = 2048;
	// =============================================================================

	static constexpr float kTableScale = static_cast<float>(kTableSize) / 2.0f;

	TableShaperCore() = default; // Tables start empty, allocated on first non-linear use

	/// Set all parameters - deferred regeneration (call regenerateIfDirty from non-audio context)
	void setParameters(const TableShaperParams& p) {
		TableShaperParams clamped = p;
		clamped.clamp();
		if (clamped != params_) {
			params_ = clamped;
			tablesDirty_ = true;
		}
	}

	/// Pre-allocate buffers (call from UI thread before scheduling regeneration)
	/// This ensures no allocation happens during the deferred regeneration task
	void ensureBuffersAllocated() {
		if (fTables_[0].size() != kTableSize + 1) {
			fTables_[0].resize(kTableSize + 1);
		}
		if (fTables_[1].size() != kTableSize + 1) {
			fTables_[1].resize(kTableSize + 1);
		}
		if (fTableTempFloat_.size() != kTableSize + 1) {
			fTableTempFloat_.resize(kTableSize + 1);
		}
	}

	/// Call from non-audio context (UI routine, etc) to regenerate tables
	void regenerateIfDirty() {
		if (tablesDirty_) {
			regenerateTables();
		}
	}

	/// Get current parameters
	[[nodiscard]] const TableShaperParams& getParameters() const { return params_; }

	/// Check if effect is effectively bypassed (transparent) based on current params
	/// Only checks drive (X axis) - threshold shouldn't cause bypass since user explicitly set X > 0
	/// Note: This checks params_, NOT the isLinear_ flag (which is for audio thread sync)
	[[nodiscard]] bool isLinear() const { return params_.drive < 0.001f; }

	/// Process a single sample using integer-only path with Q16 mix parameter
	/// @param input Input sample in raw signal format (e.g., ~23M for FM, ~1.4M for subtractive)
	/// @param driveGain_Q26 Pre-computed drive gain in Q26 format (allows up to 32x)
	/// @param mixNorm_Q16 Normalized mix in Q16.16 (65536 = 1.0, 131072 = 2.0 full wet)
	/// @return Output sample at same level as input (unity gain when undriven)
	[[gnu::always_inline]] int32_t processInt32Q16(int32_t input, int32_t driveGain_Q26, int32_t mixNorm_Q16 = 131072) {
		// Linear drive with Q26 gain (<<6 recovers from Q26 multiply)
		// Range: 0x at min drive to 4x at max drive (Q26 allows up to 32x)
		int32_t afterDrive = multiply_32x32_rshift32(input, driveGain_Q26) << 6;

		// Fast path: bypass when linear (tables may be deallocated)
		// acquire ordering ensures we see all table writes if isLinear_ is false
		if (isLinear_.load(std::memory_order_acquire)) {
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

		// Smooth blendAggression toward target (~6ms time constant at 44.1kHz)
		int32_t aggDiff = blendAggressionTarget_Q8_ - blendAggressionMult_Q8_;
		if (aggDiff != 0) {
			// IIR smoothing: change by at least 1 per sample to ensure convergence
			int32_t aggDelta = aggDiff >> 8;
			if (aggDelta == 0) {
				aggDelta = (aggDiff > 0) ? 1 : -1;
			}
			blendAggressionMult_Q8_ += aggDelta;
		}

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

		// Target-chasing crossfade: smoothly interpolate toward target table
		int8_t target = targetTableIndex_.load(std::memory_order_acquire);

		// IIR chase: blend moves toward target (0 = table 0, 32768 = table 1)
		int32_t targetBlend_Q15 = target ? 32768 : 0;
		int32_t blendDiff = targetBlend_Q15 - currentBlend_Q15_;
		if (blendDiff != 0) {
			int32_t delta = (blendDiff * kBlendAlpha_Q15) >> 15;
			if (delta == 0) {
				delta = (blendDiff > 0) ? 1 : -1;
			}
			currentBlend_Q15_ += delta;
		}

		// Table lookup with crossfade (short-circuit when settled)
		int32_t lookup;
		if (currentBlend_Q15_ == 0) {
			lookup = lookupFunctionIntDirect(tableInput, 0);
		}
		else if (currentBlend_Q15_ == 32768) {
			lookup = lookupFunctionIntDirect(tableInput, 1);
		}
		else {
			int32_t val0 = lookupFunctionIntDirect(tableInput, 0);
			int32_t val1 = lookupFunctionIntDirect(tableInput, 1);
			lookup = val0 + (((val1 - val0) * currentBlend_Q15_) >> 15);
		}

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

	/// Deallocate tables to free memory (~4KB)
	/// Called automatically when X=0 (linear bypass)
	void deallocateTables() {
		fTables_[0].clear();
		fTables_[0].shrink_to_fit();
		fTables_[1].clear();
		fTables_[1].shrink_to_fit();
	}

	/// Check if tables are currently allocated
	[[nodiscard]] bool hasAllocatedTables() const { return !fTables_[0].empty(); }

private:
	/// Regenerate both f(x) and F(x) tables based on current parameters
	/// Uses fast math approximations for speed during parameter automation.
	/// When linear (X=0), deallocates tables to save memory (~20KB per instance).
	/// IMPORTANT: isLinear_ is set AFTER tables are fully populated to prevent
	/// audio thread from reading partially-initialized data.
	void regenerateTables() {
		tablesDirty_ = false;
		bool willBeLinear = isLinear(); // Check params, but don't set isLinear_ yet

		// Compute blend aggression from drive (X axis) - defer writing until after table swap
		// Quadratic curve: very gentle at low X, snappy at high X
		// Range: [0.1, 2.0] → 20x dynamic range
		// At X=0: slope so gentle that full mix range is needed for full blend
		// At X=127: snappy onset, wet kicks in quickly
		float driveSquared = params_.drive * params_.drive;
		float aggression = 0.1f + driveSquared * 1.9f; // [0.1, 2.0]
		int32_t newBlendAggression = static_cast<int32_t>(aggression * 256.0f);

		if (willBeLinear) {
			// Set linear flag FIRST - audio thread will bypass table access
			// Tables are NOT deallocated here to avoid race with audio thread
			// (deallocation happens lazily when regenerating tables for non-linear)
			// No need to touch targetTableIndex_ - audio will bypass tables when linear
			blendAggressionTarget_Q8_ = newBlendAggression;
			blendAggressionMult_Q8_ = newBlendAggression;     // Snap (no smoothing needed when linear)
			isLinear_.store(true, std::memory_order_release); // Release LAST
			return;
		}

		// Keep isLinear_ = true until tables are FULLY populated
		// This prevents audio thread from reading partial data

		// Determine which buffer to write to (the one NOT currently being used)
		int8_t currentTarget = targetTableIndex_.load(std::memory_order_relaxed);
		int8_t writeToIdx = 1 - currentTarget;

		// CRITICAL: Snap blend to current target BEFORE writing to inactive buffer
		// With slow IIR crossfade, blend may not have converged yet, meaning audio
		// is still reading from both tables. If we write to the "inactive" buffer
		// while audio is blending from it, we get clicks from partially-written data.
		// Snapping ensures audio reads 100% from current target, leaving writeToIdx safe.
		currentBlend_Q15_ = currentTarget ? 32768 : 0;

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

		// fTableTempFloat_ must be pre-allocated via ensureBuffersAllocated()

		// Generate transfer function table, find min/max for centering
		float fMax = -1e30f;
		float fMin = 1e30f;
		for (size_t i = 0; i <= kTableSize; ++i) {
			float x = (static_cast<float>(i) / kTableScale) - 1.0f;
			float val = evaluateTransfer(x);
			fTableTempFloat_[i] = val;
			if (val > fMax)
				fMax = val;
			if (val < fMin)
				fMin = val;
		}

		// Midpoint centering in float
		float midpoint = (fMax + fMin) * 0.5f;
		float peakToPeak = fMax - fMin;
		float normalizationGain = (peakToPeak > 0.001f) ? (2.0f / peakToPeak) : 1.0f;

		// Final pass: center, normalize, and convert to int16
		for (size_t i = 0; i <= kTableSize; ++i) {
			float val = (fTableTempFloat_[i] - midpoint) * normalizationGain;
			fTables_[writeToIdx][i] = static_cast<int16_t>(std::clamp(val * 32767.0f, -32767.0f, 32767.0f));
		}

		// Install new table: flip target, audio will chase it with IIR
		// We snapped blend to currentTarget above, so now we're safe to flip
		// Audio will smoothly interpolate from old table (100%) to new table
		targetTableIndex_.store(writeToIdx, std::memory_order_release);

		// Set target for smoothing - audio thread will interpolate current toward this
		blendAggressionTarget_Q8_ = newBlendAggression;
		// Release store: ensures ALL writes (tables, params) are visible before audio sees isLinear_=false
		isLinear_.store(false, std::memory_order_release);
	}

	/// Integer table lookup - direct access to specified table buffer
	/// @param input Table input position (uint32 where 0 = -1.0, UINT32_MAX = +1.0)
	/// @param tableIdx Which table buffer to read from (0 or 1)
	/// @return Lookup value scaled by 65536 (Q16.15 format)
	[[gnu::always_inline]] int32_t lookupFunctionIntDirect(uint32_t input, int8_t tableIdx) const {
		constexpr int32_t kTableBits = (kTableSize == 128)    ? 7
		                               : (kTableSize == 256)  ? 8
		                               : (kTableSize == 512)  ? 9
		                               : (kTableSize == 1024) ? 10
		                               : (kTableSize == 2048) ? 11
		                                                      : 8; // default

		const int16_t* table = fTables_[tableIdx].data();

		// Extract table index (upper kTableBits bits of input)
		int32_t whichValue = input >> (32 - kTableBits);

		// Extract fractional part (next 16 bits after index)
		constexpr int32_t rshiftAmount = 32 - 16 - kTableBits;
		uint32_t rshifted = input >> rshiftAmount;
		int32_t strength2 = rshifted & 65535;
		int32_t strength1 = 65536 - strength2;

		// Linear interpolation with int16 table entries
		// Result is scaled by 65536 (Q16.15)
		return static_cast<int32_t>(table[whichValue]) * strength1
		       + static_cast<int32_t>(table[whichValue + 1]) * strength2;
	}

	// Integer tables for fast integer-only processing
	// Scale: float [-1,1] → int16_t [-32767,32767]
	// Double-buffer: regeneration writes to inactive buffer, then flips targetTableIndex_
	std::vector<int16_t> fTables_[2];    // Two table buffers for lock-free crossfade
	std::vector<float> fTableTempFloat_; // Temp float buffer for regeneration (avoids allocation)

	// Target-chasing crossfade: audio smoothly interpolates toward targetTableIndex_
	// This eliminates all races - we only atomically store ONE value, audio chases it
	// blend_Q15: 0 = 100% table 0, 32768 = 100% table 1
	std::atomic<int8_t> targetTableIndex_{0}; // Which table we're fading toward (0 or 1)
	mutable int32_t currentBlend_Q15_{0};     // Current blend position (Q15: 0-32768)
	// IIR alpha: α_Q15 = 4 → 99% in ~500ms at 44.1kHz (n = 4.6/α = 4.6*32768/4 = 37683 samples ≈ 854ms)
	static constexpr int32_t kBlendAlpha_Q15 = 4;

	// Parameters (consolidated struct)
	TableShaperParams params_;

	// State
	bool tablesDirty_{true};
	// Linear flag: controls whether audio thread accesses tables
	// release/acquire ordering ensures tables are visible before isLinear_ becomes false
	std::atomic<bool> isLinear_{true};

	// Blend aggression: derived from X (drive), affects mix curve sharpness
	// Q8 format: 256 = 1.0x, range [128, 512] for [0.5x, 2.0x]
	// Low X = gentle transitions, high X = snappy onset
	// Smoothed: target set during regeneration, current interpolates toward target
	int32_t blendAggressionMult_Q8_{256};   // Current (smoothed) value used by audio
	int32_t blendAggressionTarget_Q8_{256}; // Target value from latest regeneration

	// Expected peak level for int32 path (set at table generation time)
	// Used to normalize input/output so the table "expects" signals at this level
	// FM at max LOCAL_VOLUME + max OSC_VOLUME = 2^26 (~67M)
	// Calculation: sine(2^31) * sourceAmplitude(2^27) / 2^32 = 2^26
	// FM signal calibration (empirically determined):
	// - Theoretical max: 2^26 = 67M (sine * sourceAmplitude / 2^32, sourceAmplitude capped at 2^27)
	// - inputScale=256 (2^8) puts saturation onset near center drive for FM at max velocity
	// - At lower velocities: need positive drive to reach saturation (natural velocity response)
	// - Output = lookup >> inputScaleShift_ (unity gain: boost in, attenuate out)
	int32_t expectedPeak_{1 << 26}; // 67,108,864 - theoretical FM max (reference only)
	int32_t inputScaleShift_{7};    // Bit shift for input scaling (7 is slightly too much, 6 slightly too little)

public:
	/// Set the expected peak level for int32 processing (integer-only, no floats)
	/// Call this when synth mode changes (FM vs subtractive)
	void setExpectedPeak(int32_t peak) {
		expectedPeak_ = peak;
		// Compute shift using CLZ + 2: extra headroom for better saturation response
		// For peak = 2^26: CLZ = 5, shift = 7 (tuned empirically)
		inputScaleShift_ = (peak > 0) ? __builtin_clz(static_cast<uint32_t>(peak)) + 2 : 7;
	}

	[[nodiscard]] int32_t getExpectedPeak() const { return expectedPeak_; }
	[[nodiscard]] int32_t getInputScaleShift() const { return inputScaleShift_; }
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
