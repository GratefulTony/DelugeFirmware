/*
 * Copyright © 2024 Synthstrom Audible Limited
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
 */

#pragma once

#include "definitions_cxx.hpp"
#include "dsp/fast_math.h"
#include "dsp/filter/allpass_crossover.h"
#include "dsp/filter/ladder_components.h"
#include "dsp/filter/lr_crossover.h"
#include "dsp_ng/core/types.hpp"
#include "io/debug/print.h"
#include "util/fixedpoint.h"
#include "util/functions.h"
#include <array>
#include <cmath>
#include <span>

// TODO:PROFILING-DELETE - Enable to profile multiband compressor render stages
// Outputs cycle counts for crossover, envelope, gain apply, and recombine stages
// Uses Debug::OneOfN to sample every Nth buffer (default 1000)
// Set to 1 and enable ENABLE_TEXT_OUTPUT in uart.h to profile
#define MULTIBAND_PROFILE 0

namespace deluge::dsp {

/// A single-band compressor with both upward and downward compression (OTT-style).
/// Designed to be used as part of a multiband compressor.
class BandCompressor {
public:
	BandCompressor() = default;

	/// Configure the compressor parameters
	/// @param attack Attack time (0 to ONE_Q31)
	/// @param release Release time (0 to ONE_Q31)
	/// @param thresholdDown Threshold for downward compression (0 to ONE_Q31)
	/// @param thresholdUp Threshold for upward compression (0 to ONE_Q31)
	/// @param ratioDown Downward compression ratio (0 to ONE_Q31)
	/// @param ratioUp Upward compression ratio (0 to ONE_Q31)
	void setup(q31_t attack, q31_t release, q31_t thresholdDown, q31_t thresholdUp, q31_t ratioDown, q31_t ratioUp) {
		setAttack(attack);
		setRelease(release);
		setThresholdDown(thresholdDown);
		setThresholdUp(thresholdUp);
		setRatioDown(ratioDown);
		setRatioUp(ratioUp);
	}

	void setAttack(q31_t attack) {
		attackKnob_ = attack;
		// Map 0-ONE_Q31 to 0.5ms - 100ms (exponential curve)
		attackMS_ = 0.5f + (fastExp(2.0f * float(attack) / ONE_Q31f) - 1.0f) * 15.0f;
		attack_ = (-1000.0f / kSampleRate) / attackMS_;
	}

	void setRelease(q31_t release) {
		releaseKnob_ = release;
		// Map 0-ONE_Q31 to 5ms - 500ms (exponential curve)
		releaseMS_ = 5.0f + (fastExp(2.0f * float(release) / ONE_Q31f) - 1.0f) * 75.0f;
		release_ = (-1000.0f / kSampleRate) / releaseMS_;
	}

	void setThresholdDown(q31_t t) {
		thresholdDownKnob_ = t;
		// Map 0-ONE_Q31 to 0.2-1.0 (conventional: knob up = higher threshold = less compression)
		// Higher thresholdDown_ = higher internal threshold = signal must be louder to trigger compression
		thresholdDown_ = 0.2f + 0.8f * (float(t) / ONE_Q31f);
	}

	void setThresholdUp(q31_t t) {
		thresholdUpKnob_ = t;
		thresholdUp_ = 0.2f + 0.8f * (float(t) / ONE_Q31f);
	}

	void setRatioDown(q31_t r) {
		ratioDownKnob_ = r;
		// Map 0-ONE_Q31 to 0-1.0 (0 = no compression / 1:1 ratio, 1 = full limiting)
		fractionDown_ = float(r) / ONE_Q31f;
	}

	void setRatioUp(q31_t r) {
		ratioUpKnob_ = r;
		// Map 0-ONE_Q31 to 0-1.0 (0 = no expansion, 1 = full expansion)
		fractionUp_ = float(r) / ONE_Q31f;
	}

	/// Set per-band output level (0 to ONE_Q31) - applied after compression
	/// CCW = -inf, 12:00 = 0dB, CW = +16dB
	/// This controls the mix balance of bands (like OTT's L/M/H sliders)
	void setOutputLevel(q31_t g) {
		outputLevelKnob_ = g;
		float normalized = float(g) / ONE_Q31f;
		if (normalized <= 0.5f) {
			// 0 to 0.5 maps to 0x to 1.0x (-inf to 0dB)
			outputLevel_ = normalized * 2.0f;
		}
		else {
			// 0.5 to 1.0 maps to 1.0x to 6.31x (0dB to +16dB)
			outputLevel_ = 1.0f + (normalized - 0.5f) * 2.0f * 5.31f;
		}
	}

	/// Set bandwidth (gap between up and down thresholds)
	/// When BW=0, thresholds are equal. When BW=max, maximum gap.
	void setBandwidth(q31_t bw) {
		bandwidthKnob_ = bw;
		// Bandwidth as a fraction of available headroom (0 to 0.6)
		float bandwidthFraction = 0.6f * (float(bw) / ONE_Q31f);
		// thresholdUp = thresholdDown + bandwidth offset
		// This creates a "dead zone" where no compression happens
		thresholdUp_ = std::min(1.0f, thresholdDown_ + bandwidthFraction);
	}

	[[nodiscard]] q31_t getAttack() const { return attackKnob_; }
	[[nodiscard]] q31_t getRelease() const { return releaseKnob_; }
	[[nodiscard]] float getAttackMS() const { return attackMS_; }
	[[nodiscard]] float getReleaseMS() const { return releaseMS_; }
	[[nodiscard]] q31_t getThresholdDown() const { return thresholdDownKnob_; }
	[[nodiscard]] q31_t getThresholdUp() const { return thresholdUpKnob_; }
	[[nodiscard]] q31_t getRatioDown() const { return ratioDownKnob_; }
	[[nodiscard]] q31_t getRatioUp() const { return ratioUpKnob_; }
	[[nodiscard]] q31_t getOutputLevel() const { return outputLevelKnob_; }
	[[nodiscard]] q31_t getBandwidth() const { return bandwidthKnob_; }
	[[nodiscard]] float getOutputLevelLinear() const { return outputLevel_; }

	/// Get bandwidth for display in dB (0-36dB range)
	/// This represents the "dead zone" gap between up and down thresholds
	[[nodiscard]] float getBandwidthForDisplay() const {
		// Bandwidth fraction 0-0.6 maps to approximately 0-36dB
		// (assuming ~60dB reference dynamic range)
		float bandwidthFraction = 0.6f * (float(bandwidthKnob_) / ONE_Q31f);
		return bandwidthFraction * 60.0f; // 0-36dB
	}

	/// Get threshold for display in dB (approximately -60dB to -12dB range)
	/// Lower values = more aggressive compression (compresses quieter signals)
	[[nodiscard]] float getThresholdForDisplay() const {
		// thresholdDown_ is 0.2-1.0, representing fraction of dynamic range
		// Map 0.2-1.0 to -60dB to -12dB (conventional: lower threshold = more compression)
		// When thresholdDown_=0.2 (knob low), display -60dB (more compression)
		// When thresholdDown_=1.0 (knob high), display -12dB (less compression)
		constexpr float minDB = -60.0f;
		constexpr float maxDB = -12.0f;
		float normalized = (thresholdDown_ - 0.2f) / 0.8f; // 0 to 1
		return minDB + normalized * (maxDB - minDB);       // -60dB to -12dB
	}

	/// Get ratio for display in x:1 format (1.0 = no compression, higher = more compression)
	[[nodiscard]] float getRatioForDisplay() const {
		// fractionDown_ is 0-1, where 0 = no compression, 1 = limiting
		// Convert to ratio: ratio = 1 / (1 - fraction), clamped
		if (fractionDown_ >= 0.99f) {
			return 100.0f; // Essentially limiting
		}
		return 1.0f / (1.0f - fractionDown_);
	}

	/// Reset the compressor state
	void reset() {
		envelope_ = 0.0f;
		rms_ = 0.0f;
		level_ = 0.0f;
		lastFrameCount_ = 0;
	}

