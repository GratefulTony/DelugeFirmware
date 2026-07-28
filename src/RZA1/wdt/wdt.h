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
#ifndef RZA1_WDT_WDT_H
#define RZA1_WDT_WDT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// RZ/A1 watchdog (SH-style, 8-bit counter, keyed 16-bit register writes).
// Used as a BOOT watchdog only: armed at the top of resetprg, kicked at each boot stage,
// disarmed before __libc_init_array (C++ static init has unbounded duration). Max period is
// ~63ms (256 counts of P0-phi/8192 at 33.33MHz), so long boot operations must kick inside
// their loops. Overflow triggers an internal power-on reset: the chip re-runs the SPI
// bootloader, i.e. a hung chainloaded image auto-recovers into the SD-installed firmware.

void wdtBootArm(void);
void wdtKick(void);
void wdtBootDisarm(void);
// Returns 1 if the last reset was a watchdog overflow, and clears the flag.
uint8_t wdtReadAndClearOverflow(void);

#ifdef __cplusplus
}
#endif

#endif // RZA1_WDT_WDT_H
