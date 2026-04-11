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
#include <cstdint>
#include <span>

class Serializer;
class Deserializer;

namespace deluge::dsp {

// ============================================================================
// Constants
// ============================================================================

inline constexpr uint8_t kUtilityDefault = 64;

// Volume lookup: knob 0..127 → Q31 gain in Q2.29 format (multiply then <<2)
// 0 = silence, 64 = unity (0dB), 127 = +12dB (4x)
// Gain curve: 0→0, 64→1.0, 127→4.0 (linear interpolation in two segments)
//
// For gains > 1.0 we work in Q2.29 to avoid overflow:
//   multiply_32x32_rshift32(sample, gainQ29) << 2 gives correct result
// Unity (1.0) in Q2.29 = ONE_Q31 >> 2 = 0x1FFFFFFF

inline constexpr q31_t kUtilityUnityQ29 = ONE_Q31 >> 2; // 1.0 in Q2.29

// ============================================================================
// UtilityParams
// ============================================================================

struct UtilityParams {
	uint8_t volume{kUtilityDefault}; // 0=off(-inf), 64=unity(0dB), 127=+12dB
	uint8_t panL{kUtilityDefault};   // 0=silence, 64=center, 127=hard right
	uint8_t panR{kUtilityDefault};   // 0=silence, 64=center, 127=hard left
	uint8_t width{kUtilityDefault};  // 0=mono(0%), 64=normal(100%), 127=wide(~200%)

	[[nodiscard]] bool isEnabled() const {
		return volume != kUtilityDefault || panL != kUtilityDefault || panR != kUtilityDefault
		       || width != kUtilityDefault;
	}

