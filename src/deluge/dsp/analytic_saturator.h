/*
 * Copyright © 2024 Synthstrom Audible Limited
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
 */

#pragma once

#include "dsp/fast_math.h"
#include "util/fixedpoint.h"
#include "util/waves.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace deluge::dsp {

/**
 * Analytic Parametric Saturator with ADAA (Antiderivative Antialiasing)
 *
 * Features:
 * - 6 basis functions for rich harmonic exploration:
 *   1. Tanh (warm) - smooth saturation, odd harmonics
 *   2. Polynomial (bright) - soft clip x - x³/3, edgy character
 *   3. Hard knee (crisp) - aggressive limiting with sharp transition
 *   4. Chebyshev T5 (fold) - wavefolder, adds 5th harmonic richness
 *   5. Sine folder (synth) - sin(πx/2), smooth harmonic-rich folding
 *   6. Rectifier (diode) - asymmetric half-wave, even harmonics
 * - Drive parameter where 0 = linear bypass (transparent)
 * - Separate weights for each basis function (combinatoric via parameter phasing)
 * - Cached f(x) and F(x) tables for fast lookup
 * - Tables only regenerate when parameters change
 * - First-order ADAA using cached antiderivative table
 *
 * Parameters:
 * - drive (0-1): Overall intensity, 0 = bypass
 * - tanhWeight (0-1): Weight for tanh basis (warm, smooth)
 * - polyWeight (0-1): Weight for polynomial basis (bright, edgy)
 * - hardKneeWeight (0-1): Weight for hard knee basis (crisp, aggressive)
 * - chebyWeight (0-1): Weight for Chebyshev T5 basis (fold, synthy)
 * - sineFoldWeight (0-1): Weight for sine folder basis (harmonic-rich)
 * - rectifierWeight (0-1): Weight for rectifier basis (diode, asymmetric)
 * - threshold (0-1): Linear zone size (1=all linear, 0=always saturate)
 * - asymmetry (0-1): Even harmonics (0.5=symmetric)
 */
class AnalyticSaturator {
public:
	// =============================================================================
	// A/B TEST CONFIGURATION - Change these for testing different modes
	// =============================================================================
	// Table size: 512 (fast regen, less precise) vs 2048 (slow regen, more precise)
	static constexpr size_t kTableSize = 512;

	// Interpolation mode for antiderivative lookup:
	// false = linear (fast, ~10 ops)
	// true = cubic Catmull-Rom (slow, ~25 ops, smoother ADAA)
	static constexpr bool kUseCubicInterpolation = false;
	// =============================================================================

	static constexpr size_t kTableMask = kTableSize - 1;
	static constexpr float kTableScale = static_cast<float>(kTableSize) / 2.0f;

	AnalyticSaturator() { regenerateTables(); }

	/// Set drive (0 = linear bypass, 1 = full saturation)
	void setDrive(float drive) {
		drive = std::clamp(drive, 0.0f, 1.0f);
		if (drive != drive_) {
			drive_ = drive;
			tablesDirty_ = true;
		}
	}

	/// Set tanh basis weight (warm, smooth saturation)
	void setTanhWeight(float weight) {
		weight = std::clamp(weight, 0.0f, 1.0f);
		if (weight != tanhWeight_) {
			tanhWeight_ = weight;
			tablesDirty_ = true;
		}
	}

	/// Set polynomial basis weight (bright, edgy saturation)
	void setPolyWeight(float weight) {
		weight = std::clamp(weight, 0.0f, 1.0f);
		if (weight != polyWeight_) {
			polyWeight_ = weight;
			tablesDirty_ = true;
		}
	}

	/// Set hard knee basis weight (crisp, aggressive limiting)
	void setHardKneeWeight(float weight) {
		weight = std::clamp(weight, 0.0f, 1.0f);
		if (weight != hardKneeWeight_) {
			hardKneeWeight_ = weight;
			tablesDirty_ = true;
		}
	}

	/// Set Chebyshev T5 basis weight (fold, synthy waveshape)
	void setChebyWeight(float weight) {
		weight = std::clamp(weight, 0.0f, 1.0f);
		if (weight != chebyWeight_) {
			chebyWeight_ = weight;
			tablesDirty_ = true;
		}
	}

	/// Set sine folder basis weight (harmonic-rich folding)
	void setSineFoldWeight(float weight) {
		weight = std::clamp(weight, 0.0f, 1.0f);
		if (weight != sineFoldWeight_) {
			sineFoldWeight_ = weight;
			tablesDirty_ = true;
		}
	}

