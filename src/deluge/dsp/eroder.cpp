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

// Tone bank: delay time, feedback, soft clip, fold from phi triangles
// High duty cycles (0.85/0.8) minimize dead zones where output is 0
// Clip/fold use moderate duty so they naturally sweep in and out
constexpr std::array<phi::PhiTriConfig, 6> kEroderToneBank = {{
    {phi::kPhi150, 0.85f, 0.0f, false},  // [0] Delay time: maps to [kMinDelay, kEroderMaxDelay-1]
    {phi::kPhi125, 0.8f, 0.4f, false},   // [1] Feedback (0→0.95, capped for stability)
    {phi::kPhi175, 0.7f, 0.6f, false},   // [2] Soft clip intensity (0=off, 1=max drive)
    {phi::kPhi100, 0.65f, 0.15f, false}, // [3] Fold intensity (0=off, 1=max fold)
    {phi::kPhi200, 0.75f, 0.35f, false}, // [4] HPF cutoff (0→1, maps to 100-800Hz)
    {phi::kPhi125, 0.5f, 0.45f, false},  // [5] Pitched mod depth (50% duty)
}};

/// Q31 piecewise-linear soft clip with drive in Q4.28 format
/// Below knee: linear pass-through. Above knee: 4:1 compression via right-shift.
/// Cost: 1 multiply + compare + conditional shift (no extra multiplies)
[[gnu::always_inline]] inline q31_t q31SoftClip(q31_t x, int32_t driveQ28) {
	int32_t driven = multiply_32x32_rshift32(x, driveQ28); // Q4.27
	constexpr int32_t kKnee = (INT32_C(1) << 27) * 2 / 3;  // 0.67 in Q4.27
	constexpr int32_t kUnit = INT32_C(1) << 27;
	if (driven > kKnee) {
		driven = kKnee + ((driven - kKnee) >> 2);
	}
	else if (driven < -kKnee) {
		driven = -kKnee + ((driven + kKnee) >> 2);
	}
	driven = std::clamp(driven, -kUnit, kUnit - 1);
	return static_cast<q31_t>(driven) << 4;
}

/// Q31 triangle fold with drive in Q4.28 format
/// Uses bitmask modulo for period-4 fold (no float, no division)
[[gnu::always_inline]] inline q31_t q31TriFold(q31_t x, int32_t driveQ28) {
	int32_t driven = multiply_32x32_rshift32(x, driveQ28); // Q4.27
	constexpr int32_t kUnit = INT32_C(1) << 27;
	int32_t phase = (driven + kUnit) & (INT32_C(4) * kUnit - 1);
	int32_t result;
	if (phase < 2 * kUnit) {
		result = phase - kUnit;
	}
	else {
		result = 3 * kUnit - phase;
	}
	return static_cast<q31_t>(std::min(result, kUnit - 1)) << 4;
}

