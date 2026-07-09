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

#include "crash_breadcrumb.h"
#include "fatfs/ff.h"
#include "io/midi/sysex.h"
#include "model/song/song.h"
#include <cstddef>
#include <cstring>
#include <version.h>

namespace Debug {
extern MIDICable* midiDebugCable;
}

namespace AudioEngine {
extern uint32_t audioSampleTimer;
}

#if defined(__arm__)
extern "C" void v7_dma_flush_range(uint32_t start, uint32_t end);
#endif

namespace {

constexpr uint32_t kMagic = 0x4F574C43; // 'OWLC'
constexpr uint32_t kMaxStackTrace = 4;

struct BreadcrumbData {
	uint32_t magic;
	char errorCode[8];
	uint32_t lrSys;
	uint32_t lrUsr;
	uint32_t stackTrace[kMaxStackTrace];
	uint32_t numStackTrace;
	uint32_t uptimeSamples;
	char fwVersion[28];
	char songName[48];
	uint32_t contextLen;
	char context[320];
	uint32_t crc;
};

// Lives in the RZ/A1 data-retention RAM block (below the firmware image, so it also survives
// chainloading new firmware). The section is NOLOAD and deliberately not emptied at boot.
__attribute__((section(".crash_breadcrumb"))) BreadcrumbData crumb;

// Pending pointers noted by the fault handler, waiting for the next seal. Normal BSS.
uint32_t pendingLrSys;
uint32_t pendingLrUsr;
uint32_t pendingStackTrace[kMaxStackTrace];
uint32_t pendingNumStackTrace;
bool sealedThisSession;
bool provisionalThisSession;
bool contextWrittenThisSession;

uint32_t computeCrc() {
	uint32_t hash = 2166136261u;
	auto* bytes = reinterpret_cast<uint8_t const*>(&crumb);
	for (size_t i = 0; i < offsetof(BreadcrumbData, crc); i++) {
		hash = (hash ^ bytes[i]) * 16777619u;
	}
	return hash;
}

bool crumbIsValid() {
	return crumb.magic == kMagic && crumb.crc == computeCrc();
}

void flushCrumbToRam() {
#if defined(__arm__)
	auto start = reinterpret_cast<uint32_t>(&crumb) & ~31u;
	v7_dma_flush_range(start, reinterpret_cast<uint32_t>(&crumb) + sizeof(crumb));
#endif
}

void sealAndFlush() {
	crumb.crc = computeCrc();
	flushCrumbToRam();
}

void copyBounded(char* dest, char const* src, size_t destSize) {
	size_t i = 0;
	for (; i < destSize - 1 && src[i]; i++) {
		dest[i] = src[i];
	}
	dest[i] = 0;
}

void appendChar(char* buf, size_t bufSize, size_t& pos, char c) {
	if (pos < bufSize - 1) {
		buf[pos++] = c;
		buf[pos] = 0;
	}
}

void appendString(char* buf, size_t bufSize, size_t& pos, char const* text) {
	while (*text) {
		appendChar(buf, bufSize, pos, *text++);
	}
}

void appendUint(char* buf, size_t bufSize, size_t& pos, uint32_t value) {
	char digits[10];
	int32_t n = 0;
	do {
		digits[n++] = '0' + (value % 10);
		value /= 10;
	} while (value);
	while (n) {
		appendChar(buf, bufSize, pos, digits[--n]);
	}
}

void appendHex(char* buf, size_t bufSize, size_t& pos, uint32_t value) {
	for (int32_t shift = 28; shift >= 0; shift -= 4) {
		appendChar(buf, bufSize, pos, "0123456789abcdef"[(value >> shift) & 0xF]);
	}
}

} // namespace

extern "C" void crashBreadcrumbNotePointers(uint32_t lrSys, uint32_t lrUsr, const uint32_t* stackTrace,
                                            uint32_t numStackTrace) {
	pendingLrSys = lrSys;
	pendingLrUsr = lrUsr;
	pendingNumStackTrace = numStackTrace < kMaxStackTrace ? numStackTrace : kMaxStackTrace;
	for (uint32_t i = 0; i < pendingNumStackTrace; i++) {
		pendingStackTrace[i] = stackTrace[i];
	}
}

extern "C" void crashBreadcrumbSealProvisional() {
	// Called from the fault handler right after the stack walk, before anything that can hang
	// (the pad-drawing path ends in a busy-wait on the PIC DMA). Seals a pointers-only crumb
	// that the full stash upgrades moments later - or that survives alone if we never get there.
	if (sealedThisSession || provisionalThisSession || crumbIsValid()) {
		return;
	}
	provisionalThisSession = true;
	crashBreadcrumbStash("?");
	sealedThisSession = false; // Allow the real stash to upgrade this crumb
}