	/// Set rectifier basis weight (diode-like asymmetric)
	void setRectifierWeight(float weight) {
		weight = std::clamp(weight, 0.0f, 1.0f);
		if (weight != rectifierWeight_) {
			rectifierWeight_ = weight;
			tablesDirty_ = true;
		}
	}

	/// Set threshold for linear zone (1 = all linear, 0 = no linear zone)
	void setThreshold(float threshold) {
		threshold = std::clamp(threshold, 0.0f, 1.0f);
		if (threshold != threshold_) {
			threshold_ = threshold;
			tablesDirty_ = true;
		}
	}

	/// Set asymmetry (0.5 = symmetric, 0 or 1 = asymmetric/even harmonics)
	void setAsymmetry(float asymmetry) {
		asymmetry = std::clamp(asymmetry, 0.0f, 1.0f);
		if (asymmetry != asymmetry_) {
			asymmetry_ = asymmetry;
			tablesDirty_ = true;
		}
	}

	/// Set all parameters at once (more efficient than individual setters)
	void setParameters(float drive, float tanhWeight, float polyWeight, float hardKneeWeight, float chebyWeight,
	                   float sineFoldWeight, float rectifierWeight, float threshold, float asymmetry) {
		drive = std::clamp(drive, 0.0f, 1.0f);
		tanhWeight = std::clamp(tanhWeight, 0.0f, 1.0f);
		polyWeight = std::clamp(polyWeight, 0.0f, 1.0f);
		hardKneeWeight = std::clamp(hardKneeWeight, 0.0f, 1.0f);
		chebyWeight = std::clamp(chebyWeight, 0.0f, 1.0f);
		sineFoldWeight = std::clamp(sineFoldWeight, 0.0f, 1.0f);
		rectifierWeight = std::clamp(rectifierWeight, 0.0f, 1.0f);
		threshold = std::clamp(threshold, 0.0f, 1.0f);
		asymmetry = std::clamp(asymmetry, 0.0f, 1.0f);

		if (drive != drive_ || tanhWeight != tanhWeight_ || polyWeight != polyWeight_
		    || hardKneeWeight != hardKneeWeight_ || chebyWeight != chebyWeight_ || sineFoldWeight != sineFoldWeight_
		    || rectifierWeight != rectifierWeight_ || threshold != threshold_ || asymmetry != asymmetry_) {
			drive_ = drive;
			tanhWeight_ = tanhWeight;
			polyWeight_ = polyWeight;
			hardKneeWeight_ = hardKneeWeight;
			chebyWeight_ = chebyWeight;
			sineFoldWeight_ = sineFoldWeight;
			rectifierWeight_ = rectifierWeight;
			threshold_ = threshold;
			asymmetry_ = asymmetry;
			tablesDirty_ = true;
		}
	}

	/// Check if effect is effectively bypassed (transparent)
	[[nodiscard]] bool isLinear() const { return drive_ < 0.001f || threshold_ > 0.999f; }

	/// Process a single sample with ADAA (uses internal state)
	/// @param x Input sample in range [-1, 1]
	/// @return Processed sample (peak-normalized)
	[[gnu::always_inline]] float process(float x) { return process(x, &prevX_); }

	/// Process a single sample with ADAA using external state
	/// This allows one AnalyticSaturator (one set of tables) to serve multiple channels
	/// @param x Input sample in range [-1, 1]
	/// @param prevXState Pointer to previous sample state (updated in place)
	/// @return Processed sample (peak-normalized)
	[[gnu::always_inline]] float process(float x, float* prevXState) {
		// Headroom: normalizationGain_ already handles peak limiting, minimal extra needed
		constexpr float kHeadroom = 0.8f;

		// Ensure tables are current
		if (tablesDirty_) {
			regenerateTables();
		}

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
		// Wider blend zone reduces artifacts from the ADAA/direct transition
		// At 44.1kHz, a 1kHz full-scale sine changes by ~0.14 per sample
		constexpr float kMinDx = 0.01f;
		// Maximum dx for ADAA - above this, the antiderivative lookup becomes unreliable
		// (handles voice start, table regen, and large input jumps)
		constexpr float kMaxDx = 0.5f;

		if (absDx < 1e-7f || absDx > kMaxDx) {
			// Essentially static OR large jump - use direct lookup
			output = lookupFunction(x);
		}
		else if (absDx >= kMinDx) {
			// Normal ADAA range
			output = (F_curr - F_prev) / dx;
		}
		else {
			// Transition zone - smoothstep blend between ADAA and direct
			float adaaOutput = (F_curr - F_prev) / dx;
			float directOutput = lookupFunction(x);

			// Smoothstep: t² * (3 - 2t) gives smooth S-curve from 0 to 1
			float t = absDx / kMinDx;
			float blend = t * t * (3.0f - 2.0f * t);

			output = adaaOutput * blend + directOutput * (1.0f - blend);
		}

		*prevXState = x;

		// Apply peak normalization and headroom
		float result = output * normalizationGain_ * kHeadroom;

		// Safety clamp - prevent any extreme values that could cause clicks
		// This should never trigger if ADAA is working correctly, but provides protection
		return std::clamp(result, -1.0f, 1.0f);
	}

