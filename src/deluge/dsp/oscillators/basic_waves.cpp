/*
 * Copyright © 2017-2023 Synthstrom Audible Limited, 2025 Mark Adams
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

#include "basic_waves.h"
#include "definitions.h"
#include "processing/render_wave.h"
#include "util/fixedpoint.h"
#include "util/functions.h"
#include "util/lookuptables/lookuptables.h"
#include <argon.hpp>
namespace deluge::dsp {
/* Before calling, you must:
    amplitude <<= 1; ( so that it is q31)
    amplitudeIncrement <<= 1;
*/
void renderWave(const int16_t* __restrict__ table, int32_t table_size_magnitude, int32_t amplitude,
                std::span<q31_t> buffer, uint32_t phase_increment, uint32_t phase, bool apply_amplitude,
                uint32_t phase_to_add, int32_t amplitude_increment) {

	Argon<q31_t> amplitude_vector = createAmplitudeVector(amplitude, amplitude_increment);
	Argon<q31_t> amplitude_increment_vector = amplitude_increment << 1;

	// SIMD loop processes samples in groups of 4
	size_t simd_samples = buffer.size() & ~size_t{3};
	for (Argon<q31_t>& sample_vector : argon::vectorize(buffer)) {
		auto [value_vector, new_phase] =
		    waveRenderingFunctionGeneral(phase, phase_increment, phase_to_add, table, table_size_magnitude);

		if (apply_amplitude) {
			value_vector = sample_vector.MultiplyAddFixedPoint(value_vector, amplitude_vector);
			amplitude_vector = amplitude_vector + amplitude_increment_vector;
		}

		sample_vector = value_vector;
		phase = new_phase;
	}

	// Handle remainder samples (when buffer size is not a multiple of 4)
	for (size_t i = simd_samples; i < buffer.size(); i++) {
		phase += phase_increment;

		// Interpolated table lookup (scalar version of waveRenderingFunctionGeneral)
		uint32_t whichValue = phase >> (32 - table_size_magnitude);
		uint32_t rshifted = phase >> (32 - 16 - table_size_magnitude);
		int32_t strength2 = (rshifted & 0xFFFF) >> 1;

		int16_t value1 = table[whichValue];
		int16_t value2 = table[whichValue + 1];
		int32_t difference = value2 - value1;
		int32_t interpolated = (static_cast<int32_t>(value1) << 16) + (difference * strength2 * 2);

		if (apply_amplitude) {
			// Amplitude for sample i is (amplitude + (i+1)*amplitude_increment) >> 1
			int32_t sample_amplitude = (amplitude + static_cast<int32_t>(i + 1) * amplitude_increment) >> 1;
			// Use Q31 multiply (>> 31) to match SIMD MultiplyAddFixedPoint
			buffer[i] += multiply_32x32_rshift32_rounded(interpolated, sample_amplitude) << 1;
		}
		else {
			buffer[i] = interpolated;
		}
	}
}

/* Before calling, you must:
    amplitude <<= 1;
    amplitudeIncrement <<= 1;
*/
void renderPulseWave(const int16_t* __restrict__ table, int32_t table_size_magnitude, int32_t amplitude,
                     std::span<q31_t> buffer, uint32_t phase_increment, uint32_t phase, bool apply_amplitude,
                     uint32_t phase_to_add, int32_t amplitude_increment) {
	Argon<q31_t> amplitude_vector = createAmplitudeVector(amplitude, amplitude_increment);
	Argon<q31_t> amplitude_increment_vector = amplitude_increment << 1;

	// SIMD loop processes samples in groups of 4
	size_t simd_samples = buffer.size() & ~size_t{3};
	for (Argon<q31_t>& sample_vector : argon::vectorize(buffer)) {
		auto [value_vector, new_phase] =
		    waveRenderingFunctionPulse(phase, phase_increment, phase_to_add, table, table_size_magnitude);

		if (apply_amplitude) {
			value_vector = sample_vector.MultiplyAddFixedPoint(value_vector, amplitude_vector);
			amplitude_vector = amplitude_vector + amplitude_increment_vector;
		}

		sample_vector = value_vector;
		phase = new_phase;
	}

	// Handle remainder samples (when buffer size is not a multiple of 4)
	// Note: For pulse waves, we need both the base phase and the phase+phaseToAdd lookup
	for (size_t i = simd_samples; i < buffer.size(); i++) {
		phase += phase_increment;
		uint32_t phaseLater = phase + phase_to_add;

		// First table lookup (base phase)
		uint32_t whichValueA = phase >> (32 - table_size_magnitude);
		int32_t rshiftedA = (phase >> (32 - 16 - table_size_magnitude)) & 0x7FFF;
		int16_t valueA1 = table[whichValueA];
		int16_t valueA2 = table[whichValueA + 1];
		int32_t strengthA1 = rshiftedA | 0x8000;
		int32_t strengthA2 = 0x8000 - strengthA1;
		int32_t outputA = (strengthA2 * valueA2 + strengthA1 * valueA1) << 1;

		// Second table lookup (phase + phaseToAdd)
		uint32_t whichValueB = phaseLater >> (32 - table_size_magnitude);
		int32_t rshiftedB = (phaseLater >> (32 - 16 - table_size_magnitude)) & 0x7FFF;
		int16_t valueB1 = table[whichValueB];
		int16_t valueB2 = table[whichValueB + 1];
		int32_t strengthB2 = rshiftedB;
		int32_t strengthB1 = 0x7FFF - strengthB2;
		int32_t outputB = (strengthB2 * valueB2 + strengthB1 * valueB1) << 1;

		// Multiply the two outputs together (ring mod for pulse wave)
		int32_t interpolated = multiply_32x32_rshift32_rounded(outputA, outputB) << 1;

		if (apply_amplitude) {
			int32_t sample_amplitude = (amplitude + static_cast<int32_t>(i + 1) * amplitude_increment) >> 1;
			// Use Q31 multiply (>> 31) to match SIMD MultiplyAddFixedPoint
			buffer[i] += multiply_32x32_rshift32_rounded(interpolated, sample_amplitude) << 1;
		}
		else {
			buffer[i] = interpolated;
		}
	}
}

