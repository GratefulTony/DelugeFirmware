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
#pragma once

#include "dsp/oscillators/sine_osc.h"
#include "dsp/stereo_sample.h"
#include "io/debug/fx_benchmark.h"
#include "storage/field_serialization.h"
#include "util/fixedpoint.h"
#include "util/lookuptables/lookuptables.h"
#include "util/waves.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

class Serializer;
class Deserializer;

namespace deluge::dsp {

// ============================================================================
// Constants
// ============================================================================

inline constexpr float kHarmSampleRate = 44100.0f;
inline constexpr float kHarmTwoPi = 6.283185307f;

// Envelope thresholds
inline constexpr float kHarmEnvThreshold = 1.0e-7f; // ~-140dB, effectively just denormal prevention

// Portamento rate mapping: knob 0 = instant, 127 = very slow glide
// Rate = 1.0 - exp(-1/(time_samples)), approximated as simple division
inline constexpr float kHarmPortaMinRate = 0.00005f; // slowest glide (~0.5s)
inline constexpr float kHarmPortaMaxRate = 1.0f;     // instant snap

// ============================================================================
// HarmParams
// ============================================================================

struct HarmParams {
	// Direct params (serialized)
	static constexpr uint8_t kHarmDefault = 10; // 1:1 fundamental
	uint8_t harmonic{kHarmDefault};             // 0=off, 1-102=ratio from kHarmonicTable, default=10 (1:1)
	uint8_t level{0};                           // 0=silence(default), 64=mid, 127=full
	uint8_t fine{64};                           // 0=-12st, 64=0st(default), 127=+12st
	uint8_t phase{0};    // 0-64: start phase (0-360 deg, mono). 65-127: stereo spread (0-180 deg)
	uint8_t attack{0};   // AR envelope attack (0=instant)
	uint8_t release{64}; // AR envelope release
	uint8_t hpf{0};      // Note-tracking HPF cutoff (0=off/bypass)
	uint8_t porta{0};    // Portamento time (0=instant)

	// DSP state (NOT serialized)
	uint32_t phaseAccumL{0};
	uint32_t phaseAccumR{0};
	int32_t phaseCatchupL{0}; // Phase error to correct (signed, decreases exponentially)
	int32_t phaseCatchupR{0};
	float envelope{0.0f};         // AR envelope, 0.0-1.0
	float currentFreq{0.0f};      // Portamento-smoothed frequency (as phase increment)
	float targetFreq{0.0f};       // Target frequency
	bool voicesWereActive{false}; // For trigger detection
	int32_t gateCount{0};         // Note gate: >0 = held, incremented on noteOn, decremented on noteOff

	void harmNoteOn() { gateCount++; }
	void harmNoteOff() {
		if (gateCount > 0) {
			gateCount--;
		}
	}
	void harmAllNotesOff() { gateCount = 0; }
	[[nodiscard]] bool isGateOpen() const { return gateCount > 0; }
	// Notch biquad state (direct form II transposed), per channel, Q31
	q31_t notchZ1L{0};
	q31_t notchZ2L{0};
	q31_t notchZ1R{0};
	q31_t notchZ2R{0};

	// Cached notch coefficients — recomputed only when inputs change
	q31_t cachedQb0{0};
	q31_t cachedQb1{0};
	q31_t cachedQa2{0};
	int32_t cachedNotchNote{-9999};
	uint8_t cachedNotchHpf{0};
	q31_t cachedNotchFine{0};
	float cachedNotchBend{0.0f};

	// Cached osc pitch/pink — recomputed only when inputs change
	float cachedTargetFreq{0.0f};
	q31_t cachedPinkQ31{ONE_Q31};
	int32_t cachedOscNote{-9999};
	uint8_t cachedOscHarmonic{0};
	q31_t cachedOscFine{0};
	float cachedOscBend{0.0f};

