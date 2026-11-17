#include "limiter.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <utility>
#include <vector>

#include "loudness.h"

namespace sermon {

namespace {

// The limiter's smoothing as a streaming generator: `needAt(k)` gives the gain required at sample
// k (1 = none). The gain at i is a causal box average (length `look`) of the forward window minimum
// of `need` over [i, i+look): it ramps down over the lookahead and lands exactly on `need` at the
// peak, then releases with a 100 ms exponential. `emit(i, gain)` receives the result. Only
// O(look) state is held, never a full-length array.
template <class NeedAt, class Emit>
void smoothGain(size_t n, double fs, NeedAt needAt, Emit emit) {
  const size_t look = std::max<size_t>(1, size_t(std::llround(0.005 * fs)));
  const double release = std::exp(-1.0 / (0.1 * fs));
  std::deque<std::pair<size_t, float>> window;  // monotonic: increasing need values
  std::vector<float> fmRing(look, 1.0f);
  size_t nextNeed = 0;
  double run = 0, cur = 1;
  for (size_t i = 0; i < n; i++) {
    const size_t upTo = std::min(n, i + look);
    for (; nextNeed < upTo; nextNeed++) {
      const float v = needAt(nextNeed);
      while (!window.empty() && window.back().second >= v) window.pop_back();
      window.emplace_back(nextNeed, v);
    }
    while (window.front().first < i) window.pop_front();
    const float fm = window.front().second;
    run += fm;
    if (i >= look) run -= fmRing[i % look];
    fmRing[i % look] = fm;
    const double target = run / double(std::min(i + 1, look));
    cur = target < cur ? target : target + (cur - target) * release;
    emit(i, cur);
  }
}

// The gain curve that holds the true peak of (signal * scale) at the ceiling.
// Written into `g` (resized once and reused across passes, so only one full-length array exists).
void limiterGain(const float* const* ch, size_t nCh, size_t n, double fs, double scale, double ceiling, std::vector<float>& g) {
  g.resize(n);
  smoothGain(
      n, fs,
      [&](size_t k) {
        const double tp = truePeakAt(ch, nCh, n, k) * scale;
        return tp > ceiling ? float(ceiling / tp) : 1.0f;
      },
      [&](size_t i, double cur) { g[i] = float(cur); });
}

}  // namespace

void applyRequiredGain(float* const* ch, size_t nCh, size_t n, double fs, const std::vector<float>& need) {
  applyRequiredGainFn(ch, nCh, n, fs, [&](size_t k) { return need[k]; });
}

void applyRequiredGainFn(float* const* ch, size_t nCh, size_t n, double fs, const std::function<float(size_t)>& needAt) {
  smoothGain(
      n, fs, needAt,
      [&](size_t i, double cur) {
        for (size_t c = 0; c < nCh; c++) ch[c][i] = float(ch[c][i] * cur);
      });
}

double enforceTruePeak(float* const* ch, size_t nCh, size_t n, double fs, double ceilingDb) {
  const double ceiling = std::pow(10.0, ceilingDb / 20.0);
  double peak = truePeak(ch, nCh, n);
  if (peak <= ceiling) return 20 * std::log10(std::max(peak, 1e-8));
  std::vector<float> g;
  limiterGain(ch, nCh, n, fs, 1.0, ceiling, g);
  for (size_t c = 0; c < nCh; c++)
    for (size_t i = 0; i < n; i++) ch[c][i] = float(ch[c][i] * double(g[i]));
  peak = truePeak(ch, nCh, n);
  return 20 * std::log10(std::max(peak, 1e-8));
}

LimiterResult limitToStreaming(float* const* ch, size_t nCh, size_t n, double fs, double targetLufs, double ceilingDb,
                               double maxExtraDb) {
  LimiterResult r;
  r.measuredLufs = integratedLufs(ch, nCh, n, fs);
  if (r.measuredLufs <= -70) return r;  // silence

  // Limiting removes some average loudness, so a single gain-then-limit pass lands below target on
  // peaky material. Re-gain by the shortfall and limit again, at most maxExtraDb beyond the plain
  // gain so a very peaky file is never limited absurdly hard. The audio itself is never touched
  // until the end: every pass is the untouched signal plus a gain curve (one float per sample).
  constexpr double kToleranceDb = 0.1;
  constexpr int kMaxPasses = 6;
  const double ceiling = std::pow(10.0, ceilingDb / 20.0);
  const size_t hop = std::max<size_t>(1, size_t(std::llround(0.1 * fs)));
  const double baseGainDb = targetLufs - r.measuredLufs;
  double gainDb = baseGainDb;
  std::vector<float> g;
  for (int pass = 0; pass < kMaxPasses; pass++) {
    const double scale = std::pow(10.0, gainDb / 20.0);
    limiterGain(ch, nCh, n, fs, scale, ceiling, g);
    const double lufs = integratedLufsFromHops(kWeightedHopEnergyGained(ch, nCh, n, fs, hop, scale, g.data()));
    const double shortfall = targetLufs - lufs;
    r.appliedGainDb = gainDb;
    r.finalLufs = lufs;
    if (shortfall <= kToleranceDb || gainDb - baseGainDb >= maxExtraDb) break;
    gainDb = std::min(gainDb + shortfall, baseGainDb + maxExtraDb);
  }
  const double finalScale = std::pow(10.0, r.appliedGainDb / 20.0);

  // How hard was it limiting? The gain at each 10 ms window's loudest sample, over windows of speech.
  const size_t win = std::max<size_t>(1, size_t(std::llround(0.01 * fs)));
  std::vector<double> reductions;
  for (size_t start = 0; start + win <= n; start += win) {
    double a = 0;
    size_t at = start;
    for (size_t c = 0; c < nCh; c++)
      for (size_t i = start; i < start + win; i++) {
        const double v = std::fabs(double(ch[c][i])) * finalScale;
        if (v > a) { a = v; at = i; }
      }
    if (a < 0.01) continue;  // below -40 dBFS: not speech
    reductions.push_back(-20.0 * std::log10(std::max(double(g[at]), 1e-9)));
  }
  if (!reductions.empty()) {
    std::sort(reductions.begin(), reductions.end());
    auto pct = [&](double p) { return reductions[size_t(p * double(reductions.size() - 1) + 0.5)]; };
    r.medianReductionDb = pct(0.5);
    r.p90ReductionDb = pct(0.9);
    r.p99ReductionDb = pct(0.99);
    r.maxReductionDb = reductions.back();
  }

  for (size_t c = 0; c < nCh; c++)
    for (size_t i = 0; i < n; i++) ch[c][i] = float(double(ch[c][i]) * finalScale * double(g[i]));
  r.truePeakDb = 20 * std::log10(std::max(truePeak(ch, nCh, n), 1e-8));
  return r;
}

}  // namespace sermon
