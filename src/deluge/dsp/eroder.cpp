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
#include "dsp/phi_triangle.hpp"
#include "io/debug/fx_benchmark.h"
#include "util/functions.h"
#include <algorithm>

namespace phi = deluge::dsp::phi;

namespace deluge::dsp {

namespace {

// ============================================================================
// Phi Triangle Banks
// ============================================================================

// Freq bank: delay time from phi triangle
constexpr std::array<phi::PhiTriConfig, 1> kEroderFreqBank = {{
    {phi::kPhi150, 0.7f, 0.0f, false}, // [0] Delay time: maps to [1, kEroderMaxDelay-1]
}};

// Character bank: SVF cutoff, resonance, S&H blend, filter output select
constexpr std::array<phi::PhiTriConfig, 5> kEroderCharBank = {{
    {phi::kPhi150, 0.7f, 0.0f, false}, // [0] SVF cutoff (0→1, quadratic mapped)
    {phi::kPhi175, 0.5f, 0.3f, false}, // [1] SVF resonance (0→1, high = tonal)
    {phi::kPhi100, 0.4f, 0.7f, false}, // [2] S&H blend (0=white, 1=S&H)
    {phi::kPhi125, 0.6f, 0.1f, true},  // [3] LP↔HP select (bipolar: -1=LP, +1=HP)
    {phi::kPhi200, 0.4f, 0.5f, true},  // [4] (LP/HP)↔BP select (bipolar: -1=LP/HP, +1=BP)
}};

float computeEroderDelay(double freqPhase) {
	auto results = phi::evalTriangleBank<1>(freqPhase, 1.0f, kEroderFreqBank);
	return 1.0f + results[0] * static_cast<float>(kEroderMaxDelay - 2);
}

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

	// Freq phi phase → delay time
	double freqNorm = static_cast<double>(params.smoothedFreq) / static_cast<double>(ONE_Q31);
	double freqPhase = freqNorm + params.effectiveFreq();
	float baseDelay = computeEroderDelay(freqPhase);

	// Character phi phase → SVF cutoff, resonance, S&H blend, output select
	double charNorm = static_cast<double>(params.smoothedCharacter) / static_cast<double>(ONE_Q31);
	double charPhase = charNorm + params.effectiveChar();
	auto charResults = phi::evalTriangleBank<5>(charPhase, 1.0f, kEroderCharBank);

	// SVF cutoff: quadratic mapping for perceptual linearity
	// Floor at 0.03 (~200Hz) so filter always passes signal; cap at 0.85 for stability
	float cutoffRaw = charResults[0];
	float cutoffMapped = 0.03f + cutoffRaw * cutoffRaw * 0.82f;
	q31_t svfF = static_cast<q31_t>(cutoffMapped * static_cast<float>(ONE_Q31));

	// SVF resonance: Q = 1 - resonance (low Q = resonant, near self-oscillation for tonal noise)
	// Cap at 0.99 so resonant peak dominates over broadband noise excitation
	float resonance = charResults[1] * 0.99f;
	q31_t svfQ = static_cast<q31_t>((1.0f - resonance) * static_cast<float>(ONE_Q31));

	// S&H blend: 0 = pure white noise, 1 = pure S&H
	q31_t shBlendQ = static_cast<q31_t>(charResults[2] * static_cast<float>(ONE_Q31));
	q31_t whiteBlendQ = ONE_Q31 - shBlendQ;

	// SVF output blend: two bipolar params → three weights (LP, BP, HP) summing to 1.0
	// Param [3] bipolar: -1=LP, +1=HP → h = (param+1)/2
	// Param [4] bipolar: -1=LP/HP blend, +1=BP → b = (param+1)/2
	// Weights: LP=(1-h)(1-b), HP=h(1-b), BP=b
	float h = (charResults[3] + 1.0f) * 0.5f;
	float b = (charResults[4] + 1.0f) * 0.5f;
	q31_t lpMixQ = static_cast<q31_t>((1.0f - h) * (1.0f - b) * static_cast<float>(ONE_Q31));
	q31_t hpMixQ = static_cast<q31_t>(h * (1.0f - b) * static_cast<float>(ONE_Q31));
	q31_t bpMixQ = static_cast<q31_t>(b * static_cast<float>(ONE_Q31));

