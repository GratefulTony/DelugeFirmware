/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
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

#include "model/voice/voice_sample_playback_guide.h"
#include "definitions_cxx.hpp"
#include "model/sample/sample.h"
#include "model/sample/sample_holder_for_voice.h"
#include "processing/source.h"
#include "storage/multi_range/multisample_range.h"
#include <utility>

VoiceSamplePlaybackGuide::VoiceSamplePlaybackGuide() {
}

void VoiceSamplePlaybackGuide::setupPlaybackBounds(bool reversed) {

	SamplePlaybackGuide::setupPlaybackBounds(reversed);

	int32_t loopStartPlaybackAtSample = 0;
	int32_t loopEndPlaybackAtSample = 0;

	// Loop points are only obeyed if not in STRETCH mode
	if (!sequenceSyncLengthTicks) {

		loopStartPlaybackAtSample = reversed ? ((SampleHolderForVoice*)audioFileHolder)->loopEndPos
		                                     : ((SampleHolderForVoice*)audioFileHolder)->loopStartPos;
		loopEndPlaybackAtSample = reversed ? ((SampleHolderForVoice*)audioFileHolder)->loopStartPos
		                                   : ((SampleHolderForVoice*)audioFileHolder)->loopEndPos;

		if (reversed) { // Don't mix this with the above - we want to keep 0s as 0
			if (loopStartPlaybackAtSample) {
				loopStartPlaybackAtSample--;
			}
			if (loopEndPlaybackAtSample) {
				loopEndPlaybackAtSample--;
			}
		}
	}

	loopStartPlaybackAtByte = 0;
	loopEndPlaybackAtByte = 0;

	Sample* sample = (Sample*)audioFileHolder->audioFile;
	int32_t bytesPerSample = sample->numChannels * sample->byteDepth;

	if (loopStartPlaybackAtSample) {
		loopStartPlaybackAtByte = sample->audioDataStartPosBytes + loopStartPlaybackAtSample * bytesPerSample;
	}
	else {
		loopStartPlaybackAtByte = startPlaybackAtByte;
	}

	if (loopEndPlaybackAtSample) {
		loopEndPlaybackAtByte = sample->audioDataStartPosBytes + loopEndPlaybackAtSample * bytesPerSample;
	}
}

// This is, whether to obey the loop-end point as opposed to the actual end-of-sample point (which sometimes might cause
// looping too)
bool VoiceSamplePlaybackGuide::shouldObeyLoopEndPointNow() {
	// During split-loop, the loop end defines the phase boundary and must
	// always be respected (even after note-off) to maintain correct wrapping.
	// For pingpong going backward after note-off, keep obeying the loop end
	// so the voice reaches the boundary, bounces forward, then releases.
	return (loopEndPlaybackAtByte && (!noteOffReceived || loopSplit || (pingpongActive && playDirection == -1)));
}

int32_t VoiceSamplePlaybackGuide::getBytePosToStartPlayback(bool justLooped) {
	if (justLooped && wrapAroundPending) {
		wrapAroundPending = false;
		if (loopSplit) {
			loopWrapPhase = 2; // After initial wrap or phase 1, enter phase 2
		}
		return static_cast<int32_t>(wrapAroundRestartByte);
	}
	if (!justLooped) {
		return SamplePlaybackGuide::getBytePosToStartPlayback(justLooped);
	}
	if (loopSplit && pingpongActive) {
		// Advance the phase and return the NEW phase's start position.
		// This combines what onLoopRestart() used to do (phase advancement)
		// with the position lookup, so that the guide isn't mutated by a
		// separate call — important because the guide is shared between
		// unison readers.
		Sample* sample = static_cast<Sample*>(audioFileHolder->audioFile);
		int32_t bps = sample->byteDepth * sample->numChannels;
		switch (loopWrapPhase) {
		case 1:
			loopWrapPhase = 2;
			return static_cast<int32_t>(wrapAroundRestartByte);
		case 2:
			playDirection = -playDirection;
			loopWrapPhase = 3;
			return static_cast<int32_t>(loopEndPlaybackAtByte) - bps;
		case 3:
			loopWrapPhase = 4;
			return static_cast<int32_t>(endPlaybackAtByte) - bps;
		case 4:
			playDirection = -playDirection;
			loopWrapPhase = 1;
			return loopStartPlaybackAtByte;
		default:
			return loopStartPlaybackAtByte;
		}
	}
	if (pingpongActive && !loopSplit) {
		// Non-split-loop pingpong: flip direction and return the new phase's start.
		// Direction is per-reader (set on the guide before render by voice.cpp).
		Sample* sample = static_cast<Sample*>(audioFileHolder->audioFile);
		int32_t bps = sample->byteDepth * sample->numChannels;
		playDirection = -playDirection;
		if (playDirection == -1) {
			// Was forward, now backward: restart near loop end
			uint32_t endByte = loopEndPlaybackAtByte ? loopEndPlaybackAtByte : endPlaybackAtByte;
			return static_cast<int32_t>(endByte) - bps;
		}
		// Was backward, now forward: restart at loop start
		return loopStartPlaybackAtByte;
	}
	if (loopWrapPhase == 2) {
		if (oneShotWrap) {
			// One full cycle through both phases complete.
			// Mark done so getLoopingType() returns NONE on the next render.
			oneShotWrap = false;
			oneShotComplete = true;
		}
		// Phase 2→1: finished playing sampleStart→loopEnd, restart at loopStart
		loopWrapPhase = 1;
		return loopStartPlaybackAtByte;
	}
	if (loopWrapPhase == 1) {
		// Phase 1→2: finished playing loopStart→sampleEnd, restart at sampleStart
		loopWrapPhase = 2;
		return static_cast<int32_t>(wrapAroundRestartByte);
	}
	return loopStartPlaybackAtByte;
}