extern "C" void crashBreadcrumbStash(const char* errorCode) {
	// First crash wins - both within this session (a nested fault during the freeze UI must not
	// overwrite the original crumb) and against an unflushed crumb from a previous session.
	// A provisional crumb sealed by this same crash is the one exception: upgrade it.
	if (sealedThisSession || (crumbIsValid() && !provisionalThisSession)) {
		return;
	}
	sealedThisSession = true;

	// Seal a minimal crumb first, so a fault while gathering richer state can't lose the basics.
	crumb.magic = kMagic;
	copyBounded(crumb.errorCode, errorCode, sizeof(crumb.errorCode));
	crumb.lrSys = pendingLrSys;
	crumb.lrUsr = pendingLrUsr;
	crumb.numStackTrace = pendingNumStackTrace;
	for (uint32_t i = 0; i < kMaxStackTrace; i++) {
		crumb.stackTrace[i] = i < pendingNumStackTrace ? pendingStackTrace[i] : 0;
	}
	crumb.uptimeSamples = AudioEngine::audioSampleTimer;
	copyBounded(crumb.fwVersion, kFirmwareVersionString, sizeof(crumb.fwVersion));
	crumb.songName[0] = 0;
	if (!contextWrittenThisSession || crumb.contextLen >= sizeof(crumb.context)) {
		crumb.contextLen = 0;
	}
	crumb.context[crumb.contextLen] = 0;
	sealAndFlush();

	pendingLrSys = pendingLrUsr = pendingNumStackTrace = 0;

	// Enrichment: read the song name into a local buffer first, so a fault while dereferencing
	// a corrupt Song can't tear the already-sealed crumb.
	if (currentSong != nullptr) {
		char localName[sizeof(crumb.songName)];
		copyBounded(localName, currentSong->name.get(), sizeof(localName));
		memcpy(crumb.songName, localName, sizeof(crumb.songName));
		sealAndFlush();
	}
}

void crashBreadcrumbContextStart() {
	if (sealedThisSession || crumbIsValid()) {
		return;
	}
	contextWrittenThisSession = true;
	crumb.contextLen = 0;
	crumb.context[0] = 0;
}

void crashBreadcrumbContextAppend(char const* text) {
	if (sealedThisSession || crumbIsValid()) {
		return;
	}
	size_t pos = crumb.contextLen;
	appendString(crumb.context, sizeof(crumb.context), pos, text);
	crumb.contextLen = pos;
}

void crashBreadcrumbContextAppendInt(int32_t value) {
	if (sealedThisSession || crumbIsValid()) {
		return;
	}
	size_t pos = crumb.contextLen;
	if (value < 0) {
		appendChar(crumb.context, sizeof(crumb.context), pos, '-');
		value = -value;
	}
	appendUint(crumb.context, sizeof(crumb.context), pos, static_cast<uint32_t>(value));
	crumb.contextLen = pos;
}

namespace {
char reportLine[640];
size_t reportLineLength;
bool haveReport;
} // namespace

void crashBreadcrumbDumpRecent() {
	if (Debug::midiDebugCable == nullptr) {
		return;
	}
	if (haveReport) {
		Debug::sysexDebugPrint(*Debug::midiDebugCable, reportLine, false);
	}
	else if (crumb.magic == kMagic) {
		// Magic survived but CRC didn't: the crumb decayed across a power cycle.
		Debug::sysexDebugPrint(*Debug::midiDebugCable, "breadcrumb: found but corrupt (RAM decay across power-off?)",
		                       true);
	}
	else {
		Debug::sysexDebugPrint(*Debug::midiDebugCable, "breadcrumb: none this boot", true);
	}
}

void crashBreadcrumbFlushRoutine() {
	if (!crumbIsValid()) {
		return;
	}

	if (!haveReport) {
		char* line = reportLine;
		constexpr size_t lineSize = sizeof(reportLine);
		size_t pos = 0;
		appendString(line, lineSize, pos, crumb.errorCode);
		appendString(line, lineSize, pos, " fw=");
		appendString(line, lineSize, pos, crumb.fwVersion);
		appendString(line, lineSize, pos, " up=");
		appendUint(line, lineSize, pos, crumb.uptimeSamples / 44100);
		appendString(line, lineSize, pos, "s song=");
		appendString(line, lineSize, pos, crumb.songName[0] ? crumb.songName : "-");
		appendString(line, lineSize, pos, " lrS=");
		appendHex(line, lineSize, pos, crumb.lrSys);
		appendString(line, lineSize, pos, " lrU=");
		appendHex(line, lineSize, pos, crumb.lrUsr);
		appendString(line, lineSize, pos, " stk=");
		for (uint32_t i = 0; i < crumb.numStackTrace; i++) {
			if (i) {
				appendChar(line, lineSize, pos, ',');
			}
			appendHex(line, lineSize, pos, crumb.stackTrace[i]);
		}
		if (crumb.contextLen && crumb.contextLen < sizeof(crumb.context)) {
			appendString(line, lineSize, pos, " ctx: ");
			appendString(line, lineSize, pos, crumb.context);
		}
		appendChar(line, lineSize, pos, '\n');
		reportLineLength = pos;
		haveReport = true;
		crashBreadcrumbDumpRecent();
	}

	FIL file;
	if (f_open(&file, "CRASH.LOG", FA_WRITE | FA_OPEN_ALWAYS) != FR_OK) {
		return; // Card not ready - keep the crumb and retry later
	}
	f_lseek(&file, f_size(&file));
	UINT written = 0;
	FRESULT result = f_write(&file, reportLine, reportLineLength, &written);
	f_close(&file);

	if (result == FR_OK && written == reportLineLength) {
		crumb.magic = 0;
		crumb.crc = 0;
		flushCrumbToRam();
	}
}
