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
 *
 * TODO: Disperser improvements
 *
 * == HIGH PRIORITY: Topo Zone Enhancement == [DONE]
 * Each topo zone now has per-zone phi triangle patterns for:
 * - [x] Detuning: per-stage frequency offset (chorus shimmer, subtle beating)
 * - [x] Spectral Emphasis: per-stage gain tilt for low/high frequency emphasis
 * - [x] Harmonic Blend: f/2f/4f delay tap balance (timbral richness evolution)
 * These evolve with the zone's existing phi triangles (param0/1/2) for organic texture.
 *
 * == Other Improvements ==
 * - [ ] Phi phases: write gain curve (low/high freq emphasis)
 * - [ ] Feedback tuning: punch/chirp levels may need adjustment
 * - [ ] Performance: move expensive float math (std::pow) out of hot paths
 * - [ ] Consolidate processing paths (currently scattered across iterations)
 * - [ ] Consider fractional delay interpolation for smoother pitch sweeping
 */

#pragma once

#include "definitions_cxx.hpp"
#include "dsp/filter/ladder_components.h"
#include "dsp/phi_triangle.hpp"
#include "io/debug/fx_benchmark.h"
#include "util/fixedpoint.h"
#include <arm_neon.h>
#include <array>
#include <cmath>

namespace deluge::dsp {

/// Saturating add for q31_t using NEON
[[gnu::always_inline]] inline q31_t q31_sat_add(q31_t a, q31_t b) {
	return vget_lane_s32(vqadd_s32(vdup_n_s32(a), vdup_n_s32(b)), 0);
}

// Forward declarations
struct DisperserTopoParams;
struct DisperserTwistParams;

/**
 * Number of zones for disperser knobs
 * 8 zones each for Topo (topology) and Twist (character)
 */
constexpr int32_t kDisperserNumZones = 8;

/**
 * Disperser zone-based parameters
 *
 * Following sine shaper's pattern:
 * - Topo: Discrete signal routing topologies (clips to zone boundaries)
 * - Twist: Character modifiers that apply on top of any topology
 * - Secret phases: Push+twist encoder offsets for meta-modulation
 */
/// Delay line state for comb resonance (shared across topologies)
/// Supports multi-offset writes for frequency-dispersed feedback
struct DisperserDelayState {
	static constexpr size_t kMaxDelaySamples = 8820; // 200ms at 44.1kHz (fine pitch resolution)
	std::array<q31_t, kMaxDelaySamples> bufferL{};
	std::array<q31_t, kMaxDelaySamples> bufferR{};
	size_t headPos{0}; // Current head position (most recent)

	void reset() {
		bufferL.fill(0);
		bufferR.fill(0);
		headPos = 0;
	}

	/// Read from head (most recent sample)
	[[gnu::always_inline]] inline void readHead(q31_t& outL, q31_t& outR) const {
		outL = bufferL[headPos];
		outR = bufferR[headPos];
	}

	/// Read from delay buffer at given offset from head
	[[gnu::always_inline]] inline void read(size_t delaySamples, q31_t& outL, q31_t& outR) const {
		size_t readPos = (headPos + kMaxDelaySamples - delaySamples) % kMaxDelaySamples;
		outL = bufferL[readPos];
		outR = bufferR[readPos];
	}

	/// Add at offset from head (for frequency-dispersed multi-tap writes)
	/// Additive so multiple stages accumulate, with saturation to prevent blowup
	[[gnu::always_inline]] inline void writeAtOffset(q31_t inL, q31_t inR, size_t offset) {
		size_t pos = (headPos + offset) % kMaxDelaySamples;
		// Saturating add with extra headroom check to prevent runaway
		q31_t newL = q31_sat_add(bufferL[pos], inL);
		q31_t newR = q31_sat_add(bufferR[pos], inR);
		// Soft limit at ±0.5 to leave headroom for multiple writes
		bufferL[pos] = signed_saturate<30>(newL);
		bufferR[pos] = signed_saturate<30>(newR);
	}

	/// Fractional write - distributes sample between two adjacent positions
	/// Eliminates stepping when delay time changes smoothly
	[[gnu::always_inline]] inline void writeAtOffsetFractional(q31_t inL, q31_t inR, float offset) {
		size_t offsetInt = static_cast<size_t>(offset);
		float frac = offset - static_cast<float>(offsetInt);

		size_t pos0 = (headPos + offsetInt) % kMaxDelaySamples;
		size_t pos1 = (headPos + offsetInt + 1) % kMaxDelaySamples;

		// Distribute energy: (1-frac) to pos0, frac to pos1
		q31_t gain0 = static_cast<q31_t>((1.0f - frac) * ONE_Q31);
		q31_t gain1 = static_cast<q31_t>(frac * ONE_Q31);

		q31_t inL0 = multiply_32x32_rshift32(inL, gain0) << 1;
		q31_t inL1 = multiply_32x32_rshift32(inL, gain1) << 1;
		q31_t inR0 = multiply_32x32_rshift32(inR, gain0) << 1;
		q31_t inR1 = multiply_32x32_rshift32(inR, gain1) << 1;

		// Add to both positions with saturation
		bufferL[pos0] = signed_saturate<30>(q31_sat_add(bufferL[pos0], inL0));
		bufferL[pos1] = signed_saturate<30>(q31_sat_add(bufferL[pos1], inL1));
		bufferR[pos0] = signed_saturate<30>(q31_sat_add(bufferR[pos0], inR0));
		bufferR[pos1] = signed_saturate<30>(q31_sat_add(bufferR[pos1], inR1));
	}

	/// Clear head position (call after reading, before additive writes)
	[[gnu::always_inline]] inline void clearHead() {
		bufferL[headPos] = 0;
		bufferR[headPos] = 0;
	}

	/// Advance head position (call once per sample after all writes)
	[[gnu::always_inline]] inline void advanceHead() { headPos = (headPos + 1) % kMaxDelaySamples; }

	/// Legacy: write at head and advance (for simple single-write case)
	[[gnu::always_inline]] inline void write(q31_t inL, q31_t inR) {
		writeAtOffset(inL, inR, 1); // Write one ahead of current head
		advanceHead();
	}
};

struct DisperserParams {
	// Secret knob phases (unbounded, accessed via push+twist on encoders)
	phi::SecretPhases phases;

	// DSP smoothing state (per-sound, persists across buffers)
	q31_t smoothedTopo{0};
	q31_t smoothedTwist{0};
	q31_t smoothedFeedback{0}; // For feedback interpolation in processBuffer

	// Zone tracking for filter state reset on zone boundary crossing
	int32_t lastTopoZone{-1}; // -1 = uninitialized

	// Shared delay line for comb resonance (all topologies can use)
	DisperserDelayState delay;
};

/**
 * Topology-derived parameters (computed per-buffer from topo zone + position)
 *
 * Each topology uses a subset of these. The topology dispatch function
 * interprets them according to the current zone.
 */
struct DisperserTopoParams {
	int32_t zone{0}; // Current topology zone (0-7)

	// Q factor for biquad allpasses (0.5-10, higher = sharper phase rotation)
	// Zone 0 (Cascade) maps position to Q for the classic kHz Disperser "Pinch" control
	float q{2.0f};

