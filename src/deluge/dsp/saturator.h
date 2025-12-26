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

#include "deluge/dsp/analytic_saturator.h"
#include "deluge/util/fixedpoint.h"
#include "dsp_ng/core/types.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace deluge::dsp {

/**
 * Saturator with dynamically generated waveshaping table
 *
 * Features:
 * - X/Y shape control for creative distortion curves
 * - Linear zone near origin for clean pass-through at low drive
 * - 256-entry lookup table regenerated when shape changes
 * - Post-filter for anti-aliasing
 * - Zone 4 (Y >= 128): Analytic parametric saturator with ADAA
 *
 * Shape X (0-127): Soft → Hard (controls knee/clipping aggressiveness)
 * Shape Y (0-127): Clean → Weird (controls harmonic character)
 * Shape Y (128-255): Zone 4 - Analytic ADAA saturator with combinatoric sweep
 */
class Saturator {
public:
	static constexpr size_t kTableSize = 256;
	static constexpr size_t kTableMask = kTableSize - 1;
	static constexpr uint8_t kZone4Threshold = 128; // Y >= 128 triggers Zone 4

	Saturator() { regenerateTable(0, 0); }

	/**
	 * Regenerate the lookup table based on shape parameters
	 * Call this when shapeX or shapeY changes (not during audio processing)
	 * @param shapeX Soft→Hard axis (0-127, or 0-255 for Zone 4 drive)
	 * @param shapeY Clean→Weird axis (0-127 for zones 0-3, 128-255 for Zone 4)
	 */
	void regenerateTable(uint8_t shapeX, uint8_t shapeY) {
		shapeX_ = shapeX;
		shapeY_ = shapeY;

		// Check if we're in Zone 4 (analytic mode)
		if (shapeY >= kZone4Threshold) {
			useAnalytic_ = true;
			// Derive analytic parameters from X and Y
			// X maps to drive (0 = linear bypass), Y-128 maps to combinatoric sweep
			float drive, tanhWeight, polyWeight, chebyWeight, threshold, asymmetry;
			AnalyticSaturatorXYMapper::deriveParameters(shapeX, shapeY - kZone4Threshold, drive, tanhWeight, polyWeight,
			                                            chebyWeight, threshold, asymmetry);
			// Set parameters on single saturator (tables shared, state is per-channel)
			analyticSat_.setParameters(drive, tanhWeight, polyWeight, chebyWeight, threshold, asymmetry);
			return;
		}

		useAnalytic_ = false;
		float x_norm = shapeX / 127.0f; // 0-1
		float y_norm = shapeY / 127.0f; // 0-1

		for (size_t i = 0; i < kTableSize + 1; ++i) {
			// Map table index to -1..+1 range
			float input = (static_cast<float>(i) - 128.0f) / 128.0f;

			float output = computeShape(input, x_norm, y_norm);

			// Clamp and convert to int16
			output = std::fmax(-1.0f, std::fmin(1.0f, output));
			table_[i] = static_cast<int16_t>(output * 32767.0f);
		}
	}

