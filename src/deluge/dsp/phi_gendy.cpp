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
constexpr float kGendyOutGain = 1.0f;               // RMS targeting handles loudness
constexpr float kGendyRefAmplitude = 1073741824.0f; // 2^30 = square PEAK parity under the amplitude<<1 convention

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
	float briteT = phi::evalTriangle(phase, 1.0f, kPhiGendyBrite);
	p.brite = briteT * briteT * 1.8f;

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
		case 4: { // STAIRS: quantized partial-sum, zone-varied level count
			// (2..8) and warped level spacing (steps are NOT uniform: warp<1
			// compresses levels near the rails, warp>1 near zero)
			float raw = p1 * std::sin(kGendyTwoPi * nf) + p2 * std::sin(kGendyTwoPi * 2.0f * nf + 1.7f);
			float stairLevels = 2.0f + std::abs(p3) * (6.0f / 0.45f);
			float stairWarp = 0.55f + std::abs(p2);
			float mag = std::min(std::abs(raw), 1.0f);
			float warped = std::pow(mag, stairWarp);
			float quant = std::round(warped * stairLevels) / stairLevels;
			h = std::copysign(std::pow(quant, 1.0f / stairWarp), raw);
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

	// Effective-law cache (see PHI_WEAVE): parked wave costs no lerps
	if (std::abs(cf - cache.effCfCached) > 0.0005f) {
		cache.effCfCached = cf;
		for (int32_t i = 0; i < kPhiGendyNumNodes; i++) {
			cache.effStep[i] = cfInv * cache.bankA.step[i] + cf * cache.bankB.step[i];
			cache.effBHi[i] = cfInv * cache.bankA.barrierHi[i] + cf * cache.bankB.barrierHi[i];
			cache.effBLo[i] = cfInv * cache.bankA.barrierLo[i] + cf * cache.bankB.barrierLo[i];
			cache.effHome[i] = cfInv * cache.bankA.home[i] + cf * cache.bankB.home[i];
		}
		cache.effVelCap = cfInv * cache.bankA.velCap + cf * cache.bankB.velCap;
		cache.effHomePull = cfInv * cache.bankA.homePull + cf * cache.bankB.homePull;
		cache.effCurve = cfInv * cache.bankA.curve + cf * cache.bankB.curve;
		cache.effBrite = cfInv * cache.bankA.brite + cf * cache.bankB.brite;
		cache.effWStep = cfInv * cache.bankA.widthStep + cf * cache.bankB.widthStep;
		cache.effWMin = cfInv * cache.bankA.widthMin + cf * cache.bankB.widthMin;
		cache.effWMax = cfInv * cache.bankA.widthMax + cf * cache.bankB.widthMax;
		cache.effJumpProb = cfInv * cache.bankA.jumpProb + cf * cache.bankB.jumpProb;
	}

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

	float velCap = cache.effVelCap;
	float homePull = cache.effHomePull;
	float curve = cache.effCurve;
	float brite = cache.effBrite;

	uint32_t noise = cache.noiseState;
	float mean = 0.0f;

	// Duration walk: widths take their own second-order step in elastic
	// barriers, startled by the same morph kicks, then renormalize so the
	// cycle length (pitch) stays exact
	float wStep = cache.effWStep;
	float wMin = cache.effWMin;
	float wMax = cache.effWMax;
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
	float jumpProb = cache.effJumpProb;
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
		float step = cache.effStep[i];
		float bHi = cache.effBHi[i];
		float bLo = cache.effBLo[i];
		float home = cache.effHome[i];

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
	float sumSq = 0.0f;
	for (int32_t i = 0; i < kPhiGendyNumNodes; i++) {
		float d = cache.a[i] - mean;
		peak = std::max(peak, std::abs(d));
		sumSq += d * d;
	}
	cache.agcPeak = std::max(peak, cache.agcPeak * 0.998f);
	// RMS-targeting loudness with a square-parity peak ceiling (see WEAVE)
	float rms = std::sqrt(sumSq * (1.0f / static_cast<float>(kPhiGendyNumNodes)));
	cache.agcRms = std::max(rms, cache.agcRms * 0.998f);
	float scaleByRms = 0.55f * kGendyRefAmplitude / std::max(cache.agcRms, 0.15f);
	float scaleByPeak = kGendyRefAmplitude / std::max(cache.agcPeak, 0.30f);
	float scaleTarget = kGendyOutGain * std::min(scaleByRms, scaleByPeak);
	if (cache.agcScale == 0.0f) {
		cache.agcScale = scaleTarget;
	}
	cache.agcScale += 0.10f * (scaleTarget - cache.agcScale);

	// Resample the variable-width polygon onto the uniform scan grid: the
	// duration walk lives entirely at tick time; the render's cheap uniform
	// lerp (and the de-zipper crossfade) are unchanged
	memcpy(cache.nodeQPrev, cache.nodeQ, sizeof(cache.nodeQPrev));
	float scale = cache.agcScale;
	float slots[kPhiGendyScanNodes];
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
			f4 *= f4; // f^8: near-staircase at full negative curve (bright crunch)
			frac += (-curve) * (f4 - frac);
		}
		float a0 = cache.a[seg] - mean;
		float a1 = cache.a[(seg + 1) & (kPhiGendyNumNodes - 1)] - mean;
		slots[j] = a0 + (a1 - a0) * frac;
	}
	// Brilliance: circular slot-to-slot high-shelf (first difference), a
	// pitch-invariant brightener applied entirely at tick time
	float prevSlot = slots[kPhiGendyScanNodes - 1];
	for (int32_t j = 0; j < kPhiGendyScanNodes; j++) {
		float raw = slots[j];
		float bright = (raw + brite * (raw - prevSlot)) * scale;
		prevSlot = raw;
		cache.nodeQ[j] = static_cast<q31_t>(std::clamp(bright, -2147483000.0f, 2147483000.0f));
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

namespace {
struct GendyStereoChar {
	float maxOffset; // Fraction of the cycle
	bool counter;    // Right tap mirrored within the cycle
	bool lag;        // Right tap reads the previous tick's polygon
};
constexpr GendyStereoChar kGendyStereoZones[8] = {
    {0.03f, false, false},  // Slim
    {0.0625f, false, true}, // Near: close tap, one tick behind
    {0.125f, false, false}, // Open
    {0.125f, false, true},  // Tilt: lagged
    {0.25f, false, false},  // Wide
    {0.25f, true, false},   // Sway: counter-scanned
    {0.5f, false, true},    // Split
    {0.5f, true, true},     // Vast
};
} // namespace

void renderPhiGendy(PhiGendyCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                    uint32_t phaseIncrement, uint32_t* startPhase, uint32_t retriggerPhase, int32_t amplitude,
                    int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade, uint32_t pulseWidth,
                    int32_t* bufferRStart, uint16_t stereoZone) {

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

	// Tick crossfade: smoothstep advanced by forward differences (3 adds per
	// sample; see PHI_WEAVE). The fade was plain-linear here before - the
	// smoothstep shaping also brings GENDY's tick boundaries to C1 for free.
	float hf = 1.0f / static_cast<float>(numSamples);
	auto ss = [](float t) { return t * t * (3.0f - 2.0f * t); };
	float y1 = ss(hf);
	float y2 = ss(2.0f * hf);
	float y3 = ss(3.0f * hf);
	q31_t tickFade = 0;
	q31_t fd1 = static_cast<q31_t>(y1 * 2147483647.0f);
	q31_t fd2 = static_cast<q31_t>((y2 - 2.0f * y1) * 2147483647.0f);
	const q31_t fd3 = static_cast<q31_t>((y3 - 3.0f * y2 + 3.0f * y1) * 2147483647.0f);

	int32_t* thisSample = bufferStart;
	int32_t* thisSampleR = bufferRStart;
	const GendyStereoChar& sc = kGendyStereoZones[(stereoZone >> 7) & 7];
	const uint32_t tapOffset =
	    static_cast<uint32_t>(static_cast<float>(stereoZone & 127u) * (1.0f / 127.0f) * sc.maxOffset * 4294967296.0);

	for (int32_t n = 0; n < numSamples; n++) {
		phase += phaseIncrement;
		tickFade += fd1;
		fd1 += fd2;
		fd2 += fd3;
		uint32_t evalPhase = phase + retriggerPhase;
		if (applyAmplitude) {
			amplitude += amplitudeIncrement;
		}

		if (evalPhase > phaseWidth) {
			if (applyAmplitude) {
				thisSample++;
				if (thisSampleR != nullptr) {
					thisSampleR++;
				}
			}
			else {
				*thisSample++ = 0;
				if (thisSampleR != nullptr) {
					*thisSampleR++ = 0;
				}
			}
			continue;
		}

		// Linear scan between scan-grid slots of both polygons, then tick
		// lerp. Halved differences: jumps flip breakpoint signs near full
		// scale, and the raw q31 difference wraps int32 (a spike per jump)
		uint32_t idx = evalPhase >> kPhiGendyNodeShift;
		q31_t frac31 = static_cast<q31_t>((evalPhase & 0x03FFFFFF) << 5);
		q31_t baseP = cache.nodeQPrev[idx];
		q31_t wPrev = baseP + (multiply_32x32_rshift32((cache.nodeQPrev[idx + 1] >> 1) - (baseP >> 1), frac31) << 2);
		q31_t baseC = cache.nodeQ[idx];
		q31_t wCur = baseC + (multiply_32x32_rshift32((cache.nodeQ[idx + 1] >> 1) - (baseC >> 1), frac31) << 2);
		q31_t out = wPrev + (multiply_32x32_rshift32((wCur >> 1) - (wPrev >> 1), tickFade) << 2);

		if (applyAmplitude) {
			*thisSample = multiply_accumulate_32x32_rshift32_rounded(*thisSample, out, amplitude);
			thisSample++;
		}
		else {
			*thisSample++ = out;
		}

		if (thisSampleR != nullptr) { // Second tap for the right channel
			uint32_t evalR = sc.counter ? (tapOffset - evalPhase) : (evalPhase + tapOffset);
			uint32_t idxR = evalR >> kPhiGendyNodeShift;
			q31_t fracR = static_cast<q31_t>((evalR & 0x03FFFFFF) << 5);
			q31_t bP = cache.nodeQPrev[idxR];
			q31_t wP = bP + (multiply_32x32_rshift32((cache.nodeQPrev[idxR + 1] >> 1) - (bP >> 1), fracR) << 2);
			q31_t outR;
			if (sc.lag) { // Previous tick's polygon only: ~3ms micro-slap
				outR = wP;
			}
			else {
				q31_t bC = cache.nodeQ[idxR];
				q31_t wC = bC + (multiply_32x32_rshift32((cache.nodeQ[idxR + 1] >> 1) - (bC >> 1), fracR) << 2);
				outR = wP + (multiply_32x32_rshift32((wC >> 1) - (wP >> 1), tickFade) << 2);
			}
			if (applyAmplitude) {
				*thisSampleR = multiply_accumulate_32x32_rshift32_rounded(*thisSampleR, outR, amplitude);
				thisSampleR++;
			}
			else {
				*thisSampleR++ = outR;
			}
		}
	}

	*startPhase = phaseAtEnd;

#if ENABLE_FX_BENCHMARK
	FX_BENCH_STOP(bench_render);
#endif
}

} // namespace deluge::dsp
