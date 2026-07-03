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
// PHI_SWARM — injection-locking / synchronization oscillator
//
// A master phase (the note, pitch-exact) drives two slave oscillators through
// per-sample Adler coupling:
//
//   s_i += w_i + K_im*sin(theta_m - s_i) [+ K_12*sin(s_1 - s_2)] + T*noise
//
// Inside an Arnold tongue the slaves LOCK to exact rational ratios of the note
// (self-tuning consonance - even when detuned); near a tongue edge they
// exhibit injection pulling (the beat note slows, hesitates and snaps -
// asymmetric sideband "siren" spectra); far outside they beat quasiperiodically
// and never repeat. T is literally temperature: Langevin phase noise that
// melts the network from crystalline lock through analog drift into colored
// noise. The A/B crossfade's MOTION injects heat - sweep the wave index and
// the swarm de-syncs; park it and you hear it anneal, slaves finding their
// locks one by one as the system cools into zone B's coupling landscape.
//
// Couplings and temperature scale with the master increment, so locking
// behavior is pitch-invariant. All zone parameters interpolate across eight
// anchors (Still..Chaos) with phi-triangle wander. Per-voice state is two
// slave phases, packed into VoiceUnisonPartSource::prevPhaseScaler (a uint64
// used only by TRIANGLE_PW, free for this oscillator type).
//
// Literature:
//   Adler, R., "A Study of Locking Phenomena in Oscillators", Proc. IRE
//     34(6), 1946 (reprinted Proc. IEEE 61(10), 1973).
//   Kuramoto, Y., "Self-entrainment of a population of coupled non-linear
//     oscillators", Lecture Notes in Physics 39, Springer, 1975.
//   Strogatz, S., "From Kuramoto to Crawford: exploring the onset of
//     synchronization in populations of coupled oscillators", Physica D 143,
//     2000.
//   Bak, P., "The Devil's Staircase", Physics Today 39(12), 1986 (Arnold
//     tongues / mode-locking structure).
//   Essl, G., "Circle Maps as Simple Oscillators for Complex Behavior",
//     Proc. ICMC 2006 (circle maps proposed as audio oscillators).
// Deep dive with figures: docs/dev/phi-swarm-sync.md
// ============================================================================

// ============================================================================
// Phi Triangle Bank Configurations (wander around the zone anchors)
// ============================================================================

// Slave frequency-ratio wander (multiplicative, up to ~+/-6% - enough to cross
// tongue boundaries without leaving the anchor's musical neighborhood)
inline constexpr phi::PhiTriConfig kPhiSwarmRatio1Wander = {phi::kPhi125, 0.7f, 0.000f, true};
inline constexpr phi::PhiTriConfig kPhiSwarmRatio2Wander = {phi::kPhi300, 0.6f, 0.150f, true};

// Coupling strength wander (multiplicative on the anchors)
inline constexpr phi::PhiTriConfig kPhiSwarmCoupleWander = {phi::kPhiN050, 0.8f, 0.320f, true};
inline constexpr phi::PhiTriConfig kPhiSwarmCrossWander = {phi::kPhi225, 0.5f, 0.470f, true};

// Temperature wander and output weight wander
inline constexpr phi::PhiTriConfig kPhiSwarmTempWander = {phi::kPhi175, 0.6f, 0.610f, false};
inline constexpr phi::PhiTriConfig kPhiSwarmBalanceWander = {phi::kPhi075, 0.9f, 0.740f, true};
inline constexpr phi::PhiTriConfig kPhiSwarmRingWander = {phi::kPhi350, 0.5f, 0.860f, false};

// Anneal gain: how strongly crossfade MOTION heats the network (from zone B)
inline constexpr phi::PhiTriConfig kPhiSwarmAnneal = {phi::kPhi250, 0.7f, 0.090f, false};

// Beat-AM depth wander: how audibly the slave-1 phase drift breathes the level
inline constexpr phi::PhiTriConfig kPhiSwarmBeatWander = {phi::kPhi150, 0.6f, 0.520f, false};

// Output skew: each slave's sine is phase-distorted through a two-slope map
// (Casio CZ style) before the table lookup. Center = pure sine (previous
// character, bit-exact); edges = bright saw-leaning curves. Zone-varied per
// slave; the COUPLING phases stay undistorted so locking physics is untouched.
inline constexpr phi::PhiTriConfig kPhiSwarmSkew1 = {phi::kPhi325, 0.7f, 0.050f, true};
inline constexpr phi::PhiTriConfig kPhiSwarmSkew2 = {phi::kPhiN025, 0.6f, 0.430f, true};

// ============================================================================
// Types
// ============================================================================

struct PhiSwarmParams {
	uint32_t ratio1FP; // Slave 1 / master frequency ratio, Q16.16
	uint32_t ratio2FP; // Slave 2 / master frequency ratio, Q16.16
	uint32_t k1mFP;    // Coupling strengths as fractions of the master increment, Q16.16
	uint32_t k2mFP;
	uint32_t k12FP;  // Slave 1 -> slave 2 chase coupling
	uint32_t tempFP; // Temperature (phase noise) as fraction of master increment, Q16.16
	q31_t w1;        // Output weights
	q31_t w2;
	q31_t wRing;    // sin(s1)*sin(s2) cross term (sum/difference partials)
	q31_t wBeat;    // Beat-AM: slave-1 phase drift breathes the output level
	uint32_t skew1; // Two-slope phase-distortion split point per slave (0x80000000 = pure sine)
	uint32_t skew2;
	float annealGain;
};

struct PhiSwarmCache {
	PhiSwarmParams bankA{};
	PhiSwarmParams bankB{};
	PhiSwarmParams eff{}; // Crossfade-interpolated (rebuilt when smoothed crossfade moves)

	q31_t prevCrossfade{INT32_MIN};
	q31_t smoothedCrossfade{INT32_MIN};

	// Previous buffer's slave increments: ramped to current across each
	// buffer, because stepping them per buffer under wave-index modulation
	// (env/LFO/unison on the morph) is 344 Hz FM buzz
	// Morph-ramp snapshots, updated ONCE PER BUFFER (in the env block): the
	// ramps must be over SHARED quantities (ratios/weights), because per-call
	// increment snapshots seesaw between detuned unison voices. Each voice
	// derives its own increment ramp from its own pitch and these ratios.
	uint32_t ratio1From{0};
	uint32_t ratio2From{0};
	uint32_t ratio1Last{0};
	uint32_t ratio2Last{0};
	q31_t w1From{0};
	q31_t w2From{0};
	q31_t wRingFrom{0};
	q31_t wBeatFrom{0};
	q31_t w1Last{INT32_MIN};
	q31_t w2Last{0};
	q31_t wRingLast{0};
	q31_t wBeatLast{0};
	float annealEnv{0.0f}; // Heat injected by crossfade motion, decays per buffer
	float prevCf{-1.0f};
	uint32_t lastEnvTime{0xFFFFFFFF};
	uint32_t effTempFP{0}; // eff.tempFP + anneal heat, updated per buffer
	uint32_t noiseState{0x6A09E667u};

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

PhiSwarmParams buildPhiSwarmParams(uint16_t zone, float phaseOffset = 0.0f);

/// Render PHI_SWARM for one buffer. slavePhases packs the two per-voice slave
/// phase accumulators (lo 32 = slave 1, hi 32 = slave 2).
void renderPhiSwarm(PhiSwarmCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                    uint32_t phaseIncrement, uint32_t* startPhase, uint64_t* slavePhases, uint32_t retriggerPhase,
                    int32_t amplitude, int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade,
                    uint32_t pulseWidth);

} // namespace deluge::dsp
