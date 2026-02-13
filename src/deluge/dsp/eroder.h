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
#include "util/functions.h"
#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

class Serializer;
class Deserializer;

namespace deluge::dsp {

// ============================================================================
// Constants
// ============================================================================

inline constexpr int32_t kEroderNumZones = 8;

// Maximum delay line length in samples (~1.45ms at 44.1kHz)
// Must be power of 2 for fast modulo via bitmask
inline constexpr int32_t kEroderMaxDelay = 64;
inline constexpr int32_t kEroderDelayMask = kEroderMaxDelay - 1;

// Base delay times per frequency zone (in samples)
// Shorter delay = higher frequency comb notches
// First notch at f = sampleRate / delaySamples
inline constexpr int32_t kEroderBaseDelay[kEroderNumZones] = {
    60, // Zone 0: Sub    → first notch ~735 Hz
    44, // Zone 1: Bass   → first notch ~1003 Hz
    30, // Zone 2: Low    → first notch ~1470 Hz
    20, // Zone 3: Mid    → first notch ~2205 Hz
    13, // Zone 4: High   → first notch ~3392 Hz
    8,  // Zone 5: Air    → first notch ~5513 Hz
    4,  // Zone 6: Bright → first notch ~11025 Hz
    30, // Zone 7: Full   → mid delay, wide modulation
};

// Smoothing alpha per buffer (~0.05)
inline constexpr q31_t kEroderSmoothingAlpha = static_cast<q31_t>(0.05f * ONE_Q31);

// Pink noise: 4 octave bands via Voss-McCartney
inline constexpr int32_t kNumPinkBands = 4;
inline constexpr int32_t kPinkBandShift = 2; // >> 2 = divide by 4 when summing

// Sine character: phase increment for ~1kHz at 44100 Hz sample rate
inline constexpr uint32_t kSinePhaseInc = 97391472u;

// ============================================================================
// Character Zones
// ============================================================================

enum class EroderCharacter : int32_t {
	WHITE = 0,  // Raw white noise
	PINK = 1,   // Voss-McCartney pink noise (-3dB/oct)
	BROWN = 2,  // Integrated white noise (-6dB/oct)
	BLUE = 3,   // Differentiated white noise (+3dB/oct)
	SINE = 4,   // Triangle wave modulator (~1kHz)
	RING = 5,   // Input * noise (signal-dependent erosion)
	SPARSE = 6, // Sample & hold noise (digital artifacts)
	SMOOTH = 7, // Lowpass filtered noise (controlled bandwidth)
};

// ============================================================================
// Noise State
// ============================================================================

struct EroderNoiseState {
	// Brown noise integration accumulators
	q31_t brownL{0};
	q31_t brownR{0};

	// Blue noise previous samples
	q31_t prevNoiseL{0};
	q31_t prevNoiseR{0};

	// Pink noise (Voss-McCartney, 4 octave bands per channel)
	q31_t pinkBandsL[kNumPinkBands]{};
	q31_t pinkBandsR[kNumPinkBands]{};
	uint32_t pinkCounter{0};

	// Sine/triangle oscillator phase
	uint32_t sinePhase{0};

	// Sparse S&H state
	q31_t heldValueL{0};
	q31_t heldValueR{0};
	uint32_t shCounter{0};

	// Smooth LPF state
	q31_t smoothL{0};
	q31_t smoothR{0};

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
	uint8_t depth{0}; // Modulation depth (0=off/bypass, 1-127)
	uint8_t mix{64};  // Wet/dry blend (0=dry, 127=wet)

	// DSP state
	EroderDelayLine delay;
	EroderNoiseState noise;

	// Smoothing state (per-buffer interpolation)
	q31_t smoothedFreq{0};
	q31_t smoothedCharacter{0};

	[[nodiscard]] bool isEnabled() const { return depth > 0; }

	void writeToFile(Serializer& writer) const {
		WRITE_FIELD(writer, depth, "eroderDepth");
		WRITE_FIELD_DEFAULT(writer, mix, "eroderMix", 64);
		WRITE_ZONE(writer, freq.value, "eroderFreq");
		WRITE_ZONE(writer, character.value, "eroderChar");
	}

	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, depth, "eroderDepth");
		READ_FIELD(reader, tagName, mix, "eroderMix");
		READ_ZONE(reader, tagName, freq.value, "eroderFreq");
		READ_ZONE(reader, tagName, character.value, "eroderChar");
		return false;
	}
};

