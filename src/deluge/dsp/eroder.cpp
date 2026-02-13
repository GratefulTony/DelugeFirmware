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

#include "dsp/eroder.h"
#include "io/debug/fx_benchmark.h"
#include "util/functions.h"
#include <algorithm>
#include <utility>

namespace deluge::dsp {

namespace {

// ============================================================================
// Noise Generation Helpers
// ============================================================================

/// Voss-McCartney pink noise: update one octave band per sample
q31_t generatePinkNoise(q31_t bands[kNumPinkBands], uint32_t counter) {
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
q31_t generateBrownNoise(q31_t& state) {
	q31_t white = getNoise() >> 4;
	state = add_saturate(state, white);
	state -= state >> 7;
	return state;
}

/// Blue noise: differentiate white noise (+3dB/oct)
q31_t generateBlueNoise(q31_t& prevNoise) {
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
float computeEroderDelay(q31_t freqValue) {
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
std::pair<q31_t, q31_t> generateEroderNoise(EroderNoiseState& state, int32_t charZone, q31_t inputL, q31_t inputR) {
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

// Benchmark tag strings for character zones
constexpr const char* kCharacterTagNames[] = {"white", "pink", "brown", "blue", "sine", "ring", "sparse", "smooth"};

} // anonymous namespace

// ============================================================================
// Buffer Processing
// ============================================================================

void processEroder(std::span<StereoSample> buffer, EroderParams& params, q31_t freqPreset, q31_t freqCables,
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

	// Benchmark with character zone tag
	FX_BENCH_DECLARE(bench, "eroder");
	FX_BENCH_SET_TAG(bench, 0, kCharacterTagNames[charZone.index]);
	FX_BENCH_SCOPE(bench);

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
