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

void tickPhiWeave(PhiWeaveCache& cache, float cf) {
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

	// Bow oscillation (triangle, cheap)
	cache.bowPhase += bowRate;
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

		float accel = c * (xm1 + xp1 - 2.0f * x[i]) - k * (x[i] - home) - d * v[i] + bowForce;
		v[i] += accel;
		v[i] = std::clamp(v[i], -kPhiWeaveMaxVelocity, kPhiWeaveMaxVelocity);
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
	cache.agcPeak = std::max(peak, cache.agcPeak * 0.998f);
	float scale = outGain * kPhiWeaveRefAmplitude / std::max(cache.agcPeak, 0.35f);

	// Ring rotation under the scan head
	cache.travelPhase += travelRate;
	if (cache.travelPhase >= 1.0f) {
		cache.travelPhase -= 1.0f;
	}
	else if (cache.travelPhase < 0.0f) {
		cache.travelPhase += 1.0f;
	}
	cache.travelOffset = static_cast<uint32_t>(cache.travelPhase * 4294967296.0);

	for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
		float s = x[i] * scale;
		s = std::clamp(s, -2147483000.0f, 2147483000.0f);
		cache.nodeQ[i] = static_cast<q31_t>(s);
	}
	cache.nodeQ[kPhiWeaveNumNodes] = cache.nodeQ[0];

	// Anti-aliasing mips: circular 3-tap binomial smoothing. One pass nulls
	// the spatial Nyquist (adjacent-node zigzag) entirely; the second pass
	// (applied twice more) clears the top two octaves for the highest notes.
	auto binomial = [](const q31_t* src, q31_t* dst) {
		for (int32_t i = 0; i < kPhiWeaveNumNodes; i++) {
			int32_t prev = (i == 0) ? kPhiWeaveNumNodes - 1 : i - 1;
			int32_t next = (i == kPhiWeaveNumNodes - 1) ? 0 : i + 1;
			dst[i] = static_cast<q31_t>(
			    (static_cast<int64_t>(src[prev]) + 2 * static_cast<int64_t>(src[i]) + static_cast<int64_t>(src[next]))
			    >> 2);
		}
		dst[kPhiWeaveNumNodes] = dst[0];
	};
	binomial(cache.nodeQ, cache.nodeQMip1);
	binomial(cache.nodeQMip1, cache.nodeQMip2);
	binomial(cache.nodeQMip2, cache.nodeQMip2);
}

} // namespace

// ============================================================================
// Main render: scan the ring per sample (linear interp, branch-free)
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

	// Pitch-adaptive table select: ~500 Hz and ~1200 Hz fundamentals
	// (phase-increment thresholds at 44.1kHz)
	const q31_t* nodes = cache.nodeQ;
	if (phaseIncrement > 116869858u) {
		nodes = cache.nodeQMip2;
	}
	else if (phaseIncrement > 48695774u) {
		nodes = cache.nodeQMip1;
	}
	int32_t* thisSample = bufferStart;

	if (applyAmplitude) {
		for (int32_t n = 0; n < numSamples; n++) {
			phase += phaseIncrement;
			amplitude += amplitudeIncrement;
			uint32_t evalPhase = phase + scanOffset;

			if (evalPhase > phaseWidth) {
				thisSample++;
				continue;
			}

			uint32_t idx = evalPhase >> kPhiWeaveNodeShift;
			q31_t frac31 = static_cast<q31_t>((evalPhase & 0x07FFFFFF) << 4);
			q31_t base = nodes[idx];
			q31_t delta = nodes[idx + 1] - base;
			q31_t waveform = base + (multiply_32x32_rshift32(delta, frac31) << 1);

			*thisSample = multiply_accumulate_32x32_rshift32_rounded(*thisSample, waveform, amplitude);
			thisSample++;
		}
	}
	else {
		for (int32_t n = 0; n < numSamples; n++) {
			phase += phaseIncrement;
			uint32_t evalPhase = phase + scanOffset;

			if (evalPhase > phaseWidth) {
				*thisSample = 0;
				thisSample++;
				continue;
			}

			uint32_t idx = evalPhase >> kPhiWeaveNodeShift;
			q31_t frac31 = static_cast<q31_t>((evalPhase & 0x07FFFFFF) << 4);
			q31_t base = nodes[idx];
			q31_t delta = nodes[idx + 1] - base;
			*thisSample = base + (multiply_32x32_rshift32(delta, frac31) << 1);
			thisSample++;
		}
	}

	*startPhase = phaseAtEnd;

#if ENABLE_FX_BENCHMARK
	FX_BENCH_STOP(bench_render);
#endif
}

} // namespace deluge::dsp
