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

#include "model/fx/stutterer.h"
#include "dsp/scatter.hpp"
#include "dsp_ng/core/types.hpp"
#include "io/debug/fx_benchmark.h"
#include "memory/memory_allocator_interface.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "util/functions.h"
#include <algorithm>
#include <cstring>

namespace params = deluge::modulation::params;

Stutterer stutterer{};

void Stutterer::initParams(ParamManager* paramManager) {
	paramManager->getUnpatchedParamSet()->params[params::UNPATCHED_STUTTER_RATE].setCurrentValueBasicForSetup(0);
}

int32_t Stutterer::getStutterRate(ParamManager* paramManager, int32_t magnitude, uint32_t timePerTickInverse) {
	UnpatchedParamSet* unpatchedParams = paramManager->getUnpatchedParamSet();
	int32_t paramValue = unpatchedParams->getValue(params::UNPATCHED_STUTTER_RATE);

	// Quantized Stutter diff
	int32_t knobPos = unpatchedParams->paramValueToKnobPos(paramValue, nullptr);
	knobPos = knobPos + lastQuantizedKnobDiff;
	if (knobPos < -64) {
		knobPos = -64;
	}
	else if (knobPos > 64) {
		knobPos = 64;
	}
	paramValue = unpatchedParams->knobPosToParamValue(knobPos, nullptr);

	int32_t rate =
	    getFinalParameterValueExp(paramNeutralValues[params::GLOBAL_DELAY_RATE], cableToExpParamShortcut(paramValue));

	if (sync != 0) {
		rate = multiply_32x32_rshift32(rate, timePerTickInverse);
		int32_t lShiftAmount = sync + 6 - magnitude;
		int32_t limit = 2147483647 >> lShiftAmount;
		rate = std::min(rate, limit);
		rate <<= lShiftAmount;
	}
	return rate;
}

/// Calculate slice length for Repeat mode based on rate knob
/// Higher rate = smaller slice = faster repeats (from end of buffer)
size_t Stutterer::getRepeatSliceLength(ParamManager* paramManager, size_t maxLength) {
	UnpatchedParamSet* unpatchedParams = paramManager->getUnpatchedParamSet();
	int32_t paramValue = unpatchedParams->getValue(params::UNPATCHED_STUTTER_RATE);
	int32_t knobPos = unpatchedParams->paramValueToKnobPos(paramValue, nullptr);
	// knobPos ranges from -64 to +64
	// Map to slice length: -64 = full bar, +64 = minimum slice
	// Use exponential curve for musical divisions

	// Normalize knobPos to 0-128 range, then to 0.0-1.0
	// Higher knobPos = smaller slice
	int32_t normalized = 64 - knobPos; // 0 at +64, 128 at -64
	if (normalized < 0) {
		normalized = 0;
	}
	if (normalized > 128) {
		normalized = 128;
	}

	// Exponential mapping: slice = maxLength * (normalized/128)^2
	// This gives finer control over small slices
	// At normalized=128 (knob=-64): full length
	// At normalized=64 (knob=0): 1/4 length
	// At normalized=0 (knob=+64): minimum
	constexpr size_t kMinSlice = 256; // ~6ms minimum to avoid clicks

	size_t sliceLength = (maxLength * normalized * normalized) / (128 * 128);
	if (sliceLength < kMinSlice) {
		sliceLength = kMinSlice;
	}
	if (sliceLength > maxLength) {
		sliceLength = maxLength;
	}
	return sliceLength;
}

