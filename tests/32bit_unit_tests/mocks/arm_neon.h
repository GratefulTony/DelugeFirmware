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
 */
#pragma once

// Scalar stand-in for the slice of ARM NEON used by the DSP headers, so x86 host test builds can
// compile translation units that reach them (several structs carry NEON-typed members, so the types
// must exist - #ifdef guards around the includes alone can't fix that). Semantics follow the ARM
// intrinsic definitions but nothing here is performance-relevant; the ARM/QEMU test build uses the
// real <arm_neon.h> from the cross toolchain. Only intrinsics actually used in the codebase are
// provided - extend as needed when a host build reports a missing one.

#include <algorithm>
#include <cstdint>
#include <cstring>

struct int32x2_t {
	int32_t v[2];
};
struct int32x4_t {
	int32_t v[4];
};
struct uint32x2_t {
	uint32_t v[2];
};
struct uint32x4_t {
	uint32_t v[4];
};
struct float32x2_t {
	float v[2];
};
struct float32x4_t {
	float v[4];
};

namespace neon_mock {
inline int32_t satAdd(int64_t a, int64_t b) {
	return static_cast<int32_t>(std::clamp<int64_t>(a + b, INT32_MIN, INT32_MAX));
}
inline int32_t satSub(int64_t a, int64_t b) {
	return static_cast<int32_t>(std::clamp<int64_t>(a - b, INT32_MIN, INT32_MAX));
}
inline int32_t satQ31(int64_t x) {
	return static_cast<int32_t>(std::clamp<int64_t>(x, INT32_MIN, INT32_MAX));
}
// Rounding doubling multiply-high: sat((2*a*b + (1<<30)) >> 31)
inline int32_t qrdmulh(int32_t a, int32_t b) {
	return satQ31((2 * static_cast<int64_t>(a) * b + (int64_t{1} << 30)) >> 31);
}
// Doubling multiply-high (no rounding): sat((2*a*b) >> 31)
inline int32_t qdmulh(int32_t a, int32_t b) {
	return satQ31((2 * static_cast<int64_t>(a) * b) >> 31);
}
inline int32_t shl(int32_t a, int32_t shift) { // negative shift = right shift, NEON vshl semantics
	if (shift >= 0) {
		return static_cast<int32_t>(static_cast<uint32_t>(a) << (shift & 31));
	}
	return a >> std::min(-shift, 31);
}
inline int32_t qshl(int32_t a, int32_t shift) {
	if (shift <= 0) {
		return a >> std::min(-shift, 31);
	}
	return satQ31(static_cast<int64_t>(a) << std::min(shift, 63));
}
} // namespace neon_mock

// clang-format off

// --- Duplicate / load / store / lane ---
inline int32x2_t vdup_n_s32(int32_t x) { return {x, x}; }
inline int32x4_t vdupq_n_s32(int32_t x) { return {x, x, x, x}; }
inline float32x2_t vdup_n_f32(float x) { return {x, x}; }
inline float32x4_t vdupq_n_f32(float x) { return {x, x, x, x}; }
inline int32x2_t vld1_s32(int32_t const* p) { return {p[0], p[1]}; }
inline int32x4_t vld1q_s32(int32_t const* p) { return {p[0], p[1], p[2], p[3]}; }
inline float32x4_t vld1q_f32(float const* p) { return {p[0], p[1], p[2], p[3]}; }
inline void vst1_s32(int32_t* p, int32x2_t a) { p[0] = a.v[0]; p[1] = a.v[1]; }
inline void vst1q_s32(int32_t* p, int32x4_t a) { for (int i = 0; i < 4; i++) { p[i] = a.v[i]; } }
inline void vst1q_f32(float* p, float32x4_t a) { for (int i = 0; i < 4; i++) { p[i] = a.v[i]; } }
inline void vst1q_lane_s32(int32_t* p, int32x4_t a, int lane) { *p = a.v[lane]; }
inline int32_t vget_lane_s32(int32x2_t a, int lane) { return a.v[lane]; }
inline uint32_t vget_lane_u32(uint32x2_t a, int lane) { return a.v[lane]; }
inline float vget_lane_f32(float32x2_t a, int lane) { return a.v[lane]; }
inline int32_t vgetq_lane_s32(int32x4_t a, int lane) { return a.v[lane]; }
inline float vgetq_lane_f32(float32x4_t a, int lane) { return a.v[lane]; }
inline int32x2_t vset_lane_s32(int32_t x, int32x2_t a, int lane) { a.v[lane] = x; return a; }
inline int32x2_t vget_low_s32(int32x4_t a) { return {a.v[0], a.v[1]}; }
inline int32x2_t vget_high_s32(int32x4_t a) { return {a.v[2], a.v[3]}; }
inline uint32x2_t vget_low_u32(uint32x4_t a) { return {a.v[0], a.v[1]}; }
inline uint32x2_t vget_high_u32(uint32x4_t a) { return {a.v[2], a.v[3]}; }

