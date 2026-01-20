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

#pragma once

#if ENABLE_MEMORY_PROFILE

namespace MemoryProfile {

/// Output memory stats for all regions via Debug::println()
/// Format: M,region,total_size,free_space,largest_block,num_allocs,num_free_blocks
void outputStats();

} // namespace MemoryProfile

#define MEMORY_PROFILE_OUTPUT() MemoryProfile::outputStats()

#else

#define MEMORY_PROFILE_OUTPUT()                                                                                        \
	do {                                                                                                               \
	} while (0)

#endif
