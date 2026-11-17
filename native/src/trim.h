#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio_io.h"

namespace sermon {

// Keeps [startSeconds, endSeconds) (clamped to the file) and applies a raised-cosine fade in
// and out over `fadeMs` at the cut points, like the browser app's trim step. A fade is never
// longer than half the kept audio. All channels.
void trimWithFades(AudioData& audio, double startSeconds, double endSeconds, double fadeMs = 100.0);

// Waveform overview for a UI: `buckets` (min, max) pairs over the channel mix, interleaved
// [min0, max0, min1, max1, ...], each in -1..1.
std::vector<float> computePeaks(const AudioData& audio, size_t buckets);

// Fine waveform data for zoomed trim views: one (min, max) pair per bucket of `bucketSamples`
// samples, as interleaved int16 (value * 32767) of the channel mix.
std::vector<int16_t> computePeaksFine(const AudioData& audio, size_t bucketSamples);

// Writes computePeaksFine() to a small binary file the UI reads directly:
//   16-byte header: "SPK1", uint32 version (1), uint32 bucket count, float32 bucket seconds;
//   then count * 2 little-endian int16 values (min, max per bucket).
void writePeaksFine(const std::string& path, const AudioData& audio, double bucketMs);

}  // namespace sermon