	/// Process with ADAA using cubic interpolation (smoother, more expensive)
	[[gnu::always_inline]] float processCubic(float x, float* prevXState) {
		constexpr float kHeadroom = 0.8f;

		if (tablesDirty_) {
			regenerateTables();
		}

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
		constexpr float kMaxDx = 0.5f;

		if (absDx < 1e-7f || absDx > kMaxDx) {
			output = lookupFunction(x);
		}
		else if (absDx >= kMinDx) {
			output = (F_curr - F_prev) / dx;
		}
		else {
			float adaaOutput = (F_curr - F_prev) / dx;
			float directOutput = lookupFunction(x);
			float t = absDx / kMinDx;
			float blend = t * t * (3.0f - 2.0f * t);
			output = adaaOutput * blend + directOutput * (1.0f - blend);
		}

		*prevXState = x;
		float result = output * normalizationGain_ * kHeadroom;
		return std::clamp(result, -1.0f, 1.0f);
	}

	/// Process a single sample (direct table lookup, no ADAA)
	/// Note: Uses same headroom as process() for consistent levels when toggling AA
	[[gnu::always_inline]] float processNoAA(float x) {
		// Headroom: must match process() for level consistency
		constexpr float kHeadroom = 0.8f;

		if (tablesDirty_) {
			regenerateTables();
		}

		if (isLinear_) {
			return x * kHeadroom;
		}

		// Apply peak normalization and headroom so levels match process() with ADAA
		return lookupFunction(x) * normalizationGain_ * kHeadroom;
	}

	/// Reset ADAA state (call when starting a new audio stream)
	void reset() {
		prevX_ = 0.0f;
		tablesDirty_ = true;
	}