	/**
	 * Process a single sample through the saturator
	 * @param input Sample to process (q31)
	 * @param drive Input gain (q31, higher = more saturation)
	 * @param channel 0 for left, 1 for right (used for ADAA state in Zone 4)
	 * @return Shaped sample (q31)
	 */
	[[gnu::always_inline]] inline q31_t process(q31_t input, q31_t drive, int channel = 0) {
		// Zone 4: Use analytic saturator with ADAA
		if (useAnalytic_) {
			// Convert q31 to float [-1, 1]
			float inputF = static_cast<float>(input) / 2147483648.0f;

			// Apply external drive scaling (Zone 4 has internal drive from X axis)
			// External drive provides additional boost
			float driveScale = 1.0f + (static_cast<float>(drive) / 2147483648.0f + 1.0f) * 2.0f;
			inputF *= driveScale;
			inputF = std::clamp(inputF, -1.0f, 1.0f);

			// Process with ADAA - use external state pointer per channel for stereo
			float* prevXState = (channel == 0) ? &analyticPrevXL_ : &analyticPrevXR_;
			float outputF = analyticSat_.process(inputF, prevXState);

			// Convert back to q31
			return static_cast<q31_t>(outputF * 2147483647.0f);
		}

		// Zones 0-3: Original table-based processing
		// Apply drive (input gain)
		// Drive range: 0.0625x (min/CCW) to 8x (max/CW) - 128x total range
		// Center position (~0) gives approximately 4x gain
		// Uses 64-bit math for full precision, no stepping

		// drive is q31: -2^31 to +2^31-1
		// Convert to unsigned 0-0xFFFFFFFF range for linear mapping
		uint32_t driveUnsigned = static_cast<uint32_t>(drive) + 0x80000000u;

		// Linear gain mapping with full 64-bit precision:
		// gain = 0.0625 + (driveUnsigned / 0xFFFFFFFF) * 7.9375
		// At driveUnsigned = 0:          gain = 0.0625 (1/16x, -24dB)
		// At driveUnsigned = 0x80000000: gain ≈ 4.0    (center-ish)
		// At driveUnsigned = 0xFFFFFFFF: gain = 8.0    (8x, +18dB)

		// Compute in 64-bit to avoid precision loss
		// We'll work in a scaled space where 2^32 = 1.0
		uint64_t gainScaled = (1ULL << 28); // 0.0625 in our 2^32 = 1.0 space (1/16x, -24dB)
		// Add driveUnsigned * 7.9375 = driveUnsigned * (127/16) = (driveUnsigned * 127) >> 4
		gainScaled += (static_cast<uint64_t>(driveUnsigned) * 127ULL) >> 4;

		// Apply to input: result = input * gainScaled / 2^32
		// Problem: input (32 bits) * gainScaled (up to 35 bits) = 67 bits, overflows int64_t
		// Solution: pre-shift gainScaled by 8 bits, adjust final shift accordingly
		// Max product after fix: 2^31 * 2^27 = 2^58 (safely fits in int64_t)
		int64_t driven64 = (static_cast<int64_t>(input) * static_cast<int64_t>(gainScaled >> 8)) >> 24;

		// Saturate to q31 range
		q31_t driven =
		    static_cast<q31_t>(std::clamp(driven64, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
		                                  static_cast<int64_t>(std::numeric_limits<int32_t>::max())));

		// Convert to table index (driven is q31, we want 0-255)
		// Add 0x80000000 to shift from signed to unsigned, then take top 8 bits
		uint32_t tableIndexFull = static_cast<uint32_t>(driven) + 0x80000000u;
		uint32_t tableIndex = tableIndexFull >> 24; // 0-255

		// Fractional part for interpolation (next 8 bits)
		uint32_t frac = (tableIndexFull >> 16) & 0xFF;

		// Linear interpolation between table entries
		int32_t val0 = table_[tableIndex];
		int32_t val1 = table_[tableIndex + 1];
		int32_t interpolated = val0 + (((val1 - val0) * static_cast<int32_t>(frac)) >> 8);

		// Convert back to q31 (table is int16, so shift up)
		return interpolated << 16;
	}

	/// Check if currently in Zone 4 (analytic mode)
	[[nodiscard]] bool isAnalyticMode() const { return useAnalytic_; }

	/// Check if effect is transparent (Zone 4 with zero drive)
	[[nodiscard]] bool isTransparent() const { return useAnalytic_ && analyticSat_.isLinear(); }

	/// Get the analytic saturator for direct parameter access
	/// Note: L/R channels share parameters and tables, only ADAA state is separate
	[[nodiscard]] AnalyticSaturator& getAnalyticSaturator() { return analyticSat_; }
	[[nodiscard]] const AnalyticSaturator& getAnalyticSaturator() const { return analyticSat_; }

	/// Reset ADAA state for both channels (call when starting new audio stream)
	void resetAnalyticState() {
		analyticPrevXL_ = 0.0f;
		analyticPrevXR_ = 0.0f;
	}