// --- Integer arithmetic ---
inline int32x2_t vadd_s32(int32x2_t a, int32x2_t b) { return {a.v[0] + b.v[0], a.v[1] + b.v[1]}; }
inline int32x4_t vaddq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] + b.v[i]; } return r; }
inline int32x2_t vsub_s32(int32x2_t a, int32x2_t b) { return {a.v[0] - b.v[0], a.v[1] - b.v[1]}; }
inline int32x4_t vsubq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] - b.v[i]; } return r; }
inline int32x2_t vqadd_s32(int32x2_t a, int32x2_t b) { return {neon_mock::satAdd(a.v[0], b.v[0]), neon_mock::satAdd(a.v[1], b.v[1])}; }
inline int32x4_t vqaddq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = neon_mock::satAdd(a.v[i], b.v[i]); } return r; }
inline int32x2_t vqsub_s32(int32x2_t a, int32x2_t b) { return {neon_mock::satSub(a.v[0], b.v[0]), neon_mock::satSub(a.v[1], b.v[1])}; }
inline int32x4_t vqsubq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = neon_mock::satSub(a.v[i], b.v[i]); } return r; }
inline int32x2_t vhadd_s32(int32x2_t a, int32x2_t b) { return {static_cast<int32_t>((static_cast<int64_t>(a.v[0]) + b.v[0]) >> 1), static_cast<int32_t>((static_cast<int64_t>(a.v[1]) + b.v[1]) >> 1)}; }
inline int32x4_t vhaddq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = static_cast<int32_t>((static_cast<int64_t>(a.v[i]) + b.v[i]) >> 1); } return r; }
inline int32x2_t vhsub_s32(int32x2_t a, int32x2_t b) { return {static_cast<int32_t>((static_cast<int64_t>(a.v[0]) - b.v[0]) >> 1), static_cast<int32_t>((static_cast<int64_t>(a.v[1]) - b.v[1]) >> 1)}; }
inline int32x4_t vhsubq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = static_cast<int32_t>((static_cast<int64_t>(a.v[i]) - b.v[i]) >> 1); } return r; }
inline int32x2_t vqrdmulh_s32(int32x2_t a, int32x2_t b) { return {neon_mock::qrdmulh(a.v[0], b.v[0]), neon_mock::qrdmulh(a.v[1], b.v[1])}; }
inline int32x4_t vqrdmulhq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = neon_mock::qrdmulh(a.v[i], b.v[i]); } return r; }
inline int32x4_t vqdmulhq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = neon_mock::qdmulh(a.v[i], b.v[i]); } return r; }
inline int32x4_t vabsq_s32(int32x4_t a) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] == INT32_MIN ? INT32_MIN : (a.v[i] < 0 ? -a.v[i] : a.v[i]); } return r; }
inline int32x2_t vmax_s32(int32x2_t a, int32x2_t b) { return {std::max(a.v[0], b.v[0]), std::max(a.v[1], b.v[1])}; }
inline int32x4_t vmaxq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = std::max(a.v[i], b.v[i]); } return r; }
inline int32x4_t vminq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = std::min(a.v[i], b.v[i]); } return r; }
inline int32x2_t vpmax_s32(int32x2_t a, int32x2_t b) { return {std::max(a.v[0], a.v[1]), std::max(b.v[0], b.v[1])}; }