	/// Calculate gain adjustment for the band based on current RMS level.
	/// Returns a linear gain multiplier.
	/// @param numSamples Number of samples in the current buffer
	/// @param songVolumedB Reference volume level in dB (log scale)
	/// @param knee Soft knee amount (0 = hard, 1 = soft, affects transition around threshold)
	/// @param skew Up/down balance (-1 = upward only, 0 = balanced, +1 = downward only)
	/// @param frameCount Global frame counter for gap detection
	[[nodiscard]] float calculateGain(float numSamples, float songVolumedB, float knee = 0.0f, float skew = 0.0f,
	                                  uint32_t frameCount = 0) {
		// Detect gap in processing - if more than 1 frame since last call,
		// there was a pause. Calculate natural decay over the gap.
		uint32_t previousFrameCount = lastFrameCount_;
		lastFrameCount_ = frameCount;
		bool gapDetected = (frameCount > 0) && (previousFrameCount > 0) && (frameCount > previousFrameCount + 1);

		float threshDowndB = songVolumedB * thresholdDown_;
		float threshUpdB = songVolumedB * thresholdUp_;

		// Calculate how far above/below thresholds we are
		float diffDown = rms_ - threshDowndB;
		float diffUp = threshUpdB - rms_;

		// Knee width in dB (0 = hard knee, up to 12dB soft knee)
		float kneeWidthdB = knee * 12.0f;
		float halfKnee = kneeWidthdB / 2.0f;

		// Downward compression with soft knee
		float over;
		if (kneeWidthdB < 0.1f || diffDown > halfKnee) {
			// Hard knee or fully above knee region
			over = std::max(0.0f, diffDown);
		}
		else if (diffDown > -halfKnee) {
			// In soft knee transition region - quadratic interpolation
			float x = diffDown + halfKnee; // 0 to kneeWidth
			over = (x * x) / (2.0f * kneeWidthdB);
		}
		else {
			// Below knee region
			over = 0.0f;
		}

		// Upward compression with soft knee
		float under;
		if (kneeWidthdB < 0.1f || diffUp > halfKnee) {
			// Hard knee or fully below knee region
			under = std::max(0.0f, diffUp);
		}
		else if (diffUp > -halfKnee) {
			// In soft knee transition region
			float x = diffUp + halfKnee;
			under = (x * x) / (2.0f * kneeWidthdB);
		}
		else {
			// Above knee region
			under = 0.0f;
		}

		// Apply up/down skew to balance compression types
		// skew = -1: upward only, skew = 0: balanced, skew = +1: downward only
		float upwardFactor = std::clamp(1.0f - skew, 0.0f, 1.0f);
		float downwardFactor = std::clamp(1.0f + skew, 0.0f, 1.0f);

		// Combined target with skew applied
		float target = -over * fractionDown_ * downwardFactor + under * fractionUp_ * upwardFactor;

		// Run envelope follower - if gap detected, calculate natural decay
		if (gapDetected) {
			// Processing just resumed after a pause
			// Calculate how many samples elapsed during the gap
			uint32_t gapFrames = frameCount - previousFrameCount - 1;
			float gapSamples = static_cast<float>(gapFrames) * numSamples;

			// During silence, target would be 0 (no compression needed)
			// Decay envelope toward 0 using release time constant
			// This simulates what would have happened if we processed silence
			envelope_ = envelope_ * fastExp(release_ * gapSamples);

			// Reset level tracking since we have no history of what happened during gap
			level_ = 0.0f;
			rms_ = 0.0f;
		}
		else {
			envelope_ = runEnvelope(envelope_, target, numSamples);
		}

		// Convert to linear gain, clamped to prevent overflow
		float gain = fastExp(envelope_);
		return std::clamp(gain, 0.1f, 10.0f);
	}

	/// Update the level from stereo band buffers
	/// @param bufferL Left channel samples
	/// @param bufferR Right channel samples
	/// @param numSamples Number of samples in each buffer
	/// @param response 0.0 = tight (~2ms, catches transients), 1.0 = punchy (~145ms, lets transients through)
	/// Optimized: response-based stride and alpha are matched for consistent behavior
	void updateLevel(const q31_t* bufferL, const q31_t* bufferR, size_t numSamples, float response) {
		q31_t peak = 0;

		// Response-based downsampling: tight needs fine resolution, punchy doesn't
		// stride = 2 (tight) to 32 (punchy) - matched to alpha for consistent behavior
		// At 44.1kHz: stride 2 = 0.045ms resolution, stride 32 = 0.73ms resolution
		const size_t stride = 2 + static_cast<size_t>(response * 30.0f);
		for (size_t i = 0; i < numSamples; i += stride) {
			q31_t L = bufferL[i];
			q31_t R = bufferR[i];
			L = (L < 0) ? -L : L;
			R = (R < 0) ? -R : R;
			q31_t s = (L > R) ? L : R; // max(|L|, |R|)
			peak = (s > peak) ? s : peak;
		}

		// IIR smoothing with extended range for musical variety
		// alpha = 0.60 (tight, ~2ms τ) to 0.02 (punchy, ~145ms τ)
		// At 128 samples/buffer (~2.9ms): τ = -2.9ms / ln(1-α)
		float peakF = static_cast<float>(peak);
		float alpha = 0.60f - response * 0.58f;
		level_ = level_ * (1.0f - alpha) + peakF * alpha;

		// Convert to log domain for threshold comparison (used by calculateGain)
		rms_ = fastLog(std::max(level_, 1.0f));
	}

	/// Get current gain reduction in dB (for metering)
	[[nodiscard]] float getGainReductionDB() const { return envelope_; }

	/// Get current input level in log domain (for metering)
	[[nodiscard]] float getInputLevelLog() const { return rms_; }

	/// Get threshold in log domain (for metering tick marks)
	[[nodiscard]] float getThresholdLog() const { return thresholdDown_; }

private:
	[[nodiscard]] float runEnvelope(float current, float target, float numSamples) const {
		// Attack = envelope moving AWAY from unity (0) - compression/expansion starting
		// Release = envelope moving TOWARD unity (0) - compression/expansion ending
		// This works correctly for both downward (negative envelope) and upward (positive envelope)
		bool movingAwayFromUnity = std::abs(target) > std::abs(current);
		float timeConstant = movingAwayFromUnity ? attack_ : release_;
		return target + fastExp(timeConstant * numSamples) * (current - target);
	}

	float attack_ = -1000.0f / kSampleRate;
	float release_ = -1000.0f / kSampleRate;
	float attackMS_ = 1.0f;
	float releaseMS_ = 100.0f;
	float thresholdDown_ = 0.8f;
	float thresholdUp_ = 0.5f;
	float fractionDown_ = 0.5f;
	float fractionUp_ = 0.5f;
	float envelope_ = 0.0f;
	float rms_ = 0.0f;
	float level_ = 0.0f;
	uint32_t lastFrameCount_ = 0; // For gap detection

	q31_t attackKnob_ = ONE_Q31 / 4;  // Default ~10ms
	q31_t releaseKnob_ = ONE_Q31 / 4; // Default ~100ms
	q31_t thresholdDownKnob_ = 0;
	q31_t thresholdUpKnob_ = 0;
	q31_t ratioDownKnob_ = 0;
	q31_t ratioUpKnob_ = 0;
	q31_t outputLevelKnob_ = ONE_Q31 / 2; // Default 0dB (unity) - 12:00 position
	q31_t bandwidthKnob_ = ONE_Q31 / 2;   // Default medium bandwidth
	float outputLevel_ = 1.0f;            // Linear output level multiplier (matches knob default)
};

/// Character zone names for display
enum class CharacterZone : uint8_t {
	Width = 0,  // Stereo width variations
	Timing = 1, // Per-band timing offsets
	Skew = 2,   // Per-band up/down skew
	Punch = 3,  // Fast attack, transient emphasis
	Air = 4,    // High frequency emphasis
	Rich = 5,   // Upward compression focus
	OTT = 6,    // Classic OTT character
	OWLTT = 7   // Extreme/experimental - OTT cranked to 11
};

/// Vibe zone names for display - controls phase relationships between oscillations
enum class VibeZone : uint8_t {
	Sync = 0,    // All oscillations in phase
	Spread = 1,  // Evenly spread phases (120° apart)
	Pairs = 2,   // Band pairs in/out of phase
	Cascade = 3, // Progressive phase shift across bands
	Invert = 4,  // Opposite phases between parameters
	Pulse = 5,   // Clustered phases creating pulses
	Drift = 6,   // Slowly varying phase relationships
	Chaos = 7    // Rapidly oscillating phase offsets
};

/// 3-band multiband compressor with OTT-style upward/downward compression.
/// Uses allpass crossover for perfect phase-coherent band splitting.
class MultibandCompressor {
public:
	static constexpr int kNumBands = 3;
	static constexpr int kNumCharacterZones = 8;
	static constexpr int kNumVibeZones = 8;

	enum class Band : uint8_t { Low = 0, Mid = 1, High = 2 };

