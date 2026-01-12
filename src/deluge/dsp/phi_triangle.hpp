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
constexpr float kPhiN100 = 0.6180340f; // φ^-1 = 1/φ
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

// Higher powers (for multiband compressor)
constexpr float kPhi250 = 3.3302077f; // φ^2.5
constexpr float kPhi275 = 3.7515562f; // φ^2.75
constexpr float kPhi300 = 4.2360680f; // φ^3.0
constexpr float kPhi325 = 4.7742114f; // φ^3.25
constexpr float kPhi350 = 5.3884156f; // φ^3.5
constexpr float kPhi360 = 5.7067284f; // φ^3.6
constexpr float kPhi375 = 6.0409418f; // φ^3.75
constexpr float kPhi385 = 6.4408314f; // φ^3.85
constexpr float kPhi400 = 6.8541020f; // φ^4.0

/**
 * Wrap phase to [0,1) with double precision
 *
 * Use double precision for the computation to handle large secret knob values
 * (gamma can reach 10^15 before precision issues). Result is always [0,1) so
 * float output is sufficient.
 *
 * Uses int64_t truncation instead of std::floor for ~40 cycle savings per call.
 * Safe for positive values up to ~9×10^18 (int64_t max).
 *
 * @param phase Raw phase value (may be very large from secret knobs)
 * @return Wrapped phase in [0,1)
 */
[[gnu::always_inline]] inline float wrapPhase(double phase) {
	// Fast floor via int64_t truncation (valid for positive values)
	return static_cast<float>(phase - static_cast<double>(static_cast<int64_t>(phase)));
}

} // namespace deluge::dsp::phi
