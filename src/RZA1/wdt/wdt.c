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
#include "RZA1/wdt/wdt.h"
#include "RZA1/system/iodefines/wdt_iodefine.h"

// Register write keys (RZ/A1 hardware manual, WDT chapter): WTCSR takes 0xA5 in the upper
// byte, WTCNT takes 0x5A. WRCSR is dual-keyed: 0xA5xx clears WOVF only; 0x5Axx writes
// RSTE/RSTS only. All three read back their 8-bit value in the low byte.

// WTCSR bits: WT/IT=0x40 (watchdog mode), TME=0x20 (enable), CKS=0x07 (P0-phi/8192)
#define WTCSR_BOOT_CONFIG (0x40 | 0x20 | 0x07)
// WRCSR bits (0x5A-keyed): RSTE=0x40 (reset on overflow), RSTS=0 (power-on reset)
#define WRCSR_RESET_CONFIG 0x40

void wdtBootArm(void)
{
    WDT.WTCSR = 0xA500;                      /* Stop the timer while configuring */
    WDT.WTCNT = 0x5A00;                      /* Counter to zero */
    WDT.WRCSR = 0xA500;                      /* Clear WOVF */
    WDT.WRCSR = 0x5A00 | WRCSR_RESET_CONFIG; /* Overflow causes a power-on reset */
    WDT.WTCSR = 0xA500 | WTCSR_BOOT_CONFIG;  /* Watchdog mode, enabled, ~63ms period */
}

void wdtKick(void)
{
    WDT.WTCNT = 0x5A00;
}

void wdtBootDisarm(void)
{
    WDT.WTCSR = 0xA500; /* TME=0 stops the counter */
    WDT.WTCNT = 0x5A00;
}

uint8_t wdtReadAndClearOverflow(void)
{
    uint8_t overflowed = (uint8_t)((WDT.WRCSR >> 7) & 1u);
    WDT.WRCSR          = 0xA500; /* A5-keyed write clears WOVF, leaves RSTE/RSTS alone */
    return overflowed;
}