	MultibandCompressor() {
		// Default OTT-style settings - initialize all crossover types
		crossoverAllpass1_.setLowCrossover(200.0f);
		crossoverAllpass1_.setHighCrossover(2000.0f);
		crossoverAllpass2_.setLowCrossover(200.0f);
		crossoverAllpass2_.setHighCrossover(2000.0f);
		crossoverAllpass3_.setLowCrossover(200.0f);
		crossoverAllpass3_.setHighCrossover(2000.0f);
		crossoverLR2_.setLowCrossover(200.0f);
		crossoverLR2_.setHighCrossover(2000.0f);
		crossoverLR2Fast_.setLowCrossover(200.0f);
		crossoverLR2Fast_.setHighCrossover(2000.0f);

		// Set default parameters for each band
		// Use setter functions to keep knob values in sync with actual values
		for (auto& band : bands_) {
			band.setAttack(ONE_Q31 / 4);
			band.setRelease(ONE_Q31 / 4);
			band.setThresholdDown(ONE_Q31 / 2);
			band.setRatioDown(0); // Start at 1:1 ratio (transparent)
			band.setRatioUp(0);   // Start at 1:1 ratio (transparent)
			band.setBandwidth(ONE_Q31 / 2);
			band.setOutputLevel(ONE_Q31 / 2); // 12:00 = 0dB (unity)
		}

		// Initialize global parameters via setters to keep knob values in sync
		setOutputGain(ONE_Q31 / 2); // 12:00 = 0dB (unity)
		setCharacter(0);            // Default to 0 (Width zone start) - neutral settings
		setUpDownSkew(ONE_Q31 / 2);
		setVibe(0); // Default to 0 (Sync zone start) - all in phase
	}

	/// Set crossover frequency between low and mid bands
	void setLowCrossover(float freqHz) {
		crossoverAllpass1_.setLowCrossover(freqHz);
		crossoverAllpass2_.setLowCrossover(freqHz);
		crossoverAllpass3_.setLowCrossover(freqHz);
		crossoverLR2_.setLowCrossover(freqHz);
		crossoverLR2Fast_.setLowCrossover(freqHz);
	}

	/// Set crossover frequency between mid and high bands
	void setHighCrossover(float freqHz) {
		crossoverAllpass1_.setHighCrossover(freqHz);
		crossoverAllpass2_.setHighCrossover(freqHz);
		crossoverAllpass3_.setHighCrossover(freqHz);
		crossoverLR2_.setHighCrossover(freqHz);
		crossoverLR2Fast_.setHighCrossover(freqHz);
	}

	/// Get low crossover frequency in Hz
	[[nodiscard]] float getLowCrossoverHz() const { return crossoverAllpass1_.getLowCrossoverHz(); }

	/// Get high crossover frequency in Hz
	[[nodiscard]] float getHighCrossoverHz() const { return crossoverAllpass1_.getHighCrossoverHz(); }

	/// Set crossover type (ordered by CPU cost, cheapest first):
	/// 0=allpass 6dB/oct (1st order), 1=allpass 12dB/oct (2nd order),
	/// 2=allpass 18dB/oct (3rd order), 3=LR2 Fast (no phase comp), 4=LR2 12dB/oct (full)
	void setCrossoverType(uint8_t type) { crossoverType_ = std::min(type, static_cast<uint8_t>(4)); }

	/// Get crossover type (0-4, ordered by CPU cost)
	[[nodiscard]] uint8_t getCrossoverType() const { return crossoverType_; }

	/// Access a specific band's compressor
	BandCompressor& getBand(Band band) { return bands_[static_cast<size_t>(band)]; }
	[[nodiscard]] const BandCompressor& getBand(Band band) const { return bands_[static_cast<size_t>(band)]; }

	/// Access band by index
	BandCompressor& getBand(size_t index) { return bands_[index]; }
	[[nodiscard]] const BandCompressor& getBand(size_t index) const { return bands_[index]; }

	/// Set wet/dry blend (0 = fully dry, ONE_Q31 = fully wet)
	void setBlend(FixedPoint<31> blend) {
		wet_ = blend;
		dry_ = 1.0f - static_cast<float>(blend);
	}

	[[nodiscard]] FixedPoint<31> getBlend() const { return wet_; }

	/// Set output gain (0 to ONE_Q31)
	/// CCW = -inf, 12:00 = 0dB, CW = +16dB
	void setOutputGain(q31_t g) {
		outputGainKnob_ = g;
		float normalized = float(g) / ONE_Q31f;
		if (normalized <= 0.5f) {
			// 0 to 0.5 maps to 0x to 1.0x (-inf to 0dB)
			outputGain_ = normalized * 2.0f;
		}
		else {
			// 0.5 to 1.0 maps to 1.0x to 6.31x (0dB to +16dB)
			outputGain_ = 1.0f + (normalized - 0.5f) * 2.0f * 5.31f;
		}
	}

	[[nodiscard]] q31_t getOutputGain() const { return outputGainKnob_; }
	[[nodiscard]] float getOutputGainLinear() const { return outputGain_; }

	/// Set threshold for all bands simultaneously (linked control)
	/// Shifts all per-band values by delta from previous linked value
	void setAllThresholds(q31_t t) {
		int64_t delta = static_cast<int64_t>(t) - static_cast<int64_t>(linkedThreshold_);
		linkedThreshold_ = t;
		for (size_t i = 0; i < kNumBands; ++i) {
			int64_t newVal = static_cast<int64_t>(bands_[i].getThresholdDown()) + delta;
			bands_[i].setThresholdDown(
			    static_cast<q31_t>(std::clamp(newVal, static_cast<int64_t>(0), static_cast<int64_t>(ONE_Q31))));
		}
	}

	/// Set ratio for all bands simultaneously (linked control)
	/// Shifts all per-band values by delta from previous linked value
	void setAllRatios(q31_t r) {
		int64_t delta = static_cast<int64_t>(r) - static_cast<int64_t>(linkedRatio_);
		linkedRatio_ = r;
		for (size_t i = 0; i < kNumBands; ++i) {
			int64_t newVal = static_cast<int64_t>(bands_[i].getRatioDown()) + delta;
			q31_t clamped =
			    static_cast<q31_t>(std::clamp(newVal, static_cast<int64_t>(0), static_cast<int64_t>(ONE_Q31)));
			bands_[i].setRatioDown(clamped);
			bands_[i].setRatioUp(clamped);
		}
	}

	/// Set attack for all bands simultaneously (linked control)
	void setAllAttacks(q31_t a) {
		for (auto& band : bands_) {
			band.setAttack(a);
		}
	}

	/// Set release for all bands simultaneously (linked control)
	void setAllReleases(q31_t r) {
		for (auto& band : bands_) {
			band.setRelease(r);
		}
	}

