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

#include "dsp/stereo_sample.h"
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
inline constexpr float kHarmEnvThreshold = 1.0e-6f;

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
	uint8_t phase{0};    // 0-64: start phase (0-360 deg, mono). 65-127: stereo spread (0-180 deg)
	uint8_t attack{0};   // AR envelope attack (0=instant)
	uint8_t release{64}; // AR envelope release
	uint8_t hpf{0};      // Note-tracking HPF cutoff (0=off/bypass)
	uint8_t porta{0};    // Portamento time (0=instant)

	// DSP state (NOT serialized)
	uint32_t phaseAccumL{0};
	uint32_t phaseAccumR{0};
	float envelope{0.0f};         // AR envelope, 0.0-1.0
	float currentFreq{0.0f};      // Portamento-smoothed frequency (as phase increment)
	float targetFreq{0.0f};       // Target frequency
	bool voicesWereActive{false}; // For trigger detection
	// Notch biquad state (direct form II transposed), per channel
	float notchZ1L{0.0f}; // z^-1 delay left
	float notchZ2L{0.0f}; // z^-2 delay left
	float notchZ1R{0.0f}; // z^-1 delay right
	float notchZ2R{0.0f}; // z^-2 delay right

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
	// Uses Direct Form II Transposed for numerical stability with float.

	void renderHpf(std::span<StereoSample> buffer, int32_t noteCode) {
		if (!isHpfEnabled()) {
			return;
		}

		// Compute notch center frequency
		uint32_t centerPhaseInc = noteCodeToPhaseIncrement(noteCode);
		float w0 = static_cast<float>(centerPhaseInc) * (6.2831853f / 4294967296.0f); // 2*pi*fc/fs

		// Q from hpf knob: 1=wide (Q=0.5), 127=narrow (Q=30)
		// Invert: low knob = narrow surgical, high knob = wide
		// Actually: 1=narrow, 127=wide is more intuitive (turn up = more effect)
		float Q = 30.0f - (static_cast<float>(hpf - 1) / 126.0f) * 29.5f; // 30 down to 0.5

		// Biquad notch coefficients (cookbook: Audio EQ Cookbook by Robert Bristow-Johnson)
		float alpha = std::sinf(w0) / (2.0f * Q);
		float cosw0 = std::cosf(w0);

		float b0 = 1.0f;
		float b1 = -2.0f * cosw0;
		float b2 = 1.0f;
		float a0 = 1.0f + alpha;
		float a1 = -2.0f * cosw0;
		float a2 = 1.0f - alpha;

		// Normalize by a0
		float inv_a0 = 1.0f / a0;
		b0 *= inv_a0;
		b1 *= inv_a0;
		b2 *= inv_a0;
		a1 *= inv_a0;
		a2 *= inv_a0;

		// Scale factor for int32_t <-> float conversion
		constexpr float kToFloat = 1.0f / 2147483648.0f;
		constexpr float kToQ31 = 2147483648.0f;

		for (auto& sample : buffer) {
			// Left channel — Direct Form II Transposed
			float inL = static_cast<float>(sample.l) * kToFloat;
			float outL = b0 * inL + notchZ1L;
			notchZ1L = b1 * inL - a1 * outL + notchZ2L;
			notchZ2L = b2 * inL - a2 * outL;
			sample.l = static_cast<int32_t>(outL * kToQ31);

			// Right channel
			float inR = static_cast<float>(sample.r) * kToFloat;
			float outR = b0 * inR + notchZ1R;
			notchZ1R = b1 * inR - a1 * outR + notchZ2R;
			notchZ2R = b2 * inR - a2 * outR;
			sample.r = static_cast<int32_t>(outR * kToQ31);
		}
	}

	// ========================================================================
	// renderOsc — harmonic oscillator with AR envelope (POST-reverb)
	// ========================================================================
	// Generates a sine at a harmonic of the note frequency, with AR envelope,
	// portamento, and phase/spread control. Mixes additively into buffer.

	void renderOsc(std::span<StereoSample> buffer, int32_t noteCode, bool voicesActive, q31_t fineFinalValue) {
		if (!isOscEnabled()) {
			return;
		}

		// --- Pitch calculation ---
		uint32_t basePhaseInc = noteCodeToPhaseIncrement(noteCode);
		int32_t tableIdx = std::min(static_cast<int32_t>(harmonic) - 1, kNumHarmonics - 1);
		float ratio = kHarmonicTable[tableIdx].ratio;

		// Pink noise loudness scaling: -3dB/octave, calibrated at 40Hz = unity (no boost below)
		// Computed after portamento so we use the actual output frequency

		// Apply fine tune: fineFinalValue is bipolar Q31, map to +/-12 semitones
		// fineFinalValue: -ONE_Q31 = -12st, 0 = 0st, +ONE_Q31 = +12st
		// Frequency multiplier = 2^(semitones/12)
		float fineSemitones = (static_cast<float>(fineFinalValue) / static_cast<float>(ONE_Q31)) * 12.0f;
		float fineMul = std::exp2f(fineSemitones / 12.0f);

		float newTargetFreq = static_cast<float>(basePhaseInc) * ratio * fineMul;
		targetFreq = newTargetFreq;

		// --- Trigger detection (moved before portamento so we can snap on retrigger) ---
		bool triggered = voicesActive && !voicesWereActive;

		// --- Portamento ---
		// Snap immediately if: no porta, first use, or retrigger after full note-off
		if (porta == 0 || currentFreq == 0.0f || triggered) {
			currentFreq = targetFreq;
		}
		else {
			// Map porta knob (1..127) to smoothing rate per sample
			// Higher knob = slower glide
			float portaRate =
			    kHarmPortaMaxRate - (static_cast<float>(porta) / 127.0f) * (kHarmPortaMaxRate - kHarmPortaMinRate);
			// Apply per-sample smoothing for the whole buffer
			for (size_t i = 0; i < buffer.size(); i++) {
				currentFreq += (targetFreq - currentFreq) * portaRate;
			}
		}

		uint32_t phaseIncrement = static_cast<uint32_t>(std::max(currentFreq, 0.0f));

		// Pink noise scaling: -3dB/octave, 40Hz = unity, never boosts
		// Convert phaseIncrement to Hz: freq = phaseInc * sampleRate / 2^32
		float outputHz = currentFreq * (kHarmSampleRate / 4294967296.0f);
		float pinkScale = (outputHz > 40.0f) ? std::sqrtf(40.0f / outputHz) : 1.0f;

		// --- Phase/Spread on trigger ---
		if (triggered) {
			if (phase <= 64) {
				// Mono start phase: 0..64 -> 0..2pi
				uint32_t startPhase = (static_cast<uint64_t>(phase) * 0xFFFFFFFFu) / 64u;
				phaseAccumL = startPhase;
				phaseAccumR = startPhase;
			}
			else {
				// Stereo spread: 65..127 -> 0..180 degrees spread
				uint32_t spread = (static_cast<uint64_t>(phase - 65) * 0x80000000u) / 62u;
				phaseAccumL = spread / 2;
				phaseAccumR = static_cast<uint32_t>(0u - spread / 2);
			}
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

		// --- Per-sample processing ---
		for (auto& sample : buffer) {
			// Update AR envelope
			if (voicesActive || triggered) {
				// Attack / sustain
				envelope += (1.0f - envelope) * attackRate;
				if (envelope > 1.0f) {
					envelope = 1.0f;
				}
			}
			else {
				// Release
				envelope -= envelope * releaseRate;

				// Zero-crossing shutoff: snap to zero when envelope is tiny
				// and phase accumulator crosses 0 or pi
				if (envelope < kHarmEnvThreshold) {
					uint32_t prevPhase = phaseAccumL - phaseIncrement;
					// Check if we crossed 0 or 0x80000000
					bool crossedZero = (phaseAccumL < phaseIncrement); // wrapped around 0
					bool crossedPi = ((prevPhase ^ phaseAccumL) & 0x80000000u) != 0;
					if (crossedZero || crossedPi) {
						envelope = 0.0f;
					}
				}
			}

			// Advance phase accumulators
			phaseAccumL += phaseIncrement;
			if (isStereo) {
				phaseAccumR += phaseIncrement;
			}
			else {
				phaseAccumR = phaseAccumL;
			}

			// Generate sine — scale to EFFECTIVE_0DBFS_Q31 (internal 0dBFS = ONE_Q31/128)
			// getSine returns full Q31 range, shift right by 7 to match internal levels
			// Apply pink noise curve: 1/sqrt(ratio) for -3dB/octave rolloff
			q31_t pinkQ31 = static_cast<q31_t>(pinkScale * static_cast<float>(ONE_Q31));
			int32_t sineL = multiply_32x32_rshift32(getSine(phaseAccumL), pinkQ31) >> 6;
			int32_t sineR = isStereo ? (multiply_32x32_rshift32(getSine(phaseAccumR), pinkQ31) >> 6) : sineL;

			// Apply envelope (convert float to Q31)
			q31_t envQ31 = static_cast<q31_t>(envelope * static_cast<float>(ONE_Q31));
			sineL = multiply_32x32_rshift32(sineL, envQ31) << 1;
			sineR = multiply_32x32_rshift32(sineR, envQ31) << 1;

			// Apply level from direct knob (0-127 -> 0 to full scale)
			q31_t levelQ31 = static_cast<q31_t>((static_cast<int64_t>(level) * ONE_Q31) / 127);
			sineL = multiply_32x32_rshift32(sineL, levelQ31) << 1;
			sineR = multiply_32x32_rshift32(sineR, levelQ31) << 1;

			// Mix into buffer (additive)
			sample.l = add_saturate(sample.l, sineL);
			sample.r = add_saturate(sample.r, sineR);
		}

		// Update trigger state
		voicesWereActive = voicesActive;
	}

	// ========================================================================
	// Serialization
	// ========================================================================

	void writeToFile(Serializer& writer) const {
		WRITE_FIELD_DEFAULT(writer, harmonic, "harmHarmonic", kHarmDefault);
		WRITE_FIELD(writer, level, "harmLevel");
		WRITE_FIELD(writer, phase, "harmPhase");
		WRITE_FIELD(writer, attack, "harmAttack");
		WRITE_FIELD_DEFAULT(writer, release, "harmRelease", 64);
		WRITE_FIELD(writer, hpf, "harmHpf");
		WRITE_FIELD(writer, porta, "harmPorta");
	}

	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, harmonic, "harmHarmonic");
		READ_FIELD(reader, tagName, level, "harmLevel");
		READ_FIELD(reader, tagName, phase, "harmPhase");
		READ_FIELD(reader, tagName, attack, "harmAttack");
		READ_FIELD(reader, tagName, release, "harmRelease");
		READ_FIELD(reader, tagName, hpf, "harmHpf");
		READ_FIELD(reader, tagName, porta, "harmPorta");
		return false;
	}
};

} // namespace deluge::dsp
