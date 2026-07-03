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

#include "dsp/phi_weave.hpp"
#include "io/debug/fx_benchmark.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp {

// ============================================================================
// Zone -> physics parameter builder (runs only on zone/gamma change)
// ============================================================================

namespace {

constexpr float kTwoPi = 6.283185307f;

// Spatial landscape: a phi triangle evaluated around the ring. zonePhase places
// the landscape (every zone position paints different terrain); spatialCycles
// sets how many bumps fit around the loop.
float evalSpatial(double zonePhase, float nodeFrac, float spatialCycles, const phi::PhiTriConfig& cfg) {
	float wrapped =
	    phi::wrapPhase(zonePhase * static_cast<double>(cfg.phiFreq) + static_cast<double>(nodeFrac * spatialCycles)
	                   + static_cast<double>(cfg.phaseOffset));
	if (cfg.bipolar) {
		return deluge::dsp::triangleFloat(wrapped, cfg.duty);
	}
	return deluge::dsp::triangleSimpleUnipolar(wrapped, cfg.duty);
}

} // namespace

PhiWeaveParams buildPhiWeaveParams(uint16_t zone, float phaseOffset) {
	PhiWeaveParams p{};

	double phase = static_cast<double>(zone) / 1023.0 + static_cast<double>(phaseOffset);

	// Global scalars
	float stiffT = phi::evalTriangle(phase, 1.0f, kPhiWeaveStiffBase);
	float stiffBase = 0.0008f * std::pow(75.0f, stiffT); // 0.0008..0.06, exponential
	float coupleT = phi::evalTriangle(phase, 1.0f, kPhiWeaveCoupleBase);
	float coupleBase = 0.010f + coupleT * coupleT * 0.28f;
	float dampT = phi::evalTriangle(phase, 1.0f, kPhiWeaveDampBase);
	float dampBase = 0.0015f + dampT * dampT * 0.0435f;

	// Home shape partials
	float p2 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomeP2) * 0.7f;
	float p3 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomeP3) * 0.55f;
	float p5 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomeP5) * 0.4f;
	float ph2 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomePh2) * kTwoPi;
	float ph3 = phi::evalTriangle(phase, 1.0f, kPhiWeaveHomePh3) * kTwoPi;

	// Spatial cycle counts derived from the zone too (1..4 bumps around the ring)
	float stiffCycles = 1.0f + phi::evalTriangle(phase, 1.0f, {phi::kPhi050, 1.0f, 0.470f, false}) * 3.0f;
	float dampCycles = 1.0f + phi::evalTriangle(phase, 1.0f, {phi::kPhi325, 1.0f, 0.720f, false}) * 3.0f;

	float homePeak = 0.0f;
	for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
		float nf = static_cast<float>(i) / static_cast<float>(kPhiWeaveNumNodes);

		float stiffLand = 1.0f + 0.8f * evalSpatial(phase, nf, stiffCycles, kPhiWeaveStiffLand1)
		                  + 0.5f * evalSpatial(phase, nf, stiffCycles * 2.0f, kPhiWeaveStiffLand2);
		p.stiffness[i] = std::clamp(stiffBase * stiffLand, 0.0002f, 0.12f);

		float dampLand = 1.0f + 0.85f * evalSpatial(phase, nf, dampCycles, kPhiWeaveDampLand1)
		                 + 0.5f * evalSpatial(phase, nf, dampCycles * 3.0f, kPhiWeaveDampLand2);
		p.damping[i] = std::clamp(dampBase * dampLand, 0.0005f, 0.09f);

		float coupleLand = 1.0f + 0.6f * evalSpatial(phase, nf, stiffCycles, kPhiWeaveCoupleLand);
		p.coupling[i] = std::clamp(coupleBase * coupleLand, 0.004f, 0.45f);

		float h = std::sin(kTwoPi * nf) + p2 * std::sin(2.0f * kTwoPi * nf + ph2)
		          + p3 * std::sin(3.0f * kTwoPi * nf + ph3) + p5 * std::sin(5.0f * kTwoPi * nf);
		p.home[i] = h;
		homePeak = std::max(homePeak, std::abs(h));
	}

	// Normalize the home shape to +/-1 peak — it is the baseline timbre the
	// string relaxes toward, so it sets the resting loudness of every zone
	if (homePeak > 0.01f) {
		float norm = 1.0f / homePeak;
		for (float& h : p.home) {
			h *= norm;
		}
	}

	float bowDepthT = phi::evalTriangle(phase, 1.0f, kPhiWeaveBowDepth);
	p.bowDepth = 0.0015f + bowDepthT * bowDepthT * 0.030f;
	p.bowRate = 0.002f + phi::evalTriangle(phase, 1.0f, kPhiWeaveBowRate) * 0.15f;
	p.bowPos = phi::evalTriangle(phase, 1.0f, kPhiWeaveBowPos);
	p.bowSpread = 0.06f + phi::evalTriangle(phase, 1.0f, kPhiWeaveBowSpread) * 0.20f;

	float travelT = phi::evalTriangle(phase, 1.0f, kPhiWeaveTravel);
	p.travelRate = travelT * travelT * travelT * 0.004f; // Cubic: mostly near-still, occasionally drifting

	p.pluckPos = phi::evalTriangle(phase, 1.0f, kPhiWeavePluckPos);
	p.pluckWidth = 0.03f + phi::evalTriangle(phase, 1.0f, kPhiWeavePluckWidth) * 0.20f;
	p.pluckAmp = 0.4f + phi::evalTriangle(phase, 1.0f, kPhiWeavePluckAmp) * 0.6f;

	p.morphBowGain = 0.5f + phi::evalTriangle(phase, 1.0f, kPhiWeaveMorphBow) * 4.0f;
	p.outGain = 0.70f + phi::evalTriangle(phase, 1.0f, kPhiWeaveOutGain) * 0.55f;

	return p;
}

