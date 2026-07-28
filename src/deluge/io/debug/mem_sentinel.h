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
#pragma once
#include <cstdint>

// TEMP DIAGNOSTIC for the layout-sensitive corruption hunt on proto/sdram-text.
// Mirrors every immutable region (.text, .rodata, .sdram_text, .sdram_rodata) into SDRAM on the
// first tick, then memcmps each tick. Corruption events (exact address, expected, got) are held
// in a small ring and printed to the sysex debug console when one is attached, then the mirror
// resyncs so subsequent events keep getting caught.
void memSentinelRoutine();

// Sysex debug console peek: hex-dump 32 bytes at an address, for live comparison against the ELF.
void memSentinelPeek(uint32_t addr);