	// Harmonic ratio table: sorted unique fractions with denominators {1,2,3,4,8}
	// Index 0 is unused (0=OFF), indices 1-102 map to ratios
	struct HarmonicEntry {
		uint8_t num;
		uint8_t den;
		float ratio;
	};
	// clang-format off
	static constexpr HarmonicEntry kHarmonicTable[] = {
		{1,8, 0.125f},     {1,4, 0.25f},      {1,3, 0.33333333f}, {3,8, 0.375f},
		{1,2, 0.5f},       {5,8, 0.625f},      {2,3, 0.66666667f}, {3,4, 0.75f},
		{7,8, 0.875f},     {1,1, 1.0f},        {9,8, 1.125f},      {5,4, 1.25f},
		{4,3, 1.33333333f},{11,8, 1.375f},     {3,2, 1.5f},        {13,8, 1.625f},
		{5,3, 1.66666667f},{7,4, 1.75f},       {15,8, 1.875f},     {2,1, 2.0f},
		{17,8, 2.125f},    {9,4, 2.25f},       {7,3, 2.33333333f}, {19,8, 2.375f},
		{5,2, 2.5f},       {21,8, 2.625f},     {8,3, 2.66666667f}, {11,4, 2.75f},
		{23,8, 2.875f},    {3,1, 3.0f},        {25,8, 3.125f},     {13,4, 3.25f},
		{10,3, 3.33333333f},{27,8, 3.375f},    {7,2, 3.5f},        {29,8, 3.625f},
		{11,3, 3.66666667f},{15,4, 3.75f},     {31,8, 3.875f},     {4,1, 4.0f},
		{17,4, 4.25f},     {13,3, 4.33333333f},{9,2, 4.5f},        {14,3, 4.66666667f},
		{19,4, 4.75f},     {5,1, 5.0f},        {21,4, 5.25f},      {16,3, 5.33333333f},
		{11,2, 5.5f},      {17,3, 5.66666667f},{23,4, 5.75f},      {6,1, 6.0f},
		{25,4, 6.25f},     {19,3, 6.33333333f},{13,2, 6.5f},       {20,3, 6.66666667f},
		{27,4, 6.75f},     {7,1, 7.0f},        {29,4, 7.25f},      {22,3, 7.33333333f},
		{15,2, 7.5f},      {23,3, 7.66666667f},{31,4, 7.75f},      {8,1, 8.0f},
		{25,3, 8.33333333f},{17,2, 8.5f},      {26,3, 8.66666667f},{9,1, 9.0f},
		{28,3, 9.33333333f},{19,2, 9.5f},      {29,3, 9.66666667f},{10,1, 10.0f},
		{31,3, 10.33333333f},{21,2, 10.5f},    {32,3, 10.66666667f},{11,1, 11.0f},
		{23,2, 11.5f},     {12,1, 12.0f},      {25,2, 12.5f},      {13,1, 13.0f},
		{27,2, 13.5f},     {14,1, 14.0f},      {29,2, 14.5f},      {15,1, 15.0f},
		{31,2, 15.5f},     {16,1, 16.0f},      {17,1, 17.0f},      {18,1, 18.0f},
		{19,1, 19.0f},     {20,1, 20.0f},      {21,1, 21.0f},      {22,1, 22.0f},
		{23,1, 23.0f},     {24,1, 24.0f},      {25,1, 25.0f},      {26,1, 26.0f},
		{27,1, 27.0f},     {28,1, 28.0f},      {29,1, 29.0f},      {30,1, 30.0f},
		{31,1, 31.0f},     {32,1, 32.0f},
	};
	// clang-format on
	static constexpr int32_t kNumHarmonics = 102;

	[[nodiscard]] bool isOscEnabled() const { return harmonic > 0; }
	[[nodiscard]] bool isHpfEnabled() const { return hpf > 0; }
	[[nodiscard]] bool isEnabled() const { return isOscEnabled() || isHpfEnabled(); }

	// ========================================================================
	// noteCode -> phaseIncrement conversion
	// ========================================================================
	// noteFrequencyTable begins at E (4 semitones above C).
	// Pattern from voice.cpp: noteWithinOctave = (noteCode + 120 - 4) % 12
	//                         octave = (noteCode + 120 - 4) / 12
	//                         shiftRight = 20 - octave
	//                         phaseIncrement = noteFrequencyTable[noteWithinOctave] >> shiftRight
	// The +120 bias ensures the modulo works correctly for negative noteCode values.