// ============================================================================
// Physics tick (once per render buffer, per Sound — shared across voices)
// ============================================================================

namespace {

// One physics sub-tick at HALF the original per-buffer dt: spring constants
// scale by 1/4 (dt^2), first-order terms (damping, bow/travel rates, velocity
// bounds) by 1/2, AGC release by its square root. The string's audible mode
// frequencies and decay are unchanged - but its motion is sampled at ~688 Hz
// instead of ~344 Hz, pushing tick-sampling images of fast modes up an octave
// and halving their amplitude. Returns the AGC output scale.
float stepPhiWeavePhysics(PhiWeaveCache& cache, float cf) {
	const PhiWeaveParams& a = cache.bankA;
	const PhiWeaveParams& b = cache.bankB;
	float cfInv = 1.0f - cf;

	// Morph-bow: the crossfade's MOTION injects energy — turning the wave-index
	// knob bows the string. Strength comes from zone B (the morph target).
	float morphBow = 0.0f;
	if (cache.prevTickCf >= 0.0f) {
		morphBow = std::abs(cf - cache.prevTickCf) * (cfInv * a.morphBowGain + cf * b.morphBowGain);
	}
	cache.prevTickCf = cf;

	// Interpolated excitation scalars
	float bowDepth = cfInv * a.bowDepth + cf * b.bowDepth;
	float bowRate = cfInv * a.bowRate + cf * b.bowRate;
	float bowPos = cfInv * a.bowPos + cf * b.bowPos;
	float bowSpread = cfInv * a.bowSpread + cf * b.bowSpread;
	float travelRate = cfInv * a.travelRate + cf * b.travelRate;
	float outGain = cfInv * a.outGain + cf * b.outGain;

	// Bow oscillation (triangle, cheap; half-dt rate)
	cache.bowPhase += bowRate * 0.5f;
	if (cache.bowPhase >= 1.0f) {
		cache.bowPhase -= 1.0f;
	}
	float bowVal = (deluge::dsp::triangleSimpleUnipolar(cache.bowPhase, 1.0f) * 2.0f - 1.0f) * bowDepth;
	float invSpread = 1.0f / bowSpread;

	float* x = cache.x;
	float* v = cache.v;
	float peak = 0.0f;

	// Note-on pluck: displace a raised-cosine bump into the ring, in its own
	// pass so the acceleration loop sees a consistent post-pluck shape
	if (cache.pluckPending) {
		cache.pluckPending = false;
		float pluckPos = cfInv * a.pluckPos + cf * b.pluckPos;
		float pluckWidth = cfInv * a.pluckWidth + cf * b.pluckWidth;
		float pluckAmp = cfInv * a.pluckAmp + cf * b.pluckAmp;
		float invPluckWidth = 1.0f / pluckWidth;
		for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
			float nf = static_cast<float>(i) / static_cast<float>(kPhiWeaveNumNodes);
			float pd = nf - pluckPos;
			pd -= static_cast<float>(static_cast<int32_t>(pd + 1.5f)) - 1.0f; // wrap to [-0.5, 0.5)
			float pw = std::abs(pd) * invPluckWidth;
			if (pw < 1.0f) {
				x[i] += pluckAmp * (0.5f + 0.5f * std::cos(3.14159265f * pw));
			}
		}
	}

