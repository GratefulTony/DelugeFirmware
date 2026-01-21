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

#include "dsp/reverb/featherverb.hpp"
#include "dsp/phi_triangle.hpp"
#include "memory/general_memory_allocator.h"
#include "util/fixedpoint.h"

namespace deluge::dsp::reverb {

using namespace deluge::dsp;

// Compile-time diagnostic toggles (for permanent debugging builds)
// Runtime toggle cascadeOnly_ is preferred - controlled via predelay encoder button
static constexpr bool kMuteEarly = false;
static constexpr bool kMuteCascade = false;
static constexpr bool kMuteCascadeFeedback = false;
static constexpr bool kBypassFdnToCascade = false;
static constexpr bool kDisableVastUndersample = false;

// === Development notes ===
//
// RESOLVED: C3 at 8x undersample caused ringing
// C0/C1/C2 at 4x works fine. C3 at 8x caused audible ringing at high Zone 3.
// Root cause likely: allpass coefficient (up to 0.7) too high at 8x rate, or
// aliasing from insufficient anti-aliasing at that decimation factor.
// Solution: All cascade stages now use uniform 4x undersample in vast mode.
//
// FEEDBACK TUNING (after 8x undersample fix):
// With C3 at 4x (not 8x), moderate feedback increases are now stable:
// - cascadeNestFeedback_ max: 0.55 (was 0.45)
// - cascadeFeedbackMult_ max: 1.0 (was 0.95)
// - cascadeNestFeedbackBase_ max: 0.4, kicks in at 0.35 (was 0.3, kicked in at 0.4)
// - vastBoost: +0.25 in zone 7 only (compensates for pre-AA filter)
// Previous experiment with 0.7/1.1 caused ringing - may have been 8x undersample issue.
//
// MULTI-TAP WRITES FOR DENSITY:
// Multi-tap reads block CPU waiting for memory; multi-tap writes can be pipelined.
// Each cascade stage writes to both normal position AND prime-offset position,
// doubling impulse density without read penalty. Sequential writes stay cache-hot.
// Enabled via kEnableMultiTapWrites in header. Offsets are prime numbers for good diffusion.

Featherverb::Featherverb() {
	// Compute buffer offsets for contiguous layout
	size_t offset = 0;

	// FDN delays (3 delays)
	for (size_t i = 0; i < kNumFdnDelays; ++i) {
		fdnOffsets_[i] = offset;
		offset += (i == 0) ? kD0MaxLength : (i == 1) ? kD1MaxLength : kD2MaxLength;
	}

	// Cascade stages (4 allpass delays) - allocate max size
	size_t cascadeMaxLengths[kNumCascade] = {
	    static_cast<size_t>(kC0BaseLength * kCascadeMaxScale), static_cast<size_t>(kC1BaseLength * kCascadeMaxScale),
	    static_cast<size_t>(kC2BaseLength * kCascadeMaxScale), static_cast<size_t>(kC3BaseLength * kCascadeMaxScale)};
	for (size_t i = 0; i < kNumCascade; ++i) {
		cascadeOffsets_[i] = offset;
		offset += cascadeMaxLengths[i];
	}

	// Predelay
	predelayOffset_ = offset;
	offset += kPredelayMaxLength;

	// Diffusers
	diffuserOffsets_[0] = offset;
	offset += kDiffuser0Length;
	diffuserOffsets_[1] = offset;

	// Initialize defaults
	setRoomSize(0.5f);
	setDamping(0.5f);
	updateMatrix();
	updateSizes();
	updateFeedbackPattern();
}

bool Featherverb::allocate() {
	if (buffer_ != nullptr) {
		return true;
	}

	buffer_ = static_cast<float*>(
	    GeneralMemoryAllocator::get().regions[MEMORY_REGION_STEALABLE].alloc(kBufferBytes, false, nullptr));

	if (buffer_ == nullptr) {
		return false;
	}

	std::memset(buffer_, 0, kBufferBytes);

	// Reset state
	fdnWritePos_.fill(0);
	fdnLpState_.fill(0.0f);
	cascadeWritePos_.fill(0);
	cascadeLpState_ = 0.0f;
	prevC3Out_ = 0.0f;
	diffuserWritePos_.fill(0);
	predelayWritePos_ = 0;
	dcBlockState_ = 0.0f;
	inputEnvelope_ = 0.0f;
	hpState_ = 0.0f;
	lpStateL_ = 0.0f;
	lpStateR_ = 0.0f;
	lfoPhase_ = 0.0f;
	prevOutputMono_ = 0.0f;
	cascadeModDepth_ = 0.0f;
	cascadeAmpMod_ = 0.0f;

	// Reset undersampling
	undersamplePhase_ = false;
	accumIn_ = 0.0f;
	prevOutL_ = prevOutR_ = currOutL_ = currOutR_ = 0.0f;

	// Reset cascade extra undersampling
	cascadeDoubleUndersample_ = false;
	c0Phase_ = 0;
	c0Accum_ = 0.0f;
	c0Prev_ = 0.0f;
	c1Phase_ = 0;
	c1Accum_ = 0.0f;
	c1Prev_ = 0.0f;
	c2Phase_ = 0;
	c2Accum_ = 0.0f;
	c2Prev_ = 0.0f;
	c3Phase_ = 0;
	c3Accum_ = 0.0f;
	c3Prev_ = 0.0f;

	return true;
}

void Featherverb::deallocate() {
	if (buffer_ != nullptr) {
		delugeDealloc(buffer_);
		buffer_ = nullptr;
	}
}

void Featherverb::process(std::span<int32_t> input, StereoBuffer<q31_t> output) {
	if (buffer_ == nullptr) {
		return;
	}

	constexpr float kInputScale = 1.0f / static_cast<float>(std::numeric_limits<int32_t>::max());
	constexpr float kOutputScale =
	    static_cast<float>(std::numeric_limits<int32_t>::max()) * 32.0f; // 2x boost vs original

	const float hpCoeff = 0.995f - hpCutoff_ * 0.09f;
	const float outLpCoeff = 0.1f + lpCutoff_ * 0.85f;

	// Cache matrix
	const auto& m = matrix_;

	for (size_t frame = 0; frame < input.size(); ++frame) {
		// === Full-rate: HPF, envelope, predelay ===
		float in = static_cast<float>(input[frame]) * kInputScale;
		float hpOut = in - hpState_;
		hpState_ += (1.0f - hpCoeff) * hpOut;
		in = hpOut;

		// Input envelope for auto-decay
		float inAbs = std::fabs(in);
		if (inAbs > inputEnvelope_) {
			inputEnvelope_ = inAbs;
		}
		else {
			float releaseRate = 0.0001f + (1023 - zone2_) * 0.0002f / 1023.0f;
			inputEnvelope_ += releaseRate * (inAbs - inputEnvelope_);
		}

		// Predelay (single tap)
		if (predelayLength_ > 0) {
			writePredelay(in);
			in = readPredelay(predelayLength_);
		}

		float outL, outR;

		if constexpr (kUndersample) {
			// === 2x Undersampling ===
			accumIn_ += in;

			if (undersamplePhase_) {
				float fdnIn = accumIn_ * 0.5f;
				accumIn_ = 0.0f;

				// LFO (slower rate for smoother modulation)
				lfoPhase_ += 0.0000034f;
				if (lfoPhase_ >= 1.0f)
					lfoPhase_ -= 1.0f;
				float lfoTri = lfoPhase_ < 0.5f ? (4.0f * lfoPhase_ - 1.0f) : (3.0f - 4.0f * lfoPhase_);
				size_t d0Mod = static_cast<size_t>(std::max(0.0f, lfoTri * modDepth_));
				size_t d1Mod = static_cast<size_t>(std::max(0.0f, -lfoTri * modDepth_));

				// Read FDN delays
				float d0 = fdnReadAt(0, d0Mod);
				float d1 = fdnReadAt(1, d1Mod);
				float d2 = fdnRead(2);

				// 3x3 matrix multiply
				float h0 = m[0][0] * d0 + m[0][1] * d1 + m[0][2] * d2;
				float h1 = m[1][0] * d0 + m[1][1] * d1 + m[1][2] * d2;
				float h2 = m[2][0] * d0 + m[2][1] * d1 + m[2][2] * d2;

				// Cross-channel bleed: L↔R mixing for stereo complexity
				if (crossBleed_ > 0.0f) {
					float h0Orig = h0;
					h0 += h1 * crossBleed_;
					h1 += h0Orig * crossBleed_;
				}

				// Feedback with auto-decay
				float effectiveFeedback = feedback_;
				constexpr float kEnvReference = 0.001f;
				constexpr float kMinFeedbackMult = 0.6f;
				float feedbackFloor = kMinFeedbackMult + (zone2_ * (1.0f - kMinFeedbackMult)) / 1023.0f;
				float envNorm = std::min(inputEnvelope_ / kEnvReference, 1.0f);
				float feedbackMod = feedbackFloor + envNorm * (1.0f - feedbackFloor);
				effectiveFeedback *= feedbackMod;

				// Damping + feedback
				h0 = onepole(h0, fdnLpState_[0], dampCoeff_) * effectiveFeedback * feedbackMult_[0];
				h1 = onepole(h1, fdnLpState_[1], dampCoeff_) * effectiveFeedback * feedbackMult_[1];
				h2 = onepole(h2, fdnLpState_[2], dampCoeff_) * effectiveFeedback * feedbackMult_[2];

				// DC blocking on FDN
				float dcSum = (h0 + h1 + h2) * 0.333f;
				dcBlockState_ += 0.007f * (dcSum - dcBlockState_);
				h0 -= dcBlockState_;
				h1 -= dcBlockState_;
				h2 -= dcBlockState_;

				// === Cascade: 4-stage with parallel/series blend ===
				// c0→c1→c2 always series, c3 input blends between cascadeIn (parallel) and c2 (series)
				// seriesMix=0 → 9 paths (sparse), seriesMix=1 → 16 paths (dense)
				float tailFeedback = feedback_ * feedback_; // Tail decays faster than early at low room
				float cascadeIn;
				if (kBypassFdnToCascade || cascadeOnly_) {
					cascadeIn = fdnIn * 1.4f + prevC3Out_ * cascadeNestFeedback_ * tailFeedback;
				}
				else {
					// TODO(FDN tail investigation): This 0.4 gain causes ~4x less energy into cascade
					// vs cascade-only mode (1.5). Contributing factors to FDN killing tails:
					// 1. Gain mismatch: 0.4 vs 1.5 = ~4x difference in cascade drive
					// 2. FDN damping: LPF on d0/d1/d2 attenuates before reaching cascade
					// 3. Matrix absorption: Hadamard mixing distributes/cancels energy
					// To fix: try increasing to 0.8 or higher, or add separate cascade input gain
					cascadeIn = (d0 + d1 + d2) * 0.4f + prevC3Out_ * cascadeNestFeedback_ * tailFeedback;
				}

				// c0→c1→c2 series chain with optional 4x undersample for vast rooms
				float c0, c1, c2;
				if (cascadeDoubleUndersample_) {
					// Pre-decimation AA filter (1 pole before, 1 pole after cascade)
					cascadeIn = onepole(cascadeIn, cascadeAaState1_, kPreCascadeAaCoeff);
					// C0 with 4x undersample
					c0Accum_ += cascadeIn;
					if (c0Phase_ == 1) {
						float avgIn = c0Accum_ * 0.5f;
						float c0Coeff = cascadeCoeffs_[0];
						size_t origWritePos = cascadeWritePos_[0];
						size_t idx = cascadeOffsets_[0] + origWritePos;
						float delayed = buffer_[idx];
						float output = -c0Coeff * avgIn + delayed;
						float writeVal = avgIn + c0Coeff * output;
						buffer_[idx] = writeVal;
						if (++cascadeWritePos_[0] >= cascadeLengths_[0])
							cascadeWritePos_[0] = 0;
						buffer_[cascadeOffsets_[0] + cascadeWritePos_[0]] = writeVal;
						if (++cascadeWritePos_[0] >= cascadeLengths_[0])
							cascadeWritePos_[0] = 0;
						// Multi-tap write for density
						if constexpr (kEnableMultiTapWrites) {
							size_t tapPos = (origWritePos + kMultiTapOffsets[0]) % cascadeLengths_[0];
							buffer_[cascadeOffsets_[0] + tapPos] += writeVal * kMultiTapGain;
						}
						c0Prev_ = output;
						c0Accum_ = 0.0f;
					}
					c0Phase_ = (c0Phase_ + 1) & 1;
					c0 = c0Prev_;

					// C1 with 4x undersample
					c1Accum_ += c0;
					if (c1Phase_ == 1) {
						float avgIn = c1Accum_ * 0.5f;
						float c1Coeff = cascadeCoeffs_[1];
						size_t origWritePos = cascadeWritePos_[1];
						size_t idx = cascadeOffsets_[1] + origWritePos;
						float delayed = buffer_[idx];
						float output = -c1Coeff * avgIn + delayed;
						float writeVal = avgIn + c1Coeff * output;
						buffer_[idx] = writeVal;
						if (++cascadeWritePos_[1] >= cascadeLengths_[1])
							cascadeWritePos_[1] = 0;
						buffer_[cascadeOffsets_[1] + cascadeWritePos_[1]] = writeVal;
						if (++cascadeWritePos_[1] >= cascadeLengths_[1])
							cascadeWritePos_[1] = 0;
						// Multi-tap write for density
						if constexpr (kEnableMultiTapWrites) {
							size_t tapPos = (origWritePos + kMultiTapOffsets[1]) % cascadeLengths_[1];
							buffer_[cascadeOffsets_[1] + tapPos] += writeVal * kMultiTapGain;
						}
						c1Prev_ = output;
						c1Accum_ = 0.0f;
					}
					c1Phase_ = (c1Phase_ + 1) & 1;
					c1 = c1Prev_;

					// C2 with 4x undersample
					c2Accum_ += c1;
					if (c2Phase_ == 1) {
						float avgIn = c2Accum_ * 0.5f;
						float c2Coeff = cascadeCoeffs_[2];
						size_t origWritePos = cascadeWritePos_[2];
						// Pitch modulation: read from offset position for chorus-like smearing
						size_t c2ModOffset = static_cast<size_t>(std::max(0.0f, lfoTri * cascadeModDepth_));
						size_t readPos = (origWritePos + c2ModOffset) % cascadeLengths_[2];
						size_t idx = cascadeOffsets_[2] + readPos;
						float delayed = buffer_[idx];
						float output = -c2Coeff * avgIn + delayed;
						float writeVal = avgIn + c2Coeff * output;
						buffer_[cascadeOffsets_[2] + origWritePos] = writeVal;
						if (++cascadeWritePos_[2] >= cascadeLengths_[2])
							cascadeWritePos_[2] = 0;
						buffer_[cascadeOffsets_[2] + cascadeWritePos_[2]] = writeVal;
						if (++cascadeWritePos_[2] >= cascadeLengths_[2])
							cascadeWritePos_[2] = 0;
						// Multi-tap write for density
						if constexpr (kEnableMultiTapWrites) {
							size_t tapPos = (origWritePos + kMultiTapOffsets[2]) % cascadeLengths_[2];
							buffer_[cascadeOffsets_[2] + tapPos] += writeVal * kMultiTapGain;
						}
						c2Prev_ = output;
						c2Accum_ = 0.0f;
					}
					c2Phase_ = (c2Phase_ + 1) & 1;
					c2 = c2Prev_;
				}
				else {
					c0 = processCascadeStage(0, cascadeIn);
					c1 = processCascadeStage(1, c0);
					c2 = processCascadeStage(2, c1);
				}

				// C3 input: blend parallel (cascadeIn) ↔ series (c2)
				float c3In = cascadeIn + (c2 - cascadeIn) * cascadeSeriesMix_;

				// C3 with 4x undersample for vast rooms (Zone 2 > 80%)
				// All cascade stages use uniform 4x in vast mode (8x caused ringing)
				float c3;
				if (cascadeDoubleUndersample_) {
					c3Accum_ += c3In;
					if (c3Phase_ == 1) {
						float avgIn = c3Accum_ * 0.5f;
						float c3Coeff = cascadeCoeffs_[3];
						size_t origWritePos = cascadeWritePos_[3];
						// Pitch modulation: inverted phase from C2 for decorrelation
						size_t c3ModOffset = static_cast<size_t>(std::max(0.0f, -lfoTri * cascadeModDepth_));
						size_t readPos = (origWritePos + c3ModOffset) % cascadeLengths_[3];
						size_t idx = cascadeOffsets_[3] + readPos;
						float delayed = buffer_[idx];
						float output = -c3Coeff * avgIn + delayed;
						float writeVal = avgIn + c3Coeff * output;
						buffer_[cascadeOffsets_[3] + origWritePos] = writeVal;
						if (++cascadeWritePos_[3] >= cascadeLengths_[3])
							cascadeWritePos_[3] = 0;
						buffer_[cascadeOffsets_[3] + cascadeWritePos_[3]] = writeVal;
						if (++cascadeWritePos_[3] >= cascadeLengths_[3])
							cascadeWritePos_[3] = 0;
						// Multi-tap write for density
						if constexpr (kEnableMultiTapWrites) {
							size_t tapPos = (origWritePos + kMultiTapOffsets[3]) % cascadeLengths_[3];
							buffer_[cascadeOffsets_[3] + tapPos] += writeVal * kMultiTapGain;
						}
						c3Prev_ = output;
						c3Accum_ = 0.0f;
					}
					c3Phase_ = (c3Phase_ + 1) & 1;
					c3 = c3Prev_;
				}
				else {
					c3 = processCascadeStage(3, c3In);
				}
				prevC3Out_ = c3; // Store for nested feedback next sample

				// Amplitude modulation on C2/C3 for diffusion contour (opposite phases)
				// Creates stereo movement as the balance shifts between stages
				if (cascadeAmpMod_ > 0.0f) {
					c2 *= (1.0f + lfoTri * cascadeAmpMod_);
					c3 *= (1.0f - lfoTri * cascadeAmpMod_);
				}

				// Width breathing: expand stereo as signal decays
				float dynamicWidth = width_ + (1.0f - std::min(inputEnvelope_ * 100.0f, 1.0f)) * widthBreath_;

				// Mix outputs - stereo tail from cascade (mid/side)
				// c2+c3 = dense tail (both channels), c0-c1 = stereo spread scaled by width
				float cascadeMono = (c2 + c3) * 0.5f;
				float cascadeSide = (c0 - c1) * 0.2f * dynamicWidth;

				// Cascade LP filters - mono darker (~5.5kHz), side brighter (~8kHz) for airy stereo
				cascadeMono = onepole(cascadeMono, cascadeLpStateMono_, kCascadeLpCoeffMono);
				cascadeSide = onepole(cascadeSide, cascadeLpStateSide_, kCascadeLpCoeffSide);

				// Apply damping to mono component
				cascadeMono = onepole(cascadeMono, cascadeLpState_, cascadeDamping_);
				float cascadeOutL = cascadeMono + cascadeSide;
				float cascadeOutR = cascadeMono - cascadeSide;

				// Inject input + cascade feedback into FDN (tail uses squared feedback)
				if (kMuteCascadeFeedback || cascadeOnly_) {
					h0 += fdnIn;
				}
				else {
					h0 += fdnIn + cascadeMono * tailFeedback * cascadeFeedbackMult_;
				}

				// Write FDN (double write for undersampling)
				fdnWrite(0, h0);
				fdnWrite(1, h1);
				fdnWrite(2, h2);
				fdnWrite(0, h0);
				fdnWrite(1, h1);
				fdnWrite(2, h2);

				// Output: mix early (FDN) + late (cascade)
				float earlyL = 0.0f, earlyR = 0.0f;
				if (!kMuteEarly && !cascadeOnly_) {
					float earlyMid = (d0 + d1) * earlyMixGain_;
					float earlySide = (d0 - d1) * earlyMixGain_ * dynamicWidth;
					earlyL = earlyMid + earlySide;
					earlyR = earlyMid - earlySide;
					// Save direct early for brightness tap (bypasses output LPF)
					directEarlyL_ = earlyL * directEarlyGain_;
					directEarlyR_ = earlyR * directEarlyGain_;
				}
				else {
					directEarlyL_ = 0.0f;
					directEarlyR_ = 0.0f;
				}

				float newOutL, newOutR;
				if (kMuteCascade) {
					newOutL = earlyL;
					newOutR = earlyR;
				}
				else {
					newOutL = earlyL + cascadeOutL * tailMixGain_;
					newOutR = earlyR + cascadeOutR * tailMixGain_;
				}

				// Global wet side boost from width knob (mid/side)
				// width=0: normal stereo, width=1: 2x side boost
				float wetMid = (newOutL + newOutR) * 0.5f;
				float wetSide = (newOutL - newOutR) * 0.5f * (1.0f + width_);
				newOutL = wetMid + wetSide;
				newOutR = wetMid - wetSide;

				prevOutL_ = currOutL_;
				prevOutR_ = currOutR_;
				currOutL_ = newOutL;
				currOutR_ = newOutR;

				outL = currOutL_;
				outR = currOutR_;
			}
			else {
				// Interpolate
				outL = (prevOutL_ + currOutL_) * 0.5f;
				outR = (prevOutR_ + currOutR_) * 0.5f;
			}

			undersamplePhase_ = !undersamplePhase_;
		}
		else {
			// === Full-rate mode ===
			lfoPhase_ += 0.0000017f;
			if (lfoPhase_ >= 1.0f)
				lfoPhase_ -= 1.0f;
			float lfoTri = lfoPhase_ < 0.5f ? (4.0f * lfoPhase_ - 1.0f) : (3.0f - 4.0f * lfoPhase_);
			size_t d0Mod = static_cast<size_t>(std::max(0.0f, lfoTri * modDepth_));
			size_t d1Mod = static_cast<size_t>(std::max(0.0f, -lfoTri * modDepth_));

			float d0 = fdnReadAt(0, d0Mod);
			float d1 = fdnReadAt(1, d1Mod);
			float d2 = fdnRead(2);

			float h0 = m[0][0] * d0 + m[0][1] * d1 + m[0][2] * d2;
			float h1 = m[1][0] * d0 + m[1][1] * d1 + m[1][2] * d2;
			float h2 = m[2][0] * d0 + m[2][1] * d1 + m[2][2] * d2;

			// Cross-channel bleed: L↔R mixing for stereo complexity
			if (crossBleed_ > 0.0f) {
				float h0Orig = h0;
				h0 += h1 * crossBleed_;
				h1 += h0Orig * crossBleed_;
			}

			float effectiveFeedback = feedback_;
			constexpr float kEnvReference = 0.001f;
			constexpr float kMinFeedbackMult = 0.6f;
			float feedbackFloor = kMinFeedbackMult + (zone2_ * (1.0f - kMinFeedbackMult)) / 1023.0f;
			float envNorm = std::min(inputEnvelope_ / kEnvReference, 1.0f);
			float feedbackMod = feedbackFloor + envNorm * (1.0f - feedbackFloor);
			effectiveFeedback *= feedbackMod;

			h0 = onepole(h0, fdnLpState_[0], dampCoeff_) * effectiveFeedback * feedbackMult_[0];
			h1 = onepole(h1, fdnLpState_[1], dampCoeff_) * effectiveFeedback * feedbackMult_[1];
			h2 = onepole(h2, fdnLpState_[2], dampCoeff_) * effectiveFeedback * feedbackMult_[2];

			float dcSum = (h0 + h1 + h2) * 0.333f;
			dcBlockState_ += 0.007f * (dcSum - dcBlockState_);
			h0 -= dcBlockState_;
			h1 -= dcBlockState_;
			h2 -= dcBlockState_;

			// === Cascade: 4-stage with parallel/series blend ===
			// c0→c1→c2 always series, c3 input blends between cascadeIn (parallel) and c2 (series)
			// seriesMix=0 → 9 paths (sparse), seriesMix=1 → 16 paths (dense)
			float tailFeedback = feedback_ * feedback_; // Tail decays faster than early at low room
			float cascadeIn;
			if (kBypassFdnToCascade || cascadeOnly_) {
				cascadeIn = in * 1.4f + prevC3Out_ * cascadeNestFeedback_ * tailFeedback;
			}
			else {
				// TODO(FDN tail investigation): See undersampled path comment for details
				// 0.4 gain vs 1.5 in cascade-only = ~4x less cascade drive when FDN active
				cascadeIn = (d0 + d1 + d2) * 0.4f + prevC3Out_ * cascadeNestFeedback_ * tailFeedback;
			}

			// c0→c1→c2 series chain with optional 2x undersample for vast rooms
			float c0, c1, c2;
			if (cascadeDoubleUndersample_) {
				// Pre-decimation AA filter (1 pole before, 1 pole after cascade)
				cascadeIn = onepole(cascadeIn, cascadeAaState1_, kPreCascadeAaCoeff);
				// C0 with 2x undersample
				c0Accum_ += cascadeIn;
				if (c0Phase_ == 1) {
					float avgIn = c0Accum_ * 0.5f;
					float c0Coeff = cascadeCoeffs_[0];
					size_t origWritePos = cascadeWritePos_[0];
					size_t idx = cascadeOffsets_[0] + origWritePos;
					float delayed = buffer_[idx];
					float output = -c0Coeff * avgIn + delayed;
					float writeVal = avgIn + c0Coeff * output;
					buffer_[idx] = writeVal;
					if (++cascadeWritePos_[0] >= cascadeLengths_[0])
						cascadeWritePos_[0] = 0;
					buffer_[cascadeOffsets_[0] + cascadeWritePos_[0]] = writeVal;
					if (++cascadeWritePos_[0] >= cascadeLengths_[0])
						cascadeWritePos_[0] = 0;
					// Multi-tap write for density
					if constexpr (kEnableMultiTapWrites) {
						size_t tapPos = (origWritePos + kMultiTapOffsets[0]) % cascadeLengths_[0];
						buffer_[cascadeOffsets_[0] + tapPos] += writeVal * kMultiTapGain;
					}
					c0Prev_ = output;
					c0Accum_ = 0.0f;
				}
				c0Phase_ = (c0Phase_ + 1) & 1;
				c0 = c0Prev_;

				// C1 with 2x undersample
				c1Accum_ += c0;
				if (c1Phase_ == 1) {
					float avgIn = c1Accum_ * 0.5f;
					float c1Coeff = cascadeCoeffs_[1];
					size_t origWritePos = cascadeWritePos_[1];
					size_t idx = cascadeOffsets_[1] + origWritePos;
					float delayed = buffer_[idx];
					float output = -c1Coeff * avgIn + delayed;
					float writeVal = avgIn + c1Coeff * output;
					buffer_[idx] = writeVal;
					if (++cascadeWritePos_[1] >= cascadeLengths_[1])
						cascadeWritePos_[1] = 0;
					buffer_[cascadeOffsets_[1] + cascadeWritePos_[1]] = writeVal;
					if (++cascadeWritePos_[1] >= cascadeLengths_[1])
						cascadeWritePos_[1] = 0;
					// Multi-tap write for density
					if constexpr (kEnableMultiTapWrites) {
						size_t tapPos = (origWritePos + kMultiTapOffsets[1]) % cascadeLengths_[1];
						buffer_[cascadeOffsets_[1] + tapPos] += writeVal * kMultiTapGain;
					}
					c1Prev_ = output;
					c1Accum_ = 0.0f;
				}
				c1Phase_ = (c1Phase_ + 1) & 1;
				c1 = c1Prev_;

				// C2 with 2x undersample
				c2Accum_ += c1;
				if (c2Phase_ == 1) {
					float avgIn = c2Accum_ * 0.5f;
					float c2Coeff = cascadeCoeffs_[2];
					size_t origWritePos = cascadeWritePos_[2];
					// Pitch modulation: read from offset position for chorus-like smearing
					size_t c2ModOffset = static_cast<size_t>(std::max(0.0f, lfoTri * cascadeModDepth_));
					size_t readPos = (origWritePos + c2ModOffset) % cascadeLengths_[2];
					size_t idx = cascadeOffsets_[2] + readPos;
					float delayed = buffer_[idx];
					float output = -c2Coeff * avgIn + delayed;
					float writeVal = avgIn + c2Coeff * output;
					buffer_[cascadeOffsets_[2] + origWritePos] = writeVal;
					if (++cascadeWritePos_[2] >= cascadeLengths_[2])
						cascadeWritePos_[2] = 0;
					buffer_[cascadeOffsets_[2] + cascadeWritePos_[2]] = writeVal;
					if (++cascadeWritePos_[2] >= cascadeLengths_[2])
						cascadeWritePos_[2] = 0;
					// Multi-tap write for density
					if constexpr (kEnableMultiTapWrites) {
						size_t tapPos = (origWritePos + kMultiTapOffsets[2]) % cascadeLengths_[2];
						buffer_[cascadeOffsets_[2] + tapPos] += writeVal * kMultiTapGain;
					}
					c2Prev_ = output;
					c2Accum_ = 0.0f;
				}
				c2Phase_ = (c2Phase_ + 1) & 1;
				c2 = c2Prev_;
			}
			else {
				c0 = processCascadeStage(0, cascadeIn);
				c1 = processCascadeStage(1, c0);
				c2 = processCascadeStage(2, c1);
			}

			// C3 input: blend parallel (cascadeIn) ↔ series (c2)
			float c3In = cascadeIn + (c2 - cascadeIn) * cascadeSeriesMix_;

			// C3 with 4x undersample for vast rooms (Zone 2 > 80%)
			// C0/C1/C2 at 2x, C3 at 4x for maximum tail extension (~362ms effective)
			float c3;
			if (cascadeDoubleUndersample_) {
				c3Accum_ += c3In;
				if (c3Phase_ == 3) {
					float avgIn = c3Accum_ * 0.25f; // Average over 4 samples
					float c3Coeff = cascadeCoeffs_[3];
					size_t origWritePos = cascadeWritePos_[3];
					// Pitch modulation: inverted phase from C2 for decorrelation
					size_t c3ModOffset = static_cast<size_t>(std::max(0.0f, -lfoTri * cascadeModDepth_));
					size_t readPos = (origWritePos + c3ModOffset) % cascadeLengths_[3];
					size_t idx = cascadeOffsets_[3] + readPos;
					float delayed = buffer_[idx];
					float output = -c3Coeff * avgIn + delayed;
					float writeVal = avgIn + c3Coeff * output;
					// Quad-write for 4x undersample
					buffer_[cascadeOffsets_[3] + origWritePos] = writeVal;
					if (++cascadeWritePos_[3] >= cascadeLengths_[3])
						cascadeWritePos_[3] = 0;
					buffer_[cascadeOffsets_[3] + cascadeWritePos_[3]] = writeVal;
					if (++cascadeWritePos_[3] >= cascadeLengths_[3])
						cascadeWritePos_[3] = 0;
					buffer_[cascadeOffsets_[3] + cascadeWritePos_[3]] = writeVal;
					if (++cascadeWritePos_[3] >= cascadeLengths_[3])
						cascadeWritePos_[3] = 0;
					buffer_[cascadeOffsets_[3] + cascadeWritePos_[3]] = writeVal;
					if (++cascadeWritePos_[3] >= cascadeLengths_[3])
						cascadeWritePos_[3] = 0;
					// Multi-tap write for density
					if constexpr (kEnableMultiTapWrites) {
						size_t tapPos = (origWritePos + kMultiTapOffsets[3]) % cascadeLengths_[3];
						buffer_[cascadeOffsets_[3] + tapPos] += writeVal * kMultiTapGain;
					}
					c3Prev_ = output;
					c3Accum_ = 0.0f;
				}
				c3Phase_ = (c3Phase_ + 1) & 3; // 2-bit counter for 4x
				c3 = c3Prev_;
			}
			else {
				c3 = processCascadeStage(3, c3In);
			}
			prevC3Out_ = c3; // Store for nested feedback next sample

			// Amplitude modulation on C2/C3 for diffusion contour (opposite phases)
			if (cascadeAmpMod_ > 0.0f) {
				c2 *= (1.0f + lfoTri * cascadeAmpMod_);
				c3 *= (1.0f - lfoTri * cascadeAmpMod_);
			}

			// Width breathing: expand stereo as signal decays
			float dynamicWidth = width_ + (1.0f - std::min(inputEnvelope_ * 100.0f, 1.0f)) * widthBreath_;

			// Mix outputs - stereo tail from cascade (mid/side)
			// c2+c3 = dense tail (both channels), c0-c1 = stereo spread scaled by width
			float cascadeMono = (c2 + c3) * 0.5f;
			float cascadeSide = (c0 - c1) * 0.2f * dynamicWidth;

			// Cascade LP filters - mono darker (~5.5kHz), side brighter (~8kHz) for airy stereo
			cascadeMono = onepole(cascadeMono, cascadeLpStateMono_, kCascadeLpCoeffMono);
			cascadeSide = onepole(cascadeSide, cascadeLpStateSide_, kCascadeLpCoeffSide);

			// Apply damping to mono component
			cascadeMono = onepole(cascadeMono, cascadeLpState_, cascadeDamping_);
			float cascadeOutL = cascadeMono + cascadeSide;
			float cascadeOutR = cascadeMono - cascadeSide;

			// Inject input + cascade feedback into FDN (tail uses squared feedback)
			if (kMuteCascadeFeedback || cascadeOnly_) {
				h0 += in;
			}
			else {
				h0 += in + cascadeMono * tailFeedback * cascadeFeedbackMult_;
			}

			fdnWrite(0, h0);
			fdnWrite(1, h1);
			fdnWrite(2, h2);

			// Output: mix early (FDN) + late (cascade)
			float earlyL = 0.0f, earlyR = 0.0f;
			if (!kMuteEarly && !cascadeOnly_) {
				float earlyMid = (d0 + d1) * earlyMixGain_;
				float earlySide = (d0 - d1) * earlyMixGain_ * dynamicWidth;
				earlyL = earlyMid + earlySide;
				earlyR = earlyMid - earlySide;
				// Save direct early for brightness tap (bypasses output LPF)
				directEarlyL_ = earlyL * directEarlyGain_;
				directEarlyR_ = earlyR * directEarlyGain_;
			}
			else {
				directEarlyL_ = 0.0f;
				directEarlyR_ = 0.0f;
			}

			float rawOutL, rawOutR;
			if (kMuteCascade) {
				rawOutL = earlyL;
				rawOutR = earlyR;
			}
			else {
				rawOutL = earlyL + cascadeOutL * tailMixGain_;
				rawOutR = earlyR + cascadeOutR * tailMixGain_;
			}

			// Global wet side boost from width knob (mid/side)
			// width=0: normal stereo, width=1: 2x side boost
			float wetMid = (rawOutL + rawOutR) * 0.5f;
			float wetSide = (rawOutL - rawOutR) * 0.5f * (1.0f + width_);
			outL = wetMid + wetSide;
			outR = wetMid - wetSide;
		}

		prevOutputMono_ = (outL + outR) * 0.5f;

		// Output LPF
		outL = onepole(outL, lpStateL_, outLpCoeff);
		outR = onepole(outR, lpStateR_, outLpCoeff);

		// Add direct early brightness tap (bypasses LPF for crisp transients)
		outL += directEarlyL_;
		outR += directEarlyR_;

		// Clamp and output
		constexpr float kMaxFloat = 0.06f;
		outL = std::clamp(outL, -kMaxFloat, kMaxFloat);
		outR = std::clamp(outR, -kMaxFloat, kMaxFloat);
		int32_t outLq31 = static_cast<int32_t>(outL * kOutputScale);
		int32_t outRq31 = static_cast<int32_t>(outR * kOutputScale);

		output[frame].l += multiply_32x32_rshift32_rounded(outLq31, getPanLeft());
		output[frame].r += multiply_32x32_rshift32_rounded(outRq31, getPanRight());
	}
}

void Featherverb::setRoomSize(float value) {
	roomSize_ = value;
	feedback_ = 0.75f + value * 0.24f;
}

void Featherverb::setDamping(float value) {
	damping_ = value;
	dampCoeff_ = 0.1f + (1.0f - value) * 0.85f;
	// cascadeDamping_ is computed in updateSizes() with vast mode modifier
	updateSizes();
}

void Featherverb::setWidth(float value) {
	width_ = value;
}

void Featherverb::setHPF(float f) {
	hpCutoff_ = f;
}

void Featherverb::setLPF(float f) {
	lpCutoff_ = f;
}

void Featherverb::setPredelay(float value) {
	predelay_ = value;
	predelayLength_ = static_cast<size_t>(value * kPredelayMaxLength);
}

// === Zone 1: Matrix morphing ===
// Simplified 3x3 matrix with phi triangle modulation

static constexpr std::array<phi::PhiTriConfig, 9> kMatrix3TriBank = {{
    {phi::kPhi025, 0.7f, 0.000f, true},
    {phi::kPhi050, 0.7f, 0.111f, true},
    {phi::kPhi075, 0.7f, 0.222f, true},
    {phi::kPhi100, 0.7f, 0.333f, true},
    {phi::kPhi125, 0.7f, 0.444f, true},
    {phi::kPhi150, 0.7f, 0.555f, true},
    {phi::kPhi175, 0.7f, 0.666f, true},
    {phi::kPhi200, 0.7f, 0.777f, true},
    {phi::kPhi033, 0.7f, 0.888f, true},
}};

void Featherverb::setZone1(int32_t value) {
	zone1_ = value;
	updateMatrix();
}

// Simple Gram-Schmidt for 3x3
static bool gramSchmidt3x3(std::array<std::array<float, 3>, 3>& m) {
	for (int col = 0; col < 3; ++col) {
		for (int prev = 0; prev < col; ++prev) {
			float dot = 0.0f;
			for (int row = 0; row < 3; ++row) {
				dot += m[row][col] * m[row][prev];
			}
			for (int row = 0; row < 3; ++row) {
				m[row][col] -= dot * m[row][prev];
			}
		}
		float norm = 0.0f;
		for (int row = 0; row < 3; ++row) {
			norm += m[row][col] * m[row][col];
		}
		norm = std::sqrt(norm);
		if (norm < 0.0001f)
			return false;
		for (int row = 0; row < 3; ++row) {
			m[row][col] /= norm;
		}
	}
	return true;
}

void Featherverb::updateMatrix() {
	using namespace phi;

	const float yNorm = static_cast<float>(zone1_) / 1023.0f;
	const int32_t zone = zone1_ >> 7;
	const double gammaPhase = zone * 0.125;

	const PhiTriContext ctx{yNorm, 1.0f, 1.0f, gammaPhase};
	std::array<float, 9> vals = ctx.evalBank(kMatrix3TriBank);

	// Base 3x3 Hadamard-like matrix
	static constexpr std::array<std::array<float, 3>, 3> kH3Base = {
	    {{1.0f, 1.0f, 1.0f}, {1.0f, -1.0f, 0.0f}, {1.0f, 0.0f, -1.0f}}};

	static constexpr std::array<float, 8> kZoneBlend = {0.0f, 0.15f, 0.3f, 0.45f, 0.6f, 0.75f, 0.85f, 0.95f};
	const float blend = kZoneBlend[zone];

	for (int row = 0; row < 3; ++row) {
		for (int col = 0; col < 3; ++col) {
			float base = kH3Base[row][col];
			float mod = vals[row * 3 + col];
			matrix_[row][col] = base + blend * mod * 0.5f;
		}
	}

	if (!gramSchmidt3x3(matrix_)) {
		// Fallback
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 3; ++col) {
				matrix_[row][col] = kH3Base[row][col] * kH3Norm;
			}
		}
	}

	modDepth_ = blend * 25.0f;

	// Update D0/D1 lengths from phi triangles
	float d0Tri = (vals[0] + 1.0f) * 0.5f;
	float d1Tri = (vals[3] + 1.0f) * 0.5f;
	fdnLengths_[0] = kD0MinLength + static_cast<size_t>(d0Tri * (kD0MaxLength - kD0MinLength));
	fdnLengths_[1] = kD1MinLength + static_cast<size_t>(d1Tri * (kD1MaxLength - kD1MinLength));

	if (fdnWritePos_[0] >= fdnLengths_[0])
		fdnWritePos_[0] = 0;
	if (fdnWritePos_[1] >= fdnLengths_[1])
		fdnWritePos_[1] = 0;
}

