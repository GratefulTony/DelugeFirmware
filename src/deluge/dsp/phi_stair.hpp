/*
 * Copyright © 2026 Owlet Records
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

#include "dsp/phi_triangle.hpp"
#include "util/fixedpoint.h"
#include <cstdint>

namespace deluge::dsp {

// ============================================================================
// PHI_STAIR — staircase wavetable oscillator
//
// The family's digital primitive: the waveform is a STAIRCASE of up to 16
// steps with zone-derived HEIGHTS (pattern families: brick alternation,
// terraced ramps, mesas, phi-random glyph levels, teeth...), WIDTHS
// (asymmetry landscapes), and a SLOPE dimension sweeping the risers from
// hard edges (pulse-class -6 dB/oct brightness) to fully ramped polygons.
// Step count is CONTINUOUS: the last step's width fades in fractionally, so
// zone sweeps and morphs never pop a step in or out.
//
// Architecture reuses the family's proven parts: MORPH's zone/wave/gamma
// interface, a 128-slot scan table rebuilt only when the morph moves
// (epsilon-guarded; a one-buffer smoothstep crossfade de-zippers rebuilds),
// WEAVE's pitch-adaptive anti-alias mips and dual-tap stereo zones, and
// halved-difference lerps throughout (steps sign-flip at full scale by
// design - the raw q31 difference would wrap).
// ============================================================================

inline constexpr int32_t kPhiStairSteps = 16;
inline constexpr int32_t kPhiStairSlots = 128;
inline constexpr int32_t kPhiStairSlotShift = 25; // 32 - log2(128)

// ============================================================================
// Phi Triangle Bank Configurations (per zone)
// ============================================================================

// Step count (continuous 2..16) and riser slope (0 = hard edge .. ~0.9 of a
// step width = almost triangle)
inline constexpr phi::PhiTriConfig kPhiStairCount = {phi::kPhi125, 0.7f, 0.000f, false};
inline constexpr phi::PhiTriConfig kPhiStairSlope = {phi::kPhi250, 0.6f, 0.140f, false};

// Height wander (per-step spatial landscape over the pattern) and tilt
// (monotonic lean added to any pattern - saw-ness)
inline constexpr phi::PhiTriConfig kPhiStairHeightLand = {phi::kPhi275, 0.6f, 0.290f, true};
inline constexpr phi::PhiTriConfig kPhiStairTilt = {phi::kPhiN050, 0.7f, 0.410f, true};

// Width asymmetry: spatial landscape over the steps (0 = even, 1 = extreme)
inline constexpr phi::PhiTriConfig kPhiStairAsym = {phi::kPhi175, 0.6f, 0.550f, false};
inline constexpr phi::PhiTriConfig kPhiStairAsymLand = {phi::kPhi325, 0.5f, 0.660f, true};

// Spatial cycle counts for the landscapes
inline constexpr phi::PhiTriConfig kPhiStairLandCycles = {phi::kPhi050, 1.0f, 0.780f, false};

// ============================================================================
// Types
// ============================================================================

struct PhiStairParams {
	float heights[kPhiStairSteps]; // Normalized to +/-1 peak
	float widths[kPhiStairSteps];  // Relative; includes the fractional-count fade; renormalized at table build
	float slope;                   // Riser length as a fraction of each step's width
};

struct PhiStairCache {
	PhiStairParams bankA{};
	PhiStairParams bankB{};

	// Scan tables (padded +1 wrap slot), with WEAVE-style anti-alias mips and
	// previous copies for the one-buffer rebuild crossfade
	q31_t nodeQ[kPhiStairSlots + 1]{};
	q31_t nodeQMip1[kPhiStairSlots + 1]{};
	q31_t nodeQMip2[kPhiStairSlots + 1]{};
	q31_t nodeQPrev[kPhiStairSlots + 1]{};
	q31_t nodeQMip1Prev[kPhiStairSlots + 1]{};
	q31_t nodeQMip2Prev[kPhiStairSlots + 1]{};
	bool tablesValid{false};
	uint32_t fadeUntilTime{0}; // Buffer timestamp that should crossfade prev -> current

	q31_t smoothedCrossfade{INT32_MIN};
	float effCfCached{-2.0f};
	uint32_t lastTickTime{0xFFFFFFFF};

	uint16_t prevZoneA{0xFFFF};
	uint16_t prevZoneB{0xFFFF};
	float prevPhaseOffsetA{-1.0f};
	float prevPhaseOffsetB{-1.0f};

	[[nodiscard]] bool needsUpdate(uint16_t zoneA, uint16_t zoneB, float phaseOffsetA, float phaseOffsetB) const {
		return zoneA != prevZoneA || zoneB != prevZoneB || phaseOffsetA != prevPhaseOffsetA
		       || phaseOffsetB != prevPhaseOffsetB;
	}
};

// ============================================================================
// Function declarations
// ============================================================================

PhiStairParams buildPhiStairParams(uint16_t zone, float phaseOffset = 0.0f);

/// bufferRStart (nullable): stereo-zone dual tap (WEAVE-pattern characters:
/// tap distance, tonal tilt via a darker mip, counter-scan mirroring)
void renderPhiStair(PhiStairCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                    uint32_t phaseIncrement, uint32_t* startPhase, uint32_t retriggerPhase, int32_t amplitude,
                    int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade, uint32_t pulseWidth,
                    int32_t* bufferRStart = nullptr, uint16_t stereoZone = 0);

} // namespace deluge::dsp
