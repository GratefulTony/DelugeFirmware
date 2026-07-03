/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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
#include "model/sample/sample_controls.h"
#include "storage/multi_range/multi_range_array.h"
#include "util/phase_increment_fine_tuner.h"

namespace deluge::dsp {
struct PhiMorphCache;
struct PhiWeaveCache;
struct PhiVoxCache;
struct PhiSwarmCache;
struct PhiGendyCache;
} // namespace deluge::dsp

class Sound;
class ParamManagerForTimeline;
class WaveTable;
class SampleHolder;
class DxPatch;

class Source {
public:
	Source();
	~Source();

	SampleControls sampleControls;

	OscType oscType;

	// These are not valid for Samples
	int16_t transpose;
	int8_t cents;
	PhaseIncrementFineTuner fineTuner;

	MultiRangeArray ranges;

	DxPatch* dxPatch;
	bool dxPatchChanged = false;
	SampleRepeatMode repeatMode;

	// PHI_MORPH zone parameters and cache (lazily allocated)
	uint16_t phiMorphZoneA{0};
	uint16_t phiMorphZoneB{0};
	float phiMorphPhaseOffsetA{0.0f};
	float phiMorphPhaseOffsetB{0.0f};
	float phiMorphGamma{0.0f}; // Shared phase multiplier (push+twist on wave index)
	deluge::dsp::PhiMorphCache* phiMorphCache{nullptr};

	// PHI_WEAVE zone parameters (same interface family as PHI_MORPH)
	uint16_t phiWeaveZoneA{0};
	uint16_t phiWeaveZoneB{0};
	float phiWeavePhaseOffsetA{0.0f};
	float phiWeavePhaseOffsetB{0.0f};
	float phiWeaveGamma{0.0f};
	deluge::dsp::PhiWeaveCache* phiWeaveCache{nullptr};

	// PHI_VOX zone parameters (same interface family)
	uint16_t phiVoxZoneA{0};
	uint16_t phiVoxZoneB{0};
	float phiVoxPhaseOffsetA{0.0f};
	float phiVoxPhaseOffsetB{0.0f};
	float phiVoxGamma{0.0f};
	uint8_t phiVoxTracking{0}; // 0..50: formant frequencies fixed in Hz (0) -> note-relative (50)
	deluge::dsp::PhiVoxCache* phiVoxCache{nullptr};

	// PHI_SWARM zone parameters (same interface family)
	uint16_t phiSwarmZoneA{0};
	uint16_t phiSwarmZoneB{0};
	float phiSwarmPhaseOffsetA{0.0f};
	float phiSwarmPhaseOffsetB{0.0f};
	float phiSwarmGamma{0.0f};
	deluge::dsp::PhiSwarmCache* phiSwarmCache{nullptr};

	// PHI_GENDY zone parameters (same interface family)
	uint16_t phiGendyZoneA{0};
	uint16_t phiGendyZoneB{0};
	float phiGendyPhaseOffsetA{0.0f};
	float phiGendyPhaseOffsetB{0.0f};
	float phiGendyGamma{0.0f};
	deluge::dsp::PhiGendyCache* phiGendyCache{nullptr};

	// Zone-style stereo knob, shared by the phi family: position WITHIN each
	// 128-wide zone = stereo amount (every zone starts mono; knob 0 = mono),
	// zone index = stereo character (per-algorithm phi params)
	uint16_t phiStereoZone{0};

	[[nodiscard]] bool isPhiFamily() const {
		return oscType == OscType::PHI_MORPH || oscType == OscType::PHI_WEAVE || oscType == OscType::PHI_VOX
		       || oscType == OscType::PHI_SWARM || oscType == OscType::PHI_GENDY;
	}
	[[nodiscard]] bool phiStereoActive() const { return isPhiFamily() && (phiStereoZone & 127u) != 0; }

	int8_t timeStretchAmount;
	bool offsetWraps{true}; // When true, start offset wraps playhead modularly; when false, clamps at boundary

	int16_t defaultRangeI; // -1 means none yet

	bool renderInStereo(Sound* s, SampleHolder* sampleHolder = nullptr);
	void setCents(int32_t newCents);
	void recalculateFineTuner();
	int32_t getLengthInSamplesAtSystemSampleRate(int32_t note, bool forTimeStretching = false);
	void detachAllAudioFiles();
	Error loadAllSamples(bool mayActuallyReadFiles);
	void setReversed(bool newReversed);
	int32_t getRangeIndex(int32_t note);
	MultiRange* getRange(int32_t note);
	MultiRange* getOrCreateFirstRange();
	bool hasAtLeastOneAudioFileLoaded();
	void doneReadingFromFile(Sound* sound);
	bool hasAnyLoopEndPoint();
	void setOscType(OscType newType);

	DxPatch* ensureDxPatch();
	void cloneFrom(Source* other);

private:
	void destructAllMultiRanges();
};