// === Zone 2: Size (D2 + cascade scaling) ===

void Featherverb::setZone2(int32_t value) {
	zone2_ = value;
	updateSizes();
}

void Featherverb::updateSizes() {
	const float t = static_cast<float>(zone2_) / 1023.0f;
	const int32_t zone = zone2_ >> 7; // Zone ID 0-7

	// D2 scales from min to max
	fdnLengths_[2] = kD2MinLength + static_cast<size_t>(t * (kD2MaxLength - kD2MinLength));
	if (fdnWritePos_[2] >= fdnLengths_[2])
		fdnWritePos_[2] = 0;

	// Cascade scales uniformly from 1x to 1.5x
	cascadeScale_ = 1.0f + t * (kCascadeMaxScale - 1.0f);

	// Early/tail balance: inverse relationship for room character
	// Tiny rooms: punchy early reflections, minimal tail (0.4 early, 0.25 tail)
	// Vast rooms: spacious tails, subtle early (0.15 early, 1.3 tail)
	earlyMixGain_ = 0.4f - t * 0.25f;   // 0.4 → 0.15
	tailMixGain_ = 0.25f + t * 1.05f;   // 0.25 → 1.3
	directEarlyGain_ = 0.2f - t * 0.1f; // 0.2 → 0.1 (more direct brightness at small, less at vast)

	// 4x undersample only in last zone (zone 7 = "Vast") for extended tails
	// C2: ~112ms effective, C3: ~362ms effective
	cascadeDoubleUndersample_ = !kDisableVastUndersample && (zone == 7);

	// Vast mode enhancements (last zone only)
	// Recompute cascade damping from base damping_ value to avoid compounding
	float baseCascadeDamping = 0.05f + (1.0f - damping_) * 0.6f;
	if (zone == 7) {
		// Softer cascade damping - let highs ring longer for lusher shimmer
		cascadeDamping_ = baseCascadeDamping * 0.5f;
		// Pitch modulation on C2/C3 for chorus-like smearing
		cascadeModDepth_ = 14.0f; // More wobble for lush shimmer
		// Amplitude modulation on C2/C3 for diffusion contour
		cascadeAmpMod_ = 0.25f; // Subtle balance shift between stages
		// Extra nested feedback to compensate for pre-AA filter energy loss
		cascadeNestFeedback_ = std::clamp(cascadeNestFeedbackBase_ + 0.25f, 0.0f, 0.55f);
	}
	else {
		cascadeDamping_ = baseCascadeDamping;
		cascadeModDepth_ = 0.0f;
		cascadeAmpMod_ = 0.0f;
		cascadeNestFeedback_ = std::clamp(cascadeNestFeedbackBase_, 0.0f, 0.55f);
	}

	cascadeLengths_[0] = static_cast<size_t>(kC0BaseLength * cascadeScale_);
	cascadeLengths_[1] = static_cast<size_t>(kC1BaseLength * cascadeScale_);
	cascadeLengths_[2] = static_cast<size_t>(kC2BaseLength * cascadeScale_);
	cascadeLengths_[3] = static_cast<size_t>(kC3BaseLength * cascadeScale_);

	// Clamp write positions
	for (size_t i = 0; i < kNumCascade; ++i) {
		if (cascadeWritePos_[i] >= cascadeLengths_[i]) {
			cascadeWritePos_[i] = 0;
		}
	}
}