	/**
	 * Process a sample with wet/dry mix and smoothing filter
	 * @param input Sample to process
	 * @param drive Input gain
	 * @param mix Wet/dry blend (0 = bypass, ONE_Q31 = full wet)
	 * @param filterState Post-filter state for anti-aliasing
	 * @param channel 0 for left, 1 for right (used for ADAA state in Zone 4)
	 * @return Processed sample
	 */
	[[gnu::always_inline]] inline q31_t processWithMix(q31_t input, q31_t drive, q31_t mix, q31_t* filterState,
	                                                   int channel = 0) {
		if (mix <= 0) {
			return input; // Full bypass
		}

		q31_t wet = process(input, drive, channel);

		// Simple 1-pole lowpass for anti-aliasing (~12kHz at 44.1kHz)
		// Using y = (1-a)*y + a*x form to avoid overflow from (x-y) subtraction
		// alpha = 0.82, 1-alpha = 0.18
		constexpr q31_t kFilterAlpha = static_cast<q31_t>(0.82 * ONE_Q31);
		constexpr q31_t kFilterOneMinusAlpha = static_cast<q31_t>(0.18 * ONE_Q31);
		*filterState = add_saturate(multiply_32x32_rshift32(*filterState, kFilterOneMinusAlpha) << 1,
		                            multiply_32x32_rshift32(wet, kFilterAlpha) << 1);
		wet = *filterState;

		// Wet/dry crossfade
		q31_t dry = multiply_32x32_rshift32(input, ONE_Q31 - mix) << 1;
		wet = multiply_32x32_rshift32(wet, mix) << 1;

		return add_saturate(dry, wet);
	}