	// Common params used across topologies (meaning varies by zone)
	float param0{0}; // Primary: spread curve / alternation depth / cross amount / etc.
	float param1{0}; // Secondary: resonance / L/R offset / asymmetry / etc.
	float param2{0}; // Tertiary: frequency split / damping / octave / etc.

	// L/R differentiation (for stereo topologies)
	float lrOffset{0}; // Phase offset between L and R processing

	// Phi-triangle evolved parameters (per-zone patterns)
	float detuning{0};      // Per-stage freq offset spread (0=none, 1=±50 cents) for chorus shimmer
	float harmonicBlend{0}; // Delay tap balance: 0=f only, 0.5=equal f/2f, 1=2f emphasis
	float emphasis{0};      // Spectral tilt (-1=low freq boost, +1=high freq boost)
	                        // Smooth gain curve across stages (0.7x to 1.3x range)
};

/**
 * Twist (character) derived parameters
 *
 * These modify the disperser character on top of any topology.
 * Zones 0-4: Individual effects, Zones 5-7: Meta (all combined)
 *
 * Zone layout:
 * - Zone 0: Width - stereo spread via L/R frequency offset
 * - Zone 1: Punch - transient emphasis before dispersion (bigger chirps)
 * - Zone 2: Curve - frequency distribution (clustered low → linear → clustered high)
 * - Zone 3: Chirp - transient-triggered delay (chirp echoes)
 * - Zone 4: QTilt - Q varies across stages
 * - Zones 5-7: Meta - all effects combined with φ-triangle evolution
 */
struct DisperserTwistParams {
	// Width: Stereo spread via L/R frequency offset
	float width{0};

	// Punch: Transient emphasis (boost attacks before dispersion)
	// 0 = no boost, 1 = max boost (~12dB on transients)
	float punch{0};

	// Chirp: Feedback amount for chirp echoes (delay time from freq knob)
	// 0 = no feedback, 1 = max feedback (repeating echoes)
	float chirpAmount{0};

	// Spread curve: controls how frequencies are distributed across stages
	// 0 = tight (lower stages clustered, like old behavior)
	// 0.5 = linear (evenly distributed)
	// 1 = tight at high end
	float spreadCurve{0.5f};

	// Q Tilt: varies Q across stages
	// 0 = uniform Q, positive = high-freq stages sharper, negative = low-freq sharper
	float qTilt{0.0f};

	// Prototype: detuning and harmonic blend (will move to topo phi triangles)
	float detuning{0};      // Per-stage freq offset spread (0=none, 1=±50 cents) for chorus shimmer
	float harmonicBlend{0}; // Delay tap balance: 0=f only (default), 0.5=equal f/2f, 1=2f emphasis

	// Legacy fields (kept for meta zone compatibility)
	float delayTime{0};
	float delayMix{0};
	float lowFreqGain{0};
	float highFreqGain{0};
	float resonance{0};
};

/**
 * Compute topology parameters from smoothed topo value
 *
 * Each zone has internal parameters that evolve via φ-triangles.
 * Position within zone + metaPhaseTopo drives the triangles.
 *
 * For detuning/harmonicBlend, an optional twistPhaseOffset can be added
 * to the raw phase, allowing twist meta position to "rotate" through
 * topo's parameter evolution (like sine shaper's phaseHarmonic pattern).
 *
 * @param smoothedTopo Smoothed topo value (q31)
 * @param params Optional - provides metaPhaseTopo for phase offset
 * @param twistPhaseOffset Optional phase offset for detuning/harmonicBlend (from twist meta position)
 * @return Derived topology parameters
 */
DisperserTopoParams computeDisperserTopoParams(q31_t smoothedTopo, const DisperserParams* params = nullptr,
                                               float twistPhaseOffset = 0.0f);

/**
 * Compute twist (character) parameters from smoothed twist value
 *
 * Zones 0-3: Individual effects (Width, Warmth, Drift, Resonance)
 * Zones 4-7: Meta zone combining all with φ-triangle evolution
 *
 * @param smoothedTwist Smoothed twist value (q31)
 * @param params Optional - provides metaPhase/gammaPhase for meta zone
 * @return Derived character parameters
 */
DisperserTwistParams computeDisperserTwistParams(q31_t smoothedTwist, const DisperserParams* params = nullptr);

/**
 * Disperser effect using cascaded 2nd-order biquad allpass filters
 *
 * Creates phase smearing and tonal coloration effects:
 * - Frequency: Center frequency for the allpass cascade (1Hz - 8kHz, 13 octaves)
 * - Spread: 0 = all stages at center (classic disperser), 127 = stages spread ±2 octaves
 * - Q (Pinch): Controls sharpness of phase rotation (0.5 = broad, 10+ = sharp/resonant)
 * - Stages: Number of active 2nd-order allpass stages (0-32)
 *
 * 2nd-order allpasses provide 360° phase shift per stage (vs 180° for 1st-order),
 * and the Q parameter controls how sharply phase rotates around center frequency.
 * This is what gives kHz Disperser its characteristic sound.
 *
 * All parameters except stages are modulatable (q31_t) with per-sample interpolation.
 * Coefficient recalculation happens per-buffer when freq/spread/Q change.
 */
class Disperser {
public:
	static constexpr size_t kMaxStages = 32;
	static constexpr float kDefaultQ = 2.0f; // Moderate Q, good starting point

	Disperser() = default;

	/**
	 * Update coefficients with smoothing (call once per buffer)
	 * @param freq Target center frequency (q31)
	 * @param spread Target frequency spread (q31) - 0=all same, max=±4 octaves
	 * @param smoothedFreq Previous smoothed freq value (updated)
	 * @param smoothedSpread Previous smoothed spread value (updated)
	 * @param lrOffset L/R frequency offset in octaves (0=mono, positive=R higher)
	 * @param q Q factor for biquad allpasses (0.5-10, higher = sharper phase rotation)
	 * @param activeStages Number of stages that will be used (for frequency distribution)
	 * @param spreadCurve Distribution curve (0=tight/clustered, 1=linear/even)
	 * @param qTilt Q variation across stages (-1 to +1, 0=uniform)
	 * @param bimodalSeparation Mode separation in octaves (0=normal, >0=bimodal)
	 * @param detuning Per-stage frequency offset spread (0=none, 1=±50 cents)
	 */
	void updateCoefficientsSmoothed(q31_t freq, q31_t spread, q31_t* smoothedFreq, q31_t* smoothedSpread,
	                                float lrOffset = 0.0f, float q = kDefaultQ, uint8_t activeStages = kMaxStages,
	                                float spreadCurve = 1.0f, float qTilt = 0.0f, float bimodalSeparation = 0.0f,
	                                float detuning = 0.0f, float emphasis = 0.0f) {
		constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31); // ~100ms at 44.1kHz
		*smoothedFreq = *smoothedFreq + (multiply_32x32_rshift32(freq - *smoothedFreq, smoothingAlpha) << 1);
		*smoothedSpread = *smoothedSpread + (multiply_32x32_rshift32(spread - *smoothedSpread, smoothingAlpha) << 1);

		uint32_t freqU = static_cast<uint32_t>(*smoothedFreq) + 0x80000000u;
		uint32_t spreadU = static_cast<uint32_t>(*smoothedSpread) + 0x80000000u;
		updateCoefficients(freqU >> 25, spreadU >> 25, lrOffset, q, activeStages, spreadCurve, qTilt, bimodalSeparation,
		                   detuning, emphasis);
	}

