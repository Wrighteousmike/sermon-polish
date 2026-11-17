#pragma once

#include <cstddef>
#include <string>

#include "compressor.h"  // Intensity

namespace sermon {

enum class NoiseBackend { Auto, RNNoise, DeepFilterNet };

// True if this build has any noise-reduction backend.
bool noiseReductionAvailable();
// True if the given backend was compiled in (Auto = any).
bool noiseBackendAvailable(NoiseBackend backend);
// Path of the DeepFilterNet model file in use (SERMON_DF_MODEL, else the build's own path), or
// "" if DeepFilterNet isn't built in. `found` says whether the file exists.
std::string noiseModelPath(bool* found = nullptr);
// Name of the backend Auto resolves to: "DeepFilterNet3", "RNNoise" or "none".
const char* noiseBackendName(NoiseBackend backend = NoiseBackend::Auto);

// Mains-hum notches at 50/100/150 Hz (Q 12; 8 on "more" to catch drifting
// harmonics). Always available. One channel, in place.
void removeHum(float* samples, size_t length, double sampleRate, Intensity intensity = Intensity::Normal);

// Broadband noise reduction. Requires 48 kHz input (resample first). One
// channel, in place. Auto prefers DeepFilterNet3 (intensity = attenuation
// limit: 10 / 20 dB / unlimited) over RNNoise (intensity = wet/dry blend: 50% /
// 75% / 100%). Does nothing if the backend isn't available.
void reduceNoise(float* samples, size_t length, double sampleRate, Intensity intensity = Intensity::Normal,
                 NoiseBackend backend = NoiseBackend::Auto);

}  // namespace sermon
