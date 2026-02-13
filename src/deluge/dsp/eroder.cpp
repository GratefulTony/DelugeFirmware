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
#include "dsp/fast_math.h"
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

// Tone bank: delay time + feedback from phi triangles
// High duty cycles (0.85/0.8) minimize dead zones where output is 0
constexpr std::array<phi::PhiTriConfig, 2> kEroderToneBank = {{
    {phi::kPhi150, 0.85f, 0.0f, false}, // [0] Delay time: maps to [1, kEroderMaxDelay-1]
    {phi::kPhi125, 0.8f, 0.4f, false},  // [1] Feedback (0→0.95, capped for stability)
}};

// Character bank: depth, resonance, S&H blend, filter output select, stereo width
// High duty cycles reduce dead zones; depth gets epsilon floor in DSP
constexpr std::array<phi::PhiTriConfig, 6> kEroderCharBank = {{
    {phi::kPhi150, 0.85f, 0.25f, false}, // [0] Depth (0→1, noise mod intensity)
    {phi::kPhi175, 0.8f, 0.3f, false},   // [1] SVF resonance (0→1, high = tonal)
    {phi::kPhi100, 0.75f, 0.7f, false},  // [2] S&H blend (0=white, 1=S&H)
    {phi::kPhi125, 0.8f, 0.1f, true},    // [3] LP↔HP select (bipolar: -1=LP, +1=HP)
    {phi::kPhi200, 0.75f, 0.5f, true},   // [4] (LP/HP)↔BP select (bipolar: -1=LP/HP, +1=BP)
    {phi::kPhi075, 0.8f, 0.2f, false},   // [5] Width (0=mono, 1=full stereo)
}};

} // anonymous namespace

// ============================================================================
// Buffer Processing
// ============================================================================

void processEroder(std::span<StereoSample> buffer, EroderParams& params, q31_t freqPreset, q31_t freqCables,
                   q31_t charPreset, q31_t charCables, q31_t cutoffValue, int32_t noteCode) {
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

	// Tone phi phase → delay time + feedback
	double freqNorm = static_cast<double>(params.smoothedFreq) / static_cast<double>(ONE_Q31);
	double freqPhase = freqNorm + params.effectiveFreq();
	auto freqResults = phi::evalTriangleBank<2>(freqPhase, 1.0f, kEroderToneBank);
	float baseDelay = 1.0f + std::max(0.05f, freqResults[0]) * static_cast<float>(kEroderMaxDelay - 2);
	q31_t feedbackQ = static_cast<q31_t>(std::max(0.05f, freqResults[1]) * 0.95f * static_cast<float>(ONE_Q31));

	// Character phi phase → depth, resonance, S&H blend, output select, width
	double charNorm = static_cast<double>(params.smoothedCharacter) / static_cast<double>(ONE_Q31);
	double charPhase = charNorm + params.effectiveChar();
	auto charResults = phi::evalTriangleBank<6>(charPhase, 1.0f, kEroderCharBank);

	// === Pitch tracking (cached - only recompute when noteCode changes) ===
	if (noteCode != params.prevNoteCode) {
		params.prevNoteCode = noteCode;
		if (noteCode >= 0 && noteCode < 128) {
			float pitchOctaves = (static_cast<float>(noteCode) - 60.0f) / 12.0f;
			float pitchRatio = std::clamp(fastPow2(pitchOctaves), 0.25f, 4.0f);
			params.cachedPitchRatioQ16 = static_cast<int32_t>(pitchRatio * 65536.0f);
		}
		else {
			params.cachedPitchRatioQ16 = 1 << 16; // 1.0 — no pitch info
		}
	}

	// SVF cutoff: knob offset (±2 octaves) + pitch tracking, multiplicative
	// Reference cutoff at middle C with knob centered = 0.15 (mid SVF range)
	constexpr float kCutoffReference = 0.15f;
	constexpr float kCutoffMin = 0.01f;
	constexpr float kCutoffMax = 0.85f;

	// Knob → octave offset: full bipolar range (±0x80000000) = ±2 octaves
	float knobOctaves = static_cast<float>(cutoffValue) / static_cast<float>(0x40000000);
	float knobRatio = std::clamp(fastPow2(knobOctaves), 0.125f, 8.0f);

	// Combine: reference * pitch tracking * knob offset
	float cutoffMapped = kCutoffReference * (static_cast<float>(params.cachedPitchRatioQ16) / 65536.0f) * knobRatio;
	cutoffMapped = std::clamp(cutoffMapped, kCutoffMin, kCutoffMax);
	q31_t svfF = static_cast<q31_t>(cutoffMapped * static_cast<float>(ONE_Q31));

	// Depth from character bank phi triangle [0] (0→1)
	// Epsilon floor ensures effect never fully dies when mix > 0
	float depthFloat = std::max(0.05f, charResults[0]);

	// SVF resonance: Q = 1 - resonance (low Q = resonant, near self-oscillation for tonal noise)
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

	// Stereo width: 0=mono (L=R), 1=full stereo (independent L/R noise)
	q31_t widthQ = static_cast<q31_t>(charResults[5] * static_cast<float>(ONE_Q31));
	q31_t monoQ = ONE_Q31 - widthQ;

	FX_BENCH_DECLARE(bench, "eroder");
	FX_BENCH_SCOPE(bench);

	// Depth scaling: noise modulation intensity from phi triangle
	float maxMod = baseDelay * 0.5f * depthFloat;

	// Mix: wet/dry blend from knob (0=bypass, 127=100% wet)
	float mixFloat = static_cast<float>(params.mix) / 127.0f;
	q31_t mixQ = static_cast<q31_t>(mixFloat * static_cast<float>(ONE_Q31));
	q31_t dryQ = ONE_Q31 - mixQ;

	for (auto& sample : buffer) {
		// Write input + feedback to delay (IIR comb filter topology)
		// Feedback is scaled by feedbackQ (max 0.95) to prevent DC buildup
		q31_t fbScaledL = multiply_32x32_rshift32(params.feedbackL, feedbackQ) << 1;
		q31_t fbScaledR = multiply_32x32_rshift32(params.feedbackR, feedbackQ) << 1;
		q31_t inputL = add_saturate(sample.l, fbScaledL);
		q31_t inputR = add_saturate(sample.r, fbScaledR);
		params.delay.write(inputL, inputR);

		// Generate white noise with stereo width control
		q31_t whiteL = getNoise();
		q31_t whiteR_ind = getNoise();
		q31_t whiteR =
		    add_saturate(multiply_32x32_rshift32(whiteL, monoQ) << 1, multiply_32x32_rshift32(whiteR_ind, widthQ) << 1);

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

		// SVF filter (2-pole)
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

		params.feedbackL = wetL;
		params.feedbackR = wetR;

		// Wet/dry blend
		sample.l = add_saturate(multiply_32x32_rshift32(sample.l, dryQ) << 1, multiply_32x32_rshift32(wetL, mixQ) << 1);
		sample.r = add_saturate(multiply_32x32_rshift32(sample.r, dryQ) << 1, multiply_32x32_rshift32(wetR, mixQ) << 1);
	}
}

} // namespace deluge::dsp
