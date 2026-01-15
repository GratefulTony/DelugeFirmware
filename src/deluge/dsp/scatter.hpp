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

#include "dsp/phi_triangle.hpp"
#include "dsp/util.hpp"
#include "dsp/zone_param.hpp"
#include <array>
#include <cstdint>

namespace deluge::dsp::scatter {

// Zone helpers from parent namespace (dsp::computeZoneQ31, dsp::ZoneInfo) are
// accessible via C++ parent namespace lookup without explicit qualification

// Precomputed reciprocal for q31 to float conversion (multiplication is ~10x faster than division on ARM)
constexpr float kQ31ToFloat = 1.0f / static_cast<float>(ONE_Q31);

/**
 * Phi triangle bank for structural scatter params (Zone A meta)
 *
 * Bank indices:
 *   [0] sliceOffset - Offset added to slice selection
 *   [1] lengthMult  - Slice length multiplier
 *   [2] skipProb    - Probability of skipping to non-adjacent slice
 *
 * Slower φ^n frequencies for gradual structural evolution.
 * Phase offsets spread for decorrelation.
 */
constexpr std::array<phi::PhiTriConfig, 3> kStructuralBank = {{
    {phi::kPhi100, 0.80f, 0.00f, false}, // [0] sliceOffset
    {phi::kPhi150, 0.60f, 0.33f, false}, // [1] lengthMult
    {phi::kPhi175, 0.70f, 0.67f, false}, // [2] skipProb
}};

/**
 * Phi triangle bank for timbral scatter params (Zone B meta)
 *
 * Bank indices:
 *   [0] reverseProb - Probability of reversing slice
 *   [1] filterFreq  - Bandpass center frequency
 *   [2] delayFeed   - Per-grain delay send amount
 *   [3] envShape    - Envelope shape (percussive to reversed)
 *
 * Mix of slower and faster φ^n for varied timbral movement.
 * Phase offsets spread by 0.25 for decorrelation.
 */
constexpr std::array<phi::PhiTriConfig, 4> kTimbraBank = {{
    {phi::kPhiN050, 0.50f, 0.00f, false}, // [0] reverseProb (slow)
    {phi::kPhi067, 0.70f, 0.25f, false},  // [1] filterFreq
    {phi::kPhi125, 0.60f, 0.50f, false},  // [2] delayFeed
    {phi::kPhi200, 0.80f, 0.75f, false},  // [3] envShape (faster)
}};

/**
 * Map triangle value [0,1] to slice index within a zone
 * @param tri Triangle value in [0,1]
 * @param numSlices Total number of slices in the zone
 * @return Slice index [0, numSlices-1]
 */
[[gnu::always_inline]] inline int32_t sliceIndexFromTriangle(float tri, int32_t numSlices) {
	if (numSlices <= 1) {
		return 0;
	}
	int32_t idx = static_cast<int32_t>(tri * static_cast<float>(numSlices));
	return std::clamp(idx, int32_t{0}, numSlices - 1);
}

/**
 * Map triangle value [0,1] to gate duty cycle
 * Never fully mutes - minimum gate is 12.5%
 * @param tri Triangle value in [0,1]
 * @return Gate ratio [0.125, 1.0]
 */
[[gnu::always_inline]] inline float gateFromTriangle(float tri) {
	return 0.125f + tri * 0.875f;
}

/**
 * Map triangle value [0,1] to reverse decision
 * @param tri Triangle value in [0,1]
 * @param threshold Probability threshold for reversing
 * @return true if slice should be reversed
 */
[[gnu::always_inline]] inline bool reverseFromTriangle(float tri, float threshold) {
	return tri > threshold;
}

/**
 * Map triangle value [0,1] to pitch ratio
 * Center is 1.0 (no pitch change), range depends on depth
 * @param tri Triangle value in [0,1]
 * @param semitoneRange Maximum semitone deviation (e.g., 12 for octave)
 * @return Pitch ratio
 */
[[gnu::always_inline]] inline float pitchFromTriangle(float tri, float semitoneRange) {
	// Map [0,1] to [-semitoneRange, +semitoneRange] semitones
	float semitones = (tri * 2.0f - 1.0f) * semitoneRange;
	// Convert semitones to pitch ratio: 2^(semitones/12)
	// Use fast approximation: exp2f(semitones / 12.0f)
	return std::exp2(semitones / 12.0f);
}

/**
 * Map triangle value [0,1] to tape speed ratio
 * @param tri Triangle value in [0,1]
 * @param minSpeed Minimum speed (e.g., 0.5 for half speed)
 * @param maxSpeed Maximum speed (e.g., 2.0 for double speed)
 * @return Speed ratio
 */
[[gnu::always_inline]] inline float tapeSpeedFromTriangle(float tri, float minSpeed = 0.5f, float maxSpeed = 2.0f) {
	return minSpeed + tri * (maxSpeed - minSpeed);
}

/**
 * Map triangle value [0,1] to filter frequency
 * Uses exponential mapping for perceptually linear sweep
 * @param tri Triangle value in [0,1]
 * @param minFreq Minimum frequency in Hz
 * @param maxFreq Maximum frequency in Hz
 * @return Frequency in Hz
 */
[[gnu::always_inline]] inline float filterFreqFromTriangle(float tri, float minFreq = 200.0f, float maxFreq = 8000.0f) {
	// Exponential mapping for perceptually linear sweep
	float logMin = std::log2(minFreq);
	float logMax = std::log2(maxFreq);
	return std::exp2(logMin + tri * (logMax - logMin));
}

/**
 * Compute grain envelope multiplier with configurable shape
 * Very cheap: ~6 multiplies, few branches, no trig
 *
 * @param positionInSlice Current position within slice [0, sliceLength)
 * @param sliceLength Total length of slice in samples
 * @param gateRatio Gate duty cycle [0,1] - audio plays during this portion
 * @param depth Envelope depth [0,1] - 0=hard cut, 1=full smooth envelope
 * @param envShape Peak position [0,1] - 0=fade-out only, 0.5=symmetric, 1=fade-in only
 * @param envWidth Envelope region [0,1] - 1=full slice, 0.1=edges only (10% each end)
 * @return Amplitude multiplier [0,1]
 */
[[gnu::always_inline]] inline float grainEnvelope(int32_t positionInSlice, int32_t sliceLength, float gateRatio,
                                                  float depth, float envShape = 0.5f, float envWidth = 1.0f) {
	if (sliceLength <= 0) {
		return 1.0f;
	}

	// Anti-click fade: always fade to 0 at grain edges (~10ms = 440 samples at 44.1kHz)
	// This is independent of depth and happens at the raw slice boundaries
	constexpr int32_t kAntiClickSamples = 440;
	float antiClick = 1.0f;
	int32_t gatedLength = static_cast<int32_t>(static_cast<float>(sliceLength) * gateRatio);
	if (gatedLength > kAntiClickSamples * 2) {
		if (positionInSlice < kAntiClickSamples) {
			antiClick = static_cast<float>(positionInSlice) / static_cast<float>(kAntiClickSamples);
		}
		else if (positionInSlice > gatedLength - kAntiClickSamples) {
			antiClick = static_cast<float>(gatedLength - positionInSlice) / static_cast<float>(kAntiClickSamples);
		}
	}
	else if (gatedLength > 0) {
		// Very short slice: use half the length for fade
		int32_t fadeLen = gatedLength / 2;
		if (fadeLen > 0) {
			if (positionInSlice < fadeLen) {
				antiClick = static_cast<float>(positionInSlice) / static_cast<float>(fadeLen);
			}
			else if (positionInSlice > gatedLength - fadeLen) {
				antiClick = static_cast<float>(gatedLength - positionInSlice) / static_cast<float>(fadeLen);
			}
		}
	}

	// Normalized position within slice [0,1]
	float pos = static_cast<float>(positionInSlice) / static_cast<float>(sliceLength);

	// If past gate threshold, output is silent
	if (pos > gateRatio) {
		return 0.0f;
	}

	// Rescale position to [0,1] within the gated portion
	float gatedPos = pos / gateRatio;

	// Edge-only mode: flat middle with envelope only at edges
	// envWidth=1.0: full envelope, envWidth=0.1: only first/last 10%
	float envelope;
	if (envWidth < 1.0f && envWidth > 0.0f) {
		float edgeSize = envWidth * 0.5f; // Half at each end
		if (gatedPos < edgeSize) {
			// Attack region - remap to [0, envShape]
			float t = gatedPos / edgeSize;
			// Parabolic attack: t^2 for smooth start
			float attack = (envShape > 0.001f) ? t * t : 1.0f;
			envelope = attack;
		}
		else if (gatedPos > (1.0f - edgeSize)) {
			// Decay region - remap to [envShape, 1]
			float t = (gatedPos - (1.0f - edgeSize)) / edgeSize;
			// Parabolic decay: (1-t)^2 for smooth end
			float decay = (envShape < 0.999f) ? (1.0f - t) * (1.0f - t) : 1.0f;
			envelope = decay;
		}
		else {
			// Flat middle region
			envelope = 1.0f;
		}
	}
	else {
		// Full slice envelope with configurable peak position (envShape)
		// envShape=0: peak at start (fade-out only, preserves attack)
		// envShape=0.5: peak at middle (symmetric Hanning-like)
		// envShape=1: peak at end (fade-in only)
		if (envShape < 0.001f) {
			// Fade-out only: (1-pos)^2
			envelope = (1.0f - gatedPos) * (1.0f - gatedPos);
		}
		else if (envShape > 0.999f) {
			// Fade-in only: pos^2
			envelope = gatedPos * gatedPos;
		}
		else {
			// Asymmetric envelope with peak at envShape position
			if (gatedPos < envShape) {
				// Attack phase: parabolic ramp up to peak
				float t = gatedPos / envShape;
				envelope = t * t;
			}
			else {
				// Decay phase: parabolic ramp down from peak
				float t = (gatedPos - envShape) / (1.0f - envShape);
				envelope = (1.0f - t) * (1.0f - t);
			}
		}
	}

	// Combine: anti-click always applied, depth-controlled envelope on top
	// depth=0: just anti-click fade at edges
	// depth=1: full envelope shape
	float depthEnv = 1.0f + depth * (envelope - 1.0f);
	return antiClick * depthEnv;
}

/**
 * Precomputed envelope parameters (Q31 fixed-point) for zero-float per-sample evaluation
 * Compute once per slice boundary, use for all samples in slice
 * All reciprocals stored as Q31: multiply position by reciprocal, result is Q31 [0, ONE_Q31]
 */
struct GrainEnvPrecomputedQ31 {
	int32_t invSliceLength{0};         ///< ONE_Q31 / sliceLength (for pos normalization)
	int32_t invGateRatio{ONE_Q31};     ///< ONE_Q31 / gateRatio (for gated pos)
	int32_t invFadeLen{0};             ///< ONE_Q31 / fadeLen (for anti-click, legacy)
	int32_t invAttackLen{0};           ///< ONE_Q31 / attackFadeLen (asymmetric fade-in)
	int32_t invDecayLen{0};            ///< ONE_Q31 / decayFadeLen (asymmetric fade-out)
	int32_t invEdgeSize{0};            ///< ONE_Q31 / edgeSize (for edge-only mode)
	int32_t invEnvShape{0};            ///< ONE_Q31 / envShape (for attack phase)
	int32_t invOneMinusEnvShape{0};    ///< ONE_Q31 / (1 - envShape) (for decay phase)
	int32_t gatedLength{0};            ///< sliceLength * gateRatio
	int32_t fadeLen{0};                ///< Base anti-click fade length
	int32_t attackFadeLen{0};          ///< Fade-in length (envShape scales this)
	int32_t decayFadeLen{0};           ///< Fade-out length (1-envShape scales this)
	int32_t edgeSizeQ31{0};            ///< envWidth * 0.5 in Q31
	int32_t depthQ31{0};               ///< Envelope depth in Q31
	int32_t oneMinusDepthQ31{ONE_Q31}; ///< (1 - depth) in Q31, precomputed for blend
	int32_t envShapeQ31{ONE_Q31 / 2};  ///< Envelope shape in Q31
	int32_t gateRatioQ31{ONE_Q31};     ///< Gate ratio in Q31 for threshold check
	bool useEdgeMode{false};           ///< Whether envWidth < 1.0
	bool useShortFade{false};          ///< Whether gatedLength <= 880 (2x anti-click)
	bool depthIsMax{false};            ///< depth >= 0.99, skip blending
};

/**
 * Precomputed envelope parameters for fast per-sample evaluation
 * Compute once per slice boundary, use for all samples in slice
 * Eliminates ~9 divisions per sample by converting to multiplications
 */
struct GrainEnvPrecomputed {
	float invSliceLength{0};      ///< 1.0 / sliceLength
	float invGateRatio{1.0f};     ///< 1.0 / gateRatio
	float invFadeLen{0};          ///< 1.0 / fadeLen (for anti-click)
	float invEdgeSize{0};         ///< 1.0 / edgeSize (for edge-only mode)
	float invEnvShape{0};         ///< 1.0 / envShape (for attack phase)
	float invOneMinusEnvShape{0}; ///< 1.0 / (1 - envShape) (for decay phase)
	int32_t gatedLength{0};       ///< sliceLength * gateRatio
	int32_t fadeLen{0};           ///< Anti-click fade length
	float edgeSize{0};            ///< envWidth * 0.5
	float depth{0};               ///< Envelope depth
	float envShape{0.5f};         ///< Envelope shape
	float envWidth{1.0f};         ///< Envelope width
	float gateRatio{1.0f};        ///< Gate ratio for threshold check
	bool useEdgeMode{false};      ///< Whether envWidth < 1.0
	bool useShortFade{false};     ///< Whether gatedLength <= 880 (2x anti-click)
};

/**
 * Prepare precomputed envelope parameters at slice boundary
 * Call once when slice changes, result used for all samples in slice
 */
[[gnu::always_inline]] inline GrainEnvPrecomputed
prepareGrainEnvelope(int32_t sliceLength, float gateRatio, float depth, float envShape = 0.5f, float envWidth = 1.0f) {
	GrainEnvPrecomputed p;
	constexpr int32_t kAntiClickSamples = 440;

	if (sliceLength <= 0) {
		return p; // Will return 1.0 for all samples
	}

	p.invSliceLength = 1.0f / static_cast<float>(sliceLength);
	p.gateRatio = gateRatio;
	p.depth = depth;
	p.envShape = envShape;
	p.envWidth = envWidth;

	// Gate ratio reciprocal (avoid div by zero)
	p.invGateRatio = (gateRatio > 0.001f) ? (1.0f / gateRatio) : 1000.0f;

	// Gated length and fade parameters
	p.gatedLength = static_cast<int32_t>(static_cast<float>(sliceLength) * gateRatio);

	if (p.gatedLength > kAntiClickSamples * 2) {
		p.fadeLen = kAntiClickSamples;
		p.invFadeLen = 1.0f / static_cast<float>(kAntiClickSamples);
		p.useShortFade = false;
	}
	else if (p.gatedLength > 0) {
		p.fadeLen = p.gatedLength / 2;
		p.invFadeLen = (p.fadeLen > 0) ? (1.0f / static_cast<float>(p.fadeLen)) : 0.0f;
		p.useShortFade = true;
	}

	// Edge mode parameters
	p.useEdgeMode = (envWidth < 1.0f && envWidth > 0.0f);
	if (p.useEdgeMode) {
		p.edgeSize = envWidth * 0.5f;
		p.invEdgeSize = (p.edgeSize > 0.001f) ? (1.0f / p.edgeSize) : 1000.0f;
	}

	// Envelope shape reciprocals
	p.invEnvShape = (envShape > 0.001f) ? (1.0f / envShape) : 1000.0f;
	p.invOneMinusEnvShape = (envShape < 0.999f) ? (1.0f / (1.0f - envShape)) : 1000.0f;

	return p;
}

/**
 * Prepare precomputed Q31 envelope parameters at slice boundary
 * All reciprocals in Q31 format for pure integer per-sample math
 */
[[gnu::always_inline]] inline GrainEnvPrecomputedQ31 prepareGrainEnvelopeQ31(int32_t sliceLength, float gateRatio,
                                                                             float depth, float envShape = 0.5f,
                                                                             float envWidth = 1.0f) {
	GrainEnvPrecomputedQ31 p;
	constexpr int32_t kAntiClickSamples = 440;

	if (sliceLength <= 0) {
		return p; // Will return ONE_Q31 for all samples
	}

	// Inverse slice length: ONE_Q31 / sliceLength
	p.invSliceLength = ONE_Q31 / sliceLength;

	// Gate ratio and inverse
	p.gateRatioQ31 = static_cast<int32_t>(gateRatio * static_cast<float>(ONE_Q31));
	p.invGateRatio = (gateRatio > 0.001f) ? static_cast<int32_t>(ONE_Q31 / gateRatio) : ONE_Q31;

	// Depth in Q31
	p.depthQ31 = static_cast<int32_t>(depth * static_cast<float>(ONE_Q31));

	// Envelope shape in Q31
	p.envShapeQ31 = static_cast<int32_t>(envShape * static_cast<float>(ONE_Q31));

	// Gated length and fade parameters
	p.gatedLength = static_cast<int32_t>(static_cast<float>(sliceLength) * gateRatio);

	if (p.gatedLength > kAntiClickSamples * 2) {
		p.fadeLen = kAntiClickSamples;
		p.invFadeLen = ONE_Q31 / kAntiClickSamples;
		p.useShortFade = false;
	}
	else if (p.gatedLength > 0) {
		p.fadeLen = p.gatedLength / 2;
		p.invFadeLen = (p.fadeLen > 0) ? (ONE_Q31 / p.fadeLen) : 0;
		p.useShortFade = true;
	}

	// Asymmetric fade lengths based on envShape
	// envShape=0: instant attack, full decay (percussive)
	// envShape=0.5: symmetric
	// envShape=1: full attack, instant decay (reversed)
	// Scale factor 2x so envShape=0.5 gives full fadeLen to each
	int32_t baseFade = p.fadeLen;
	float attackScale = std::min(envShape * 2.0f, 1.0f);
	float decayScale = std::min((1.0f - envShape) * 2.0f, 1.0f);
	p.attackFadeLen = static_cast<int32_t>(static_cast<float>(baseFade) * attackScale);
	p.decayFadeLen = static_cast<int32_t>(static_cast<float>(baseFade) * decayScale);
	// Ensure minimum anti-click even at extreme shapes
	constexpr int32_t kMinAntiClick = 64;
	if (p.attackFadeLen < kMinAntiClick && baseFade >= kMinAntiClick) {
		p.attackFadeLen = kMinAntiClick;
	}
	if (p.decayFadeLen < kMinAntiClick && baseFade >= kMinAntiClick) {
		p.decayFadeLen = kMinAntiClick;
	}
	p.invAttackLen = (p.attackFadeLen > 0) ? (ONE_Q31 / p.attackFadeLen) : 0;
	p.invDecayLen = (p.decayFadeLen > 0) ? (ONE_Q31 / p.decayFadeLen) : 0;

	// Edge mode parameters
	p.useEdgeMode = (envWidth < 1.0f && envWidth > 0.0f);
	if (p.useEdgeMode) {
		float edgeSize = envWidth * 0.5f;
		p.edgeSizeQ31 = static_cast<int32_t>(edgeSize * static_cast<float>(ONE_Q31));
		// invEdgeSize: need Q31 / edgeSize, but edgeSize is [0,0.5], so divide by fraction
		p.invEdgeSize = (edgeSize > 0.001f) ? static_cast<int32_t>(ONE_Q31 / edgeSize) : ONE_Q31;
	}

	// Envelope shape reciprocals
	p.invEnvShape = (envShape > 0.001f) ? static_cast<int32_t>(ONE_Q31 / envShape) : ONE_Q31;
	p.invOneMinusEnvShape = (envShape < 0.999f) ? static_cast<int32_t>(ONE_Q31 / (1.0f - envShape)) : ONE_Q31;

	// Precompute depth blend values for per-sample optimization
	p.oneMinusDepthQ31 = ONE_Q31 - p.depthQ31;
	p.depthIsMax = (depth >= 0.99f);

	return p;
}

/**
 * Fast grain envelope using precomputed reciprocals
 * ~10x faster than grainEnvelope() - uses only multiplications, no divisions
 *
 * @param positionInSlice Current position within slice [0, sliceLength)
 * @param p Precomputed parameters from prepareGrainEnvelope()
 * @return Amplitude multiplier [0,1]
 */
[[gnu::always_inline]] inline float grainEnvelopeFast(int32_t positionInSlice, const GrainEnvPrecomputed& p) {
	// Early out if no valid slice
	if (p.invSliceLength == 0.0f) {
		return 1.0f;
	}

	// Anti-click fade (multiplication instead of division)
	float antiClick = 1.0f;
	if (!p.useShortFade) {
		if (positionInSlice < p.fadeLen) {
			antiClick = static_cast<float>(positionInSlice) * p.invFadeLen;
		}
		else if (positionInSlice > p.gatedLength - p.fadeLen) {
			antiClick = static_cast<float>(p.gatedLength - positionInSlice) * p.invFadeLen;
		}
	}
	else if (p.fadeLen > 0) {
		if (positionInSlice < p.fadeLen) {
			antiClick = static_cast<float>(positionInSlice) * p.invFadeLen;
		}
		else if (positionInSlice > p.gatedLength - p.fadeLen) {
			antiClick = static_cast<float>(p.gatedLength - positionInSlice) * p.invFadeLen;
		}
	}

	// Normalized position (multiplication instead of division)
	float pos = static_cast<float>(positionInSlice) * p.invSliceLength;

	// Gate threshold check
	if (pos > p.gateRatio) {
		return 0.0f;
	}

	// Gated position (multiplication instead of division)
	float gatedPos = pos * p.invGateRatio;

	// Envelope calculation
	float envelope;
	if (p.useEdgeMode) {
		if (gatedPos < p.edgeSize) {
			float t = gatedPos * p.invEdgeSize;
			envelope = (p.envShape > 0.001f) ? t * t : 1.0f;
		}
		else if (gatedPos > (1.0f - p.edgeSize)) {
			float t = (gatedPos - (1.0f - p.edgeSize)) * p.invEdgeSize;
			envelope = (p.envShape < 0.999f) ? (1.0f - t) * (1.0f - t) : 1.0f;
		}
		else {
			envelope = 1.0f;
		}
	}
	else {
		if (p.envShape < 0.001f) {
			envelope = (1.0f - gatedPos) * (1.0f - gatedPos);
		}
		else if (p.envShape > 0.999f) {
			envelope = gatedPos * gatedPos;
		}
		else {
			if (gatedPos < p.envShape) {
				float t = gatedPos * p.invEnvShape;
				envelope = t * t;
			}
			else {
				float t = (gatedPos - p.envShape) * p.invOneMinusEnvShape;
				envelope = (1.0f - t) * (1.0f - t);
			}
		}
	}

	// Combine anti-click with depth-controlled envelope
	float depthEnv = 1.0f + p.depth * (envelope - 1.0f);
	return antiClick * depthEnv;
}

/**
 * Ultra-fast linear-only Q31 grain envelope - minimal per-sample cost
 * Only computes linear anti-click fades, no parabolic curves or depth blending.
 * Use this when caller has already determined we're in a fade region.
 *
 * @param positionInSlice Current position within slice [0, sliceLength)
 * @param p Precomputed Q31 parameters from prepareGrainEnvelopeQ31()
 * @return Amplitude multiplier in Q31 format [0, ONE_Q31]
 */
[[gnu::always_inline]] inline int32_t grainEnvelopeLinearQ31(int32_t positionInSlice, const GrainEnvPrecomputedQ31& p) {
	// Asymmetric fade: attackFadeLen for fade-in, decayFadeLen for fade-out
	// envShape=0: short attack, long decay (percussive)
	// envShape=1: long attack, short decay (reversed)

	// Fade in region (uses attackFadeLen)
	if (positionInSlice < p.attackFadeLen) {
		return positionInSlice * p.invAttackLen;
	}
	// Fade out region (uses decayFadeLen)
	if (positionInSlice > p.gatedLength - p.decayFadeLen) {
		int32_t remaining = p.gatedLength - positionInSlice;
		return (remaining > 0) ? (remaining * p.invDecayLen) : 0;
	}
	// Flat middle
	return ONE_Q31;
}

/**
 * Pure Q31 fixed-point grain envelope - zero float operations per sample
 * Uses only integer math: comparisons, additions, subtractions, and multiply_32x32_rshift32
 *
 * @param positionInSlice Current position within slice [0, sliceLength)
 * @param p Precomputed Q31 parameters from prepareGrainEnvelopeQ31()
 * @return Amplitude multiplier in Q31 format [0, ONE_Q31]
 */
[[gnu::always_inline]] inline int32_t grainEnvelopeQ31(int32_t positionInSlice, const GrainEnvPrecomputedQ31& p) {
	// Early out if no valid slice
	if (p.invSliceLength == 0) {
		return ONE_Q31;
	}

	// Anti-click fade in Q31
	// t = pos * invFadeLen gives Q31 result directly since pos is int and invFadeLen is Q31/len
	int32_t antiClickQ31 = ONE_Q31;
	if (!p.useShortFade) {
		if (positionInSlice < p.fadeLen) {
			// antiClick = position / fadeLen = position * invFadeLen
			antiClickQ31 = positionInSlice * p.invFadeLen;
		}
		else if (positionInSlice > p.gatedLength - p.fadeLen) {
			antiClickQ31 = (p.gatedLength - positionInSlice) * p.invFadeLen;
		}
	}
	else if (p.fadeLen > 0) {
		if (positionInSlice < p.fadeLen) {
			antiClickQ31 = positionInSlice * p.invFadeLen;
		}
		else if (positionInSlice > p.gatedLength - p.fadeLen) {
			antiClickQ31 = (p.gatedLength - positionInSlice) * p.invFadeLen;
		}
	}

	// Normalized position in Q31: pos = positionInSlice * invSliceLength
	int32_t posQ31 = positionInSlice * p.invSliceLength;

	// Gate threshold check (Q31 comparison)
	if (posQ31 > p.gateRatioQ31) {
		return 0;
	}

	// Gated position in Q31 - need to rescale [0, gateRatio] to [0, 1]
	// gatedPos = pos / gateRatio = pos * invGateRatio, but both are Q31, so multiply_32x32_rshift32
	int32_t gatedPosQ31 = multiply_32x32_rshift32(posQ31, p.invGateRatio) << 1;

	// Envelope calculation in Q31
	// For t^2 in Q31: multiply_32x32_rshift32(t, t) << 1 gives Q31 result
	int32_t envelopeQ31;
	if (p.useEdgeMode) {
		if (gatedPosQ31 < p.edgeSizeQ31) {
			// t = gatedPos / edgeSize
			int32_t tQ31 = multiply_32x32_rshift32(gatedPosQ31, p.invEdgeSize) << 1;
			// envelope = t^2
			envelopeQ31 = (p.envShapeQ31 > (ONE_Q31 / 1000)) ? (multiply_32x32_rshift32(tQ31, tQ31) << 1) : ONE_Q31;
		}
		else if (gatedPosQ31 > (ONE_Q31 - p.edgeSizeQ31)) {
			// t = (gatedPos - (1 - edgeSize)) / edgeSize
			int32_t tQ31 = multiply_32x32_rshift32(gatedPosQ31 - (ONE_Q31 - p.edgeSizeQ31), p.invEdgeSize) << 1;
			// envelope = (1-t)^2
			int32_t oneMinusT = ONE_Q31 - tQ31;
			envelopeQ31 = (p.envShapeQ31 < (ONE_Q31 - ONE_Q31 / 1000))
			                  ? (multiply_32x32_rshift32(oneMinusT, oneMinusT) << 1)
			                  : ONE_Q31;
		}
		else {
			envelopeQ31 = ONE_Q31;
		}
	}
	else {
		if (p.envShapeQ31 < (ONE_Q31 / 1000)) {
			// Fade-out only: (1-gatedPos)^2
			int32_t oneMinusPos = ONE_Q31 - gatedPosQ31;
			envelopeQ31 = multiply_32x32_rshift32(oneMinusPos, oneMinusPos) << 1;
		}
		else if (p.envShapeQ31 > (ONE_Q31 - ONE_Q31 / 1000)) {
			// Fade-in only: gatedPos^2
			envelopeQ31 = multiply_32x32_rshift32(gatedPosQ31, gatedPosQ31) << 1;
		}
		else {
			if (gatedPosQ31 < p.envShapeQ31) {
				// Attack phase: t = gatedPos / envShape, envelope = t^2
				int32_t tQ31 = multiply_32x32_rshift32(gatedPosQ31, p.invEnvShape) << 1;
				envelopeQ31 = multiply_32x32_rshift32(tQ31, tQ31) << 1;
			}
			else {
				// Decay phase: t = (gatedPos - envShape) / (1 - envShape), envelope = (1-t)^2
				int32_t tQ31 = multiply_32x32_rshift32(gatedPosQ31 - p.envShapeQ31, p.invOneMinusEnvShape) << 1;
				int32_t oneMinusT = ONE_Q31 - tQ31;
				envelopeQ31 = multiply_32x32_rshift32(oneMinusT, oneMinusT) << 1;
			}
		}
	}

	// Combine: result = (1 - depth) + depth * envelope
	// In Q31: result = (ONE_Q31 - depth) + multiply(depth, envelope)
	int32_t depthEnvQ31 = (ONE_Q31 - p.depthQ31) + (multiply_32x32_rshift32(p.depthQ31, envelopeQ31) << 1);

	// Final: antiClick * depthEnv
	return multiply_32x32_rshift32(antiClickQ31, depthEnvQ31) << 1;
}

/**
 * Scatter control parameters from menu/modulation
 * Zone params are unsigned q31 [0, ONE_Q31] from PatchedParamSet/UnpatchedParamSet
 */
struct ScatterParams {
	q31_t zoneA{0}; ///< Zone A control (meaning depends on mode)
	q31_t zoneB{0}; ///< Zone B control (meaning depends on mode)
	q31_t depth{0}; ///< Effect depth/intensity
	q31_t gate{0};  ///< Gate duty cycle