// This is actually an important function whose output is the basis for a lot of stuff
int32_t VoiceSamplePlaybackGuide::getBytePosToEndOrLoopPlayback() {
	if (loopSplit && pingpongActive) {
		Sample* sample = static_cast<Sample*>(audioFileHolder->audioFile);
		int32_t bps = sample->byteDepth * sample->numChannels;
		switch (loopWrapPhase) {
		case 1:
			return SamplePlaybackGuide::getBytePosToEndOrLoopPlayback();
		case 2:
			return loopEndPlaybackAtByte;
		case 3:
			return static_cast<int32_t>(wrapAroundRestartByte) - bps;
		case 4:
			return static_cast<int32_t>(loopStartPlaybackAtByte) - bps;
		default:
			return SamplePlaybackGuide::getBytePosToEndOrLoopPlayback();
		}
	}
	if (pingpongActive && !loopSplit) {
		// Non-split-loop pingpong: boundary depends on current direction.
		if (playDirection == 1) {
			// Forward: boundary at loop end (or sample end if note-off)
			if (shouldObeyLoopEndPointNow()) {
				return loopEndPlaybackAtByte;
			}
			return SamplePlaybackGuide::getBytePosToEndOrLoopPlayback();
		}
		// Backward: boundary at loop start
		Sample* sample = static_cast<Sample*>(audioFileHolder->audioFile);
		int32_t bps = sample->byteDepth * sample->numChannels;
		return static_cast<int32_t>(loopStartPlaybackAtByte) - bps;
	}
	if (wrapAroundPending || loopWrapPhase == 1) {
		// Phase 1 or initial wrap: play to sample end
		return SamplePlaybackGuide::getBytePosToEndOrLoopPlayback();
	}
	if (shouldObeyLoopEndPointNow()) {
		// Phase 2 or normal: play to loop end
		return loopEndPlaybackAtByte;
	}
	return SamplePlaybackGuide::getBytePosToEndOrLoopPlayback();
}

LoopType VoiceSamplePlaybackGuide::getLoopingType(const Source& source) const {
	if (loopEndPlaybackAtByte) {
		if (loopSplit) {
			if (oneShotComplete) {
				return LoopType::NONE;
			}
			return LoopType::LOW_LEVEL;
		}
		if (noteOffReceived) {
			// Pingpong going backward: allow one more bounce to forward
			// before stopping, so the release tail plays naturally.
			return (pingpongActive && playDirection == -1) ? LoopType::LOW_LEVEL : LoopType::NONE;
		}
		return LoopType::LOW_LEVEL;
	}
	if (isLoopingRepeatMode(source.repeatMode)) {
		return LoopType::LOW_LEVEL;
	}
	// Enable looping for STRETCH mode when start offset is active,
	// so the time stretcher wraps around instead of stopping at the sample end
	if (wrapSyncPosition) {
		return LoopType::LOW_LEVEL;
	}
	return LoopType::NONE;
}

void VoiceSamplePlaybackGuide::onLoopRestart() {
	// All phase advancement and pingpong handling is now done inside
	// getBytePosToStartPlayback(true), which is called by
	// setupClusersForInitialPlay during loop-back. This avoids mutating
	// the shared guide from a low-level reader call site, which corrupts
	// boundaries for other unison readers that haven't hit the boundary yet.
}