	/**
	 * Update coefficients when parameters change (call per-buffer, not per-sample)
	 *
	 * 2nd-order biquad allpass architecture with independent spread and Q:
	 * - freq: Center frequency for all stages
	 * - spread: Frequency spread across stages (0=all same, 127=±4 octaves)
	 * - lrOffset: Stereo spread via L/R frequency offset
	 * - q: Q factor controlling sharpness of phase rotation (the "Pinch")
	 * - spreadCurve: Distribution curve (0=tight/clustered at low end, 1=linear/even)
	 * - qTilt: Q variation across stages (positive = high-freq sharper)
	 * - bimodalSeparation: Mode separation for bimodal distribution (0=normal, >0=two modes)
	 *
	 * @param freq Center frequency (0-127, maps to 50Hz-8kHz)
	 * @param spread Frequency spread (0-127, 0=all same, 127=±4 octaves)
	 * @param lrOffset L/R frequency offset in octaves (0=mono, positive=R higher than L)
	 * @param q Q factor for biquad allpasses (0.5-10, higher = sharper phase rotation)
	 * @param activeStages Number of stages that will be used (frequencies distributed across these)
	 * @param spreadCurve Distribution curve (0=tight/clustered, 1=linear/even)
	 * @param qTilt Q variation across stages (-1 to +1, 0=uniform)
	 * @param bimodalSeparation Mode separation in octaves (0=normal single mode, >0=two modes)
	 * @param detuning Per-stage frequency offset spread (0=none, 1=±50 cents) for chorus shimmer
	 */
	void updateCoefficients(uint8_t freq, uint8_t spread, float lrOffset = 0.0f, float q = kDefaultQ,
	                        uint8_t activeStages = kMaxStages, float spreadCurve = 1.0f, float qTilt = 0.0f,
	                        float bimodalSeparation = 0.0f, float detuning = 0.0f, float emphasis = 0.0f) {
		// Force recalc if any parameter changed
		bool lrChanged = std::abs(lrOffset - lastLrOffset_) > 0.01f;
		bool qChanged = std::abs(q - lastQ_) > 0.05f;
		bool stagesChanged = activeStages != lastActiveStages_;
		bool curveChanged = std::abs(spreadCurve - lastSpreadCurve_) > 0.02f;
		bool tiltChanged = std::abs(qTilt - lastQTilt_) > 0.02f;
		bool bimodalChanged = std::abs(bimodalSeparation - lastBimodal_) > 0.02f;
		bool detuneChanged = std::abs(detuning - lastDetuning_) > 0.01f;
		bool emphasisChanged = std::abs(emphasis - lastEmphasis_) > 0.02f;
		if (freq == lastFreq_ && spread == lastSpread_ && !lrChanged && !qChanged && !stagesChanged && !curveChanged
		    && !tiltChanged && !bimodalChanged && !detuneChanged && !emphasisChanged) {
			return; // No change, skip recalc
		}
		lastFreq_ = freq;
		lastSpread_ = spread;
		lastLrOffset_ = lrOffset;
		lastQ_ = q;
		lastActiveStages_ = activeStages;
		lastSpreadCurve_ = spreadCurve;
		lastQTilt_ = qTilt;
		lastBimodal_ = bimodalSeparation;
		lastDetuning_ = detuning;
		lastEmphasis_ = emphasis;

		// freq: 0-127 -> 1Hz-8kHz (13 octaves)
		float centerHz = 1.0f * std::pow(2.0f, (freq / 127.0f) * 13.0f);

		// spread: 0-127 -> ±4 octaves (used as local spread around each mode in bimodal)
		float spreadOctaves = (spread / 127.0f) * 4.0f;

		// L/R offset: shift L down, R up by half the offset each
		float lOffsetOct = -lrOffset * 0.5f;
		float rOffsetOct = lrOffset * 0.5f;

		// Base Q clamped to reasonable range (0.5 = very broad, 20 = very sharp/resonant)
		float qBase = std::clamp(q, 0.5f, 20.0f);

		size_t numActive = std::max(static_cast<size_t>(activeStages), size_t{1});

		// Bipolar spread curve: exponent varies from 5.0 (low cluster) to 0.2 (high cluster)
		// spreadCurve=0: exponent=5.0 (stages cluster toward mode center)
		// spreadCurve=0.5: exponent=1.0 (linear distribution within mode)
		// spreadCurve=1: exponent=0.2 (stages spread away from mode center)
		float curveClamped = std::clamp(spreadCurve, 0.0f, 1.0f);
		float exponent = std::pow(5.0f, 1.0f - curveClamped * 2.0f);

		// Q tilt: multiply Q by factor based on stage position
		float qTiltClamped = std::clamp(qTilt, -1.0f, 1.0f);

		// Bimodal mode: stages alternate between two frequency clusters
		bool isBimodal = bimodalSeparation > 0.02f;
		float modeAHz = centerHz; // Mode A center (will be adjusted if bimodal)
		float modeBHz = centerHz; // Mode B center
		if (isBimodal) {
			// Two modes centered geometrically around centerHz
			// separation=1 octave → modeA = center/sqrt(2), modeB = center*sqrt(2)
			float halfSep = bimodalSeparation * 0.5f;
			modeAHz = centerHz / std::pow(2.0f, halfSep);
			modeBHz = centerHz * std::pow(2.0f, halfSep);
		}

		for (size_t i = 0; i < kMaxStages; ++i) {
			float stageHzBase;
			float t; // 0..1 position for Q tilt (global across all stages)

			if (i < numActive) {
				t = static_cast<float>(i) / std::max(1.0f, static_cast<float>(numActive - 1));

				if (isBimodal) {
					// Bimodal: alternate stages between modes A and B
					// Mode A spreads UPWARD from modeAHz (toward Mode B)
					// Mode B spreads DOWNWARD from modeBHz (toward Mode A)
					// This creates two clusters that "reach toward" each other
					bool isModeB = (i % 2 == 1);
					float modeCenter = isModeB ? modeBHz : modeAHz;

					// Count stages in this mode and this stage's position within mode
					size_t stagesInMode = (numActive + (isModeB ? 0 : 1)) / 2;
					size_t indexInMode = i / 2;
					float tLocal =
					    static_cast<float>(indexInMode) / std::max(1.0f, static_cast<float>(stagesInMode - 1));

					// Apply power curve within mode (log-distributed distance from center)
					float curved = std::pow(tLocal, exponent);

					// Mode A: positive offset (upward), Mode B: negative offset (downward)
					// Clamping at 20Hz/16kHz handles boundaries naturally
					float localPosition = isModeB ? -curved : curved;

					// Each mode uses half the spread range
					float localSpread = spreadOctaves * 0.5f;
					stageHzBase = modeCenter * std::pow(2.0f, localPosition * localSpread);
				}
				else {
					// Normal single-mode distribution
					float curved = std::pow(t, exponent);
					float stagePosition = curved * 2.0f - 1.0f;
					stageHzBase = centerHz * std::pow(2.0f, stagePosition * spreadOctaves);
				}
			}
			else {
				stageHzBase = centerHz; // Unused stages
				t = 1.0f;
			}

			// Per-stage Q with tilt
			float qFactor = std::pow(2.0f, qTiltClamped * 2.0f * (t * 2.0f - 1.0f));
			float stageQ = std::clamp(qBase * qFactor, 0.5f, 20.0f);

			// Per-stage detuning: alternating +/- cents for chorus shimmer
			// detuning=1.0 means ±50 cents (≈±0.042 octaves)
			// Pattern: even stages detune up, odd stages detune down
			float detuneOct = 0.0f;
			if (detuning > 0.001f) {
				float maxDetuneCents = 50.0f * detuning;
				float detuneSign = (i & 1) ? -1.0f : 1.0f;
				detuneOct = detuneSign * (maxDetuneCents / 1200.0f); // cents to octaves
			}

			// Apply L/R offset + detuning and clamp to useful range (1Hz floor for subharmonics)
			float stageHzL = std::clamp(stageHzBase * std::pow(2.0f, lOffsetOct + detuneOct), 1.0f, 16000.0f);
			float stageHzR = std::clamp(stageHzBase * std::pow(2.0f, rOffsetOct + detuneOct), 1.0f, 16000.0f);

			// Compute 2nd-order biquad allpass coefficients with per-stage Q
			coeffsL_[i].compute(stageHzL, stageQ, static_cast<float>(kSampleRate));
			coeffsR_[i].compute(stageHzR, stageQ, static_cast<float>(kSampleRate));

			// Calculate delay offset for this stage (based on average frequency)
			// offset = sampleRate / freq → higher freq = shorter offset
			// Triangle fold in OCTAVE space, then quantize to octave of ORIGINAL pitch
			// This preserves user's inharmonic offset while preventing pileup
			constexpr float kMinOffsetF = 441.0f;  // 10ms at 44.1kHz (~100Hz)
			constexpr float kMaxOffsetF = 8819.0f; // 200ms at 44.1kHz (~5Hz)
			constexpr float kMaxOctaves = 4.32f;   // log2(8819/441) ≈ 4.32 octaves range
			float avgHz = (stageHzL + stageHzR) * 0.5f;
			float rawOffset = kSampleRate / avgHz;

			// Triangle fold in octave space to get a TARGET position (guides octave selection)
			float octavesFromMin = std::log2(std::max(rawOffset, 1.0f) / kMinOffsetF);
			if (octavesFromMin < 0.0f || octavesFromMin > kMaxOctaves) {
				float doubled = 2.0f * kMaxOctaves;
				octavesFromMin = std::fmod(std::fmod(octavesFromMin, doubled) + doubled, doubled);
				if (octavesFromMin > kMaxOctaves) {
					octavesFromMin = doubled - octavesFromMin;
				}
			}
			float targetOffset = kMinOffsetF * std::pow(2.0f, octavesFromMin);

			// Quantize to nearest perfect fifth of ORIGINAL pitch (preserves user's offset)
			constexpr float kFifth = 0.5849625f; // log2(3/2)
			float idealShift = std::log2(targetOffset / rawOffset);
			float quantizedShift = std::round(idealShift / kFifth) * kFifth;
			float octaveOffset = rawOffset * std::pow(2.0f, quantizedShift);

			// Ensure result is in range - adjust by one octave if needed
			if (octaveOffset > kMaxOffsetF) {
				octaveOffset *= 0.5f;
			}
			if (octaveOffset < kMinOffsetF) {
				octaveOffset *= 2.0f;
			}
			size_t offset = static_cast<size_t>(std::clamp(octaveOffset, kMinOffsetF, kMaxOffsetF));

			size_t prevOffset = stageOffsets_[i];
			stageOffsets_[i] = std::clamp(offset, size_t{1}, DisperserDelayState::kMaxDelaySamples - 1);
			// Detect large offset change (>40%) for potential buffer clear
			if (prevOffset > 0) {
				float ratio = static_cast<float>(offset) / static_cast<float>(prevOffset);
				if (ratio > 1.4f || ratio < 0.7f) {
					needsBufferClear_ = true;
				}
			}

			// Per-stage emphasis: spectral tilt using dB (normalizes to unity product)
			// emphasis > 0: boost high freq stages, cut low freq stages
			// emphasis < 0: boost low freq stages, cut high freq stages
			// Using log scale so gains multiply to ~1.0 across the cascade
			float emphasisClamped = std::clamp(emphasis, -1.0f, 1.0f);
			float stageGain = 1.0f;
			if (std::abs(emphasisClamped) > 0.01f) {
				// dB tilt centered at 0: t=0.5 gets 0dB, extremes get ±tiltDb
				// Asymmetric: +2.5dB for high emphasis, -2dB for low emphasis
				float tiltDb = (emphasisClamped > 0) ? (emphasisClamped * 2.5f) : (emphasisClamped * 2.0f);
				float stageDb = tiltDb * (t * 2.0f - 1.0f);
				stageGain = std::pow(10.0f, stageDb / 20.0f);
			}
			// Smooth gain transitions to avoid transient volume excursions
			// (especially problematic at high chirp/feedback values)
			q31_t targetGain = static_cast<q31_t>(std::clamp(stageGain, 0.6f, 1.7f) * (ONE_Q31 / 2));
			q31_t currentGain = stageGains_[i];
			// ~6.25% blend per buffer (~50ms settling time at 44.1kHz/128 samples)
			stageGains_[i] = currentGain + ((targetGain - currentGain) >> 4);
		}
	}

