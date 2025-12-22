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
#include "dsp/filter/allpass_crossover.h"
#include "dsp_ng/core/types.hpp"
#include "util/fixedpoint.h"
#include <array>
#include <cmath>
#include <span>

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
		attackMS_ = 0.5f + (std::exp(2.0f * float(attack) / ONE_Q31f) - 1.0f) * 10.0f;
		attack_ = (-1000.0f / kSampleRate) / attackMS_;
	}

	void setRelease(q31_t release) {
		releaseMS_ = 50.0f + (std::exp(2.0f * float(release) / ONE_Q31f) - 1.0f) * 50.0f;
		release_ = (-1000.0f / kSampleRate) / releaseMS_;
	}

	void setThresholdDown(q31_t t) {
		thresholdDownKnob_ = t;
		thresholdDown_ = 1.0f - 0.8f * (float(t) / ONE_Q31f);
	}

	void setThresholdUp(q31_t t) {
		thresholdUpKnob_ = t;
		thresholdUp_ = 0.2f + 0.8f * (float(t) / ONE_Q31f);
	}

	void setRatioDown(q31_t r) {
		ratioDownKnob_ = r;
		fractionDown_ = 0.5f + (float(r) / ONE_Q31f) / 2.0f;
	}

	void setRatioUp(q31_t r) {
		ratioUpKnob_ = r;
		fractionUp_ = 0.5f + (float(r) / ONE_Q31f) / 2.0f;
	}

	[[nodiscard]] q31_t getAttack() const { return static_cast<q31_t>(attackMS_ * ONE_Q31f / 70.0f); }
	[[nodiscard]] q31_t getRelease() const { return static_cast<q31_t>(releaseMS_ * ONE_Q31f / 400.0f); }
	[[nodiscard]] q31_t getThresholdDown() const { return thresholdDownKnob_; }
	[[nodiscard]] q31_t getThresholdUp() const { return thresholdUpKnob_; }
	[[nodiscard]] q31_t getRatioDown() const { return ratioDownKnob_; }
	[[nodiscard]] q31_t getRatioUp() const { return ratioUpKnob_; }

	/// Reset the compressor state
	void reset() {
		envelope_ = 0.0f;
		rms_ = 0.0f;
		mean_ = 0.0f;
	}

	/// Calculate gain adjustment for the band based on current RMS level.
	/// Returns a linear gain multiplier.
	/// @param numSamples Number of samples in the current buffer
	/// @param songVolumedB Reference volume level in dB (log scale)
	[[nodiscard]] float calculateGain(float numSamples, float songVolumedB) {
		float threshDowndB = songVolumedB * thresholdDown_;
		float threshUpdB = songVolumedB * thresholdUp_;

		// Downward compression: reduce gain when above threshold
		float over = std::max(0.0f, rms_ - threshDowndB);

		// Upward compression: boost gain when below threshold
		float under = std::max(0.0f, threshUpdB - rms_);

		// Combined target
		float target = -over * fractionDown_ + under * fractionUp_;

		// Run envelope follower
		envelope_ = runEnvelope(envelope_, target, numSamples);

		// Convert to linear gain, clamped to prevent overflow
		float gain = std::exp(envelope_);
		return std::clamp(gain, 0.1f, 10.0f);
	}

	/// Update the RMS level from the band's audio buffer
	void updateRMS(std::span<const q31_t> buffer) {
		q31_t sum = 0;
		for (q31_t sample : buffer) {
			q31_t s = std::abs(sample);
			sum += multiply_32x32_rshift32(s, s);
		}

		float ns = static_cast<float>(buffer.size());
		float newMean = (static_cast<float>(sum) / ONE_Q31f) / ns;
		mean_ = (newMean * ns + mean_) / (1.0f + ns);
		float rmsLinear = ONE_Q31 * std::sqrt(mean_);
		rms_ = std::log(std::max(rmsLinear, 1.0f));
	}

	/// Get current gain reduction in dB (for metering)
	[[nodiscard]] float getGainReductionDB() const { return envelope_; }

private:
	[[nodiscard]] float runEnvelope(float current, float target, float numSamples) const {
		float timeConstant = (target > current) ? attack_ : release_;
		return target + std::exp(timeConstant * numSamples) * (current - target);
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
	float mean_ = 0.0f;

	q31_t thresholdDownKnob_ = 0;
	q31_t thresholdUpKnob_ = 0;
	q31_t ratioDownKnob_ = 0;
	q31_t ratioUpKnob_ = 0;
};

/// 3-band multiband compressor with OTT-style upward/downward compression.
/// Uses allpass crossover for perfect phase-coherent band splitting.
class MultibandCompressor {
public:
	static constexpr int kNumBands = 3;

	enum class Band : uint8_t { Low = 0, Mid = 1, High = 2 };