Error Stutterer::beginStutter(void* source, ParamManagerForTimeline* paramManager, StutterConfig sc, int32_t magnitude,
                              uint32_t timePerTickInverse, size_t loopLengthSamples, bool halfBar) {
	// Note: We don't auto-unlatch the old source here because stutterSource is a void*
	// and we can't safely cast it back - the old source may have been deleted.
	// Users need to manually switch from Latch to Momentary if they want to stop.

	stutterConfig = sc;
	currentReverse = stutterConfig.reversed;
	halfBarMode = halfBar;

	// Non-Classic modes: double buffer system (swap instead of copy)
	bool useLooper = (stutterConfig.scatterMode != ScatterMode::Classic);
	if (useLooper) {
		// If we have buffers from standby recording, swap and start playback
		if (recordBuffer != nullptr && loopLengthSamples > 0) {
			// Clamp loop length to buffer size
			// KNOWN ISSUE: If user releases and re-triggers before a full bar has been recorded,
			// playback may include stale audio from before the previous trigger.
			// Fix if needed: clamp to recordWritePos: std::min({loopLengthSamples, kLooperBufferSize, recordWritePos})
			playbackLength = std::min(loopLengthSamples, kLooperBufferSize);

			// Calculate where loop starts in the record buffer (which becomes play buffer)
			// recordWritePos is where we WOULD write next, so loop ends there
			if (recordWritePos >= playbackLength) {
				playbackStartPos = recordWritePos - playbackLength;
			}
			else {
				playbackStartPos = kLooperBufferSize - (playbackLength - recordWritePos);
			}

			// Swap buffers - no copy needed!
			std::swap(recordBuffer, playBuffer);

			// Reset for playback and new recording
			playbackPos = 0;
			recordWritePos = 0;
			// Initialize slice to full length; will be updated at first sample for Repeat mode
			currentSliceLength = playbackLength;
			sliceStartOffset = 0;
			// Reset scatter state
			scatterSliceIndex = 0;
			scatterReversed = false;
			status = Status::PLAYING;
			stutterSource = source;
			return Error::NONE;
		}

		// No buffers - allocate both and start standby recording
		if (bufferA == nullptr) {
			bufferA = static_cast<deluge::dsp::StereoSample<q31_t>*>(
			    allocLowSpeed(kLooperBufferSize * sizeof(deluge::dsp::StereoSample<q31_t>)));
			if (bufferA == nullptr) {
				status = Status::OFF;
				return Error::INSUFFICIENT_RAM;
			}
		}
		if (bufferB == nullptr) {
			bufferB = static_cast<deluge::dsp::StereoSample<q31_t>*>(
			    allocLowSpeed(kLooperBufferSize * sizeof(deluge::dsp::StereoSample<q31_t>)));
			if (bufferB == nullptr) {
				status = Status::OFF;
				return Error::INSUFFICIENT_RAM;
			}
		}
		recordBuffer = bufferA;
		playBuffer = bufferB;
		recordWritePos = 0;
		status = Status::STANDBY;
		stutterSource = source;
		return Error::NONE;
	}

	// Classic mode: original community behavior
	// Quantized snapping
	if (stutterConfig.quantized) {
		UnpatchedParamSet* unpatchedParams = paramManager->getUnpatchedParamSet();
		int32_t paramValue = unpatchedParams->getValue(params::UNPATCHED_STUTTER_RATE);
		int32_t knobPos = unpatchedParams->paramValueToKnobPos(paramValue, nullptr);
		if (knobPos < -39) {
			knobPos = -16; // 4ths
		}
		else if (knobPos < -14) {
			knobPos = -8; // 8ths
		}
		else if (knobPos < 14) {
			knobPos = 0; // 16ths
		}
		else if (knobPos < 39) {
			knobPos = 8; // 32nds
		}
		else {
			knobPos = 16; // 64ths
		}
		valueBeforeStuttering = paramValue;
		lastQuantizedKnobDiff = knobPos;
		unpatchedParams->params[params::UNPATCHED_STUTTER_RATE].setCurrentValueBasicForSetup(0);
	}

	startedFromStandby = false;
	Error error = buffer.init(getStutterRate(paramManager, magnitude, timePerTickInverse), 0, true);
	if (error == Error::NONE) {
		status = Status::RECORDING;
		sizeLeftUntilRecordFinished = buffer.size();
		stutterSource = source;
	}
	return error;
}

/// Mode name tags for benchmarking
static constexpr const char* kScatterModeNames[] = {
    "classic", "repeat", "reverse", "chop", "shuffle", "tape", "pitch", "filter",
};