// === Zone 3: Feedback pattern ===

static constexpr std::array<phi::PhiTriConfig, 3> kFeedback3TriBank = {{
    {phi::kPhi033, 0.6f, 0.00f, true},
    {phi::kPhi067, 0.6f, 0.33f, true},
    {phi::kPhi100, 0.6f, 0.66f, true},
}};

void Featherverb::setZone3(int32_t value) {
	zone3_ = value;
	updateFeedbackPattern();
}

void Featherverb::updateFeedbackPattern() {
	using namespace phi;

	const float yNorm = static_cast<float>(zone3_) / 1023.0f;
	const int32_t zone = zone3_ >> 7;
	const double gammaPhase = zone * 0.125;

	static constexpr std::array<std::array<float, 3>, 8> kZoneBias = {{{1.00f, 1.00f, 1.00f},
	                                                                   {1.05f, 1.00f, 0.95f},
	                                                                   {0.95f, 1.00f, 1.05f},
	                                                                   {1.02f, 0.96f, 1.02f},
	                                                                   {0.92f, 1.08f, 0.92f},
	                                                                   {1.06f, 0.94f, 1.00f},
	                                                                   {0.90f, 1.00f, 1.10f},
	                                                                   {1.08f, 1.00f, 0.92f}}};

	const PhiTriContext ctx{yNorm, 1.0f, 1.0f, gammaPhase};
	std::array<float, 3> mods = ctx.evalBank(kFeedback3TriBank);

	for (size_t i = 0; i < 3; ++i) {
		feedbackMult_[i] = std::clamp(kZoneBias[zone][i] + mods[i] * 0.15f, 0.75f, 1.25f);
	}

	// Cascade series mix - 10 periods for fine density control
	// Triangle wave: 10 complete cycles over yNorm 0→1
	// seriesMix=0 → C3 parallel (9 paths, sparse), seriesMix=1 → C3 series (16 paths, dense)
	float phase10 = yNorm * 10.0f;
	float tri10 = 1.0f - 4.0f * std::abs(phase10 - std::floor(phase10) - 0.5f);
	// Map -1..1 to 0.35..0.85 range (biased toward series/dense)
	cascadeSeriesMix_ = 0.6f + tri10 * 0.25f;

	// Cascade feedback - 7 periods, clockwise = more feedback
	// Base increases with yNorm (0.4 to 0.9), triangle adds ±0.1 texture
	float phase7 = yNorm * 7.0f;
	float tri7 = 1.0f - 4.0f * std::abs(phase7 - std::floor(phase7) - 0.5f);
	float baseFeedback = 0.4f + yNorm * 0.5f; // 0.4 to 0.9 as yNorm increases
	cascadeFeedbackMult_ = std::clamp(baseFeedback + tri7 * 0.1f, 0.3f, 1.0f);

	// Nested cascade feedback (C3→C0) - 5 periods, clockwise = more recirculation
	// Kicks in at higher yNorm values; scaled by room feedback in processing
	// Sets base value; Zone 2 adds vast boost on top
	float phase5 = yNorm * 5.0f;
	float tri5 = 1.0f - 4.0f * std::abs(phase5 - std::floor(phase5) - 0.5f);
	float baseNest = std::max(0.0f, (yNorm - 0.35f) * 0.5f); // 0 until yNorm>0.35, then 0 to 0.325
	cascadeNestFeedbackBase_ = std::clamp(baseNest + tri5 * 0.08f, 0.0f, 0.4f);
	// Recompute combined value (Zone 2 adds vast boost)
	updateSizes();

	// LFO pitch wobble depth - 13 periods (fast), adds subtle chorus/shimmer
	float phase13 = yNorm * 13.0f;
	float tri13 = 1.0f - 4.0f * std::abs(phase13 - std::floor(phase13) - 0.5f);
	modDepth_ = std::clamp(0.3f + tri13 * 0.25f, 0.0f, 0.6f); // 0.05 to 0.55 range

	// Width breathing - 11 periods (fast), expands stereo as signal decays
	// Mid/side: dynamicWidth > 1 = wider than normal, higher values = dramatic expansion
	float phase11 = yNorm * 11.0f;
	float tri11 = 1.0f - 4.0f * std::abs(phase11 - std::floor(phase11) - 0.5f);
	widthBreath_ = std::clamp(0.5f + tri11 * 0.4f, 0.0f, 1.2f); // 0.1 to 0.9 range

	// Cross-channel bleed - 9 periods, L↔R mixing in FDN feedback for stereo complexity
	// Subtle effect: 0.0 to 0.25 range adds correlation without smearing stereo image
	float phase9 = yNorm * 9.0f;
	float tri9 = 1.0f - 4.0f * std::abs(phase9 - std::floor(phase9) - 0.5f);
	float baseBleed = yNorm * 0.15f; // Increases with zone position
	crossBleed_ = std::clamp(baseBleed + tri9 * 0.1f, 0.0f, 0.25f);

	// Per-stage cascade allpass coefficients - higher coeffs = more diffusion (less slapback)
	// Each stage uses different triangle period for variety
	// Base increases with yNorm: 0.3 at low (more transient) → 0.55 at high (more smear)
	float coeffBase = 0.3f + yNorm * 0.25f;

	// C0: 8 periods - fastest stage, moderate variation
	float phase8 = yNorm * 8.0f;
	float tri8 = 1.0f - 4.0f * std::abs(phase8 - std::floor(phase8) - 0.5f);
	cascadeCoeffs_[0] = std::clamp(coeffBase + tri8 * 0.08f, 0.25f, 0.6f);

	// C1: 6 periods - slightly more variation
	float phase6 = yNorm * 6.0f;
	float tri6 = 1.0f - 4.0f * std::abs(phase6 - std::floor(phase6) - 0.5f);
	cascadeCoeffs_[1] = std::clamp(coeffBase + tri6 * 0.1f, 0.25f, 0.6f);

	// C2: 4 periods - slower variation, slightly higher base for density
	float phase4 = yNorm * 4.0f;
	float tri4 = 1.0f - 4.0f * std::abs(phase4 - std::floor(phase4) - 0.5f);
	cascadeCoeffs_[2] = std::clamp(coeffBase + 0.05f + tri4 * 0.1f, 0.28f, 0.65f);

	// C3: 3 periods - longest stage, highest base for maximum diffusion
	float phase3 = yNorm * 3.0f;
	float tri3 = 1.0f - 4.0f * std::abs(phase3 - std::floor(phase3) - 0.5f);
	cascadeCoeffs_[3] = std::clamp(coeffBase + 0.08f + tri3 * 0.12f, 0.3f, 0.7f);
}

} // namespace deluge::dsp::reverb