	/**
	 * Detect transients using dual envelope followers
	 *
	 * Fast envelope (~1ms attack) tracks peaks
	 * Slow envelope is delay-aware: shorter delay = faster tracking
	 * Transient = fast - slow (simple difference)
	 *
	 * @param inL Left input sample
	 * @param inR Right input sample
	 * @param delaySamples Current delay time (for adaptive slow envelope)
	 * @return Transient level 0-1 (0=sustain, 1=sharp attack)
	 */
	[[gnu::always_inline]] inline float detectTransient(q31_t inL, q31_t inR, size_t delaySamples = 441) {
		// Get absolute value of input (mono sum for detection)
		q31_t absIn = std::abs(inL >> 1) + std::abs(inR >> 1);

		// Fast envelope: ~1ms attack at 44.1kHz
		// Decay ~10ms
		constexpr q31_t kFastDecay = static_cast<q31_t>(0.998 * ONE_Q31);
		if (absIn > fastEnv_[0]) {
			fastEnv_[0] = absIn; // Instant attack
		}
		else {
			fastEnv_[0] = multiply_32x32_rshift32(fastEnv_[0], kFastDecay);
		}

		// Slow envelope: delay-aware time constant
		// Short delay (441 samples/10ms) → fast tracking (τ≈10ms, coeff=0.999)
		// Long delay (8819 samples/200ms) → slow tracking (τ≈200ms, coeff=0.9999)
		constexpr size_t kMinDelay = 441;  // Match folding range lower bound
		constexpr size_t kMaxDelay = 8819; // Match folding range upper bound
		float delayNorm = static_cast<float>(std::clamp(delaySamples, kMinDelay, kMaxDelay) - kMinDelay)
		                  / static_cast<float>(kMaxDelay - kMinDelay);
		float slowCoeffF = 0.999f + delayNorm * 0.0009f; // 0.999 to 0.9999
		q31_t slowCoeff = static_cast<q31_t>(slowCoeffF * ONE_Q31);
		slowEnv_[0] = slowEnv_[0] + multiply_32x32_rshift32(absIn - slowEnv_[0], ONE_Q31 - slowCoeff);

		// Transient = fast - slow, normalized to 0-1
		q31_t diff = fastEnv_[0] - slowEnv_[0];
		if (diff <= 0) {
			return 0.0f;
		}

		// Scale to 0-1 range (diff is q31, so divide by ONE_Q31)
		return std::clamp(static_cast<float>(diff) / static_cast<float>(ONE_Q31), 0.0f, 1.0f);
	}