	for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
		float nf = static_cast<float>(i) / static_cast<float>(kPhiWeaveNumNodes);

		float k = cfInv * a.stiffness[i] + cf * b.stiffness[i];
		float c = cfInv * a.coupling[i] + cf * b.coupling[i];
		float d = cfInv * a.damping[i] + cf * b.damping[i];
		float home = cfInv * a.home[i] + cf * b.home[i];

		float xm1 = x[(i + kPhiWeaveNumNodes - 1) & (kPhiWeaveNumNodes - 1)];
		float xp1 = x[(i + 1) & (kPhiWeaveNumNodes - 1)];

		// Bow force: raised-cosine window around bowPos (ring distance)
		float bd = nf - bowPos;
		bd -= static_cast<float>(static_cast<int32_t>(bd + 1.5f)) - 1.0f; // wrap to [-0.5, 0.5)
		float bw = std::abs(bd) * invSpread;
		float bowForce = (bw < 1.0f) ? bowVal * (0.5f + 0.5f * std::cos(3.14159265f * bw)) : 0.0f;

		// Morph-bow: broadband agitation (cheap LCG noise)
		cache.noiseState = cache.noiseState * 1664525u + 1013904223u;
		float noise = static_cast<float>(static_cast<int32_t>(cache.noiseState)) * (1.0f / 2147483648.0f);
		bowForce += noise * morphBow * 0.02f;

		// Half-dt scalings: spring terms and forcing x1/4, damping x1/2
		float accel = 0.25f * (c * (xm1 + xp1 - 2.0f * x[i]) - k * (x[i] - home) + bowForce) - 0.5f * d * v[i];
		v[i] += accel;
		v[i] = std::clamp(v[i], -kPhiWeaveMaxVelocity * 0.5f, kPhiWeaveMaxVelocity * 0.5f);
	}

	// Position update after all accelerations (keeps neighbor reads consistent)
	for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
		x[i] += v[i];
		x[i] = std::clamp(x[i], -kPhiWeaveMaxDisplacement, kPhiWeaveMaxDisplacement);
		peak = std::max(peak, std::abs(x[i]));
	}

	// Slow AGC: normalize output so quiet zones (soft home shapes, heavy
	// damping) and violent ones land at comparable loudness. Attack instant,
	// release ~1.5s of ticks.
	cache.agcPeak = std::max(peak, cache.agcPeak * 0.999f); // sqrt(0.998) per half-dt step
	float scale = outGain * kPhiWeaveRefAmplitude / std::max(cache.agcPeak, 0.35f);

	// Ring rotation under the scan head (half-dt rate)
	cache.travelPhase += travelRate * 0.5f;
	if (cache.travelPhase >= 1.0f) {
		cache.travelPhase -= 1.0f;
	}
	else if (cache.travelPhase < 0.0f) {
		cache.travelPhase += 1.0f;
	}
	cache.travelOffset = static_cast<uint32_t>(cache.travelPhase * 4294967296.0);

	return scale;
}

void buildPhiWeaveTables(PhiWeaveCache& cache, float scale, q31_t* t0, q31_t* t1, q31_t* t2) {
	for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
		float s = cache.x[i] * scale;
		s = std::clamp(s, -2147483000.0f, 2147483000.0f);
		t0[i + 1] = static_cast<q31_t>(s);
	}
	auto pad = [](q31_t* t) {
		t[0] = t[kPhiWeaveNumNodes];
		t[kPhiWeaveNumNodes + 1] = t[1];
		t[kPhiWeaveNumNodes + 2] = t[2];
	};
	pad(t0);

	// Anti-aliasing mips: circular 3-tap binomial smoothing. One pass nulls
	// the spatial Nyquist (adjacent-node zigzag) entirely; the second clears
	// the top octave for the highest notes. (The Catmull-Rom scan's sinc^4
	// image rolloff handles the rest.)
	auto binomial = [&pad](const q31_t* src, q31_t* dst) {
		for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
			dst[i + 1] = static_cast<q31_t>(
			    (static_cast<int64_t>(src[i]) + 2 * static_cast<int64_t>(src[i + 1]) + static_cast<int64_t>(src[i + 2]))
			    >> 2);
		}
		pad(dst);
	};
	binomial(t0, t1);
	binomial(t1, t2);
}

