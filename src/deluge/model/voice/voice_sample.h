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

#include "definitions_cxx.hpp"
#include "model/sample/sample_low_level_reader.h"

enum class LateStartAttemptStatus {
	SUCCESS = 0,
	FAILURE = 1,
	WAIT = 2,
};
class TimeStretcher;
class SampleCache;
class Sample;
class Voice;
class VoiceSamplePlaybackGuide;
class Source;
class SampleControls;

class VoiceSample final : public SampleLowLevelReader {
public:
	VoiceSample() = default;
	explicit VoiceSample(VoiceSample& other) = default;
	VoiceSample(SampleLowLevelReader&& other) : SampleLowLevelReader{std::move(other)} {}
	~VoiceSample() override { endTimeStretching(); }
	VoiceSample& operator=(const VoiceSample& other) = delete;
	VoiceSample& operator=(VoiceSample&& other) = default;

	void noteOn(SamplePlaybackGuide* guide, uint32_t samplesLate, int32_t priorityRating);
	bool noteOffWhenLoopEndPointExists(Voice* voice, VoiceSamplePlaybackGuide* voiceSource);

	void setupCacheLoopPoints(SamplePlaybackGuide* voiceSource, Sample* sample, LoopType loopingType);
	LateStartAttemptStatus attemptLateSampleStart(SamplePlaybackGuide* voiceSource, Sample* sample,
	                                              int64_t rawSamplesLate, int32_t numSamples = 0);
	void endTimeStretching();
	bool render(SamplePlaybackGuide* guide, int32_t* oscBuffer, int32_t numSamples, Sample* sample, int32_t numChannels,
	            LoopType loopingType, int32_t phaseIncrement, int32_t timeStretchRatio, int32_t amplitude,
	            int32_t amplitudeIncrement, int32_t bufferSize, InterpolationMode desiredInterpolationMode,
	            int32_t priorityRating);
	void beenUnassigned(bool wontBeUsedAgain);

	// AudioClips don't obey markers because they "fudge" instead.
	// Or if fudging can't happen cos no pre-margin, then
	// AudioClip::doTickForward() manually forces restart.
	[[nodiscard]] bool shouldObeyMarkers() const override {
		return (cache == nullptr && timeStretcher == nullptr && !forAudioClip);
	}

	void readSamplesResampledPossiblyCaching(int32_t** oscBufferPos, int32_t** oscBufferRPos, int32_t numSamples,
	                                         Sample* sample, int32_t jumpAmount, int32_t numChannels,
	                                         int32_t numChannelsAfterCondensing, int32_t phaseIncrement,
	                                         int32_t* sourceAmplitudeNow, int32_t amplitudeIncrement,
	                                         int32_t bufferSize, int32_t reduceMagnitudeBy = 1);

	bool sampleZoneChanged(SamplePlaybackGuide* voiceSource, Sample* sample, bool reversed, MarkerType markerType,
	                       LoopType loopingType, int32_t priorityRating, bool forAudioClip = false);
	int32_t getPlaySample(Sample* sample, SamplePlaybackGuide* guide);
	bool stopUsingCache(SamplePlaybackGuide* guide, Sample* sample, int32_t priorityRating, bool loopingAtLowLevel);
	bool possiblySetUpOffsetLoopCache(SampleControls* sampleControls, SamplePlaybackGuide* guide,
	                                  int32_t phaseIncrement, int32_t timeStretchRatio, LoopType loopingType,
	                                  int32_t priorityRating);
	bool possiblySetUpCache(SampleControls* sampleControls, SamplePlaybackGuide* guide, int32_t phaseIncrement,
	                        int32_t timeStretchRatio, int32_t priorityRating, LoopType loopingType);
	bool fudgeTimeStretchingToAvoidClick(Sample* sample, SamplePlaybackGuide* guide, int32_t phaseIncrement,
	                                     int32_t numSamplesTilLoop, int32_t playDirection, int32_t priorityRating);

