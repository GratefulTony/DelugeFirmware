/*
 * Mock arm_neon.h for x86 unit tests
 * Uses SIMDe (SIMD Everywhere) to provide portable NEON intrinsics
 */
#ifndef ARM_NEON_MOCK_H
#define ARM_NEON_MOCK_H

#define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/arm/neon.h"

#endif // ARM_NEON_MOCK_H