	/// Get zone info for Zone A using standard zone helpers
	[[gnu::always_inline]] ZoneInfo getZoneAInfo(int32_t numZones = 8) const { return computeZoneQ31(zoneA, numZones); }

	/// Get zone info for Zone B using standard zone helpers
	[[gnu::always_inline]] ZoneInfo getZoneBInfo(int32_t numZones = 8) const { return computeZoneQ31(zoneB, numZones); }

	/// Get depth as normalized [0,1]
	[[gnu::always_inline]] float depthNormalized() const { return static_cast<float>(depth) * kQ31ToFloat; }

	/// Get gate as normalized [0,1]
	[[gnu::always_inline]] float gateNormalized() const { return static_cast<float>(gate) * kQ31ToFloat; }
};

/**
 * State for scatter DSP processing
 */
struct ScatterState {
	double phiPhase{0.0};       ///< Phase accumulator for phi triangle evolution
	int32_t currentSlice{0};    ///< Current slice index in playback sequence
	int32_t targetSlice{0};     ///< Target slice (remapped) for scatter playback
	int32_t positionInSlice{0}; ///< Current sample position within current slice
	int32_t sliceLength{0};     ///< Length of each slice in samples
	int32_t numSlices{8};       ///< Number of slices to divide buffer into
	int32_t bufferLength{0};    ///< Total buffer length for dynamic slice updates
	float tapeSpeed{1.0f};      ///< Current tape speed for Tape mode
	float pitchRatio{1.0f};     ///< Current pitch ratio for Pitch mode
	float filterFreq{4000.0f};  ///< Current filter frequency for Filter mode
	int32_t repeatCount{0};     ///< Current repeat count for Repeat mode
	bool sliceReversed{false};  ///< Whether current slice is playing reversed
	float gatePosition{0.0f};   ///< Current position within gate cycle
	float gateRatio{1.0f};      ///< Current gate duty cycle
	bool initialized{false};    ///< Whether scatter state has been initialized for current buffer