	uint32_t pendingSamplesLate = 0; // This isn't used for AudioClips. And for samples in STRETCH mode, the exact
	                                 // number isn't relevant - it gets recalculated
	TimeStretcher* timeStretcher = nullptr;

	SampleCache* cache = nullptr;
	bool doneFirstRenderYet = false;
	bool fudging = false;
	bool forAudioClip = false;       // This is a wee bit of a hack - but we need to be able to know this
	bool writingToCache = false;     // Value is only valid if cache assigned
	int8_t pingpongPlayDirection{1}; // Per-reader direction for pingpong mode (1=forward, -1=backward)
	int32_t loopFadeInSamplesRemaining{0};
	int32_t loopFadeInSamplesTotal{0};
	int32_t loopFadeStepQ31{0}; // 0x7FFFFFFF / loopFadeInSamplesTotal, kept in sync by Voice
	// Adaptive crossfade curve: blend between linear (0, equal-gain, for correlated
	// material) and sqrt (0x7FFFFFFF, equal-power, for uncorrelated material) fade
	// shapes. Starts at the 50/50 compromise; measured from the cached loop regions at
	// the first cached-crossfade trigger (measureCrossfadeCurve).
	int32_t crossfadeCurveBlendQ31{0x40000000};
	bool crossfadeCurveMeasured{false};
	bool pingpongBouncePointsSnapped{false};
	// Pingpong bounce-apex seek ("wait for an extremum"): while >= 0, the first pass
	// keeps writing past the raw loop end toward this target, which gets refined onto
	// the next predicted waveform extremum as slope flips are observed
	int32_t pingpongSeekTargetBytes{-1};
	int32_t pingpongSeekScannedToBytes{0};
	int32_t pingpongSeekFlip1Bytes{-1};
	int32_t pingpongSeekFlip2Bytes{-1};
	int32_t pingpongSeekPrevSample{0};
	int32_t pingpongSeekPrevDiff{0};
	int32_t crossfadeCacheBytePos{0};
	bool crossfadeActive{false};
	bool cacheHandoffPending{false}; // Attach a loop-start-keyed cache at the first loop restart (start offset
	                                 // made the first pass asymmetric, so caching couldn't begin at note-on)

private:
	bool weShouldBeTimeStretchingNow(Sample* sample, SamplePlaybackGuide* guide, int32_t numSamples,
	                                 int32_t phaseIncrement, int32_t timeStretchRatio, int32_t playDirection,
	                                 int32_t priorityRating, LoopType loopingType);
	void switchToReadingCacheFromWriting();
	bool stopReadingFromCache();
	bool attachCacheAtLoopStart(SamplePlaybackGuide* guide, Sample* sample, int32_t phaseIncrement,
	                            int32_t timeStretchRatio, int32_t interpolationBufferSize, LoopType loopingType,
	                            int32_t priorityRating);
	void measureCrossfadeCurve(int32_t crossfadeLengthCacheBytes, int32_t frameSizeBytes);
	void snapPingpongBouncePoints(int32_t frameSizeBytes);
	void snapPingpongLoopStart(int32_t frameSizeBytes);

	int32_t cacheBytePos = 0;
	int8_t cachePlayDirection{1}; // Direction for reading cache in pingpong mode (1=forward, -1=backward)

	// Cached fade-out proximity threshold: skips the exact (division-heavy) distance
	// computation while the play position is far from the loop boundary
	int32_t fadeZoneThresholdBytes{0};
	int32_t fadeZoneLastPhaseIncrement{0};
	int32_t fadeZoneLastTotal{-1};

	uint32_t cacheLoopLengthBytes = 0;
	int32_t cacheLoopStartPointBytes{0}; // Cache byte position of loop start (for pingpong backward boundary)
	int32_t cacheLoopEndPointBytes = 0;  // 2147483647 means no looping. Will be set to sample end-point if looping
	                                     // there. Gets re-set to 2147483647 when note "released"
	int32_t cacheEndPointBytes = 0; // Will sometimes be the whole length of the sample. Wherever the red marker is. Or
	                                // a little further if it's the full length of the sample, to allow for timestretch
	                                // / interpolation ring-out
};
