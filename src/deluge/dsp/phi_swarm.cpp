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

#include "dsp/phi_swarm.hpp"
#include "dsp/oscillators/sine_osc.h"
#include "io/debug/fx_benchmark.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp {

namespace {

// Triangle coupling force (Kuramoto theory only needs an odd periodic
// coupling with a stable zero - verified equivalent tongue structure by
// simulation). Unlike a sine, the force keeps CONSTANT slope up to a sharp
// corner at the extremum: locks strain linearly, then release abruptly -
// snappier pull-offs. Also multiply-free. Sine-phased: zero at 0, peak at
// the quarter cycle.
[[gnu::always_inline]] inline int32_t couplingTriangle(uint32_t phase) {
	uint32_t p = phase + 0x40000000u;
	uint32_t folded = (p < 0x80000000u) ? p : (0xFFFFFFFFu - p);
	return static_cast<int32_t>((folded - 0x40000000u) << 1);
}

// Zone anchors (Still, Drift, Pull, Swarm, Flock, Surge, Fray, Chaos).
// Ratios sit on / near / far-from Arnold tongues; couplings and temperature
// trace the path from crystalline lock to melted noise.
constexpr float kAnchorRatio1[8] = {1.000f, 1.004f, 1.022f, 1.618f, 1.260f, 1.720f, 2.330f, 1.414f};
constexpr float kAnchorRatio2[8] = {2.000f, 1.997f, 1.508f, 2.618f, 1.190f, 3.110f, 3.870f, 2.236f};
constexpr float kAnchorKM[8] = {0.0200f, 0.0022f, 0.0080f, 0.0060f, 0.0015f, 0.0080f, 0.0025f, 0.1200f};
constexpr float kAnchorK12[8] = {0.0020f, 0.0010f, 0.0020f, 0.0060f, 0.0250f, 0.0080f, 0.0040f, 0.0600f};
constexpr float kAnchorTemp[8] = {0.0000f, 0.0003f, 0.0007f, 0.0020f, 0.0015f, 0.0090f, 0.0220f, 0.0550f};
constexpr float kAnchorRing[8] = {0.10f, 0.12f, 0.20f, 0.30f, 0.25f, 0.45f, 0.35f, 0.50f};
// Beat-AM is strongest where the beat IS the story (Drift/Pull), present
// everywhere the swarm moves, minimal when locked
constexpr float kAnchorBeat[8] = {0.05f, 0.40f, 0.50f, 0.30f, 0.35f, 0.30f, 0.25f, 0.20f};

float lerpAnchor(const float* table, float pos, int32_t idx) {
	return table[idx] + (table[idx + 1] - table[idx]) * pos;
}

} // namespace

PhiSwarmParams buildPhiSwarmParams(uint16_t zone, float phaseOffset) {
	PhiSwarmParams p{};

	double phase = static_cast<double>(zone) / 1023.0 + static_cast<double>(phaseOffset);

	float zonePos = static_cast<float>(zone) / 1023.0f * 7.0f;
	int32_t zi = std::min(static_cast<int32_t>(zonePos), static_cast<int32_t>(6));
	float zf = zonePos - static_cast<float>(zi);

	// Ratio wander: multiplicative, up to ~+/-6% - enough to cross a tongue
	// boundary (lock <-> pull <-> quasiperiodic) without leaving the anchor's
	// musical neighborhood
	float r1 =
	    lerpAnchor(kAnchorRatio1, zf, zi) * std::exp2(phi::evalTriangle(phase, 1.0f, kPhiSwarmRatio1Wander) * 0.085f);
	float r2 =
	    lerpAnchor(kAnchorRatio2, zf, zi) * std::exp2(phi::evalTriangle(phase, 1.0f, kPhiSwarmRatio2Wander) * 0.085f);
	p.ratio1FP = static_cast<uint32_t>(std::clamp(r1, 0.25f, 8.0f) * 65536.0f);
	p.ratio2FP = static_cast<uint32_t>(std::clamp(r2, 0.25f, 8.0f) * 65536.0f);

	float kWander = std::exp2(phi::evalTriangle(phase, 1.0f, kPhiSwarmCoupleWander) * 1.0f);
	float kM = lerpAnchor(kAnchorKM, zf, zi) * kWander;
	float k12 = lerpAnchor(kAnchorK12, zf, zi) * std::exp2(phi::evalTriangle(phase, 1.0f, kPhiSwarmCrossWander) * 1.0f);
	p.k1mFP = static_cast<uint32_t>(std::clamp(kM, 0.0f, 0.25f) * 65536.0f);
	p.k2mFP = static_cast<uint32_t>(std::clamp(kM * 0.8f, 0.0f, 0.25f) * 65536.0f);
	p.k12FP = static_cast<uint32_t>(std::clamp(k12, 0.0f, 0.25f) * 65536.0f);

	float temp = lerpAnchor(kAnchorTemp, zf, zi) * (0.4f + phi::evalTriangle(phase, 1.0f, kPhiSwarmTempWander) * 1.6f);
	p.tempFP = static_cast<uint32_t>(std::clamp(temp, 0.0f, 0.2f) * 65536.0f);

	// Output weights: balance between the slaves, plus the ring cross-term
	// (sum/difference partials from already-computed sines - cheap richness)
	float balance = phi::evalTriangle(phase, 1.0f, kPhiSwarmBalanceWander) * 0.35f;
	float ring = lerpAnchor(kAnchorRing, zf, zi) * (0.5f + phi::evalTriangle(phase, 1.0f, kPhiSwarmRingWander));
	ring = std::clamp(ring, 0.0f, 0.7f);
	// Weights sum to <= 1 so coincident slave peaks can't clip the saturating
	// output sum (w1 + w2 + wRing > 1 hard-clipped in pulses at the beat rate)
	float w1 = (0.5f + balance) * (1.0f - ring);
	float w2 = (0.5f - balance) * (1.0f - ring);
	p.w1 = static_cast<q31_t>(w1 * 2147483647.0f);
	p.w2 = static_cast<q31_t>(w2 * 2147483647.0f);
	p.wRing = static_cast<q31_t>(ring * 2147483647.0f);

	float beat = lerpAnchor(kAnchorBeat, zf, zi) * (0.5f + phi::evalTriangle(phase, 1.0f, kPhiSwarmBeatWander));
	p.wBeat = static_cast<q31_t>(std::clamp(beat, 0.0f, 0.45f) * 2147483647.0f);

	p.annealGain = 0.004f + phi::evalTriangle(phase, 1.0f, kPhiSwarmAnneal) * 0.05f;

	return p;
}

