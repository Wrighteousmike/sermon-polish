#include "voice_leveling.h"

#include <algorithm>
#include <cmath>

namespace sermon {

namespace {

constexpr double kTargetRmsDb = -20;
constexpr double kWindowMs = 400;
constexpr double kAttackMs = 300;
constexpr double kReleaseMs = 1000;
// ±18dB: a clumsy mid-sermon fader push can easily be 15-20dB.
constexpr double kMaxBoostDb = 18;
constexpr double kMaxCutDb = 18;

}  // namespace

void voiceLeveling(const float* src, float* dst, size_t length, double sampleRate) {
  const size_t windowSize = std::max<size_t>(1, static_cast<size_t>(std::llround(kWindowMs / 1000 * sampleRate)));
  const double attackCoeff = std::exp(-1.0 / (kAttackMs / 1000 * sampleRate));
  const double releaseCoeff = std::exp(-1.0 / (kReleaseMs / 1000 * sampleRate));
  const double targetLin = std::pow(10.0, kTargetRmsDb / 20);
  const double maxBoost = std::pow(10.0, kMaxBoostDb / 20);
  const double maxCut = std::pow(10.0, -kMaxCutDb / 20);

  // Continuously-sliding RMS window: O(n) running sum of squares. Kept in
  // double so an hour-long file doesn't accumulate meaningful drift.
  double sumSq = 0;
  double currentGain = 1;
  for (size_t i = 0; i < length; i++) {
    const double s = src[i];
    sumSq += s * s;
    if (i >= windowSize) {
      const double old = src[i - windowSize];
      sumSq -= old * old;
    }
    // Guards against drift from the running add/subtract going slightly
    // negative near silence.
    const double rms = std::sqrt(std::max(0.0, sumSq) / static_cast<double>(std::min(i + 1, windowSize)));

    const double target = rms > 1e-6 ? std::min(maxBoost, std::max(maxCut, targetLin / rms)) : 1.0;
    const double coeff = target < currentGain ? attackCoeff : releaseCoeff;
    currentGain = target + (currentGain - target) * coeff;
    dst[i] = static_cast<float>(s * currentGain);
  }
}

}  // namespace sermon