// ============================================================================
// Noise Generation Helpers
// ============================================================================

/// Voss-McCartney pink noise: update one octave band per sample
[[gnu::always_inline]] inline q31_t generatePinkNoise(q31_t bands[kNumPinkBands], uint32_t counter) {
	int band = __builtin_ctz(counter | (1u << kNumPinkBands));
	if (band < kNumPinkBands) {
		bands[band] = getNoise();
	}
	q31_t sum = 0;
	for (int32_t i = 0; i < kNumPinkBands; i++) {
		sum = add_saturate(sum, bands[i] >> kPinkBandShift);
	}
	return sum;
}

/// Brown noise: leaky integration of white noise (-6dB/oct above ~55Hz)
[[gnu::always_inline]] inline q31_t generateBrownNoise(q31_t& state) {
	q31_t white = getNoise() >> 4;
	state = add_saturate(state, white);
	state -= state >> 7;
	return state;
}

/// Blue noise: differentiate white noise (+3dB/oct)
[[gnu::always_inline]] inline q31_t generateBlueNoise(q31_t& prevNoise) {
	q31_t white = getNoise();
	q31_t blue = white - prevNoise;
	prevNoise = white;
	return blue;
}

// ============================================================================
// Delay Time Computation
// ============================================================================

/// Compute base delay time from frequency zone value
/// Interpolates between adjacent zone delays
[[gnu::always_inline]] inline float computeEroderDelay(q31_t freqValue) {
	ZoneInfo zone = computeZoneQ31(freqValue, kEroderNumZones);
	float baseDelay = static_cast<float>(kEroderBaseDelay[zone.index]);

	if (zone.index < kEroderNumZones - 1) {
		float nextDelay = static_cast<float>(kEroderBaseDelay[zone.index + 1]);
		baseDelay += (nextDelay - baseDelay) * zone.position;
	}
	return baseDelay;
}

// ============================================================================
// Noise Routing
// ============================================================================

/// Generate stereo noise pair based on character zone
inline std::pair<q31_t, q31_t> generateEroderNoise(EroderNoiseState& state, int32_t charZone, q31_t inputL,
                                                   q31_t inputR) {
	q31_t noiseL, noiseR;

	switch (static_cast<EroderCharacter>(charZone)) {
	case EroderCharacter::WHITE:
		noiseL = getNoise();
		noiseR = getNoise();
		break;

	case EroderCharacter::PINK:
		state.pinkCounter++;
		noiseL = generatePinkNoise(state.pinkBandsL, state.pinkCounter);
		noiseR = generatePinkNoise(state.pinkBandsR, state.pinkCounter + 7);
		break;

	case EroderCharacter::BROWN:
		noiseL = generateBrownNoise(state.brownL);
		noiseR = generateBrownNoise(state.brownR);
		break;

	case EroderCharacter::BLUE:
		noiseL = generateBlueNoise(state.prevNoiseL);
		noiseR = generateBlueNoise(state.prevNoiseR);
		break;

	case EroderCharacter::SINE: {
		state.sinePhase += kSinePhaseInc;
		int32_t saw = static_cast<int32_t>(state.sinePhase);
		q31_t halfTri = saw < 0 ? ~saw : saw;
		noiseL = halfTri - (ONE_Q31 >> 1);
		noiseR = noiseL;
		break;
	}

	case EroderCharacter::RING:
		noiseL = multiply_32x32_rshift32(inputL, getNoise()) << 1;
		noiseR = multiply_32x32_rshift32(inputR, getNoise()) << 1;
		break;

	case EroderCharacter::SPARSE: {
		state.shCounter++;
		if ((state.shCounter & 7) == 0) {
			state.heldValueL = getNoise();
			state.heldValueR = getNoise();
		}
		noiseL = state.heldValueL;
		noiseR = state.heldValueR;
		break;
	}

	case EroderCharacter::SMOOTH: {
		constexpr q31_t kSmoothAlpha = 0x08F5C28F;
		q31_t rawL = getNoise();
		q31_t rawR = getNoise();
		state.smoothL += multiply_32x32_rshift32(rawL - state.smoothL, kSmoothAlpha) << 1;
		state.smoothR += multiply_32x32_rshift32(rawR - state.smoothR, kSmoothAlpha) << 1;
		noiseL = state.smoothL;
		noiseR = state.smoothR;
		break;
	}

	default:
		noiseL = getNoise();
		noiseR = getNoise();
		break;
	}

	return {noiseL, noiseR};
}

