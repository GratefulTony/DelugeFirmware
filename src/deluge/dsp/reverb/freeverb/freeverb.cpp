// Reverb model implementation
//
// Written by Jezar at Dreampoint, June 2000
// http://www.dreampoint.co.uk
// This code is public domain

/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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
 */

#include "dsp/reverb/freeverb/freeverb.hpp"
#include <limits>

namespace deluge::dsp::reverb {

Freeverb::Freeverb() {
	// Set default values for allpass feedback
	allpassL[0].setFeedback(0.5f);
	allpassR[0].setFeedback(0.5f);
	allpassL[1].setFeedback(0.5f);
	allpassR[1].setFeedback(0.5f);
	allpassL[2].setFeedback(0.5f);
	allpassR[2].setFeedback(0.5f);
	allpassL[3].setFeedback(0.5f);
	allpassR[3].setFeedback(0.5f);

	// Set default parameter values
	setWet(initialwet);
	setRoomSize(initialroom);
	setDry(initialdry);
	setDamping(initialdamp);
	setWidth(initialwidth);

	// Note: buffers are NOT set up here - call allocate() before first use
}

bool Freeverb::allocate() {
	if (buffer_ != nullptr) {
		return true; // Already allocated
	}
	// Prefer fast SRAM, fall back to SDRAM - benchmarking shows no performance difference
	buffer_ = static_cast<int32_t*>(GeneralMemoryAllocator::get().allocMaxSpeed(kTotalBufferBytes, nullptr));
	if (buffer_ == nullptr) {
		return false;
	}
	std::memset(buffer_, 0, kTotalBufferBytes);
	setupBuffers();
	return true;
}

void Freeverb::deallocate() {
	if (buffer_ != nullptr) {
		delugeDealloc(buffer_);
		buffer_ = nullptr;
		// Clear buffer references in comb/allpass filters
		for (int32_t i = 0; i < numcombs; i++) {
			combL[i].setBuffer(std::span<int32_t>());
			combR[i].setBuffer(std::span<int32_t>());
		}
		for (int32_t i = 0; i < numallpasses; i++) {
			allpassL[i].setBuffer(std::span<int32_t>());
			allpassR[i].setBuffer(std::span<int32_t>());
		}
	}
}

void Freeverb::setupBuffers() {
	// Wire up comb and allpass filters to regions of the contiguous buffer
	// Buffer layout: [CombL1..CombL8][CombR1..CombR8][AllpassL1..L4][AllpassR1..R4]
	int32_t* ptr = buffer_;

	// Comb L buffers
	static constexpr size_t combSizesL[] = {combtuningL1, combtuningL2, combtuningL3, combtuningL4,
	                                        combtuningL5, combtuningL6, combtuningL7, combtuningL8};
	for (int32_t i = 0; i < numcombs; i++) {
		combL[i].setBuffer(std::span<int32_t>(ptr, combSizesL[i]));
		ptr += combSizesL[i];
	}

	// Comb R buffers
	static constexpr size_t combSizesR[] = {combtuningR1, combtuningR2, combtuningR3, combtuningR4,
	                                        combtuningR5, combtuningR6, combtuningR7, combtuningR8};
	for (int32_t i = 0; i < numcombs; i++) {
		combR[i].setBuffer(std::span<int32_t>(ptr, combSizesR[i]));
		ptr += combSizesR[i];
	}

	// Allpass L buffers
	static constexpr size_t allpassSizesL[] = {allpasstuningL1, allpasstuningL2, allpasstuningL3, allpasstuningL4};
	for (int32_t i = 0; i < numallpasses; i++) {
		allpassL[i].setBuffer(std::span<int32_t>(ptr, allpassSizesL[i]));
		ptr += allpassSizesL[i];
	}

	// Allpass R buffers
	static constexpr size_t allpassSizesR[] = {allpasstuningR1, allpasstuningR2, allpasstuningR3, allpasstuningR4};
	for (int32_t i = 0; i < numallpasses; i++) {
		allpassR[i].setBuffer(std::span<int32_t>(ptr, allpassSizesR[i]));
		ptr += allpassSizesR[i];
	}
}

void Freeverb::mute() {
	for (int32_t i = 0; i < numcombs; i++) {
		combL[i].mute();
		combR[i].mute();
	}
	for (int32_t i = 0; i < numallpasses; i++) {
		allpassL[i].mute();
		allpassR[i].mute();
	}
}

void Freeverb::update() {
	// Recalculate internal values after parameter change

	wet1 = wet * (width / 2 + 0.5f);
	wet2 = (((float)1 - width) / 2) / (width / 2 + 0.5f) * std::numeric_limits<int32_t>::max();

	gain = fixedgain * std::numeric_limits<int32_t>::max();

	for (size_t i = 0; i < numcombs; i++) {
		combL[i].setFeedback(roomsize * std::numeric_limits<int32_t>::max());
		combR[i].setFeedback(roomsize * std::numeric_limits<int32_t>::max());
	}

	for (size_t i = 0; i < numcombs; i++) {
		combL[i].setDamp(damp);
		combR[i].setDamp(damp);
	}
}

} // namespace deluge::dsp::reverb
