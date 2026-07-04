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

	// Phi wander in octaves around the anchors, plus a slow COHERENT shift
	// of both formants together (the "same vowel, moved" dimension)
	float shiftMul = std::exp2(phi::evalTriangle(phase, 1.0f, kPhiVoxShift) * 0.6f);
	float f1 = f1Anchor * std::exp2(phi::evalTriangle(phase, 1.0f, kPhiVoxF1Wander) * 0.6f) * shiftMul;
	float f2 = f2Anchor * std::exp2(phi::evalTriangle(phase, 1.0f, kPhiVoxF2Wander) * 0.8f) * shiftMul;
	p.formant[0].phaseIncrement = hzToPhaseInc(f1);
	p.formant[1].phaseIncrement = hzToPhaseInc(f2);
	// Note-relative ratios for formant tracking (reference note: C3 = 130.81 Hz,
	// so 0% and 100% tracking sound identical when playing C3)
	constexpr float kReferenceHz = 130.81f;
	p.formant[0].noteRatio = std::clamp(f1, 60.0f, 8000.0f) / kReferenceHz;
	p.formant[1].noteRatio = std::clamp(f2, 60.0f, 8000.0f) / kReferenceHz;

	// FRACTIONAL pulse counts: the last pulse fades in continuously, so sweeping
	// through phi space never pops a whole pulse in/out of the burst (that
	// integer step was an audible discontinuity at specific zone positions)
	float n1 = 1.0f + phi::evalTriangle(phase, 1.0f, kPhiVoxN1) * 11.0f;
	float n2 = 1.0f + phi::evalTriangle(phase, 1.0f, kPhiVoxN2) * 11.0f;
	float b1 = 0.30f + phi::evalTriangle(phase, 1.0f, kPhiVoxDecay1) * 0.88f;
	float b2 = 0.30f + phi::evalTriangle(phase, 1.0f, kPhiVoxDecay2) * 0.88f;
	float pol1 = phi::evalTriangle(phase, 1.0f, kPhiVoxPolarity1);
	float pol2 = phi::evalTriangle(phase, 1.0f, kPhiVoxPolarity2);

	// F1/F2 balance: 0 -> F1 only, 1 -> equal
	float balance = 0.15f + phi::evalTriangle(phase, 1.0f, kPhiVoxBalance) * 0.85f;
	float gains[kPhiVoxNumFormants] = {0.62f, 0.62f * balance};
	float counts[kPhiVoxNumFormants] = {n1, n2};
	float decays[kPhiVoxNumFormants] = {b1, b2};
	float pols[kPhiVoxNumFormants] = {pol1, pol2};

	for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
		float count = std::min(counts[f], static_cast<float>(kPhiVoxMaxPulses));
		int32_t nFull = static_cast<int32_t>(count);
		float partial = count - static_cast<float>(nFull);
		float mag = 1.0f;
		float peak = 0.0f;
		for (int32_t k = 0; k < nFull + (partial > 0.0f ? 1 : 0) && k < kPhiVoxMaxPulses; k++) {
			float sign = 1.0f - 2.0f * static_cast<float>(k & 1) * pols[f];
			float frac = (k < nFull) ? 1.0f : partial;
			p.formant[f].pulseGain[k] = mag * sign * frac;
			peak = std::max(peak, std::abs(mag) * frac);
			mag *= decays[f];
		}
		// Peak-normalize the burst (decay > 1 makes the LAST pulse loudest)
		float norm = (peak > 0.0001f) ? gains[f] / peak : 0.0f;
		for (int32_t k = 0; k < kPhiVoxMaxPulses; k++) {
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
                  uint32_t phaseIncrement, uint32_t* startPhase, uint64_t* formantState, uint32_t retriggerPhase,
                  int32_t amplitude, int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade,
                  uint32_t pulseWidth, int32_t trackingAmount) {

#if ENABLE_FX_BENCHMARK
	FX_BENCH_DECLARE(bench_render, "phi_vox", "render");
	FX_BENCH_START(bench_render);
#endif

	// Rebuild the crossfaded effective tables when the (smoothed) crossfade moves.
	// Cheap: 2 formant increments + 32 gain lerps.
	if (crossfade != cache.prevCrossfade) {
		// Outgoing eff values become this buffer's ramp start points
		memcpy(cache.effFormantIncFrom, cache.effFormantInc, sizeof(cache.effFormantIncFrom));
		memcpy(cache.effNoteRatioFrom, cache.effNoteRatio, sizeof(cache.effNoteRatioFrom));
		memcpy(cache.effMeanCompFrom, cache.effMeanComp, sizeof(cache.effMeanCompFrom));
		memcpy(cache.effPulseGainFrom, cache.effPulseGain, sizeof(cache.effPulseGainFrom));
		memcpy(cache.effPulseGainAbsFrom, cache.effPulseGainAbs, sizeof(cache.effPulseGainAbsFrom));
		float cf = std::clamp(static_cast<float>(crossfade) / 2147483648.0f + 0.5f, 0.0f, 1.0f);
		float cfInv = 1.0f - cf;
		for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
			float incA = static_cast<float>(cache.bankA.formant[f].phaseIncrement);
			float incB = static_cast<float>(cache.bankB.formant[f].phaseIncrement);
			cache.effFormantInc[f] = static_cast<uint32_t>(cfInv * incA + cf * incB);
			cache.effNoteRatio[f] = cfInv * cache.bankA.formant[f].noteRatio + cf * cache.bankB.formant[f].noteRatio;
			float gainSum = 0.0f;
			for (int32_t k = 0; k < kPhiVoxMaxPulses; k++) {
				float g = cfInv * cache.bankA.formant[f].pulseGain[k] + cf * cache.bankB.formant[f].pulseGain[k];
				cache.effPulseGain[f][k] = static_cast<q31_t>(g * 2147483647.0f);
				cache.effPulseGainAbs[f][k] = static_cast<q31_t>(std::abs(g) * 2147483647.0f);
				gainSum += g;
			}
			// DC compensation: mean of the burst = 0.5 * sum(gains) * (Tformant/Tnote).
			// The pitch-dependent factor is applied per buffer below.
			cache.effMeanComp[f] = 0.5f * gainSum;
		}
		cache.prevCrossfade = crossfade;
		cache.effVersion++;
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

		// Formant-frequency slew: glide toward the effective targets
		if (!cache.slewInit) {
			cache.slewInit = true;
			for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
				cache.incSlew[f] = static_cast<float>(cache.effFormantInc[f]);
				cache.ratioSlew[f] = cache.effNoteRatio[f];
			}
		}
		for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
			cache.incSlewFrom[f] = cache.incSlew[f];
			cache.ratioSlewFrom[f] = cache.ratioSlew[f];
			cache.incSlew[f] += 0.15f * (static_cast<float>(cache.effFormantInc[f]) - cache.incSlew[f]);
			cache.ratioSlew[f] += 0.15f * (cache.effNoteRatio[f] - cache.ratioSlew[f]);
		}

		// Morph-ramp snapshots (once per buffer, shared across voices)
		cache.morphRamping = (cache.effVersion != cache.effVersionSeen);
		if (cache.morphRamping) {
			cache.effVersionSeen = cache.effVersion;
		}
		else {
			// Parked: From == To, ramps degenerate to the static values
			memcpy(cache.effFormantIncFrom, cache.effFormantInc, sizeof(cache.effFormantIncFrom));
			memcpy(cache.effNoteRatioFrom, cache.effNoteRatio, sizeof(cache.effNoteRatioFrom));
			memcpy(cache.effMeanCompFrom, cache.effMeanComp, sizeof(cache.effMeanCompFrom));
			memcpy(cache.effPulseGainFrom, cache.effPulseGain, sizeof(cache.effPulseGainFrom));
			memcpy(cache.effPulseGainAbsFrom, cache.effPulseGainAbs, sizeof(cache.effPulseGainAbsFrom));
		}
	}

	// Formant tracking: blend each formant's increment from fixed Hz (vocal)
	// toward a note-relative ratio. From/To pairs ramp across the buffer -
	// per-buffer steps were audible under wave modulation
	float track = static_cast<float>(trackingAmount) * (1.0f / 50.0f);
	uint32_t finalInc[kPhiVoxNumFormants];
	int32_t incStep[kPhiVoxNumFormants];
	q31_t dcComp = 0;
	q31_t dcCompTo = 0;
	for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
		// Slewed frequencies: the ramp endpoints are the previous and current
		// slew values, so every buffer glides (portamento) instead of stepping
		float incAbsF = cache.incSlewFrom[f];
		float incFrom = incAbsF + (cache.ratioSlewFrom[f] * static_cast<float>(phaseIncrement) - incAbsF) * track;
		float incAbsT = cache.incSlew[f];
		float incTo = incAbsT + (cache.ratioSlew[f] * static_cast<float>(phaseIncrement) - incAbsT) * track;
		uint32_t from = static_cast<uint32_t>(std::clamp(incFrom, 60.0f * 97391.5f, 8000.0f * 97391.5f));
		uint32_t to = static_cast<uint32_t>(std::clamp(incTo, 60.0f * 97391.5f, 8000.0f * 97391.5f));
		finalInc[f] = from;
		incStep[f] = static_cast<int32_t>(to - from) / numSamples;
		float ratioF = static_cast<float>(phaseIncrement) / static_cast<float>(from);
		float ratioT = static_cast<float>(phaseIncrement) / static_cast<float>(to);
		dcComp += static_cast<q31_t>(cache.effMeanCompFrom[f] * std::min(ratioF, 1.0f) * 2147483647.0f);
		dcCompTo += static_cast<q31_t>(cache.effMeanComp[f] * std::min(ratioT, 1.0f) * 2147483647.0f);
	}
	const q31_t dcStep = static_cast<q31_t>((static_cast<int64_t>(dcCompTo) - dcComp) / numSamples);

	uint32_t phase = *startPhase;
	uint32_t phaseAtEnd = phase + phaseIncrement * static_cast<uint32_t>(numSamples);

	// Match the triangle-oscillator amplitude convention (as PHI_MORPH does)
	amplitude <<= 1;
	amplitudeIncrement <<= 1;

	const uint32_t phaseWidth = pulseWidth ? (0xFFFFFFFF - (pulseWidth << 1)) : 0xFFFFFFFF;

	// Formant state carries across buffers per voice (phase 28b + index 4b
	// per formant, packed in an otherwise-unused uint64). Re-deriving it per
	// buffer teleported the burst mid-cycle whenever the increments moved.
	// Derivation remains as the init path (state == 0: fresh voice).
	uint32_t evalPhase = phase + phaseIncrement + retriggerPhase;
	uint32_t fPhase[kPhiVoxNumFormants];
	int32_t pulseIdx[kPhiVoxNumFormants];
	if (*formantState == 0) {
		uint32_t samplesIntoCycle = evalPhase / phaseIncrement; // One divide, init only
		for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
			uint64_t elapsed = static_cast<uint64_t>(samplesIntoCycle) * finalInc[f];
			pulseIdx[f] = std::min(static_cast<int32_t>(elapsed >> 32), kPhiVoxMaxPulses - 1);
			fPhase[f] = static_cast<uint32_t>(elapsed);
		}
	}
	else {
		uint32_t lo = static_cast<uint32_t>(*formantState);
		uint32_t hi = static_cast<uint32_t>(*formantState >> 32);
		fPhase[0] = lo & 0xFFFFFFF0u;
		pulseIdx[0] = static_cast<int32_t>(lo & 0xFu);
		fPhase[1] = hi & 0xFFFFFFF0u;
		pulseIdx[1] = static_cast<int32_t>(hi & 0xFu);
	}

	// Seed the wrap detector with the PRE-buffer phase: seeding it with the
	// first sample's own value made cycle wraps that fall exactly between
	// buffers invisible - with carried state that meant missed burst resets
	// (burst-position jitter even without modulation)
	uint32_t prevEvalPhase = phase + retriggerPhase;
	uint32_t noiseState = cache.noiseState;
	const q31_t voicedNoise = cache.effVoicedNoise;
	const bool morphing = cache.morphRamping;
	q31_t morphRampQ = 0;
	const q31_t morphRampInc = 0x7FFFFFFF / numSamples;
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

		dcComp += dcStep;
		morphRampQ += morphRampInc;
		q31_t out = 0;
		q31_t envelope = 0; // Strongest pulse envelope, gates the noise

		for (int32_t f = 0; f < kPhiVoxNumFormants; f++) {
			finalInc[f] += static_cast<uint32_t>(incStep[f]);
			uint32_t newPhase = fPhase[f] + finalInc[f];
			if (newPhase < fPhase[f] && pulseIdx[f] < kPhiVoxMaxPulses - 1) {
				pulseIdx[f]++; // Formant period completed: next pulse
			}
			fPhase[f] = newPhase;

			// Raised cosine: sin²(pi*t) over one formant period = (1 - cos)/2
			q31_t cosv = SineOsc::doFMNew(newPhase + 0x40000000u, 0);
			q31_t rc = 0x3FFFFFFF - (cosv >> 1); // [0, ~Q31]

			q31_t gain = cache.effPulseGain[f][pulseIdx[f]];
			q31_t gainAbs = cache.effPulseGainAbs[f][pulseIdx[f]];
			if (morphing) { // Lerp gains from last buffer's tables while the wave moves.
				// Halved-difference lerp: the raw difference wraps int32 when a
				// pulse's SIGN flips at high gain across a zone change
				q31_t gF = cache.effPulseGainFrom[f][pulseIdx[f]];
				gain = gF + (multiply_32x32_rshift32((gain >> 1) - (gF >> 1), morphRampQ) << 2);
				q31_t gaF = cache.effPulseGainAbsFrom[f][pulseIdx[f]];
				gainAbs = gaF + (multiply_32x32_rshift32((gainAbs >> 1) - (gaF >> 1), morphRampQ) << 2);
			}
			out = add_saturate(out, multiply_32x32_rshift32(rc, gain) << 1);
			// Envelope follows the GAINED pulse so noise stays inside the burst
			// (silent gap stays silent - no hiss between glottal pulses)
			envelope = std::max(envelope, multiply_32x32_rshift32(rc, gainAbs) << 1);
		}

		// Voiced-gated noise: breath + articulation bursts live inside the
		// pulse envelope, so consonants articulate rather than hiss
		if (voicedNoise != 0) { // Breathless zones skip the whole noise path
			noiseState = noiseState * 1664525u + 1013904223u;
			q31_t noise = multiply_32x32_rshift32(static_cast<int32_t>(noiseState), voicedNoise);
			out = add_saturate(out, multiply_32x32_rshift32(noise, envelope) << 1);
		}

		// Cycle-end release: when the pulse burst is LONGER than the note
		// period (long bursts at low formant rates), the wrap amputates a
		// pulse mid-flight - a hard discontinuity EVERY cycle (audible as
		// harsh buzz at specific zone/wave settings, worst near the start of
		// Breath). A linear fade over the last 1/32 of the cycle makes every
		// truncation land at zero. Sim-verified: worst-case step 0.55 -> 0.05.
		uint32_t toWrap = ~evalPhase;
		if (toWrap < (1u << 27)) {
			out = multiply_32x32_rshift32(out, static_cast<q31_t>(toWrap << 4)) << 1;
		}
		out -= dcComp;

		if (applyAmplitude) {
			*thisSample = multiply_accumulate_32x32_rshift32_rounded(*thisSample, out, amplitude);
			thisSample++;
		}
		else {
			*thisSample++ = out;
		}
	}

	cache.noiseState = noiseState;
	*formantState = (static_cast<uint64_t>((fPhase[1] & 0xFFFFFFF0u) | static_cast<uint32_t>(pulseIdx[1])) << 32)
	                | ((fPhase[0] & 0xFFFFFFF0u) | static_cast<uint32_t>(pulseIdx[0]));
	*startPhase = phaseAtEnd;

#if ENABLE_FX_BENCHMARK
	FX_BENCH_STOP(bench_render);
#endif
}

} // namespace deluge::dsp