	/// Reset state for new stutter session
	void reset() {
		phiPhase = 0.0;
		currentSlice = 0;
		targetSlice = 0;
		positionInSlice = 0;
		sliceLength = 0;
		numSlices = 8;
		bufferLength = 0;
		tapeSpeed = 1.0f;
		pitchRatio = 1.0f;
		filterFreq = 4000.0f;
		repeatCount = 0;
		sliceReversed = false;
		gatePosition = 0.0f;
		gateRatio = 1.0f;
		initialized = false;
	}

	/// Initialize slice parameters based on buffer size
	void initSlices(int32_t bufferSize, int32_t sliceCount) {
		numSlices = std::clamp(sliceCount, int32_t{2}, int32_t{16});
		sliceLength = bufferSize / numSlices;
		positionInSlice = 0;
		currentSlice = 0;
		targetSlice = 0;
		bufferLength = bufferSize;
		initialized = true;
	}

	/// Update slice count dynamically during playback
	/// Preserves relative position within buffer when slice count changes
	/// @param newSliceCount Desired number of slices
	/// @return true if slice count changed
	bool updateSliceCount(int32_t newSliceCount) {
		newSliceCount = std::clamp(newSliceCount, int32_t{2}, int32_t{16});
		if (newSliceCount == numSlices || bufferLength <= 0) {
			return false;
		}

		// Calculate current absolute position in buffer
		int32_t absPos = targetSlice * sliceLength + positionInSlice;

		// Update slice parameters
		numSlices = newSliceCount;
		sliceLength = bufferLength / numSlices;

		// Recalculate slice and position from absolute position
		if (sliceLength > 0) {
			targetSlice = (absPos / sliceLength) % numSlices;
			positionInSlice = absPos % sliceLength;
			currentSlice = targetSlice;
		}
		return true;
	}

