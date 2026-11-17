#pragma once

#include <cstddef>

namespace sermon {

// One-time peak normalization to -3 dBFS (boost capped at +24 dB) across all
// channels together, so every later stage sees a consistent starting level.
// Port of src/lib/dsp/normalize.ts. In place.
void normalizePeak(float* const* channels, size_t numChannels, size_t length, double targetPeakDb = -3,
                   double maxGainDb = 24);

}  // namespace sermon
