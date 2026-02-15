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
	return (loopEndPlaybackAtByte && !noteOffReceived);
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
	if (loopWrapPhase == 2) {
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
	if (wrapAroundPending || loopWrapPhase == 1 || (noteOffReceived && releaseWrapPending)) {
		// Phase 1, initial wrap, or release wrap: play to original sample end
		return sampleEndByte ? static_cast<int32_t>(sampleEndByte)
		                     : SamplePlaybackGuide::getBytePosToEndOrLoopPlayback();
	}
	if (shouldObeyLoopEndPointNow()) {
		// Phase 2 or normal: play to loop end
		return loopEndPlaybackAtByte;
	}
	// After release wrap completes: endPlaybackAtByte is the shifted end
	return SamplePlaybackGuide::getBytePosToEndOrLoopPlayback();
}

LoopType VoiceSamplePlaybackGuide::getLoopingType(const Source& source) const {
	if (loopEndPlaybackAtByte) {
		if (noteOffReceived) {
			// Allow one more wrap cycle so the release tail crosses the sample boundary
			return releaseWrapPending ? LoopType::LOW_LEVEL : LoopType::NONE;
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
	if (wrapAroundPending) {
		return; // Flag cleared in getBytePosToStartPlayback
	}
	if (noteOffReceived && releaseWrapPending) {
		// Release wrap: played to original sample end, now wrap to sample start
		releaseWrapPending = false;
		loopWrapPhase = 0;
		loopSplit = false;
		wrapAroundPending = true; // getBytePosToStartPlayback will return sampleStart
		return;
	}
	if (loopSplit) {
		return; // Phase transitions handled in getBytePosToStartPlayback
	}
	if (pingpongActive) {
		playDirection = -playDirection;
		std::swap(loopStartPlaybackAtByte, loopEndPlaybackAtByte);
		std::swap(startPlaybackAtByte, endPlaybackAtByte);
	}
}