	/// Set character (0 to ONE_Q31) - replaces knee, controls multiple internal params
	/// Divided into 8 zones: Width, Timing, Skew, Punch, Air, Rich, OTT, Wild
	/// Each zone emphasizes different aspects of the compression character
	/// Uses vibe phase offsets for OWLTT zone oscillations
	void setCharacter(q31_t c) {
		// Cache: skip recalculation if knob hasn't changed
		// Note: OWLTT zone depends on vibe phases, but user must wiggle character to update
		if (c == characterKnob_ && characterComputed_) {
			return;
		}
		characterKnob_ = c;
		characterComputed_ = true;
		float t = float(c) / ONE_Q31f; // 0.0 to 1.0

		// Determine zone (0-7) and position within zone (0.0-1.0)
		float zoneFloat = t * kNumCharacterZones;
		int zone = std::min(kNumCharacterZones - 1, static_cast<int>(zoneFloat));
		float zonePos = zoneFloat - zone; // 0-1 within zone

		// Triangle wave: cheap periodic function for OWLTT zone oscillations
		// Input: phase (any value), Output: -1 to +1
		auto triangle = [](float phase) {
			float p = phase - std::floor(phase); // Wrap to 0-1
			return 1.0f - 4.0f * std::abs(p - 0.5f);
		};

		// === Compute derived parameters based on zone ===
		// Each zone has characteristic curves for width, knee, timing, skew

		// Response: 0=smooth (~145ms), 1=punchy (~2ms) - extended range
		// Detection time varies from ~2ms (transient-accurate) to ~145ms (glue-like)
		switch (zone) {
		case 3:                                  // Punch: fast detection for transients (2-4ms)
		case 6:                                  // OTT: fast for classic aggressive response
			response_ = 0.85f + zonePos * 0.15f; // 0.85→1.0
			break;
		case 4:                                  // Air: slow detection for smooth glue (70-145ms)
		case 5:                                  // Rich: slow for warm sustain
			response_ = 0.15f - zonePos * 0.15f; // 0.15→0.0
			break;
		case 7: // OWLTT: oscillates full range for dynamic breathing
			response_ = 0.5f + 0.5f * triangle(zonePos * 2.0f + vibePhaseWidth_);
			break;
		default:
			response_ = 0.5f; // Balanced (~9ms)
		}

		// Stereo width: 0=mono, 1=full stereo (bass always mono regardless)
		// Width zone: sweeps 0→1, others have moderate values
		switch (zone) {
		case 0: // Width: sweep mono to full stereo
			width_ = zonePos;
			break;
		case 4: // Air: wide stereo for spaciousness
		case 6: // OTT: wide for that classic sound
			width_ = 0.7f + zonePos * 0.3f;
			break;
		case 3: // Punch: narrower for impact
			width_ = 0.3f + zonePos * 0.2f;
			break;
		case 7: // OWLTT: oscillates wildly (with vibe phase offset)
			width_ = 0.5f + 0.5f * triangle(zonePos * 2.0f + vibePhaseWidth_);
			break;
		default:
			width_ = 0.5f; // Moderate stereo
		}

		// Knee: 0=hard, 1=soft
		switch (zone) {
		case 0: // Width: start with steepish knee (0.2), soften as width increases
			knee_ = 0.2f + zonePos * 0.4f;
			break;
		case 4: // Air: soft for smoothness
		case 5: // Rich: soft for warmth
			knee_ = 0.6f + zonePos * 0.3f;
			break;
		case 3: // Punch: hard knee for transients
		case 6: // OTT: medium-hard for aggression
			knee_ = 0.1f + zonePos * 0.2f;
			break;
		case 7: // OWLTT: varies dramatically (with vibe phase offset)
			knee_ = 0.5f + 0.4f * triangle(zonePos * 3.0f + vibePhaseKnee_);
			break;
		default:
			knee_ = 0.4f; // Medium
		}

		// Per-band timing offsets: multiplier on base attack/release (0.5x to 2x)
		// Stored as offset from 1.0 (so 0 = no change, -0.5 = half speed, +1.0 = double)
		switch (zone) {
		case 1:                                 // Timing: sweep from uniform to differentiated
			timingOffset_[0] = -0.3f * zonePos; // Low slower
			timingOffset_[1] = 0.0f;
			timingOffset_[2] = 0.3f * zonePos; // High faster
			break;
		case 3: // Punch: fast attack across all bands
			timingOffset_[0] = -0.4f - zonePos * 0.2f;
			timingOffset_[1] = -0.3f - zonePos * 0.2f;
			timingOffset_[2] = -0.2f - zonePos * 0.2f;
			break;
		case 4: // Air: fast high band
			timingOffset_[0] = 0.2f;
			timingOffset_[1] = 0.0f;
			timingOffset_[2] = -0.4f - zonePos * 0.3f;
			break;
		case 6: // OTT: classic fast timing
			timingOffset_[0] = -0.2f;
			timingOffset_[1] = -0.3f;
			timingOffset_[2] = -0.4f;
			break;
		case 7: // OWLTT: chaos (with vibe phase offsets)
			timingOffset_[0] = 0.5f * triangle(zonePos * 4.0f + vibePhaseTiming_[0]);
			timingOffset_[1] = 0.5f * triangle(zonePos * 5.0f + 0.333f + vibePhaseTiming_[1]);
			timingOffset_[2] = 0.5f * triangle(zonePos * 6.0f + 0.667f + vibePhaseTiming_[2]);
			break;
		default:
			timingOffset_[0] = timingOffset_[1] = timingOffset_[2] = 0.0f;
		}

		// Per-band skew: -1=upward, 0=balanced, +1=downward
		switch (zone) {
		case 2:                               // Skew: sweep through skew variations
			skewOffset_[0] = -0.5f + zonePos; // Low: up→balanced
			skewOffset_[1] = 0.0f;
			skewOffset_[2] = 0.5f - zonePos; // High: down→balanced
			break;
		case 4: // Air: upward on highs
			skewOffset_[0] = 0.0f;
			skewOffset_[1] = -0.2f * zonePos;
			skewOffset_[2] = -0.5f - zonePos * 0.3f;
			break;
		case 5: // Rich: upward emphasis
			skewOffset_[0] = -0.3f - zonePos * 0.3f;
			skewOffset_[1] = -0.4f - zonePos * 0.3f;
			skewOffset_[2] = -0.2f - zonePos * 0.2f;
			break;
		case 6: // OTT: balanced aggressive
			skewOffset_[0] = 0.1f;
			skewOffset_[1] = 0.0f;
			skewOffset_[2] = -0.1f;
			break;
		case 7: // OWLTT: extreme variation (with vibe phase offsets)
			skewOffset_[0] = 0.8f * triangle(zonePos * 5.0f + vibePhaseSkew_[0]);
			skewOffset_[1] = 0.8f * triangle(zonePos * 4.0f + 0.167f + vibePhaseSkew_[1]);
			skewOffset_[2] = 0.8f * triangle(zonePos * 6.0f + 0.333f + vibePhaseSkew_[2]);
			break;
		default:
			skewOffset_[0] = skewOffset_[1] = skewOffset_[2] = 0.0f;
		}
	}

	/// Get character knob value
	[[nodiscard]] q31_t getCharacter() const { return characterKnob_; }

	/// Get current character zone for display
	[[nodiscard]] CharacterZone getCharacterZone() const {
		float t = float(characterKnob_) / ONE_Q31f;
		int zone = std::min(kNumCharacterZones - 1, static_cast<int>(t * kNumCharacterZones));
		return static_cast<CharacterZone>(zone);
	}

	/// Get position within current zone (0-127 for display)
	[[nodiscard]] int32_t getCharacterZonePosition() const {
		float t = float(characterKnob_) / ONE_Q31f;
		float zoneFloat = t * kNumCharacterZones;
		int zone = static_cast<int>(zoneFloat);
		float zonePos = zoneFloat - zone;
		return static_cast<int32_t>(zonePos * 127.0f);
	}

	/// Get stereo width (0=mono, 1=full stereo) - bass always mono regardless
	[[nodiscard]] float getWidth() const { return width_; }

	/// Get knee value (internal, computed from character)
	[[nodiscard]] float getKnee() const { return knee_; }

	/// Get per-band skew offset (added to global skew)
	[[nodiscard]] float getBandSkewOffset(size_t band) const { return (band < kNumBands) ? skewOffset_[band] : 0.0f; }

	/// Get per-band timing offset (multiplier adjustment)
	[[nodiscard]] float getBandTimingOffset(size_t band) const {
		return (band < kNumBands) ? timingOffset_[band] : 0.0f;
	}

	/// Set up/down ratio skew (0 = favor upward, ONE_Q31/2 = balanced, ONE_Q31 = favor downward)
	/// Controls the balance between upward and downward compression
	void setUpDownSkew(q31_t s) {
		upDownSkewKnob_ = s;
		// Map to -1.0 to +1.0 (-1 = all upward, 0 = balanced, +1 = all downward)
		upDownSkew_ = (float(s) / ONE_Q31f) * 2.0f - 1.0f;
	}