void Stutterer::processStutter(deluge::dsp::StereoBuffer<q31_t> audio, ParamManager* paramManager, int32_t magnitude,
                               uint32_t timePerTickInverse) {

	// Non-Classic modes: double buffer - play from playBuffer, record to recordBuffer
	// Core loop: play current slice fully, then get next slice at boundary
	bool useLooper = (stutterConfig.scatterMode != ScatterMode::Classic);
	if (useLooper) {
		if (status == Status::PLAYING && playBuffer != nullptr && playbackLength > 0) {
			// Benchmark: total scatter processing time
			FX_BENCH_DECLARE(benchTotal, "scatter", "total");
			FX_BENCH_DECLARE(benchSlice, "scatter", "slice");
			FX_BENCH_SET_TAG(benchTotal, 0, kScatterModeNames[static_cast<int>(stutterConfig.scatterMode)]);
			FX_BENCH_SET_TAG(benchSlice, 0, kScatterModeNames[static_cast<int>(stutterConfig.scatterMode)]);
			FX_BENCH_START(benchTotal);

			for (deluge::dsp::StereoSample<q31_t>& sample : audio) {
				// Record incoming audio to recordBuffer (continuous recording)
				if (recordBuffer != nullptr) {
					recordBuffer[recordWritePos] = sample;
					recordWritePos++;
					if (recordWritePos >= kLooperBufferSize) {
						recordWritePos = 0;
					}
				}

				// === SLICE BOUNDARY: get next slice parameters ===
				// This is where each mode determines what to play next.
				// Slice changes ONLY happen here, ensuring complete playback.
				// Future modes: advance pattern index, call setSliceByBeat(), etc.
				if (playbackPos == 0) {
					FX_BENCH_START(benchSlice);
					switch (stutterConfig.scatterMode) {
					case ScatterMode::Repeat:
						// Rate knob controls slice length from END of captured bar
						currentSliceLength = getRepeatSliceLength(paramManager, playbackLength);
						sliceStartOffset = playbackLength - currentSliceLength;
						break;

					case ScatterMode::Shuffle: {
						// Rate knob controls number of slices (2-16)
						UnpatchedParamSet* unpatchedParams = paramManager->getUnpatchedParamSet();
						int32_t rateParam = unpatchedParams->getValue(params::UNPATCHED_STUTTER_RATE);
						int32_t knobPos = unpatchedParams->paramValueToKnobPos(rateParam, nullptr);
						// Map -64..+64 to 2..16 slices
						scatterNumSlices = 2 + ((knobPos + 64) * 14) / 128;
						scatterNumSlices = std::clamp(scatterNumSlices, int32_t{2}, int32_t{16});

						// Read zone params from menu
						int32_t zoneAParam = unpatchedParams->getValue(params::UNPATCHED_SCATTER_ZONE_A);
						int32_t zoneBParam = unpatchedParams->getValue(params::UNPATCHED_SCATTER_ZONE_B);
						int32_t depthParam = unpatchedParams->getValue(params::UNPATCHED_SCATTER_DEPTH);

						// Compute grain params - phi triangle deadzone in computeGrainParams
						// makes many consecutive slices equivalent, so we always call but
						// the result will be identical when sliceIndex is in deadzone
						float zoneA = deluge::dsp::scatter::ScatterParams::paramToNormalized(zoneAParam);
						float zoneB = deluge::dsp::scatter::ScatterParams::paramToNormalized(zoneBParam);
						float depth = deluge::dsp::scatter::ScatterParams::paramToNormalized(depthParam);

						auto grain = deluge::dsp::scatter::computeGrainParams(zoneA, zoneB, depth, scatterSliceIndex);

						// Calculate target slice from sequential index + offset
						int32_t targetSlice = scatterSliceIndex;

						// Apply slice offset (phi-modulated in meta zones)
						int32_t offsetSlices = static_cast<int32_t>(grain.sliceOffset * scatterNumSlices);
						targetSlice = (targetSlice + offsetSlices) % scatterNumSlices;

						// Skip probability - jump to quasi-random slice
						if (grain.skipProb > 0) {
							float skipRoll = deluge::dsp::phi::wrapPhase(static_cast<float>(scatterSliceIndex) * 7.3f);
							if (skipRoll < grain.skipProb) {
								targetSlice = static_cast<int32_t>(skipRoll / grain.skipProb * scatterNumSlices);
								targetSlice = targetSlice % scatterNumSlices;
							}
						}

						// Set slice parameters
						currentSliceLength = playbackLength / scatterNumSlices;
						currentSliceLength =
						    static_cast<size_t>(currentSliceLength * std::clamp(grain.lengthMult, 0.25f, 2.0f));
						if (currentSliceLength < 256) {
							currentSliceLength = 256;
						}
						sliceStartOffset = targetSlice * (playbackLength / scatterNumSlices);

						// Reverse based on probability
						float revRoll = deluge::dsp::phi::wrapPhase(static_cast<float>(scatterSliceIndex) * 3.7f);
						scatterReversed = (revRoll < grain.reverseProb);

						// Advance for next slice
						scatterSliceIndex = (scatterSliceIndex + 1) % scatterNumSlices;
						break;
					}

					default:
						// Default: play full bar
						currentSliceLength = playbackLength;
						sliceStartOffset = 0;
						break;
					}
					FX_BENCH_STOP(benchSlice);
				}

				// === PLAYBACK: read from current slice ===
				size_t readPos;
				if (scatterReversed && stutterConfig.scatterMode == ScatterMode::Shuffle) {
					// Reverse: read from end of slice going backward
					readPos = playbackStartPos + sliceStartOffset + (currentSliceLength - 1 - playbackPos);
				}
				else {
					readPos = playbackStartPos + sliceStartOffset + playbackPos;
				}
				if (readPos >= kLooperBufferSize) {
					readPos -= kLooperBufferSize;
				}
				sample.l = playBuffer[readPos].l;
				sample.r = playBuffer[readPos].r;

				// === ADVANCE: move through slice, wrap at boundary ===
				playbackPos++;
				if (playbackPos >= currentSliceLength) {
					playbackPos = 0; // Triggers next slice selection on next sample
				}
			}

			FX_BENCH_STOP(benchTotal);
		}
		return;
	}

	// Classic mode: original community behavior with resampling
	// Benchmark: classic stutter processing (separate from scatter modes)
	FX_BENCH_DECLARE(benchClassic, "stutter", "classic");
	FX_BENCH_SCOPE(benchClassic);

	int32_t rate = getStutterRate(paramManager, magnitude, timePerTickInverse);
	buffer.setupForRender(rate);

	if (status == Status::RECORDING) {
		for (deluge::dsp::StereoSample<q31_t> sample : audio) {
			int32_t strength1;
			int32_t strength2;

			if (buffer.isNative()) {
				buffer.clearAndMoveOn();
				sizeLeftUntilRecordFinished--;
			}
			else {
				strength2 = buffer.advance([&] {
					buffer.clearAndMoveOn();
					sizeLeftUntilRecordFinished--;
				});
				strength1 = 65536 - strength2;
			}

			buffer.write(sample, strength1, strength2);
		}

		if (sizeLeftUntilRecordFinished < 0) {
			if (currentReverse) {
				buffer.setCurrent(buffer.end() - 1);
			}
			else {
				buffer.setCurrent(buffer.begin());
			}
			status = Status::PLAYING;
		}
	}
	else { // PLAYING
		for (deluge::dsp::StereoSample<q31_t>& sample : audio) {
			int32_t strength1;
			int32_t strength2;

			if (buffer.isNative()) {
				if (currentReverse) {
					buffer.moveBack();
				}
				else {
					buffer.moveOn();
				}
				sample.l = buffer.current().l;
				sample.r = buffer.current().r;
			}
			else {
				if (currentReverse) {
					strength2 = buffer.retreat([&] { buffer.moveBack(); });
				}
				else {
					strength2 = buffer.advance([&] { buffer.moveOn(); });
				}

				strength1 = 65536 - strength2;

				if (currentReverse) {
					deluge::dsp::StereoSample<q31_t>* prevPos = &buffer.current() - 1;
					if (prevPos < buffer.begin()) {
						prevPos = buffer.end() - 1;
					}
					deluge::dsp::StereoSample<q31_t>& fromDelay1 = buffer.current();
					deluge::dsp::StereoSample<q31_t>& fromDelay2 = *prevPos;
					sample.l = (multiply_32x32_rshift32(fromDelay1.l, strength1 << 14)
					            + multiply_32x32_rshift32(fromDelay2.l, strength2 << 14))
					           << 2;
					sample.r = (multiply_32x32_rshift32(fromDelay1.r, strength1 << 14)
					            + multiply_32x32_rshift32(fromDelay2.r, strength2 << 14))
					           << 2;
				}
				else {
					deluge::dsp::StereoSample<q31_t>* nextPos = &buffer.current() + 1;
					if (nextPos == buffer.end()) {
						nextPos = buffer.begin();
					}
					deluge::dsp::StereoSample<q31_t>& fromDelay1 = buffer.current();
					deluge::dsp::StereoSample<q31_t>& fromDelay2 = *nextPos;
					sample.l = (multiply_32x32_rshift32(fromDelay1.l, strength1 << 14)
					            + multiply_32x32_rshift32(fromDelay2.l, strength2 << 14))
					           << 2;
					sample.r = (multiply_32x32_rshift32(fromDelay1.r, strength1 << 14)
					            + multiply_32x32_rshift32(fromDelay2.r, strength2 << 14))
					           << 2;
				}
			}

			// Ping-pong
			if (stutterConfig.pingPong
			    && ((currentReverse && &buffer.current() == buffer.begin())
			        || (!currentReverse && &buffer.current() == buffer.end() - 1))) {
				currentReverse = !currentReverse;
			}
		}
	}
}