	static uint32_t noteCodeToPhaseIncrement(int32_t noteCode) {
		int32_t adjusted = noteCode + 120 - 4;
		int32_t noteWithinOctave = adjusted % 12;
		int32_t octave = adjusted / 12;
		int32_t shiftRightAmount = 20 - octave;

		if (shiftRightAmount >= 0 && shiftRightAmount < 32) {
			return static_cast<uint32_t>(noteFrequencyTable[noteWithinOctave]) >> shiftRightAmount;
		}
		else if (shiftRightAmount < 0 && shiftRightAmount > -32) {
			return static_cast<uint32_t>(noteFrequencyTable[noteWithinOctave]) << (-shiftRightAmount);
		}
		return 0; // Out of range
	}

	// ========================================================================
	// renderNotch — note-tracking notch filter (PRE-reverb)
	// ========================================================================
	// Biquad notch filter tuned to the note frequency.
	// hpf knob 1..127 controls notch width: 1=narrow (surgical), 127=wide.
	// When hpf==0, caller should skip this entirely.
	// Coefficients computed in float per-buffer, inner loop in Q31.

	void renderHpf(std::span<StereoSample> buffer, int32_t noteCode, float bendSemitones, q31_t fineModulation) {
		if (!isHpfEnabled()) {
			return;
		}
		FX_BENCH_DECLARE(bench, "harm", "notch");
		FX_BENCH_START(bench);

		// Only recompute coefficients when inputs change
		bool needRecompute = (noteCode != cachedNotchNote || hpf != cachedNotchHpf || fineModulation != cachedNotchFine
		                      || bendSemitones != cachedNotchBend);

		q31_t qb0, qb1, qa2;
		if (needRecompute) {
			// Compute notch center frequency — track the oscillator's actual pitch
			uint32_t basePhaseInc = noteCodeToPhaseIncrement(noteCode);
			float ratio = isOscEnabled()
			                  ? kHarmonicTable[std::min(static_cast<int32_t>(harmonic) - 1, kNumHarmonics - 1)].ratio
			                  : 1.0f;
			float fineSemitones =
			    (static_cast<float>(fineModulation) / static_cast<float>(ONE_Q31)) * 12.0f + bendSemitones;
			float fineMul = std::exp2f(fineSemitones / 12.0f);
			float centerFreq = static_cast<float>(basePhaseInc) * ratio * fineMul;
			float w0 = centerFreq * (6.2831853f / 4294967296.0f);

			float Q = 30.0f - (static_cast<float>(hpf - 1) / 126.0f) * 29.5f;
			float sinw0 = std::sinf(w0);
			float cosw0 = std::cosf(w0);
			float alpha = sinw0 / (2.0f * Q);
			float inv_a0 = 1.0f / (1.0f + alpha);

			constexpr float kQ30Scale = 1073741824.0f;
			cachedQb0 = static_cast<q31_t>(inv_a0 * kQ30Scale);
			cachedQb1 = static_cast<q31_t>(-2.0f * cosw0 * inv_a0 * kQ30Scale);
			cachedQa2 = static_cast<q31_t>((1.0f - alpha) * inv_a0 * kQ30Scale);
			cachedNotchNote = noteCode;
			cachedNotchHpf = hpf;
			cachedNotchFine = fineModulation;
			cachedNotchBend = bendSemitones;
		}
		qb0 = cachedQb0;
		qb1 = cachedQb1;
		q31_t qa1 = qb1; // same for notch
		qa2 = cachedQa2;

		for (auto& sample : buffer) {
			// Left channel — Direct Form II Transposed in Q31
			// out = b0*in + z1  (all Q1.30 multiplies, <<2 to recover Q31)
			q31_t outL = (multiply_32x32_rshift32(qb0, sample.l) << 2) + notchZ1L;
			notchZ1L =
			    (multiply_32x32_rshift32(qb1, sample.l) << 2) - (multiply_32x32_rshift32(qa1, outL) << 2) + notchZ2L;
			notchZ2L = (multiply_32x32_rshift32(qb0, sample.l) << 2) - (multiply_32x32_rshift32(qa2, outL) << 2);
			sample.l = outL;

			// Right channel
			q31_t outR = (multiply_32x32_rshift32(qb0, sample.r) << 2) + notchZ1R;
			notchZ1R =
			    (multiply_32x32_rshift32(qb1, sample.r) << 2) - (multiply_32x32_rshift32(qa1, outR) << 2) + notchZ2R;
			notchZ2R = (multiply_32x32_rshift32(qb0, sample.r) << 2) - (multiply_32x32_rshift32(qa2, outR) << 2);
			sample.r = outR;
		}
		FX_BENCH_STOP(bench);
	}

