#pragma once

#include "definitions_cxx.hpp"
#include <atomic>
#include <cstdint>

namespace deluge::processing::engines {

// Lock-free single-writer (audio thread) / single-reader (card/slow thread) lookback buffer.
// Stores interleaved q31_t samples (L,R for stereo). Frame = stereo pair or mono sample.
class LookbackBuffer {
public:
	// Initialize with sample rate, channels (1 or 2) and length in seconds.
	// Returns false on insufficient RAM.
	bool init(int32_t sampleRate, int32_t channels, int32_t lengthSeconds);

	// Free resources (called at shutdown or swap).
	void deinit();

	// Called from audio render path: write interleaved frames from buffer pointer (numFrames).
	// input must be q31_t[] interleaved (L,R if stereo).
	inline void writeFrames(const q31_t* input, int32_t numFrames);

	// Request a snapshot to save the most recent numFrames frames.
	// Returns true if the snapshot info was returned successfully into outStartIndex/outNumFrames.
	// The caller should then call readFramesInto(...) from a background (non-audio) thread to copy sequentially.
	bool requestSnapshot(int32_t numFrames, uint64_t* outStartSampleIndex, int32_t* outNumFrames);

	// Background thread helper to read frames starting at a sample index (absolute modulo buffer).
	// outBuffer must have capacity for outNumFrames * channels.
	// Returns number of frames copied (should equal outNumFrames unless the buffer was reinitialized).
	int32_t readFramesInto(uint64_t startSampleIndex, int32_t numFrames, q31_t* outBuffer);

	// Query current configuration
	int32_t getChannels() const { return channels_; }
	int32_t getSampleRate() const { return sampleRate_; }
	int32_t getLengthSeconds() const { return lengthSeconds_; }

private:
	// absolute frame index monotonic counter incremented by writer (frame = sample frame, not per channel)
	std::atomic<uint64_t> absoluteWriteIndex_{0};

	// raw buffer (interleaved samples)
	q31_t* buffer_{nullptr};
	// capacity in frames (per-channel)
	int32_t capacityFrames_{0};
	int32_t channels_{0};
	int32_t sampleRate_{0};
	int32_t lengthSeconds_{0};

	// helper to allocate and free in one place
	bool allocateBuffer(int32_t frames);
	void freeBuffer();
};

} // namespace deluge::processing::engines