void Stutterer::endStutter(ParamManagerForTimeline* paramManager) {
	bool isScatterMode = (stutterConfig.scatterMode != ScatterMode::Classic);

	if (isScatterMode) {
		// Non-Classic modes: return to standby for continuous recording
		// Don't clear buffer - old audio will be overwritten during standby recording
		// and clearing 1.7MB causes audio glitches
		// Playback buffer is kept allocated for next trigger
		playbackPos = 0;
		status = Status::STANDBY;
		return;
	}

	// Classic mode: original community behavior
	if (startedFromStandby) {
		status = Status::STANDBY;
		buffer.setCurrent(buffer.begin() + deluge::dsp::delaySpaceBetweenReadAndWrite);
		startedFromStandby = false;
	}
	else {
		buffer.discard();
		status = Status::OFF;
	}

	if (paramManager) {
		UnpatchedParamSet* unpatchedParams = paramManager->getUnpatchedParamSet();

		if (stutterConfig.quantized) {
			unpatchedParams->params[params::UNPATCHED_STUTTER_RATE].setCurrentValueBasicForSetup(valueBeforeStuttering);
		}
		else {
			if (unpatchedParams->getValue(params::UNPATCHED_STUTTER_RATE) < 0) {
				unpatchedParams->params[params::UNPATCHED_STUTTER_RATE].setCurrentValueBasicForSetup(0);
			}
		}
	}
	lastQuantizedKnobDiff = 0;
	valueBeforeStuttering = 0;
	if (!startedFromStandby && !isScatterMode) {
		stutterSource = nullptr;
	}
}

