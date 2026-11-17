#pragma once

#include <cstddef>

namespace sermon {

// Voice leveling: a slow auto-fader that pulls long-term level toward a
// target RMS over phrase-length windows (not compression). Port of
// src/lib/dsp/voiceLeveling.ts. Processes one channel, out of place
// (`in` and `out` must not overlap); call once per channel.
void voiceLeveling(const float* in, float* out, size_t length, double sampleRate);

}  // namespace sermon