	/// Set vibe (0 to ONE_Q31) - controls phase relationships between Feel oscillations
	/// Divided into 8 zones: Sync, Spread, Pairs, Cascade, Invert, Pulse, Drift, Chaos
	void setVibe(q31_t v) {
		vibeKnob_ = v;
		// Invalidate character cache - OWLTT zone depends on vibe phases
		characterComputed_ = false;
		float t = float(v) / ONE_Q31f; // 0.0 to 1.0

		// Determine zone (0-7) and position within zone (0.0-1.0)
		float zoneFloat = t * kNumVibeZones;
		int zone = std::min(kNumVibeZones - 1, static_cast<int>(zoneFloat));
		float zonePos = zoneFloat - zone; // 0-1 within zone

		// Triangle wave for Chaos zone oscillations
		auto triangle = [](float phase) {
			float p = phase - std::floor(phase);
			return 1.0f - 4.0f * std::abs(p - 0.5f);
		};

		// Compute phase offsets based on zone
		switch (zone) {
		case 0: // Sync: all in phase, sweep from 0 to slight offset
			vibePhaseWidth_ = zonePos * 0.1f;
			vibePhaseKnee_ = zonePos * 0.1f;
			vibePhaseTiming_ = {zonePos * 0.1f, zonePos * 0.1f, zonePos * 0.1f};
			vibePhaseSkew_ = {zonePos * 0.1f, zonePos * 0.1f, zonePos * 0.1f};
			break;

		case 1: // Spread: evenly spread phases (120° = 0.333 apart)
			vibePhaseWidth_ = 0.0f;
			vibePhaseKnee_ = 0.167f * zonePos;
			vibePhaseTiming_ = {0.0f, 0.333f * zonePos, 0.667f * zonePos};
			vibePhaseSkew_ = {0.0f, 0.333f * zonePos, 0.667f * zonePos};
			break;

		case 2: // Pairs: low+high in phase, mid opposite
			vibePhaseWidth_ = 0.0f;
			vibePhaseKnee_ = 0.5f * zonePos;
			vibePhaseTiming_ = {0.0f, 0.5f * zonePos, 0.0f};
			vibePhaseSkew_ = {0.0f, 0.5f * zonePos, 0.0f};
			break;

		case 3: // Cascade: progressive phase shift
			vibePhaseWidth_ = 0.25f * zonePos;
			vibePhaseKnee_ = 0.5f * zonePos;
			vibePhaseTiming_ = {0.0f, 0.25f * zonePos, 0.5f * zonePos};
			vibePhaseSkew_ = {0.0f, 0.333f * zonePos, 0.667f * zonePos};
			break;

		case 4: // Invert: width/knee vs timing/skew opposite
			vibePhaseWidth_ = 0.0f;
			vibePhaseKnee_ = 0.0f;
			vibePhaseTiming_ = {0.5f * zonePos, 0.5f * zonePos, 0.5f * zonePos};
			vibePhaseSkew_ = {0.5f * zonePos, 0.5f * zonePos, 0.5f * zonePos};
			break;

		case 5: // Pulse: clustered phases creating pulses
			vibePhaseWidth_ = 0.0f;
			vibePhaseKnee_ = 0.1f * zonePos;
			vibePhaseTiming_ = {0.0f, 0.05f * zonePos, 0.1f * zonePos};
			vibePhaseSkew_ = {0.5f, 0.55f * zonePos, 0.6f * zonePos};
			break;

		case 6: // Drift: slowly varying relationships
			vibePhaseWidth_ = 0.2f * zonePos;
			vibePhaseKnee_ = 0.3f * zonePos;
			vibePhaseTiming_ = {0.1f * zonePos, 0.2f * zonePos, 0.4f * zonePos};
			vibePhaseSkew_ = {0.15f * zonePos, 0.35f * zonePos, 0.25f * zonePos};
			break;

		case 7: // Chaos: rapidly oscillating phase offsets
			vibePhaseWidth_ = 0.5f * triangle(zonePos * 3.0f);
			vibePhaseKnee_ = 0.5f * triangle(zonePos * 4.0f + 0.25f);
			vibePhaseTiming_[0] = 0.5f * triangle(zonePos * 5.0f);
			vibePhaseTiming_[1] = 0.5f * triangle(zonePos * 6.0f + 0.333f);
			vibePhaseTiming_[2] = 0.5f * triangle(zonePos * 7.0f + 0.667f);
			vibePhaseSkew_[0] = 0.5f * triangle(zonePos * 4.0f + 0.5f);
			vibePhaseSkew_[1] = 0.5f * triangle(zonePos * 5.0f + 0.167f);
			vibePhaseSkew_[2] = 0.5f * triangle(zonePos * 6.0f + 0.833f);
			break;

		default:
			vibePhaseWidth_ = 0.0f;
			vibePhaseKnee_ = 0.0f;
			vibePhaseTiming_ = {0.0f, 0.0f, 0.0f};
			vibePhaseSkew_ = {0.0f, 0.0f, 0.0f};
		}
	}

	/// Get vibe knob value
	[[nodiscard]] q31_t getVibe() const { return vibeKnob_; }

	/// Get current vibe zone for display
	[[nodiscard]] VibeZone getVibeZone() const {
		float t = float(vibeKnob_) / ONE_Q31f;
		int zone = std::min(kNumVibeZones - 1, static_cast<int>(t * kNumVibeZones));
		return static_cast<VibeZone>(zone);
	}

	/// Get position within current vibe zone (0-127 for display)
	[[nodiscard]] int32_t getVibeZonePosition() const {
		float t = float(vibeKnob_) / ONE_Q31f;
		float zoneFloat = t * kNumVibeZones;
		int zone = static_cast<int>(zoneFloat);
		float zonePos = zoneFloat - zone;
		return static_cast<int32_t>(zonePos * 127.0f);
	}

	/// Get the linked threshold value
	[[nodiscard]] q31_t getLinkedThreshold() const { return linkedThreshold_; }

	/// Get the linked ratio value
	[[nodiscard]] q31_t getLinkedRatio() const { return linkedRatio_; }

	/// Get the linked attack value (from first band)
	[[nodiscard]] q31_t getLinkedAttack() const { return bands_[0].getAttack(); }

	/// Get the linked release value (from first band)
	[[nodiscard]] q31_t getLinkedRelease() const { return bands_[0].getRelease(); }

	/// Get the up/down skew value
	[[nodiscard]] q31_t getUpDownSkew() const { return upDownSkewKnob_; }

	// ========== Enable/Disable Zone ==========

	/// Check if multiband compressor is enabled
	[[nodiscard]] bool isEnabled() const { return enabledZone_ > (ONE_Q31 / 2); }

	/// Get the enabled zone value
	[[nodiscard]] q31_t getEnabledZone() const { return enabledZone_; }

	/// Set the enabled zone value
	void setEnabledZone(q31_t zone) { enabledZone_ = zone; }

	// ========== Linked Bandwidth ==========

	/// Get the linked bandwidth value (shared across all bands)
	[[nodiscard]] q31_t getLinkedBandwidth() const { return linkedBandwidth_; }

	/// Set the linked bandwidth value
	/// Shifts all per-band values by delta from previous linked value
	void setLinkedBandwidth(q31_t bw) {
		int64_t delta = static_cast<int64_t>(bw) - static_cast<int64_t>(linkedBandwidth_);
		linkedBandwidth_ = bw;
		for (size_t i = 0; i < kNumBands; ++i) {
			int64_t newVal = static_cast<int64_t>(bands_[i].getBandwidth()) + delta;
			bands_[i].setBandwidth(
			    static_cast<q31_t>(std::clamp(newVal, static_cast<int64_t>(0), static_cast<int64_t>(ONE_Q31))));
		}
	}

	// ========== Per-Band Offsets ==========

	/// Get threshold offset for a specific band (for XML persistence)
	/// Computes offset dynamically as (band_value - linked_value)
	[[nodiscard]] q31_t getThresholdOffset(size_t band) const {
		if (band >= kNumBands) {
			return 0;
		}
		return static_cast<q31_t>(static_cast<int64_t>(bands_[band].getThresholdDown())
		                          - static_cast<int64_t>(linkedThreshold_));
	}

	/// Set threshold offset for a specific band (for XML persistence)
	/// When loading from XML, applies offset to reach target per-band value
	void setThresholdOffset(size_t band, q31_t offset) {
		if (band < kNumBands) {
			// Apply offset to linked value to set per-band value
			q31_t net = static_cast<q31_t>(std::clamp(static_cast<int64_t>(linkedThreshold_) + offset,
			                                          static_cast<int64_t>(0), static_cast<int64_t>(ONE_Q31)));
			bands_[band].setThresholdDown(net);
		}
	}

	/// Get ratio offset for a specific band (for XML persistence)
	/// Computes offset dynamically as (band_value - linked_value)
	[[nodiscard]] q31_t getRatioOffset(size_t band) const {
		if (band >= kNumBands) {
			return 0;
		}
		return static_cast<q31_t>(static_cast<int64_t>(bands_[band].getRatioDown())
		                          - static_cast<int64_t>(linkedRatio_));
	}

	/// Set ratio offset for a specific band (for XML persistence)
	/// When loading from XML, applies offset to reach target per-band value
	void setRatioOffset(size_t band, q31_t offset) {
		if (band < kNumBands) {
			// Apply offset to linked value to set per-band value
			q31_t net = static_cast<q31_t>(std::clamp(static_cast<int64_t>(linkedRatio_) + offset,
			                                          static_cast<int64_t>(0), static_cast<int64_t>(ONE_Q31)));
			bands_[band].setRatioDown(net);
			bands_[band].setRatioUp(net);
		}
	}

	/// Get bandwidth offset for a specific band (for XML persistence)
	/// Computes offset dynamically as (band_value - linked_value)
	[[nodiscard]] q31_t getBandwidthOffset(size_t band) const {
		if (band >= kNumBands) {
			return 0;
		}
		return static_cast<q31_t>(static_cast<int64_t>(bands_[band].getBandwidth())
		                          - static_cast<int64_t>(linkedBandwidth_));
	}

	/// Set bandwidth offset for a specific band (for XML persistence)
	/// When loading from XML, applies offset to reach target per-band value
	void setBandwidthOffset(size_t band, q31_t offset) {
		if (band < kNumBands) {
			// Apply offset to linked value to set per-band value
			q31_t net = static_cast<q31_t>(std::clamp(static_cast<int64_t>(linkedBandwidth_) + offset,
			                                          static_cast<int64_t>(0), static_cast<int64_t>(ONE_Q31)));
			bands_[band].setBandwidth(net);
		}
	}

	// ========== Net Values (Actual Per-Band Values) ==========

	/// Get actual threshold for a specific band
	[[nodiscard]] q31_t getNetThreshold(size_t band) const {
		return (band < kNumBands) ? bands_[band].getThresholdDown() : 0;
	}

	/// Get actual ratio for a specific band
	[[nodiscard]] q31_t getNetRatio(size_t band) const { return (band < kNumBands) ? bands_[band].getRatioDown() : 0; }

