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
// PHI_GENDY — dynamic stochastic synthesis (Xenakis GENDYN lineage)
//
// The waveform is a 16-breakpoint polygon whose breakpoint amplitudes perform
// second-order random walks (velocity walks, then position walks - the GENDYN
// double random walk) between ELASTIC BARRIERS, once per audio buffer. Voices
// scan the current polygon at their note pitch - unlike Xenakis's original,
// the cycle length is locked, so pitch is stable and only the TIMBRE writhes.
//
// The Xenakis signature elements kept: reflecting (elastic) barriers, the
// second-order walk (drift has momentum), and step-size as the entropy knob.
// Added phi-family elements: barriers, step sizes and a home shape are painted
// PER NODE by phi-triangle spatial landscapes, so one region of the waveform
// can boil while another stays calm; a gentle spring toward a zone-defined
// home polygon means zero-entropy zones relax into a stable timbre instead of
// silence. Morph interpolates the WALK LAWS (never the walker state - click-
// free by construction), and crossfade MOTION startles the walkers: a kick of
// velocity noise, the family's morph-as-gesture in its fifth form. State is
// one shared polygon per Sound (like PHI_WEAVE's shared string).
//
// Literature:
//   Xenakis, I., "Formalized Music: Thought and Mathematics in Composition",
//     rev. ed., Pendragon Press, 1992 (chs. 9, 13-14: dynamic stochastic
//     synthesis / GENDY3).
//   Serra, M.-H., "Stochastic Composition and Stochastic Timbre: GENDY3 by
//     Iannis Xenakis", Computer Music Journal 17(1), 1993.
//   Hoffmann, P., "The New GENDYN Program", Computer Music Journal 24(2),
//     2000 (the algorithm as implemented here, including duration walks -
//     ours renormalize total cycle length so pitch stays locked).
//   Luque, S., "The Stochastic Synthesis of Iannis Xenakis", Leonardo Music
//     Journal 19, 2009.
// Deep dive with figures: docs/dev/phi-gendy-stochastic.md
// ============================================================================

inline constexpr int32_t kPhiGendyNumNodes = 16;  // Walking breakpoints
inline constexpr int32_t kPhiGendyScanNodes = 64; // Uniform scan table (variable-width polygon resampled onto it)
inline constexpr int32_t kPhiGendyNodeShift = 26; // 32 - log2(64)

// ============================================================================
// Phi Triangle Bank Configurations
// ============================================================================

// Entropy: base step size of the velocity walk (exponential range) and its
// spatial landscape around the polygon (some nodes boil, some stay calm)
inline constexpr phi::PhiTriConfig kPhiGendyStepBase = {phi::kPhi125, 0.7f, 0.000f, false};
inline constexpr phi::PhiTriConfig kPhiGendyStepLand = {phi::kPhi275, 0.6f, 0.140f, false};

// Elastic barriers: width base and spatial landscape (pinched nodes are
// quiet/controlled, open nodes swing hard)
inline constexpr phi::PhiTriConfig kPhiGendyBarrierBase = {phi::kPhi050, 0.8f, 0.290f, false};
inline constexpr phi::PhiTriConfig kPhiGendyBarrierLand = {phi::kPhi350, 0.5f, 0.410f, false};
// Barrier center offset landscape (asymmetric cages skew the polygon)
inline constexpr phi::PhiTriConfig kPhiGendyCenterLand = {phi::kPhiN050, 0.7f, 0.550f, true};

// Velocity cap (drift momentum) and home-shape pull strength
inline constexpr phi::PhiTriConfig kPhiGendyVelCap = {phi::kPhi200, 0.6f, 0.660f, false};
inline constexpr phi::PhiTriConfig kPhiGendyHomePull = {phi::kPhi075, 0.9f, 0.780f, false};

// Home shape partials (the polygon the walkers relax toward at low entropy)
inline constexpr phi::PhiTriConfig kPhiGendyHomeP1 = {phi::kPhi175, 0.5f, 0.060f, true};
inline constexpr phi::PhiTriConfig kPhiGendyHomeP2 = {phi::kPhi325, 0.45f, 0.870f, true};
inline constexpr phi::PhiTriConfig kPhiGendyHomeP3 = {phi::kPhiN025, 0.5f, 0.930f, true};

// Startle: how strongly crossfade MOTION kicks the walkers (from zone B)
inline constexpr phi::PhiTriConfig kPhiGendyStartle = {phi::kPhi300, 0.7f, 0.330f, false};

// DURATION WALKS (the deferred Xenakis element): segment WIDTHS perform their
// own second-order walk in elastic barriers, renormalized so cycle length -
// and therefore pitch - is exact. Amplitude walks tilt the spectrum; width
// walks move the harmonic SKELETON itself (the "vague -> immediate" fix).
// WidthStep = lurch rate (immediacy); WidthRange = how narrow/wide segments
// may go (breadth - near-zero widths are formant-like spikes).
inline constexpr phi::PhiTriConfig kPhiGendyWidthStep = {phi::kPhi225, 0.7f, 0.510f, false};
inline constexpr phi::PhiTriConfig kPhiGendyWidthRange = {phi::kPhiN100, 0.6f, 0.350f, false};