	MultibandCompressor() {
		// Default OTT-style settings
		crossover_.setLowCrossover(200.0f);
		crossover_.setHighCrossover(2000.0f);

		// Set default parameters for each band
		for (auto& band : bands_) {
			band.setAttack(ONE_Q31 / 4);
			band.setRelease(ONE_Q31 / 4);
			band.setThresholdDown(ONE_Q31 / 2);
			band.setThresholdUp(ONE_Q31 / 2);
			band.setRatioDown(ONE_Q31 / 2);
			band.setRatioUp(ONE_Q31 / 2);
		}
	}

	/// Set crossover frequency between low and mid bands
	void setLowCrossover(float freqHz) { crossover_.setLowCrossover(freqHz); }

	/// Set crossover frequency between mid and high bands
	void setHighCrossover(float freqHz) { crossover_.setHighCrossover(freqHz); }

	/// Get low crossover frequency in Hz
	[[nodiscard]] float getLowCrossoverHz() const { return crossover_.getLowCrossoverHz(); }

	/// Get high crossover frequency in Hz
	[[nodiscard]] float getHighCrossoverHz() const { return crossover_.getHighCrossoverHz(); }

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

	/// Reset all filter and compressor states
	void reset() {
		crossover_.reset();
		for (auto& band : bands_) {
			band.reset();
		}
	}

	/// Render the multiband compressor in-place
	/// @param buffer Stereo audio buffer to process
	/// @param finalVolume Reference volume level (same as RMSFeedbackCompressor)
	void render(StereoBuffer<q31_t> buffer, q31_t finalVolume) {
		if (buffer.empty()) {
			return;
		}

		// Store dry signal if blending
		static std::array<StereoSample<q31_t>, SSI_TX_BUFFER_NUM_SAMPLES> dryBuffer;
		if (wet_ != 1.0f) {
			std::copy(buffer.begin(), buffer.end(), dryBuffer.begin());
		}

		// Temporary buffers for each band
		static std::array<q31_t, SSI_TX_BUFFER_NUM_SAMPLES> bandBufferL[kNumBands];
		static std::array<q31_t, SSI_TX_BUFFER_NUM_SAMPLES> bandBufferR[kNumBands];

		// Split into bands
		for (size_t i = 0; i < buffer.size(); ++i) {
			filter::AllpassCrossover::Bands bandsL, bandsR;
			crossover_.processStereo(buffer[i].l, buffer[i].r, bandsL, bandsR);

			bandBufferL[0][i] = bandsL.low;
			bandBufferL[1][i] = bandsL.mid;
			bandBufferL[2][i] = bandsL.high;
			bandBufferR[0][i] = bandsR.low;
			bandBufferR[1][i] = bandsR.mid;
			bandBufferR[2][i] = bandsR.high;
		}

		// Calculate song volume in dB for threshold reference
		float songVolumedB = std::log(static_cast<float>(finalVolume) + 1e-10f);

		// Process each band
		for (size_t b = 0; b < kNumBands; ++b) {
			// Calculate RMS for the band (combine L+R)
			std::array<q31_t, SSI_TX_BUFFER_NUM_SAMPLES> combinedBuffer;
			for (size_t i = 0; i < buffer.size(); ++i) {
				combinedBuffer[i] = (bandBufferL[b][i] >> 1) + (bandBufferR[b][i] >> 1);
			}
			bands_[b].updateRMS(std::span(combinedBuffer.data(), buffer.size()));

			// Calculate and apply gain
			float gain = bands_[b].calculateGain(static_cast<float>(buffer.size()), songVolumedB);
			q31_t gainFixed = static_cast<q31_t>(gain * (1 << 27)); // 4.27 format

			for (size_t i = 0; i < buffer.size(); ++i) {
				bandBufferL[b][i] = multiply_32x32_rshift32(bandBufferL[b][i], gainFixed) << 4;
				bandBufferR[b][i] = multiply_32x32_rshift32(bandBufferR[b][i], gainFixed) << 4;
			}
		}

		// Recombine bands
		for (size_t i = 0; i < buffer.size(); ++i) {
			buffer[i].l = bandBufferL[0][i] + bandBufferL[1][i] + bandBufferL[2][i];
			buffer[i].r = bandBufferR[0][i] + bandBufferR[1][i] + bandBufferR[2][i];
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
	}

	/// Get combined gain reduction for display (average of all bands)
	[[nodiscard]] uint8_t getGainReduction() const {
		float totalReduction = 0.0f;
		for (const auto& band : bands_) {
			totalReduction += std::abs(band.getGainReductionDB());
		}
		return static_cast<uint8_t>(std::clamp(totalReduction * 4.0f * 4.0f / kNumBands, 0.0f, 127.0f));
	}

private:
	filter::AllpassCrossover crossover_;
	std::array<BandCompressor, kNumBands> bands_;
	FixedPoint<31> wet_{ONE_Q31};
	float dry_ = 0.0f;
};

} // namespace deluge::dsp
