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
#include "util/boot_trace.h"
#include "RZA1/compiler/asm/inc/asm.h"
#include "RZA1/wdt/wdt.h"
#include "util/crash_breadcrumb.h"

#define BOOT_TRACE_MAGIC 0xB007B007u

struct BootTrace {
	uint32_t magic;
	uint32_t stage;
	uint32_t bootCount;
};

__attribute__((__section__(".boot_trace"))) static volatile struct BootTrace bootTrace;

// This boot's snapshot of the previous run (regular bss, valid after bootTraceBegin)
static uint32_t prevStage;
static uint32_t prevWasWdt;

static void flushTrace(void) {
	// Make sure the words are in physical RAM, not just D-cache: a hang or WDT reset won't
	// write back the cache. Safe to call before the cache is enabled too.
	v7_dma_flush_range((uintptr_t)&bootTrace, (uintptr_t)(&bootTrace + 1));
}

void bootTraceBegin(void) {
	prevWasWdt = wdtReadAndClearOverflow();
	if (bootTrace.magic == BOOT_TRACE_MAGIC) {
		prevStage = bootTrace.stage;
	}
	else {
		prevStage = 0; /* Power cycle (RAM decayed) or first boot of an instrumented image */
		bootTrace.bootCount = 0;
	}
	bootTrace.magic = BOOT_TRACE_MAGIC;
	bootTrace.bootCount++;
	bootTrace.stage = 1;
	flushTrace();
	crashBreadcrumbBootStage(1);
}

void bootTraceStage(uint32_t stage) {
	bootTrace.stage = stage;
	flushTrace();
	// Mirrored into a breadcrumb: the plain trace above only survives proto-to-proto
	// chainloads, while the breadcrumb pipeline survives the WDT-reset-into-SD-firmware path
	// (the SD firmware flushes it to CRASH.LOG).
	crashBreadcrumbBootStage(stage);
}

void bootTraceDone(void) {
	bootTrace.stage = 0xFF;
	flushTrace();
	crashBreadcrumbBootDone();
}

uint32_t bootTracePrevStage(void) {
	return prevStage;
}

uint32_t bootTracePrevWasWdt(void) {
	return prevWasWdt;
}

uint32_t bootTraceBootCount(void) {
	return bootTrace.bootCount;
}

uint32_t bootTraceIsComplete(void) {
	return bootTrace.stage == 0xFF;
}
