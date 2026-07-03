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

#include "dsp/phi_gendy.hpp"
#include "io/debug/fx_benchmark.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp {

// ============================================================================
// Zone -> walk-law builder (runs only on zone/gamma change)
// ============================================================================

namespace {

constexpr float kGendyTwoPi = 6.283185307f;
constexpr float kGendyOutGain = 0.85f;
constexpr float kGendyRefAmplitude = 1073741824.0f; // 2^30, matches sibling convention

// Spatial landscape: a phi triangle evaluated around the polygon (same idiom
// as PHI_WEAVE's ring landscapes)
float gendySpatial(double zonePhase, float nodeFrac, float spatialCycles, const phi::PhiTriConfig& cfg) {
	float wrapped =
	    phi::wrapPhase(zonePhase * static_cast<double>(cfg.phiFreq) + static_cast<double>(nodeFrac * spatialCycles)
	                   + static_cast<double>(cfg.phaseOffset));
	if (cfg.bipolar) {
		return deluge::dsp::triangleFloat(wrapped, cfg.duty);
	}
	return deluge::dsp::triangleSimpleUnipolar(wrapped, cfg.duty);
}

} // namespace

PhiGendyParams buildPhiGendyParams(uint16_t zone, float phaseOffset) {
	PhiGendyParams p{};

	double phase = static_cast<double>(zone) / 1023.0 + static_cast<double>(phaseOffset);

	// Entropy: exponential step-size range. Low zones drift almost
	// imperceptibly; high zones jump hard enough that the reflections
	// themselves become the sound (the Xenakis grit).
	float stepT = phi::evalTriangle(phase, 1.0f, kPhiGendyStepBase);
	float stepBase = 0.0015f * std::pow(180.0f, stepT); // 0.0015 .. 0.27

	float barrierT = phi::evalTriangle(phase, 1.0f, kPhiGendyBarrierBase);
	float barrierBase = 0.25f + barrierT * 0.75f; // 0.25 .. 1.0

	p.velCap = 0.01f + phi::evalTriangle(phase, 1.0f, kPhiGendyVelCap) * 0.19f;

	// Home pull fades as entropy rises: calm zones have a timbre to return
	// to; frenzied zones are pure walk
	float pullT = phi::evalTriangle(phase, 1.0f, kPhiGendyHomePull);
	p.homePull = (0.02f + pullT * 0.20f) * (1.0f - stepT * 0.85f);

	p.startleGain = 0.03f + phi::evalTriangle(phase, 1.0f, kPhiGendyStartle) * 0.25f;

	// Duration walk: step exponential 0.0008..0.032 (lurch rate); barriers
	// widen with range t so narrow segments can approach spikes at the top
	float wStepT = phi::evalTriangle(phase, 1.0f, kPhiGendyWidthStep);
	p.widthStep = 0.0008f * std::pow(40.0f, wStepT);
	float wRangeT = phi::evalTriangle(phase, 1.0f, kPhiGendyWidthRange);
	float range = 0.25f + wRangeT * 0.60f; // 0.25..0.85
	// Jump probability: exponential 0.0001..0.5 per tick per breakpoint
	// (calm = a snap every ~2 seconds across the whole polygon; top = ~170/sec)
	float jumpT = phi::evalTriangle(phase, 1.0f, kPhiGendyJumpProb);
	p.jumpProb = 0.0001f * std::pow(5000.0f, jumpT);
	p.widthMin = (1.0f - range) * (1.0f / 16.0f);
	p.widthMax = (1.0f + 2.0f * range) * (1.0f / 16.0f);

	// Home shape: family archetype selected by zone
	float p1 = phi::evalTriangle(phase, 1.0f, kPhiGendyHomeP1) * 0.9f;
	float p2 = phi::evalTriangle(phase, 1.0f, kPhiGendyHomeP2) * 0.6f;
	float p3 = phi::evalTriangle(phase, 1.0f, kPhiGendyHomeP3) * 0.45f;
	int32_t homeFamily = std::min(static_cast<int32_t>(5),
	                              static_cast<int32_t>(phi::evalTriangle(phase, 1.0f, kPhiGendyHomeFamily) * 6.0f));
	uint32_t frozenSeed = static_cast<uint32_t>(phase * 4096.0) * 2654435761u + 12345u;

	p.curve = phi::evalTriangle(phase, 1.0f, kPhiGendyCurve);

	float stepCycles = 1.0f + phi::evalTriangle(phase, 1.0f, {phi::kPhi050, 1.0f, 0.470f, false}) * 3.0f;
	float barrierCycles = 1.0f + phi::evalTriangle(phase, 1.0f, {phi::kPhi325, 1.0f, 0.720f, false}) * 3.0f;

	float homePeak = 0.0001f;
	for (int32_t i = 0; i < kPhiGendyNumNodes; i++) {
		float nf = static_cast<float>(i) / static_cast<float>(kPhiGendyNumNodes);

		// Per-node entropy and cage
		float stepLand = 0.25f + 0.75f * gendySpatial(phase, nf, stepCycles, kPhiGendyStepLand);
		p.step[i] = stepBase * stepLand;

		float width = barrierBase * (0.30f + 0.70f * gendySpatial(phase, nf, barrierCycles, kPhiGendyBarrierLand));
		float center = gendySpatial(phase, nf, 2.0f, kPhiGendyCenterLand) * (1.0f - width) * 0.6f;
		p.barrierHi[i] = center + width;
		p.barrierLo[i] = center - width;

		float h;
		switch (homeFamily) {
		case 1: // RAMP: saw-like
			h = 2.0f * nf - 1.0f;
			break;
		case 2: // SQUARE: hard-clipped fundamental
			h = std::clamp(std::sin(kGendyTwoPi * nf + p2) * 3.0f, -1.0f, 1.0f);
			break;
		case 3: { // SPIKE: narrow raised-cosine bump (nasal/formant)
			float d = nf - (0.5f + p3 * 0.4f);
			d -= std::floor(d + 0.5f);
			float wq = std::abs(d) * 5.0f;
			h = (wq < 1.0f) ? (0.5f + 0.5f * std::cos(3.14159265f * wq)) * 2.0f - 0.5f : -0.5f;
			break;
		}
		case 4: { // STAIRS: quantized partial-sum (organ/8-bit)
			float raw = p1 * std::sin(kGendyTwoPi * nf) + p2 * std::sin(kGendyTwoPi * 2.0f * nf + 1.7f);
			h = std::round(raw * 3.0f) * (1.0f / 3.0f);
			break;
		}
		case 5: { // FROZEN NOISE: fixed random polygon per zone position (glassy)
			frozenSeed = frozenSeed * 1664525u + 1013904223u;
			h = static_cast<float>(static_cast<int32_t>(frozenSeed)) * (1.0f / 2147483648.0f);
			break;
		}
		default: // PARTIALS: low phi partial sum
			h = p1 * std::sin(kGendyTwoPi * nf) + p2 * std::sin(kGendyTwoPi * 2.0f * nf + 1.7f)
			    + p3 * std::sin(kGendyTwoPi * 3.0f * nf + 4.1f);
			break;
		}
		p.home[i] = h;
		homePeak = std::max(homePeak, std::abs(h));
	}

	// Normalize home into the cage and keep it audible
	for (int32_t i = 0; i < kPhiGendyNumNodes; i++) {
		p.home[i] *= 0.8f / homePeak;
		p.home[i] = std::clamp(p.home[i], p.barrierLo[i], p.barrierHi[i]);
	}

	return p;
}