	[[nodiscard]] uint8_t getShapeX() const { return shapeX_; }
	[[nodiscard]] uint8_t getShapeY() const { return shapeY_; }

private:
	/**
	 * Compute the waveshaping function for a given input and shape parameters
	 * X axis: Soft (early compression, lower ceiling) → Hard (linear until harsh clip)
	 * Y axis: Clean → Rectify → Fold → Crush (4 distinct zones, no subtle blending)
	 *
	 * These are DRAMATIC differences - meant to be obviously audible.
	 */
	static float computeShape(float x, float xNorm, float yNorm) {
		float sign = (x >= 0.0f) ? 1.0f : -1.0f;
		float mag = std::fabs(x);

		// === X axis: Controls saturation intensity (affects ALL Y modes) ===
		// X=0 (CCW): Clean/linear - minimal coloration
		// X=127 (CW): Heavy saturation - aggressive compression + harmonics
		float satAmount = xNorm; // 0 = clean, 1 = saturated

		// Saturation curve intensity controlled by X
		// At X=0: denominator = 1.0 (linear), At X=127: denominator = 0.15 (aggressive)
		float satDenom = 1.0f - satAmount * 0.85f; // 1.0 down to 0.15

		// === Y axis: 4 DISTINCT character modes ===
		// Y 0-31:   Standard saturation
		// Y 32-63:  Asymmetric tube (even harmonics - warm/vintage)
		// Y 64-95:  Hard clip with linear zone + shaped flat
		// Y 96-127: Crossover distortion (dead zone - buzzy/aggressive)
		//
		// Within each zone: higher Y = more intense version of that character

		float output;
		int yZone = std::min(3, static_cast<int>(yNorm * 4.0f));   // 0, 1, 2, or 3
		float zoneBlend = std::fmin(1.0f, (yNorm * 4.0f) - yZone); // 0-1 within zone

		if (yZone == 0) {
			// Zone 0: Standard symmetric saturation
			// X controls compression curve, zoneBlend controls odd harmonic emphasis
			// Higher zoneBlend = more cubic/5th harmonic character (buzzier)
			float curve = mag / (satDenom + mag * (1.0f - satDenom));

			// Add progressively more odd harmonics as Y increases within zone
			// zoneBlend 0 = pure saturation curve
			// zoneBlend 1 = significant cubic character
			float mag2 = mag * mag;
			float mag3 = mag2 * mag;
			float oddHarmonics = mag3 * (1.0f - mag2 * 0.5f); // Cubic with slight 5th
			oddHarmonics = std::fmin(oddHarmonics, 1.0f);

			float blend = zoneBlend * 0.6f; // Up to 60% odd harmonic content
			output = curve * (1.0f - blend) + oddHarmonics * blend;
			output = std::fmin(output, 1.0f) * sign;
		}
		else if (yZone == 1) {
			// Zone 1: Asymmetric tube saturation (even harmonics)
			// X controls overall saturation
			// zoneBlend controls asymmetry amount: 0 = slight, 1 = extreme
			float asymmetry = 0.15f + zoneBlend * 0.7f; // 0.15 to 0.85

			// Positive: progressively softer with more asymmetry
			// Negative: progressively harder with more asymmetry
			float posDenom = satDenom + asymmetry * 0.4f;          // Gets softer
			float negDenom = satDenom * (1.0f - asymmetry * 0.5f); // Gets harder
			negDenom = std::fmax(negDenom, 0.05f);                 // Don't go to zero

			float posCurve = mag / (posDenom + mag * (1.0f - posDenom));
			float negCurve = mag / (negDenom + mag * (1.0f - negDenom));

			// Clip positive softer, negative at full
			float posCeil = 1.0f - asymmetry * 0.25f;
			posCurve = std::fmin(posCurve, posCeil);
			negCurve = std::fmin(negCurve, 1.0f);

			output = (sign > 0) ? posCurve : -negCurve;
		}
		else if (yZone == 2) {
			// Zone 2: Hard clip with linear zone + shaped flat portion
			// X controls both saturation AND threshold (more clipping at high X)
			// zoneBlend controls: linear zone size, clip severity, and ripple

			// Threshold: Y lowers it from 0.8 to 0.4, X lowers it further by up to 0.2
			float linearThreshold = 0.8f - zoneBlend * 0.4f - xNorm * 0.2f;
			linearThreshold = std::fmax(linearThreshold, 0.15f); // Don't go below 15%

			// Ripple goes from subtle to quite obvious
			float rippleAmount = 0.02f + zoneBlend * 0.15f;
			float rippleFreq = 2.0f + zoneBlend * 4.0f; // More oscillations at high Y

			// Apply X-controlled saturation first
			float saturated = mag / (satDenom + mag * (1.0f - satDenom));

			if (saturated < linearThreshold) {
				output = saturated * sign;
			}
			else {
				float overdriveAmount = (saturated - linearThreshold) / (1.0f - linearThreshold);
				float ripple = std::sin(overdriveAmount * 6.28318f * rippleFreq) * rippleAmount;
				output = (linearThreshold + ripple) * sign;
				// Boost to maintain volume
				output *= (1.0f / std::fmax(linearThreshold, 0.15f));
			}
			output = std::fmax(-1.0f, std::fmin(1.0f, output));
		}
		else {
			// Zone 3: Crossover distortion (Class B style)
			// Reduced gain near zero creates buzzy character
			//
			// Dead zone width: 5% to 30% of full scale
			float deadZoneWidth = 0.05f + zoneBlend * 0.25f;
			// Minimum gain at zero: 30% down to 5%
			float minGain = 0.3f - zoneBlend * 0.25f;

			// Calculate compensation FIRST so we can apply it to the gain curve
			// This ensures loud signals benefit from compensation (not just quiet ones)
			float avgGainInZone = (minGain + 1.0f) * 0.5f;
			float powerLoss = deadZoneWidth * (1.0f - avgGainInZone * avgGainInZone);
			float compensation = 1.0f / std::fmax(0.4f, 1.0f - powerLoss * 2.0f);

			// Gain curve: starts at minGain at zero, reaches compensation above deadZoneWidth
			float gainMult;
			if (mag < deadZoneWidth) {
				float t = mag / deadZoneWidth;
				// Smoothstep: 3t² - 2t³ gives smooth transition
				float smooth = t * t * (3.0f - 2.0f * t);
				// Ramp from minGain*compensation to compensation
				gainMult = (minGain + (1.0f - minGain) * smooth) * compensation;
			}
			else {
				// Above dead zone: apply full compensation
				gainMult = compensation;
			}

			// Apply compensated gain, then saturation (which will soft-clip the boosted signal)
			float shaped = mag * gainMult;
			float saturated = shaped / (satDenom + shaped * (1.0f - satDenom));

			output = std::fmin(saturated, 1.0f) * sign;
		}

		return std::fmax(-1.0f, std::fmin(1.0f, output));
	}

	// 257 entries to allow interpolation at index 255
	std::array<int16_t, kTableSize + 1> table_{};
	uint8_t shapeX_{0};
	uint8_t shapeY_{0};

	// Zone 4: Single analytic saturator (shared tables) with separate L/R ADAA state
	AnalyticSaturator analyticSat_;
	mutable float analyticPrevXL_{0.0f}; // ADAA state for L channel (mutable for const process)
	mutable float analyticPrevXR_{0.0f}; // ADAA state for R channel
	bool useAnalytic_{false};
};

} // namespace deluge::dsp
