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
#include "io/debug/mem_sentinel.h"
#include "io/midi/sysex.h"
#include "memory/memory_allocator_interface.h"
#include "util/boot_trace.h"
#include <cstring>

namespace Debug {
extern MIDICable* midiDebugCable;
}

extern uint32_t _stext;
extern uint32_t _etext;
extern uint32_t __rodata_start;
extern uint32_t __rodata_end;
extern uint32_t __sdram_text_start;
extern uint32_t __sdram_text_end;
extern uint32_t __sdram_rodata_start;
extern uint32_t __sdram_rodata_end;

namespace {

struct Region {
	char const* name;
	uint32_t* start;
	uint32_t* end;
	uint8_t* mirror;
};

Region regions[] = {
    {"text", &_stext, &_etext, nullptr},
    {"rodata", &__rodata_start, &__rodata_end, nullptr},
    {"sdram_text", &__sdram_text_start, &__sdram_text_end, nullptr},
    {"sdram_rodata", &__sdram_rodata_start, &__sdram_rodata_end, nullptr},
};
constexpr uint32_t kNumRegions = sizeof(regions) / sizeof(regions[0]);

struct Event {
	uint8_t regionIdx;
	uint32_t addr;
	uint32_t expect;
	uint32_t got;
};
constexpr uint32_t kMaxEvents = 8;
Event events[kMaxEvents];
uint32_t numEventsSeen; // Total, may exceed kMaxEvents (extras counted, not stored)
uint32_t numEventsSent;

bool mirrorsReady = false;

char lineBuf[160];
uint32_t linePos;

void lineStart() {
	linePos = 0;
	lineBuf[0] = 0;
}

void append(char const* s) {
	while (*s != 0 && linePos < sizeof(lineBuf) - 1) {
		lineBuf[linePos++] = *s++;
	}
	lineBuf[linePos] = 0;
}

void appendHex(uint32_t value) {
	static char const digits[] = "0123456789ABCDEF";
	char buf[11] = "0x";
	for (int32_t i = 0; i < 8; i++) {
		buf[2 + i] = digits[(value >> (28 - i * 4)) & 0xF];
	}
	buf[10] = 0;
	append(buf);
}

void lineSend() {
	if (Debug::midiDebugCable != nullptr) {
		Debug::sysexDebugPrint(*Debug::midiDebugCable, lineBuf, true);
	}
}

void setupMirrors() {
	for (Region& r : regions) {
		uint32_t size = (uint32_t)r.end - (uint32_t)r.start;
		r.mirror = (uint8_t*)allocSdram(size);
		if (r.mirror == nullptr) {
			return; // Retry next tick; SDRAM this large should never fail
		}
		memcpy(r.mirror, r.start, size);
	}
	mirrorsReady = true;
}

void recordEvent(uint32_t regionIdx, uint32_t addr, uint32_t expect, uint32_t got) {
	if (numEventsSeen < kMaxEvents) {
		events[numEventsSeen] = Event{(uint8_t)regionIdx, addr, expect, got};
	}
	numEventsSeen++;
}

void drainEvents() {
	while (numEventsSent < numEventsSeen && numEventsSent < kMaxEvents) {
		Event const& e = events[numEventsSent];
		lineStart();
		append("{\"sentinel\":{\"region\":\"");
		append(regions[e.regionIdx].name);
		append("\",\"addr\":\"");
		appendHex(e.addr);
		append("\",\"expect\":\"");
		appendHex(e.expect);
		append("\",\"got\":\"");
		appendHex(e.got);
		append("\"}}");
		lineSend();
		numEventsSent++;
	}
}

} // namespace

void memSentinelRoutine() {
	static bool bootMarkedDone = false;
	if (!bootMarkedDone) {
		bootMarkedDone = true;
		bootTraceDone(); // First tick of the scheduler = boot effectively complete
	}

	// Report the PREVIOUS boot's outcome once per session when a console is attached: after a
	// watchdog-recovered hang, this line (on the next instrumented boot) carries the stage the
	// hung boot died at.
	static bool bootTraceReported = false;
	if (!bootTraceReported && Debug::midiDebugCable != nullptr) {
		bootTraceReported = true;
		lineStart();
		append("{\"boottrace\":{\"prevStage\":");
		appendHex(bootTracePrevStage());
		append(",\"prevWasWdt\":");
		appendHex(bootTracePrevWasWdt());
		append(",\"bootCount\":");
		appendHex(bootTraceBootCount());
		append("}}");
		lineSend();
	}

	if (!mirrorsReady) {
		setupMirrors();
		return;
	}

	// Sweep one small slice per tick: comparing everything at once blocks the cooperative
	// scheduler for ~10ms (audible glitch every tick) and evicts the audio working set from the
	// D-cache. 32KB/tick keeps each visit well under the audio buffer budget; a full 2MB sweep
	// completes every ~1-2 minutes, which is plenty - immutable-region corruption doesn't undo
	// itself, we only trade detection latency.
	constexpr uint32_t kSliceBytes = 32 * 1024;
	static uint32_t regionCursor = 0;
	static uint32_t offsetCursor = 0;

	Region& r = regions[regionCursor];
	uint32_t regionSize = (uint32_t)r.end - (uint32_t)r.start;
	uint32_t sliceLen = kSliceBytes;
	if (offsetCursor + sliceLen > regionSize) {
		sliceLen = regionSize - offsetCursor;
	}
	uint32_t const* live = (uint32_t const*)((uint32_t)r.start + offsetCursor);
	uint32_t const* gold = (uint32_t const*)(r.mirror + offsetCursor);

	if (memcmp(gold, live, sliceLen) != 0) {
		uint32_t found = 0;
		for (uint32_t w = 0; w < sliceLen / 4 && found < kMaxEvents; w++) {
			if (live[w] != gold[w]) {
				recordEvent(regionCursor, (uint32_t)&live[w], gold[w], live[w]);
				found++;
			}
		}
		memcpy((void*)(r.mirror + offsetCursor), live, sliceLen);
	}

	offsetCursor += sliceLen;
	if (offsetCursor >= regionSize) {
		offsetCursor = 0;
		regionCursor = (regionCursor + 1) % kNumRegions;
	}

	// One event line per tick, same pacing rule as the bench reporter
	if (Debug::midiDebugCable != nullptr && numEventsSent < numEventsSeen) {
		drainEvents();
		if (numEventsSeen > kMaxEvents && numEventsSent == kMaxEvents) {
			lineStart();
			append("{\"sentinel\":{\"suppressed\":");
			appendHex(numEventsSeen - kMaxEvents);
			append("}}");
			lineSend();
		}
	}
}

void memSentinelPeek(uint32_t addr) {
	static char const digits[] = "0123456789ABCDEF";
	lineStart();
	append("{\"peek\":\"");
	appendHex(addr);
	append("\",\"bytes\":\"");
	uint8_t const* p = (uint8_t const*)addr;
	for (uint32_t i = 0; i < 32 && linePos < sizeof(lineBuf) - 4; i++) {
		lineBuf[linePos++] = digits[p[i] >> 4];
		lineBuf[linePos++] = digits[p[i] & 0xF];
	}
	lineBuf[linePos] = 0;
	append("\"}");
	lineSend();
}
