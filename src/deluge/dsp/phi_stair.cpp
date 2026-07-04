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

#include "dsp/phi_stair.hpp"
#include "io/debug/fx_benchmark.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp {

// ============================================================================
// Zone -> staircase builder (runs only on zone/gamma change)
// ============================================================================

namespace {

constexpr float kStairTwoPi = 6.283185307f;

float stairSpatial(double zonePhase, float stepFrac, float cycles, const phi::PhiTriConfig& cfg) {
	float wrapped = phi::wrapPhase(zonePhase * static_cast<double>(cfg.phiFreq) + static_cast<double>(stepFrac * cycles)
	                               + static_cast<double>(cfg.phaseOffset));
	if (cfg.bipolar) {
		return deluge::dsp::triangleFloat(wrapped, cfg.duty);
	}
	return deluge::dsp::triangleSimpleUnipolar(wrapped, cfg.duty);
}

// Height pattern families per zone: Brick, Terrace, Ramp, Mesa, Pylon,
// Glyph, Shard, Teeth
float stairPattern(int32_t family, float t, double phase) {
	switch (family) {
	case 0: // BRICK: alternation (square-class)
		return (static_cast<int32_t>(t * 16.0f) & 1) ? -0.8f : 0.8f;
	case 1: // TERRACE: monotonic climb (saw-stair)
		return t * 2.0f - 1.0f;
	case 2: // RAMP: gentle climb, meant for high slopes
		return t * 1.6f - 0.8f;
	case 3: // MESA: up, plateau, down
		return (t < 0.3f) ? (t / 0.3f) : ((t < 0.7f) ? 1.0f : (1.0f - (t - 0.7f) / 0.3f)) * 1.6f - 0.8f;
	case 4: // PYLON: tall center
		return (std::abs(t - 0.5f) < 0.2f) ? 0.9f : -0.45f;
	case 5: // GLYPH: phi-random quantized levels
		return std::round(stairSpatial(phase, t, 7.0f, {phi::kPhi300, 0.9f, 0.230f, true}) * 3.0f) * (1.0f / 3.0f);
	case 6: // SHARD: big asymmetric jumps
		return stairSpatial(phase, t, 3.0f, {phi::kPhi200, 0.5f, 0.510f, true});
	default: // TEETH: rapid pair alternation with lean
		return ((static_cast<int32_t>(t * 16.0f) & 2) ? 0.7f : -0.7f) + t * 0.5f - 0.25f;
	}
}

} // namespace

PhiStairParams buildPhiStairParams(uint16_t zone, float phaseOffset) {
	PhiStairParams p{};

	double phase = static_cast<double>(zone) / 1023.0 + static_cast<double>(phaseOffset);
	int32_t family = std::min(static_cast<int32_t>(7), static_cast<int32_t>(zone >> 7));

	// Continuous step count: the last step's width fades in fractionally
	float countF = 2.0f + phi::evalTriangle(phase, 1.0f, kPhiStairCount) * 14.0f;
	p.slope = phi::evalTriangle(phase, 1.0f, kPhiStairSlope);
	p.slope = p.slope * p.slope * 0.9f; // Squared: most of the map stays steppy
	if (family == 2) {
		p.slope = 0.3f + p.slope * 0.6f; // RAMP zone is meant to be smooth
	}

	float tilt = phi::evalTriangle(phase, 1.0f, kPhiStairTilt) * 0.6f;
	float asym = phi::evalTriangle(phase, 1.0f, kPhiStairAsym);
	float cycles = 1.0f + phi::evalTriangle(phase, 1.0f, kPhiStairLandCycles) * 3.0f;

	float peak = 0.0001f;
	for (int32_t i = 0; i < kPhiStairSteps; i++) {
		float t = (static_cast<float>(i) + 0.5f) / countF;
		float h = stairPattern(family, std::min(t, 1.0f), phase)
		          + 0.35f * stairSpatial(phase, t, cycles, kPhiStairHeightLand) + tilt * (t * 2.0f - 1.0f);
		p.heights[i] = h;
		peak = std::max(peak, std::abs(h));

		// Width: even split warped by the asymmetry landscape, with the
		// fractional-count fade keeping the step count CONTINUOUS
		float wLand = 1.0f + asym * 0.85f * stairSpatial(phase, t, cycles * 2.0f, kPhiStairAsymLand);
		float countFade = std::clamp(countF - static_cast<float>(i), 0.0f, 1.0f);
		p.widths[i] = std::max(wLand, 0.1f) * countFade;
	}
	float norm = 1.0f / peak;
	for (float& h : p.heights) {
		h *= norm;
	}

	return p;
}