	/// Get actual bandwidth for a specific band
	[[nodiscard]] q31_t getNetBandwidth(size_t band) const {
		return (band < kNumBands) ? bands_[band].getBandwidth() : 0;
	}

	/// Reset all filter and compressor states
	void reset() {
		crossoverAllpass1_.reset();
		crossoverAllpass2_.reset();
		crossoverAllpass3_.reset();
		crossoverLR2_.reset();
		crossoverLR2Fast_.reset();
		for (auto& band : bands_) {
			band.reset();
		}
		saturationStateL_.fill(0);
		saturationStateR_.fill(0);
		dcBlockL_.reset();
		dcBlockR_.reset();
	}

	/// Render the multiband compressor in-place
	/// Pure dynamics processor - output gain knob is the only gain control.
	/// At 1:1 ratio with output gain at unity, this is transparent.
	/// @param buffer Stereo audio buffer to process
	/// @param finalVolume Reference volume level for threshold calculation (log domain)
	void render(StereoBuffer<q31_t> buffer, q31_t finalVolume) {
		if (buffer.empty()) {
			return;
		}

// TODO:PROFILING-DELETE begin
#if MULTIBAND_PROFILE
		static Debug::OneOfN profTotal("MB_total", 1000);
		static Debug::OneOfN profXover("MB_xover", 1000);
		static Debug::OneOfN profEnv("MB_env", 1000);
		static Debug::OneOfN profRecomb("MB_recomb", 1000); // Gain apply is now fused into recombine
		profTotal.start();
		profXover.start();
#endif
		// TODO:PROFILING-DELETE end

		// Increment frame counter for gap detection in band compressors
		++frameCount_;

		// Store dry signal if blending
		static std::array<StereoSample<q31_t>, SSI_TX_BUFFER_NUM_SAMPLES> dryBuffer;
		if (wet_ != 1.0f) {
			std::copy(buffer.begin(), buffer.end(), dryBuffer.begin());
		}

		// Temporary buffers for each band
		static std::array<q31_t, SSI_TX_BUFFER_NUM_SAMPLES> bandBufferL[kNumBands];
		static std::array<q31_t, SSI_TX_BUFFER_NUM_SAMPLES> bandBufferR[kNumBands];

		// Split into bands using selected crossover type (ordered by CPU cost):
		// - 0: Allpass 1st order (6dB/oct) - cheapest, default
		// - 1: Allpass 2nd order (12dB/oct) - experimental
		// - 2: Allpass 3rd order (18dB/oct) - experimental
		// - 3: LR2 12dB/oct - most expensive
		// Separate loops per crossover type for better branch prediction and potential vectorization
		switch (crossoverType_) {
		case 1:
			for (size_t i = 0; i < buffer.size(); ++i) {
				filter::CrossoverBands bandsL, bandsR;
				crossoverAllpass2_.processStereo(buffer[i].l, buffer[i].r, bandsL, bandsR);
				bandBufferL[0][i] = bandsL.low;
				bandBufferL[1][i] = bandsL.mid;
				bandBufferL[2][i] = bandsL.high;
				bandBufferR[0][i] = bandsR.low;
				bandBufferR[1][i] = bandsR.mid;
				bandBufferR[2][i] = bandsR.high;
			}
			break;
		case 2:
			for (size_t i = 0; i < buffer.size(); ++i) {
				filter::CrossoverBands bandsL, bandsR;
				crossoverAllpass3_.processStereo(buffer[i].l, buffer[i].r, bandsL, bandsR);
				bandBufferL[0][i] = bandsL.low;
				bandBufferL[1][i] = bandsL.mid;
				bandBufferL[2][i] = bandsL.high;
				bandBufferR[0][i] = bandsR.low;
				bandBufferR[1][i] = bandsR.mid;
				bandBufferR[2][i] = bandsR.high;
			}
			break;
		case 3: // LR2 Fast - no phase compensation (4 filter ops/channel)
			for (size_t i = 0; i < buffer.size(); ++i) {
				filter::CrossoverBands bandsL, bandsR;
				crossoverLR2Fast_.processStereo(buffer[i].l, buffer[i].r, bandsL, bandsR);
				bandBufferL[0][i] = bandsL.low;
				bandBufferL[1][i] = bandsL.mid;
				bandBufferL[2][i] = bandsL.high;
				bandBufferR[0][i] = bandsR.low;
				bandBufferR[1][i] = bandsR.mid;
				bandBufferR[2][i] = bandsR.high;
			}
			break;
		case 4: // LR2 Full - with phase compensation (6 filter ops/channel)
			for (size_t i = 0; i < buffer.size(); ++i) {
				filter::CrossoverBands bandsL, bandsR;
				crossoverLR2_.processStereo(buffer[i].l, buffer[i].r, bandsL, bandsR);
				bandBufferL[0][i] = bandsL.low;
				bandBufferL[1][i] = bandsL.mid;
				bandBufferL[2][i] = bandsL.high;
				bandBufferR[0][i] = bandsR.low;
				bandBufferR[1][i] = bandsR.mid;
				bandBufferR[2][i] = bandsR.high;
			}
			break;
		case 0:
		default:
			for (size_t i = 0; i < buffer.size(); ++i) {
				filter::CrossoverBands bandsL, bandsR;
				crossoverAllpass1_.processStereo(buffer[i].l, buffer[i].r, bandsL, bandsR);
				bandBufferL[0][i] = bandsL.low;
				bandBufferL[1][i] = bandsL.mid;
				bandBufferL[2][i] = bandsL.high;
				bandBufferR[0][i] = bandsR.low;
				bandBufferR[1][i] = bandsR.mid;
				bandBufferR[2][i] = bandsR.high;
			}
			break;
		}

// TODO:PROFILING-DELETE begin
#if MULTIBAND_PROFILE
		profXover.stop();
		profEnv.start();
#endif
		// TODO:PROFILING-DELETE end

		// Calculate song volume in dB for threshold reference
		float songVolumedB = fastLog(static_cast<float>(finalVolume) + 1e-10f);

		// Process each band - envelope detection
		// updateLevel now combines L+R inline with 4x downsampling - no temp buffer needed
		std::array<float, kNumBands> bandGains;
		for (size_t b = 0; b < kNumBands; ++b) {
			// Calculate level for the band (L+R combined inline, 4x downsampled)
			bands_[b].updateLevel(bandBufferL[b].data(), bandBufferR[b].data(), buffer.size(), response_);

			// Calculate compression gain
			// Use character-derived knee and add per-band skew offset to global skew
			float bandSkew = std::clamp(upDownSkew_ + skewOffset_[b], -1.0f, 1.0f);
			bandGains[b] =
			    bands_[b].calculateGain(static_cast<float>(buffer.size()), songVolumedB, knee_, bandSkew, frameCount_);
		}

// TODO:PROFILING-DELETE begin
#if MULTIBAND_PROFILE
		profEnv.stop();
		profRecomb.start();
#endif
		// TODO:PROFILING-DELETE end

		// Fused gain apply + recombine loop
		// This combines compression gain, per-band output level, stereo width, and output gain
		// into a single pass over the data, reducing memory bandwidth

		// Pre-compute combined gain = compression gain * output level (4.28 format)
		// This fuses the gain apply loop into the recombine loop
		std::array<q31_t, kNumBands> bandCombinedGain;
		for (size_t b = 0; b < kNumBands; ++b) {
			float combined = bandGains[b] * bands_[b].getOutputLevelLinear();
			bandCombinedGain[b] = static_cast<q31_t>(combined * (1 << 28));
		}
		// Pre-compute output gain as fixed-point (4.28 format)
		q31_t outputGainFixed = static_cast<q31_t>(outputGain_ * (1 << 28));

		// Pre-compute stereo width as fixed-point for mid/high bands (bass is always mono)
		// Using Q31 format for width (0 = mono, ONE_Q31 = full stereo)
		q31_t widthFixed = static_cast<q31_t>(width_ * ONE_Q31f);

		int64_t truePeak = 0;                              // Track true peak before clamping for accurate metering
		std::array<q31_t, kNumBands> bandPeakThisBuffer{}; // Per-band peak tracking (post-level)
		const bool doMetering = meteringEnabled_;

		for (size_t i = 0; i < buffer.size(); ++i) {
			// Use 32-bit accumulator with multiply_32x32_rshift32 (like single-band compressor)
			// This is faster and uses ARM SMMUL/SMMLA instructions efficiently
			q31_t sumL = 0, sumR = 0;

			// Band 0 (bass): Always mono - skip M/S processing entirely
			// Fused: applies compression gain + output level in one multiply
			{
				q31_t L = bandBufferL[0][i];
				q31_t R = bandBufferR[0][i];
				q31_t mono = (L >> 1) + (R >> 1); // Sum to mono
				q31_t scaled = multiply_32x32_rshift32(mono, bandCombinedGain[0]) << 4;
				sumL += scaled;
				sumR += scaled;

				if (doMetering) {
					q31_t peak = (scaled < 0) ? -scaled : scaled;
					bandPeakThisBuffer[0] = std::max(bandPeakThisBuffer[0], peak);
				}
			}

			// Bands 1-2 (mid/high): Apply stereo width via M/S processing
			// Fused: applies compression gain + output level in one multiply
			for (size_t b = 1; b < kNumBands; ++b) {
				q31_t L = bandBufferL[b][i];
				q31_t R = bandBufferR[b][i];

				// M/S encoding: M = (L+R)/2, S = (L-R)/2
				// Width scaling: S_out = S * width (using fixed-point)
				// Decoding: L_out = M + S_out, R_out = M - S_out
				q31_t mid = (L >> 1) + (R >> 1);
				q31_t side = (L >> 1) - (R >> 1);
				q31_t sideScaled = multiply_32x32_rshift32(side, widthFixed);
				L = mid + sideScaled;
				R = mid - sideScaled;

				// Apply combined gain (compression + level) using 32-bit multiply
				q31_t scaledL = multiply_32x32_rshift32(L, bandCombinedGain[b]) << 4;
				q31_t scaledR = multiply_32x32_rshift32(R, bandCombinedGain[b]) << 4;
				sumL += scaledL;
				sumR += scaledR;

				// Track per-band peak only if metering is enabled
				if (doMetering) {
					q31_t absL = (scaledL < 0) ? -scaledL : scaledL;
					q31_t absR = (scaledR < 0) ? -scaledR : scaledR;
					bandPeakThisBuffer[b] = std::max(bandPeakThisBuffer[b], std::max(absL, absR));
				}
			}

			// Apply output gain using 32-bit multiply (matches single-band compressor pattern)
			q31_t outL = multiply_32x32_rshift32(sumL, outputGainFixed) << 4;
			q31_t outR = multiply_32x32_rshift32(sumR, outputGainFixed) << 4;

			// Track true peak (only if metering enabled)
			if (doMetering) {
				q31_t absL = (outL < 0) ? -outL : outL;
				q31_t absR = (outR < 0) ? -outR : outR;
				truePeak = std::max(truePeak, static_cast<int64_t>(std::max(absL, absR)));
			}

			// Apply DC-blocking high-pass filter (removes DC offset from saturation)
			buffer[i].l = outL - dcBlockL_.doFilter(outL, kDCBlockCoeff);
			buffer[i].r = outR - dcBlockR_.doFilter(outR, kDCBlockCoeff);
		}

		// Metering calculations - only run when analyzer is enabled
		if (doMetering) {
			// Update per-band output peaks with decay (~50ms)
			// This is cheap: just max and multiply per band
			for (size_t b = 0; b < kNumBands; ++b) {
				bandOutputPeak_[b] = std::max(bandPeakThisBuffer[b], static_cast<q31_t>(bandOutputPeak_[b] * 0.95f));
			}

			// Track output level using true peak (before clamping) for accurate metering
			q31_t bufferPeak = (truePeak > INT32_MAX) ? INT32_MAX : static_cast<q31_t>(truePeak);
			outputPeak_ = std::max(bufferPeak, static_cast<q31_t>(outputPeak_ * 0.95f));

			// Increment refresh counter - only do expensive calculations on refresh frames
			if (++meterRefreshCounter_ >= kMeterRefreshBuffers) {
				meterRefreshCounter_ = 0;
				meterNeedsRefresh_ = true;

				// Saturation detection with hold timer (~500ms) - only on refresh frames
				constexpr q31_t saturationThreshold = static_cast<q31_t>(EFFECTIVE_0DBFS_Q31 * 1.33);
				for (size_t b = 0; b < kNumBands; ++b) {
					if (bandOutputPeak_[b] > saturationThreshold) {
						bandSaturationHoldCounter_[b] = kIndicatorHoldBuffers / kMeterRefreshBuffers;
					}
					else if (bandSaturationHoldCounter_[b] > 0) {
						bandSaturationHoldCounter_[b]--;
					}
					bandSaturating_[b] = (bandSaturationHoldCounter_[b] > 0);
				}

				// Clipping detection with hold timer - only on refresh frames
				constexpr int64_t clipThreshold64 = static_cast<int64_t>(EFFECTIVE_0DBFS_Q31 * 1.33);
				if (outputPeak_ > static_cast<q31_t>(clipThreshold64)) {
					clippingHoldCounter_ = kIndicatorHoldBuffers / kMeterRefreshBuffers;
				}
				else if (clippingHoldCounter_ > 0) {
					clippingHoldCounter_--;
				}
				clipping_ = (clippingHoldCounter_ > 0);
			}
		}

		// Apply wet/dry blend
		if (wet_ != 1.0f) {
			for (size_t i = 0; i < buffer.size(); ++i) {
				buffer[i].l = static_cast<q31_t>(static_cast<float>(buffer[i].l) * wet_.raw() / ONE_Q31f
				                                 + static_cast<float>(dryBuffer[i].l) * dry_);
				buffer[i].r = static_cast<q31_t>(static_cast<float>(buffer[i].r) * wet_.raw() / ONE_Q31f
				                                 + static_cast<float>(dryBuffer[i].r) * dry_);
			}
		}

// TODO:PROFILING-DELETE begin
#if MULTIBAND_PROFILE
		profRecomb.stop();
		profTotal.stop();
#endif
		// TODO:PROFILING-DELETE end
	}

