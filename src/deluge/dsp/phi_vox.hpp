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
// PHI_VOX — VOSIM oscillator
//
// VOSIM ("VOice SIMulation", Kaegi & Tempelaars, Institute of Sonology,
// Utrecht): each fundamental cycle emits a burst of N raised-cosine (sin²)
// pulses at a formant rate, successive pulses scaled by a decay factor, then
// silence until the cycle wraps. The pulse rate sets the formant's center
// frequency; the pulse count and decay set its bandwidth. Two such branches
// make a vowel. Formant frequencies are fixed in Hz, so vocal identity
// survives transposition like a real vocal tract.
//
// Zone positions anchor near real vowel formant pairs and wander far outside
// them via phi-triangle banks (decay > 1 = growing "brass rasp" bursts,
// alternating pulse polarity = hollow odd-harmonic variants, F2 into alien
// territory). The A/B crossfade glides formants (a diphthong), and crossfade
// MOTION injects a noise burst into the pulse envelope: an articulated
// consonant — sweeping the wave index makes it speak.
//
// The output is a pure function of cycle phase (pulse index and formant phase
// derive from it), so there is NO per-voice state and no buffers: the whole
// oscillator is two small parameter banks per Sound.
//
// Literature:
//   Kaegi, W., Tempelaars, S., "VOSIM—A New Sound Synthesis System",
//     Journal of the Audio Engineering Society 26(6), pp. 418-425, 1978.
//   Rodet, X., "Time-Domain Formant-Wave-Function Synthesis", Computer
//     Music Journal 8(3), 1984 (the related FOF/CHANT lineage).
// Deep dive with figures: docs/dev/phi-vox-vosim.md
// ============================================================================

inline constexpr int32_t kPhiVoxNumFormants = 2;
inline constexpr int32_t kPhiVoxMaxPulses = 16; // Gain tables zero-padded to here

// ============================================================================
// Phi Triangle Bank Configurations (per zone)
// ============================================================================

// Formant frequency wander: bipolar octave offsets around the zone's vowel
// anchor (F1 ±0.6 oct, F2 ±0.8 oct — wide, per "unique over believable")
inline constexpr phi::PhiTriConfig kPhiVoxF1Wander = {phi::kPhi125, 0.7f, 0.000f, true};
inline constexpr phi::PhiTriConfig kPhiVoxF2Wander = {phi::kPhi275, 0.6f, 0.140f, true};

// Pulses per burst (1..12) and decay per formant.
// Decay maps 0.30..1.18 — values > 1 GROW across the burst (edgy rasp).
inline constexpr phi::PhiTriConfig kPhiVoxN1 = {phi::kPhi050, 0.8f, 0.280f, false};
inline constexpr phi::PhiTriConfig kPhiVoxN2 = {phi::kPhi350, 0.6f, 0.410f, false};
inline constexpr phi::PhiTriConfig kPhiVoxDecay1 = {phi::kPhiN050, 0.7f, 0.550f, false};
inline constexpr phi::PhiTriConfig kPhiVoxDecay2 = {phi::kPhi200, 0.5f, 0.660f, false};

// Formant balance (F2 level relative to F1) and pulse polarity alternation
// (0 = all-positive classic VOSIM, 1 = alternating-sign hollow variant)
inline constexpr phi::PhiTriConfig kPhiVoxBalance = {phi::kPhi075, 0.9f, 0.780f, false};
inline constexpr phi::PhiTriConfig kPhiVoxPolarity1 = {phi::kPhi325, 0.45f, 0.870f, false};
inline constexpr phi::PhiTriConfig kPhiVoxPolarity2 = {phi::kPhiN025, 0.5f, 0.930f, false};

// Breath: noise mixed into the pulse envelope (voiced-gated aspiration)
inline constexpr phi::PhiTriConfig kPhiVoxBreath = {phi::kPhi175, 0.5f, 0.060f, false};

// Articulation: how strongly crossfade MOTION injects a consonant burst
// (evaluated on zone B — the morph target shapes the transition)
inline constexpr phi::PhiTriConfig kPhiVoxArticulation = {phi::kPhi300, 0.7f, 0.330f, false};

// ============================================================================
// Types
// ============================================================================

struct PhiVoxFormant {
	uint32_t phaseIncrement;           // Formant frequency as phase/sample at 44.1kHz
	float noteRatio;                   // Formant frequency as a ratio to the reference note (C3)
	float pulseGain[kPhiVoxMaxPulses]; // Signed; includes decay, polarity, balance; zero-padded
};

struct PhiVoxParams {
	PhiVoxFormant formant[kPhiVoxNumFormants];
	float breath;       // 0..~0.3
	float articulation; // Morph-burst gain
};

struct PhiVoxCache {
	PhiVoxParams bankA{};
	PhiVoxParams bankB{};

	// Crossfaded effective tables (rebuilt when the smoothed crossfade moves)
	uint32_t effFormantInc[kPhiVoxNumFormants]{};
	float effNoteRatio[kPhiVoxNumFormants]{};
	q31_t effPulseGain[kPhiVoxNumFormants][kPhiVoxMaxPulses]{};
	q31_t effPulseGainAbs[kPhiVoxNumFormants][kPhiVoxMaxPulses]{}; // For the noise-gate envelope
	q31_t effVoicedNoise{}; // breath + articulation burst, Q31, gated by pulse envelope in render

	// Previous-buffer snapshots for morph ramps: stepping increments/gains
	// per buffer under wave modulation was audible (and the old stateless
	// phase re-derivation TELEPORTED the burst mid-cycle every buffer)
	uint32_t effFormantIncFrom[kPhiVoxNumFormants]{};
	float effNoteRatioFrom[kPhiVoxNumFormants]{};
	float effMeanCompFrom[kPhiVoxNumFormants]{};
	q31_t effPulseGainFrom[kPhiVoxNumFormants][kPhiVoxMaxPulses]{};
	q31_t effPulseGainAbsFrom[kPhiVoxNumFormants][kPhiVoxMaxPulses]{};
	bool morphRamping{false};
	uint32_t effVersion{0};
	uint32_t effVersionSeen{0};
	float effMeanComp[kPhiVoxNumFormants]{}; // DC compensation numerators (× noteInc/formantInc at render)

	q31_t prevCrossfade{INT32_MIN};
	q31_t smoothedCrossfade{INT32_MIN};

	float artEnv{0.0f};  // Articulation burst envelope (decays per buffer)
	float prevCf{-1.0f}; // For crossfade-velocity detection
	uint32_t lastEnvTime{0xFFFFFFFF};
	uint32_t noiseState{0x9E3779B9u};

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

PhiVoxParams buildPhiVoxParams(uint16_t zone, float phaseOffset = 0.0f);

/// Render PHI_VOX for one buffer. Stateless per voice: pulse index and formant
/// phase are derived from the cycle phase at buffer start and advanced
/// incrementally (adds and carries only) within it.
/// trackingAmount 0..50: blends formant frequencies from fixed Hz (0, vocal
/// behavior) to note-relative ratios (50, harmonic-locked overtone behavior)
/// formantState packs per-voice formant phase (28b) + pulse index (4b) per
/// formant into an otherwise-unused per-voice uint64 - carrying it across
/// buffers instead of re-deriving eliminates morph-time phase teleports
void renderPhiVox(PhiVoxCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                  uint32_t phaseIncrement, uint32_t* startPhase, uint64_t* formantState, uint32_t retriggerPhase,
                  int32_t amplitude, int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade,
                  uint32_t pulseWidth, int32_t trackingAmount);

} // namespace deluge::dsp
