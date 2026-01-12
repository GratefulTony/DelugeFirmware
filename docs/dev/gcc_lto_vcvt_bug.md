# GCC LTO vcvt Register Allocation Bug

## Summary

A latent bug in ARM VFP inline assembly constraints was exposed by GCC's Link-Time Optimization (LTO) when new code paths changed register allocation patterns. The bug affected fixed-point conversion functions using the `vcvt` instruction.

## The Error

During LTO linking, the build would fail with:

```
/tmp/ccHa0b3W.s:36596: Error: operands 0 and 1 must be the same register -- `vcvt.s32.f32 s15,s16,#31'
make: *** [/tmp/ccnnNyi8.mk:62: /tmp/ccLx0P2G.ltrans20.ltrans.o] Error 1
make: *** Waiting for unfinished jobs....
lto-wrapper: fatal error: make returned 2 exit status
compilation terminated.
/home/tony/src/DelugeFirmware/toolchain/v22/linux-x86_64/arm-none-eabi-gcc/bin/../lib/gcc/arm-none-eabi/14.2.1/../../../../arm-none-eabi/bin/ld: error: lto-wrapper failed
collect2: error: ld returned 1 exit status
ninja: build stopped: subcommand failed.
```

The key error is:
```
Error: operands 0 and 1 must be the same register -- `vcvt.s32.f32 s15,s16,#31'
```

## Root Cause

### The ARM vcvt Instruction

The ARM VFP `vcvt` instruction with fixed-point conversion requires the **source and destination registers to be identical**. This is an in-place conversion - the instruction reads a float from a register, converts it to fixed-point, and writes the result back to the **same** register.

Valid:
```asm
vcvt.s32.f32 s15, s15, #31   ; OK - same register
```

Invalid:
```asm
vcvt.s32.f32 s15, s16, #31   ; ERROR - different registers
```

### The Buggy Inline Assembly

The original code in `intrinsics.h` and `fixedpoint.h` used:

```cpp
asm("vcvt.s32.f32 %0, %0, #31" : "=t"(value) : "t"(value));
```

This constraint specification tells GCC:
- `"=t"(value)` - Output goes to **some** VFP single-precision register
- `"t"(value)` - Input comes from **some** VFP single-precision register

The problem: GCC is **allowed** to pick different registers for input and output. The `%0, %0` in the assembly template is just a text substitution - it tells the assembler to use operand 0 twice, but if GCC allocated `s15` for output and `s16` for input, the generated code becomes:

```asm
vcvt.s32.f32 s15, s16, #31   ; INVALID
```

### Why It Worked Before

Without LTO, GCC's register allocator happened to choose the same register for both operands - likely because the input and output were the same C variable. This was luck, not correctness.

### Why LTO Exposed It

LTO merges code from multiple translation units and performs aggressive cross-file optimization. This changes:

1. **Function inlining patterns** - More code visible at once means different inlining decisions
2. **Register pressure** - More code in scope means more competition for registers
3. **Optimization order** - Different sequencing of optimization passes

The new sine shaper meta zone code (Zone 4+ with phi-ratio triangles) added enough complexity to shift GCC's register allocation, causing it to finally pick different registers for the vcvt operands.

## The Fix

Change the constraint from separate input/output to a **read-modify-write** operand:

### Before (Buggy)
```cpp
asm("vcvt.s32.f32 %0, %0, #31" : "=t"(value) : "t"(value));
```

### After (Fixed)
```cpp
asm("vcvt.s32.f32 %0, %0, #31" : "+t"(value));
```

The `+` modifier means "read-modify-write" - GCC **must** use the same register for both reading the input and writing the output. This correctly models the vcvt instruction's behavior.

### Files Changed

| File | Function | Line |
|------|----------|------|
| `src/deluge/util/intrinsics.h` | `q31_from_float()` | 221 |
| `src/deluge/util/intrinsics.h` | `int32_to_float()` | 229 |
| `src/deluge/util/fixedpoint.h` | `from_float(float)` | 130 |
| `src/deluge/util/fixedpoint.h` | `to_float()` | 162 |
| `src/deluge/util/fixedpoint.h` | `from_float(double)` | 176 |
| `src/deluge/util/fixedpoint.h` | `operator double()` | 199 |

## Performance Impact

**None.** The fix has zero performance regression:

1. **Same instruction generated** - The vcvt instruction is identical; only the register allocation constraint changes

2. **No additional instructions** - The `+` modifier doesn't add any code; it only restricts GCC's register choices

3. **Potentially better performance** - By forcing the same register, we avoid any theoretical scenario where GCC might have added a register-to-register move to satisfy the (incorrectly specified) separate input/output constraint

4. **Cycle count unchanged** - vcvt remains 1 cycle issue, 4 cycles result latency

## Verification

The fix can be verified by:

1. **Successful build** - LTO linking completes without assembler errors
2. **Disassembly inspection** - The generated vcvt instructions use identical source/destination registers
3. **Binary size comparison** - No significant change in code size

## Lessons Learned

1. **LTO exposes latent bugs** - Code that "works" without LTO may have undefined behavior that LTO reveals

2. **Inline assembly constraints must model hardware accurately** - If an instruction requires in-place operation, the constraint must enforce it

3. **The `+` modifier exists for a reason** - Use it for read-modify-write operations instead of separate `=` output and input constraints with the same variable

4. **Test with LTO enabled** - Include LTO builds in CI to catch these issues early

## References

- [GCC Inline Assembly Constraints](https://gcc.gnu.org/onlinedocs/gcc/Constraints.html)
- [ARM VFP vcvt Instruction](https://developer.arm.com/documentation/dui0489/i/vfp-instructions/vcvt--between-floating-point-and-fixed-point-)
- [GCC Modifiers for Constraint Letters](https://gcc.gnu.org/onlinedocs/gcc/Modifiers.html)