	// ========================================================================
	// renderOsc — harmonic oscillator with AR envelope (POST-reverb)
	// ========================================================================
	// Generates a sine at a harmonic of the note frequency, with AR envelope,
	// portamento, and phase/spread control. Mixes additively into buffer.

	void renderOsc(std::span<StereoSample> buffer, int32_t noteCode, bool voicesActive, float bendSemitones,
	               q31_t levelModulation, q31_t fineModulation) {
		if (!isOscEnabled()) {
			return;
		}
		FX_BENCH_DECLARE(bench, "harm", "osc");
		FX_BENCH_START(bench);

		// --- Pitch calculation (cached when inputs unchanged) ---
		bool pitchChanged = (noteCode != cachedOscNote || harmonic != cachedOscHarmonic
		                     || fineModulation != cachedOscFine || bendSemitones != cachedOscBend);
		if (pitchChanged) {
			uint32_t basePhaseInc = noteCodeToPhaseIncrement(noteCode);
			int32_t tableIdx = std::min(static_cast<int32_t>(harmonic) - 1, kNumHarmonics - 1);
			float ratio = kHarmonicTable[tableIdx].ratio;
			float fineSemitones =
			    (static_cast<float>(fineModulation) / static_cast<float>(ONE_Q31)) * 12.0f + bendSemitones;
			float fineMul = std::exp2f(fineSemitones / 12.0f);
			cachedTargetFreq = static_cast<float>(basePhaseInc) * ratio * fineMul;

			cachedOscNote = noteCode;
			cachedOscHarmonic = harmonic;
			cachedOscFine = fineModulation;
			cachedOscBend = bendSemitones;
		}
		targetFreq = cachedTargetFreq;

		// --- Trigger detection (moved before portamento so we can snap on retrigger) ---
		bool triggered = voicesActive && !voicesWereActive;

		// --- Portamento ---
		// Snap immediately if: no porta, first use, or retrigger after full note-off
		if (porta == 0 || currentFreq == 0.0f || triggered) {
			currentFreq = targetFreq;
		}
		else if (currentFreq != targetFreq) {
			// Map porta knob (1..127) to smoothing rate per sample
			float portaRate =
			    kHarmPortaMaxRate - (static_cast<float>(porta) / 127.0f) * (kHarmPortaMaxRate - kHarmPortaMinRate);
			// Compute final value after N samples: freq = target + (current - target) * (1 - rate)^N
			float retention = 1.0f - portaRate;
			float retentionN = std::powf(retention, static_cast<float>(buffer.size()));
			currentFreq = targetFreq + (currentFreq - targetFreq) * retentionN;
		}

		uint32_t phaseIncrement = static_cast<uint32_t>(std::max(currentFreq, 0.0f));

		// Pink noise scaling: -3dB/octave, 40Hz = unity, never boosts
		// Only recompute when frequency changed (sqrtf is ~30 cycles)
		if (pitchChanged || currentFreq != targetFreq) {
			float outputHz = currentFreq * (kHarmSampleRate / 4294967296.0f);
			float pinkScale = (outputHz > 40.0f) ? std::sqrtf(40.0f / outputHz) : 1.0f;
			cachedPinkQ31 = static_cast<q31_t>(pinkScale * static_cast<float>(ONE_Q31));
		}

		// --- Phase catchup on trigger ---
		// Instead of snapping phase (which clicks), compute the error between
		// current phase and desired phase, then gradually correct over ~24 cycles.
		if (triggered) {
			uint32_t targetPhaseL, targetPhaseR;
			if (phase <= 64) {
				// Mono start phase: 0..64 -> 0..2pi
				uint32_t startPhase = (static_cast<uint64_t>(phase) * 0xFFFFFFFFu) / 64u;
				targetPhaseL = startPhase;
				targetPhaseR = startPhase;
			}
			else {
				// Stereo spread: 65..127 -> 0..180 degrees spread
				uint32_t spread = (static_cast<uint64_t>(phase - 65) * 0x80000000u) / 62u;
				targetPhaseL = spread / 2;
				targetPhaseR = static_cast<uint32_t>(0u - spread / 2);
			}
			// Compute signed phase error (what we need to add to reach target)
			phaseCatchupL = static_cast<int32_t>(targetPhaseL - phaseAccumL);
			phaseCatchupR = static_cast<int32_t>(targetPhaseR - phaseAccumR);
		}

		bool isStereo = (phase > 64);

		// --- AR envelope rates ---
		// Attack: knob 0 = instant, 127 = ~1s
		// Rate per sample: instant = jump to 1.0, else ramp
		float attackRate;
		if (attack == 0) {
			attackRate = 1.0f; // instant
		}
		else {
			// Exponential mapping: higher knob = slower attack
			// ~0.5ms (attack=1) to ~1s (attack=127)
			float attackTimeSamples = 1.0f + static_cast<float>(attack) * (kHarmSampleRate / 127.0f);
			attackRate = 1.0f / attackTimeSamples;
		}

		// Release: knob 0 = instant, 127 = ~2s
		float releaseRate;
		if (release == 0) {
			releaseRate = 1.0f; // instant
		}
		else {
			float releaseTimeSamples = 1.0f + static_cast<float>(release) * (2.0f * kHarmSampleRate / 127.0f);
			releaseRate = 1.0f / releaseTimeSamples;
		}

		// --- Pre-compute loop-invariant values ---
		q31_t levelGain = std::max(static_cast<int32_t>(0), levelModulation + (ONE_Q31 >> 2));

		// Helper to compute combined gain from current envelope
		auto computeCombinedGain = [&]() -> q31_t {
			q31_t envQ31 = static_cast<q31_t>(envelope * static_cast<float>(ONE_Q31));
			q31_t envLevel = multiply_32x32_rshift32(envQ31, levelGain) << 2;
			return multiply_32x32_rshift32(cachedPinkQ31, envLevel);
		};

		// Helper to advance envelope by 1 sample
		auto advanceEnvelope = [&]() {
			if (voicesActive) {
				envelope += (1.0f - envelope) * attackRate;
				if (envelope > 1.0f) {
					envelope = 1.0f;
				}
			}
			else if (envelope > 0.0f) {
				envelope -= envelope * releaseRate;
				if (envelope < kHarmEnvThreshold) {
					envelope = 0.0f;
				}
			}
		};

		// Skip output entirely if silent
		if (levelGain == 0 && !voicesActive) {
			// No output possible and not attacking — just advance phase
			uint32_t totalPhaseAdvance = phaseIncrement * static_cast<uint32_t>(buffer.size());
			phaseAccumL += totalPhaseAdvance;
			phaseAccumR = isStereo ? (phaseAccumR + totalPhaseAdvance) : phaseAccumL;
			phaseCatchupL = 0;
			phaseCatchupR = 0;
			voicesWereActive = voicesActive;
			FX_BENCH_STOP(bench);
			return;
		}

		// --- Decide path: NEON (stable sustain) or scalar (envelope changing) ---
		bool envelopeStable = (voicesActive && envelope >= 0.999f) || (!voicesActive && envelope == 0.0f);
		bool noCatchup = (phaseCatchupL == 0 && phaseCatchupR == 0);

		if (envelopeStable && noCatchup && !isStereo && envelope > 0.0f) {
			// NEON fast path: constant gain, no catchup, mono
			q31_t combinedGain = computeCombinedGain();
			size_t chunks = buffer.size() / 4;
			for (size_t c = 0; c < chunks; c++) {
				Argon<int32_t> sineVec = SineOsc::getSineVector(&phaseAccumL, phaseIncrement);
				Argon<int32_t> scaled = sineVec.MultiplyFixedPoint(combinedGain) >> 6;
				size_t base = c * 4;
				for (int i = 0; i < 4; i++) {
					buffer[base + i].l = add_saturate(buffer[base + i].l, scaled[i]);
					buffer[base + i].r = add_saturate(buffer[base + i].r, scaled[i]);
				}
			}
			// Remaining scalar
			for (size_t s = chunks * 4; s < buffer.size(); s++) {
				phaseAccumL += phaseIncrement;
				int32_t sine = multiply_32x32_rshift32(getSine(phaseAccumL), combinedGain) >> 5;
				buffer[s].l = add_saturate(buffer[s].l, sine);
				buffer[s].r = add_saturate(buffer[s].r, sine);
			}
			phaseAccumR = phaseAccumL;
		}
		else {
			// Scalar path: per-sample envelope, catchup, stereo support
			for (auto& sample : buffer) {
				advanceEnvelope();

				// Phase catchup
				if (phaseCatchupL != 0) {
					int32_t corrL = multiply_32x32_rshift32(phaseCatchupL, static_cast<int32_t>(phaseIncrement)) >> 4;
					if (corrL == 0) {
						corrL = (phaseCatchupL > 0) ? 1 : -1;
					}
					phaseAccumL += static_cast<uint32_t>(corrL);
					phaseCatchupL -= corrL;
				}
				phaseAccumL += phaseIncrement;

				if (isStereo) {
					if (phaseCatchupR != 0) {
						int32_t corrR =
						    multiply_32x32_rshift32(phaseCatchupR, static_cast<int32_t>(phaseIncrement)) >> 4;
						if (corrR == 0) {
							corrR = (phaseCatchupR > 0) ? 1 : -1;
						}
						phaseAccumR += static_cast<uint32_t>(corrR);
						phaseCatchupR -= corrR;
					}
					phaseAccumR += phaseIncrement;
				}
				else {
					phaseAccumR = phaseAccumL;
					phaseCatchupR = phaseCatchupL;
				}

				if (envelope == 0.0f) {
					continue;
				}

				q31_t combinedGain = computeCombinedGain();
				int32_t sineL = multiply_32x32_rshift32(getSine(phaseAccumL), combinedGain) >> 5;
				int32_t sineR = isStereo ? (multiply_32x32_rshift32(getSine(phaseAccumR), combinedGain) >> 5) : sineL;

				sample.l = add_saturate(sample.l, sineL);
				sample.r = add_saturate(sample.r, sineR);
			}
		}

		// Update trigger state
		voicesWereActive = voicesActive;
		FX_BENCH_STOP(bench);
	}