// ============================================================================
// Effective table build (crossfaded banks -> 128-slot staircase + mips)
// ============================================================================

namespace {

void buildPhiStairTables(PhiStairCache& cache, float cf) {
	float cfInv = 1.0f - cf;

	float h[kPhiStairSteps];
	float w[kPhiStairSteps];
	float wSum = 0.0f;
	for (int32_t i = 0; i < kPhiStairSteps; i++) {
		h[i] = cfInv * cache.bankA.heights[i] + cf * cache.bankB.heights[i];
		w[i] = cfInv * cache.bankA.widths[i] + cf * cache.bankB.widths[i];
		wSum += w[i];
	}
	float slope = cfInv * cache.bankA.slope + cf * cache.bankB.slope;
	float wNorm = 1.0f / wSum;

	// Walk the slots through the variable-width steps (GENDY's resample walk)
	memcpy(cache.nodeQPrev, cache.nodeQ, sizeof(cache.nodeQPrev));
	memcpy(cache.nodeQMip1Prev, cache.nodeQMip1, sizeof(cache.nodeQMip1Prev));
	memcpy(cache.nodeQMip2Prev, cache.nodeQMip2, sizeof(cache.nodeQMip2Prev));

	// Last ACTIVE step's height wraps to become step 0's riser origin
	int32_t lastActive = 0;
	for (int32_t i = 0; i < kPhiStairSteps; i++) {
		if (w[i] > 0.0f) {
			lastActive = i;
		}
	}

	float slots[kPhiStairSlots];
	float mean = 0.0f;
	int32_t seg = 0;
	float segStart = 0.0f;
	float segWidth = w[0] * wNorm;
	for (int32_t j = 0; j < kPhiStairSlots; j++) {
		float u = static_cast<float>(j) * (1.0f / static_cast<float>(kPhiStairSlots));
		while (u >= segStart + segWidth && seg < kPhiStairSteps - 1) {
			segStart += segWidth;
			seg++;
			segWidth = w[seg] * wNorm;
		}
		float hPrev = (seg == 0) ? h[lastActive] : h[seg - 1];
		float val = h[seg];
		if (segWidth > 0.0f && slope > 0.001f) {
			float frac = (u - segStart) / segWidth;
			if (frac < slope) { // Riser: ramp from the previous step's level
				val = hPrev + (h[seg] - hPrev) * (frac / slope);
			}
		}
		slots[j] = val;
		mean += val;
	}
	mean *= 1.0f / static_cast<float>(kPhiStairSlots);

	// Static loudness normalization (the family's square-calibrated rule,
	// computed once per rebuild since the waveform is fixed): target saw-
	// class RMS (0.55 of the ceiling) with a hard peak cap at square parity.
	// Peak-normalized heights alone left sparse patterns (Glyph's quantized
	// levels, Pylon's lone tall step) much quieter than dense ones, and
	// mean-subtraction shrank DC-heavy patterns further.
	float peak = 0.0001f;
	float sumSq = 0.0f;
	for (int32_t j = 0; j < kPhiStairSlots; j++) {
		float d = slots[j] - mean;
		peak = std::max(peak, std::abs(d));
		sumSq += d * d;
	}
	float rms = std::sqrt(sumSq * (1.0f / static_cast<float>(kPhiStairSlots)));
	float scale = std::min(0.55f / std::max(rms, 0.05f), 0.95f / peak) * 1073741823.0f;

	for (int32_t j = 0; j < kPhiStairSlots; j++) {
		cache.nodeQ[j] = static_cast<q31_t>(std::clamp((slots[j] - mean) * scale, -2147483000.0f, 2147483000.0f));
	}
	cache.nodeQ[kPhiStairSlots] = cache.nodeQ[0];

	// Anti-alias mips (WEAVE pattern: binomial passes over the slots)
	auto binomial = [](const q31_t* src, q31_t* dst) {
		for (int32_t j = 0; j < kPhiStairSlots; j++) {
			int32_t prev = (j == 0) ? kPhiStairSlots - 1 : j - 1;
			dst[j] = static_cast<q31_t>(
			    (static_cast<int64_t>(src[prev]) + 2 * static_cast<int64_t>(src[j]) + static_cast<int64_t>(src[j + 1]))
			    >> 2);
		}
		dst[kPhiStairSlots] = dst[0];
	};
	binomial(cache.nodeQ, cache.nodeQMip1);
	binomial(cache.nodeQMip1, cache.nodeQMip2);

	if (!cache.tablesValid) {
		memcpy(cache.nodeQPrev, cache.nodeQ, sizeof(cache.nodeQPrev));
		memcpy(cache.nodeQMip1Prev, cache.nodeQMip1, sizeof(cache.nodeQMip1Prev));
		memcpy(cache.nodeQMip2Prev, cache.nodeQMip2, sizeof(cache.nodeQMip2Prev));
		cache.tablesValid = true;
	}
}

struct StairStereoChar {
	float maxOffset;
	bool tilt;
	bool counter;
};
constexpr StairStereoChar kStairStereoZones[8] = {
    {0.03f, false, false}, {0.0625f, true, false}, {0.125f, false, false}, {0.125f, true, false},
    {0.25f, false, false}, {0.25f, false, true},   {0.5f, true, false},    {0.5f, true, true},
};

} // namespace