// INTERMITTENCY (walk v3): continuous walks are statistically stationary -
// the same jitter every tick reads as vague wash. Breakpoints now mostly
// HOLD (spring + faint drift) and, with this zone-controlled probability per
// tick, JUMP toward a fresh random target in their cage (Xenakis drew new
// breakpoints as discrete events). Low prob = a shape that stands still for
// seconds then snaps; high prob = denser chaos than the old walk. Wider
// character range, far less noise on average.
inline constexpr phi::PhiTriConfig kPhiGendyJumpProb = {phi::kPhi275, 0.6f, 0.590f, false};

// TONAL DIVERSITY: with intermittent walks, calm zones sit ON the home shape
// most of the time - the home IS the tone. HomeFamily selects among six
// archetypes (partials, ramp, square, spike, stairs, frozen noise); Curve
// reshapes segment interpolation in the resampler from smooth (rounded,
// dark) through linear to hold-like (staircase, buzzy).
inline constexpr phi::PhiTriConfig kPhiGendyHomeFamily = {phi::kPhi100, 0.95f, 0.150f, false};
inline constexpr phi::PhiTriConfig kPhiGendyCurve = {phi::kPhi250, 0.7f, 0.450f, true};
// Brilliance: spatial high-shelf baked into the scan table (slot-to-slot
// first difference, pitch-invariant under scanning). Random polylines fall
// at -12 dB/oct - twice a saw's slope - so GENDY reads dark without it.
inline constexpr phi::PhiTriConfig kPhiGendyBrite = {phi::kPhi175, 0.7f, 0.700f, false};

// ============================================================================
// Types
// ============================================================================

struct PhiGendyParams {
	float step[kPhiGendyNumNodes];      // Velocity-walk step size per node
	float barrierHi[kPhiGendyNumNodes]; // Elastic barrier cage per node
	float barrierLo[kPhiGendyNumNodes];
	float home[kPhiGendyNumNodes]; // Shape the walkers relax toward
	float velCap;                  // Momentum limit
	float homePull;                // Spring toward home (0 at high entropy)
	float startleGain;             // Morph-motion kick
	float widthStep;               // Duration-walk step size
	float widthMin;                // Elastic width barriers (fractions of a segment's nominal 1/16)
	float widthMax;
	float jumpProb; // Per-tick, per-breakpoint probability of a jump event
	float curve;    // Segment interpolation shape: -1 hold-like .. 0 linear .. +1 smooth
	float brite;    // Spatial high-shelf amount (0..~1.8)
};

struct PhiGendyCache {
	PhiGendyParams bankA{};
	PhiGendyParams bankB{};

	// Walker state: one shared polygon per Sound (never interpolated by morph)
	float a[kPhiGendyNumNodes]{};  // Breakpoint amplitudes
	float v[kPhiGendyNumNodes]{};  // Breakpoint velocities (second-order walk)
	float w[kPhiGendyNumNodes]{};  // Segment widths (duration walk; sum renormalized to 1)
	float vw[kPhiGendyNumNodes]{}; // Width velocities
	bool widthsInit{false};

	// Uniform scan table, rebuilt each tick by resampling the variable-width
	// polygon (extra entry duplicates position 0 for wrap). The render
	// crossfades prev -> current across each buffer so the polygon moves
	// continuously instead of stepping at the tick rate (same de-zipper as
	// PHI_WEAVE; the walk's jumps remain in the SHAPE, not as clicks)
	q31_t nodeQ[kPhiGendyScanNodes + 1]{};
	q31_t nodeQPrev[kPhiGendyScanNodes + 1]{};
	bool tablesValid{false};

	q31_t smoothedCrossfade{INT32_MIN};
	// Slow AGC (as PHI_WEAVE): the elastic cages can be as narrow as ~0.075,
	// and without normalization those zones were much quieter than siblings
	float agcPeak{0.0f};
	float agcRms{0.0f};
	float agcScale{0.0f};

	// Effective (crossfaded) walk laws, rebuilt only when the smoothed
	// crossfade moves past an epsilon (parked wave = zero lerp cost)
	float effStep[kPhiGendyNumNodes]{};
	float effBHi[kPhiGendyNumNodes]{};
	float effBLo[kPhiGendyNumNodes]{};
	float effHome[kPhiGendyNumNodes]{};
	float effVelCap{0.1f};
	float effHomePull{0.05f};
	float effCurve{0.0f};
	float effBrite{0.0f};
	float effWStep{0.001f};
	float effWMin{0.02f};
	float effWMax{0.12f};
	float effJumpProb{0.01f};
	float effCfCached{-2.0f};
	uint32_t lastTickTime{0xFFFFFFFF};
	float prevCf{-1.0f};
	float startleEnv{0.0f};
	uint32_t noiseState{0xB5297A4Du};
	bool startlePending{true}; // Note-on kicks the walkers awake

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

PhiGendyParams buildPhiGendyParams(uint16_t zone, float phaseOffset = 0.0f);

void renderPhiGendy(PhiGendyCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                    uint32_t phaseIncrement, uint32_t* startPhase, uint32_t retriggerPhase, int32_t amplitude,
                    int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade, uint32_t pulseWidth);

} // namespace deluge::dsp
