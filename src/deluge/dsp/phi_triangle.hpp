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

#include "dsp/util.hpp"
#include <cmath>

namespace deluge::dsp::phi {

/**
 * φ-power constants for irrational frequency ratios
 *
 * Using powers of the golden ratio (φ ≈ 1.618) creates frequencies that
 * never align, producing quasi-periodic patterns with no exact repetition.
 * This is the mathematical foundation for zone-based parameter evolution.
 */
constexpr float kPhi = 1.6180340f; // Golden ratio (φ)

// Negative powers (slower than base)
constexpr float kPhiN050 = 0.7861513f; // φ^-0.5
constexpr float kPhiN025 = 0.8872984f; // φ^-0.25

// Positive powers (faster than base)
constexpr float kPhi025 = 1.1271566f; // φ^0.25
constexpr float kPhi033 = 1.1746627f; // φ^0.33
constexpr float kPhi050 = 1.2720196f; // φ^0.5
constexpr float kPhi067 = 1.3871872f; // φ^0.67
constexpr float kPhi075 = 1.4352958f; // φ^0.75
constexpr float kPhi100 = 1.6180340f; // φ^1.0
constexpr float kPhi125 = 1.8257419f; // φ^1.25
constexpr float kPhi150 = 2.0581710f; // φ^1.5
constexpr float kPhi175 = 2.3197171f; // φ^1.75
constexpr float kPhi200 = 2.6180340f; // φ^2.0
constexpr float kPhi225 = 2.9603110f; // φ^2.25

/**
 * Wrap phase*freq to [0,1) using double precision
 *
 * Double precision maintains accuracy even when phase (from secret knobs)
 * reaches very large values (gamma can be 10^15 before issues).
 *
 * @param phase Raw phase value (may be large, from secret knobs)
 * @param freq φ-power frequency multiplier
 * @return Wrapped phase in [0,1)
 */
[[gnu::always_inline]] inline float wrapPhase(double phase, float freq) {
	double scaled = phase * static_cast<double>(freq);
	return static_cast<float>(scaled - std::floor(scaled));
}

/**
 * Frequency modulation factor for triangle acceleration
 *
 * As position increases, triangles can speed up. The speedup amount
 * varies with the wrapped phase, creating complex beating patterns.
 *
 * Pattern: fm = 1.0 + pos * (base + vary * phXXX)
 * - At pos=0: fm=1.0 (base rate)
 * - At pos=1: fm=1.0+(base+vary*ph) (25-50% faster typically)
 *
 * @param pos Position in zone (0-1)
 * @param wrappedPh Wrapped phase for this frequency
 * @param base Base speedup at pos=1 (default 0.25 = 25%)
 * @param vary Additional speedup variation (default 0.25 = +25%)
 * @return Frequency multiplier (1.0 to ~1.5)
 */
[[gnu::always_inline]] inline float freqMod(float pos, float wrappedPh, float base = 0.25f, float vary = 0.25f) {
	return 1.0f + pos * (base + vary * wrappedPh);
}

/**
 * Secret knob phases - three unbounded phase offsets
 *
 * Accessed via push+twist on encoders. These shift the entire
 * φ-triangle constellation without changing the zone selection.
 *
 * - metaPhase: Primary offset (e.g., push Twist encoder)
 * - metaPhaseTopo: Topology offset (e.g., push Topo encoder)
 * - gammaPhase: Coarse offset, multiplied by 100 (e.g., push third encoder)
 */
struct SecretPhases {
	float metaPhase{0};
	float metaPhaseTopo{0};
	float gammaPhase{0};

	/// Effective phase for meta zones: metaPhase + 100*gammaPhase
	[[nodiscard]] double effectiveMeta() const {
		return static_cast<double>(metaPhase) + 100.0 * static_cast<double>(gammaPhase);
	}

	/// Effective phase for topology: just metaPhaseTopo
	[[nodiscard]] double effectiveTopo() const { return static_cast<double>(metaPhaseTopo); }
};

/**
 * Pre-wrapped phases for all standard φ-power frequencies
 *
 * Compute once per zone update, then reuse for all triangle evaluations.
 * Uses double precision internally to handle large secret knob values.
 */
struct WrappedPhases {
	// Negative powers
	float phN050{0}, phN025{0};
	// Positive powers
	float ph025{0}, ph033{0}, ph050{0}, ph067{0}, ph075{0}, ph100{0};
	float ph125{0}, ph150{0}, ph175{0}, ph200{0}, ph225{0};

