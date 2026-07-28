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
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */
#ifndef DELUGE_UTIL_BOOT_TRACE_H
#define DELUGE_UTIL_BOOT_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Boot-stage tracer for the chainload-boot hang hunt. Lives in the .boot_trace retention
// section (placed directly after the crash breadcrumb, NOLOAD, never zeroed at boot), so it
// survives watchdog resets and chainloads - internal RAM keeps its contents while power stays
// on. A hung boot therefore leaves its last stage number readable by the NEXT boot of an
// instrumented image, even after the watchdog has reset the chip into the SD firmware.
//
// Stage map (resetprg.c): 1 entry, 2 frunk cleared, 3 intc, 4 caches, 5 irq enabled,
// 6 sdram controller, 7 sdram memset, 8 relocations, 9 sdram_text cache maintenance,
// 10 wdt disarmed / pre libc_init_array, 11 libc_init_array done. 0xFF = boot complete
// (stamped by the first mem-sentinel tick).

// Snapshot the previous boot's outcome, reset stage to 1, arm nothing (WDT is separate).
void bootTraceBegin(void);
void bootTraceStage(uint32_t stage);
void bootTraceDone(void);

// Previous boot's outcome, as captured by bootTraceBegin() this boot.
uint32_t bootTracePrevStage(void);  // 0xFF = completed; 0 = no valid trace (e.g. power cycle)
uint32_t bootTracePrevWasWdt(void); // 1 if the reset that ended the previous run was the WDT
uint32_t bootTraceBootCount(void);
uint32_t bootTraceIsComplete(void); // 1 once bootTraceDone() has run this boot

// Runtime watchdog: armed once boot completes; the scheduler kicks it on every task dispatch
// and stamps the task name into retention. A runtime hang (any handler spinning >63ms without
// dispatching) resets into the SD firmware; the next instrumented boot then reports
// prevStage=0xFF + prevWasWdt=1 + the name of the task that was running when kicking stopped.
void runtimeWatchdogArm(void);
void runtimeWatchdogTaskTick(const char* taskName);
const char* bootTracePrevRuntimeTask(void); // "" unless previous run = runtime WDT reset

#ifdef __cplusplus
}
#endif

#endif // DELUGE_UTIL_BOOT_TRACE_H