	/// Apply utility processing to a stereo buffer in-place.
	/// When all params are at default, this is a no-op (early return).
	void render(std::span<StereoSample> buffer) const {
		if (!isEnabled()) {
			return;
		}

		// Pre-compute volume gain in Q2.29 format
		// Segment 1: knob 0..64 → gain 0..1.0
		// Segment 2: knob 64..127 → gain 1.0..4.0
		q31_t volGainQ29;
		if (volume == 0) {
			volGainQ29 = 0;
		}
		else if (volume <= kUtilityDefault) {
			// Linear ramp: 0→0, 64→unity
			// gain = (volume / 64) in Q2.29
			volGainQ29 = static_cast<q31_t>((static_cast<int64_t>(volume) * kUtilityUnityQ29) / kUtilityDefault);
		}
		else {
			// Linear ramp: 64→unity, 127→4x
			// gain = 1.0 + (volume - 64) * 3.0 / 63 in Q2.29
			// 3.0 in Q2.29 = 3 * kUtilityUnityQ29
			int32_t above = volume - kUtilityDefault;
			int64_t threeQ29 = static_cast<int64_t>(kUtilityUnityQ29) * 3;
			volGainQ29 = kUtilityUnityQ29 + static_cast<q31_t>((above * threeQ29) / 63);
		}

		// Pre-compute width factor in Q31
		// knob 0→0.0, 64→1.0(ONE_Q31), 127→2.0
		// For >1.0 we use Q1.30: widthQ30 where 1.0 = ONE_Q31>>1
		// Actually simpler: work in Q1.30 throughout so 2.0 is representable
		// Q1.30: 1.0 = 0x40000000, 2.0 = 0x7FFFFFFF (clamped to max)
		q31_t widthQ30;
		if (width <= kUtilityDefault) {
			// 0→0, 64→0x40000000
			widthQ30 = static_cast<q31_t>((static_cast<int64_t>(width) * (ONE_Q31 >> 1)) / kUtilityDefault);
		}
		else {
			// 64→0x40000000, 127→0x7FFFFFFF (~2.0)
			int32_t above = width - kUtilityDefault;
			q31_t halfQ31 = ONE_Q31 >> 1; // 0x40000000 = 1.0 in Q1.30
			widthQ30 = halfQ31 + static_cast<q31_t>((static_cast<int64_t>(above) * halfQ31) / 63);
		}

		bool needWidth = (width != kUtilityDefault);
		bool needPan = (panL != kUtilityDefault || panR != kUtilityDefault);
		bool needVolume = (volume != kUtilityDefault);

		// Pre-compute pan gains in Q31
		// panL: controls left channel placement. 64=keep left, 0=silence left, 127=move left to right
		// panR: controls right channel placement. 64=keep right, 0=silence right, 127=move right to left
		// Each pan knob produces two gains: how much stays in-channel vs crosses over
		q31_t panL_stayL, panL_toR; // Left input: stays in L, crosses to R
		q31_t panR_stayR, panR_toL; // Right input: stays in R, crosses to L

		if (needPan) {
			if (panL <= kUtilityDefault) {
				// 0=silence, 64=full left (L stays, nothing to R)
				panL_stayL = static_cast<q31_t>((static_cast<int64_t>(panL) * ONE_Q31) / kUtilityDefault);
				panL_toR = 0;
			}
			else {
				// 64=full left, 127=full right (L fades out, R fades in)
				int32_t above = panL - kUtilityDefault;
				panL_stayL =
				    ONE_Q31 - static_cast<q31_t>((static_cast<int64_t>(above) * ONE_Q31) / (127 - kUtilityDefault));
				panL_toR = static_cast<q31_t>((static_cast<int64_t>(above) * ONE_Q31) / (127 - kUtilityDefault));
			}

			if (panR <= kUtilityDefault) {
				// 0=silence, 64=full right (R stays, nothing to L)
				panR_stayR = static_cast<q31_t>((static_cast<int64_t>(panR) * ONE_Q31) / kUtilityDefault);
				panR_toL = 0;
			}
			else {
				// 64=full right, 127=full left (R fades out, L fades in)
				int32_t above = panR - kUtilityDefault;
				panR_stayR =
				    ONE_Q31 - static_cast<q31_t>((static_cast<int64_t>(above) * ONE_Q31) / (127 - kUtilityDefault));
				panR_toL = static_cast<q31_t>((static_cast<int64_t>(above) * ONE_Q31) / (127 - kUtilityDefault));
			}
		}
		else {
			panL_stayL = ONE_Q31;
			panL_toR = 0;
			panR_stayR = ONE_Q31;
			panR_toL = 0;
		}

		for (auto& sample : buffer) {
			q31_t left = sample.l;
			q31_t right = sample.r;

			// Width processing (mid/side)
			if (needWidth) {
				// mid = (L + R) / 2, side = (L - R) / 2
				q31_t mid = (left >> 1) + (right >> 1);
				q31_t side = (left >> 1) - (right >> 1);

				// Apply width to side in Q1.30: side * widthQ30
				// multiply_32x32_rshift32(side, widthQ30) = side * widthQ30 / 2^32
				// widthQ30 is Q1.30, so 1.0 = 0x40000000 = 2^30
				// To recover side when width=1.0: side * 2^30 / 2^32 = side/4, need <<2
				q31_t scaledSide = multiply_32x32_rshift32(side, widthQ30) << 2;

				// Reconstruct: L = mid + scaledSide, R = mid - scaledSide
				left = add_saturate(mid, scaledSide);
				right = subtract_saturate(mid, scaledSide);
			}

			// Pan processing
			if (needPan) {
				// Apply pan: each input channel distributes to both outputs
				q31_t newL = add_saturate(multiply_32x32_rshift32(left, panL_stayL) << 1,
				                          multiply_32x32_rshift32(right, panR_toL) << 1);
				q31_t newR = add_saturate(multiply_32x32_rshift32(right, panR_stayR) << 1,
				                          multiply_32x32_rshift32(left, panL_toR) << 1);
				left = newL;
				right = newR;
			}

			// Volume processing (Q2.29 gain, apply with <<2 correction)
			if (needVolume) {
				left = multiply_32x32_rshift32(left, volGainQ29) << 2;
				right = multiply_32x32_rshift32(right, volGainQ29) << 2;
			}

			sample.l = left;
			sample.r = right;
		}
	}

	void writeToFile(Serializer& writer) const {
		WRITE_FIELD_DEFAULT(writer, volume, "utilityVolume", kUtilityDefault);
		WRITE_FIELD_DEFAULT(writer, panL, "utilityPanL", kUtilityDefault);
		WRITE_FIELD_DEFAULT(writer, panR, "utilityPanR", kUtilityDefault);
		WRITE_FIELD_DEFAULT(writer, width, "utilityWidth", kUtilityDefault);
	}

	bool readTag(Deserializer& reader, const char* tagName) {
		READ_FIELD(reader, tagName, volume, "utilityVolume");
		READ_FIELD(reader, tagName, panL, "utilityPanL");
		READ_FIELD(reader, tagName, panR, "utilityPanR");
		READ_FIELD(reader, tagName, width, "utilityWidth");
		return false;
	}
};

} // namespace deluge::dsp