	/// Advance position within slice, returns true if slice boundary crossed
	bool advancePosition() {
		positionInSlice++;
		if (positionInSlice >= sliceLength) {
			positionInSlice = 0;
			currentSlice = (currentSlice + 1) % numSlices;
			return true; // Slice boundary crossed
		}
		return false;
	}

	/// Get buffer offset for current scatter position (targetSlice + positionInSlice)
	[[gnu::always_inline]] int32_t getBufferOffset() const { return targetSlice * sliceLength + positionInSlice; }

	/// Advance phi phase for quasi-periodic evolution
	void advancePhase(float rate = 0.001f) { phiPhase += rate; }
};

/**
 * Computed grain parameters from zone knobs
 * All values normalized [0,1] unless noted
 */
struct GrainParams {
	// Structural (from Zone A)
	float sliceOffset{0};   ///< Offset to add to slice selection [0,1] maps to [0,numSlices)
	float lengthMult{1.0f}; ///< Slice length multiplier [0.25, 2.0]
	float skipProb{0};      ///< Probability of skipping to non-adjacent slice
	float dryMix{0};        ///< Crossfade with dry input [0,1]: 0=full grain, 1=full dry

	// Timbral (from Zone B)
	float reverseProb{0};   ///< Probability of reversing slice
	float filterFreq{0.5f}; ///< Bandpass center [0,1] maps to freq range
	float delayFeed{0};     ///< Per-grain delay send amount
	float envShape{0.5f};   ///< Envelope shape (0=percussive, 0.5=hanning, 1=reverse)
	float envDepth{0};      ///< Envelope depth [0,1]: 0=hard cut, 1=full envelope
	float panAmount{0};     ///< Crossfeed pan amount [0,1] before direction applied

