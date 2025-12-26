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
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace deluge::dsp {

/**
 * Analytic Parametric Saturator with ADAA (Antiderivative Antialiasing)
 *
 * Features:
 * - 3 basis functions: Tanh (warm), Polynomial (bright), Chebyshev (fold)
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
 * - chebyWeight (0-1): Weight for Chebyshev T5 basis (fold, synthy)
 * - threshold (0-1): Linear zone size (1=all linear, 0=always saturate)
 * - asymmetry (0-1): Even harmonics (0.5=symmetric)
 */
class AnalyticSaturator {
public:
	static constexpr size_t kTableSize = 512;
	static constexpr size_t kTableMask = kTableSize - 1;
	static constexpr float kTableScale = static_cast<float>(kTableSize) / 2.0f; // Maps [-1,1] to [0,512]

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

	/// Set Chebyshev T5 basis weight (fold, synthy waveshape)
	void setChebyWeight(float weight) {
		weight = std::clamp(weight, 0.0f, 1.0f);
		if (weight != chebyWeight_) {
			chebyWeight_ = weight;
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
	void setParameters(float drive, float tanhWeight, float polyWeight, float chebyWeight, float threshold,
	                   float asymmetry) {
		drive = std::clamp(drive, 0.0f, 1.0f);
		tanhWeight = std::clamp(tanhWeight, 0.0f, 1.0f);
		polyWeight = std::clamp(polyWeight, 0.0f, 1.0f);
		chebyWeight = std::clamp(chebyWeight, 0.0f, 1.0f);
		threshold = std::clamp(threshold, 0.0f, 1.0f);
		asymmetry = std::clamp(asymmetry, 0.0f, 1.0f);

		if (drive != drive_ || tanhWeight != tanhWeight_ || polyWeight != polyWeight_ || chebyWeight != chebyWeight_
		    || threshold != threshold_ || asymmetry != asymmetry_) {
			drive_ = drive;
			tanhWeight_ = tanhWeight;
			polyWeight_ = polyWeight;
			chebyWeight_ = chebyWeight;
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
		// Ensure tables are current
		if (tablesDirty_) {
			regenerateTables();
		}

		// Fast path: bypass when linear
		if (isLinear_) {
			*prevXState = x;
			return x;
		}

		// First-order ADAA: output = (F(x) - F(x_prev)) / (x - x_prev)
		// Where F(x) is the antiderivative of f(x)
		float F_curr = lookupAntiderivative(x);
		float F_prev = lookupAntiderivative(*prevXState);

		float dx = x - *prevXState;
		float output;

		if (std::fabs(dx) > 1e-6f) {
			// Normal ADAA
			output = (F_curr - F_prev) / dx;
		}
		else {
			// Samples too close, use direct lookup to avoid division issues
			output = lookupFunction(x);
		}

		*prevXState = x;

		// Apply peak normalization for volume-neutral saturation
		return output * normalizationGain_;
	}

	/// Process a single sample without ADAA (faster, more aliasing)
	[[gnu::always_inline]] float processNoAA(float x) {
		if (tablesDirty_) {
			regenerateTables();
		}

		if (isLinear_) {
			return x;
		}

		// Apply peak normalization for volume-neutral saturation
		return lookupFunction(x) * normalizationGain_;
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
	[[nodiscard]] float getChebyWeight() const { return chebyWeight_; }
	[[nodiscard]] float getThreshold() const { return threshold_; }
	[[nodiscard]] float getAsymmetry() const { return asymmetry_; }

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

		// Precompute weight normalization
		float weightSum = tanhWeight_ + polyWeight_ + chebyWeight_;
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

				// Asymmetric k for positive/negative
				float kEff = k * ((x >= 0.0f) ? asymRatio : (2.0f - asymRatio));
				float invTanhNorm = (x >= 0.0f) ? invTanhNormPos : invTanhNormNeg;

				// Basis 1: Tanh (warm, smooth) - using fast approximation
				float tanh_out = fastTanh(kEff * norm) * invTanhNorm;

				// Basis 2: Polynomial soft clip (bright, edgy)
				// f(x) = x - x³/3, normalized to [0,1] output for [0,1] input
				float norm3 = norm * norm * norm;
				float poly_out = (norm - norm3 * 0.333333f) * 1.5f; // Fused multiply
				poly_out = std::fmin(poly_out, 1.0f);

				// Basis 3: Chebyshev T5 (fold, synthy)
				// T5(x) = 16x⁵ - 20x³ + 5x - optimized with Horner's method
				float n2 = norm * norm;
				float cheby_out = norm * (5.0f + n2 * (-20.0f + n2 * 16.0f));
				cheby_out = std::fmax(-1.0f, std::fmin(1.0f, cheby_out));

				// Blend using weights (normalized by sum)
				float basis_out;
				if (!hasWeights) {
					basis_out = norm;
				}
				else {
					basis_out =
					    (tanh_out * tanhWeight_ + poly_out * polyWeight_ + cheby_out * chebyWeight_) * invWeightSum;
				}

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

	/// Lookup F(x) (antiderivative) from cached table with linear interpolation
	[[gnu::always_inline]] float lookupAntiderivative(float x) const {
		// Map x from [-1, 1] to [0, kTableSize]
		float idx = (x + 1.0f) * kTableScale;
		idx = std::clamp(idx, 0.0f, static_cast<float>(kTableSize));

		size_t i0 = static_cast<size_t>(idx);
		size_t i1 = std::min(i0 + 1, kTableSize);
		float frac = idx - static_cast<float>(i0);

		return FTable_[i0] + (FTable_[i1] - FTable_[i0]) * frac;
	}

	// Cached tables (regenerated when parameters change)
	std::array<float, kTableSize + 1> fTable_{}; // f(x) values
	std::array<float, kTableSize + 1> FTable_{}; // F(x) antiderivative values

	// Parameters
	float drive_{0.0f};
	float tanhWeight_{1.0f}; // Default to pure tanh (warm)
	float polyWeight_{0.0f};
	float chebyWeight_{0.0f};
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
 * using triangle waves with different periods for parameter interference
 *
 * Parameter phasing: Each parameter oscillates at a different period, creating
 * dense coverage of the parameter space as Y is swept. The irrational period
 * ratios ensure the pattern never repeats exactly.
 */
struct AnalyticSaturatorXYMapper {
	/// Triangle wave function: 0→1→0 over period
	static float triangle(float phase) {
		phase = std::fmod(phase, 1.0f);
		if (phase < 0.0f)
			phase += 1.0f;
		return (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
	}

	/// Derive parameters from X (0-127) and Y (0-127) with combinatoric sweep
	/// @param x X position (0-127), maps to drive (0 = linear bypass)
	/// @param y Y position (0-127), creates combinatoric parameter sweep
	/// @param outDrive Output drive parameter
	/// @param outTanhWeight Output tanh basis weight
	/// @param outPolyWeight Output polynomial basis weight
	/// @param outChebyWeight Output Chebyshev basis weight
	/// @param outThreshold Output threshold parameter
	/// @param outAsymmetry Output asymmetry parameter
	static void deriveParameters(uint8_t x, uint8_t y, float& outDrive, float& outTanhWeight, float& outPolyWeight,
	                             float& outChebyWeight, float& outThreshold, float& outAsymmetry) {
		// X maps directly to drive (0 = bypass/linear, 127 = full)
		outDrive = static_cast<float>(x) / 127.0f;

		// Y creates combinatoric sweep using triangle waves with different periods
		// Irrational ratios ensure dense coverage without exact repetition
		float yNorm = static_cast<float>(y) / 127.0f;

		// Basis weights: use phased triangle waves to explore combinations
		// Period ratios chosen to maximize coverage: 3, e, π (irrational)
		outTanhWeight = triangle(yNorm * 3.0f);               // 3 cycles
		outPolyWeight = triangle(yNorm * 2.718f + 0.333f);    // e cycles, phase offset
		outChebyWeight = triangle(yNorm * 3.14159f + 0.666f); // π cycles, phase offset

		// Threshold: period ~2.5 (irrational-ish for dense coverage)
		// Interacts with basis weights to create soft/hard variants
		outThreshold = triangle(yNorm * 2.5f + 0.25f);

		// Asymmetry: period ~1.618 (golden ratio for never-repeating pattern)
		// Subtle parameter, longest effective period
		float asymPhase = triangle(yNorm * 1.618f);
		outAsymmetry = 0.3f + asymPhase * 0.4f; // Range 0.3 to 0.7 (subtle asymmetry)
	}

	/// Derive parameters with phase offsets for DOTT vibe/feel integration
	/// @param x X position (0-127)
	/// @param y Y position (0-127)
	/// @param phaseOffset Phase offset for parameter interference (from vibe knob)
	/// @param periodScale Period scaling for parameter sweep rate (from feel knob)
	static void deriveParametersWithPhase(uint8_t x, uint8_t y, float phaseOffset, float periodScale, float& outDrive,
	                                      float& outTanhWeight, float& outPolyWeight, float& outChebyWeight,
	                                      float& outThreshold, float& outAsymmetry) {
		outDrive = static_cast<float>(x) / 127.0f;

		float yNorm = static_cast<float>(y) / 127.0f;

		// Apply phase offsets and period scaling for interference patterns
		// Phase offsets create evolving timbral changes under automation
		outTanhWeight = triangle(yNorm * 3.0f * periodScale + phaseOffset * 0.0f);
		outPolyWeight = triangle(yNorm * 2.718f * periodScale + phaseOffset * 0.333f);
		outChebyWeight = triangle(yNorm * 3.14159f * periodScale + phaseOffset * 0.666f);

		outThreshold = triangle(yNorm * 2.5f * periodScale + phaseOffset * 0.25f);
		float asymPhase = triangle(yNorm * 1.618f * periodScale + phaseOffset * 0.618f);
		outAsymmetry = 0.3f + asymPhase * 0.4f;
	}
};

} // namespace deluge::dsp
