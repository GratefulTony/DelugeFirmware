/*
 * Copyright © 2026 Owlet Records
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

#include "dsp/phi_vox.hpp"
#include "dsp/oscillators/sine_osc.h"
#include "io/debug/fx_benchmark.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp {

// ============================================================================
// Zone -> VOSIM parameter builder (runs only on zone/gamma change)
// ============================================================================

namespace {

// Vowel formant anchors per zone (F1, F2 in Hz). Gravity wells only - the phi
// banks wander up to +/-0.6..0.8 octaves around them.
//                            Breath      Hum        Round      Open       Bright     Nasal      Growl      Rasp
constexpr float kAnchorF1[8] = {300.0f, 350.0f, 450.0f, 750.0f, 550.0f, 900.0f, 250.0f, 700.0f};
constexpr float kAnchorF2[8] = {2200.0f, 800.0f, 900.0f, 1150.0f, 1900.0f, 1600.0f, 600.0f, 3400.0f};

constexpr float kSampleRate = 44100.0f;
constexpr float kPhaseUnitsPerHz = 4294967296.0f / kSampleRate;

uint32_t hzToPhaseInc(float hz) {
	hz = std::clamp(hz, 60.0f, 8000.0f);
	return static_cast<uint32_t>(hz * kPhaseUnitsPerHz);
}

} // namespace

PhiVoxParams buildPhiVoxParams(uint16_t zone, float phaseOffset) {
	PhiVoxParams p{};

	double phase = static_cast<double>(zone) / 1023.0 + static_cast<double>(phaseOffset);

	// Anchor interpolation: continuous position across the 8 vowel anchors
	float zonePos = static_cast<float>(zone) / 1023.0f * 7.0f;
	int32_t zi = std::min(static_cast<int32_t>(zonePos), static_cast<int32_t>(6));
	float zf = zonePos - static_cast<float>(zi);
	float f1Anchor = kAnchorF1[zi] + (kAnchorF1[zi + 1] - kAnchorF1[zi]) * zf;
	float f2Anchor = kAnchorF2[zi] + (kAnchorF2[zi + 1] - kAnchorF2[zi]) * zf;

	// Phi wander in octaves around the anchors
	float f1 = f1Anchor * std::exp2(phi::evalTriangle(phase, 1.0f, kPhiVoxF1Wander) * 0.6f);
	float f2 = f2Anchor * std::exp2(phi::evalTriangle(phase, 1.0f, kPhiVoxF2Wander) * 0.8f);
	p.formant[0].phaseIncrement = hzToPhaseInc(f1);
	p.formant[1].phaseIncrement = hzToPhaseInc(f2);

	int32_t n1 = 1 + static_cast<int32_t>(phi::evalTriangle(phase, 1.0f, kPhiVoxN1) * 11.0f);
	int32_t n2 = 1 + static_cast<int32_t>(phi::evalTriangle(phase, 1.0f, kPhiVoxN2) * 11.0f);
	float b1 = 0.30f + phi::evalTriangle(phase, 1.0f, kPhiVoxDecay1) * 0.88f;
	float b2 = 0.30f + phi::evalTriangle(phase, 1.0f, kPhiVoxDecay2) * 0.88f;
	float pol1 = phi::evalTriangle(phase, 1.0f, kPhiVoxPolarity1);
	float pol2 = phi::evalTriangle(phase, 1.0f, kPhiVoxPolarity2);

	// F1/F2 balance: 0 -> F1 only, 1 -> equal
	float balance = 0.15f + phi::evalTriangle(phase, 1.0f, kPhiVoxBalance) * 0.85f;
	float gains[kPhiVoxNumFormants] = {0.62f, 0.62f * balance};
	int32_t counts[kPhiVoxNumFormants] = {n1, n2};
	float decays[kPhiVoxNumFormants] = {b1, b2};
	float pols[kPhiVoxNumFormants] = {pol1, pol2};

	for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
		int32_t n = std::min(counts[f], kPhiVoxMaxPulses);
		float mag = 1.0f;
		float peak = 0.0f;
		for (int32_t k = 0; k < n; k++) {
			float sign = 1.0f - 2.0f * static_cast<float>(k & 1) * pols[f];
			p.formant[f].pulseGain[k] = mag * sign;
			peak = std::max(peak, std::abs(mag));
			mag *= decays[f];
		}
		// Peak-normalize the burst (decay > 1 makes the LAST pulse loudest)
		float norm = (peak > 0.0001f) ? gains[f] / peak : 0.0f;
		for (int32_t k = 0; k < n; k++) {
			p.formant[f].pulseGain[k] *= norm;
		}
		// Remaining entries stay zero: the silent gap until the cycle wraps
	}

	p.breath = phi::evalTriangle(phase, 1.0f, kPhiVoxBreath);
	p.breath = p.breath * p.breath * 0.30f;
	p.articulation = 0.5f + phi::evalTriangle(phase, 1.0f, kPhiVoxArticulation) * 3.5f;

	return p;
}

// ============================================================================
// Main render
// ============================================================================

void renderPhiVox(PhiVoxCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                  uint32_t phaseIncrement, uint32_t* startPhase, uint32_t retriggerPhase, int32_t amplitude,
                  int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade, uint32_t pulseWidth) {

#if ENABLE_FX_BENCHMARK
	FX_BENCH_DECLARE(bench_render, "phi_vox", "render");
	FX_BENCH_START(bench_render);
#endif

	// Rebuild the crossfaded effective tables when the (smoothed) crossfade moves.
	// Cheap: 2 formant increments + 32 gain lerps.
	if (crossfade != cache.prevCrossfade) {
		float cf = std::clamp(static_cast<float>(crossfade) / 2147483648.0f + 0.5f, 0.0f, 1.0f);
		float cfInv = 1.0f - cf;
		for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
			float incA = static_cast<float>(cache.bankA.formant[f].phaseIncrement);
			float incB = static_cast<float>(cache.bankB.formant[f].phaseIncrement);
			cache.effFormantInc[f] = static_cast<uint32_t>(cfInv * incA + cf * incB);
			float gainSum = 0.0f;
			for (int32_t k = 0; k < kPhiVoxMaxPulses; k++) {
				float g = cfInv * cache.bankA.formant[f].pulseGain[k] + cf * cache.bankB.formant[f].pulseGain[k];
				cache.effPulseGain[f][k] = static_cast<q31_t>(g * 2147483647.0f);
				gainSum += g;
			}
			// DC compensation: mean of the burst = 0.5 * sum(gains) * (Tformant/Tnote).
			// The pitch-dependent factor is applied per buffer below.
			cache.effMeanComp[f] = 0.5f * gainSum;
		}
		cache.prevCrossfade = crossfade;
	}

	// Articulation envelope: crossfade VELOCITY injects a consonant burst.
	// Advanced once per audio buffer regardless of voice/unison count.
	if (AudioEngine::audioSampleTimer != cache.lastEnvTime) {
		cache.lastEnvTime = AudioEngine::audioSampleTimer;
		float cf = std::clamp(static_cast<float>(crossfade) / 2147483648.0f + 0.5f, 0.0f, 1.0f);
		float cfInv = 1.0f - cf;
		if (cache.prevCf >= 0.0f) {
			float art = cfInv * cache.bankA.articulation + cf * cache.bankB.articulation;
			cache.artEnv = std::max(std::abs(cf - cache.prevCf) * art * 8.0f, cache.artEnv * 0.80f);
		}
		cache.prevCf = cf;
		float breath = cfInv * cache.bankA.breath + cf * cache.bankB.breath;
		float noiseAmt = std::min(breath + cache.artEnv, 0.9f);
		cache.effVoicedNoise = static_cast<q31_t>(noiseAmt * 2147483647.0f);
	}

	// DC compensation for this buffer's pitch: mean = comp * (noteInc / formantInc)
	q31_t dcComp = 0;
	for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
		float ratio = static_cast<float>(phaseIncrement) / static_cast<float>(cache.effFormantInc[f]);
		dcComp += static_cast<q31_t>(cache.effMeanComp[f] * std::min(ratio, 1.0f) * 2147483647.0f);
	}

	uint32_t phase = *startPhase;
	uint32_t phaseAtEnd = phase + phaseIncrement * static_cast<uint32_t>(numSamples);

	// Match the triangle-oscillator amplitude convention (as PHI_MORPH does)
	amplitude <<= 1;
	amplitudeIncrement <<= 1;

	const uint32_t phaseWidth = pulseWidth ? (0xFFFFFFFF - (pulseWidth << 1)) : 0xFFFFFFFF;

	// Derive formant state from the cycle phase at buffer start (stateless per
	// voice): samples into the cycle, then formant cycles elapsed (= pulse
	// index, high word) and formant phase (low word) in one 64-bit multiply.
	uint32_t evalPhase = phase + phaseIncrement + retriggerPhase;
	uint32_t samplesIntoCycle = evalPhase / phaseIncrement; // One divide per buffer
	uint32_t fPhase[kPhiVoxNumFormants];
	int32_t pulseIdx[kPhiVoxNumFormants];
	for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
		uint64_t elapsed = static_cast<uint64_t>(samplesIntoCycle) * cache.effFormantInc[f];
		pulseIdx[f] = std::min(static_cast<int32_t>(elapsed >> 32), kPhiVoxMaxPulses - 1);
		fPhase[f] = static_cast<uint32_t>(elapsed);
	}

	uint32_t prevEvalPhase = evalPhase;
	uint32_t noiseState = cache.noiseState;
	const q31_t voicedNoise = cache.effVoicedNoise;
	int32_t* thisSample = bufferStart;

	for (int32_t n = 0; n < numSamples; n++) {
		phase += phaseIncrement;
		evalPhase = phase + retriggerPhase;
		if (applyAmplitude) {
			amplitude += amplitudeIncrement;
		}

		// Cycle wrap (including retrigger jumps): restart the pulse burst
		if (evalPhase < prevEvalPhase) {
			fPhase[0] = 0;
			fPhase[1] = 0;
			pulseIdx[0] = 0;
			pulseIdx[1] = 0;
		}
		prevEvalPhase = evalPhase;

		// Pulse-width deadzone (parity with PHI_MORPH): silences the cycle tail
		if (evalPhase > phaseWidth) {
			if (applyAmplitude) {
				thisSample++;
			}
			else {
				*thisSample++ = 0;
			}
			continue;
		}

		q31_t out = -dcComp;
		q31_t envelope = 0; // Strongest pulse envelope, gates the noise

		for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
			uint32_t newPhase = fPhase[f] + cache.effFormantInc[f];
			if (newPhase < fPhase[f] && pulseIdx[f] < kPhiVoxMaxPulses - 1) {
				pulseIdx[f]++; // Formant period completed: next pulse
			}
			fPhase[f] = newPhase;

			// Raised cosine: sin²(pi*t) over one formant period = (1 - cos)/2
			q31_t cosv = SineOsc::doFMNew(newPhase + 0x40000000u, 0);
			q31_t rc = 0x3FFFFFFF - (cosv >> 1); // [0, ~Q31]

			q31_t gain = cache.effPulseGain[f][pulseIdx[f]];
			out = add_saturate(out, multiply_32x32_rshift32(rc, gain) << 1);
			// Envelope follows the GAINED pulse so noise stays inside the burst
			// (silent gap stays silent - no hiss between glottal pulses)
			q31_t gainAbs = (gain < 0) ? -gain : gain;
			envelope = std::max(envelope, multiply_32x32_rshift32(rc, gainAbs) << 1);
		}

		// Voiced-gated noise: breath + articulation bursts live inside the
		// pulse envelope, so consonants articulate rather than hiss
		noiseState = noiseState * 1664525u + 1013904223u;
		q31_t noise = multiply_32x32_rshift32(static_cast<int32_t>(noiseState), voicedNoise);
		out = add_saturate(out, multiply_32x32_rshift32(noise, envelope) << 1);

		if (applyAmplitude) {
			*thisSample = multiply_accumulate_32x32_rshift32_rounded(*thisSample, out, amplitude);
			thisSample++;
		}
		else {
			*thisSample++ = out;
		}
	}

	cache.noiseState = noiseState;
	*startPhase = phaseAtEnd;

#if ENABLE_FX_BENCHMARK
	FX_BENCH_STOP(bench_render);
#endif
}

} // namespace deluge::dsp