// Per audio buffer: two physics sub-ticks, building the mid and current table
// sets; the render crossfades prev -> mid -> current across the buffer
void tickPhiWeave(PhiWeaveCache& cache, float cf) {
	memcpy(cache.nodeQPrev, cache.nodeQ, sizeof(cache.nodeQPrev));
	memcpy(cache.nodeQMip1Prev, cache.nodeQMip1, sizeof(cache.nodeQMip1Prev));
	memcpy(cache.nodeQMip2Prev, cache.nodeQMip2, sizeof(cache.nodeQMip2Prev));

	float scaleMid = stepPhiWeavePhysics(cache, cf);
	buildPhiWeaveTables(cache, scaleMid, cache.nodeQMid, cache.nodeQMidMip1, cache.nodeQMidMip2);
	float scaleCur = stepPhiWeavePhysics(cache, cf);
	buildPhiWeaveTables(cache, scaleCur, cache.nodeQ, cache.nodeQMip1, cache.nodeQMip2);

	if (!cache.tablesValid) { // First tick: nothing to fade from
		memcpy(cache.nodeQPrev, cache.nodeQ, sizeof(cache.nodeQPrev));
		memcpy(cache.nodeQMip1Prev, cache.nodeQMip1, sizeof(cache.nodeQMip1Prev));
		memcpy(cache.nodeQMip2Prev, cache.nodeQMip2, sizeof(cache.nodeQMip2Prev));
		memcpy(cache.nodeQMid, cache.nodeQ, sizeof(cache.nodeQMid));
		memcpy(cache.nodeQMidMip1, cache.nodeQMip1, sizeof(cache.nodeQMidMip1));
		memcpy(cache.nodeQMidMip2, cache.nodeQMip2, sizeof(cache.nodeQMidMip2));
		cache.tablesValid = true;
	}
}

} // namespace

namespace {

// Catmull-Rom scan through the tick-crossfaded string. Taps are lerped
// prev -> current first (same cost as per-table interpolation, simpler), then
// a 4-point Hermite reconstructs the waveform: interpolation images fall as
// sinc^4 instead of linear's sinc^2 - this was the residual "lofi" fizz.
// Taps are pre-shifted >>4 for Horner headroom and the result saturates on
// the way back up (Catmull-Rom can overshoot the tap range by ~1.25x).
// t^2 (3 - 2t) in Q31: zero derivative at both endpoints
[[gnu::always_inline]] inline q31_t smoothstepQ31(q31_t t) {
	q31_t t2 = multiply_32x32_rshift32(t, t) << 1;
	q31_t q = 0x60000000 - (t >> 1); // (3 - 2t) / 4
	return multiply_32x32_rshift32(t2, q) << 3;
}

[[gnu::always_inline]] inline q31_t scanCatmullRom(const q31_t* nodes, const q31_t* nodesPrev, uint32_t idx,
                                                   q31_t frac31, q31_t tickFade) {
	q31_t taps[4];
	for (int32_t k = 0; k < 4; k++) {
		q31_t pv = nodesPrev[idx + k];
		taps[k] = (pv + (multiply_32x32_rshift32(nodes[idx + k] - pv, tickFade) << 1)) >> 4;
	}
	q31_t c1 = (taps[2] - taps[0]) >> 1;
	q31_t c2 = taps[0] + 2 * taps[2] - ((5 * taps[1] + taps[3]) >> 1);
	q31_t c3 = ((3 * (taps[1] - taps[2])) >> 1) + ((taps[3] - taps[0]) >> 1);
	q31_t r = c2 + (multiply_32x32_rshift32(frac31, c3) << 1);
	r = c1 + (multiply_32x32_rshift32(frac31, r) << 1);
	r = taps[1] + (multiply_32x32_rshift32(frac31, r) << 1);
	return static_cast<q31_t>(std::clamp<int64_t>(static_cast<int64_t>(r) << 4, INT32_MIN, INT32_MAX));
}

} // namespace

// ============================================================================
// Main render: scan the ring per sample (Catmull-Rom, tick-crossfaded)
// ============================================================================