uint32_t renderCrudeSawWave(std::span<q31_t> buffer, uint32_t phase, uint32_t phase_increment, int32_t amplitude,
                            int32_t amplitude_increment) {
#pragma gcc unroll 4
	for (q31_t& sample : buffer) {
		phase += phase_increment;
		amplitude += amplitude_increment;
		sample = multiply_accumulate_32x32_rshift32_rounded(sample, (int32_t)phase, amplitude);
	}

	return phase;
}

uint32_t renderCrudeSawWave(std::span<q31_t> buffer, uint32_t phase, uint32_t phase_increment) {

#pragma gcc unroll 8
	for (q31_t& sample : buffer) {
		phase += phase_increment;
		sample = (int32_t)phase >> 1;
	}

	return phase;
}

/**
 * @brief Get a table number and size, depending on the increment
 *
 * @return table_number, table_size
 */
std::pair<int32_t, int32_t> getTableNumber(uint32_t phaseIncrement) {
	if (phaseIncrement <= 1247086) {
		return {0, 13};
	}
	else if (phaseIncrement <= 1764571) {
		return {1, 12};
	}
	else if (phaseIncrement <= 2494173) {
		return {2, 12};
	}
	else if (phaseIncrement <= 3526245) {
		return {3, 11};
	}
	else if (phaseIncrement <= 4982560) {
		return {4, 11};
	}
	else if (phaseIncrement <= 7040929) {
		return {5, 11};
	}
	else if (phaseIncrement <= 9988296) {
		return {6, 11};
	}
	else if (phaseIncrement <= 14035840) {
		return {7, 11};
	}
	else if (phaseIncrement <= 19701684) {
		return {8, 11};
	}
	else if (phaseIncrement <= 28256363) {
		return {9, 11};
	}
	else if (phaseIncrement <= 40518559) {
		return {10, 11};
	}
	else if (phaseIncrement <= 55063683) {
		return {11, 11};
	}
	else if (phaseIncrement <= 79536431) {
		return {12, 11};
	}
	else if (phaseIncrement <= 113025455) {
		return {13, 11};
	}
	else if (phaseIncrement <= 165191049) {
		return {14, 10};
	}
	else if (phaseIncrement <= 238609294) {
		return {15, 10};
	}
	else if (phaseIncrement <= 306783378) {
		return {16, 10};
	}
	else if (phaseIncrement <= 429496729) {
		return {17, 10};
	}
	else if (phaseIncrement <= 715827882) {
		return {18, 9};
	}
	else {
		return {19, 9};
	}
}

const int16_t* sawTables[20] = {NULL,       NULL,       NULL,      NULL,      NULL,      NULL,      sawWave215,
                                sawWave153, sawWave109, sawWave76, sawWave53, sawWave39, sawWave27, sawWave19,
                                sawWave13,  sawWave9,   sawWave7,  sawWave5,  sawWave3,  sawWave1};
const int16_t* squareTables[20] = {NULL,         NULL,          NULL,          NULL,          NULL,
                                   NULL,         squareWave215, squareWave153, squareWave109, squareWave76,
                                   squareWave53, squareWave39,  squareWave27,  squareWave19,  squareWave13,
                                   squareWave9,  squareWave7,   squareWave5,   squareWave3,   squareWave1};
const int16_t* analogSquareTables[20] = {analogSquare_1722, analogSquare_1217, analogSquare_861, analogSquare_609,
                                         analogSquare_431,  analogSquare_305,  analogSquare_215, analogSquare_153,
                                         analogSquare_109,  analogSquare_76,   analogSquare_53,  analogSquare_39,
                                         analogSquare_27,   analogSquare_19,   analogSquare_13,  analogSquare_9,
                                         analogSquare_7,    analogSquare_5,    analogSquare_3,   analogSquare_1};

// The lower 8 are from (mystery synth A) - higher than that, it's (mystery synth B)
const int16_t* analogSawTables[20] = {
    mysterySynthASaw_1722, mysterySynthASaw_1217, mysterySynthASaw_861, mysterySynthASaw_609, mysterySynthASaw_431,
    mysterySynthASaw_305,  mysterySynthASaw_215,  mysterySynthASaw_153, mysterySynthBSaw_109, mysterySynthBSaw_76,
    mysterySynthBSaw_53,   mysterySynthBSaw_39,   mysterySynthBSaw_27,  mysterySynthBSaw_19,  mysterySynthBSaw_13,
    mysterySynthBSaw_9,    mysterySynthBSaw_7,    mysterySynthBSaw_5,   mysterySynthBSaw_3,   mysterySynthBSaw_1};
} // namespace deluge::dsp