	/**
	 * Process a stereo buffer through the disperser with parameter interpolation
	 *
	 * Frequency-dispersed feedback: write offsets derived from stage frequencies
	 * - dispersion=0: instant (all stages write at offset 1)
	 * - dispersion=1: natural (frequency-derived offsets)
	 * - dispersion>1: more comb character (scaled offsets)
	 *
	 * Per-stage write gain shapes which stages contribute to feedback:
	 * - lowFreqGain/highFreqGain define gain curve across stages
	 * - Different modes use different curves (e.g., comb emphasizes low-freq)
	 *
	 * @param buffer Stereo audio buffer to process in-place
	 * @param stages Number of stages (0 = bypass)
	 * @param feedback Target feedback amount (q31)
	 * @param smoothedFeedback Previous smoothed feedback (updated)
	 * @param delay Shared delay state
	 * @param dispersion Write offset scale (0=instant, 1=natural, >1=comb)
	 * @param delayMix Feedforward: head to output (flanging)
	 * @param topology Topology zone (0=Cascade, etc.)
	 * @param crossMix Cross-coupling for Cross topology
	 * @param lowFreqGain Write gain for lowest-frequency stages
	 * @param highFreqGain Write gain for highest-frequency stages
	 * @param readGain Safety multiplier on feedback read (dev tuning)
	 */
	void processBuffer(StereoBuffer<q31_t> buffer, uint8_t stages, q31_t feedback, q31_t* smoothedFeedback,
	                   DisperserDelayState& delay, float dispersion = 1.0f, float delayMix = 0.0f, int32_t topology = 0,
	                   float crossMix = 0.0f, float lowFreqGain = 1.0f, float highFreqGain = 1.0f,
	                   float readGain = 0.5f) {
		if (stages == 0 || buffer.empty()) {
			return;
		}

		// Stage count tags for benchmarking (s1, s2, ..., s32)
		static constexpr const char* kStageNames[] = {"s1",  "s2",  "s3",  "s4",  "s5",  "s6",  "s7",  "s8",
		                                              "s9",  "s10", "s11", "s12", "s13", "s14", "s15", "s16",
		                                              "s17", "s18", "s19", "s20", "s21", "s22", "s23", "s24",
		                                              "s25", "s26", "s27", "s28", "s29", "s30", "s31", "s32"};
		FX_BENCH_DECLARE(bench, "disperser");
		FX_BENCH_SET_TAG(bench, 0, kStageNames[std::min(stages, static_cast<uint8_t>(32)) - 1]);
		FX_BENCH_SCOPE(bench);

		constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);
		q31_t targetSmoothed =
		    *smoothedFeedback + (multiply_32x32_rshift32(feedback - *smoothedFeedback, smoothingAlpha) << 1);
		int32_t fbIncrement = (targetSmoothed - *smoothedFeedback) / static_cast<int32_t>(buffer.size());
		q31_t currentFb = *smoothedFeedback;

		// All topologies now use unified feedback through delay buffer
		for (auto& sample : buffer) {
			currentFb += fbIncrement;
			q31_t outL, outR;
			processWithFeedback(sample.l, sample.r, outL, outR, stages, currentFb, delay, topology, dispersion,
			                    delayMix, crossMix, lowFreqGain, highFreqGain);
			sample.l = outL;
			sample.r = outR;
		}

