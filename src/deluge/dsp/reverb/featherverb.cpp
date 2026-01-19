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

// Pi constant for angle calculations
static constexpr float kPi = 3.14159265358979323846f;
static constexpr float kTwoPi = 2.0f * kPi;

using namespace deluge::dsp;

// Note: D2/D3 min/max lengths are defined in featherverb.hpp

Featherverb::Featherverb() {
	// Compute buffer offsets for contiguous layout
	size_t offset = 0;
	for (size_t i = 0; i < kNumDelays; ++i) {
		delayOffsets_[i] = offset;
		offset += kDelayLengths[i];
	}

	// Initialize default parameters
	setRoomSize(0.5f);
	setDamping(0.5f);

	// Initialize zone parameters to sensible defaults
	updateMatrix();
	updateDelayLengths();
	updateFeedbackPattern();
}

bool Featherverb::allocate() {
	if (buffer_ != nullptr) {
		return true; // Already allocated
	}

	buffer_ = static_cast<float*>(
	    GeneralMemoryAllocator::get().regions[MEMORY_REGION_STEALABLE].alloc(kBufferBytes, false, nullptr));

	if (buffer_ == nullptr) {
		return false;
	}

	// Zero the buffer
	std::memset(buffer_, 0, kBufferBytes);

	// Reset write positions and filter states
	writePos_.fill(0);
	lpState_.fill(0.0f);
	hpState_ = 0.0f;
	lpStateL_ = 0.0f;
	lpStateR_ = 0.0f;

	return true;
}

void Featherverb::deallocate() {
	if (buffer_ != nullptr) {
		delugeDealloc(buffer_);
		buffer_ = nullptr;
	}
}

void Featherverb::process(std::span<int32_t> input, StereoBuffer<q31_t> output) {
	// Safety check
	if (buffer_ == nullptr) {
		return;
	}

	constexpr float kInputScale = 1.0f / static_cast<float>(std::numeric_limits<int32_t>::max());
	// Match Mutable's output level
	constexpr float kOutputScale = static_cast<float>(std::numeric_limits<int32_t>::max()) * 16.0f;

	// HPF coefficient for input (removes DC)
	const float hpCoeff = 0.995f - hpCutoff_ * 0.09f;

	// Output LPF coefficient
	const float outLpCoeff = 0.1f + lpCutoff_ * 0.85f;

	// Cache matrix locally for faster access
	const auto& m = matrix_;

	for (size_t frame = 0; frame < input.size(); ++frame) {
		// Convert input to float and apply HPF
		float in = static_cast<float>(input[frame]) * kInputScale;
		float hpOut = in - hpState_;
		hpState_ += (1.0f - hpCoeff) * hpOut;
		in = hpOut;

		// Read from all delay lines
		float d0 = read(0);
		float d1 = read(1);
		float d2 = read(2);
		float d3 = read(3);

		// Apply 4x4 mixing matrix (phi-rotated orthogonal matrix)
		float h0 = m[0][0] * d0 + m[0][1] * d1 + m[0][2] * d2 + m[0][3] * d3;
		float h1 = m[1][0] * d0 + m[1][1] * d1 + m[1][2] * d2 + m[1][3] * d3;
		float h2 = m[2][0] * d0 + m[2][1] * d1 + m[2][2] * d2 + m[2][3] * d3;
		float h3 = m[3][0] * d0 + m[3][1] * d1 + m[3][2] * d2 + m[3][3] * d3;

		// Apply feedback with damping (lowpass in feedback path)
		// Near delays (D0-D1): brighter early reflections
		// Far delays (D2-D3): darker tail (scaled by room size)
		// Per-delay feedback multipliers add character variation
		h0 = onepole(h0, lpState_[0], dampCoeffNear_) * feedback_ * feedbackMult_[0];
		h1 = onepole(h1, lpState_[1], dampCoeffNear_) * feedback_ * feedbackMult_[1];
		h2 = onepole(h2, lpState_[2], dampCoeffFar_) * feedback_ * feedbackMult_[2];
		h3 = onepole(h3, lpState_[3], dampCoeffFar_) * feedback_ * feedbackMult_[3];

		// Inject input into first delay line
		h0 += in;

		// Write back to delay lines
		write(0, h0);
		write(1, h1);
		write(2, h2);
		write(3, h3);

		// Extra taps from D3 for increased density (early reflections)
		float tap1 = readAt(3, kTap1Offset);
		float tap2 = readAt(3, kTap2Offset);

		// Output mix: base taps + density taps cross-mixed for stereo width
		// Base: d0+d2 (L), d1+d3 (R) with width control
		// Density: tap1/tap2 cross-mixed at lower levels
		float outL = d0 + d2 * width_ + tap1 * 0.25f + tap2 * 0.12f;
		float outR = d1 + d3 * width_ + tap2 * 0.25f + tap1 * 0.12f;

		// Apply output lowpass
		outL = onepole(outL, lpStateL_, outLpCoeff);
		outR = onepole(outR, lpStateR_, outLpCoeff);

		// Convert to fixed point and mix into output
		int32_t outLq31 = static_cast<int32_t>(outL * kOutputScale);
		int32_t outRq31 = static_cast<int32_t>(outR * kOutputScale);

		output[frame].l += multiply_32x32_rshift32_rounded(outLq31, getPanLeft());
		output[frame].r += multiply_32x32_rshift32_rounded(outRq31, getPanRight());
	}
}