// ============================================================================
// The double random walk (once per audio buffer)
// ============================================================================

namespace {

void tickPhiGendy(PhiGendyCache& cache, q31_t crossfade, uint32_t phaseIncrement) {
	float cf = std::clamp(static_cast<float>(crossfade) / 2147483648.0f + 0.5f, 0.0f, 1.0f);
	float cfInv = 1.0f - cf;

	// Startle: crossfade motion (and note-on) kicks velocity noise into the
	// walkers - the morph gesture. Decays fast; it's an event, not a state.
	if (cache.prevCf >= 0.0f) {
		float gain = cfInv * cache.bankA.startleGain + cf * cache.bankB.startleGain;
		float kick = std::abs(cf - cache.prevCf) * gain * 30.0f;
		if (cache.startlePending) {
			kick = std::max(kick, gain);
			cache.startlePending = false;
		}
		cache.startleEnv = std::max(kick, cache.startleEnv * 0.5f);
	}
	else if (cache.startlePending) {
		cache.startleEnv = cfInv * cache.bankA.startleGain + cf * cache.bankB.startleGain;
		cache.startlePending = false;
	}
	cache.prevCf = cf;

	float velCap = cfInv * cache.bankA.velCap + cf * cache.bankB.velCap;
	float homePull = cfInv * cache.bankA.homePull + cf * cache.bankB.homePull;
	float curve = cfInv * cache.bankA.curve + cf * cache.bankB.curve;

	uint32_t noise = cache.noiseState;
	float mean = 0.0f;

	// Duration walk: widths take their own second-order step in elastic
	// barriers, startled by the same morph kicks, then renormalize so the
	// cycle length (pitch) stays exact
	float wStep = cfInv * cache.bankA.widthStep + cf * cache.bankB.widthStep;
	float wMin = cfInv * cache.bankA.widthMin + cf * cache.bankB.widthMin;
	float wMax = cfInv * cache.bankA.widthMax + cf * cache.bankB.widthMax;
	if (!cache.widthsInit) {
		cache.widthsInit = true;
		for (float& wi : cache.w) {
			wi = 1.0f / 16.0f;
		}
	}
	// Intermittency: startle raises the jump probability (gestures = flurries).
	// The rate scales with PITCH (reference C3), so the character is snaps
	// per waveform cycle, not per second - consistent across the keyboard.
	// (Shared walk: with a chord, the first-ticking voice sets the rate.)
	float pitchScale = static_cast<float>(phaseIncrement) * (1.0f / 12742000.0f);
	float jumpProb = cfInv * cache.bankA.jumpProb + cf * cache.bankB.jumpProb;
	// Startle adds a MILD probability boost, and (below) startle-era jumps
	// commit only partway: sustained knob motion was triggering hundreds of
	// full teleports per second - audible as crackle on the wave knob
	jumpProb = std::min(0.8f, jumpProb * pitchScale + cache.startleEnv * 0.25f);
	float startleSoften = 1.0f / (1.0f + cache.startleEnv * 6.0f);
	uint32_t jumpGate = static_cast<uint32_t>(jumpProb * 4294967295.0f);

	float wSum = 0.0f;
	for (int32_t i = 0; i < kPhiGendyNumNodes; i++) {
		noise = noise * 1664525u + 1013904223u;
		uint32_t gateDraw = noise;
		noise = noise * 1664525u + 1013904223u;
		float r = static_cast<float>(static_cast<int32_t>(noise)) * (1.0f / 2147483648.0f);
		float wi = cache.w[i];
		if (gateDraw < jumpGate) {
			// Width jump: leap most of the way toward a fresh random width
			float target = wMin + (wMax - wMin) * (r * 0.5f + 0.5f);
			wi += (target - wi) * 0.7f * startleSoften;
			cache.vw[i] = 0.0f;
		}
		else {
			float vel = cache.vw[i] * 0.9f + wStep * r * 0.015f + homePull * ((1.0f / 16.0f) - wi);
			cache.vw[i] = std::clamp(vel, -0.02f, 0.02f);
			wi += cache.vw[i];
		}
		cache.w[i] = std::clamp(wi, wMin, wMax);
		wSum += cache.w[i];
	}
	float wNorm = 1.0f / wSum;

	for (int32_t i = 0; i < kPhiGendyNumNodes; i++) {
		// Walk LAWS morph; walker STATE persists (click-free by construction)
		float step = cfInv * cache.bankA.step[i] + cf * cache.bankB.step[i];
		float bHi = cfInv * cache.bankA.barrierHi[i] + cf * cache.bankB.barrierHi[i];
		float bLo = cfInv * cache.bankA.barrierLo[i] + cf * cache.bankB.barrierLo[i];
		float home = cfInv * cache.bankA.home[i] + cf * cache.bankB.home[i];

		noise = noise * 1664525u + 1013904223u;
		uint32_t gateDraw = noise;
		noise = noise * 1664525u + 1013904223u;
		float r = static_cast<float>(static_cast<int32_t>(noise)) * (1.0f / 2147483648.0f);

		float amp = cache.a[i];
		float vel = cache.v[i];
		if (gateDraw < jumpGate) {
			// JUMP: leap toward a fresh random target in the cage (Xenakis
			// drew new breakpoints as discrete events). The render's tick
			// crossfade turns each jump into a 3ms snap - click-free but
			// immediate. Step size scales how far the leap commits.
			float target = bLo + (bHi - bLo) * (r * 0.5f + 0.5f);
			float commit = std::min(1.0f, 0.35f + step * 4.0f) * startleSoften;
			amp += (target - amp) * commit;
			vel = 0.0f;
		}
		else {
			// HOLD: spring toward home with barely-there drift - the shape
			// stands still between events, so calm zones read as tonal
			vel = vel * 0.9f + step * r * 0.02f;
			vel = std::clamp(vel, -velCap, velCap);
			amp += vel + homePull * (home - amp);
		}

		amp = std::clamp(amp, bLo, bHi);
		cache.a[i] = amp;
		cache.v[i] = vel;
		mean += amp;
	}
	cache.noiseState = noise;
	mean *= 1.0f / static_cast<float>(kPhiGendyNumNodes);

	// Slow AGC: normalize the polygon's peak so narrow-cage zones land at
	// the same loudness as wide ones (instant attack, ~1.5s release, slewed
	// scale so level changes never step at tick rate)
	float peak = 0.0f;
	for (int32_t i = 0; i < kPhiGendyNumNodes; i++) {
		peak = std::max(peak, std::abs(cache.a[i] - mean));
	}
	cache.agcPeak = std::max(peak, cache.agcPeak * 0.998f);
	float scaleTarget = kGendyOutGain * kGendyRefAmplitude / std::max(cache.agcPeak, 0.30f);
	if (cache.agcScale == 0.0f) {
		cache.agcScale = scaleTarget;
	}
	cache.agcScale += 0.10f * (scaleTarget - cache.agcScale);

	// Resample the variable-width polygon onto the uniform scan grid: the
	// duration walk lives entirely at tick time; the render's cheap uniform
	// lerp (and the de-zipper crossfade) are unchanged
	memcpy(cache.nodeQPrev, cache.nodeQ, sizeof(cache.nodeQPrev));
	float scale = cache.agcScale;
	int32_t seg = 0;
	float segStart = 0.0f;
	float segWidth = cache.w[0] * wNorm;
	float invSegWidth = 1.0f / segWidth;
	for (int32_t j = 0; j < kPhiGendyScanNodes; j++) {
		float u = static_cast<float>(j) * (1.0f / static_cast<float>(kPhiGendyScanNodes));
		while (u >= segStart + segWidth && seg < kPhiGendyNumNodes - 1) {
			segStart += segWidth;
			seg++;
			segWidth = cache.w[seg] * wNorm;
			invSegWidth = 1.0f / segWidth;
		}
		float frac = std::min((u - segStart) * invSegWidth, 1.0f);
		// Curve: reshape the segment transition (smooth = rounded/dark,
		// hold-like = staircase/buzzy)
		if (curve > 0.0f) {
			float ss = frac * frac * (3.0f - 2.0f * frac);
			frac += curve * (ss - frac);
		}
		else if (curve < 0.0f) {
			float f4 = frac * frac;
			f4 *= f4;
			frac += (-curve) * (f4 - frac);
		}
		float a0 = cache.a[seg] - mean;
		float a1 = cache.a[(seg + 1) & (kPhiGendyNumNodes - 1)] - mean;
		cache.nodeQ[j] = static_cast<q31_t>((a0 + (a1 - a0) * frac) * scale);
	}
	cache.nodeQ[kPhiGendyScanNodes] = cache.nodeQ[0];
	if (!cache.tablesValid) { // First tick: nothing to fade from
		memcpy(cache.nodeQPrev, cache.nodeQ, sizeof(cache.nodeQPrev));
		cache.tablesValid = true;
	}
}

} // namespace