// ============================================================================
// Main render: scan the staircase (rebuilds crossfade over one buffer)
// ============================================================================

void renderPhiStair(PhiStairCache& cache, int32_t* bufferStart, int32_t* bufferEnd, int32_t numSamples,
                    uint32_t phaseIncrement, uint32_t* startPhase, uint32_t retriggerPhase, int32_t amplitude,
                    int32_t amplitudeIncrement, bool applyAmplitude, q31_t crossfade, uint32_t pulseWidth,
                    int32_t* bufferRStart, uint16_t stereoZone) {

#if ENABLE_FX_BENCHMARK
	FX_BENCH_DECLARE(bench_render, "phi_stair", "render");
	FX_BENCH_START(bench_render);
#endif

	// Rebuild the effective staircase only when the morph moves (epsilon cuts
	// the IIR tail); each rebuild crossfades prev -> current over one buffer
	if (AudioEngine::audioSampleTimer != cache.lastTickTime) {
		cache.lastTickTime = AudioEngine::audioSampleTimer;
		float cf = std::clamp(static_cast<float>(crossfade) / 2147483648.0f + 0.5f, 0.0f, 1.0f);
		if (std::abs(cf - cache.effCfCached) > 0.0005f || !cache.tablesValid) {
			buildPhiStairTables(cache, cf);
			cache.effCfCached = cf;
			cache.fadeUntilTime = AudioEngine::audioSampleTimer;
		}
	}
	const bool fading = (cache.fadeUntilTime == AudioEngine::audioSampleTimer);

	// Pitch-adaptive mip select (thresholds shared with WEAVE: ~700/1800 Hz)
	const q31_t* nodes = cache.nodeQ;
	const q31_t* nodesPrev = cache.nodeQPrev;
	const q31_t* nodesMip = cache.nodeQMip1;
	const q31_t* nodesMipPrev = cache.nodeQMip1Prev;
	if (phaseIncrement > 175304787u) {
		nodes = cache.nodeQMip2;
		nodesPrev = cache.nodeQMip2Prev;
		nodesMip = cache.nodeQMip2;
		nodesMipPrev = cache.nodeQMip2Prev;
	}
	else if (phaseIncrement > 68174083u) {
		nodes = cache.nodeQMip1;
		nodesPrev = cache.nodeQMip1Prev;
		nodesMip = cache.nodeQMip2;
		nodesMipPrev = cache.nodeQMip2Prev;
	}

	// Stereo-zone dual tap (WEAVE pattern)
	const StairStereoChar& sc = kStairStereoZones[(stereoZone >> 7) & 7];
	const uint32_t tapOffset =
	    static_cast<uint32_t>(static_cast<float>(stereoZone & 127u) * (1.0f / 127.0f) * sc.maxOffset * 4294967296.0);
	const q31_t* nodesR = sc.tilt ? nodesMip : nodes;
	const q31_t* nodesPrevR = sc.tilt ? nodesMipPrev : nodesPrev;

	// One-buffer rebuild crossfade, forward-differenced smoothstep
	float hf = 1.0f / static_cast<float>(numSamples);
	auto ss = [](float t) { return t * t * (3.0f - 2.0f * t); };
	float y1 = ss(hf);
	float y2 = ss(2.0f * hf);
	float y3 = ss(3.0f * hf);
	q31_t fade = fading ? 0 : 0x7FFFFFFF;
	q31_t fd1 = fading ? static_cast<q31_t>(y1 * 2147483647.0f) : 0;
	q31_t fd2 = fading ? static_cast<q31_t>((y2 - 2.0f * y1) * 2147483647.0f) : 0;
	const q31_t fd3 = fading ? static_cast<q31_t>((y3 - 3.0f * y2 + 3.0f * y1) * 2147483647.0f) : 0;

	uint32_t phase = *startPhase;
	uint32_t phaseAtEnd = phase + phaseIncrement * static_cast<uint32_t>(numSamples);

	// Match the triangle-oscillator amplitude convention (as the siblings do)
	amplitude <<= 1;
	amplitudeIncrement <<= 1;

	const uint32_t phaseWidth = pulseWidth ? (0xFFFFFFFF - (pulseWidth << 1)) : 0xFFFFFFFF;

	// Halved-difference scan (steps sign-flip at full scale by design)
	auto scan = [](const q31_t* cur, const q31_t* prev, uint32_t ph, q31_t fadeNow) -> q31_t {
		uint32_t idx = ph >> kPhiStairSlotShift;
		q31_t frac31 = static_cast<q31_t>((ph & 0x01FFFFFF) << 6);
		q31_t pA = prev[idx];
		q31_t a = pA + (multiply_32x32_rshift32((cur[idx] >> 1) - (pA >> 1), fadeNow) << 2);
		q31_t pB = prev[idx + 1];
		q31_t b = pB + (multiply_32x32_rshift32((cur[idx + 1] >> 1) - (pB >> 1), fadeNow) << 2);
		return a + (multiply_32x32_rshift32((b >> 1) - (a >> 1), frac31) << 2);
	};

	int32_t* thisSample = bufferStart;
	int32_t* thisSampleR = bufferRStart;

	for (int32_t n = 0; n < numSamples; n++) {
		phase += phaseIncrement;
		fade += fd1;
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

		q31_t out = scan(nodes, nodesPrev, evalPhase, fade);

		if (applyAmplitude) {
			*thisSample = multiply_accumulate_32x32_rshift32_rounded(*thisSample, out, amplitude);
			thisSample++;
		}
		else {
			*thisSample++ = out;
		}

		if (thisSampleR != nullptr) {
			uint32_t evalR = sc.counter ? (tapOffset - evalPhase) : (evalPhase + tapOffset);
			q31_t outR = scan(nodesR, nodesPrevR, evalR, fade);
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
