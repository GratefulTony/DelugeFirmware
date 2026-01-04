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

#include "dsp/disperser.h"

namespace deluge::dsp {

using namespace phi;

/**
 * Topology Zone Descriptions:
 *
 * Zone 0: Cascade - classic disperser with position→Q (Pinch) mapping
 *         Low position = low Q (broad, subtle), high = high Q (sharp, resonant)
 * Zone 1: Ping-Pong - stages alternate L/R processing
 * Zone 2: Stereo Spread - L/R get different frequency offsets
 * Zone 3: Cross-Coupled - L↔R feedback mixing between stages
 * Zone 4: Pitch Track - frequencies follow note pitch (when available)
 * Zone 5: Nested - Schroeder-style nested allpass structure
 * Zone 6: Diffuse - randomized per-stage coefficient variation
 * Zone 7: Spring - chirp/spring reverb character
 */

DisperserTopoParams computeDisperserTopoParams(q31_t smoothedTopo, const DisperserParams* params,
                                               float twistPhaseOffset) {
	constexpr q31_t kZoneWidth = ONE_Q31 / kDisperserNumZones;

	DisperserTopoParams result;
	result.zone =
	    std::clamp(static_cast<int32_t>(smoothedTopo / kZoneWidth), int32_t{0}, int32_t{kDisperserNumZones - 1});

	// Position within current zone (0-1)
	q31_t zoneStart = result.zone * kZoneWidth;
	float pos = static_cast<float>(smoothedTopo - zoneStart) / static_cast<float>(kZoneWidth);
	pos = std::clamp(pos, 0.0f, 1.0f);

	// Get phase offset from secret knob (metaPhaseTopo)
	double phRaw = params ? params->phases.effectiveTopo() : 0.0;

	// Compute wrapped phases for each φ-power frequency
	WrappedPhases wp = WrappedPhases::fromRaw(phRaw);

	// For detuning/harmonicBlend: add twist meta position to phase
	// This allows twist to "rotate" through topo's parameter evolution
	double phRawDet = phRaw + static_cast<double>(twistPhaseOffset);
	WrappedPhases wpDet = WrappedPhases::fromRaw(phRawDet);

	// Per-param frequency modulation (position-dependent speedup)
	float fm0 = freqMod(pos, wp.ph025);
	float fm1 = freqMod(pos, wp.ph050);
	float fm2 = freqMod(pos, wp.ph075);

	// Frequency modulation for detuning/harmonicBlend/emphasis uses wpDet phases
	// (fm slope also shifts with twist position)
	float fmDet = freqMod(pos, wpDet.ph033);
	float fmHarm = freqMod(pos, wpDet.ph067);
	float fmEmph = freqMod(pos, wpDet.ph100);

	// Each zone uses these triangles differently
	// The param meanings vary by topology - DSP dispatch interprets them
	switch (result.zone) {
	case 0: // Cascade: position → Q (Pinch), classic kHz Disperser behavior
		// Q maps from 0.5 (broad, subtle) to 20.0 (sharp, resonant)
		// Exponential curve for musical response: pos 0→0.5, 0.5→3.2, 1.0→20.0
		result.q = 0.5f * std::pow(40.0f, pos);
		// param0/1 can still evolve with phi-triangles for subtle modulation
		result.param0 = phiTriangleUnipolar(pos, kPhi025, fm0, wp.ph025, 0.1f);
		result.param1 = phiTriangleUnipolar(pos, kPhi050, fm1, wp.ph050, 0.3f);
		result.param2 = 0.0f; // spread=0 for classic cascade (all stages same freq)
		result.lrOffset = 0.0f;
		// Cascade: subtle detuning at low Q (shimmer), less at high Q (focus)
		// Uses wpDet so twist meta position can rotate through the pattern
		// Duty 0.63 for smooth shimmer at low Q
		result.detuning = phiTriangleUnipolar(pos, kPhi033, fmDet, wpDet.ph033, 0.2f, 0.63f) * (1.0f - pos * 0.7f);
		// Harmonics increase with Q (sharper = more overtone emphasis)
		result.harmonicBlend = phiTriangleUnipolar(pos, kPhi067, fmHarm, wpDet.ph067, 0.4f) * pos;
		// Cascade: subtle emphasis that increases with Q (sharper = more spectral contrast)
		// Duty 0.35: quick rise, slow fall - more time with negative (low freq) emphasis
		result.emphasis = phiTriangleBipolar(pos, kPhi100, fmEmph, wpDet.ph100, 0.55f, 0.35f) * pos * 0.6f;
		break;

	case 1:               // Ping-Pong: alternation depth, L/R phase, freq split
		result.q = 20.0f; // Max Q for non-cascade topologies
		result.param0 = phiTriangleUnipolar(pos, kPhi025, fm0, wp.ph025, 0.1f, 0.8f);
		result.param1 = phiTriangleUnipolar(pos, kPhi050, fm1, wp.ph050, 0.3f, 0.7f);
		result.param2 = phiTriangleUnipolar(pos, kPhi075, fm2, wp.ph075, 0.6f, 0.6f);
		result.lrOffset = result.param1 * 0.5f; // L/R phase difference
		// Ping-pong: moderate detuning for stereo shimmer (uses wpDet)
		result.detuning = phiTriangleUnipolar(pos, kPhi050, fmDet, wpDet.ph050, 0.3f, 0.54f);
		// Balanced harmonics evolving with alternation
		result.harmonicBlend = phiTriangleUnipolar(pos, kPhi033, fmHarm, wpDet.ph033, 0.5f, 0.5f);
		// Ping-pong: emphasis alternates for stereo spectral interest
		// Duty 0.6: slower rise, quicker fall - more time with positive (high freq) emphasis
		result.emphasis = phiTriangleBipolar(pos, kPhi075, fmEmph, wpDet.ph075, 0.4f, 0.6f) * 0.5f;
		break;

	case 2: // Bimodal (Formant) - stages cluster into two frequency groups
		result.q = 20.0f;
		// Position controls separation between modes (0=together, 1=5 octaves apart)
		// Safe because modes reach toward each other, keeping stages bounded
		result.param0 = pos * 5.0f; // 0-5 octave separation
		// param1 evolves balance between modes via phi-triangle
		result.param1 = phiTriangleUnipolar(pos, kPhi075, fm1, wp.ph075, 0.5f, 0.6f);
		result.lrOffset = pos * 0.4f; // L/R get opposite modes at high separation
		// Bimodal: detuning increases with separation (formant shimmer, uses wpDet)
		// Duty 0.59 for smooth formant transitions
		result.detuning = phiTriangleUnipolar(pos, kPhi025, fmDet, wpDet.ph025, 0.15f, 0.59f) * (0.3f + pos * 0.7f);
		// Harmonics follow mode balance (shifting timbral emphasis, uses wpDet)
		result.harmonicBlend = phiTriangleUnipolar(pos, kPhi075, fmHarm, wpDet.ph075, 0.5f, 0.6f);
		// Bimodal: emphasis follows mode separation (more contrast at wider separation)
		// Duty 0.45: near-symmetric with slight low-freq bias
		result.emphasis = phiTriangleBipolar(pos, kPhi050, fmEmph, wpDet.ph050, 0.3f, 0.45f) * (0.2f + pos * 0.6f);
		break;

	case 3: // Cross-Coupled: cross amount, asymmetry, damping
		result.q = 20.0f;
		result.param0 = phiTriangleUnipolar(pos, kPhi050, fm0, wp.ph050, 0.2f);
		result.param1 = phiTriangleBipolar(pos, kPhi075, fm1, wp.ph075, 0.5f) * 0.5f + 0.5f;
		result.param2 = phiTriangleUnipolar(pos, kPhi025, fm2, wp.ph025, 0.8f);
		result.lrOffset = (result.param1 - 0.5f) * 0.3f; // Asymmetry creates offset
		// Cross: detuning from asymmetry (swirling stereo, uses wpDet)
		// Duty 0.5 for more percussive swirl character
		result.detuning =
		    std::abs(result.param1 - 0.5f) * phiTriangleUnipolar(pos, kPhi067, fmDet, wpDet.ph067, 0.4f, 0.5f);
		// Harmonics from cross amount (more coupling = richer harmonics, uses wpDet)
		result.harmonicBlend = phiTriangleUnipolar(pos, kPhi050, fmHarm, wpDet.ph050, 0.2f) * 0.8f;
		// Cross: emphasis from asymmetry for swirling spectral contrast
		// Duty 0.7: slow rise, fast fall - extended high-freq dwell for swirl character
		result.emphasis =
		    (result.param1 - 0.5f) * phiTriangleBipolar(pos, kPhi033, fmEmph, wpDet.ph033, 0.6f, 0.7f) * 0.7f;
		break;

	case 4: // Pitch Track: tracking tightness, harmonic blend, octave
		result.q = 20.0f;
		result.param0 = phiTriangleUnipolar(pos, kPhi025, fm0, wp.ph025, 0.1f);
		result.param1 = phiTriangleUnipolar(pos, kPhi067, fm1, wp.ph067, 0.35f);
		result.param2 = phiTriangleUnipolar(pos, kPhi100, fm2, wp.ph100, 0.6f);
		result.lrOffset = 0.0f; // Mono pitch tracking
		// Pitch track: minimal detuning to preserve pitch accuracy (uses wpDet)
		// Duty 0.77 for very smooth tracking, minimal shimmer disruption
		result.detuning = phiTriangleUnipolar(pos, kPhi033, fmDet, wpDet.ph033, 0.1f, 0.77f) * 0.2f;
		// Harmonics evolve for timbral richness while tracking pitch (uses wpDet)
		result.harmonicBlend = phiTriangleUnipolar(pos, kPhi050, fmHarm, wpDet.ph050, 0.4f, 0.7f);
		// Pitch track: minimal emphasis to preserve pitch clarity
		// Duty 0.55: near-symmetric, gentle high-freq bias
		result.emphasis = phiTriangleBipolar(pos, kPhi025, fmEmph, wpDet.ph025, 0.2f, 0.55f) * 0.15f;
		break;

	case 5: // Nested: nesting depth, inner/outer balance
		result.q = 20.0f;
		result.param0 = phiTriangleUnipolar(pos, kPhi033, fm0, wp.ph033, 0.2f);
		result.param1 = phiTriangleUnipolar(pos, kPhi075, fm1, wp.ph075, 0.45f);
		result.param2 = phiTriangleUnipolar(pos, kPhi050, fm2, wp.ph050, 0.7f);
		result.lrOffset = result.param2 * 0.2f;
		// Nested: detuning for Schroeder diffusion shimmer (uses wpDet)
		result.detuning = phiTriangleUnipolar(pos, kPhi033, fmDet, wpDet.ph033, 0.3f, 0.54f);
		// Harmonics evolve with nesting depth
		result.harmonicBlend = phiTriangleUnipolar(pos, kPhi067, fmHarm, wpDet.ph067, 0.45f, 0.5f);
		// Nested: emphasis for Schroeder spectral contrast
		// Duty 0.4: quick rise, extended fall - low-freq warmth for diffusion
		result.emphasis = phiTriangleBipolar(pos, kPhi100, fmEmph, wpDet.ph100, 0.5f, 0.4f) * 0.55f;
		break;

	case 6: // Diffuse: randomness, correlation, drift
		result.q = 20.0f;
		result.param0 = phiTriangleUnipolar(pos, kPhi050, fm0, wp.ph050, 0.15f);
		result.param1 = phiTriangleUnipolar(pos, kPhi033, fm1, wp.ph033, 0.4f);
		result.param2 = phiTriangleUnipolar(pos, kPhi075, fm2, wp.ph075, 0.65f);
		result.lrOffset = result.param0 * 0.4f; // Decorrelation
		// Diffuse: high detuning for maximum shimmer (uses wpDet)
		result.detuning = phiTriangleUnipolar(pos, kPhi050, fmDet, wpDet.ph050, 0.4f, 0.63f);
		// Harmonics follow randomness
		result.harmonicBlend = phiTriangleUnipolar(pos, kPhi025, fmHarm, wpDet.ph025, 0.5f, 0.6f);
		// Diffuse: strong emphasis for maximum spectral variety
		// Duty 0.65: high-freq bias for bright diffusion character
		result.emphasis = phiTriangleBipolar(pos, kPhi067, fmEmph, wpDet.ph067, 0.35f, 0.65f) * 0.7f;
		break;

	case 7: // Spring: chirp character, decay, density
		result.q = 20.0f;
		result.param0 = phiTriangleUnipolar(pos, kPhi025, fm0, wp.ph025, 0.1f);
		result.param1 = phiTriangleUnipolar(pos, kPhi050, fm1, wp.ph050, 0.3f);
		result.param2 = phiTriangleUnipolar(pos, kPhi075, fm2, wp.ph075, 0.55f);
		result.lrOffset = result.param2 * 0.15f;
		// Spring: moderate detuning for spring reverb character (uses wpDet)
		result.detuning = phiTriangleUnipolar(pos, kPhi075, fmDet, wpDet.ph075, 0.25f, 0.45f);
		// Harmonics evolve for spring timbre
		result.harmonicBlend = phiTriangleUnipolar(pos, kPhi033, fmHarm, wpDet.ph033, 0.35f, 0.6f);
		// Spring: moderate emphasis for spring timbral character
		// Duty 0.5: symmetric for balanced spring response
		result.emphasis = phiTriangleBipolar(pos, kPhi050, fmEmph, wpDet.ph050, 0.45f, 0.5f) * 0.45f;
		break;

	default:
		break;
	}

	return result;
}

/**
 * Twist Zone Descriptions:
 *
 * Maximum chirp architecture - transient emphasis for bigger chirps.
 *
 * Zone 0: Width - Stereo spread via L/R frequency offset
 * Zone 1: Punch - Transient emphasis before dispersion (bigger chirps!)
 * Zone 2: Curve - Frequency distribution (low cluster → linear → high cluster)
 * Zone 3: Chirp - Transient-triggered delay for chirp echoes
 * Zone 4: QTilt - Q varies across stages (uniform → high sharp → low sharp)
 * Zones 5-7: Meta - All effects combined with φ-triangle evolution
 */

DisperserTwistParams computeDisperserTwistParams(q31_t smoothedTwist, const DisperserParams* params) {
	constexpr q31_t kZoneWidth = ONE_Q31 / kDisperserNumZones;
	constexpr q31_t kZone5 = kZoneWidth * 5; // Start of meta zone

	DisperserTwistParams result;

	int32_t zone =
	    std::clamp(static_cast<int32_t>(smoothedTwist / kZoneWidth), int32_t{0}, int32_t{kDisperserNumZones - 1});

	if (zone < 5) {
		// Zones 0-4: Individual effects
		q31_t zoneStart = zone * kZoneWidth;
		float pos = static_cast<float>(smoothedTwist - zoneStart) / static_cast<float>(kZoneWidth);
		pos = std::clamp(pos, 0.0f, 1.0f);

		switch (zone) {
		case 0: // Width - stereo spread only
			result.width = pos;
			break;

		case 1: // Punch - transient emphasis before dispersion
			// pos controls transient boost amount (0 = none, 1 = max ~12dB)
			// More punch = bigger chirps because transients have more energy to disperse
			result.punch = pos;
			break;

		case 2: // Curve - bipolar frequency spread distribution
			// pos=0: stages cluster at LOW frequencies
			// pos=0.5: LINEAR distribution (even spacing)
			// pos=1: stages cluster at HIGH frequencies
			result.spreadCurve = pos;
			break;

		case 3: // Chirp - feedback-based chirp echoes
			// pos controls feedback amount (delay time from freq knob)
			// Higher feedback = more repeating chirp echoes
			result.chirpAmount = pos;
			break;

		case 4: // QTilt - Q varies across stages
			// Continuous: 0→0 tilt, 0.5→+1 tilt (high sharp), 1→-1 tilt (low sharp)
			if (pos < 0.5f) {
				result.qTilt = pos * 2.0f; // 0 → +1
			}
			else {
				result.qTilt = 3.0f - pos * 4.0f; // +1 → -1
			}
			break;
		}
	}
	else {
		// Zones 5-7: Meta - all effects with φ-triangle evolution
		float pos = static_cast<float>(smoothedTwist - kZone5) / static_cast<float>(ONE_Q31 - kZone5);
		pos = std::clamp(pos, 0.0f, 1.0f);

		// Get combined phase offset (metaPhase + 100*gammaPhase)
		double phRaw = params ? params->phases.effectiveMeta() : 0.0;
		WrappedPhases wp = WrappedPhases::fromRaw(phRaw);

		// Per-effect frequency modulation
		float fmW = freqMod(pos, wp.ph025);
		float fmP = freqMod(pos, wp.ph033);
		float fmC = freqMod(pos, wp.ph050);
		float fmQ = freqMod(pos, wp.ph067);
		float fmD = freqMod(pos, wp.ph075);

		// Width: scale * param pattern
		float wS = std::min(phiTriangleUnipolar(pos, kPhi025, fmW, wp.ph025, 0.166f, 0.8f) * 2.0f, 1.0f);
		float wP = phiTriangleUnipolar(pos, kPhi050, fmW, wp.ph050, 0.984f, 0.7f);
		result.width = wS * wP;

		// Punch evolves - more punch during certain phases
		result.punch = phiTriangleUnipolar(pos, kPhi033, fmP, wp.ph033, 0.3f, 0.7f);

		// Curve sweeps bipolar
		float curveRaw = phiTriangleBipolar(pos, kPhi050, fmC, wp.ph050, 0.5f);
		result.spreadCurve = 0.5f + curveRaw * 0.5f; // Map to 0-1

		// Chirp feedback evolves (delay time from freq knob)
		result.chirpAmount = phiTriangleUnipolar(pos, kPhi067, fmD, wp.ph067, 0.4f, 0.6f);

		// Q tilt sweeps bipolar
		result.qTilt = phiTriangleBipolar(pos, kPhiN025, fmQ, wp.phN025, 0.7f) * 0.8f;

		// Note: detuning/harmonicBlend now handled via topo zones + twistPhaseOffset
		// Twist meta position adds to topo's raw phase, rotating through topo's patterns
	}

	return result;
}

} // namespace deluge::dsp
