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

#include <stdint.h>

// Crash breadcrumb: when the firmware freezes with an error code (or hard-faults), the code,
// caller/stack pointers and any diagnostic context are sealed into a struct living in the
// RZ/A1's data-retention RAM (0x2000_0000 block, below the firmware image), then the cache
// lines are flushed so the crumb is in physical RAM. The crumb survives resume-after-freeze,
// chainloads and soft resets, and has decent odds across a quick power flip. A slow SD task
// appends any valid crumb to CRASH.LOG on the card once the system is healthy again.
//
// Writing to the SD card at freeze time is deliberately avoided: the filesystem stack is not
// reentrant, the freeze may have fired inside it, and the card holds the user's music.

#ifdef __cplusplus
extern "C" {
#endif

// Called from the fault handler (C) with whatever return addresses its stack walk found.
// Safe to call at any time; only the next seal picks them up.
void crashBreadcrumbNotePointers(uint32_t lrSys, uint32_t lrUsr, const uint32_t* stackTrace, uint32_t numStackTrace);

// Seals a crumb for the given error code. First-crash-wins: further calls this session are
// ignored, so a nested fault during the freeze UI can't overwrite the original crumb.
void crashBreadcrumbStash(const char* errorCode);

#ifdef __cplusplus
}

// Diagnostic context, appended by error sites just before they freeze (e.g. the E078 reason
// dump). Cleared by crashBreadcrumbContextStart(); truncated silently when full.
void crashBreadcrumbContextStart();
void crashBreadcrumbContextAppend(char const* text);
void crashBreadcrumbContextAppendInt(int32_t value);

// Appends any sealed crumb to CRASH.LOG and invalidates it. Call only from a context where
// SD access is safe (RESOURCE_SD task); no-op when no valid crumb exists.
void crashBreadcrumbFlushRoutine();
#endif
