#pragma once

#include "util/lookuptables/lookuptables.h"

#define CONG (jcong = 69069 * jcong + 1234567)

extern uint32_t z, w, jcong;

/* Basic waveform generation functions.
 *
 * Extracted from functions.h.
 */

// input must not have any extra bits set than numBitsInInput specifies
[[gnu::always_inline]] inline int32_t interpolateTableSigned(uint32_t input, int32_t numBitsInInput,
                                                             const int16_t* table, int32_t numBitsInTableSize = 8) {
	int32_t whichValue = input >> (numBitsInInput - numBitsInTableSize);
	int32_t rshiftAmount = numBitsInInput - 16 - numBitsInTableSize;
	uint32_t rshifted;
	if (rshiftAmount >= 0)
		rshifted = input >> rshiftAmount;
	else
		rshifted = input << (-rshiftAmount);
	int32_t strength2 = rshifted & 65535;
	int32_t strength1 = 65536 - strength2;
	return (int32_t)table[whichValue] * strength1 + (int32_t)table[whichValue + 1] * strength2;
}

[[gnu::always_inline]] inline int32_t getSine(uint32_t phase, uint8_t numBitsInInput = 32) {
	return interpolateTableSigned(phase, numBitsInInput, sineWaveSmall, 8);
}

[[gnu::always_inline]] inline int32_t getSquare(uint32_t phase, uint32_t phaseWidth = 2147483648u) {
	return ((phase >= phaseWidth) ? (-2147483648) : 2147483647);
}

[[gnu::always_inline]] inline int32_t getSquareSmall(uint32_t phase, uint32_t phaseWidth = 2147483648u) {
	return ((phase >= phaseWidth) ? (-1073741824) : 1073741823);
}

/* Should be faster, but isn't...
 * inline int32_t getTriangleSmall(int32_t phase) {
    int32_t multiplier = (phase >> 31) | 1;
    phase *= multiplier;
 */

[[gnu::always_inline]] inline int32_t getTriangleSmall(uint32_t phase) {
	if (phase >= 2147483648u)
		phase = -phase;
	return phase - 1073741824;
}

[[gnu::always_inline]] inline int32_t getTriangle(uint32_t phase) {
	int32_t slope = 2;
	int32_t offset = 0x80000000u;
	if (phase >= 0x80000000u) {
		slope = -2;
		offset = 0x80000000u - 1;
	}
	return slope * phase + offset;
}

/**
 * Unipolar triangle with dead zone - 0 → +max → 0, then silence
 *
 * Phase behavior:
 *   - Phase wraps naturally via uint32_t overflow (0xFFFFFFFF → 0)
 *   - Active region: [0, phaseWidth) - one triangle hump
 *   - Dead zone: [phaseWidth, 0xFFFFFFFF] - outputs 0
 *   - Peak occurs at phase = phaseWidth/2
 *
 * Output range:
 *   - Minimum: 0 (at phase 0, phaseWidth, and in dead zone)
 *   - Maximum: 0x7FFFFFFF (at phase = phaseWidth/2)
 *
 * @param phase Full 32-bit phase (0 to 0xFFFFFFFF)
 * @param phaseWidth End of active region; 0x80000000 = 50% active, 50% dead
 * @return Q31 unipolar value (0 to 0x7FFFFFFF)
 */
[[gnu::always_inline]] inline int32_t triangleWithDeadzone(uint32_t phase, uint32_t phaseWidth = 2147483648u) {
	constexpr int32_t kMaxQ31 = 0x7FFFFFFF;
	if (phase >= phaseWidth || phaseWidth == 0) {
		return 0;
	}
	uint32_t halfWidth = phaseWidth >> 1;
	if (phase < halfWidth) {
		return static_cast<int32_t>(((uint64_t)phase * kMaxQ31) / halfWidth);
	}
	uint32_t fallPhase = phase - halfWidth;
	return kMaxQ31 - static_cast<int32_t>(((uint64_t)fallPhase * kMaxQ31) / halfWidth);
}

/**
 * Bipolar triangle with dead zone - one complete cycle then silence
 *
 * Phase behavior:
 *   - Phase wraps naturally via uint32_t overflow (0xFFFFFFFF → 0)
 *   - Active region: [0, phaseWidth) - one complete bipolar cycle compressed to fit
 *   - Dead zone: [phaseWidth, 0xFFFFFFFF] - outputs 0
 *   - Waveform goes: 0 → +max → 0 → -max → 0 within active region
 *
 * Output range:
 *   - Minimum: -0x7FFFFFFF (at 3/4 through active region)
 *   - Maximum: +0x7FFFFFFF (at 1/4 through active region)
 *   - Zero crossings: at phase 0, phaseWidth/2, and phaseWidth (entering dead zone)
 *
 * @param phase Full 32-bit phase (0 to 0xFFFFFFFF)
 * @param phaseWidth End of active region; 0x80000000 = 50% active, 50% dead
 * @return Q31 bipolar value (-0x7FFFFFFF to +0x7FFFFFFF)
 */
[[gnu::always_inline]] inline int32_t triangleWithDeadzoneBipolar(uint32_t phase, uint32_t phaseWidth = 2147483648u) {
	if (phase >= phaseWidth || phaseWidth == 0) {
		return 0;
	}
	uint32_t scaledPhase = (uint32_t)(((uint64_t)phase << 32) / phaseWidth);
	return getTriangle(scaledPhase + 0x40000000u);
}