// ============================================================================
// Main render
// ============================================================================

void renderPhiSwarm(PhiSwarmCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                    uint32_t phaseIncrement, uint32_t* startPhase, uint64_t* slavePhases, uint32_t retriggerPhase,
                    int32_t amplitude, int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade,
                    uint32_t pulseWidth) {

#if ENABLE_FX_BENCHMARK
	FX_BENCH_DECLARE(bench_render, "phi_swarm", "render");
	FX_BENCH_START(bench_render);
#endif

	// Rebuild the crossfaded effective params when the smoothed crossfade moves
	if (crossfade != cache.prevCrossfade) {
		float cf = std::clamp(static_cast<float>(crossfade) / 2147483648.0f + 0.5f, 0.0f, 1.0f);
		float cfInv = 1.0f - cf;
		auto lerpU = [cf, cfInv](uint32_t a, uint32_t b) {
			return static_cast<uint32_t>(cfInv * static_cast<float>(a) + cf * static_cast<float>(b));
		};
		auto lerpQ = [cf, cfInv](q31_t a, q31_t b) {
			return static_cast<q31_t>(cfInv * static_cast<float>(a) + cf * static_cast<float>(b));
		};
		cache.eff.ratio1FP = lerpU(cache.bankA.ratio1FP, cache.bankB.ratio1FP);
		cache.eff.ratio2FP = lerpU(cache.bankA.ratio2FP, cache.bankB.ratio2FP);
		cache.eff.k1mFP = lerpU(cache.bankA.k1mFP, cache.bankB.k1mFP);
		cache.eff.k2mFP = lerpU(cache.bankA.k2mFP, cache.bankB.k2mFP);
		cache.eff.k12FP = lerpU(cache.bankA.k12FP, cache.bankB.k12FP);
		cache.eff.tempFP = lerpU(cache.bankA.tempFP, cache.bankB.tempFP);
		cache.eff.w1 = lerpQ(cache.bankA.w1, cache.bankB.w1);
		cache.eff.w2 = lerpQ(cache.bankA.w2, cache.bankB.w2);
		cache.eff.wRing = lerpQ(cache.bankA.wRing, cache.bankB.wRing);
		cache.eff.wBeat = lerpQ(cache.bankA.wBeat, cache.bankB.wBeat);
		cache.prevCrossfade = crossfade;
	}

	// Annealing: crossfade MOTION heats the network; parked, it cools (~0.85
	// per buffer, so locks audibly re-form over a few hundred ms)
	if (AudioEngine::audioSampleTimer != cache.lastEnvTime) {
		cache.lastEnvTime = AudioEngine::audioSampleTimer;
		float cf = std::clamp(static_cast<float>(crossfade) / 2147483648.0f + 0.5f, 0.0f, 1.0f);
		if (cache.prevCf >= 0.0f) {
			float cfInv = 1.0f - cf;
			float gain = cfInv * cache.bankA.annealGain + cf * cache.bankB.annealGain;
			cache.annealEnv = std::max(std::abs(cf - cache.prevCf) * gain * 40.0f, cache.annealEnv * 0.85f);
		}
		cache.prevCf = cf;
		float heat = std::min(cache.annealEnv, 0.15f);
		cache.effTempFP = cache.eff.tempFP + static_cast<uint32_t>(heat * 65536.0f);
	}

	// Per-buffer conversions: everything scales with the master increment so
	// locking behavior is pitch-invariant
	uint32_t inc1 = static_cast<uint32_t>((static_cast<uint64_t>(phaseIncrement) * cache.eff.ratio1FP) >> 16);
	uint32_t inc2 = static_cast<uint32_t>((static_cast<uint64_t>(phaseIncrement) * cache.eff.ratio2FP) >> 16);
	int32_t k1mPhase = static_cast<int32_t>((static_cast<uint64_t>(phaseIncrement) * cache.eff.k1mFP) >> 16);
	int32_t k2mPhase = static_cast<int32_t>((static_cast<uint64_t>(phaseIncrement) * cache.eff.k2mFP) >> 16);
	int32_t k12Phase = static_cast<int32_t>((static_cast<uint64_t>(phaseIncrement) * cache.eff.k12FP) >> 16);
	int32_t tempPhase = static_cast<int32_t>((static_cast<uint64_t>(phaseIncrement) * cache.effTempFP) >> 16);

	const q31_t w1 = cache.eff.w1;
	const q31_t w2 = cache.eff.w2;
	const q31_t wRing = cache.eff.wRing;
	const q31_t wBeat = cache.eff.wBeat;
	const q31_t beatGainBase = 0x7FFFFFFF - wBeat;

	uint32_t phase = *startPhase;
	uint32_t phaseAtEnd = phase + phaseIncrement * static_cast<uint32_t>(numSamples);
	uint32_t s1 = static_cast<uint32_t>(*slavePhases);
	uint32_t s2 = static_cast<uint32_t>(*slavePhases >> 32);
	uint32_t noiseState = cache.noiseState;

	// Match the triangle-oscillator amplitude convention (as the siblings do)
	amplitude <<= 1;
	amplitudeIncrement <<= 1;

	const uint32_t phaseWidth = pulseWidth ? (0xFFFFFFFF - (pulseWidth << 1)) : 0xFFFFFFFF;

	int32_t* thisSample = bufferStart;

	int32_t psinD1 = 0;
	int32_t pull1 = 0;
	int32_t pull2 = 0;
	int32_t chase = 0;

	for (int32_t n = 0; n < numSamples; n++) {
		phase += phaseIncrement;
		uint32_t thetaM = phase + retriggerPhase;
		if (applyAmplitude) {
			amplitude += amplitudeIncrement;
		}

		// Adler coupling: each slave is pulled toward (a rational relation
		// with) the master; slave 1 additionally drags slave 2. The coupling
		// forces are slow signals (beat rates are Hz-scale), so they're
		// recomputed every 4th sample and held - ~30% cheaper, inaudible
		if ((n & 3) == 0) {
			psinD1 = couplingTriangle(thetaM - s1);
			pull1 = multiply_32x32_rshift32(psinD1, k1mPhase) << 1;
			pull2 = multiply_32x32_rshift32(couplingTriangle(thetaM - s2), k2mPhase) << 1;
			chase = multiply_32x32_rshift32(couplingTriangle(s1 - s2), k12Phase) << 1;
		}

		// Temperature: Langevin phase noise, one LCG draw split across slaves
		noiseState = noiseState * 1664525u + 1013904223u;
		int32_t noise1 = multiply_32x32_rshift32(static_cast<int32_t>(noiseState), tempPhase) << 1;
		int32_t noise2 = multiply_32x32_rshift32(static_cast<int32_t>(noiseState << 13 | noiseState >> 19), //<
		                                         tempPhase)
		                 << 1;

		s1 += inc1 + pull1 + noise1;
		s2 += inc2 + pull2 + chase + noise2;

		if (thetaM > phaseWidth) {
			if (applyAmplitude) {
				thisSample++;
			}
			else {
				*thisSample++ = 0;
			}
			continue;
		}

		q31_t o1 = SineOsc::doFMNew(s1, 0);
		q31_t o2 = SineOsc::doFMNew(s2, 0);

		q31_t out = multiply_32x32_rshift32(o1, w1) << 1;
		out = add_saturate(out, multiply_32x32_rshift32(o2, w2) << 1);
		q31_t ring = multiply_32x32_rshift32(o1, o2) << 1;
		out = add_saturate(out, multiply_32x32_rshift32(ring, wRing) << 1);

		// Beat-AM: the coupling sine IS the beat waveform (flat when locked,
		// slow-asymmetric when pulling, cycling when free) - breathe the
		// output level with it so the phase drift is directly audible.
		// gain in [1 - 2*wBeat, 1]: never clips
		q31_t beatGain = beatGainBase + multiply_32x32_rshift32(psinD1, wBeat);
		out = multiply_32x32_rshift32(out, beatGain) << 1;

		if (applyAmplitude) {
			*thisSample = multiply_accumulate_32x32_rshift32_rounded(*thisSample, out, amplitude);
			thisSample++;
		}
		else {
			*thisSample++ = out;
		}
	}

	cache.noiseState = noiseState;
	*slavePhases = static_cast<uint64_t>(s1) | (static_cast<uint64_t>(s2) << 32);
	*startPhase = phaseAtEnd;

#if ENABLE_FX_BENCHMARK
	FX_BENCH_STOP(bench_render);
#endif
}

} // namespace deluge::dsp