	/// Get combined gain reduction for display (average of all bands)
	[[nodiscard]] uint8_t getGainReduction() const {
		float totalReduction = 0.0f;
		for (const auto& band : bands_) {
			totalReduction += std::abs(band.getGainReductionDB());
		}
		return static_cast<uint8_t>(std::clamp(totalReduction * 4.0f * 4.0f / kNumBands, 0.0f, 127.0f));
	}

	/// Get gain change for a specific band (bipolar: -127 to +127)
	/// Negative = downward compression (gain reduction) → meter bars go DOWN
	/// Positive = upward compression (gain boost) → meter bars go UP
	[[nodiscard]] int8_t getBandGainReduction(size_t bandIndex) const {
		if (bandIndex >= kNumBands) {
			return 0;
		}
		// envelope_ is in natural log units, convert to dB:
		// dB = 20 * log10(exp(envelope_)) = 20 * envelope_ / ln(10) ≈ 8.686 * envelope_
		float envelope = bands_[bandIndex].getGainReductionDB();
		// Apply noise floor - ignore very small envelope values (< 0.1dB)
		if (std::abs(envelope) < 0.012f) { // ~0.1dB in natural log units
			return 0;
		}
		float grDB = envelope * 8.686f; // Convert from natural log to dB
		// Scale ±12dB to ±127, preserve sign so bars match gain direction
		float scaled = std::clamp(grDB * (127.0f / 12.0f), -127.0f, 127.0f);
		return static_cast<int8_t>(scaled);
	}

