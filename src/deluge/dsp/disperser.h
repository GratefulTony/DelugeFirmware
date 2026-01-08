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
 * - [x] Performance: use fastmath intrinsics (exp2f, log2f, powf vs std::pow)
 * - [x] Consolidate processing paths (unified processSample/processBuffer)
 * - [ ] Consider fractional delay interpolation for smoother pitch sweeping
 */

#pragma once

#include "definitions_cxx.hpp"
#include "dsp/filter/ladder_components.h"
#include "dsp/phi_triangle.hpp"
#include "dsp/zone_param.hpp"
#include "io/debug/fx_benchmark.h"
#include "modulation/params/param.h"
#include "storage/field_serialization.h"
#include "util/fixedpoint.h"
#include <arm_neon.h>
#include <array>
#include <cmath>
#include <cstring>

namespace deluge::dsp {

/// Saturating add for q31_t using NEON
[[gnu::always_inline]] inline q31_t q31_sat_add(q31_t a, q31_t b) {
	return vget_lane_s32(vqadd_s32(vdup_n_s32(a), vdup_n_s32(b)), 0);
}

// Forward declarations
struct DisperserTopoParams;
struct DisperserTwistParams;

/// Zone count derived from param definition (single source of truth)
constexpr int32_t kDisperserNumZones =
    modulation::params::getZoneParamInfo(modulation::params::GLOBAL_DISPERSER_TOPO).zoneCount;

// Fastmath constants (avoid repeated divisions and slow std::pow)
constexpr float k127Recip = 1.0f / 127.0f; // Reciprocal for MIDI-style 0-127 params
constexpr float kLog5 = 1.6094379124341f;  // log(5) for std::pow(5,x) → expf(x*kLog5)
constexpr float kLog10 = 2.302585092994f;  // log(10) for curve exponent range 10.0↔0.1

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

/**
 * Secret knob phases - three unbounded phase offsets for disperser
 *
 * Accessed via push+twist on encoders. These shift the φ-triangle
 * constellation without changing the zone selection.
 */
struct SecretPhases {
	float metaPhase{0};     // Push Twist encoder
	float metaPhaseTopo{0}; // Push Topo encoder
	float gammaPhase{0};    // Push third encoder (×100 multiplier)

	/// Effective phase for meta zones: metaPhase + 100*gammaPhase
	[[nodiscard]] double effectiveMeta() const {
		return static_cast<double>(metaPhase) + 100.0 * static_cast<double>(gammaPhase);
	}

	/// Effective phase for topology: metaPhaseTopo + 100*gammaPhase
	[[nodiscard]] double effectiveTopo() const {
		return static_cast<double>(metaPhaseTopo) + 100.0 * static_cast<double>(gammaPhase);
	}

	void writeToFile(Serializer& writer) const {
		WRITE_FLOAT(writer, metaPhase, "dispMeta", 10.0f);
		WRITE_FLOAT(writer, metaPhaseTopo, "dispMetaTopo", 10.0f);
		WRITE_FLOAT(writer, gammaPhase, "dispGamma", 10.0f);
	}

	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FLOAT(reader, tagName, metaPhase, "dispMeta", 10.0f);
		READ_FLOAT(reader, tagName, metaPhaseTopo, "dispMetaTopo", 10.0f);
		READ_FLOAT(reader, tagName, gammaPhase, "dispGamma", 10.0f);
		return false;
	}
};

struct DisperserParams {
	// Secret knob phases (unbounded, accessed via push+twist on encoders)
	SecretPhases phases;

	// Zone base values for modulation (like sine shaper pattern)
	// Topo clips to zone boundaries (different algorithms per zone)
	// Twist allows cross-zone modulation (continuous modifier space)
	ZoneBasedParam<kDisperserNumZones, true> topo;
	ZoneBasedParam<kDisperserNumZones, false> twist;

	// User-facing knob values
	uint8_t freq{64};  // Center frequency (0-127, maps to 1Hz-8kHz)
	uint8_t stages{0}; // Number of active stages (0-32, 0 = bypass)