		*smoothedFeedback = targetSmoothed;
	}

	/**
	 * Unified processing with delay buffer feedback for all topologies
	 *
	 * All topologies share common feedback path:
	 * 1. Read from delay head → bandpass filter → add to input
	 * 2. Topology-specific routing through allpass cascade
	 * 3. Write output to delay buffer
	 * 4. Optional feedforward from delay head
	 */
	[[gnu::always_inline]] inline void processWithFeedback(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                       uint8_t stages, q31_t feedback, DisperserDelayState& delay,
	                                                       int32_t topology, float dispersion, float delayMix,
	                                                       float crossMix, float lowFreqGain, float highFreqGain) {

		if (stages == 0) {
			outL = inL;
			outR = inR;
			delay.advanceHead();
			return;
		}

		size_t numStages = std::min(static_cast<size_t>(stages), kMaxStages);

		// === COMMON FEEDBACK READ ===
		q31_t fbGain = feedback;
		q31_t fbL, fbR;
		delay.readHead(fbL, fbR);
		delay.clearHead();

		// Apply feedback gain with hard clamp to prevent runaway
		// signed_saturate<29> allows ~1/4 of full scale
		q31_t fbL_scaled = signed_saturate<29>(multiply_32x32_rshift32(fbL, fbGain) << 1);
		q31_t fbR_scaled = signed_saturate<29>(multiply_32x32_rshift32(fbR, fbGain) << 1);
		q31_t procL = q31_sat_add(inL, fbL_scaled);
		q31_t procR = q31_sat_add(inR, fbR_scaled);

		// === TOPOLOGY-SPECIFIC ROUTING ===
		switch (topology) {
		case 1: // PingPong
			processRoutingPingPong(procL, procR, outL, outR, numStages);
			break;
		case 3: // Cross
			processRoutingCross(procL, procR, outL, outR, numStages, crossMix);
			break;
		case 5: // Nested
			processRoutingNested(procL, procR, outL, outR, numStages);
			break;
		case 7: // Spring
			processRoutingSpring(procL, procR, outL, outR, numStages);
			break;
		default: // 0=Cascade, 2=Stereo, 4=Pitch, 6=Diffuse
			processRoutingCascade(procL, procR, outL, outR, numStages, delay, dispersion, lowFreqGain, highFreqGain);
			break;
		}

		// === COMMON DELAY WRITE (for non-cascade topologies) ===
		// Cascade does per-stage writes; others write final output
		if (topology == 1 || topology == 3 || topology == 5 || topology == 7) {
			// Single write at center frequency offset
			size_t offset = stageOffsets_[numStages / 2];
			if (dispersion < 0.01f) {
				offset = 1; // Instant feedback mode
			}
			// Use average of low/high gain (center freq) × stage normalization
			float avgGain = (lowFreqGain + highFreqGain) * 0.5f;
			float stageNorm = 1.875f / std::pow(static_cast<float>(numStages), 0.75f);
			q31_t writeGain = static_cast<q31_t>(avgGain * stageNorm * ONE_Q31);
			q31_t writeL = multiply_32x32_rshift32(outL, writeGain) << 1;
			q31_t writeR = multiply_32x32_rshift32(outR, writeGain) << 1;
			delay.writeAtOffset(writeL, writeR, offset);
		}

		delay.advanceHead();

		// === COMMON FEEDFORWARD ===
		if (delayMix > 0.001f) {
			q31_t headL, headR;
			delay.readHead(headL, headR);
			q31_t combGain = static_cast<q31_t>(delayMix * 0.9f * ONE_Q31);
			outL = q31_sat_add(outL, multiply_32x32_rshift32(headL, combGain) << 1);
			outR = q31_sat_add(outR, multiply_32x32_rshift32(headR, combGain) << 1);
		}
	}

	/**
	 * Process with Punch and Chirp for maximum chirp character
	 *
	 * Punch: Transient boost on INPUT before allpass chain → bigger chirps
	 * Chirp: Delay-based feedback with transient emphasis → chirp echoes
	 *
	 * @param punch Transient boost amount (0-1, maps to 0-12dB on attacks)
	 * @param chirpFeedback Feedback amount for chirp echoes (0-1, from twist position)
	 * @param delaySamples Delay time in samples (float for fractional - smooth pitch)
	 * @param harmonicBlend Delay tap balance: 0=f only, 0.5=equal f/2f, 1=2f emphasis
	 */
	[[gnu::always_inline]] inline void processWithPunchChirp(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                         uint8_t stages, DisperserDelayState& delay, float punch,
	                                                         float chirpFeedback, float delaySamples,
	                                                         float harmonicBlend = 0.0f) {

		if (stages == 0) {
			outL = inL;
			outR = inR;
			delay.advanceHead();
			return;
		}

		size_t numStages = std::min(static_cast<size_t>(stages), kMaxStages);

		// Delay range constants (match updateCoefficients)
		constexpr float kMinDelayF = 441.0f; // 10ms at 44.1kHz (~100Hz)
		constexpr float kMaxOctaves = 4.32f; // log2(8819/441) ≈ 4.32 octaves range

		needsBufferClear_ = false; // Consume flag but don't act on it

		// Triangle fold in OCTAVE space to get a TARGET, then quantize to octave of ORIGINAL
		// This preserves user's inharmonic offset while preventing pileup
		constexpr float kMaxDelayF = 8819.0f; // 200ms at 44.1kHz
		float octavesFromMin = std::log2(std::max(delaySamples, 1.0f) / kMinDelayF);

		// Triangle fold: bounces between 0 and maxOctaves to get target position
		if (octavesFromMin < 0.0f || octavesFromMin > kMaxOctaves) {
			float doubled = 2.0f * kMaxOctaves;
			octavesFromMin = std::fmod(std::fmod(octavesFromMin, doubled) + doubled, doubled);
			if (octavesFromMin > kMaxOctaves) {
				octavesFromMin = doubled - octavesFromMin;
			}
		}
		float targetDelay = kMinDelayF * std::pow(2.0f, octavesFromMin);

		// Quantize to nearest perfect fifth of ORIGINAL pitch (preserves user's inharmonic offset)
		constexpr float kFifth = 0.5849625f; // log2(3/2)
		float idealShift = std::log2(targetDelay / delaySamples);
		float quantizedShift = std::round(idealShift / kFifth) * kFifth;
		float foldedDelay = delaySamples * std::pow(2.0f, quantizedShift);

		// Ensure result is in range - adjust by one octave if needed
		if (foldedDelay > kMaxDelayF) {
			foldedDelay *= 0.5f;
		}
		if (foldedDelay < kMinDelayF) {
			foldedDelay *= 2.0f;
		}

		// Smooth toward folded target for glitch-free transitions
		// Very slow glide (~5000ms settling) so folds become gradual bends
		if (smoothedDelay_ == 0.0f) {
			smoothedDelay_ = foldedDelay; // Initialize on first call
		}
		else {
			smoothedDelay_ += (foldedDelay - smoothedDelay_) * 0.00002f;
		}
		float smoothDelay = smoothedDelay_;

		// === TRANSIENT DETECTION (delay-aware) ===
		float transient = detectTransient(inL, inR, static_cast<size_t>(smoothDelay));

		// === PUNCH: Write transient-scaled DRY input to delay ===
		// Echoes the original transient (before dispersion)
		q31_t procL = inL;
		q31_t procR = inR;
		if (punch > 0.01f) {
			float punchDelay = smoothDelay; // Already clamped to folding range
			// Write dry input scaled by transient and punch amount
			float punchWrite = punch * (0.5f + transient * 0.49f); // Base 50% + transient boost
			q31_t punchGain = static_cast<q31_t>(std::min(punchWrite, 0.99f) * ONE_Q31);
			q31_t writeL = multiply_32x32_rshift32(inL, punchGain) << 1;
			q31_t writeR = multiply_32x32_rshift32(inR, punchGain) << 1;
			delay.writeAtOffsetFractional(writeL, writeR, punchDelay);
		}

		// === READ FEEDBACK (before allpass so echoes get dispersed too) ===
		float fbAmount = std::max(punch, chirpFeedback);
		if (fbAmount > 0.01f) {
			q31_t fbL, fbR;
			delay.readHead(fbL, fbR);
			delay.clearHead();

			// Feedback with hard clamp to prevent runaway
			// 29-bit: more KS sustain/beef, occasional DC occlusion
			// 28-bit: stable, almost no DC issues, slightly less sustain
			q31_t fbGain = static_cast<q31_t>(fbAmount * 0.99f * ONE_Q31);
			q31_t fbL_scaled = signed_saturate<29>(multiply_32x32_rshift32(fbL, fbGain) << 1);
			q31_t fbR_scaled = signed_saturate<29>(multiply_32x32_rshift32(fbR, fbGain) << 1);
			procL = q31_sat_add(procL, fbL_scaled);
			procR = q31_sat_add(procR, fbR_scaled);
		}

		// === ALLPASS CASCADE with per-stage emphasis gains ===
		int32_t tmp[2] = {procL, procR};
		int32x2_t proc = vld1_s32(tmp);

		for (size_t i = 0; i < numStages; ++i) {
			proc = stages_[i].processLR(proc, coeffsL_[i], coeffsR_[i]);
			// Apply per-stage emphasis gain (includes polarity flip)
			// stageGains_ stored as gain * (ONE_Q31/2), so shift left 1 after multiply
			int32x2_t gain = vdup_n_s32(stageGains_[i]);
			proc = vshl_n_s32(vqrdmulh_s32(proc, gain), 1);
		}

		outL = vget_lane_s32(proc, 0);
		outR = vget_lane_s32(proc, 1);

		// === WRITE TO DELAY at f and optionally 2f ===
		// harmonicBlend controls f/2f balance: 0=f only, 0.5=equal, 1=2f emphasis
		// When harmonicBlend=0, skip 2f write entirely for efficiency
		// Uses fractional writes for smooth pitch transitions
		if (chirpFeedback > 0.01f) {
			float delayF = smoothDelay; // Already clamped to folding range

			// Transient-focused write: lower base prevents DC accumulation
			// while transient boost preserves punchy attack character
			// Tuning: 80/20 preserves full mojo, rare DC self-resolves over time
			// (75/25 very close; 70/30 loses some sustain; 50/50 too thin)
			float baseWrite = chirpFeedback * 0.8f;
			float transientBoost = chirpFeedback * transient * 0.2f;
			float writeAmount = std::min(baseWrite + transientBoost, 0.99f);

			// Gain balance: harmonicBlend=0 → f=1.0, 2f=0; harmonicBlend=1 → f=0.5, 2f=0.8
			float fGain = 1.0f - harmonicBlend * 0.5f; // 1.0 → 0.5
			float f2Gain = harmonicBlend * 0.8f;       // 0.0 → 0.8

			// Write at f (fundamental) - fractional for smooth pitch
			q31_t writeGainF = static_cast<q31_t>(writeAmount * fGain * ONE_Q31);
			q31_t writeLF = multiply_32x32_rshift32(outL, writeGainF) << 1;
			q31_t writeRF = multiply_32x32_rshift32(outR, writeGainF) << 1;
			delay.writeAtOffsetFractional(writeLF, writeRF, delayF);

			// Write at 2f (octave up) only if harmonicBlend > 0
			if (harmonicBlend > 0.01f) {
				float delay2F = std::max(smoothDelay * 0.5f, kMinDelayF); // Clamp to minimum
				q31_t writeGain2F = static_cast<q31_t>(writeAmount * f2Gain * ONE_Q31);
				q31_t writeL2F = multiply_32x32_rshift32(outL, writeGain2F) << 1;
				q31_t writeR2F = multiply_32x32_rshift32(outR, writeGain2F) << 1;
				delay.writeAtOffsetFractional(writeL2F, writeR2F, delay2F);
			}
		}

		delay.advanceHead();
	}

	/**
	 * Buffer-level wrapper for punch/chirp processing with benchmarking
	 *
	 * Wraps processWithPunchChirp per-sample calls with FX_BENCH instrumentation.
	 */
	void processBufferPunchChirp(StereoBuffer<q31_t> buffer, uint8_t stages, DisperserDelayState& delay, float punch,
	                             float chirpFeedback, size_t delaySamples, float harmonicBlend = 0.0f) {
		if (stages == 0 || buffer.empty()) {
			return;
		}

		// Stage count tags for benchmarking (shared with processBuffer)
		static constexpr const char* kStageNames[] = {"s1",  "s2",  "s3",  "s4",  "s5",  "s6",  "s7",  "s8",
		                                              "s9",  "s10", "s11", "s12", "s13", "s14", "s15", "s16",
		                                              "s17", "s18", "s19", "s20", "s21", "s22", "s23", "s24",
		                                              "s25", "s26", "s27", "s28", "s29", "s30", "s31", "s32"};
		FX_BENCH_DECLARE(bench, "disperser");
		FX_BENCH_SET_TAG(bench, 0, kStageNames[std::min(stages, static_cast<uint8_t>(32)) - 1]);
		FX_BENCH_SCOPE(bench);

		for (auto& sample : buffer) {
			q31_t outL, outR;
			processWithPunchChirp(sample.l, sample.r, outL, outR, stages, delay, punch, chirpFeedback,
			                      static_cast<float>(delaySamples), harmonicBlend);
			sample.l = outL;
			sample.r = outR;
		}
	}

	/// Get stage delay offset (for chirp delay time from freq knob)
	[[nodiscard]] size_t getStageOffset(size_t stageIndex) const {
		if (stageIndex >= kMaxStages) {
			return 100; // Default ~2ms at 44.1kHz
		}
		return stageOffsets_[stageIndex];
	}

	/// Reset all filter states (delay is in shared DisperserParams, reset separately)
	void reset() {
		for (auto& stage : stages_) {
			stage.reset();
		}
		// Initialize stage gains to unity (1.0 stored as ONE_Q31/2 for multiply scaling)
		stageGains_.fill(ONE_Q31 / 2);
		feedback_[0] = 0;
		feedback_[1] = 0;
		dcPrevIn_[0] = 0;
		dcPrevIn_[1] = 0;
		dcPrevOut_[0] = 0;
		dcPrevOut_[1] = 0;
		springFeedback_[0] = 0;
		springFeedback_[1] = 0;
		fastEnv_[0] = 0;
		fastEnv_[1] = 0;
		slowEnv_[0] = 0;
		slowEnv_[1] = 0;
	}