	/// Get input level for a specific band (0-127 scale for metering)
	/// dBFS-linear scale: -48dBFS = 0, -24dBFS = ~64 (half), 0dBFS = 127
	[[nodiscard]] uint8_t getBandInputLevel(size_t bandIndex) const {
		if (bandIndex >= kNumBands) {
			return 0;
		}
		// rms_ is log(level) where level is in raw sample space
		// Convert to dBFS: dBFS = 20 * log10(level / ONE_Q31) = 8.686 * (rms_ - 21.5)
		float level = bands_[bandIndex].getInputLevelLog();
		constexpr float refLevel = 21.5f; // log(ONE_Q31)
		float dBFS = 8.686f * (level - refLevel);
		// Noise floor at -48dBFS
		if (dBFS < -48.0f) {
			return 0;
		}
		// Linear mapping from [-48, 0] dBFS to [0, 127]
		float scaled = (dBFS + 48.0f) * (127.0f / 48.0f);
		return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 127.0f));
	}

	/// Get output level for a specific band (0-127 scale for metering)
	/// Shows pre-saturation signal level, dBFS scale: -48dBFS = 0, 0dBFS = 127
	/// Note: 0dBFS is calibrated to downstream clip point (~24dB below internal full scale)
	[[nodiscard]] uint8_t getBandOutputLevel(size_t bandIndex) const {
		if (bandIndex >= kNumBands) {
			return 0;
		}
		q31_t peak = bandOutputPeak_[bandIndex];
		if (peak < 1000) {
			return 0;
		}
		// Use system constant for downstream clipping point
		float peakNormalized = static_cast<float>(peak) / EFFECTIVE_0DBFS_Q31f;
		// 20 * log10(x) = 8.686 * ln(x) - use fastLog for consistency
		float dB = 8.686f * fastLog(peakNormalized + 1e-10f);
		// Range: -48dBFS (bottom) to 0dBFS (full scale)
		if (dB < -48.0f) {
			return 0;
		}
		float scaled = (dB + 48.0f) * (127.0f / 48.0f);
		return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 127.0f));
	}

	/// Get threshold position for metering (0.0 to 1.0, where 0=bottom, 1=top of meter)
	/// This represents where the threshold tick should be drawn on the band meter
	[[nodiscard]] float getBandThresholdPosition(size_t bandIndex) const {
		if (bandIndex >= kNumBands) {
			return 0.5f;
		}
		// thresholdDown_ is 0.2-1.0, map to 0-1 for meter position
		float threshold = bands_[bandIndex].getThresholdLog();
		return (threshold - 0.2f) / 0.8f;
	}

	/// Get output level for metering (0-127 scale)
	/// Range: -48dBFS = 0, 0dBFS = 127
	/// Note: 0dBFS is calibrated to downstream clip point (~24dB below internal full scale)
	[[nodiscard]] uint8_t getOutputLevel() const {
		if (outputPeak_ < 1000) {
			return 0;
		}
		// Use system constant for downstream clipping point
		float peakNormalized = static_cast<float>(outputPeak_) / EFFECTIVE_0DBFS_Q31f;
		// 20 * log10(x) = 8.686 * ln(x) - use fastLog for consistency
		float dB = 8.686f * fastLog(peakNormalized + 1e-10f);
		// Range: -48dBFS (bottom) to 0dBFS (full scale)
		if (dB < -48.0f) {
			return 0;
		}
		float scaled = (dB + 48.0f) * (127.0f / 48.0f);
		return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 127.0f));
	}

	/// Get raw output peak value for debugging
	[[nodiscard]] q31_t getOutputPeak() const { return outputPeak_; }

	/// Check if output is clipping (exceeded 0dBFS in recent buffer)
	[[nodiscard]] bool isClipping() const { return clipping_; }

	/// Clear the clipping indicator (also auto-clears after ~500ms)
	void clearClipping() {
		clipping_ = false;
		clippingHoldCounter_ = 0;
	}

	/// Check if a specific band is saturating (hitting the tanh soft clipper)
	[[nodiscard]] bool isBandSaturating(size_t bandIndex) const {
		if (bandIndex >= kNumBands) {
			return false;
		}
		return bandSaturating_[bandIndex];
	}

private:
	// Crossover filters - ordered by CPU cost (cheapest to most expensive)
	// Type 0: AllpassCrossoverLR1 - 6dB/oct (2 ops/ch), cheapest, default
	// Type 1: AllpassCrossoverLR2 - 12dB/oct (4 ops/ch)
	// Type 2: AllpassCrossoverLR3 - 18dB/oct (6 ops/ch)
	// Type 3: LR2CrossoverFast - 12dB/oct (4 ops/ch), no phase comp
	// Type 4: LR2CrossoverFull - 12dB/oct (6 ops/ch), with phase comp
	filter::AllpassCrossoverLR1 crossoverAllpass1_; // Type 0 - 6dB/oct (cheapest)
	filter::AllpassCrossoverLR2 crossoverAllpass2_; // Type 1 - 12dB/oct
	filter::AllpassCrossoverLR3 crossoverAllpass3_; // Type 2 - 18dB/oct
	filter::LR2CrossoverFast crossoverLR2Fast_;     // Type 3 - 12dB/oct no phase comp
	filter::LR2CrossoverFull crossoverLR2_;         // Type 4 - 12dB/oct with phase comp
	uint8_t crossoverType_ = 0;                     // Default to cheapest (1st order allpass)
	std::array<BandCompressor, kNumBands> bands_;
	FixedPoint<31> wet_{ONE_Q31};
	float dry_ = 0.0f;
	q31_t outputGainKnob_ = (ONE_Q31 / 5) * 3; // Default +12dB (~4x) to compensate for low band gains
	float outputGain_ = 4.18f;                 // Linear output gain multiplier (matches knob)

	// Character knob (replaces knee) - controls width, knee, timing, skew
	q31_t characterKnob_ = 0;        // Default 0 (Width zone start) - neutral settings
	bool characterComputed_ = false; // Cache flag: true after first setCharacter() call
	float width_ = 0.0f;             // Stereo width: 0=mono, 1=full (bass always mono)
	float knee_ = 0.2f;              // 0=hard, 1=soft (derived from character) - steepish default
	std::array<float, kNumBands> timingOffset_{0.0f, 0.0f, 0.0f}; // Per-band timing multiplier offset
	std::array<float, kNumBands> skewOffset_{0.0f, 0.0f, 0.0f};   // Per-band up/down skew offset

	q31_t upDownSkewKnob_ = ONE_Q31 / 2; // Default balanced
	float upDownSkew_ = 0.0f;            // -1=upward, 0=balanced, +1=downward

	// Response - controlled by Feel zone, not a separate knob
	float response_ = 0.5f; // 0=smooth/MAV, 1=punchy/peak

	// Vibe knob - controls phase relationships between oscillations in Feel
	q31_t vibeKnob_ = 0;                                             // Default 0 (Sync zone start)
	float vibePhaseWidth_ = 0.0f;                                    // Phase offset for width oscillation
	float vibePhaseKnee_ = 0.0f;                                     // Phase offset for knee oscillation
	std::array<float, kNumBands> vibePhaseTiming_{0.0f, 0.0f, 0.0f}; // Phase offsets for timing
	std::array<float, kNumBands> vibePhaseSkew_{0.0f, 0.0f, 0.0f};   // Phase offsets for skew

	// Enable/disable zone (0 = off, >ONE_Q31/2 = on)
	q31_t enabledZone_{0};

	// Linked values (global controls - per-band offsets are computed dynamically)
	q31_t linkedThreshold_{ONE_Q31 / 2};
	q31_t linkedRatio_{0};
	q31_t linkedBandwidth_{ONE_Q31 / 2};

	// Saturation state for antialiasing (per-band, per-channel)
	// Initialize to midpoint (2147483648 = 0x80000000) which represents zero signal
	// Using 0 causes DC offset because the 2D interpolation sees a transition from "large negative" to "zero"
	static constexpr uint32_t kSaturationNeutral = 2147483648u;
	std::array<uint32_t, kNumBands> saturationStateL_{kSaturationNeutral, kSaturationNeutral, kSaturationNeutral};
	std::array<uint32_t, kNumBands> saturationStateR_{kSaturationNeutral, kSaturationNeutral, kSaturationNeutral};

	// DC-blocking high-pass filter (removes DC offset introduced by saturation)
	// fc = 5Hz gives very low cutoff that only removes DC, not audio
	// hpfCoeff = tan(pi*fc/fs) / (1 + tan(pi*fc/fs)) ≈ fc/fs for small fc
	static constexpr q31_t kDCBlockCoeff = static_cast<q31_t>((5.0f / kSampleRate) * ONE_Q31);
	filter::BasicFilterComponent dcBlockL_;
	filter::BasicFilterComponent dcBlockR_;

	// Output metering state
	q31_t outputPeak_{0};                                        // Peak output level for metering
	bool clipping_{false};                                       // True if output exceeded 0dBFS recently
	uint8_t clippingHoldCounter_{0};                             // Hold counter for clip indicator
	std::array<bool, kNumBands> bandSaturating_{};               // Per-band saturation indicators
	std::array<uint8_t, kNumBands> bandSaturationHoldCounter_{}; // Hold counters for saturation indicators
	std::array<q31_t, kNumBands> bandOutputPeak_{};              // Per-band output peak levels

	// Hold time for indicators (~500ms at 345 buffers/sec)
	static constexpr uint8_t kIndicatorHoldBuffers = 170;

	// Frame counter for gap detection (passed to band compressors)
	uint32_t frameCount_{0};

	// UI refresh counter for meter animation (~10fps at 44.1kHz/128 samples)
	static constexpr uint8_t kMeterRefreshBuffers = 35;
	uint8_t meterRefreshCounter_{0};
	bool meterNeedsRefresh_{false}; // Set by audio path, cleared by UI

public:
	/// Check if meter display needs refresh (called by UI)
	/// Returns true once per refresh interval, then auto-clears
	[[nodiscard]] bool checkAndClearMeterRefresh() {
		if (meterNeedsRefresh_) {
			meterNeedsRefresh_ = false;
			return true;
		}
		return false;
	}

	/// Enable/disable metering calculations (saves CPU when analyzer is off)
	void setMeteringEnabled(bool enabled) { meteringEnabled_ = enabled; }
	[[nodiscard]] bool isMeteringEnabled() const { return meteringEnabled_; }

private:
	bool meteringEnabled_{true}; // Metering calculations enabled (can be disabled to save CPU)
};

} // namespace deluge::dsp