	/// Get current parameters
	[[nodiscard]] float getDrive() const { return drive_; }
	[[nodiscard]] float getTanhWeight() const { return tanhWeight_; }
	[[nodiscard]] float getPolyWeight() const { return polyWeight_; }
	[[nodiscard]] float getHardKneeWeight() const { return hardKneeWeight_; }
	[[nodiscard]] float getChebyWeight() const { return chebyWeight_; }
	[[nodiscard]] float getSineFoldWeight() const { return sineFoldWeight_; }
	[[nodiscard]] float getRectifierWeight() const { return rectifierWeight_; }
	[[nodiscard]] float getThreshold() const { return threshold_; }
	[[nodiscard]] float getAsymmetry() const { return asymmetry_; }
	[[nodiscard]] float getNormalizationGain() const { return normalizationGain_; }

private:
	/// Regenerate both f(x) and F(x) tables based on current parameters
	/// Uses fast math approximations for speed during parameter automation.
	void regenerateTables() {
		tablesDirty_ = false;
		isLinear_ = isLinear();

		if (isLinear_) {
			// Linear: f(x) = x, F(x) = x²/2, normalization = 1
			normalizationGain_ = 1.0f;
			for (size_t i = 0; i <= kTableSize; ++i) {
				float x = (static_cast<float>(i) / kTableScale) - 1.0f; // -1 to +1
				fTable_[i] = x;
				FTable_[i] = x * x * 0.5f;
			}
			return;
		}

		// Compute effective parameters
		// Drive affects steepness (k) and threshold reduction
		float k = 1.0f + drive_ * 9.0f;                // Steepness: 1 to 10
		float T = threshold_ * (1.0f - drive_ * 0.8f); // Threshold shrinks with drive
		T = std::fmax(T, 0.05f);                       // Never fully zero

		// Asymmetry ratio for positive vs negative
		float asymRatio = 0.5f + asymmetry_; // 0.5 to 1.5

		// Precompute inverse normalization factors for tanh (using fast approximation)
		float invTanhNormPos = 1.0f / std::fmax(0.01f, fastTanh(k * asymRatio));
		float invTanhNormNeg = 1.0f / std::fmax(0.01f, fastTanh(k * (2.0f - asymRatio)));

		// Precompute weight normalization for all 6 basis functions
		float weightSum =
		    tanhWeight_ + polyWeight_ + hardKneeWeight_ + chebyWeight_ + sineFoldWeight_ + rectifierWeight_;
		float invWeightSum = (weightSum > 0.001f) ? (1.0f / weightSum) : 1.0f;
		bool hasWeights = (weightSum >= 0.001f);

		// Generate tables - track peak for normalization
		float peakValue = 0.0f;
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
				float intensity = 1.0f + drive_ * 1.0f;
				float overdriven = norm * intensity;

				// Asymmetric k for positive/negative
				float kEff = k * ((x >= 0.0f) ? asymRatio : (2.0f - asymRatio));
				float invTanhNorm = (x >= 0.0f) ? invTanhNormPos : invTanhNormNeg;

				// =========================================================
				// BASIS 1: Tanh (warm, smooth) - using fast approximation
				// Classic tube-like saturation, pure odd harmonics
				// Overdrive creates harder saturation
				// =========================================================
				float tanh_out = fastTanh(kEff * overdriven) * invTanhNorm;

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
				constexpr float kSineFoldA = 0.4f;                      // Tanh envelope softness
				float sineFoldB = 3.14159265f * (1.0f + drive_ * 1.0f); // 1-2 folds
				float sineFold_raw =
				    fastTanh(overdriven / kSineFoldA) * std::sin(sineFoldB * overdriven) + fastTanh(overdriven) * 0.3f;
				float sineFold_out = std::fabs(sineFold_raw);
				sineFold_out = std::fmin(sineFold_out, 1.0f);

				// =========================================================
				// BASIS 6: Rectifier (diode) - asymmetric, even harmonics
				// Full-wave rectifier with variable bias for rich even harmonics
				// =========================================================
				float bias = 0.2f * drive_; // Adds DC offset for even harmonics
				float rect_raw = std::fabs(overdriven + bias) - bias;
				float rect_out = fastTanh(rect_raw * 2.0f); // Soft limit

				// Blend using weights (normalized by sum)
				float basis_out;
				if (!hasWeights) {
					basis_out = norm;
				}
				else {
					basis_out =
					    (tanh_out * tanhWeight_ + poly_out * polyWeight_ + hardClip_out * hardKneeWeight_
					     + cheby_out * chebyWeight_ + sineFold_out * sineFoldWeight_ + rect_out * rectifierWeight_)
					    * invWeightSum;
				}

				// ==========================================================
				// Drive-dependent blend toward linear (like tanh envelope)
				// At drive=0: output = norm (completely linear/transparent)
				// At drive=1: output = basis_out (full nonlinear character)
				// This unifies X-axis control across all basis functions
				// ==========================================================
				basis_out = norm + (basis_out - norm) * drive_;

				// Map back to output range
				f_val = sign * (T + (1.0f - T) * std::fabs(basis_out));
			}

			fTable_[i] = f_val;

			// Track peak for normalization
			float absVal = std::fabs(f_val);
			if (absVal > peakValue) {
				peakValue = absVal;
			}

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