void renderPhiWeave(PhiWeaveCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                    uint32_t phaseIncrement, uint32_t* startPhase, uint32_t retriggerPhase, int32_t amplitude,
                    int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade, uint32_t pulseWidth) {

#if ENABLE_FX_BENCHMARK
	FX_BENCH_DECLARE(bench_render, "phi_weave", "render");
	FX_BENCH_START(bench_render);
#endif

	// Advance the physics once per audio buffer, no matter how many voices or
	// unison parts render from this cache
	if (AudioEngine::audioSampleTimer != cache.lastTickTime) {
		cache.lastTickTime = AudioEngine::audioSampleTimer;
		float cf = static_cast<float>(crossfade) / 2147483648.0f + 0.5f;
		tickPhiWeave(cache, std::clamp(cf, 0.0f, 1.0f));
	}

	uint32_t phase = *startPhase;
	uint32_t phaseAtEnd = phase + phaseIncrement * static_cast<uint32_t>(numSamples);
	const uint32_t scanOffset = retriggerPhase + cache.travelOffset;

	// Match the triangle-oscillator amplitude convention (as PHI_MORPH does)
	amplitude <<= 1;
	amplitudeIncrement <<= 1;

	// Pulse width deadzone: phase beyond phaseWidth outputs zero (parity with PHI_MORPH)
	const uint32_t phaseWidth = pulseWidth ? (0xFFFFFFFF - (pulseWidth << 1)) : 0xFFFFFFFF;

	// Pitch-adaptive table select: ~700 Hz and ~1800 Hz fundamentals
	// (phase-increment thresholds at 44.1kHz; the Catmull-Rom scan's sinc^4
	// image rolloff lets full detail run higher than linear scanning did)
	const q31_t* tabPrev = cache.nodeQPrev;
	const q31_t* tabMid = cache.nodeQMid;
	const q31_t* tabCur = cache.nodeQ;
	if (phaseIncrement > 175304787u) {
		tabPrev = cache.nodeQMip2Prev;
		tabMid = cache.nodeQMidMip2;
		tabCur = cache.nodeQMip2;
	}
	else if (phaseIncrement > 68174083u) {
		tabPrev = cache.nodeQMip1Prev;
		tabMid = cache.nodeQMidMip1;
		tabCur = cache.nodeQMip1;
	}

	// Two crossfade segments per buffer, one per physics sub-tick:
	// prev -> mid over the first half, mid -> current over the second. Each
	// ramp is smoothstep-shaped (zero slope at the ends), so node motion is
	// C1 at every join and at buffer boundaries.
	int32_t* thisSample = bufferStart;
	int32_t segStart = 0;

	for (int32_t seg = 0; seg < 2; seg++) {
		int32_t segEnd = (seg == 0) ? (numSamples >> 1) : numSamples;
		int32_t segLen = segEnd - segStart;
		if (segLen <= 0) {
			continue;
		}
		const q31_t* nodesPrev = (seg == 0) ? tabPrev : tabMid;
		const q31_t* nodes = (seg == 0) ? tabMid : tabCur;
		const q31_t tickFadeInc = 0x7FFFFFFF / segLen;
		q31_t tickFade = 0;

		if (applyAmplitude) {
			for (int32_t n = segStart; n < segEnd; n++) {
				phase += phaseIncrement;
				amplitude += amplitudeIncrement;
				tickFade += tickFadeInc;
				uint32_t evalPhase = phase + scanOffset;

				if (evalPhase > phaseWidth) {
					thisSample++;
					continue;
				}

				uint32_t idx = evalPhase >> kPhiWeaveNodeShift;
				q31_t frac31 = static_cast<q31_t>((evalPhase & 0x07FFFFFF) << 4);
				q31_t waveform = scanCatmullRom(nodes, nodesPrev, idx, frac31, smoothstepQ31(tickFade));

				*thisSample = multiply_accumulate_32x32_rshift32_rounded(*thisSample, waveform, amplitude);
				thisSample++;
			}
		}
		else {
			for (int32_t n = segStart; n < segEnd; n++) {
				phase += phaseIncrement;
				tickFade += tickFadeInc;
				uint32_t evalPhase = phase + scanOffset;

				if (evalPhase > phaseWidth) {
					*thisSample = 0;
					thisSample++;
					continue;
				}

				uint32_t idx = evalPhase >> kPhiWeaveNodeShift;
				q31_t frac31 = static_cast<q31_t>((evalPhase & 0x07FFFFFF) << 4);
				*thisSample = scanCatmullRom(nodes, nodesPrev, idx, frac31, smoothstepQ31(tickFade));
				thisSample++;
			}
		}
		segStart = segEnd;
	}

	*startPhase = phaseAtEnd;

#if ENABLE_FX_BENCHMARK
	FX_BENCH_STOP(bench_render);
#endif
}

} // namespace deluge::dsp
