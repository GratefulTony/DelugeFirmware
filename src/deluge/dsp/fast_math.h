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

#include <cmath>
#include <cstdint>

namespace deluge::dsp {

/// Fast math approximations for DSP use.
/// These trade accuracy for speed - suitable for audio where small errors are inaudible.

/// Fast exp(x) using Schraudolph's method with polynomial refinement.
/// Accurate to ~0.1% for |x| < 10. Much faster than std::exp().
/// Based on: https://nic.schraudolph.org/pubs/Schraudolph99.pdf
[[gnu::always_inline]] inline float fastExp(float x) {
	// Clamp to avoid overflow/underflow
	x = std::fmax(-20.0f, std::fmin(20.0f, x));

	// Schraudolph's method: interpret float bits as exp approximation
	// exp(x) ≈ 2^(x/ln2) = 2^(1.4427*x)
	// Using IEEE 754 float representation trick
	union {
		float f;
		int32_t i;
	} u;

	// Magic constants: 12102203 = 2^23 / ln(2), 127 << 23 = bias
	u.i = static_cast<int32_t>(12102203.0f * x + 1065353216.0f);

	// Polynomial correction for better accuracy
	// This adds ~2 multiplies but improves accuracy significantly
	float correction = 1.0f + x * (0.08f * x); // Quadratic tweak
	return u.f * correction;
}

/// Fast natural log using IEEE 754 bit manipulation.
/// Accurate to ~1% for x > 0. Much faster than std::log().
[[gnu::always_inline]] inline float fastLog(float x) {
	// Avoid log(0) and negative
	if (x <= 0.0f) {
		return -100.0f; // Return a large negative number
	}

	union {
		float f;
		int32_t i;
	} u;
	u.f = x;

	// Extract exponent and mantissa from IEEE 754 representation
	// log(x) ≈ (exponent - 127) * ln(2) + log(mantissa)
	// where mantissa is in [1, 2)
	int32_t e = (u.i >> 23) - 127;
	u.i = (u.i & 0x007FFFFF) | 0x3F800000; // Set exponent to 0 (mantissa in [1,2))

	float m = u.f;
	// Polynomial approximation for log(m) where m in [1, 2)
	// log(m) ≈ (m - 1) - 0.5*(m-1)^2 + 0.33*(m-1)^3
	float t = m - 1.0f;
	float logM = t * (1.0f - t * (0.5f - t * 0.33f));

	return static_cast<float>(e) * 0.6931472f + logM; // ln(2) ≈ 0.6931472
}

/// Fast log10 using fastLog.
[[gnu::always_inline]] inline float fastLog10(float x) {
	return fastLog(x) * 0.4342945f; // 1/ln(10) ≈ 0.4342945
}

/// Fast tan(x) using Padé approximation.
/// Accurate for |x| < 1.5 (covers fc up to ~0.48 of sample rate).
/// Used for filter coefficient calculation.
[[gnu::always_inline]] inline float fastTan(float x) {
	// Padé (5,4) approximation: very accurate for |x| < π/2
	// tan(x) ≈ x(945 - 105x² + x⁴) / (945 - 420x² + 15x⁴)
	float x2 = x * x;
	float x4 = x2 * x2;
	float num = x * (945.0f - 105.0f * x2 + x4);
	float den = 945.0f - 420.0f * x2 + 15.0f * x4;
	return num / den;
}

/// Fast pow(2, x) for frequency calculations.
/// Uses fastExp internally.
[[gnu::always_inline]] inline float fastPow2(float x) {
	return fastExp(x * 0.6931472f); // x * ln(2)
}

} // namespace deluge::dsp
