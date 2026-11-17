#include "leveler.h"

#include <algorithm>
#include <limits>
#include <cmath>
#include <vector>

#include "loudness.h"

namespace sermon {

namespace {

constexpr double kHopSeconds = 0.1;
constexpr size_t kHopsPerBlock = 4;  // 400ms momentary blocks
constexpr double kAbsoluteGateLufs = -60;
// A block counts as speech if it's within this many dB of the loud end
// (80th percentile) of its neighbourhood — pauses and breaths sit well below.
constexpr double kSpeechRangeDb = 12;
constexpr size_t kLevelWindowBlocks = 25;  // ±2.5s neighbourhood
constexpr size_t kSmoothRadiusBlocks = 8;  // ±0.8s box, applied twice

double percentile(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5)];
}

double median(std::vector<double> v) { return percentile(std::move(v), 0.5); }

void boxSmooth(std::vector<double>& x, size_t radius) {
  const size_t n = x.size();
  std::vector<double> out(n);
  for (size_t i = 0; i < n; i++) {
    const size_t lo = i >= radius ? i - radius : 0;
    const size_t hi = std::min(n - 1, i + radius);
    double s = 0;
    for (size_t k = lo; k <= hi; k++) s += x[k];
    out[i] = s / static_cast<double>(hi - lo + 1);
  }
  x = std::move(out);
}

}  // namespace

LevelerLane planLevelerLane(const std::vector<double>& energy, size_t hop, const LevelerOptions& options) {
  LevelerLane lane;
  if (energy.size() < kHopsPerBlock) return lane;

  // Momentary loudness; block b covers hops [b, b+4), centred at (b+2)*hop.
  const size_t blocks = energy.size() - kHopsPerBlock + 1;
  std::vector<double> loud(blocks);
  for (size_t b = 0; b < blocks; b++) {
    double e = 0;
    for (size_t k = 0; k < kHopsPerBlock; k++) e += energy[b + k];
    loud[b] = energyToLufs(e / kHopsPerBlock);
  }

  // Local speech level per block (NaN where there's no speech nearby).
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> level(blocks, nan);
  std::vector<double> window, speech;
  for (size_t b = 0; b < blocks; b++) {
    const size_t lo = b >= kLevelWindowBlocks ? b - kLevelWindowBlocks : 0;
    const size_t hi = std::min(blocks - 1, b + kLevelWindowBlocks);
    window.clear();
    for (size_t k = lo; k <= hi; k++)
      if (loud[k] > kAbsoluteGateLufs) window.push_back(loud[k]);
    if (window.empty()) continue;
    const double cutoff = percentile(window, 0.8) - kSpeechRangeDb;
    speech.clear();
    for (double v : window)
      if (v > cutoff) speech.push_back(v);
    level[b] = median(speech);
  }

  std::vector<double> valid;
  for (double v : level)
    if (!std::isnan(v)) valid.push_back(v);
  if (valid.empty()) return lane;  // all silence
  const double target = std::isnan(options.targetLufs) ? median(valid) : options.targetLufs;

  // Gain in dB; fill gaps by interpolating between the nearest valid
  // neighbours (hold at the ends) so pauses ride along with the speech
  // around them.
  std::vector<double> gainDb(blocks, 0.0);
  std::vector<size_t> idx;
  for (size_t b = 0; b < blocks; b++)
    if (!std::isnan(level[b])) {
      gainDb[b] = std::min(options.maxBoostDb, std::max(-options.maxCutDb, target - level[b]));
      idx.push_back(b);
    }
  for (size_t b = 0; b < idx.front(); b++) gainDb[b] = gainDb[idx.front()];
  for (size_t b = idx.back() + 1; b < blocks; b++) gainDb[b] = gainDb[idx.back()];
  for (size_t k = 0; k + 1 < idx.size(); k++) {
    const size_t a = idx[k], c = idx[k + 1];
    for (size_t b = a + 1; b < c; b++) {
      const double t = static_cast<double>(b - a) / static_cast<double>(c - a);
      gainDb[b] = gainDb[a] + (gainDb[c] - gainDb[a]) * t;
    }
  }

  boxSmooth(gainDb, kSmoothRadiusBlocks);
  boxSmooth(gainDb, kSmoothRadiusBlocks);

  lane.hop = hop;
  lane.firstCentre = kHopsPerBlock / 2 * hop;
  lane.gainDb = std::move(gainDb);
  return lane;
}

double levelerGainAt(const LevelerLane& lane, size_t i, double amount) {
  if (lane.empty()) return 1.0;
  const size_t blocks = lane.gainDb.size();
  const double pos = (static_cast<double>(i) - static_cast<double>(lane.firstCentre)) / static_cast<double>(lane.hop);
  auto lin = [&](size_t b) { return std::pow(10.0, amount * lane.gainDb[b] / 20.0); };
  if (pos <= 0) return lin(0);
  if (pos >= static_cast<double>(blocks - 1)) return lin(blocks - 1);
  const size_t k = static_cast<size_t>(pos);
  const double t = pos - static_cast<double>(k);
  const double a = lin(k), b = lin(k + 1);
  return a + (b - a) * t;
}

void levelVoice(float* const* channels, size_t numChannels, size_t length, double sampleRate,
                const LevelerOptions& options) {
  const size_t hop = std::max<size_t>(1, static_cast<size_t>(std::llround(kHopSeconds * sampleRate)));
  const LevelerLane lane = planLevelerLane(kWeightedHopEnergy(channels, numChannels, length, sampleRate, hop), hop, options);
  if (lane.empty()) return;
  for (size_t i = 0; i < length; i++) {
    const double g = levelerGainAt(lane, i, 1.0);
    for (size_t ch = 0; ch < numChannels; ch++) channels[ch][i] = static_cast<float>(channels[ch][i] * g);
  }
}

}  // namespace sermon