	FX_BENCH_DECLARE(bench, "eroder");
	FX_BENCH_SCOPE(bench);

	// Depth and mix scaling
	float depthFloat = static_cast<float>(params.depth) / 127.0f;
	float maxMod = baseDelay * 0.5f * depthFloat;
	q31_t mixWet = static_cast<q31_t>(params.mix) << 24;
	q31_t mixDry = ONE_Q31 - mixWet;

	for (auto& sample : buffer) {
		params.delay.write(sample.l, sample.r);

		// Generate white noise
		q31_t whiteL = getNoise();
		q31_t whiteR = getNoise();

		// S&H: trigger on input or SVF zero crossing
		bool inputCross = (sample.l ^ params.noise.prevInputL) < 0;
		bool filterCross = (params.noise.svfLowL ^ params.noise.prevFilteredL) < 0;
		if (inputCross || filterCross) {
			params.noise.heldL = getNoise();
			params.noise.heldR = getNoise();
		}
		params.noise.prevInputL = sample.l;
		params.noise.prevFilteredL = params.noise.svfLowL;

		// Blend white noise and S&H
		q31_t noiseL = add_saturate(multiply_32x32_rshift32(whiteL, whiteBlendQ) << 1,
		                            multiply_32x32_rshift32(params.noise.heldL, shBlendQ) << 1);
		q31_t noiseR = add_saturate(multiply_32x32_rshift32(whiteR, whiteBlendQ) << 1,
		                            multiply_32x32_rshift32(params.noise.heldR, shBlendQ) << 1);

		// SVF filter (2-pole, LP output with resonance)
		q31_t highL = noiseL - params.noise.svfLowL - (multiply_32x32_rshift32(params.noise.svfBandL, svfQ) << 1);
		params.noise.svfBandL += multiply_32x32_rshift32(highL, svfF) << 1;
		params.noise.svfLowL += multiply_32x32_rshift32(params.noise.svfBandL, svfF) << 1;

		q31_t highR = noiseR - params.noise.svfLowR - (multiply_32x32_rshift32(params.noise.svfBandR, svfQ) << 1);
		params.noise.svfBandR += multiply_32x32_rshift32(highR, svfF) << 1;
		params.noise.svfLowR += multiply_32x32_rshift32(params.noise.svfBandR, svfF) << 1;

		// Blend SVF outputs: LP*(1-h)(1-b) + HP*h(1-b) + BP*b
		q31_t filteredL = add_saturate(add_saturate(multiply_32x32_rshift32(params.noise.svfLowL, lpMixQ) << 1,
		                                            multiply_32x32_rshift32(highL, hpMixQ) << 1),
		                               multiply_32x32_rshift32(params.noise.svfBandL, bpMixQ) << 1);
		q31_t filteredR = add_saturate(add_saturate(multiply_32x32_rshift32(params.noise.svfLowR, lpMixQ) << 1,
		                                            multiply_32x32_rshift32(highR, hpMixQ) << 1),
		                               multiply_32x32_rshift32(params.noise.svfBandR, bpMixQ) << 1);

		float modL = static_cast<float>(filteredL) / static_cast<float>(ONE_Q31) * maxMod;
		float modR = static_cast<float>(filteredR) / static_cast<float>(ONE_Q31) * maxMod;

		float delayL = std::clamp(baseDelay + modL, 1.0f, static_cast<float>(kEroderMaxDelay - 1));
		float delayR = std::clamp(baseDelay + modR, 1.0f, static_cast<float>(kEroderMaxDelay - 1));

		q31_t wetL = params.delay.readL(delayL);
		q31_t wetR = params.delay.readR(delayR);

		sample.l =
		    add_saturate(multiply_32x32_rshift32(sample.l, mixDry) << 1, multiply_32x32_rshift32(wetL, mixWet) << 1);
		sample.r =
		    add_saturate(multiply_32x32_rshift32(sample.r, mixDry) << 1, multiply_32x32_rshift32(wetR, mixWet) << 1);
	}
}

} // namespace deluge::dsp
