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

#include "definitions_cxx.hpp"
#include "dsp/stereo_sample.h"
#include "dsp/zone_param.hpp"
#include "storage/field_serialization.h"
#include "util/fixedpoint.h"
#include <cstdint>
#include <span>

class Serializer;
class Deserializer;

namespace deluge::dsp {

// ============================================================================
// Constants
// ============================================================================

inline constexpr int32_t kEroderNumZones = 8;

// Maximum delay line length in samples (~5.8ms at 44.1kHz)
// Must be power of 2 for fast modulo via bitmask
inline constexpr int32_t kEroderMaxDelay = 256;
inline constexpr int32_t kEroderDelayMask = kEroderMaxDelay - 1;

// Smoothing alpha per buffer (~0.05)
inline constexpr q31_t kEroderSmoothingAlpha = static_cast<q31_t>(0.05f * ONE_Q31);

// ============================================================================
// Noise + SVF State
// ============================================================================

struct EroderNoiseState {
	// SVF filter state (stereo)
	q31_t svfLowL{0};
	q31_t svfLowR{0};
	q31_t svfBandL{0};
	q31_t svfBandR{0};

	// S&H state (zero-crossing triggered)
	q31_t heldL{0};
	q31_t heldR{0};
	q31_t prevInputL{0};
	q31_t prevFilteredL{0};

	void reset() { *this = EroderNoiseState{}; }
};

// ============================================================================
// Delay Line State
// ============================================================================

struct EroderDelayLine {
	q31_t bufferL[kEroderMaxDelay]{};
	q31_t bufferR[kEroderMaxDelay]{};
	int32_t writePos{0};

	void reset() { *this = EroderDelayLine{}; }

	/// Write a stereo sample into the delay line
	[[gnu::always_inline]] void write(q31_t left, q31_t right) {
		bufferL[writePos] = left;
		bufferR[writePos] = right;
		writePos = (writePos + 1) & kEroderDelayMask;
	}

	/// Read single channel from delay line with fractional delay (linear interpolation)
	[[gnu::always_inline]] static q31_t readChannel(const q31_t* buf, int32_t wp, float delaySamples) {
		int32_t delayInt = static_cast<int32_t>(delaySamples);
		float frac = delaySamples - static_cast<float>(delayInt);

		int32_t idx0 = (wp - 1 - delayInt) & kEroderDelayMask;
		int32_t idx1 = (idx0 - 1) & kEroderDelayMask;

		q31_t fracQ = static_cast<q31_t>(frac * ONE_Q31);
		q31_t oneMinusFrac = ONE_Q31 - fracQ;

		return add_saturate(multiply_32x32_rshift32(buf[idx0], oneMinusFrac) << 1,
		                    multiply_32x32_rshift32(buf[idx1], fracQ) << 1);
	}

	/// Read L channel at given delay
	[[gnu::always_inline]] q31_t readL(float delaySamples) const {
		return readChannel(bufferL, writePos, delaySamples);
	}

	/// Read R channel at given delay
	[[gnu::always_inline]] q31_t readR(float delaySamples) const {
		return readChannel(bufferR, writePos, delaySamples);
	}
};

// ============================================================================
// EroderParams
// ============================================================================

struct EroderParams {
	// Zone parameters
	ZoneBasedParam<kEroderNumZones, true> freq;       // Frequency zone (clips to boundaries)
	ZoneBasedParam<kEroderNumZones, false> character; // Character zone (continuous across zones)

	// User-facing knob values
	uint8_t mix{0}; // Wet/dry mix (0=off/bypass, 1-127)

	// DSP state
	EroderDelayLine delay;
	EroderNoiseState noise;
	q31_t feedbackL{0};
	q31_t feedbackR{0};

	// Pitch tracking cache (recomputed when noteCode changes)
	int32_t prevNoteCode{-1};
	int32_t cachedPitchRatioQ16{1 << 16}; // 1.0 in Q16.16 fixed point

	// Smoothing state (per-buffer interpolation)
	q31_t smoothedFreq{0};
	q31_t smoothedCharacter{0};

	// Phase offsets (push+twist on zone encoders for phi triangle evaluation)
	float freqPhaseOffset{0};
	float charPhaseOffset{0};
	float gammaPhase{0};

	/// Effective phase for freq: freqPhaseOffset + 1024*gammaPhase
	[[nodiscard]] double effectiveFreq() const {
		return static_cast<double>(freqPhaseOffset) + 1024.0 * static_cast<double>(gammaPhase);
	}

	/// Effective phase for character: charPhaseOffset + 1024*gammaPhase
	[[nodiscard]] double effectiveChar() const {
		return static_cast<double>(charPhaseOffset) + 1024.0 * static_cast<double>(gammaPhase);
	}

	[[nodiscard]] bool isEnabled() const { return mix > 0; }

	void writeToFile(Serializer& writer) const {
		WRITE_FIELD(writer, mix, "eroderMix");
		WRITE_ZONE(writer, freq.value, "eroderFreq");
		WRITE_ZONE(writer, character.value, "eroderChar");
		WRITE_FLOAT(writer, freqPhaseOffset, "eroderFreqPhase", 10.0f);
		WRITE_FLOAT(writer, charPhaseOffset, "eroderCharPhase", 10.0f);
		WRITE_FLOAT(writer, gammaPhase, "eroderGamma", 10.0f);
	}

	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, mix, "eroderMix");
		READ_ZONE(reader, tagName, freq.value, "eroderFreq");
		READ_ZONE(reader, tagName, character.value, "eroderChar");
		READ_FLOAT(reader, tagName, freqPhaseOffset, "eroderFreqPhase", 10.0f);
		READ_FLOAT(reader, tagName, charPhaseOffset, "eroderCharPhase", 10.0f);
		READ_FLOAT(reader, tagName, gammaPhase, "eroderGamma", 10.0f);
		return false;
	}
};

// ============================================================================
// Buffer Processing (implementation in eroder.cpp)
// ============================================================================

void processEroder(std::span<StereoSample> buffer, EroderParams& params, q31_t freqPreset, q31_t freqCables,
                   q31_t charPreset, q31_t charCables, q31_t cutoffValue, int32_t noteCode = -1);

} // namespace deluge::dsp