Error Stutterer::enableStandby(void* source, int32_t magnitude, uint32_t timePerTickInverse) {
	if (status == Status::STANDBY && stutterSource == source) {
		return Error::NONE;
	}

	if (status == Status::RECORDING || status == Status::PLAYING) {
		return Error::UNSPECIFIED;
	}

	if (status == Status::STANDBY) {
		buffer.discard();
	}

	// Allocate ring buffer for continuous recording
	Error error = buffer.initWithSize(kLooperBufferSize, false);
	if (error != Error::NONE) {
		return error;
	}
	buffer.setCurrent(buffer.begin());

	status = Status::STANDBY;
	stutterSource = source;
	return Error::NONE;
}

void Stutterer::disableStandby() {
	if (status == Status::STANDBY) {
		// Classic mode: discard delay buffer
		buffer.discard();

		// Non-Classic modes: deallocate double buffers
		if (bufferA != nullptr) {
			delugeDealloc(bufferA);
			bufferA = nullptr;
		}
		if (bufferB != nullptr) {
			delugeDealloc(bufferB);
			bufferB = nullptr;
		}
		recordBuffer = nullptr;
		playBuffer = nullptr;

		status = Status::OFF;
		stutterSource = nullptr;
	}
}

void Stutterer::recordStandby(void* source, deluge::dsp::StereoBuffer<q31_t> audio) {
	// Non-Classic modes: use double buffer system
	bool useLooper = (stutterConfig.scatterMode != ScatterMode::Classic);

	// For non-Classic modes, record during STANDBY, ARMED, and PLAYING (continuous recording)
	if (useLooper) {
		if (status != Status::STANDBY && status != Status::ARMED && status != Status::PLAYING) {
			return;
		}
	}
	else {
		// Classic mode: only record during STANDBY
		if (status != Status::STANDBY) {
			return;
		}
	}

	// Only record from the source that owns this stutter session
	if (source != stutterSource) {
		return;
	}

	if (useLooper && recordBuffer != nullptr) {
		for (deluge::dsp::StereoSample<q31_t> sample : audio) {
			recordBuffer[recordWritePos].l = sample.l;
			recordBuffer[recordWritePos].r = sample.r;
			recordWritePos++;
			if (recordWritePos >= kLooperBufferSize) {
				recordWritePos = 0;
			}
		}
		return;
	}

	// Classic mode: use delay buffer
	for (deluge::dsp::StereoSample<q31_t> sample : audio) {
		buffer.current().l = sample.l;
		buffer.current().r = sample.r;
		buffer.moveOn();
	}
}