	// DSP smoothing state (per-sound, persists across buffers)
	q31_t smoothedTopo{0};
	q31_t smoothedTwist{0};
	q31_t smoothedFeedback{0}; // For feedback interpolation in processBuffer
	q31_t smoothedFreq{0};     // Previous freq value for coefficient smoothing
	q31_t smoothedSpread{0};   // Previous spread value for coefficient smoothing

	// Zone tracking for filter state reset on zone boundary crossing
	int32_t lastTopoZone{-1}; // -1 = uninitialized

	// Shared delay line for comb resonance (all topologies can use)
	DisperserDelayState delay;

	/// Check if disperser is enabled
	[[nodiscard]] bool isEnabled() const { return stages > 0; }

	/// Write disperser params to file (only non-default values)
	void writeToFile(Serializer& writer) const {
		WRITE_FIELD_DEFAULT(writer, freq, "dispFreq", 64);
		WRITE_FIELD(writer, stages, "dispStages");
		WRITE_ZONE(writer, topo.value, "dispTopo");
		WRITE_ZONE(writer, twist.value, "dispTwist");
		phases.writeToFile(writer);
	}

	/// Read a tag into disperser params, returns true if tag was handled
	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, freq, "dispFreq");
		READ_FIELD(reader, tagName, stages, "dispStages");
		READ_ZONE(reader, tagName, topo.value, "dispTopo");
		READ_ZONE(reader, tagName, twist.value, "dispTwist");
		if (phases.readTag(reader, tagName)) {
			return true;
		}
		return false;
	}
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

	// Phase offset for detuning/harmonicBlend in topo (computed from twist meta position)
	// In meta zones (5-7), twist position rotates through topo's phi triangle patterns
	float phaseOffset{0.0f};
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
		float centerHz = exp2f(freq * k127Recip * 13.0f);

		// spread: 0-127 -> ±4 octaves (used as local spread around each mode in bimodal)
		float spreadOctaves = spread * k127Recip * 4.0f;

		// L/R offset: shift L down, R up by half the offset each
		float lOffsetOct = -lrOffset * 0.5f;
		float rOffsetOct = lrOffset * 0.5f;

		// Base Q clamped to reasonable range (0.5 = very broad, 20 = very sharp/resonant)
		float qBase = std::clamp(q, 0.5f, 20.0f);

		size_t numActive = std::max(static_cast<size_t>(activeStages), size_t{1});

		// Bipolar spread curve: exponent varies from 10.0 (tight cluster) to 0.1 (extreme spread)
		// spreadCurve=0: exponent=10.0 (stages cluster tightly toward mode center)
		// spreadCurve=0.5: exponent=1.0 (linear distribution within mode)
		// spreadCurve=1: exponent=0.1 (stages spread extremely toward edges)
		float curveClamped = std::clamp(spreadCurve, 0.0f, 1.0f);
		float exponent = expf((1.0f - curveClamped * 2.0f) * kLog10);

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
			float sepMult = exp2f(halfSep);
			modeAHz = centerHz / sepMult;
			modeBHz = centerHz * sepMult;
		}

		// Pre-compute detuning in octaves (check once, not per-stage)
		// detuning=1.0 means ±50 cents (≈±0.042 octaves)
		bool useDetuning = detuning > 0.001f;
		float maxDetuneOct = useDetuning ? (50.0f * detuning / 1200.0f) : 0.0f;

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
					float curved = powf(tLocal, exponent);

					// Mode A: positive offset (upward), Mode B: negative offset (downward)
					// Clamping at 20Hz/16kHz handles boundaries naturally
					float localPosition = isModeB ? -curved : curved;

					// Each mode uses half the spread range
					float localSpread = spreadOctaves * 0.5f;
					stageHzBase = modeCenter * exp2f(localPosition * localSpread);
				}
				else {
					// Normal single-mode distribution
					float curved = powf(t, exponent);
					float stagePosition = curved * 2.0f - 1.0f;
					stageHzBase = centerHz * exp2f(stagePosition * spreadOctaves);
				}
			}
			else {
				stageHzBase = centerHz; // Unused stages
				t = 1.0f;
			}

			// Per-stage Q with tilt
			float qFactor = exp2f(qTiltClamped * 2.0f * (t * 2.0f - 1.0f));
			float stageQ = std::clamp(qBase * qFactor, 0.5f, 20.0f);

			// Per-stage detuning: alternating +/- for chorus shimmer
			// Pattern: even stages detune up, odd stages detune down
			float detuneOct = useDetuning ? (((i & 1) ? -1.0f : 1.0f) * maxDetuneOct) : 0.0f;

			// Apply L/R offset + detuning and clamp to useful range (1Hz floor for subharmonics)
			float stageHzL = std::clamp(stageHzBase * exp2f(lOffsetOct + detuneOct), 1.0f, 16000.0f);
			float stageHzR = std::clamp(stageHzBase * exp2f(rOffsetOct + detuneOct), 1.0f, 16000.0f);

			// Compute 2nd-order biquad allpass coefficients with per-stage Q
			coeffsL_[i].compute(stageHzL, stageQ, static_cast<float>(kSampleRate));
			coeffsR_[i].compute(stageHzR, stageQ, static_cast<float>(kSampleRate));

			// Calculate delay offset for this stage (based on average frequency)
			// Uses shared triangle fold + fifth quantization logic
			float avgHz = (stageHzL + stageHzR) * 0.5f;
			float rawOffset = kSampleRate / avgHz;
			float foldedOffset = calculateFoldedDelay(rawOffset);
			size_t offset = static_cast<size_t>(foldedOffset);

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
	 * Per-sample processing with topology routing
	 *
	 * Routes through allpass cascade based on topology zone.
	 * Called by processSample after feedback read, before delay write.
	 */
	[[gnu::always_inline]] inline void processWithTopology(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                       uint8_t stages, int32_t topology, q31_t crossGain) {
		if (stages == 0) {
			outL = inL;
			outR = inR;
			return;
		}

		size_t numStages = std::min(static_cast<size_t>(stages), kMaxStages);

		// === TOPOLOGY-SPECIFIC ROUTING ===
		switch (topology) {
		case 1: // PingPong
			processRoutingPingPong(inL, inR, outL, outR, numStages);
			break;
		case 3: // Cross
			processRoutingCross(inL, inR, outL, outR, numStages, crossGain);
			break;
		case 4: // Parallel
			processRoutingParallel(inL, inR, outL, outR, numStages);
			break;
		case 5: // Nested
			processRoutingNested(inL, inR, outL, outR, numStages);
			break;
		case 7: // Spring
			processRoutingSpring(inL, inR, outL, outR, numStages);
			break;
		default: // 0=Cascade, 2=Bimodal, 6=Diffuse
			processRoutingCascade(inL, inR, outL, outR, numStages);
			break;
		}
	}

	/**
	 * Calculate triangle-folded delay with perfect fifth quantization (once per buffer)
	 *
	 * Folds delay time in octave space to stay within usable range, then quantizes
	 * to nearest perfect fifth of original pitch to preserve inharmonic character.
	 *
	 * @param delaySamples Raw delay time in samples
	 * @return Folded and quantized delay time
	 */
	[[nodiscard]] static float calculateFoldedDelay(float delaySamples) {
		// Delay range constants
		constexpr float kMinDelayF = 441.0f;  // 10ms at 44.1kHz (~100Hz)
		constexpr float kMaxDelayF = 8819.0f; // 200ms at 44.1kHz
		constexpr float kMaxOctaves = 4.32f;  // log2(8819/441) ≈ 4.32 octaves range

		// Triangle fold in OCTAVE space to get a TARGET, then quantize to octave of ORIGINAL
		// This preserves user's inharmonic offset while preventing pileup
		float octavesFromMin = log2f(std::max(delaySamples, 1.0f) / kMinDelayF);

		// Triangle fold: bounces between 0 and maxOctaves to get target position
		if (octavesFromMin < 0.0f || octavesFromMin > kMaxOctaves) {
			float doubled = 2.0f * kMaxOctaves;
			octavesFromMin = std::fmod(std::fmod(octavesFromMin, doubled) + doubled, doubled);
			if (octavesFromMin > kMaxOctaves) {
				octavesFromMin = doubled - octavesFromMin;
			}
		}
		float targetDelay = kMinDelayF * exp2f(octavesFromMin);

		// Quantize to nearest perfect fifth of ORIGINAL pitch (preserves user's inharmonic offset)
		constexpr float kFifth = 0.5849625f; // log2(3/2)
		float idealShift = log2f(targetDelay / delaySamples);
		float quantizedShift = std::round(idealShift / kFifth) * kFifth;
		float foldedDelay = delaySamples * exp2f(quantizedShift);

		// Ensure result is in range - adjust by one octave if needed
		if (foldedDelay > kMaxDelayF) {
			foldedDelay *= 0.5f;
		}
		if (foldedDelay < kMinDelayF) {
			foldedDelay *= 2.0f;
		}

		return foldedDelay;
	}

	/**
	 * Process a single stereo sample through disperser
	 *
	 * Unified processing path with topology routing and optional feedback.
	 * When punch=0 and chirp=0, this is pure topology routing (no delay access).
	 *
	 * @param punch Transient boost amount (0-1, 0=disabled)
	 * @param chirp Feedback amount for chirp echoes (0-1, 0=disabled)
	 * @param foldedDelay Pre-calculated folded delay time
	 * @param harmonicBlend Delay tap balance: 0=f only, 0.5=equal f/2f, 1=2f emphasis
	 * @param topology Topology zone for routing (0=Cascade, 1=PingPong, etc.)
	 * @param crossGain Pre-computed cross-coupling gain for Cross topology (q31)
	 */
	[[gnu::always_inline]] inline void processSample(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR, uint8_t stages,
	                                                 DisperserDelayState& delay, float punch, float chirp,
	                                                 float foldedDelay, float harmonicBlend, int32_t topology,
	                                                 q31_t crossGain) {

		if (stages == 0) {
			outL = inL;
			outR = inR;
			delay.advanceHead();
			return;
		}

		// Minimum delay for clamping 2f tap
		constexpr float kMinDelayF = 441.0f; // 10ms at 44.1kHz

		needsBufferClear_ = false; // Consume flag but don't act on it

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
		float fbAmount = std::max(punch, chirp);
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

		// === ALLPASS CASCADE with topology routing ===
		processWithTopology(procL, procR, outL, outR, stages, topology, crossGain);

		// === WRITE TO DELAY at f and optionally 2f ===
		// harmonicBlend controls f/2f balance: 0=f only, 0.5=equal, 1=2f emphasis
		// When harmonicBlend=0, skip 2f write entirely for efficiency
		// Uses fractional writes for smooth pitch transitions
		if (chirp > 0.01f) {
			float delayF = smoothDelay; // Already clamped to folding range

			// Transient-focused write: lower base prevents DC accumulation
			// while transient boost preserves punchy attack character
			// Tuning: 80/20 preserves full mojo, rare DC self-resolves over time
			// (75/25 very close; 70/30 loses some sustain; 50/50 too thin)
			float baseWrite = chirp * 0.8f;
			float transientBoost = chirp * transient * 0.2f;
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
	 * Process a stereo buffer through disperser
	 *
	 * Unified processing with topology routing and optional punch/chirp feedback.
	 * Calculates triangle-folded delay once per buffer for efficiency.
	 */
	void processBuffer(StereoBuffer<q31_t> buffer, uint8_t stages, DisperserDelayState& delay, float punch, float chirp,
	                   size_t delaySamples, float harmonicBlend, int32_t topology, float crossMix) {
		if (stages == 0 || buffer.empty()) {
			return;
		}

		// Stage count tags for benchmarking
		static constexpr const char* kStageNames[] = {"s1",  "s2",  "s3",  "s4",  "s5",  "s6",  "s7",  "s8",
		                                              "s9",  "s10", "s11", "s12", "s13", "s14", "s15", "s16",
		                                              "s17", "s18", "s19", "s20", "s21", "s22", "s23", "s24",
		                                              "s25", "s26", "s27", "s28", "s29", "s30", "s31", "s32"};
		FX_BENCH_DECLARE(bench, "disperser");
		FX_BENCH_SET_TAG(bench, 0, kStageNames[std::min(stages, static_cast<uint8_t>(32)) - 1]);
		FX_BENCH_SCOPE(bench);

		// Calculate folded delay ONCE per buffer (expensive log2/pow/fmod/round)
		float foldedDelay = calculateFoldedDelay(static_cast<float>(delaySamples));

		// Precompute crossGain from crossMix (40-90% based on crossMix)
		q31_t crossGain = static_cast<q31_t>((0.4f + crossMix * 0.5f) * ONE_Q31);

		for (auto& sample : buffer) {
			q31_t outL, outR;
			processSample(sample.l, sample.r, outL, outR, stages, delay, punch, chirp, foldedDelay, harmonicBlend,
			              topology, crossGain);
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
		fastEnv_[0] = 0;
		fastEnv_[1] = 0;
		slowEnv_[0] = 0;
		slowEnv_[1] = 0;
	}

private:
	// ==================== ROUTING METHODS (used by processWithTopology) ====================
	// These handle allpass cascade routing only - pure phase processing, no feedback

	/// Cascade routing: allpass cascade with per-stage emphasis gains
	[[gnu::always_inline]] inline void processRoutingCascade(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                         size_t numStages) {
		int32_t tmp[2] = {inL, inR};
		int32x2_t proc = vld1_s32(tmp);

		for (size_t i = 0; i < numStages; ++i) {
			proc = stages_[i].processLR(proc, coeffsL_[i], coeffsR_[i]);
			int32x2_t emphGain = vdup_n_s32(stageGains_[i]);
			proc = vshl_n_s32(vqrdmulh_s32(proc, emphGain), 1);
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

		// L processes through first half of stages (mono: duplicate to both lanes)
		int32x2_t vecL = vdup_n_s32(inL);
		for (size_t i = 0; i < half; ++i) {
			vecL = stages_[i].processLR(vecL, coeffsL_[i], coeffsR_[i]);
			vecL = vshl_n_s32(vqrdmulh_s32(vecL, vdup_n_s32(stageGains_[i])), 1);
		}
		q31_t procL = vget_lane_s32(vecL, 0);

		// R processes through second half + cross-feed from L (blend, not add)
		// 80% inR + 20% procL = unity gain
		q31_t procRstart = q31_sat_add(multiply_32x32_rshift32(inR, 0x66666666) << 1,    // 0.8
		                               multiply_32x32_rshift32(procL, 0x19999999) << 1); // 0.2
		int32x2_t vecR = vdup_n_s32(procRstart);
		for (size_t i = half; i < numStages; ++i) {
			vecR = stages_[i].processLR(vecR, coeffsL_[i], coeffsR_[i]);
			vecR = vshl_n_s32(vqrdmulh_s32(vecR, vdup_n_s32(stageGains_[i])), 1);
		}
		q31_t procR = vget_lane_s32(vecR, 0);

		// Cross-mix outputs with unity gain (blend, not add)
		// 67% self + 33% other = 100% total
		outL = q31_sat_add(multiply_32x32_rshift32(procL, 0x55555555) << 1,  // 0.67
		                   multiply_32x32_rshift32(procR, 0x2AAAAAAB) << 1); // 0.33
		outR = q31_sat_add(multiply_32x32_rshift32(procR, 0x55555555) << 1, multiply_32x32_rshift32(procL, 0x2AAAAAAB)
		                                                                        << 1);
	}

	/// Cross routing: L↔R swap every 4 stages for swirling stereo
	[[gnu::always_inline]] inline void processRoutingCross(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                       size_t numStages, q31_t crossGain) {
		// Build initial vector: L in lane 0, R in lane 1
		int32x2_t proc = vset_lane_s32(inR, vdup_n_s32(inL), 1);

		for (size_t i = 0; i < numStages; ++i) {
			proc = stages_[i].processLR(proc, coeffsL_[i], coeffsR_[i]);
			proc = vshl_n_s32(vqrdmulh_s32(proc, vdup_n_s32(stageGains_[i])), 1);

			// Cross-swap every 4 stages for swirling stereo character
			if ((i & 3) == 3) {
				q31_t procL = vget_lane_s32(proc, 0);
				q31_t procR = vget_lane_s32(proc, 1);
				q31_t keepGain = ONE_Q31 - crossGain;
				q31_t newL = q31_sat_add(multiply_32x32_rshift32(procL, keepGain) << 1,
				                         multiply_32x32_rshift32(procR, crossGain) << 1);
				q31_t newR = q31_sat_add(multiply_32x32_rshift32(procR, keepGain) << 1,
				                         multiply_32x32_rshift32(procL, crossGain) << 1);
				proc = vset_lane_s32(newR, vdup_n_s32(newL), 1);
			}
		}

		outL = vget_lane_s32(proc, 0);
		outR = vget_lane_s32(proc, 1);
	}

	/// Parallel routing: Two parallel cascades for thick, chorus-like character
	/// Less phase smear than series cascade, more like multiple detuned sources
	[[gnu::always_inline]] inline void processRoutingParallel(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR,
	                                                          size_t numStages) {
		size_t half = numStages / 2;
		if (half == 0)
			half = 1;

		// Build input vector once
		int32x2_t input = vset_lane_s32(inR, vdup_n_s32(inL), 1);

		// Path A: first half of stages
		int32x2_t pathA = input;
		for (size_t i = 0; i < half; ++i) {
			pathA = stages_[i].processLR(pathA, coeffsL_[i], coeffsR_[i]);
			pathA = vshl_n_s32(vqrdmulh_s32(pathA, vdup_n_s32(stageGains_[i])), 1);
		}

		// Path B: second half of stages (parallel, not series!)
		int32x2_t pathB = input; // Same input, not pathA output
		for (size_t i = half; i < numStages; ++i) {
			pathB = stages_[i].processLR(pathB, coeffsL_[i], coeffsR_[i]);
			pathB = vshl_n_s32(vqrdmulh_s32(pathB, vdup_n_s32(stageGains_[i])), 1);
		}

		// Sum paths with 50% each for unity gain
		int32x2_t sum =
		    vqadd_s32(vqrdmulh_s32(pathA, vdup_n_s32(0x40000000)), vqrdmulh_s32(pathB, vdup_n_s32(0x40000000)));

		outL = vget_lane_s32(sum, 0);
		outR = vget_lane_s32(sum, 1);
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
		// Unity gain blend: 50% input + 50% inner
		int32x2_t outerIn =
		    vqadd_s32(vqrdmulh_s32(input, vdup_n_s32(0x40000000)), vqrdmulh_s32(inner, vdup_n_s32(0x40000000)));
		int32x2_t outer = outerIn;
		for (size_t i = half; i < numStages; ++i) {
			outer = stages_[i].processLR(outer, coeffsL_[i], coeffsR_[i]);
			// Apply per-stage emphasis gain
			int32x2_t emphGain = vdup_n_s32(stageGains_[i]);
			outer = vshl_n_s32(vqrdmulh_s32(outer, emphGain), 1);
		}

		// Cross-mix L/R with unity gain (blend, not add): 80% self + 20% other
		q31_t outLraw = vget_lane_s32(outer, 0);
		q31_t outRraw = vget_lane_s32(outer, 1);
		outL = q31_sat_add(multiply_32x32_rshift32(outLraw, 0x66666666) << 1,  // 0.8
		                   multiply_32x32_rshift32(outRraw, 0x19999999) << 1); // 0.2
		outR = q31_sat_add(multiply_32x32_rshift32(outRraw, 0x66666666) << 1,
		                   multiply_32x32_rshift32(outLraw, 0x19999999) << 1);
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

		// Blend taps with main output for spring character (unity gain)
		// 70% proc + 15% tap1 + 15% tap2 = 100%
		int32x2_t blended = vqadd_s32(vqrdmulh_s32(proc, vdup_n_s32(0x59999999)),     // 0.7
		                              vqrdmulh_s32(tap1Val, vdup_n_s32(0x13333333))); // 0.15
		blended = vqadd_s32(blended, vqrdmulh_s32(tap2Val, vdup_n_s32(0x13333333)));  // 0.15

		// Cross-mix L/R with unity gain (blend, not add): 87.5% self + 12.5% other
		q31_t outLraw = vget_lane_s32(blended, 0);
		q31_t outRraw = vget_lane_s32(blended, 1);
		outL = q31_sat_add(multiply_32x32_rshift32(outLraw, 0x70000000) << 1,  // 0.875
		                   multiply_32x32_rshift32(outRraw, 0x10000000) << 1); // 0.125
		outR = q31_sat_add(multiply_32x32_rshift32(outRraw, 0x70000000) << 1,
		                   multiply_32x32_rshift32(outLraw, 0x10000000) << 1);
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

// ============================================================================
// Pitch Tracking Helpers
// ============================================================================

/// Convert MIDI note code to Hz (A4=440Hz standard)
/// Note code 60 = middle C (C3) = ~130.81 Hz
/// A4 (note 69) = 440 Hz
[[gnu::always_inline]] inline float noteCodeToHz(int32_t noteCode) {
	// Standard formula: Hz = 440 * 2^((noteCode - 69) / 12)
	return 440.0f * exp2f((static_cast<float>(noteCode) - 69.0f) / 12.0f);
}

/// Convert Hz to 0-127 range for disperser (matching 1Hz-8kHz = 13 octaves)
[[gnu::always_inline]] inline uint8_t hzToDisperserFreq(float hz) {
	// Inverse of: centerHz = 1.0f * exp2f((freq / 127.0f) * 13.0f)
	// freq = 127 * log2(hz) / 13
	float octaves = log2f(std::max(hz, 1.0f));
	return static_cast<uint8_t>(std::clamp(octaves * 127.0f / 13.0f, 0.0f, 127.0f));
}

// ============================================================================
// Encapsulated Processing Function
// ============================================================================

/**
 * Process disperser effect on audio buffer (encapsulated API)
 *
 * Handles param combination, smoothing, topology dispatch, and DSP processing.
 * Follows the sine shaper pattern: all impl details inside, clean callsite.
 *
 * @param buffer Audio buffer to process in place
 * @param dsp Disperser DSP engine
 * @param params DisperserParams state (freq, stages, zones, smoothing, delay)
 * @param topoPreset Topo value from patched params (automation)
 * @param topoCables Topo modulation from mod matrix cables
 * @param twistPreset Twist value from patched params (automation)
 * @param twistCables Twist modulation from mod matrix cables
 * @param noteCode Last played note for pitch tracking (-1 if none)
 */
inline void processDisperser(StereoBuffer<q31_t> buffer, Disperser& dsp, DisperserParams& params, q31_t topoPreset,
                             q31_t topoCables, q31_t twistPreset, q31_t twistCables, int32_t noteCode) {
	if (!params.isEnabled() || buffer.empty()) {
		return;
	}

	// Combine preset + cables using zone-aware scaling (like sine shaper pattern)
	q31_t topoValue = params.topo.combinePresetAndCables(topoPreset, topoCables);
	q31_t twistValue = params.twist.combinePresetAndCables(twistPreset, twistCables);

	// Smooth zone params for DSP
	constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03 * ONE_Q31);
	params.smoothedTopo += (multiply_32x32_rshift32(topoValue - params.smoothedTopo, smoothingAlpha) << 1);
	params.smoothedTwist += (multiply_32x32_rshift32(twistValue - params.smoothedTwist, smoothingAlpha) << 1);

	// Compute derived parameters from zones (twist first - provides phaseOffset for topo)
	DisperserTwistParams twistParams = computeDisperserTwistParams(params.smoothedTwist, &params);
	DisperserTopoParams topoParams = computeDisperserTopoParams(params.smoothedTopo, &params, twistParams.phaseOffset);

	// Reset filter state when crossing zone boundaries to avoid artifacts
	if (params.lastTopoZone != topoParams.zone && params.lastTopoZone >= 0) {
		dsp.reset();
	}
	params.lastTopoZone = topoParams.zone;

	// Topology-specific spread modulation
	float spreadMod = 1.0f;
	float lrSpreadOffset = twistParams.width; // Full range stereo spread

	switch (topoParams.zone) {
	case 0: // Cascade: classic disperser, param0 = spread
		spreadMod = topoParams.param0;
		break;
	case 1: // PingPong: tighter spread for rhythmic effect, alternation via lrOffset
		spreadMod = topoParams.param0 * 0.7f;
		lrSpreadOffset += topoParams.lrOffset * 0.3f;
		break;
	case 2: // Stereo: L/R frequency offset for width, param0 = spread, lrOffset = stereo amount
		spreadMod = topoParams.param0;
		lrSpreadOffset += topoParams.lrOffset;
		break;
	case 3: // Cross: cross-coupled, tighter for comb
		spreadMod = topoParams.param0 * 0.5f;
		break;
	case 4: // Pitch: placeholder - would track pitch, for now acts like tight comb
		spreadMod = topoParams.param0 * 0.3f;
		break;
	case 5: // Nested: wider spread for diffusion
		spreadMod = 0.3f + topoParams.param0 * 0.7f;
		break;
	case 6: // Diffuse: randomized feel via param variations
		spreadMod = topoParams.param0;
		break;
	case 7: // Spring: chirp character
		spreadMod = 0.2f + topoParams.param0 * 0.5f;
		break;
	}

	q31_t dispSpread = static_cast<q31_t>(std::clamp(spreadMod, 0.0f, 1.0f) * ONE_Q31);

	// Pitch tracking: try to get note frequency, use freq knob as offset
	// Note: drums use kNoteForDrum=60, so they track to ~261Hz with offset from there
	uint8_t effectiveFreq = params.freq;
	if (noteCode >= 0 && noteCode < 128) {
		// Got a valid note - use it as base frequency
		float noteHz = noteCodeToHz(noteCode);

		// Freq knob becomes bipolar offset: 64=center (no offset), 0=-6.5 octaves, 127=+6.5 octaves
		float offsetOctaves = (static_cast<float>(params.freq) - 64.0f) / 64.0f * 6.5f;
		float offsetHz = noteHz * exp2f(offsetOctaves);
		effectiveFreq = hzToDisperserFreq(offsetHz);
	}
	// else: no pitch info (clips/samples), use freq knob as absolute frequency

	// Convert freq to q31
	q31_t dispFreq = static_cast<q31_t>(effectiveFreq) << 24;

	// Bimodal separation: topoParams.param0 in Bimodal zone (2), else 0
	float bimodalSeparation = (topoParams.zone == 2) ? topoParams.param0 : 0.0f;

	// Update coefficients with smoothing (lrOffset creates stereo width, Q from topo zone)
	dsp.updateCoefficientsSmoothed(dispFreq, dispSpread, &params.smoothedFreq, &params.smoothedSpread, lrSpreadOffset,
	                               topoParams.q, params.stages, twistParams.spreadCurve, twistParams.qTilt,
	                               bimodalSeparation, topoParams.detuning, topoParams.emphasis);

	// Cross mix amount for Cross topology (zone 3)
	float crossMix = (topoParams.zone == 3) ? (0.3f + topoParams.param0 * 0.5f) : 0.0f;

	// Delay time comes from freq knob (via stage offsets), doubled for longer echo
	size_t centerStage = params.stages / 2;
	size_t delaySamples = dsp.getStageOffset(centerStage) * 2;

	// Unified processing: topology routing + optional punch/chirp feedback
	dsp.processBuffer(buffer, params.stages, params.delay, twistParams.punch, twistParams.chirpAmount, delaySamples,
	                  topoParams.harmonicBlend, topoParams.zone, crossMix);
}

} // namespace deluge::dsp
