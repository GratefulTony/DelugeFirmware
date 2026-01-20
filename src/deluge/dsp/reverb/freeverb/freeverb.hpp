// Reverb model declaration
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

#pragma once

#include "dsp/reverb/base.hpp"
#include "dsp/reverb/freeverb/allpass.hpp"
#include "dsp/reverb/freeverb/comb.hpp"
#include "dsp/reverb/freeverb/tuning.h"
#include "memory/general_memory_allocator.h"
#include <cstdint>
#include <cstring>
#include <span>

namespace deluge::dsp::reverb {
class Freeverb : public Base {
	// Total buffer size: all comb + allpass buffers combined (~93 KB)
	// Comb L: 1116+1188+1277+1356+1422+1491+1557+1617 = 10024
	// Comb R: 1139+1211+1300+1379+1445+1514+1580+1640 = 10208
	// Allpass L: 556+441+341+225 = 1563
	// Allpass R: 579+464+364+248 = 1655
	static constexpr size_t kTotalBufferSamples = 10024 + 10208 + 1563 + 1655; // 23450
	static constexpr size_t kTotalBufferBytes = kTotalBufferSamples * sizeof(int32_t);

public:
	Freeverb();
	~Freeverb() override { deallocate(); }

	// Dynamic allocation for SDRAM - call before first use
	[[nodiscard]] bool allocate();
	void deallocate();
	[[nodiscard]] bool isAllocated() const { return buffer_ != nullptr; }

	void mute();

	void setRoomSize(float value) override {
		roomsize = (value * scaleroom) + offsetroom;
		update();
	}

	[[nodiscard]] constexpr float getRoomSize() const override { return (roomsize - offsetroom) / scaleroom; }

	void setDamping(float value) override {
		damp = value * scaledamp;
		update();
	}

	[[nodiscard]] constexpr float getDamping() const override { return damp / scaledamp; }

	void setWet(float value) {
		wet = value * scalewet;
		update();
	}

	[[nodiscard]] constexpr float getWet() const { return wet / scalewet; }

	void setDry(float value) { dry = value * scaledry; }

	[[nodiscard]] constexpr float getDry() const { return dry / scaledry; }

	void setWidth(float value) override {
		width = value;
		update();
	}

	[[nodiscard]] constexpr float getWidth() const override { return width; }

	[[gnu::always_inline]] void ProcessOne(int32_t input, StereoSample<q31_t>& output_sample) {
		int32_t out_l = 0;
		int32_t out_r = 0;

		// Accumulate comb filters in parallel
		for (int32_t i = 0; i < numcombs; i++) {
			out_l += combL[i].process(input);
			out_r += combR[i].process(input);
		}

		// Feed through allpasses in series
		for (int32_t i = 0; i < numallpasses; i++) {
			out_l = allpassL[i].process(out_l);
			out_r = allpassR[i].process(out_r);
		}

		// Calculate output
		out_l = (out_l + q31_mult_rounded(out_r, wet2));
		out_r = (out_r + q31_mult_rounded(out_l, wet2));

		// Mix output
		output_sample.l += multiply_32x32_rshift32_rounded(out_l, this->getPanLeft());
		output_sample.r += multiply_32x32_rshift32_rounded(out_r, this->getPanRight());
	}

	[[gnu::always_inline]] void process(std::span<int32_t> input, StereoBuffer<q31_t> output) override {
		// Safety check - skip if buffer deallocated (can happen during model switch race)
		if (buffer_ == nullptr) {
			return;
		}
		// HPF on reverb input, cos if it has DC offset, the reverb magnifies that, and the sound farts out
		for (int32_t& reverb_sample : input) {
			int32_t distance_to_go_l = reverb_sample - reverb_send_post_lpf_;
			reverb_send_post_lpf_ += distance_to_go_l >> 11;
			reverb_sample -= reverb_send_post_lpf_;
		}

		for (size_t frame = 0; frame < input.size(); frame++) {
			ProcessOne(input[frame], output[frame]);
		}
	}

private:
	void update();
	void setupBuffers(); // Called from allocate() to wire up comb/allpass to buffer_

	int32_t gain;
	float roomsize;
	float damp;
	float wet;
	float wet1;
	int32_t wet2;
	float dry;
	float width;

	// Comb filters (state objects, buffers set via setBuffer())
	std::array<freeverb::Comb, numcombs> combL;
	std::array<freeverb::Comb, numcombs> combR;

	// Allpass filters (state objects, buffers set via setBuffer())
	std::array<freeverb::Allpass, numallpasses> allpassL;
	std::array<freeverb::Allpass, numallpasses> allpassR;

	// Single contiguous buffer for all comb/allpass delay lines (~93 KB)
	// Allocated from SDRAM via allocate(), nullptr until then
	int32_t* buffer_{nullptr};

	int32_t reverb_send_post_lpf_ = 0;
};
} // namespace deluge::dsp::reverb