Error Stutterer::armStutter(void* source, ParamManagerForTimeline* paramManager, StutterConfig sc, int32_t magnitude,
                            uint32_t timePerTickInverse, int64_t targetTick, size_t loopLengthSamples, bool halfBar) {
	if (status == Status::RECORDING || status == Status::PLAYING) {
		return Error::UNSPECIFIED;
	}

	startedFromStandby = (status == Status::STANDBY && stutterSource == source);

	// Store armed state
	armedTargetTick = targetTick;
	armedConfig = sc;
	armedHalfBarMode = halfBar;
	armedMagnitude = magnitude;
	armedTimePerTickInverse = timePerTickInverse;
	armedParamManager = paramManager;
	armedLoopLengthSamples = loopLengthSamples;
	stutterSource = source;
	status = Status::ARMED;

	return Error::NONE;
}

bool Stutterer::checkArmedTrigger(int64_t currentTick, ParamManager* paramManager, int32_t magnitude,
                                  uint32_t timePerTickInverse) {
	if (status != Status::ARMED) {
		return false;
	}

	if (currentTick >= armedTargetTick) {
		stutterConfig = armedConfig;
		currentReverse = stutterConfig.reversed;
		halfBarMode = armedHalfBarMode;

		// Non-Classic modes: swap buffers (same logic as beginStutter)
		bool useLooper = (stutterConfig.scatterMode != ScatterMode::Classic);
		if (useLooper) {
			// If we have buffers, swap and start playback
			// See KNOWN ISSUE comment in beginStutter re: stale audio on rapid re-trigger
			if (recordBuffer != nullptr && armedLoopLengthSamples > 0) {
				playbackLength = std::min(armedLoopLengthSamples, kLooperBufferSize);

				// Calculate where loop starts in record buffer (which becomes play buffer)
				if (recordWritePos >= playbackLength) {
					playbackStartPos = recordWritePos - playbackLength;
				}
				else {
					playbackStartPos = kLooperBufferSize - (playbackLength - recordWritePos);
				}

				// Swap buffers - no copy needed!
				std::swap(recordBuffer, playBuffer);

				playbackPos = 0;
				recordWritePos = 0;
				// Initialize slice to full length; will be updated at first sample for Repeat mode
				currentSliceLength = playbackLength;
				sliceStartOffset = 0;
				// Reset scatter state
				scatterSliceIndex = 0;
				scatterReversed = false;
				status = Status::PLAYING;
				armedParamManager = nullptr;
				armedLoopLengthSamples = 0;
				return true;
			}

			// No buffers - allocate both and start standby
			if (bufferA == nullptr) {
				bufferA = static_cast<deluge::dsp::StereoSample<q31_t>*>(
				    allocLowSpeed(kLooperBufferSize * sizeof(deluge::dsp::StereoSample<q31_t>)));
				if (bufferA == nullptr) {
					status = Status::OFF;
					stutterSource = nullptr;
					armedParamManager = nullptr;
					return false;
				}
			}
			if (bufferB == nullptr) {
				bufferB = static_cast<deluge::dsp::StereoSample<q31_t>*>(
				    allocLowSpeed(kLooperBufferSize * sizeof(deluge::dsp::StereoSample<q31_t>)));
				if (bufferB == nullptr) {
					status = Status::OFF;
					stutterSource = nullptr;
					armedParamManager = nullptr;
					return false;
				}
			}
			recordBuffer = bufferA;
			playBuffer = bufferB;
			recordWritePos = 0;
			status = Status::STANDBY;
			armedParamManager = nullptr;
			armedLoopLengthSamples = 0;
			return true;
		}

		// Classic mode: use DelayBuffer with resampling
		int32_t rate = getStutterRate(armedParamManager, armedMagnitude, armedTimePerTickInverse);
		Error error = buffer.init(rate, 0, true);
		if (error == Error::NONE) {
			status = Status::RECORDING;
			sizeLeftUntilRecordFinished = buffer.size();
			armedParamManager = nullptr;
			return true;
		}

		// Failed to allocate
		status = Status::OFF;
		stutterSource = nullptr;
		armedParamManager = nullptr;
		return false;
	}

	return false;
}

void Stutterer::cancelArmed() {
	if (status == Status::ARMED) {
		if (startedFromStandby) {
			status = Status::STANDBY;
		}
		else {
			buffer.discard();
			status = Status::OFF;
			stutterSource = nullptr;
		}
		armedParamManager = nullptr;
	}
}