// Character bank: depth, resonance, S&H blend, stereo width, pitched mod offset
// High duty cycles reduce dead zones; depth gets epsilon floor in DSP
constexpr std::array<phi::PhiTriConfig, 5> kEroderCharBank = {{
    {phi::kPhi150, 1.0f, 0.25f, false}, // [0] Depth (0→1, duty=1.0: no dead zone)
    {phi::kPhi175, 0.8f, 0.3f, false},  // [1] SVF resonance (0→1, high = tonal)
    {phi::kPhi100, 0.75f, 0.7f, false}, // [2] S&H blend (0=white, 1=S&H)
    {phi::kPhi075, 0.8f, 0.2f, false},  // [3] Width (0=mono, 1=full stereo)
    {phi::kPhi075, 0.6f, 0.1f, true},   // [4] Pitched mod offset (bipolar: ±4 octaves, subharmonic range)
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

	// Compute phi triangle input phases
	double freqNorm = static_cast<double>(params.smoothedFreq) / static_cast<double>(ONE_Q31);
	double freqPhase = freqNorm + params.effectiveFreq();
	double charNorm = static_cast<double>(params.smoothedCharacter) / static_cast<double>(ONE_Q31);
	double charPhase = charNorm + params.effectiveChar();

	// Throttled phi triangle evaluation: update every N buffers, skip if phase unchanged
	{
		bool phiTick = (params.phiUpdateCounter % EroderParams::kPhiUpdateInterval) == 0;
		params.phiUpdateCounter++;
		if (phiTick || !params.phiCacheValid) {
			if (freqPhase != params.prevTonePhase || !params.phiCacheValid) {
				params.cachedToneResults = phi::evalTriangleBank<6>(freqPhase, 1.0f, kEroderToneBank);
				params.prevTonePhase = freqPhase;
			}
			if (charPhase != params.prevCharPhase || !params.phiCacheValid) {
				params.cachedCharResults = phi::evalTriangleBank<5>(charPhase, 1.0f, kEroderCharBank);
				params.prevCharPhase = charPhase;
			}
			params.phiCacheValid = true;
		}
	}

	// === Pitch tracking (cached - only recompute when noteCode changes) ===
	// Must run before baseDelay/HPF/triangle which all use cachedPitchRatioQ16
	if (noteCode != params.prevNoteCode) {
		params.prevNoteCode = noteCode;
		if (noteCode >= 0 && noteCode < 128) {
			float pitchOctaves = (static_cast<float>(noteCode) - 60.0f) / 12.0f;
			float pitchRatio = fastPow2(pitchOctaves); // unclamped — SVF/HPF/triangle need full range
			params.cachedPitchRatioQ16 = static_cast<int32_t>(pitchRatio * 65536.0f);
		}
		else {
			params.cachedPitchRatioQ16 = 1 << 16; // 1.0 — no pitch info
		}
	}

	// Tone bank results → delay time, feedback, clip, fold
	const auto& freqResults = params.cachedToneResults;
	constexpr float kMinDelay = static_cast<float>(kEroderMaxDelay) * 0.25f; // 25% of buffer
	float baseDelay = kMinDelay + std::max(0.05f, freqResults[0]) * static_cast<float>(kEroderMaxDelay - 1 - kMinDelay);
	// Pitch tracking: shorter delay for higher notes (comb fundamental tracks pitch)
	baseDelay /= (static_cast<float>(params.cachedPitchRatioQ16) / 65536.0f);
	baseDelay = std::clamp(baseDelay, kMinDelay, static_cast<float>(kEroderMaxDelay - 1));
	q31_t feedbackQ = static_cast<q31_t>(std::max(0.05f, freqResults[1]) * 0.95f * static_cast<float>(ONE_Q31));
	float clipIntensity = freqResults[2]; // 0=off, 1=max soft clip drive
	float foldIntensity = freqResults[3]; // 0=off, 1=max fold

	// Character bank results → depth, resonance, S&H blend, width
	const auto& charResults = params.cachedCharResults;

	// SVF cutoff: knob offset (±2 octaves) + pitch tracking, multiplicative
	constexpr float kCutoffReference = 0.15f;
	constexpr float kCutoffMin = 0.01f;
	constexpr float kCutoffMax = 0.85f;

	float knobOctaves = static_cast<float>(cutoffValue) / static_cast<float>(0x40000000);
	float knobRatio = std::clamp(fastPow2(knobOctaves), 0.125f, 8.0f);

	float cutoffMapped = kCutoffReference * (static_cast<float>(params.cachedPitchRatioQ16) / 65536.0f) * knobRatio;
	cutoffMapped = std::clamp(cutoffMapped, kCutoffMin, kCutoffMax);
	q31_t svfF = static_cast<q31_t>(cutoffMapped * static_cast<float>(ONE_Q31));

	// Depth from character bank phi triangle [0] — baked into noise blend weights
	float depthFloat = std::max(0.15f, charResults[0]);
	q31_t depthQ = static_cast<q31_t>(depthFloat * static_cast<float>(ONE_Q31));

	// SVF resonance: Q = 1 - resonance
	float resonance = charResults[1] * 0.99f;
	q31_t svfQ = static_cast<q31_t>((1.0f - resonance) * static_cast<float>(ONE_Q31));

	// S&H blend weights with depth baked in
	q31_t shBlendQ = static_cast<q31_t>(charResults[2] * static_cast<float>(ONE_Q31));
	q31_t whiteBlendQ = ONE_Q31 - shBlendQ;
	q31_t whiteDepthQ = multiply_32x32_rshift32(whiteBlendQ, depthQ) << 1;
	q31_t shDepthQ = multiply_32x32_rshift32(shBlendQ, depthQ) << 1;

	// Stereo width with combined depth-baked weights for R channel (saves 1 mult per sample)
	q31_t widthQ = static_cast<q31_t>(charResults[3] * static_cast<float>(ONE_Q31));
	q31_t monoQ = ONE_Q31 - widthQ;
	q31_t monoWhiteDepthQ = multiply_32x32_rshift32(monoQ, whiteDepthQ) << 1;
	q31_t widthWhiteDepthQ = multiply_32x32_rshift32(widthQ, whiteDepthQ) << 1;

	FX_BENCH_DECLARE(bench, "eroder");
	FX_BENCH_SCOPE(bench);

	// Fixed-point delay: Q16.16 avoids all float ops in the inner-loop delay path
	// modScaleQ converts Q31 noise → Q16.16 delay offset (baseDelay * 0.5 max excursion)
	constexpr int32_t kFracBits = 16;
	constexpr int32_t kFracMask = (1 << kFracBits) - 1;
	int32_t baseDelayQ16 = static_cast<int32_t>(baseDelay * static_cast<float>(1 << kFracBits));
	int32_t minDelayQ16 = 1 << kFracBits;
	int32_t maxDelayQ16 = (kEroderMaxDelay - 1) << kFracBits;
	int32_t modScaleQ = static_cast<int32_t>(baseDelay * 32768.0f);

	// Pitched triangle modulator: step-based oscillator (no multiply in loop)
	// Amplitude baked into step size; direction persists between buffers
	float pitchedModDepth = freqResults[5];
	bool usePitchedMod = pitchedModDepth > 0.0f;
	q31_t triPeak = 0;
	if (usePitchedMod) {
		float offsetOctaves = charResults[4] * 4.0f; // bipolar ±4 octaves (subharmonic to +4 oct)
		float pitchRatioF = static_cast<float>(params.cachedPitchRatioQ16) / 65536.0f;
		float triFreq = 261.626f * pitchRatioF * knobRatio * fastPow2(offsetOctaves);
		// Step = 4 * amplitude * freq / sampleRate (4 quarter-cycles per period)
		float stepF = 4.0f * pitchedModDepth * triFreq / 44100.0f;
		q31_t stepMag = static_cast<q31_t>(stepF * static_cast<float>(ONE_Q31));
		// Preserve direction from previous buffer
		params.triStep = (params.triStep >= 0) ? stepMag : -stepMag;
		triPeak = static_cast<q31_t>(pitchedModDepth * static_cast<float>(ONE_Q31));
		// Clamp value in case peak shrank since last buffer
		params.triValue = std::clamp(params.triValue, -triPeak, triPeak);
	}

	// Waveshaping drive amounts (precomputed outside loop, Q4.28 fixed-point)
	bool doShape = clipIntensity > 0.0f || foldIntensity > 0.0f;
	float clipDrive = 1.0f + clipIntensity * 4.0f; // 1x → 5x
	float foldDrive = 1.0f + foldIntensity * 3.0f; // 1x → 4x
	int32_t clipDriveQ28 = static_cast<int32_t>(clipDrive * static_cast<float>(1 << 28));
	int32_t foldDriveQ28 = static_cast<int32_t>(foldDrive * static_cast<float>(1 << 28));

	// HPF: 1-pole high-pass on wet path
	// Base 100Hz, phi [4] sweeps 0-3 octaves, plus pitch tracking + cutoff knob
	float hpfCutoff = 100.0f * fastPow2(3.0f * freqResults[4])
	                  * (static_cast<float>(params.cachedPitchRatioQ16) / 65536.0f) * knobRatio;
	float hpfAlphaF = std::min(6.2831853f * hpfCutoff / 44100.0f, 0.5f);
	q31_t hpfAlpha = static_cast<q31_t>(hpfAlphaF * static_cast<float>(ONE_Q31));

	// Wet level with gain compensation (zero inner-loop cost):
	//   Additive mix: output = dry + level * wet (dry always passes through)
	//   Compensation: (1-feedback)/drive tames comb resonance + waveshaper amplification
	float wetLevel = static_cast<float>(params.mix) / 127.0f;
	float feedbackFloat = std::max(0.05f, freqResults[1]) * 0.95f;
	float wetCompensation = 1.0f - feedbackFloat;
	if (clipIntensity > 0.0f) {
		wetCompensation /= clipDrive;
	}
	if (foldIntensity > 0.0f) {
		wetCompensation /= foldDrive;
	}
	q31_t wetQ = static_cast<q31_t>(wetLevel * wetCompensation * static_cast<float>(ONE_Q31));

	for (auto& sample : buffer) {
		// Write input + feedback to delay (IIR comb filter topology)
		q31_t fbScaledL = multiply_32x32_rshift32(params.feedbackL, feedbackQ) << 1;
		q31_t fbScaledR = multiply_32x32_rshift32(params.feedbackR, feedbackQ) << 1;
		q31_t inputL = add_saturate(sample.l, fbScaledL);
		q31_t inputR = add_saturate(sample.r, fbScaledR);
		params.delay.write(inputL, inputR);

		// Generate white noise
		q31_t whiteL = getNoise();
		q31_t whiteR_ind = getNoise();

		// S&H: trigger on input or SVF zero crossing
		bool inputCross = (sample.l ^ params.noise.prevInputL) < 0;
		bool filterCross = (params.noise.svfLowL ^ params.noise.prevFilteredL) < 0;
		if (inputCross || filterCross) {
			params.noise.heldL = getNoise() >> 1;
		}
		params.noise.prevInputL = sample.l;
		params.noise.prevFilteredL = params.noise.svfLowL;

		// Noise with depth baked into blend weights
		// S&H term shared between L and R (same mono held value)
		q31_t shTerm = multiply_32x32_rshift32(params.noise.heldL, shDepthQ) << 1;
		// L: 2 mults (depth-scaled white + S&H blend)
		q31_t noiseL = add_saturate(multiply_32x32_rshift32(whiteL, whiteDepthQ) << 1, shTerm);
		// R: 2 mults + shared shTerm (was 3 mults)
		q31_t noiseR = add_saturate(add_saturate(multiply_32x32_rshift32(whiteL, monoWhiteDepthQ) << 1,
		                                         multiply_32x32_rshift32(whiteR_ind, widthWhiteDepthQ) << 1),
		                            shTerm);

		// SVF filter (2-pole state variable filter)
		q31_t highL = noiseL - params.noise.svfLowL - (multiply_32x32_rshift32(params.noise.svfBandL, svfQ) << 1);
		params.noise.svfBandL += multiply_32x32_rshift32(highL, svfF) << 1;
		params.noise.svfLowL += multiply_32x32_rshift32(params.noise.svfBandL, svfF) << 1;

		q31_t highR = noiseR - params.noise.svfLowR - (multiply_32x32_rshift32(params.noise.svfBandR, svfQ) << 1);
		params.noise.svfBandR += multiply_32x32_rshift32(highR, svfF) << 1;
		params.noise.svfLowR += multiply_32x32_rshift32(params.noise.svfBandR, svfF) << 1;

		// Bandpass + pitched triangle combined in integer
		q31_t combinedL = params.noise.svfBandL;
		q31_t combinedR = params.noise.svfBandR;
		if (usePitchedMod) {
			params.triValue += params.triStep;
			if (params.triValue > triPeak) {
				params.triValue = triPeak - (params.triValue - triPeak);
				params.triStep = -params.triStep;
			}
			else if (params.triValue < -triPeak) {
				params.triValue = -triPeak - (params.triValue + triPeak);
				params.triStep = -params.triStep;
			}
			combinedL = add_saturate(combinedL, params.triValue);
			combinedR = add_saturate(combinedR, params.triValue);
		}
		// Delay modulation in Q16.16 fixed-point (no float ops)
		int32_t modL = multiply_32x32_rshift32(combinedL, modScaleQ) << 1;
		int32_t modR = multiply_32x32_rshift32(combinedR, modScaleQ) << 1;
		int32_t delayLQ16 = std::clamp(baseDelayQ16 + modL, minDelayQ16, maxDelayQ16);
		int32_t delayRQ16 = std::clamp(baseDelayQ16 + modR, minDelayQ16, maxDelayQ16);

		q31_t wetL = params.delay.readLQ16(delayLQ16);
		q31_t wetR = params.delay.readRQ16(delayRQ16);

		// Feedback tap BEFORE waveshaping
		params.feedbackL = wetL;
		params.feedbackR = wetR;

		// Waveshaping: soft clip → fold (all Q31, no float conversion)
		if (doShape) {
			if (clipIntensity > 0.0f) {
				wetL = q31SoftClip(wetL, clipDriveQ28);
				wetR = q31SoftClip(wetR, clipDriveQ28);
			}
			if (foldIntensity > 0.0f) {
				wetL = q31TriFold(wetL, foldDriveQ28);
				wetR = q31TriFold(wetR, foldDriveQ28);
			}
		}

		// 1-pole HPF on wet path (DC blocking + low-frequency cleanup)
		params.wetHpfL += multiply_32x32_rshift32(wetL - params.wetHpfL, hpfAlpha) << 1;
		wetL -= params.wetHpfL;
		params.wetHpfR += multiply_32x32_rshift32(wetR - params.wetHpfR, hpfAlpha) << 1;
		wetR -= params.wetHpfR;

		// Additive wet level (dry always passes through at unity)
		sample.l = add_saturate(sample.l, multiply_32x32_rshift32(wetL, wetQ) << 1);
		sample.r = add_saturate(sample.r, multiply_32x32_rshift32(wetR, wetQ) << 1);
	}
}

} // namespace deluge::dsp