	// Combined
	float gateRatio{1.0f};   ///< Gate duty cycle [0.125, 1.0]
	int32_t subdivisions{1}; ///< Ratchet subdivisions (1,2,3,4,6,8,12) - play slice start N times
};

/**
 * Phase offsets from secret encoder menus (push+twist on zone knobs)
 * These shift the effective zone position and scale phi evolution
 */
struct ScatterPhaseOffsets {
	float zoneA{0};       ///< Zone A structural phase offset
	float zoneB{0};       ///< Zone B timbral phase offset
	float macroConfig{0}; ///< Macro config phase offset
	float gamma{0};       ///< Gamma multiplier for phi evolution (100x scale)
};

/**
 * Compute grain parameters from zone knobs via phi triangles
 *
 * When phaseOffset == 0 (standard mode):
 *   Zone A (Structural): Controls grain selection, length, skip patterns
 *     Zones 0-4: Individual behaviors with position controlling intensity
 *     Zones 5-7: Meta - all structural params via phi evolution (uses kStructuralBank)
 *   Zone B (Timbral): Controls per-grain effects
 *     Zones 0-3: Individual effects (reverse, filter, delay, envelope)
 *     Zones 4-7: Meta - all timbral params via phi evolution (uses kTimbraBank)
 *
 * When phaseOffset != 0 (full evolution mode, like sine shaper):
 *   Ignores discrete zones entirely - applies phi triangles to ALL params
 *   Position (knob) controls intensity, phaseOffset controls pattern selection
 *   Different phi frequencies per parameter for non-monotonic evolution
 *
 * @param zoneAParam Zone A raw q31 param value [0, ONE_Q31]
 * @param zoneBParam Zone B raw q31 param value [0, ONE_Q31]
 * @param macroConfigParam Macro config raw q31 param value [0, ONE_Q31]
 * @param macroParam Macro raw q31 param value [0, ONE_Q31]
 * @param sliceIndex Current slice index (converted to phi-based phase internally)
 * @param offsets Phase offsets from secret encoder menus (optional)
 */
inline GrainParams computeGrainParams(q31_t zoneAParam, q31_t zoneBParam, q31_t macroConfigParam, q31_t macroParam,
                                      int32_t sliceIndex, const ScatterPhaseOffsets& offsets = {}) {
	GrainParams p;

	constexpr int32_t kNumZones = 8;
	constexpr double kResolution = 1024.0; // Matches UI kScatterResolution for non-overlapping phase patterns

	// Compute effective phase offsets (individual offset + resolution * gammaPhase)
	// This matches sine shaper: phaseOffset + 1024.0 * gammaPhase
	// Resolution (1024) ensures gamma sweeps through distinct non-repeating patterns
	double phRawA = static_cast<double>(offsets.zoneA) + kResolution * static_cast<double>(offsets.gamma);
	double phRawB = static_cast<double>(offsets.zoneB) + kResolution * static_cast<double>(offsets.gamma);

	// Apply macroConfig offset (in normalized units, 0.1 per click)
	float macroConfigNorm = static_cast<float>(macroConfigParam) * kQ31ToFloat;
	macroConfigNorm = std::clamp(macroConfigNorm + offsets.macroConfig * 0.1f, 0.0f, 1.0f);
	float macroNorm = static_cast<float>(macroParam) * kQ31ToFloat;

	// Phi triangle deadzone: when triangle output is low, sliceIndex contribution is zeroed
	// This creates sparse activation - many consecutive slices get identical params → cache hits
	float sliceWeight = triangleSimpleUnipolar(phi::wrapPhase(static_cast<float>(sliceIndex) * phi::kPhiN050), 0.5f);
	int32_t effectiveSlice = (sliceWeight > 0.1f) ? sliceIndex : 0;
	float slicePhase = phi::wrapPhase(static_cast<float>(effectiveSlice) * phi::kPhi);

	// === Zone A: Structural ===
	if (phRawA != 0.0) {
		// Full range phi-triangle evolution (like sine shaper when phRaw != 0)
		// Position is normalized across full range, phi triangles evolve all structural params
		// slicePhase adds per-grain variation (unlike sine shaper which is continuous)
		float pos = static_cast<float>(zoneAParam) * kQ31ToFloat;
		pos = std::clamp(pos, 0.0f, 1.0f);

		// Per-effect frequency modulation using phi triangles (non-monotonic)
		float fmO = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawA * phi::kPhi025));
		float fmL = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawA * phi::kPhi033));
		float fmS = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawA * phi::kPhi067));
		float fmD = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawA * phi::kPhiN025));

		// Scale and wrap ph per-frequency to preserve irrational divergence
		float ph025 = phi::wrapPhase(phRawA * phi::kPhi025);
		float ph033 = phi::wrapPhase(phRawA * phi::kPhi033);
		float ph050 = phi::wrapPhase(phRawA * phi::kPhi050);
		float ph067 = phi::wrapPhase(phRawA * phi::kPhi067);

		// Slice offset: primary evolution + per-slice variation
		p.sliceOffset = triangleSimpleUnipolar(pos * phi::kPhi025 * fmO + ph025 + slicePhase + 0.166f, 0.7f);

		// Length mult: [0.5, 1.0] range
		float lS = triangleSimpleUnipolar(pos * phi::kPhi033 * fmL + ph033 + slicePhase + 0.333f, 0.6f);
		p.lengthMult = 0.5f + lS * 0.5f;

		// Skip prob: capped at 80%
		p.skipProb = triangleSimpleUnipolar(pos * phi::kPhi050 * fmS + ph050 + slicePhase + 0.500f, 0.5f) * 0.8f;

		// Dry mix: sparse activation (low duty = mostly wet with occasional dry)
		p.dryMix = triangleSimpleUnipolar(pos * phi::kPhi067 * fmD + ph067 + slicePhase + 0.667f, 0.3f);
	}
	else {
		// Standard discrete zone behavior (phaseOffset == 0)
		ZoneInfo zoneAInfo = computeZoneQ31(zoneAParam, kNumZones);

		// PhiTriContext: slicePhase for per-slice variation, offsets.gamma shifts evolution pattern
		phi::PhiTriContext ctx{slicePhase, 1.0f, 1.0f, offsets.gamma};
		constexpr int32_t kZoneADiscreteZones = 5; // Zones 0-4 are discrete, 5-7 are meta

		if (zoneAInfo.index < kZoneADiscreteZones) {
			// Zones 0-4: Discrete behaviors
			p.dryMix = 0.0f;

			switch (zoneAInfo.index) {
			case 0: // Drift: Sequential with slight offset
				p.sliceOffset = zoneAInfo.position * 0.25f;
				break;
			case 1: // Swap: Adjacent pair swapping
				p.sliceOffset = (zoneAInfo.position > 0.5f) ? 0.5f : 0.0f;
				p.skipProb = zoneAInfo.position * 0.5f;
				break;
			case 2: // Retro: Reverse order tendency
				p.sliceOffset = zoneAInfo.position * 0.5f;
				p.lengthMult = 1.0f - zoneAInfo.position * 0.5f;
				break;
			case 3: // Leap: Interleaved skipping
				p.sliceOffset = zoneAInfo.position * 0.5f;
				p.skipProb = zoneAInfo.position;
				break;
			case 4: { // Density: Per-grain dry crossfade
				float phiMod = triangleSimpleUnipolar(slicePhase, 0.3f);
				float maxDry = 1.0f - zoneAInfo.position;
				p.dryMix = maxDry * phiMod;
				break;
			}
			default:
				break;
			}
		}
		else {
			// Zones 5-7: Meta - phi triangle evolution via bank
			auto structural = ctx.evalBank(kStructuralBank, zoneAInfo.position);
			p.sliceOffset = structural[0];
			p.lengthMult = 0.5f + structural[1] * 0.5f;
			p.skipProb = structural[2] * 0.8f;
			p.dryMix = 0.0f;
		}
	}

	// === Zone B: Timbral ===
	if (phRawB != 0.0) {
		// Full range phi-triangle evolution (like sine shaper when phRaw != 0)
		// slicePhase adds per-grain variation
		float pos = static_cast<float>(zoneBParam) * kQ31ToFloat;
		pos = std::clamp(pos, 0.0f, 1.0f);

		// Per-effect frequency modulation
		float fmR = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawB * phi::kPhiN050));
		float fmF = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawB * phi::kPhi067));
		float fmD = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawB * phi::kPhi125));
		float fmE = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawB * phi::kPhi200));

		// Scale and wrap ph per-frequency
		float phN050 = phi::wrapPhase(phRawB * phi::kPhiN050);
		float ph067 = phi::wrapPhase(phRawB * phi::kPhi067);
		float ph125 = phi::wrapPhase(phRawB * phi::kPhi125);
		float ph200 = phi::wrapPhase(phRawB * phi::kPhi200);

		// Reverse prob + per-slice variation
		p.reverseProb = triangleSimpleUnipolar(pos * phi::kPhiN050 * fmR + phN050 + slicePhase + 0.000f, 0.5f);

		// Filter freq
		p.filterFreq = triangleSimpleUnipolar(pos * phi::kPhi067 * fmF + ph067 + slicePhase + 0.250f, 0.7f);

		// Delay feed: capped at 80%
		p.delayFeed = triangleSimpleUnipolar(pos * phi::kPhi125 * fmD + ph125 + slicePhase + 0.500f, 0.6f) * 0.8f;

		// Envelope shape
		p.envShape = triangleSimpleUnipolar(pos * phi::kPhi200 * fmE + ph200 + slicePhase + 0.750f, 0.8f);

		// Envelope depth: separate phi frequency for independent evolution
		float phDepth = phi::wrapPhase(phRawB * phi::kPhi050);
		float fmDepth = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawB * phi::kPhi050));
		p.envDepth = triangleSimpleUnipolar(pos * phi::kPhi050 * fmDepth + phDepth + slicePhase, 0.6f);

		// Pan amount: yet another phi frequency
		float phPan = phi::wrapPhase(phRawB * phi::kPhi125);
		p.panAmount = triangleSimpleUnipolar(pos * phi::kPhi125 + phPan + slicePhase, 0.25f);
	}
	else {
		// Standard discrete zone behavior (phaseOffset == 0)
		ZoneInfo zoneBInfo = computeZoneQ31(zoneBParam, kNumZones);

		// PhiTriContext: slicePhase for per-slice variation, offsets.gamma shifts evolution pattern
		phi::PhiTriContext ctx{slicePhase, 1.0f, 1.0f, offsets.gamma};
		constexpr int32_t kZoneBDiscreteZones = 4; // Zones 0-3 are discrete, 4-7 are meta

		if (zoneBInfo.index < kZoneBDiscreteZones) {
			// Zones 0-3: Individual effects
			switch (zoneBInfo.index) {
			case 0: // Flip: Reverse probability
				p.reverseProb = zoneBInfo.position;
				break;
			case 1: // Filter: Bandpass sweep
				p.filterFreq = zoneBInfo.position;
				break;
			case 2: // Echo: Delay feed
				p.delayFeed = zoneBInfo.position * 0.8f;
				break;
			case 3: // Shape: Envelope shape
			default:
				p.envShape = zoneBInfo.position;
				break;
			}
		}
		else {
			// Zones 4-7: Meta - phi triangle evolution via bank
			auto timbral = ctx.evalBank(kTimbraBank, zoneBInfo.position);
			p.reverseProb = timbral[0];
			p.filterFreq = timbral[1];
			p.delayFeed = timbral[2] * 0.8f;
			p.envShape = timbral[3];
		}

		// Envelope depth and pan from Zone B position (standard mode)
		p.envDepth = triangleSimpleUnipolar(zoneBInfo.position * phi::kPhi050, 0.6f);
		p.panAmount = triangleSimpleUnipolar(zoneBInfo.position * phi::kPhi125, 0.25f);

		// Gate from macroConfig only (standard mode)
		// Range [0.25, 1.0] - macroConfig high = shorter gate
		p.gateRatio = 0.25f + (1.0f - macroConfigNorm) * 0.75f;
	}

	// In full evolution mode, Zone B modulates gate via phi triangle
	if (phRawB != 0.0) {
		float pos = static_cast<float>(zoneBParam) * kQ31ToFloat;
		pos = std::clamp(pos, 0.0f, 1.0f);
		// Gate phi triangle with 50% deadzone - half the time gate is full
		float phGate = phi::wrapPhase(phRawB * phi::kPhi150);
		float gateRaw = triangleSimpleUnipolar(pos * phi::kPhi150 + phGate + slicePhase, 0.5f);
		// Range [0.125, 1.0] - more dramatic gating when active
		p.gateRatio = 0.125f + (1.0f - gateRaw) * 0.875f;
	}

	// === Subdivisions (Ratchet) ===
	// Two triangles determine base subdivision pattern
	// macro=0 → no subdivisions (=1), macro=max → 2x base subdivisions
	// macroConfig triangle gates macro's influence (standard pattern)
	float zoneANorm = static_cast<float>(zoneAParam) * kQ31ToFloat;
	float subdivInfluence = triangleSimpleUnipolar(macroConfigNorm * phi::kPhi225, 0.5f);
	float subdivMix = macroNorm * subdivInfluence; // 0 to 1

	// Binary: 80% deadzone, magnitude → 2/4/8
	float binaryRaw = triangleSimpleUnipolar(zoneANorm * phi::kPhi150 + slicePhase, 0.8f);
	int32_t binarySub = (binaryRaw > 0.66f) ? 8 : (binaryRaw > 0.33f) ? 4 : (binaryRaw > 0.001f) ? 2 : 1;

	// Triplet: 90% deadzone (sparser), magnitude → 3/6
	float tripletRaw = triangleSimpleUnipolar(zoneANorm * phi::kPhi200 + slicePhase * phi::kPhi, 0.9f);
	int32_t tripletSub = (tripletRaw > 0.5f) ? 6 : (tripletRaw > 0.001f) ? 3 : 1;

	// Combine base subdivisions (multiply when both active, cap at 12)
	int32_t baseSub;
	if (tripletSub > 1 && binarySub > 1) {
		baseSub = std::min<int32_t>(binarySub * tripletSub, 12);
	}
	else {
		baseSub = (tripletSub > 1) ? tripletSub : binarySub;
	}

	// macro scales from 1 (no subdiv) to baseSub*2 (double), capped at 12
	int32_t targetSub = std::min<int32_t>(baseSub * 2, 12);
	p.subdivisions = 1 + static_cast<int32_t>(static_cast<float>(targetSub - 1) * subdivMix);

	return p;
}

} // namespace deluge::dsp::scatter
