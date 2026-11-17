#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace sermon {

struct LimiterResult {
  double measuredLufs = 0, appliedGainDb = 0, truePeakDb = 0;
  double finalLufs = 0;               // loudness actually reached
  // Gain reduction per 10 ms window of speech (windows whose plainly-gained peak is above
  // -40 dBFS), in dB: what the limiter does most of the time, not just at its deepest.
  double medianReductionDb = 0, p90ReductionDb = 0, p99ReductionDb = 0, maxReductionDb = 0;
};

// "Match streaming loudness": gain the file to the target integrated loudness
// (default -14 LUFS; if limiting costs loudness, re-gain by the shortfall, at
// most `maxExtraDb` beyond the plain gain), then a lookahead limiter holds the *true* peak (4x
// oversampled) at the ceiling (default -1 dBTP). 5ms lookahead, 100ms
// release, detection linked across channels. In place.
LimiterResult limitToStreaming(float* const* channels, size_t numChannels, size_t length, double sampleRate,
                               double targetLufs = -14, double ceilingDb = -1, double maxExtraDb = 4.0);

// Applies a precomputed per-sample required gain (1 = no change, < 1 = reduce) with the
// limiter's smoothing: 5 ms lookahead ramps landing exactly on the requirement, 100 ms release.
void applyRequiredGain(float* const* channels, size_t numChannels, size_t length, double sampleRate,
                       const std::vector<float>& need);
// Same, with the requirement computed on demand (no full-length array).
void applyRequiredGainFn(float* const* channels, size_t numChannels, size_t length, double sampleRate,
                         const std::function<float(size_t)>& needAt);

// Peak-only pass: no loudness gain, just holds the true peak at the ceiling
// (same lookahead/release as above). Returns the resulting true peak in dB.
// Run after anything that can add overshoot (e.g. the final lowpass).
double enforceTruePeak(float* const* channels, size_t numChannels, size_t length, double sampleRate,
                       double ceilingDb = -1);

}  // namespace sermon
