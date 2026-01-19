/*
 * Copyright © 2024-2025 Owlet Records
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

#if ENABLE_MEMORY_PROFILE

#include "io/debug/memory_profile.h"
#include "io/debug/print.h"
#include "memory/general_memory_allocator.h"
#include "util/d_string.h"

namespace MemoryProfile {

namespace {

const char* getRegionName(int32_t regionIndex) {
	static const char* names[] = {
	    "stealable", // MEMORY_REGION_STEALABLE
	    "internal",  // MEMORY_REGION_INTERNAL
	    "external",  // MEMORY_REGION_EXTERNAL
	    "ext_small", // MEMORY_REGION_EXTERNAL_SMALL
	    "int_small", // MEMORY_REGION_INTERNAL_SMALL
	};
	if (regionIndex >= 0 && regionIndex < NUM_MEMORY_REGIONS) {
		return names[regionIndex];
	}
	return "unknown";
}

// Helper to append string (from fx_benchmark pattern)
char* appendStr(char* p, const char* src, char* end) {
	while (*src && p < end) {
		*p++ = *src++;
	}
	return p;
}

// Helper to append number
char* appendNum(char* p, uint32_t num, char* end) {
	char numBuf[12];
	intToString(num, numBuf);
	return appendStr(p, numBuf, end);
}

} // namespace

void outputStats() {
	char buffer[128];
	char* end = buffer + 120;

	// Header line
	Debug::println("M,region,total_size,free_space,largest_block,num_allocs,num_free_blocks");

	GeneralMemoryAllocator& allocator = GeneralMemoryAllocator::get();

	for (int32_t i = 0; i < NUM_MEMORY_REGIONS; i++) {
		MemoryRegion& region = allocator.regions[i];

		uint32_t totalSize = region.getRegionSize();
		uint32_t freeSpace = region.getTotalFreeSpace();
		uint32_t largestBlock = region.getLargestFreeBlock();
		uint32_t numAllocs = region.getNumAllocations();
		int32_t numFreeBlocks = region.emptySpaces.getNumElements();

		// Build CSV line: M,region,total_size,free_space,largest_block,num_allocs,num_free_blocks
		char* p = buffer;
		*p++ = 'M';
		*p++ = ',';
		p = appendStr(p, getRegionName(i), end);
		*p++ = ',';
		p = appendNum(p, totalSize, end);
		*p++ = ',';
		p = appendNum(p, freeSpace, end);
		*p++ = ',';
		p = appendNum(p, largestBlock, end);
		*p++ = ',';
		p = appendNum(p, numAllocs, end);
		*p++ = ',';
		p = appendNum(p, static_cast<uint32_t>(numFreeBlocks), end);
		*p = '\0';

		Debug::println(buffer);
	}
}

} // namespace MemoryProfile

#endif // ENABLE_MEMORY_PROFILE
