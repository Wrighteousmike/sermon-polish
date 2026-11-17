#include "declip.h"

#include <cmath>

namespace sermon {

namespace {

// Below this many consecutive clipped samples, treat it as a single loud
// sample rather than a clipping event.
constexpr size_t kMinRun = 2;

struct Settings {
  float threshold;  // |sample| at/above this counts as clipped
  size_t maxRun;    // longer runs are assumed to be real content, left alone
};

Settings settingsFor(DeclipIntensity intensity) {
  switch (intensity) {
    case DeclipIntensity::Less: return {0.99f, 100};
    case DeclipIntensity::More: return {0.95f, 400};
    case DeclipIntensity::Normal:
    default: return {0.98f, 200};
  }
}

}  // namespace

void declip(float* dst, size_t length, DeclipIntensity intensity) {
  const Settings s = settingsFor(intensity);

  size_t i = 0;
  while (i < length) {
    if (std::fabs(dst[i]) < s.threshold) {
      i++;
      continue;
    }
    size_t j = i;
    while (j < length && std::fabs(dst[j]) >= s.threshold) j++;
    const size_t runLength = j - i;

    if (runLength >= kMinRun && runLength <= s.maxRun && i > 0 && j < length) {
      const double a = dst[i - 1];
      const double b = dst[j];
      const double span = static_cast<double>(j - (i - 1));
      for (size_t k = i; k < j; k++) {
        const double t = static_cast<double>(k - (i - 1)) / span;
        const double eased = t * t * (3.0 - 2.0 * t);  // smoothstep between the clean anchors
        dst[k] = static_cast<float>(a + (b - a) * eased);
      }
    }
    i = j;
  }
}

}  // namespace sermon