private:
	// ==================== ROUTING METHODS (used by processWithFeedback) ====================
	// These handle allpass cascade routing only - feedback is handled by caller

	/// Cascade routing: standard allpass cascade with per-stage delay writes
	[[gnu::always_inline]] inline void processRoutingCascade(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                         size_t numStages, DisperserDelayState& delay,
	                                                         float dispersion, float lowFreqGain, float highFreqGain) {
		int32_t tmp[2] = {inL, inR};
		int32x2_t proc = vld1_s32(tmp);
		float gainDelta = (highFreqGain - lowFreqGain) / std::max(1.0f, static_cast<float>(numStages - 1));

		// n^0.75 scaling for stage normalization
		float stageNorm = 1.875f / std::pow(static_cast<float>(numStages), 0.75f);

		for (size_t i = 0; i < numStages; ++i) {
			proc = stages_[i].processLR(proc, coeffsL_[i], coeffsR_[i]);
			// Apply per-stage emphasis gain (includes polarity flip)
			int32x2_t emphGain = vdup_n_s32(stageGains_[i]);
			proc = vshl_n_s32(vqrdmulh_s32(proc, emphGain), 1);

			// Per-stage delay write with frequency-dispersed offsets
			size_t baseOffset = stageOffsets_[i];
			size_t scaledOffset = (dispersion < 0.01f)
			                          ? 1
			                          : std::clamp(static_cast<size_t>(1 + (baseOffset - 1) * dispersion), size_t{1},
			                                       DisperserDelayState::kMaxDelaySamples - 1);

			float stageGain = std::clamp(lowFreqGain + gainDelta * static_cast<float>(i), 0.0f, 1.0f) * stageNorm;
			q31_t writeGain = static_cast<q31_t>(stageGain * ONE_Q31);
			q31_t writeL = multiply_32x32_rshift32(vget_lane_s32(proc, 0), writeGain) << 1;
			q31_t writeR = multiply_32x32_rshift32(vget_lane_s32(proc, 1), writeGain) << 1;
			delay.writeAtOffset(writeL, writeR, scaledOffset);
		}

		outL = vget_lane_s32(proc, 0);
		outR = vget_lane_s32(proc, 1);
	}

	/// PingPong routing: L through first half, R through second half, cross-mix
	[[gnu::always_inline]] inline void processRoutingPingPong(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                          size_t numStages) {
		size_t half = numStages / 2;
		if (half == 0)
			half = 1;

		// L processes through first half of stages
		q31_t procL = inL;
		for (size_t i = 0; i < half; ++i) {
			int32_t tmp[2] = {procL, procL};
			int32x2_t vec = vld1_s32(tmp);
			vec = stages_[i].processLR(vec, coeffsL_[i], coeffsR_[i]);
			// Apply per-stage emphasis gain
			int32x2_t emphGain = vdup_n_s32(stageGains_[i]);
			vec = vshl_n_s32(vqrdmulh_s32(vec, emphGain), 1);
			procL = vget_lane_s32(vec, 0);
		}

		// R processes through second half + cross-feed from L
		q31_t procR = q31_sat_add(inR, procL >> 2); // 25% cross-feed from L
		for (size_t i = half; i < numStages; ++i) {
			int32_t tmp[2] = {procR, procR};
			int32x2_t vec = vld1_s32(tmp);
			vec = stages_[i].processLR(vec, coeffsL_[i], coeffsR_[i]);
			// Apply per-stage emphasis gain
			int32x2_t emphGain = vdup_n_s32(stageGains_[i]);
			vec = vshl_n_s32(vqrdmulh_s32(vec, emphGain), 1);
			procR = vget_lane_s32(vec, 0);
		}

		// Cross-mix outputs for stereo ping-pong character
		outL = q31_sat_add(procL, procR >> 1);
		outR = q31_sat_add(procR, procL >> 1);
	}

	/// Cross routing: L↔R swap every 4 stages for swirling stereo
	[[gnu::always_inline]] inline void processRoutingCross(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                       size_t numStages, float crossMix) {
		// Cross amount: 40% to 90% based on crossMix
		q31_t crossGain = static_cast<q31_t>((0.4f + crossMix * 0.5f) * ONE_Q31);

		q31_t procL = inL;
		q31_t procR = inR;

		for (size_t i = 0; i < numStages; ++i) {
			int32_t tmp[2] = {procL, procR};
			int32x2_t proc = vld1_s32(tmp);
			proc = stages_[i].processLR(proc, coeffsL_[i], coeffsR_[i]);
			// Apply per-stage emphasis gain
			int32x2_t emphGain = vdup_n_s32(stageGains_[i]);
			proc = vshl_n_s32(vqrdmulh_s32(proc, emphGain), 1);
			procL = vget_lane_s32(proc, 0);
			procR = vget_lane_s32(proc, 1);

			// Cross-swap every 4 stages for swirling stereo character
			if ((i & 3) == 3) {
				q31_t newL = q31_sat_add(multiply_32x32_rshift32(procL, ONE_Q31 - crossGain) << 1,
				                         multiply_32x32_rshift32(procR, crossGain) << 1);
				q31_t newR = q31_sat_add(multiply_32x32_rshift32(procR, ONE_Q31 - crossGain) << 1,
				                         multiply_32x32_rshift32(procL, crossGain) << 1);
				procL = newL;
				procR = newR;
			}
		}

		outL = procL;
		outR = procR;
	}

	/// Nested routing: Schroeder-style inner/outer chains for diffuse character
	[[gnu::always_inline]] inline void processRoutingNested(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                        size_t numStages) {
		size_t half = numStages / 2;
		if (half == 0)
			half = 1;

		int32_t inTmp[2] = {inL, inR};
		int32x2_t input = vld1_s32(inTmp);

		// Inner chain (first half)
		int32x2_t inner = input;
		for (size_t i = 0; i < half; ++i) {
			inner = stages_[i].processLR(inner, coeffsL_[i], coeffsR_[i]);
			// Apply per-stage emphasis gain
			int32x2_t emphGain = vdup_n_s32(stageGains_[i]);
			inner = vshl_n_s32(vqrdmulh_s32(inner, emphGain), 1);
		}

		// Outer chain (second half) - fed by inner for nested structure
		// 70% blend of inner into outer input
		int32x2_t outerIn = vqadd_s32(input, vqrdmulh_s32(inner, vdup_n_s32(0x59999999)));
		int32x2_t outer = outerIn;
		for (size_t i = half; i < numStages; ++i) {
			outer = stages_[i].processLR(outer, coeffsL_[i], coeffsR_[i]);
			// Apply per-stage emphasis gain
			int32x2_t emphGain = vdup_n_s32(stageGains_[i]);
			outer = vshl_n_s32(vqrdmulh_s32(outer, emphGain), 1);
		}

		// Cross-mix L/R at output for stereo diffusion
		q31_t outLraw = vget_lane_s32(outer, 0);
		q31_t outRraw = vget_lane_s32(outer, 1);
		outL = q31_sat_add(outLraw, outRraw >> 2); // 25% cross-mix
		outR = q31_sat_add(outRraw, outLraw >> 2);
	}

	/// Spring routing: Multi-tap capture for chirp character
	[[gnu::always_inline]] inline void processRoutingSpring(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                        size_t numStages) {
		int32_t inTmp[2] = {inL, inR};
		int32x2_t proc = vld1_s32(inTmp);

		// Tap points at 1/3 and 2/3 for spring character
		size_t tap1 = numStages / 3;
		size_t tap2 = (numStages * 2) / 3;
		if (tap1 == 0)
			tap1 = 1;
		if (tap2 <= tap1)
			tap2 = tap1 + 1;

		int32x2_t tap1Val{};
		int32x2_t tap2Val{};

		for (size_t i = 0; i < numStages; ++i) {
			proc = stages_[i].processLR(proc, coeffsL_[i], coeffsR_[i]);
			// Apply per-stage emphasis gain
			int32x2_t emphGain = vdup_n_s32(stageGains_[i]);
			proc = vshl_n_s32(vqrdmulh_s32(proc, emphGain), 1);

			// Capture taps for blending into output
			if (i == tap1) {
				tap1Val = proc;
			}
			else if (i == tap2) {
				tap2Val = proc;
			}
		}

		// Blend taps into output for spring character
		int32x2_t tapBlend =
		    vqadd_s32(vqrdmulh_s32(tap1Val, vdup_n_s32(0x20000000)), vqrdmulh_s32(tap2Val, vdup_n_s32(0x20000000)));
		proc = vqadd_s32(proc, tapBlend);

		// Cross-mix L/R for stereo interest
		q31_t outLraw = vget_lane_s32(proc, 0);
		q31_t outRraw = vget_lane_s32(proc, 1);
		outL = q31_sat_add(outLraw, outRraw >> 3);
		outR = q31_sat_add(outRraw, outLraw >> 3);
	}

	std::array<filter::StereoBiquadAllpass, kMaxStages> stages_{};  // 2nd-order biquad allpasses
	std::array<filter::BiquadAllpassCoeffs, kMaxStages> coeffsL_{}; // L channel coefficients
	std::array<filter::BiquadAllpassCoeffs, kMaxStages> coeffsR_{}; // R channel coefficients
	std::array<size_t, kMaxStages> stageOffsets_{};                 // Delay offsets per stage (from frequency)
	std::array<q31_t, kMaxStages> stageGains_ = []() {
		std::array<q31_t, kMaxStages> gains{};
		gains.fill(ONE_Q31 / 2); // Initialize to unity (1.0 in our storage format)
		return gains;
	}(); // Per-stage gain with sign for polarity flip
	q31_t feedback_[2]{};
	q31_t dcPrevIn_[2]{};
	q31_t dcPrevOut_[2]{};
	q31_t lpfState_[2]{};          // 1-pole LPF state for bandpass feedback
	q31_t springFeedback_[2]{};    // Secondary feedback tap for Spring topology
	float smoothedDelay_{0.0f};    // Smoothed delay time for pitch glide
	bool needsBufferClear_{false}; // Set by updateCoefficients when octave wrap detected

	// Transient detection state (for Punch and Chirp zones)
	// Fast envelope (~1ms attack) tracks peaks
	// Slow envelope (~50ms attack) tracks average level
	// Transient = fast - slow (positive during attacks)
	q31_t fastEnv_[2]{}; // Fast envelope follower (L/R)
	q31_t slowEnv_[2]{}; // Slow envelope follower (L/R)

	uint8_t lastFreq_{255};
	uint8_t lastSpread_{255};
	uint8_t lastActiveStages_{255}; // Force initial coefficient calculation
	float lastLrOffset_{0.0f};
	float lastQ_{-1.0f};           // Force initial coefficient calculation
	float lastSpreadCurve_{-1.0f}; // Force initial coefficient calculation
	float lastQTilt_{999.0f};      // Force initial coefficient calculation
	float lastBimodal_{-1.0f};     // Force initial coefficient calculation
	float lastDetuning_{-1.0f};    // Force initial coefficient calculation
	float lastEmphasis_{999.0f};   // Force initial calculation (bipolar, so 999 triggers)
	                               // Note: Delay buffers are in DisperserDelayState (shared state), not here
};

} // namespace deluge::dsp
