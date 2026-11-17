#include "compressor.h"

#include <algorithm>
#include <cmath>

#include "amount.h"
#include "loudness.h"

namespace sermon {

namespace {
constexpr double kKneeDb = 25, kAttackS = 0.02, kReleaseS = 0.2;
}  // namespace

void compress(float* const* ch, size_t nCh, size_t n, double fs, Intensity intensity) {
  const amount::CompressorParams p = amount::compressor(intensity);
  const double lufs = integratedLufs(ch, nCh, n, fs);
  const double threshold = std::min(-10.0, std::max(-40.0, lufs - p.thresholdOffsetDb));
  const double aCoeff = std::exp(-1.0 / (kAttackS * fs)), rCoeff = std::exp(-1.0 / (kReleaseS * fs));
  const double makeup = std::pow(10.0, p.makeupDb / 20.0);

  double gainDb = 0;  // smoothed gain (<= 0)
  for (size_t i = 0; i < n; i++) {
    double peak = 0;
    for (size_t c = 0; c < nCh; c++) peak = std::max(peak, double(std::fabs(ch[c][i])));
    const double levelDb = 20.0 * std::log10(std::max(peak, 1e-8));
    const double over = levelDb - threshold;
    double outDb;
    if (2 * over < -kKneeDb) outDb = levelDb;
    else if (2 * std::fabs(over) <= kKneeDb) {
      const double t = over + kKneeDb / 2;
      outDb = levelDb + (1.0 / p.ratio - 1.0) * t * t / (2 * kKneeDb);
    } else outDb = threshold + over / p.ratio;
    const double targetDb = outDb - levelDb;  // <= 0
    const double coeff = targetDb < gainDb ? aCoeff : rCoeff;
    gainDb = targetDb + (gainDb - targetDb) * coeff;
    const double g = std::pow(10.0, gainDb / 20.0) * makeup;
    for (size_t c = 0; c < nCh; c++) ch[c][i] = float(ch[c][i] * g);
  }
}

}  // namespace sermon