// --- Shifts ---
inline int32x2_t vshl_n_s32(int32x2_t a, int n) { return {neon_mock::shl(a.v[0], n), neon_mock::shl(a.v[1], n)}; }
inline int32x4_t vshlq_n_s32(int32x4_t a, int n) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = neon_mock::shl(a.v[i], n); } return r; }
inline int32x4_t vshrq_n_s32(int32x4_t a, int n) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] >> n; } return r; }
inline int32x4_t vshlq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = neon_mock::shl(a.v[i], b.v[i]); } return r; }
inline int32x4_t vqshlq_s32(int32x4_t a, int32x4_t b) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = neon_mock::qshl(a.v[i], b.v[i]); } return r; }

// --- Float arithmetic ---
inline float32x2_t vmul_f32(float32x2_t a, float32x2_t b) { return {a.v[0] * b.v[0], a.v[1] * b.v[1]}; }
inline float32x4_t vmulq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] * b.v[i]; } return r; }
inline float32x4_t vmulq_n_f32(float32x4_t a, float b) { float32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] * b; } return r; }
inline float32x4_t vaddq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] + b.v[i]; } return r; }
inline float32x4_t vsubq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] - b.v[i]; } return r; }
inline float32x4_t vmlaq_f32(float32x4_t a, float32x4_t b, float32x4_t c) { float32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] + b.v[i] * c.v[i]; } return r; }
inline float32x4_t vnegq_f32(float32x4_t a) { float32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = -a.v[i]; } return r; }
inline float32x4_t vabdq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) { float d = a.v[i] - b.v[i]; r.v[i] = d < 0 ? -d : d; } return r; }

// --- Float reciprocal estimate / step (Newton-Raphson) ---
inline float32x2_t vrecpe_f32(float32x2_t a) { return {1.0f / a.v[0], 1.0f / a.v[1]}; }
inline float32x4_t vrecpeq_f32(float32x4_t a) { float32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = 1.0f / a.v[i]; } return r; }
inline float32x2_t vrecps_f32(float32x2_t a, float32x2_t b) { return {2.0f - a.v[0] * b.v[0], 2.0f - a.v[1] * b.v[1]}; }
inline float32x4_t vrecpsq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = 2.0f - a.v[i] * b.v[i]; } return r; }

// --- Conversions / reinterpret / compare / select / bitwise ---
inline float32x4_t vcvtq_f32_s32(int32x4_t a) { float32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = static_cast<float>(a.v[i]); } return r; }
inline int32x4_t vcvtq_s32_f32(float32x4_t a) { int32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = static_cast<int32_t>(a.v[i]); } return r; }
inline int32x4_t vreinterpretq_s32_u32(uint32x4_t a) { int32x4_t r; std::memcpy(r.v, a.v, sizeof(r.v)); return r; }
inline uint32x4_t vcltq_f32(float32x4_t a, float32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) { r.v[i] = a.v[i] < b.v[i] ? 0xFFFFFFFFu : 0u; } return r; }
inline uint32x2_t vand_u32(uint32x2_t a, uint32x2_t b) { return {a.v[0] & b.v[0], a.v[1] & b.v[1]}; }
inline float32x4_t vbslq_f32(uint32x4_t mask, float32x4_t a, float32x4_t b) {
	float32x4_t r;
	for (int i = 0; i < 4; i++) {
		uint32_t ua;
		uint32_t ub;
		std::memcpy(&ua, &a.v[i], 4);
		std::memcpy(&ub, &b.v[i], 4);
		uint32_t ur = (ua & mask.v[i]) | (ub & ~mask.v[i]);
		std::memcpy(&r.v[i], &ur, 4);
	}
	return r;
}

// clang-format on
