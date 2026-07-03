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
// PHI_WEAVE — scanned-physics oscillator
//
// A ring of 32 masses on springs, simulated at CONTROL rate (once per render
// buffer) and scanned at AUDIO rate as a wavetable (Mathews/Verplank scanned
// synthesis, on a closed loop so the scanned cycle is seam-free by topology).
// Pitch = scan speed; timbre = the physics. The zone position doesn't select a
// waveform — it selects the LAWS: where the ring is stiff, where it's damp,
// what shape it relaxes toward, and how it's continuously bowed. The A/B
// crossfade interpolates the physics under one shared, ever-continuous state,
// and the crossfade's own motion bows the string (the morph is an excitation).
//
// Literature:
//   Verplank, B., Mathews, M., Shaw, R., "Scanned Synthesis", Proc. ICMC 2000.
//   Boulanger, R., Smaragdis, P., ffitch, J., "Scanned Synthesis: An
//     Introduction and Demonstration...", Proc. ICMC 2000.
//   Verlet, L., "Computer 'Experiments' on Classical Fluids. I.",
//     Phys. Rev. 159(1), 1967 (the integration scheme).
// Deep dive with figures: docs/dev/phi-weave-physics.md
// ============================================================================

inline constexpr int32_t kPhiWeaveNumNodes = 32;              // Power of two: scan index = phase >> 27
inline constexpr int32_t kPhiWeaveNodeShift = 27;             // 32 - 5
inline constexpr float kPhiWeaveRefAmplitude = 1073741823.0f; // ~0.5 x Q31, matches PHI_MORPH

// Physics safety rails. Leapfrog with dt=1 tick is stable while
// (stiffness + 4*coupling) < 4; ranges below keep it under ~1.4.
inline constexpr float kPhiWeaveMaxVelocity = 0.5f;
inline constexpr float kPhiWeaveMaxDisplacement = 2.0f;

// ============================================================================
// Phi Triangle Bank Configurations (per zone)
// ============================================================================

// --- Global physics scalars ---

// Base stiffness (unipolar, exponentially mapped 0.0008..0.06):
// how strongly nodes are pulled toward the home shape (slack rubber -> taut wire)
inline constexpr phi::PhiTriConfig kPhiWeaveStiffBase = {phi::kPhi125, 0.6f, 0.000f, false};

// Base neighbor coupling (unipolar, 0.01..0.29): wave propagation speed around
// the ring — low = slow smearing ripples, high = fast glassy travel
inline constexpr phi::PhiTriConfig kPhiWeaveCoupleBase = {phi::kPhiN050, 0.7f, 0.210f, false};

// Base damping (unipolar, 0.0015..0.045): how quickly motion dies back to the
// home shape — breathing organ vs quickly-settling pluck body
inline constexpr phi::PhiTriConfig kPhiWeaveDampBase = {phi::kPhi250, 0.5f, 0.420f, false};

// --- Spatial landscapes (evaluated per node with zone-dependent phase) ---
// Two triangles each; spatialCycles below sets how many bumps fit around the ring.

inline constexpr phi::PhiTriConfig kPhiWeaveStiffLand1 = {phi::kPhi075, 0.8f, 0.060f, true};
inline constexpr phi::PhiTriConfig kPhiWeaveStiffLand2 = {phi::kPhi300, 0.5f, 0.530f, true};
inline constexpr phi::PhiTriConfig kPhiWeaveDampLand1 = {phi::kPhi175, 0.7f, 0.140f, true};
inline constexpr phi::PhiTriConfig kPhiWeaveDampLand2 = {phi::kPhiN025, 0.6f, 0.660f, true};
inline constexpr phi::PhiTriConfig kPhiWeaveCoupleLand = {phi::kPhi225, 0.6f, 0.310f, true};

// --- Home shape: partial amounts and phases for the string's rest pose ---

inline constexpr phi::PhiTriConfig kPhiWeaveHomeP2 = {phi::kPhi150, 0.6f, 0.480f, true};  // 2nd partial
inline constexpr phi::PhiTriConfig kPhiWeaveHomeP3 = {phi::kPhi325, 0.5f, 0.760f, true};  // 3rd partial
inline constexpr phi::PhiTriConfig kPhiWeaveHomeP5 = {phi::kPhi050, 0.35f, 0.900f, true}; // 5th partial
inline constexpr phi::PhiTriConfig kPhiWeaveHomePh2 = {phi::kPhi025, 1.0f, 0.320f, false};
inline constexpr phi::PhiTriConfig kPhiWeaveHomePh3 = {phi::kPhi275, 1.0f, 0.610f, false};

// --- Bow: continuous micro-excitation keeping the ring alive ---