		// Compute peak normalization gain
		// This ensures output peak matches input peak regardless of basis/parameter settings
		normalizationGain_ = (peakValue > 0.01f) ? (1.0f / peakValue) : 1.0f;
	}

	/// Lookup f(x) from cached table with linear interpolation
	[[gnu::always_inline]] float lookupFunction(float x) const {
		// Map x from [-1, 1] to [0, kTableSize]
		float idx = (x + 1.0f) * kTableScale;
		idx = std::clamp(idx, 0.0f, static_cast<float>(kTableSize));

		size_t i0 = static_cast<size_t>(idx);
		size_t i1 = std::min(i0 + 1, kTableSize);
		float frac = idx - static_cast<float>(i0);

		return fTable_[i0] + (fTable_[i1] - fTable_[i0]) * frac;
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

	/// Lookup F(x) (antiderivative) from cached table
	/// Uses linear or cubic interpolation based on kUseCubicInterpolation flag
	[[gnu::always_inline]] float lookupAntiderivative(float x) const {
		// Map x from [-1, 1] to [0, kTableSize]
		float idx = (x + 1.0f) * kTableScale;
		idx = std::clamp(idx, 0.0f, static_cast<float>(kTableSize));

		size_t i0 = static_cast<size_t>(idx);
		float frac = idx - static_cast<float>(i0);

		if constexpr (kUseCubicInterpolation) {
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
	std::array<float, kTableSize + 1> fTable_{}; // f(x) values
	std::array<float, kTableSize + 1> FTable_{}; // F(x) antiderivative values

	// Parameters
	float drive_{0.0f};
	float tanhWeight_{1.0f}; // Default to pure tanh (warm)
	float polyWeight_{0.0f};
	float hardKneeWeight_{0.0f};
	float chebyWeight_{0.0f};
	float sineFoldWeight_{0.0f};
	float rectifierWeight_{0.0f};
	float threshold_{1.0f};
	float asymmetry_{0.5f};

	// State
	bool tablesDirty_{true};
	bool isLinear_{true};
	float prevX_{0.0f};
	float normalizationGain_{1.0f}; // Peak normalization (computed during table regen)
};

/**
 * Helper to derive saturator parameters from XY position with combinatoric sweep
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
 * - Irrational period ratios: ensure the pattern never exactly repeats
 *
 * Result: distinct character zones at low Y, fragmented/chaotic at high Y
 */
struct AnalyticSaturatorXYMapper {
	// =================================================================
	// Duty cycle controls the active/gap ratio of basis oscillators
	// 0.25 = 25% active, 75% gap (very sparse, distinct characters)
	// 0.5  = 50% active, 50% gap (balanced, default)
	// 0.75 = 75% active, 25% gap (more blending, smoother)
	// 1.0  = 100% active, no gaps (original continuous triangle)
	// =================================================================
	static constexpr float kPhaseWidth = 0.5f;

	/// PhaseWidth-controlled triangle wave with dead zones
	/// Wrapper for triangleWithDeadzone from util/waves.h
	/// @param phase Float phase (0-N cycles, wraps naturally)
	/// @param width Active portion (0-1), rest is dead zone at 0
	/// @return Float value 0-1
	static float triangle(float phase, float width = kPhaseWidth) {
		constexpr float kPhaseScale = 4294967296.0f;
		constexpr float kInvQ31 = 1.0f / static_cast<float>(0x7FFFFFFF);
		// Wrap to [0,1) before scaling to avoid UB from float->uint32 overflow
		phase = std::fmod(phase, 1.0f);
		if (phase < 0.0f) {
			phase += 1.0f;
		}
		uint32_t phaseU32 = static_cast<uint32_t>(phase * kPhaseScale);
		// width is fraction of cycle that's active
		uint32_t phaseWidth = static_cast<uint32_t>(width * 4294967295.0f);
		return static_cast<float>(triangleWithDeadzone(phaseU32, phaseWidth)) * kInvQ31;
	}

	/// Derive parameters from X (0-127) and Y (0-1023) with combinatoric sweep
	/// @param x X position (0-127), maps to drive (0 = linear bypass)
	/// @param y Y position (0-1023), creates high-res combinatoric parameter sweep
	/// @param outDrive Output drive parameter
	/// @param outTanhWeight Output tanh basis weight (warm)
	/// @param outPolyWeight Output polynomial basis weight (bright)
	/// @param outHardKneeWeight Output hard knee basis weight (crisp)
	/// @param outChebyWeight Output Chebyshev T5 basis weight (fold)
	/// @param outSineFoldWeight Output sine folder basis weight (gold)
	/// @param outRectifierWeight Output rectifier basis weight (diode)
	/// @param outThreshold Output threshold parameter
	/// @param outAsymmetry Output asymmetry parameter
	static void deriveParameters(uint8_t x, uint16_t y, float& outDrive, float& outTanhWeight, float& outPolyWeight,
	                             float& outHardKneeWeight, float& outChebyWeight, float& outSineFoldWeight,
	                             float& outRectifierWeight, float& outThreshold, float& outAsymmetry) {
		// X maps directly to drive (0 = bypass/linear, 127 = full)
		outDrive = static_cast<float>(x) / 127.0f;

		// Y creates combinatoric sweep using triangle waves with different periods
		// 1024 steps for high-resolution zone exploration
		float yNorm = static_cast<float>(y) / 1023.0f;

		// =================================================================
		// Accelerating interference pattern: frequency increases with Y
		// At Y=0: slow, gradual parameter changes (easy to find sweet spots)
		// At Y=1023: fast, chaotic interference (rich exploration)
		// freq(y) = base_freq * (1 + y² * accel), quadratic for smooth ramp
		// =================================================================
		constexpr float kAccelFactor = 3.0f; // Max 4x frequency at Y=1023
		float freqMult = 1.0f + yNorm * yNorm * kAccelFactor;

		// =================================================================
		// 6 Basis weights with irrational period ratios for dense coverage
		// Each basis has a unique period ensuring all combinations are explored
		// =================================================================

		// Basis 1: Tanh (warm) - period 3, always has minimum weight for smooth foundation
		outTanhWeight = 0.2f + triangle(yNorm * 3.0f * freqMult) * 0.8f; // 0.2 to 1.0

		// Basis 2: Polynomial (bright) - period e (2.718)
		outPolyWeight = triangle(yNorm * 2.718f * freqMult + 0.167f); // 0 to 1.0

		// Basis 3: Hard knee (crisp) - period √5 (2.236)
		outHardKneeWeight = triangle(yNorm * 2.236f * freqMult + 0.333f); // 0 to 1.0

		// Basis 4: Chebyshev T5 (fold) - period π (3.14159)
		outChebyWeight = triangle(yNorm * 3.14159f * freqMult + 0.5f); // 0 to 1.0

		// Basis 5: Sine folder/Gold (harmonic) - period φ² (2.618)
		outSineFoldWeight = triangle(yNorm * 2.618f * freqMult + 0.667f); // 0 to 1.0

		// Basis 6: Rectifier (diode) - period √3 (1.732)
		outRectifierWeight = triangle(yNorm * 1.732f * freqMult + 0.833f); // 0 to 1.0

		// Threshold: period ~2.5, also accelerates
		// Interacts with basis weights to create soft/hard variants
		outThreshold = triangle(yNorm * 2.5f * freqMult + 0.25f);

		// Asymmetry: period φ (1.618, golden ratio), slower acceleration
		// Subtle parameter, uses sqrt of freqMult for gentler variation
		float asymFreqMult = 1.0f + yNorm * yNorm * (kAccelFactor * 0.5f);
		float asymPhase = triangle(yNorm * 1.618f * asymFreqMult);
		outAsymmetry = 0.3f + asymPhase * 0.4f; // Range 0.3 to 0.7 (subtle asymmetry)
	}

	/// Derive parameters with phase offsets for DOTT vibe/feel integration
	/// @param x X position (0-127)
	/// @param y Y position (0-1023)
	/// @param phaseOffset Phase offset for parameter interference (from vibe knob)
	/// @param periodScale Period scaling for parameter sweep rate (from feel knob)
	static void deriveParametersWithPhase(uint8_t x, uint16_t y, float phaseOffset, float periodScale, float& outDrive,
	                                      float& outTanhWeight, float& outPolyWeight, float& outHardKneeWeight,
	                                      float& outChebyWeight, float& outSineFoldWeight, float& outRectifierWeight,
	                                      float& outThreshold, float& outAsymmetry) {
		outDrive = static_cast<float>(x) / 127.0f;

		float yNorm = static_cast<float>(y) / 1023.0f;

		// Accelerating frequency: slow at Y=0, fast at Y=1023
		constexpr float kAccelFactor = 3.0f;
		float freqMult = 1.0f + yNorm * yNorm * kAccelFactor;

		// Apply phase offsets and period scaling for interference patterns
		// Phase offsets create evolving timbral changes under automation
		outTanhWeight = 0.2f + triangle(yNorm * 3.0f * freqMult * periodScale + phaseOffset * 0.0f) * 0.8f;
		outPolyWeight = triangle(yNorm * 2.718f * freqMult * periodScale + phaseOffset * 0.167f);
		outHardKneeWeight = triangle(yNorm * 2.236f * freqMult * periodScale + phaseOffset * 0.333f);
		outChebyWeight = triangle(yNorm * 3.14159f * freqMult * periodScale + phaseOffset * 0.5f);
		outSineFoldWeight = triangle(yNorm * 2.618f * freqMult * periodScale + phaseOffset * 0.667f);
		outRectifierWeight = triangle(yNorm * 1.732f * freqMult * periodScale + phaseOffset * 0.833f);

		outThreshold = triangle(yNorm * 2.5f * freqMult * periodScale + phaseOffset * 0.25f);
		float asymFreqMult = 1.0f + yNorm * yNorm * (kAccelFactor * 0.5f);
		float asymPhase = triangle(yNorm * 1.618f * asymFreqMult * periodScale + phaseOffset * 0.618f);
		outAsymmetry = 0.3f + asymPhase * 0.4f;
	}
};

} // namespace deluge::dsp
