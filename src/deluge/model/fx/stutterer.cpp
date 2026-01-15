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
#include "util/cfunctions.h"
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
	stutterConfig = sc;
	currentReverse = stutterConfig.reversed;
	halfBarMode = halfBar;

	// Non-Classic modes: double buffer system (swap instead of copy)
	bool useLooper = (stutterConfig.scatterMode != ScatterMode::Classic);
	if (useLooper) {
		// Check if this is a takeover trigger (source was recording, now wants to play)
		bool isTakeoverTrigger = (recordSource == source && playSource != source && status == Status::PLAYING);
		if (isTakeoverTrigger) {
			// Use stored values from armStutter
			stutterConfig = armedConfig;
			currentReverse = stutterConfig.reversed;
			if (loopLengthSamples == 0) {
				loopLengthSamples = armedLoopLengthSamples;
			}
			halfBarMode = armedHalfBarMode;
		}

		// If source has been recording (owns recordBuffer), swap and start playback
		if (recordBuffer != nullptr && recordSource == source && loopLengthSamples > 0) {
			// Use full loop length for correct timing
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

			// Clear new recordBuffer to prevent stale audio on next trigger
			// EXPERIMENT: commented out to test if memset causes audio glitch
			// if (recordBuffer != nullptr) {
			// 	memset(recordBuffer, 0, kLooperBufferSize * sizeof(deluge::dsp::StereoSample<q31_t>));
			// }

			// Reset for playback and new recording
			playbackPos = 0;
			recordWritePos = 0;
			currentSliceLength = playbackLength;
			sliceStartOffset = 0;
			scatterSliceIndex = 0;
			scatterReversed = false;
			scatterDryMix = 0;
			scatterDryThreshold = 1.0f;
			scatterEnvDepth = 0;
			scatterEnvShape = 0.5f;
			scatterEnvWidth = 1.0f;
			scatterGateRatio = 1.0f;
			scatterPan = 0;
			scatterSubdivisions = 1;
			scatterSubdivIndex = 0;
			status = Status::PLAYING;
			// Source now owns both buffers
			playSource = source;
			recordSource = source;
			return Error::NONE;
		}

		// No buffers yet - allocate both and start standby recording
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
		// Clear record buffer
		// EXPERIMENT: commented out to test if memset causes audio glitch
		// memset(recordBuffer, 0, kLooperBufferSize * sizeof(deluge::dsp::StereoSample<q31_t>));

		// Source claims recordBuffer, starts recording in STANDBY
		// If someone else was playing, they keep playSource
		recordSource = source;
		if (status != Status::PLAYING) {
			status = Status::STANDBY;
		}
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
		playSource = source;
		recordSource = source;
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
			// Benchmark: granular scatter processing with dynamic tags
			// Tag layout: [0]=type, [1]=mode, [2]=extra (slices/subdiv for slice benchmark)
			FX_BENCH_DECLARE(benchTotal, "scatter", "total");
			FX_BENCH_DECLARE(benchSlice, "scatter", "slice");
			FX_BENCH_DECLARE(benchParams, "scatter", "params");
			FX_BENCH_DECLARE(benchEnv, "scatter", "env");
			FX_BENCH_DECLARE(benchPan, "scatter", "pan");
			FX_BENCH_DECLARE(benchRecord, "scatter", "record");
			const char* modeName = kScatterModeNames[static_cast<int>(stutterConfig.scatterMode)];
			FX_BENCH_SET_TAG(benchTotal, 1, modeName);
			FX_BENCH_SET_TAG(benchSlice, 1, modeName);
			FX_BENCH_SET_TAG(benchParams, 1, modeName);
			FX_BENCH_SET_TAG(benchEnv, 1, modeName);
			FX_BENCH_SET_TAG(benchPan, 1, modeName);
			FX_BENCH_SET_TAG(benchRecord, 1, modeName);
			FX_BENCH_START(benchTotal);

			// Sample counter for benchmarking (only first sample per buffer)
			int32_t sampleIdx = 0;

			for (deluge::dsp::StereoSample<q31_t>& sample : audio) {
				// Record incoming audio to recordBuffer for re-trigger capability
				// Only record if playSource also owns recordSource (no takeover in progress)
				if (recordBuffer != nullptr && recordSource == playSource) {
					if (sampleIdx == 0) {
						FX_BENCH_START(benchRecord);
					}
					recordBuffer[recordWritePos] = sample;
					recordWritePos++;
					if (recordWritePos >= kLooperBufferSize) {
						recordWritePos = 0;
					}
					if (sampleIdx == 0) {
						FX_BENCH_STOP(benchRecord);
					}
				}

				// === SLICE BOUNDARY: get next slice parameters ===
				// This is where each mode determines what to play next.
				// Slice changes ONLY happen here, ensuring complete playback.
				// Future modes: advance pattern index, call setSliceByBeat(), etc.
				// Only setup new slice when ALL subdivisions (ratchets) complete
				if (playbackPos == 0 && scatterSubdivIndex == 0) {
					FX_BENCH_START(benchSlice);
					switch (stutterConfig.scatterMode) {
					case ScatterMode::Repeat:
						// Rate knob controls slice length from END of captured bar
						currentSliceLength = getRepeatSliceLength(paramManager, playbackLength);
						sliceStartOffset = playbackLength - currentSliceLength;
						scatterDryMix = 0; // No density crossfade in Repeat mode
						break;

					case ScatterMode::Shuffle: {
						// Rate knob controls number of slices - match UI note division labels
						// UI optionValues: {2, 6, 13, 19, 25, 31, 38, 47} for 0-50 range
						// Maps to: 1 BAR, 2nds, 4ths, 8ths, 16ths, 32nds, 64ths, 128ths
						UnpatchedParamSet* unpatchedParams = paramManager->getUnpatchedParamSet();
						int32_t rateParam = unpatchedParams->getValue(params::UNPATCHED_STUTTER_RATE);
						int32_t knobPos = unpatchedParams->paramValueToKnobPos(rateParam, nullptr);
						// Convert knobPos (-64..+64) to UI value (0..50) range
						int32_t uiValue = ((knobPos + 64) * 50) / 128;
						// Map UI value to note divisions (thresholds at midpoints between optionValues)
						// Capped at 32 slices max for performance
						// Thresholds: 4, 9, 16, 22, 28 (midpoints)
						if (uiValue < 4) {
							scatterNumSlices = 1; // 1 BAR
						}
						else if (uiValue < 9) {
							scatterNumSlices = 2; // 2nds (half notes)
						}
						else if (uiValue < 16) {
							scatterNumSlices = 4; // 4ths (quarter notes)
						}
						else if (uiValue < 22) {
							scatterNumSlices = 8; // 8ths
						}
						else if (uiValue < 28) {
							scatterNumSlices = 16; // 16ths
						}
						else {
							scatterNumSlices = 32; // 32nds (max)
						}

						// Read zone params - use patched params for Sound context, unpatched for Song
						q31_t zoneAParam, zoneBParam, macroConfigParam, macroParam;
						if (paramManager->containsPatchedParamSetCollection()) {
							PatchedParamSet* patchedParams = paramManager->getPatchedParamSet();
							zoneAParam = patchedParams->getValue(params::GLOBAL_SCATTER_ZONE_A);
							zoneBParam = patchedParams->getValue(params::GLOBAL_SCATTER_ZONE_B);
							macroConfigParam = patchedParams->getValue(params::GLOBAL_SCATTER_MACRO_CONFIG);
							macroParam = patchedParams->getValue(params::GLOBAL_SCATTER_MACRO);
						}
						else {
							zoneAParam = unpatchedParams->getValue(params::UNPATCHED_SCATTER_ZONE_A);
							zoneBParam = unpatchedParams->getValue(params::UNPATCHED_SCATTER_ZONE_B);
							macroConfigParam = unpatchedParams->getValue(params::UNPATCHED_SCATTER_MACRO_CONFIG);
							macroParam = unpatchedParams->getValue(params::UNPATCHED_SCATTER_MACRO);
						}

						// Macro influence on zone phases - macroConfig phi triangles gate macro's effect
						// Different phi frequencies for independent routing of A vs B
						float macroConfigNorm =
						    static_cast<float>(macroConfigParam) * deluge::dsp::scatter::kQ31ToFloat;
						float macroNorm = static_cast<float>(macroParam) * deluge::dsp::scatter::kQ31ToFloat;
						float zoneAMacroInfluence =
						    deluge::dsp::triangleSimpleUnipolar(macroConfigNorm * deluge::dsp::phi::kPhi050, 0.5f);
						float zoneBMacroInfluence =
						    deluge::dsp::triangleSimpleUnipolar(macroConfigNorm * deluge::dsp::phi::kPhi075, 0.5f);
						// Macro adds to phase offsets (max 30% of full phase range at full influence)
						// Additive with secret encoder offsets and gamma scaling
						constexpr float kMacroPhaseMax = 0.3f;
						float macroZoneAPhase = macroNorm * zoneAMacroInfluence * kMacroPhaseMax;
						float macroZoneBPhase = macroNorm * zoneBMacroInfluence * kMacroPhaseMax;

						// Phase offsets from secret encoder menus (push+twist) + macro contribution
						deluge::dsp::scatter::ScatterPhaseOffsets offsets{
						    stutterConfig.zoneAPhaseOffset + macroZoneAPhase,
						    stutterConfig.zoneBPhaseOffset + macroZoneBPhase,
						    stutterConfig.macroConfigPhaseOffset,
						    stutterConfig.gammaPhase,
						};

						// Compute grain params (adaptive caching disabled for testing)
						FX_BENCH_START(benchParams);
						deluge::dsp::scatter::GrainParams grain = deluge::dsp::scatter::computeGrainParams(
						    zoneAParam, zoneBParam, macroConfigParam, macroParam, scatterSliceIndex, offsets);
						FX_BENCH_STOP(benchParams);

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

						// Set slice parameters - timing stays locked to bar
						currentSliceLength = playbackLength / scatterNumSlices;
						if (currentSliceLength < 256) {
							currentSliceLength = 256;
						}
						sliceStartOffset = targetSlice * (playbackLength / scatterNumSlices);

						// Reverse based on probability
						float revRoll = deluge::dsp::phi::wrapPhase(static_cast<float>(scatterSliceIndex) * 3.7f);
						scatterReversed = (revRoll < grain.reverseProb);

						// Store grain params for playback
						scatterDryMix = grain.dryMix;

						// Dry threshold: macroConfig phi triangle determines how much influence macro has
						// When triangle=0, macro has no effect (threshold=0, all grains)
						// When triangle=1, macro has full effect on threshold
						// Uses different phi frequency than zone phase influence for independent routing
						float thresholdInfluence =
						    deluge::dsp::triangleSimpleUnipolar(macroConfigNorm * deluge::dsp::phi::kPhi, 0.5f);
						// Threshold = macro scaled by its influence (0 influence = always grains)
						scatterDryThreshold = macroNorm * thresholdInfluence;

						// All timbral params from grain (computed with phase offset and gamma in computeGrainParams)
						scatterEnvShape = grain.envShape;
						scatterGateRatio = grain.gateRatio;
						scatterEnvDepth = grain.envDepth;

						// Pan: direction decorrelated from slice content using separate counter
						// Amount from grain params (incorporates phase offset for evolving stereo field)
						float panDir =
						    (deluge::dsp::phi::wrapPhase(static_cast<float>(scatterPanCounter++) * 5.3f) < 0.5f) ? -1.0f
						                                                                                         : 1.0f;
						scatterPan = panDir * grain.panAmount;

						// Precompute pan coefficients (Q31, once per slice)
						float panAbs = (scatterPan > 0) ? scatterPan : -scatterPan;
						scatterPanActive = (panAbs > 0.001f);
						scatterPanFadeQ31 = static_cast<int32_t>((1.0f - panAbs) * 2147483647.0f);
						scatterPanKeepQ31 = static_cast<int32_t>((1.0f - panAbs * 0.5f) * 2147483647.0f);
						scatterPanCrossQ31 = static_cast<int32_t>((panAbs * 0.5f) * 2147483647.0f);
						scatterPanRight = (scatterPan > 0);

						// Precompute envelope/gate active flags (once per slice, avoid per-sample checks)
						scatterEnvActive = (scatterEnvDepth > 0.001f);
						scatterGateActive = (scatterGateRatio < 0.999f);

						// Subdivisions (ratchet) from grain params - clamp to valid range
						// Limit subdivisions so sub-slices stay above ~80 Hz (551 samples at 44.1kHz)
						constexpr int32_t kMinSubSliceSamples = 551;
						int32_t maxSubdivisions = static_cast<int32_t>(currentSliceLength) / kMinSubSliceSamples;
						if (maxSubdivisions < 1) {
							maxSubdivisions = 1;
						}
						scatterSubdivisions = std::clamp(grain.subdivisions, int32_t{1}, maxSubdivisions);
						scatterSubdivIndex = 0; // Reset for new slice

						// Precompute Q31 envelope parameters (once per slice, used for all samples)
						// Envelope applies per sub-slice for ratchet effect
						int32_t envSliceLen = static_cast<int32_t>(currentSliceLength) / scatterSubdivisions;
						scatterEnvPrecomputed = deluge::dsp::scatter::prepareGrainEnvelopeQ31(
						    envSliceLen, scatterGateRatio, scatterEnvDepth, scatterEnvShape, scatterEnvWidth);

						// Tag slice benchmark with slice count and subdiv (combined in tag[2])
						// tag[0]="slice", tag[1]=mode, tag[2]="8s/x4" format
						{
							static char sliceInfoTag[16];
							char* p = sliceInfoTag;
							intToString(scatterNumSlices, p, 1);
							while (*p)
								p++;
							*p++ = 's';
							*p++ = '/';
							*p++ = 'x';
							intToString(scatterSubdivisions, p, 1);
							FX_BENCH_SET_TAG(benchSlice, 2, sliceInfoTag);
						}

						// Advance for next slice
						scatterSliceIndex = (scatterSliceIndex + 1) % scatterNumSlices;
						break;
					}

					default:
						// Default: play full bar
						currentSliceLength = playbackLength;
						sliceStartOffset = 0;
						scatterDryMix = 0; // No density crossfade in default mode
						break;
					}
					FX_BENCH_STOP(benchSlice);
				}

				// === PLAYBACK: read from current slice ===
				// Save dry input for potential crossfade (density zone)
				q31_t dryL = sample.l;
				q31_t dryR = sample.r;

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
				// Density threshold: hard cut between grain and dry (not a blend)
				// dryMix > threshold = use dry signal for this grain, else use buffer grain
				// Threshold = macro * macroInfluence (macroConfig phi triangle gates macro's effect)
				bool useDry = (scatterDryMix > scatterDryThreshold);

				q31_t outputL, outputR;
				if (useDry) {
					// Use dry input signal
					outputL = dryL;
					outputR = dryR;
				}
				else {
					// Use grain from buffer
					outputL = playBuffer[readPos].l;
					outputR = playBuffer[readPos].r;
				}

				// Benchmark first sample only to avoid 128x overhead
				bool benchThisSample = (sampleIdx == 0);

				// Apply grain envelope and gate (using precomputed flags from slice boundary)
				if (stutterConfig.scatterMode == ScatterMode::Shuffle && (scatterEnvActive || scatterGateActive)) {
					if (benchThisSample) {
						FX_BENCH_START(benchEnv);
					}
					// Asymmetric linear envelope with depth blending - skip middle samples
					int32_t pos = static_cast<int32_t>(playbackPos);
					int32_t attackLen = scatterEnvPrecomputed.attackFadeLen;
					int32_t decayLen = scatterEnvPrecomputed.decayFadeLen;
					int32_t gatedLen = scatterEnvPrecomputed.gatedLength;

					if (pos > gatedLen) {
						// Past gate cutoff - silence
						outputL = 0;
						outputR = 0;
					}
					else if (pos < attackLen || pos > gatedLen - decayLen) {
						// In fade region - apply asymmetric linear envelope with depth blending
						int32_t linearQ31 = deluge::dsp::scatter::grainEnvelopeLinearQ31(pos, scatterEnvPrecomputed);
						int32_t envQ31;
#if 0 // PERF TEST: disable depth blend
						if (scatterEnvPrecomputed.depthIsMax) {
							// depth >= 0.99: skip blend, use linear directly
							envQ31 = linearQ31;
						}
						else {
							// Depth blend: (1 - depth) + depth * linear (precomputed oneMinusDepthQ31)
							envQ31 = scatterEnvPrecomputed.oneMinusDepthQ31
							         + (multiply_32x32_rshift32(scatterEnvPrecomputed.depthQ31, linearQ31) << 1);
						}
#else
						envQ31 = linearQ31; // PERF TEST: always use linear, no blend
#endif
						outputL = multiply_32x32_rshift32(outputL, envQ31) << 1;
						outputR = multiply_32x32_rshift32(outputR, envQ31) << 1;
					}
					// else: flat middle - (1-depth) + depth*1.0 = 1.0, no attenuation needed
					if (benchThisSample) {
						FX_BENCH_STOP(benchEnv);
					}
				}

				// Apply crossfeed pan using precomputed Q31 coefficients
				// At pan=1: L=0, R=(L+R)/2  |  At pan=-1: L=(L+R)/2, R=0
				if (scatterPanActive) {
					if (benchThisSample) {
						FX_BENCH_START(benchPan);
					}
					if (scatterPanRight) {
						// Pan right: L fades to 0, R blends toward (L+R)/2
						q31_t newL = multiply_32x32_rshift32(outputL, scatterPanFadeQ31) << 1;
						q31_t newR = (multiply_32x32_rshift32(outputR, scatterPanKeepQ31) << 1)
						             + (multiply_32x32_rshift32(outputL, scatterPanCrossQ31) << 1);
						outputL = newL;
						outputR = newR;
					}
					else {
						// Pan left: R fades to 0, L blends toward (L+R)/2
						q31_t newR = multiply_32x32_rshift32(outputR, scatterPanFadeQ31) << 1;
						q31_t newL = (multiply_32x32_rshift32(outputL, scatterPanKeepQ31) << 1)
						             + (multiply_32x32_rshift32(outputR, scatterPanCrossQ31) << 1);
						outputL = newL;
						outputR = newR;
					}
					if (benchThisSample) {
						FX_BENCH_STOP(benchPan);
					}
				}

				sample.l = outputL;
				sample.r = outputR;

				// === ADVANCE: move through slice with subdivisions (ratchet) ===
				// When subdivisions > 1, replay start of slice N times
				playbackPos++;
				// Guard against division by zero (scatterSubdivisions should be 1-12)
				int32_t safeSubdiv = (scatterSubdivisions >= 1) ? scatterSubdivisions : 1;
				size_t subSliceLength = currentSliceLength / static_cast<size_t>(safeSubdiv);
				if (subSliceLength < 64) {
					subSliceLength = 64; // Minimum for anti-click
				}
				if (playbackPos >= subSliceLength) {
					playbackPos = 0;
					scatterSubdivIndex++;
					if (scatterSubdivIndex >= safeSubdiv) {
						scatterSubdivIndex = 0; // Triggers next slice selection on next sample
					}
				}

				sampleIdx++;
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
		playbackPos = 0;
		playSource = nullptr; // Stop playing

		// If someone else was recording for takeover, their recording is now orphaned
		// Reset to clean state - nobody owns anything
		if (recordSource != nullptr) {
			recordSource = nullptr;
			recordWritePos = 0;
		}
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
		playSource = nullptr;
		recordSource = nullptr;
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
}

Error Stutterer::enableStandby(void* source, int32_t magnitude, uint32_t timePerTickInverse) {
	if (status == Status::STANDBY && recordSource == source) {
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
	recordSource = source;
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
		playSource = nullptr;
		recordSource = nullptr;
	}
}

void Stutterer::recordStandby(void* source, deluge::dsp::StereoBuffer<q31_t> audio) {
	// === CLEAN OWNERSHIP MODEL ===
	// Only recordSource can write to recordBuffer. Period.
	// This works for both standby (source == recordSource) and takeover (B stole recordSource from A).

	if (source != recordSource) {
		return; // Not your buffer
	}

	// Check if double buffers are in use (scatter mode)
	bool hasDoubleBuffers = (recordBuffer != nullptr && playBuffer != nullptr);

	if (hasDoubleBuffers) {
		// Scatter mode: record during STANDBY or PLAYING (takeover)
		if (status != Status::STANDBY && status != Status::PLAYING) {
			return;
		}
		for (deluge::dsp::StereoSample<q31_t> sample : audio) {
			recordBuffer[recordWritePos] = sample;
			recordWritePos++;
			if (recordWritePos >= kLooperBufferSize) {
				recordWritePos = 0;
			}
		}
		return;
	}

	// Classic mode: use delay buffer during STANDBY only
	if (status != Status::STANDBY) {
		return;
	}
	for (deluge::dsp::StereoSample<q31_t> sample : audio) {
		buffer.current().l = sample.l;
		buffer.current().r = sample.r;
		buffer.moveOn();
	}
}

Error Stutterer::armStutter(void* source, ParamManagerForTimeline* paramManager, StutterConfig sc, int32_t magnitude,
                            uint32_t timePerTickInverse, int64_t targetTick, size_t loopLengthSamples, bool halfBar) {
	// === SIMPLIFIED: armStutter just claims recordBuffer for this source ===
	// No beat quantization for now - that's broken anyway.
	// This is called when a source wants to START recording (first encoder press).

	if (status == Status::RECORDING) {
		return Error::UNSPECIFIED; // Classic mode recording, can't interrupt
	}

	bool hasDoubleBuffers = (bufferA != nullptr && bufferB != nullptr);

	// Store config for when trigger fires
	armedConfig = sc;
	armedHalfBarMode = halfBar;
	armedLoopLengthSamples = loopLengthSamples;

	if (status == Status::PLAYING && hasDoubleBuffers) {
		// TAKEOVER: Someone else is playing, we want to steal the record buffer
		// Source claims recordBuffer, starts recording while other source keeps playing
		recordSource = source;
		recordWritePos = 0;
		// Clear buffer to start fresh
		// EXPERIMENT: commented out to test if memset causes audio glitch
		// if (recordBuffer != nullptr) {
		// 	memset(recordBuffer, 0, kLooperBufferSize * sizeof(deluge::dsp::StereoSample<q31_t>));
		// }
		return Error::NONE;
	}

	// Not playing - this is initial setup, delegate to beginStutter
	return beginStutter(source, paramManager, sc, magnitude, timePerTickInverse, loopLengthSamples, halfBar);
}

bool Stutterer::checkArmedTrigger(int64_t currentTick, ParamManager* paramManager, int32_t magnitude,
                                  uint32_t timePerTickInverse) {
	// === SIMPLIFIED: No beat quantization for now ===
	// Takeover trigger happens via beginStutter when recordSource calls it.
	// This function is vestigial - always returns false.
	// TODO: Re-implement beat quantization properly later.
	return false;
}

void Stutterer::cancelArmed() {
	// === SIMPLIFIED: Cancel takeover ===
	// If source was recording for takeover, give up recordBuffer
	// The current playSource keeps playing.

	if (status == Status::PLAYING && recordSource != playSource) {
		// Someone was preparing takeover - cancel it
		// Give recordBuffer back to playSource
		recordSource = playSource;
		recordWritePos = 0;
		// EXPERIMENT: commented out to test if memset causes audio glitch
		// if (recordBuffer != nullptr) {
		// 	memset(recordBuffer, 0, kLooperBufferSize * sizeof(deluge::dsp::StereoSample<q31_t>));
		// }
		return;
	}

	if (status == Status::ARMED) {
		// Classic mode armed - go back to standby or off
		if (startedFromStandby) {
			status = Status::STANDBY;
		}
		else {
			buffer.discard();
			status = Status::OFF;
			playSource = nullptr;
			recordSource = nullptr;
		}
	}
}