void Featherverb::setRoomSize(float value) {
	roomSize_ = value;
	// Map 0-1 to feedback range ~0.75-0.995 for natural decay
	// Higher range gives longer sustain at max settings
	feedback_ = 0.75f + value * 0.245f;
}

void Featherverb::setDamping(float value) {
	damping_ = value;
	// Map 0-1 to LP coefficient (higher coeff = less damping = brighter)
	// Inverted: high damping value = more filtering = darker sound
	dampCoeffNear_ = 0.1f + (1.0f - value) * 0.85f;

	// Far damping is darker - will be further scaled by room size in updateDelayLengths()
	// Base: 20% darker than near (multiply coeff by 0.8)
	dampCoeffFar_ = dampCoeffNear_ * 0.8f;
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

// === Zone 1: Matrix generation using 16 phi triangles ===
// 16 phi triangles generate raw matrix values, then Gram-Schmidt orthonormalization
// produces a valid orthogonal mixing matrix. This gives maximum flexibility.

// Phi triangle bank for 16 matrix entries (4 rows × 4 cols)
// Each entry has unique φ^n frequency for non-repeating evolution
// Phase offsets spread entries evenly
static constexpr std::array<phi::PhiTriConfig, 16> kMatrixTriBank = {{
    // Row 0
    {phi::kPhi025, 0.7f, 0.000f, true},  // m[0][0]
    {phi::kPhi050, 0.7f, 0.0625f, true}, // m[0][1]
    {phi::kPhi075, 0.7f, 0.125f, true},  // m[0][2]
    {phi::kPhi100, 0.7f, 0.1875f, true}, // m[0][3]
    // Row 1
    {phi::kPhi125, 0.7f, 0.250f, true},  // m[1][0]
    {phi::kPhi150, 0.7f, 0.3125f, true}, // m[1][1]
    {phi::kPhi175, 0.7f, 0.375f, true},  // m[1][2]
    {phi::kPhi200, 0.7f, 0.4375f, true}, // m[1][3]
    // Row 2
    {phi::kPhi033, 0.7f, 0.500f, true},   // m[2][0]
    {phi::kPhi067, 0.7f, 0.5625f, true},  // m[2][1]
    {phi::kPhiN025, 0.7f, 0.625f, true},  // m[2][2]
    {phi::kPhiN050, 0.7f, 0.6875f, true}, // m[2][3]
    // Row 3
    {phi::kPhi225, 0.7f, 0.750f, true},  // m[3][0]
    {phi::kPhi250, 0.7f, 0.8125f, true}, // m[3][1]
    {phi::kPhi275, 0.7f, 0.875f, true},  // m[3][2]
    {phi::kPhi300, 0.7f, 0.9375f, true}, // m[3][3]
}};

void Featherverb::setZone1(int32_t value) {
	zone1_ = value;
	updateMatrix();
}

/// Gram-Schmidt orthonormalization of 4x4 matrix (in-place)
/// Returns false if matrix is degenerate (shouldn't happen with phi triangles)
static bool gramSchmidt4x4(std::array<std::array<float, 4>, 4>& m) {
	// Process each column
	for (int col = 0; col < 4; ++col) {
		// Subtract projections onto previous columns
		for (int prev = 0; prev < col; ++prev) {
			// Compute dot product with previous column
			float dot = 0.0f;
			for (int row = 0; row < 4; ++row) {
				dot += m[row][col] * m[row][prev];
			}
			// Subtract projection
			for (int row = 0; row < 4; ++row) {
				m[row][col] -= dot * m[row][prev];
			}
		}

		// Normalize this column
		float norm = 0.0f;
		for (int row = 0; row < 4; ++row) {
			norm += m[row][col] * m[row][col];
		}
		norm = std::sqrt(norm);
		if (norm < 0.0001f) {
			return false; // Degenerate matrix
		}
		for (int row = 0; row < 4; ++row) {
			m[row][col] /= norm;
		}
	}
	return true;
}

void Featherverb::updateMatrix() {
	using namespace phi;

	// Map zone1 to yNorm and gammaPhase for phi triangle evaluation
	const float yNorm = static_cast<float>(zone1_) / 1023.0f;
	const int32_t zone = zone1_ >> 7;       // 0-7
	const double gammaPhase = zone * 0.125; // Each zone shifts phase by 1/8

	// Create phi triangle context
	const PhiTriContext ctx{yNorm, 1.0f, 1.0f, gammaPhase};

	// Evaluate all 16 phi triangles
	std::array<float, 16> vals = ctx.evalBank(kMatrixTriBank);

	// Start with Hadamard as base (well-conditioned, no degenerate risk)
	// Then add phi triangle modulation
	static constexpr std::array<std::array<float, 4>, 4> kHadamard = {
	    {{1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, -1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f, 1.0f}}};

	// Blend factor: how much phi triangles influence the matrix
	// Zone determines blend intensity
	static constexpr std::array<float, 8> kZoneBlend = {
	    0.0f,  // Zone 0: Pure Hadamard
	    0.15f, // Zone 1: Subtle variation
	    0.3f,  // Zone 2: Moderate
	    0.45f, // Zone 3: More varied
	    0.6f,  // Zone 4: Significant
	    0.75f, // Zone 5: Strong
	    0.85f, // Zone 6: Very morphed
	    0.95f  // Zone 7: Nearly full phi control
	};

	const float blend = kZoneBlend[zone];

	// Build matrix: Hadamard + blend * phi_triangles
	for (int row = 0; row < 4; ++row) {
		for (int col = 0; col < 4; ++col) {
			float base = kHadamard[row][col];
			float mod = vals[row * 4 + col]; // -1 to +1 from bipolar phi triangle
			matrix_[row][col] = base + blend * mod * 0.5f;
		}
	}

	// Orthonormalize to ensure energy preservation
	// After Gram-Schmidt, matrix is already orthonormal (no additional scaling needed)
	if (!gramSchmidt4x4(matrix_)) {
		// Fallback to normalized Hadamard if degenerate (shouldn't happen)
		for (int row = 0; row < 4; ++row) {
			for (int col = 0; col < 4; ++col) {
				matrix_[row][col] = kHadamard[row][col] * 0.5f;
			}
		}
	}
}

// === Zone 2: Size control (D2 + D3) ===
// D2 scales slowly (texture), D3 scales fast (tail)
// At zone2=0 (Tiny):  D2=650 (~15ms), D3=1297 (~29ms) - tight room
// At zone2=1023 (Vast): D2=1039 (~24ms), D3=6553 (~148ms) - cathedral

void Featherverb::setZone2(int32_t value) {
	zone2_ = value;
	updateDelayLengths();
}

void Featherverb::updateDelayLengths() {
	const float t = static_cast<float>(zone2_) / 1023.0f;

	// D2: scales 1.6x (650 → 1039)
	actualDelayLengths_[2] = kD2MinLength + static_cast<size_t>(t * (kD2MaxLength - kD2MinLength));

	// D3: scales 5x (1297 → 6553)
	actualDelayLengths_[3] = kD3MinLength + static_cast<size_t>(t * (kD3MaxLength - kD3MinLength));

	// Scale far damping based on room size
	// Tiny room: far = near * 0.8 (subtle difference)
	// Vast room: far = near * 0.5 (40% darker tail)
	// This models increased HF absorption in larger spaces
	const float farDampScale = 0.8f - t * 0.3f; // 0.8 at tiny → 0.5 at vast
	dampCoeffFar_ = dampCoeffNear_ * farDampScale;

	// Clamp write positions if they exceed new lengths
	if (writePos_[2] >= actualDelayLengths_[2]) {
		writePos_[2] = 0;
	}
	if (writePos_[3] >= actualDelayLengths_[3]) {
		writePos_[3] = 0;
	}
}

// === Zone 3: Feedback pattern control using phi triangles ===
// 4 phi triangles modulate per-delay feedback multipliers.
// Creates different decay characters that evolve smoothly.

// Phi triangle bank for 4 feedback multipliers
static constexpr std::array<phi::PhiTriConfig, 4> kFeedbackTriBank = {{
    {phi::kPhi033, 0.6f, 0.00f, true}, // D0 feedback - slow, bipolar
    {phi::kPhi067, 0.6f, 0.25f, true}, // D1 feedback
    {phi::kPhi100, 0.6f, 0.50f, true}, // D2 feedback
    {phi::kPhi125, 0.6f, 0.75f, true}, // D3 feedback - fastest
}};

void Featherverb::setZone3(int32_t value) {
	zone3_ = value;
	updateFeedbackPattern();
}

void Featherverb::updateFeedbackPattern() {
	using namespace phi;

	// Map zone3 to yNorm and gammaPhase
	const float yNorm = static_cast<float>(zone3_) / 1023.0f;
	const int32_t zone = zone3_ >> 7; // 0-7
	const double gammaPhase = zone * 0.125;

	// Zone-specific character: base multiplier patterns
	// Each zone emphasizes different decay characteristics
	static constexpr std::array<std::array<float, 4>, 8> kZoneBias = {{
	    {1.00f, 1.00f, 1.00f, 1.00f}, // Zone 0: Balanced
	    {1.05f, 1.00f, 0.98f, 0.95f}, // Zone 1: Front-heavy
	    {0.95f, 0.98f, 1.00f, 1.05f}, // Zone 2: Tail-heavy
	    {1.02f, 0.95f, 1.02f, 0.95f}, // Zone 3: Alternating
	    {0.90f, 1.05f, 1.05f, 0.90f}, // Zone 4: Scooped
	    {1.05f, 0.90f, 0.90f, 1.05f}, // Zone 5: Humped
	    {0.88f, 1.00f, 1.00f, 1.08f}, // Zone 6: Sparse early
	    {1.08f, 1.00f, 1.00f, 0.88f}  // Zone 7: Dense early
	}};

	// Create phi triangle context with smaller range (±0.15)
	const PhiTriContext ctx{yNorm, 1.0f, 1.0f, gammaPhase};

	// Evaluate phi triangles for modulation
	std::array<float, 4> mods = ctx.evalBank(kFeedbackTriBank);

	// Combine zone bias with phi triangle modulation
	// Modulation range: ±0.15 around zone bias
	for (size_t i = 0; i < 4; ++i) {
		feedbackMult_[i] = kZoneBias[zone][i] + mods[i] * 0.15f;
		// Clamp to safe range [0.75, 1.25]
		feedbackMult_[i] = std::clamp(feedbackMult_[i], 0.75f, 1.25f);
	}
}

} // namespace deluge::dsp::reverb
