#pragma once

#include <vector>

namespace sermon {

// High-quality rational-ratio resampler (Kaiser-windowed sinc, ~16 zero
// crossings per side, cutoff at 96% of the lower Nyquist). One channel.
std::vector<float> resample(const float* in, size_t length, int srcRate, int dstRate);

}  // namespace sermon
