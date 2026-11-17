#pragma once

#include <cstddef>

#include "intensity.h"

namespace sermon {

// Vocal compressor: soft knee (25 dB), 20ms attack / 200ms release, ratio
// 4/8/12 and makeup 0/2/4 dB by intensity. The threshold is derived per file:
// 6 dB below the integrated loudness, clamped to [-40, -10] dB. Detection is
// linked across channels (stereo image preserved). Port of the browser
// preset, using a standard log-domain feed-forward design rather than the
// Web Audio node. In place.
void compress(float* const* channels, size_t numChannels, size_t length, double sampleRate,
              Intensity intensity = Intensity::Normal);

}  // namespace sermon