// ============================================================================
// Main render: scan the current polygon at the note pitch
// ============================================================================

void renderPhiGendy(PhiGendyCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                    uint32_t phaseIncrement, uint32_t* startPhase, uint32_t retriggerPhase, int32_t amplitude,
                    int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade, uint32_t pulseWidth) {

#if ENABLE_FX_BENCHMARK
	FX_BENCH_DECLARE(bench_render, "phi_gendy", "render");
	FX_BENCH_START(bench_render);
#endif

	// Advance the walk once per audio buffer, shared across voices/unison
	if (AudioEngine::audioSampleTimer != cache.lastTickTime) {
		cache.lastTickTime = AudioEngine::audioSampleTimer;
		tickPhiGendy(cache, crossfade, phaseIncrement);
	}

	uint32_t phase = *startPhase;
	uint32_t phaseAtEnd = phase + phaseIncrement * static_cast<uint32_t>(numSamples);

	// Match the triangle-oscillator amplitude convention (as the siblings do)
	amplitude <<= 1;
	amplitudeIncrement <<= 1;

	const uint32_t phaseWidth = pulseWidth ? (0xFFFFFFFF - (pulseWidth << 1)) : 0xFFFFFFFF;

	// Tick crossfade ramp: prev polygon -> current polygon across this buffer
	const q31_t tickFadeInc = 0x7FFFFFFF / numSamples;
	q31_t tickFade = 0;

	int32_t* thisSample = bufferStart;

	for (int32_t n = 0; n < numSamples; n++) {
		phase += phaseIncrement;
		tickFade += tickFadeInc;
		uint32_t evalPhase = phase + retriggerPhase;
		if (applyAmplitude) {
			amplitude += amplitudeIncrement;
		}

		if (evalPhase > phaseWidth) {
			if (applyAmplitude) {
				thisSample++;
			}
			else {
				*thisSample++ = 0;
			}
			continue;
		}

		// Linear scan between scan-grid slots of both polygons, then tick lerp
		uint32_t idx = evalPhase >> kPhiGendyNodeShift;
		q31_t frac31 = static_cast<q31_t>((evalPhase & 0x03FFFFFF) << 5);
		q31_t baseP = cache.nodeQPrev[idx];
		q31_t wPrev = baseP + (multiply_32x32_rshift32(cache.nodeQPrev[idx + 1] - baseP, frac31) << 1);
		q31_t baseC = cache.nodeQ[idx];
		q31_t wCur = baseC + (multiply_32x32_rshift32(cache.nodeQ[idx + 1] - baseC, frac31) << 1);
		q31_t out = wPrev + (multiply_32x32_rshift32(wCur - wPrev, tickFade) << 1);

		if (applyAmplitude) {
			*thisSample = multiply_accumulate_32x32_rshift32_rounded(*thisSample, out, amplitude);
			thisSample++;
		}
		else {
			*thisSample++ = out;
		}
	}

	*startPhase = phaseAtEnd;

#if ENABLE_FX_BENCHMARK
	FX_BENCH_STOP(bench_render);
#endif
}

} // namespace deluge::dsp