inline constexpr phi::PhiTriConfig kPhiWeaveBowDepth = {phi::kPhi350, 0.55f, 0.190f, false};
inline constexpr phi::PhiTriConfig kPhiWeaveBowRate = {phi::kPhi100, 0.8f, 0.370f, false};
inline constexpr phi::PhiTriConfig kPhiWeaveBowPos = {phi::kPhi200, 1.0f, 0.740f, false};
inline constexpr phi::PhiTriConfig kPhiWeaveBowSpread = {phi::kPhiN100, 0.7f, 0.880f, false};

// --- Travel: slow rotation of the ring under the scan head (chorus-like drift) ---

inline constexpr phi::PhiTriConfig kPhiWeaveTravel = {phi::kPhi067, 0.5f, 0.250f, true};

// --- Pluck: note-on displacement injection ---

inline constexpr phi::PhiTriConfig kPhiWeavePluckPos = {phi::kPhi375, 1.0f, 0.090f, false};
inline constexpr phi::PhiTriConfig kPhiWeavePluckWidth = {phi::kPhi033, 0.7f, 0.440f, false};
inline constexpr phi::PhiTriConfig kPhiWeavePluckAmp = {phi::kPhi250, 0.6f, 0.820f, false};

// --- Morph bow: how strongly crossfade MOTION excites the ring (from zone B) ---

inline constexpr phi::PhiTriConfig kPhiWeaveMorphBow = {phi::kPhi175, 0.7f, 0.150f, false};

// --- Output gain trim ---

inline constexpr phi::PhiTriConfig kPhiWeaveOutGain = {phi::kPhiN025, 0.9f, 0.560f, false};

// ============================================================================
// Types
// ============================================================================

struct PhiWeaveParams {
	float stiffness[kPhiWeaveNumNodes];
	float coupling[kPhiWeaveNumNodes];
	float damping[kPhiWeaveNumNodes];
	float home[kPhiWeaveNumNodes];
	float bowDepth;
	float bowRate;
	float bowPos;
	float bowSpread;
	float travelRate;
	float pluckPos;
	float pluckWidth;
	float pluckAmp;
	float morphBowGain;
	float outGain;
};

struct PhiWeaveCache {
	PhiWeaveParams bankA{};
	PhiWeaveParams bankB{};

	// Physics state — ONE string shared by all voices/unison of the Sound.
	// Morphing interpolates the laws, never the state, so it cannot click.
	float x[kPhiWeaveNumNodes]{};
	float v[kPhiWeaveNumNodes]{};

	// Scan table rebuilt each tick ([32] duplicates [0] for wrap-free lerp)
	// Scan tables, padded for the Catmull-Rom scan: [0] = node 31 (prelude),
	// [1..32] = nodes 0..31, [33] = node 0, [34] = node 1. Scan index i reads
	// taps [i..i+3].
	q31_t nodeQ[kPhiWeaveNumNodes + 3]{};
	// Pitch-adaptive anti-aliasing: spatially smoothed copies of the scan
	// table, selected per buffer by phase increment (high notes read the
	// smoothed string so spatial zigzag modes don't fold past Nyquist)
	q31_t nodeQMip1[kPhiWeaveNumNodes + 3]{};
	q31_t nodeQMip2[kPhiWeaveNumNodes + 3]{};
	// Previous tick's tables: the render crossfades prev -> current across
	// each buffer so the string moves continuously instead of stepping once
	// per tick (the per-buffer step sprayed ~344Hz sidebands around every
	// partial - it read as aliasing at all pitches)
	q31_t nodeQPrev[kPhiWeaveNumNodes + 3]{};
	q31_t nodeQMip1Prev[kPhiWeaveNumNodes + 3]{};
	q31_t nodeQMip2Prev[kPhiWeaveNumNodes + 3]{};
	bool tablesValid{false};

	// IIR-smoothed crossfade, advanced once per buffer by the caller (same
	// convention as PhiMorphCache)
	q31_t smoothedCrossfade{INT32_MIN};

	uint32_t lastTickTime{0xFFFFFFFF}; // AudioEngine::audioSampleTimer at last physics tick
	uint32_t travelOffset{0};          // Scan phase offset from ring rotation
	float travelPhase{0.0f};
	float bowPhase{0.0f};
	float prevTickCf{-1.0f}; // For morph-bow (crossfade velocity)
	float agcPeak{1.0f};     // Slow output normalization tracker
	uint32_t noiseState{0x2545F491u};
	bool pluckPending{true};

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

PhiWeaveParams buildPhiWeaveParams(uint16_t zone, float phaseOffset = 0.0f);

/// Render PHI_WEAVE for one buffer. Advances the physics once per buffer
/// (guarded by AudioEngine::audioSampleTimer), then scans the ring per sample.
void renderPhiWeave(PhiWeaveCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                    uint32_t phaseIncrement, uint32_t* startPhase, uint32_t retriggerPhase, int32_t amplitude,
                    int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade, uint32_t pulseWidth);

} // namespace deluge::dsp
