#include "normalize.h"

#include <algorithm>
#include <cmath>

namespace sermon {

void normalizePeak(float* const* ch, size_t nCh, size_t n, double targetPeakDb, double maxGainDb) {
  double peak = 0;
  for (size_t c = 0; c < nCh; c++)
    for (size_t i = 0; i < n; i++) peak = std::max(peak, double(std::fabs(ch[c][i])));
  if (peak <= 1e-6) return;
  const double gain = std::min(std::pow(10.0, targetPeakDb / 20) / peak, std::pow(10.0, maxGainDb / 20));
  if (std::fabs(gain - 1) < 0.001) return;
  for (size_t c = 0; c < nCh; c++)
    for (size_t i = 0; i < n; i++) ch[c][i] = float(ch[c][i] * gain);
}

}  // namespace sermon