	/// Construct from raw phase value (uses double precision)
	static WrappedPhases fromRaw(double rawPhase) {
		WrappedPhases wp;
		wp.phN050 = wrapPhase(rawPhase, kPhiN050);
		wp.phN025 = wrapPhase(rawPhase, kPhiN025);
		wp.ph025 = wrapPhase(rawPhase, kPhi025);
		wp.ph033 = wrapPhase(rawPhase, kPhi033);
		wp.ph050 = wrapPhase(rawPhase, kPhi050);
		wp.ph067 = wrapPhase(rawPhase, kPhi067);
		wp.ph075 = wrapPhase(rawPhase, kPhi075);
		wp.ph100 = wrapPhase(rawPhase, kPhi100);
		wp.ph125 = wrapPhase(rawPhase, kPhi125);
		wp.ph150 = wrapPhase(rawPhase, kPhi150);
		wp.ph175 = wrapPhase(rawPhase, kPhi175);
		wp.ph200 = wrapPhase(rawPhase, kPhi200);
		wp.ph225 = wrapPhase(rawPhase, kPhi225);
		return wp;
	}
};

/**
 * Compute a φ-modulated triangle parameter
 *
 * This is the core pattern used throughout zone-based effects:
 *   triangleFunc(pos * phiFreq * freqMod + wrappedPhase + offset, duty)
 *
 * @param pos Position in zone (0-1)
 * @param phiFreq φ-power frequency (e.g., kPhi050)
 * @param fm Frequency modulation factor (from freqMod())
 * @param wrappedPh Wrapped phase for this frequency
 * @param offset Fixed phase offset for this parameter
 * @param duty Triangle duty cycle (default 0.8)
 * @return Unipolar triangle value (0-1)
 */
[[gnu::always_inline]] inline float phiTriangleUnipolar(float pos, float phiFreq, float fm, float wrappedPh,
                                                        float offset, float duty = 0.8f) {
	return triangleSimpleUnipolar(pos * phiFreq * fm + wrappedPh + offset, duty);
}

/**
 * Compute a bipolar φ-modulated triangle parameter
 *
 * Same as phiTriangleUnipolar but returns -1 to +1 range.
 * Useful for sign-based mode selection (positive = mode A, negative = mode B).
 *
 * @param pos Position in zone (0-1)
 * @param phiFreq φ-power frequency (e.g., kPhi075)
 * @param fm Frequency modulation factor
 * @param wrappedPh Wrapped phase for this frequency
 * @param offset Fixed phase offset
 * @param duty Triangle duty cycle (default 0.5)
 * @return Bipolar triangle value (-1 to +1)
 */
[[gnu::always_inline]] inline float phiTriangleBipolar(float pos, float phiFreq, float fm, float wrappedPh,
                                                       float offset, float duty = 0.5f) {
	return triangleFloat(pos * phiFreq * fm + wrappedPh + offset, duty);
}

/**
 * Helper to compute common "scale * param" pattern with clipping
 *
 * Many zone params use: scale = min(triangle*2, 1) * param_triangle
 * This creates smooth ramps with saturation.
 *
 * @param scale Scale triangle value (0-1, will be doubled and clipped)
 * @param param Parameter triangle value (0-1)
 * @return Combined value (0-1)
 */
[[gnu::always_inline]] inline float scaleParam(float scale, float param) {
	return std::min(scale * 2.0f, 1.0f) * param;
}

/**
 * Bipolar sign-based mode selection
 *
 * For triangles that select between two modes based on sign:
 * - Positive triangle → mode A amount
 * - Negative triangle → mode B amount
 *
 * @param tri Bipolar triangle value (-1 to +1)
 * @param scale Scale factor (0-1)
 * @param[out] modeA Amount for positive mode
 * @param[out] modeB Amount for negative mode
 */
[[gnu::always_inline]] inline void bipolarSelect(float tri, float scale, float& modeA, float& modeB) {
	float absTri = std::abs(tri);
	modeA = scale * ((tri > 0.0f) ? absTri : 0.0f);
	modeB = scale * ((tri < 0.0f) ? absTri : 0.0f);
}

} // namespace deluge::dsp::phi
