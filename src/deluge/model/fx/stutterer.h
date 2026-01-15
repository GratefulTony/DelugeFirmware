/*
 * Copyright © 2016-2024 Synthstrom Audible Limited
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

#include "dsp/delay/delay_buffer.h"
#include <cstdint>
#include <span>

class ParamManagerForTimeline;
class ParamManager;

/// Scatter mode determines how the stutter buffer is manipulated during playback
enum class ScatterMode : uint8_t {
	Classic = 0, ///< Original stutter behavior (passthrough)
	Repeat,      ///< Beat repeat with count control
	Reverse,     ///< Segment reversal
	Chop,        ///< Beat slicing with gate
	Shuffle,     ///< Phi-based segment reordering
	Tape,        ///< Tape stop/start speed effect
	Pitch,       ///< Pitch manipulation
	Filter,      ///< Filter sweep
	NUM_MODES
};

struct StutterConfig {
	bool useSongStutter = true;
	bool quantized = true;
	bool reversed = false;
	bool pingPong = false;
	bool latch = false; ///< Scatter mode: latch (stays on after release) vs normal (release to stop)
	ScatterMode scatterMode = ScatterMode::Classic;

	// Secret knob phase offsets (push+twist encoder on zone knobs)
	float zoneAPhaseOffset{0};       ///< Zone A structural phase offset (push Zone A encoder)
	float zoneBPhaseOffset{0};       ///< Zone B timbral phase offset (push Zone B encoder)
	float macroConfigPhaseOffset{0}; ///< Macro config phase offset (push Macro Config encoder)
	float gammaPhase{0};             ///< Gamma multiplier for macro (push Macro encoder)
};

class Stutterer {
public:
	Stutterer() = default;
	static void initParams(ParamManager* paramManager);

	/// Buffer size for non-Classic looper modes (4 seconds at 44.1kHz for ring buffer)
	static constexpr size_t kLooperBufferSize = 44100 * 4;
	/// Check if source is actively playing from the scatter buffer
	inline bool isStuttering(void* source) {
		return playSource == source && (status == Status::RECORDING || status == Status::PLAYING);
	}
	/// Check if scatter is actively playing (regardless of ownership)
	inline bool isScatterPlaying() const {
		return stutterConfig.scatterMode != ScatterMode::Classic
		       && (status == Status::RECORDING || status == Status::PLAYING);
	}
	/// Check if this source owns either play or record buffer
	inline bool ownsStutter(void* source) { return playSource == source || recordSource == source; }
	/// Get the current scatter mode (valid while stuttering or armed)
	inline ScatterMode getScatterMode() const {
		if (status == Status::ARMED) {
			return armedConfig.scatterMode;
		}
		return stutterConfig.scatterMode;
	}
	/// Check if standby recording is active
	inline bool isInStandby() const { return status == Status::STANDBY; }
	/// Check if armed and waiting for beat quantize
	/// Takeover = PLAYING with a different source recording (preparing to take over)
	inline bool isArmed() const {
		return status == Status::ARMED || (status == Status::PLAYING && recordSource != playSource);
	}
	/// Check if source is armed for takeover (recording while someone else plays)
	inline bool isArmedForTakeover(void* source) const {
		return status == Status::PLAYING && recordSource == source && playSource != source;
	}
	// These calls are slightly awkward with the magniture & timePerTickInverse, but that's the price for not depending
	// on currentSong and playbackhandler...
	// loopLengthSamples: for scatter modes, the length of the loop region in samples (one bar or 2 beats)
	// halfBar: if true, buffer contains 2 beats that should be virtually doubled for scatter processing
	[[nodiscard]] Error beginStutter(void* source, ParamManagerForTimeline* paramManager, StutterConfig stutterConfig,
	                                 int32_t magnitude, uint32_t timePerTickInverse, size_t loopLengthSamples = 0,
	                                 bool halfBar = false);
	void processStutter(deluge::dsp::StereoBuffer<q31_t> audio, ParamManager* paramManager, int32_t magnitude,
	                    uint32_t timePerTickInverse);
	void endStutter(ParamManagerForTimeline* paramManager = nullptr);

	/// Arm stutter for quantized trigger (starts on next beat)
	/// Returns Error::NONE if armed successfully
	/// loopLengthSamples: for scatter modes, the length of the loop region in samples
	/// halfBar: if true, buffer contains 2 beats that should be virtually doubled for scatter processing
	[[nodiscard]] Error armStutter(void* source, ParamManagerForTimeline* paramManager, StutterConfig stutterConfig,
	                               int32_t magnitude, uint32_t timePerTickInverse, int64_t targetTick,
	                               size_t loopLengthSamples = 0, bool halfBar = false);

	/// Check if armed trigger should fire (call from audio processing)
	/// Returns true if stutter was started
	bool checkArmedTrigger(int64_t currentTick, ParamManager* paramManager, int32_t magnitude,
	                       uint32_t timePerTickInverse);

	/// Cancel armed stutter without starting
	void cancelArmed();

	/// Enable/disable standby recording for instant scatter engagement
	/// When enabled, buffer continuously records audio in ring-buffer fashion
	Error enableStandby(void* source, int32_t magnitude, uint32_t timePerTickInverse);
	void disableStandby();

	/// Feed audio to standby buffer (call during audio processing when standby is active)
	/// Only records if source matches the one that enabled standby
	void recordStandby(void* source, deluge::dsp::StereoBuffer<q31_t> audio);

private:
	enum class Status {
		OFF,
		STANDBY, ///< Buffer allocated and recording, but not affecting output
		ARMED,   ///< Waiting for beat-quantized trigger
		RECORDING,
		PLAYING,
	};
	int32_t getStutterRate(ParamManager* paramManager, int32_t magnitude, uint32_t timePerTickInverse);
	size_t getRepeatSliceLength(ParamManager* paramManager, size_t maxLength);
	bool currentReverse;
	deluge::dsp::DelayBuffer buffer;
	Status status = Status::OFF;
	// TODO: This is currently unused! It's set to 7 initially, and never modified. Either we should set it depending
	// on sync, or get rid of it entirely.
	uint8_t sync = 7;
	StutterConfig stutterConfig;
	int32_t sizeLeftUntilRecordFinished = 0;
	int32_t valueBeforeStuttering = 0;
	int32_t lastQuantizedKnobDiff = 0;
	/// === CLEAN BUFFER OWNERSHIP MODEL ===
	/// Two independent ownership slots - can be same or different sources:
	/// - playSource: who is playing from playBuffer (gets scatter output)
	/// - recordSource: who is recording to recordBuffer (capturing audio)
	///
	/// When different, we have a takeover in progress:
	///   A is playSource (still playing), B is recordSource (preparing to take over)
	/// When B triggers, swap buffers and B becomes both playSource and recordSource.
	void* playSource = nullptr;   ///< Who owns playBuffer (or nullptr)
	void* recordSource = nullptr; ///< Who owns recordBuffer (or nullptr)

	/// Track if we started from standby mode (to return to it after stutter ends)
	bool startedFromStandby = false;

	/// Double buffer system - swap instead of copy on trigger
	deluge::dsp::StereoSample<q31_t>* bufferA = nullptr;
	deluge::dsp::StereoSample<q31_t>* bufferB = nullptr;
	deluge::dsp::StereoSample<q31_t>* recordBuffer = nullptr; ///< Points to buffer being recorded
	deluge::dsp::StereoSample<q31_t>* playBuffer = nullptr;   ///< Points to buffer being played

	size_t recordWritePos = 0;   ///< Current write position in record buffer (ring buffer style)
	size_t playbackStartPos = 0; ///< Where captured bar starts in play buffer (ring buffer offset)
	size_t playbackLength = 0;   ///< Full captured bar length in samples

	/// Slice playback system - flexible enough for complex patterns
	/// A "slice" is a region within the captured bar defined by offset and length.
	/// Future scatter modes can play arbitrary patterns like:
	///   - Reordered beats: [beat3, beat1, beat2, beat4]
	///   - Fractional positions: [1.0, 3.5, 2.0, 4.5]
	///   - Variable lengths: different slice sizes in sequence
	///
	/// For Repeat mode: single slice from end of bar, length controlled by rate knob
	/// For future modes: sliceStartOffset/currentSliceLength set by pattern sequencer
	size_t playbackPos = 0;        ///< Current read offset within current slice
	size_t sliceStartOffset = 0;   ///< Offset from bar start to current slice (in samples)
	size_t currentSliceLength = 0; ///< Length of current slice (in samples)

	/// Convert beat position to sample offset within captured bar
	/// Supports fractional beats (e.g., 2.5 = halfway through beat 3)
	/// @param beat Beat number (0-based, can be fractional)
	/// @param beatsPerBar Number of beats in the bar (typically 4)
	/// @return Sample offset from start of captured bar
	size_t beatToSamples(float beat, size_t beatsPerBar = 4) const {
		if (playbackLength == 0 || beatsPerBar == 0) {
			return 0;
		}
		size_t samplesPerBeat = playbackLength / beatsPerBar;
		return static_cast<size_t>(beat * samplesPerBeat);
	}

	/// Set current slice by beat position and length
	/// @param startBeat Start beat (0-based, can be fractional)
	/// @param lengthBeats Length in beats (can be fractional)
	/// @param beatsPerBar Number of beats in the bar
	void setSliceByBeat(float startBeat, float lengthBeats, size_t beatsPerBar = 4) {
		sliceStartOffset = beatToSamples(startBeat, beatsPerBar);
		currentSliceLength = beatToSamples(lengthBeats, beatsPerBar);
		if (currentSliceLength < 256) {
			currentSliceLength = 256; // Minimum to avoid clicks
		}
	}

	/// Half-bar mode: when bar is too long for buffer, we capture 2 beats and virtually double them
	bool halfBarMode = false;

	/// Scatter/Shuffle state
	int32_t scatterSliceIndex{0}; ///< Current sequential slice (0 to numSlices-1)
	int32_t scatterNumSlices{8};  ///< Number of slices to divide bar into
	bool scatterReversed{false};  ///< Whether current slice is playing reversed
	float scatterDryMix{0};       ///< Per-grain dry crossfade [0,1]: 0=full grain, 1=full dry
	float scatterEnvDepth{0};     ///< Envelope depth [0,1]: 0=hard cut, 1=full envelope
	float scatterEnvShape{0.5f};  ///< Envelope shape [0,1]: 0=fade-out, 0.5=symmetric, 1=fade-in
	float scatterEnvWidth{1.0f};  ///< Envelope region [0,1]: 1=full slice, smaller=edges only
	float scatterGateRatio{1.0f}; ///< Gate duty cycle [0,1]: 1=full slice, smaller=truncated with silence

	/// Stored config for takeover (when recordSource triggers playback)
	StutterConfig armedConfig{};
	size_t armedLoopLengthSamples{0};
	bool armedHalfBarMode{false};
};

// There's only one stutter effect active at a time, so we have a global stutterer to save memory.
// NOTE: Classic mode uses DelayBuffer, scatter modes use double buffers (bufferA/bufferB).
// These are separate memory, so in theory classic + scatter could run simultaneously on
// different tracks. Would require separating the state (status, playSource, recordSource) per mode.
extern Stutterer stutterer;