// ============================================================================
// Buffer Processing
// ============================================================================

/// Process eroder effect on audio buffer in place
///
/// Core algorithm: noise-modulated short delay line creates comb filtering.
/// Noise modulates the delay read position around a base delay time set by
/// the frequency zone. The character zone selects the noise type/color.
///
/// @param buffer Audio buffer (stereo, modified in place)
/// @param params EroderParams (knob values + DSP state)
/// @param freqPreset Freq zone from patched param
/// @param freqCables Freq modulation from cables
/// @param charPreset Character zone from patched param
/// @param charCables Character modulation from cables
inline void processEroder(std::span<StereoSample> buffer, EroderParams& params, q31_t freqPreset, q31_t freqCables,
                          q31_t charPreset, q31_t charCables) {
	if (!params.isEnabled() || buffer.empty()) {
		return;
	}

	// Combine preset + cables using zone-aware scaling
	q31_t freqValue = params.freq.combinePresetAndCables(freqPreset, freqCables);
	q31_t charValue = params.character.combinePresetAndCables(charPreset, charCables);

	// Smooth zone params (per-buffer interpolation)
	params.smoothedFreq += multiply_32x32_rshift32(freqValue - params.smoothedFreq, kEroderSmoothingAlpha) << 1;
	params.smoothedCharacter += multiply_32x32_rshift32(charValue - params.smoothedCharacter, kEroderSmoothingAlpha)
	                            << 1;

	// Compute base delay time from smoothed freq (in fractional samples)
	float baseDelay = computeEroderDelay(params.smoothedFreq);

	// Get character zone index for noise type selection
	ZoneInfo charZone = computeZoneQ31(params.smoothedCharacter, kEroderNumZones);

	// Depth scaling: maps 0-127 to 0.0-1.0 modulation range
	// At max depth, noise can swing delay by ±baseDelay/2
	float depthFloat = static_cast<float>(params.depth) / 127.0f;
	float maxMod = baseDelay * 0.5f * depthFloat;

	// Mix scaling: 0-127 → q31
	q31_t mixWet = static_cast<q31_t>(params.mix) << 24;
	q31_t mixDry = ONE_Q31 - mixWet;

	for (auto& sample : buffer) {
		// Write current input to delay line
		params.delay.write(sample.l, sample.r);

		// Generate stereo noise pair
		auto [noiseL, noiseR] = generateEroderNoise(params.noise, charZone.index, sample.l, sample.r);

		// Convert noise from q31 to float modulation offset
		float modL = static_cast<float>(noiseL) / static_cast<float>(ONE_Q31) * maxMod;
		float modR = static_cast<float>(noiseR) / static_cast<float>(ONE_Q31) * maxMod;

		// Compute modulated delay times (clamp to valid range)
		float delayL = std::clamp(baseDelay + modL, 1.0f, static_cast<float>(kEroderMaxDelay - 1));
		float delayR = std::clamp(baseDelay + modR, 1.0f, static_cast<float>(kEroderMaxDelay - 1));

		// Read L and R at their respective modulated delay times
		q31_t wetL = params.delay.readL(delayL);
		q31_t wetR = params.delay.readR(delayR);

		// Wet/dry mix
		sample.l =
		    add_saturate(multiply_32x32_rshift32(sample.l, mixDry) << 1, multiply_32x32_rshift32(wetL, mixWet) << 1);
		sample.r =
		    add_saturate(multiply_32x32_rshift32(sample.r, mixDry) << 1, multiply_32x32_rshift32(wetR, mixWet) << 1);
	}
}

} // namespace deluge::dsp
