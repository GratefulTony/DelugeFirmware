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
	uint8_t harmonic{0}; // 0=off, 1=1/4, 2=1/3, 3=1/2, 4=1, 5=2, 6=3, 7=4, 8=5, 9=6, 10=7, 11=8
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
	q31_t hpfStateL1{0};          // Cascaded HPF state (1st pole, left)
	q31_t hpfStateR1{0};          // 1st pole, right
	q31_t hpfStateL2{0};          // 2nd pole, left
	q31_t hpfStateR2{0};          // 2nd pole, right

	// Harmonic ratio table: index maps to frequency multiplier
	static constexpr float kHarmonicRatios[] = {
	    0.0f, 0.25f, 1.0f / 3.0f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
	};
	static constexpr int32_t kNumHarmonics = 12;

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
	// renderHpf — 12dB note-tracking high-pass filter (PRE-reverb)
	// ========================================================================
	// Two cascaded single-pole HPF stages.
	// Cutoff derived from noteCode: hpf knob 1..127 maps cutoff from
	// 3 octaves below the note (knob=1) up to the note frequency (knob=127).
	// When hpf==0, caller should skip this entirely.

	void renderHpf(std::span<StereoSample> buffer, int32_t noteCode) {
		if (!isHpfEnabled()) {
			return;
		}

		// Compute cutoff frequency as phase increment
		// hpf knob 1..127 -> cutoff at (note - 3_octaves) .. (note)
		// That's noteCode - 36 .. noteCode
		// Linear interpolation: cutoffNote = noteCode - 36 + (hpf - 1) * 36 / 126
		int32_t cutoffOffset = 36 - ((hpf - 1) * 36) / 126; // 36 down to 0
		int32_t cutoffNote = noteCode - cutoffOffset;

		uint32_t cutoffPhaseInc = noteCodeToPhaseIncrement(cutoffNote);

		// alpha for single-pole filter: alpha = 2*pi*fc/fs
		// Since phaseIncrement = fc * 2^32 / fs, we have:
		//   alpha = phaseIncrement * 2*pi / 2^32
		// In Q31: alpha_q31 = phaseIncrement * (2*pi / 2^32) * 2^31
		//       = phaseIncrement * pi / 2^1 (approximately)
		// Simpler: alpha ~= phaseIncrement >> 1 (since 2pi/2^32 * 2^31 = pi ≈ 3.14, close to shifting)
		// More accurate: multiply by 3 and shift right by 1 to approximate pi
		// Actually: alpha = phaseInc * (2*pi) / (2^32)
		// As a fraction of full scale Q31: alpha_q31 = (phaseInc * 2*pi * 2^31) / 2^32
		//   = phaseInc * pi ≈ phaseInc * 3
		// But we need alpha < 1.0 (Q31 < ONE_Q31), and for audio frequencies phaseInc is already
		// large enough that phaseInc*3 could overflow. So use: alpha_q31 = phaseInc (this
		// approximates alpha = phaseInc/2^31 * pi/2 which slightly underestimates but is stable).
		// For better accuracy without overflow:
		q31_t alpha = static_cast<q31_t>(std::min(cutoffPhaseInc, static_cast<uint32_t>(ONE_Q31 >> 1)));

		for (auto& sample : buffer) {
			// Left channel — 1st pole
			q31_t hpOutL = sample.l - hpfStateL1;
			hpfStateL1 += multiply_32x32_rshift32(alpha, hpOutL) << 1;

			// Left channel — 2nd pole
			q31_t hpOutL2 = hpOutL - hpfStateL2;
			hpfStateL2 += multiply_32x32_rshift32(alpha, hpOutL2) << 1;

			// Right channel — 1st pole
			q31_t hpOutR = sample.r - hpfStateR1;
			hpfStateR1 += multiply_32x32_rshift32(alpha, hpOutR) << 1;

			// Right channel — 2nd pole
			q31_t hpOutR2 = hpOutR - hpfStateR2;
			hpfStateR2 += multiply_32x32_rshift32(alpha, hpOutR2) << 1;

			sample.l = hpOutL2;
			sample.r = hpOutR2;
		}
	}

	// ========================================================================
	// renderOsc — harmonic oscillator with AR envelope (POST-reverb)
	// ========================================================================
	// Generates a sine at a harmonic of the note frequency, with AR envelope,
	// portamento, and phase/spread control. Mixes additively into buffer.

	void renderOsc(std::span<StereoSample> buffer, int32_t noteCode, bool voicesActive, q31_t levelFinalValue,
	               q31_t fineFinalValue) {
		if (!isOscEnabled()) {
			return;
		}

		// --- Pitch calculation ---
		uint32_t basePhaseInc = noteCodeToPhaseIncrement(noteCode);
		float ratio = kHarmonicRatios[std::min(static_cast<int>(harmonic), kNumHarmonics - 1)];

		// Apply fine tune: fineFinalValue is bipolar Q31, map to +/-12 semitones
		// fineFinalValue: -ONE_Q31 = -12st, 0 = 0st, +ONE_Q31 = +12st
		// Frequency multiplier = 2^(semitones/12)
		float fineSemitones = (static_cast<float>(fineFinalValue) / static_cast<float>(ONE_Q31)) * 12.0f;
		float fineMul = std::exp2f(fineSemitones / 12.0f);

		float newTargetFreq = static_cast<float>(basePhaseInc) * ratio * fineMul;
		targetFreq = newTargetFreq;

		// --- Portamento ---
		if (porta == 0 || currentFreq == 0.0f) {
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

		// --- Trigger detection ---
		bool triggered = voicesActive && !voicesWereActive;

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

			// Skip mix if level is zero (keep phase/envelope running)
			if (levelFinalValue == 0) {
				continue;
			}

			// Generate sine
			int32_t sineL = getSine(phaseAccumL);
			int32_t sineR = isStereo ? getSine(phaseAccumR) : sineL;

			// Apply envelope (convert float to Q31)
			q31_t envQ31 = static_cast<q31_t>(envelope * static_cast<float>(ONE_Q31));
			sineL = multiply_32x32_rshift32(sineL, envQ31) << 1;
			sineR = multiply_32x32_rshift32(sineR, envQ31) << 1;

			// Apply level
			sineL = multiply_32x32_rshift32(sineL, levelFinalValue) << 1;
			sineR = multiply_32x32_rshift32(sineR, levelFinalValue) << 1;

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
		WRITE_FIELD(writer, harmonic, "harmHarmonic");
		WRITE_FIELD(writer, phase, "harmPhase");
		WRITE_FIELD(writer, attack, "harmAttack");
		WRITE_FIELD_DEFAULT(writer, release, "harmRelease", 64);
		WRITE_FIELD(writer, hpf, "harmHpf");
		WRITE_FIELD(writer, porta, "harmPorta");
	}

	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, harmonic, "harmHarmonic");
		READ_FIELD(reader, tagName, phase, "harmPhase");
		READ_FIELD(reader, tagName, attack, "harmAttack");
		READ_FIELD(reader, tagName, release, "harmRelease");
		READ_FIELD(reader, tagName, hpf, "harmHpf");
		READ_FIELD(reader, tagName, porta, "harmPorta");
		return false;
	}
};

} // namespace deluge::dsp