	// ========================================================================
	// Serialization
	// ========================================================================

	void writeToFile(Serializer& writer) const {
		WRITE_FIELD_DEFAULT(writer, harmonic, "harmHarmonic", kHarmDefault);
		WRITE_FIELD(writer, level, "harmLevel");
		WRITE_FIELD_DEFAULT(writer, fine, "harmFine", 64);
		WRITE_FIELD(writer, phase, "harmPhase");
		WRITE_FIELD(writer, attack, "harmAttack");
		WRITE_FIELD_DEFAULT(writer, release, "harmRelease", 64);
		WRITE_FIELD(writer, hpf, "harmHpf");
		WRITE_FIELD(writer, porta, "harmPorta");
	}

	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, harmonic, "harmHarmonic");
		READ_FIELD(reader, tagName, level, "harmLevel");
		READ_FIELD(reader, tagName, fine, "harmFine");
		READ_FIELD(reader, tagName, phase, "harmPhase");
		READ_FIELD(reader, tagName, attack, "harmAttack");
		READ_FIELD(reader, tagName, release, "harmRelease");
		READ_FIELD(reader, tagName, hpf, "harmHpf");
		READ_FIELD(reader, tagName, porta, "harmPorta");
		return false;
	}
};

} // namespace deluge::dsp
