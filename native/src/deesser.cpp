#include "deesser.h"

#include "amount.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "biquad.h"

namespace sermon {

namespace {

using Settings = amount::DeEssParams;
Settings settingsFor(Intensity i) { return amount::deEss(i); }

constexpr double kHopSeconds = 0.0025;
constexpr double kLookaheadSeconds = 0.005;
constexpr double kAttackSeconds = 0.0015, kReleaseSeconds = 0.025;
constexpr double kActiveRangeDb = 30;  // slices this far below the loud speech are pauses/hiss

double db(double p) { return 10.0 * std::log10(std::max(p, 1e-20)); }

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[size_t(p * double(v.size() - 1) + 0.5)];
}

// Streaming filters: the same chains as before, run sample by sample with their own state, so
// no full-length filtered copy of the audio is ever held. Pass 2 recreates them from zero state
// and sees exactly the same values as pass 1.
//  - crossover: Linkwitz-Riley 4th-order split at 4 kHz. low + high sums back to a flat (all-pass)
//    response, so the high band can be turned down by any amount without the phase problems of
//    subtracting a band-passed copy.
//  - body: 4th-order Butterworth band-pass 300 Hz-3 kHz, used for detection only.
struct Crossover {
  Biquad lp1, lp2, hp1, hp2;
  explicit Crossover(double fs) {
    const double q = 0.70710678;
    lp1 = lp2 = Biquad::lowpass(fs, 4000, q);
    hp1 = hp2 = Biquad::highpass(fs, 4000, q);
  }
  void split(double x, double& low, double& high) {
    low = lp2.process(lp1.process(x));
    high = hp2.process(hp1.process(x));
  }
};

struct BodyBand {
  Biquad hpA, lpA, hpB, lpB;
  explicit BodyBand(double fs) {
    constexpr double kPi = 3.14159265358979323846;
    const double qA = 1.0 / (2.0 * std::cos(1.0 * kPi / 8.0)), qB = 1.0 / (2.0 * std::cos(3.0 * kPi / 8.0));
    hpA = Biquad::highpass(fs, 300, qA);
    lpA = Biquad::lowpass(fs, std::min(3000.0, 0.45 * fs), qA);
    hpB = Biquad::highpass(fs, 300, qB);
    lpB = Biquad::lowpass(fs, std::min(3000.0, 0.45 * fs), qB);
  }
  double process(double x) { return lpB.process(hpB.process(lpA.process(hpA.process(x)))); }
};

}  // namespace

void deEss(float* x, size_t n, double fs, Intensity intensity) {
  const Settings s = settingsFor(intensity);
  const size_t hop = std::max<size_t>(1, size_t(std::llround(kHopSeconds * fs)));
  const size_t frames = n / hop;
  if (frames < 40) return;

  // Pass 1: energy of the sibilance band, the voice body and the whole signal in every 2.5 ms hop.
  const size_t hops = (n + hop - 1) / hop;
  std::vector<double> hs(hops, 0.0), hb(hops, 0.0), ht(hops, 0.0);
  {
    Crossover xo(fs);
    BodyBand body(fs);
    for (size_t i = 0; i < n; i++) {
      double low, high;
      xo.split(x[i], low, high);
      const double b = body.process(x[i]);
      const size_t h = i / hop;
      hs[h] += high * high;
      hb[h] += b * b;
      ht[h] += double(x[i]) * x[i];
    }
  }

  // Per-slice levels (dB) over a 2-hop (5 ms) window.
  std::vector<double> sDb(frames), bDb(frames), tDb(frames);
  for (size_t f = 0; f < frames; f++) {
    const size_t a = f * hop, b = std::min(n, a + 2 * hop);
    const double m = double(b - a);
    const double ss = hs[f] + (f + 1 < hops ? hs[f + 1] : 0.0), bb = hb[f] + (f + 1 < hops ? hb[f + 1] : 0.0),
                 tt = ht[f] + (f + 1 < hops ? ht[f + 1] : 0.0);
    sDb[f] = db(ss / m);
    bDb[f] = db(bb / m);
    tDb[f] = db(tt / m);
  }
  hs.clear(); hs.shrink_to_fit();
  hb.clear(); hb.shrink_to_fit();
  ht.clear(); ht.shrink_to_fit();

  // Active speech: not pauses or hiss. The reference is the loud end of the file.
  const double loud = percentile(tDb, 0.95);
  std::vector<double> ratio(frames), active;
  std::vector<bool> isActive(frames, false);
  for (size_t f = 0; f < frames; f++) {
    ratio[f] = sDb[f] - bDb[f];
    if (tDb[f] >= loud - kActiveRangeDb) {
      isActive[f] = true;
      active.push_back(ratio[f]);
    }
  }
  if (active.size() < 40) return;
  const double threshold = percentile(active, s.percentile);
  const double slope = 1.0 - 1.0 / s.ratio;

  // Required reduction per slice, then 5 ms lookahead (take the deepest reduction coming up).
  std::vector<double> reduction(frames, 0.0);
  for (size_t f = 0; f < frames; f++)
    if (isActive[f] && ratio[f] > threshold) reduction[f] = std::min(s.maxReductionDb, (ratio[f] - threshold) * slope);
  const size_t look = std::max<size_t>(1, size_t(std::llround(kLookaheadSeconds / kHopSeconds)));
  std::vector<double> ahead(frames, 0.0);
  for (size_t f = 0; f < frames; f++) {
    double m = 0;
    for (size_t k = f; k < std::min(frames, f + look); k++) m = std::max(m, reduction[k]);
    ahead[f] = m;
  }

  // Pass 2: per-sample gain (interpolated between slice centres, then a fast attack and short
  // release), applied to the band above 4 kHz only: out = low + g * high.
  const double aCoeff = std::exp(-1.0 / (kAttackSeconds * fs)), rCoeff = std::exp(-1.0 / (kReleaseSeconds * fs));
  Crossover xo(fs);
  double gainDb = 0;  // current reduction, >= 0
  for (size_t i = 0; i < n; i++) {
    const double pos = (double(i) - double(hop)) / double(hop);
    double target;
    if (pos <= 0) target = ahead[0];
    else if (pos >= double(frames - 1)) target = ahead[frames - 1];
    else {
      const size_t k = size_t(pos);
      const double t = pos - double(k);
      target = ahead[k] + (ahead[k + 1] - ahead[k]) * t;
    }
    const double coeff = target > gainDb ? aCoeff : rCoeff;
    gainDb = target + (gainDb - target) * coeff;
    const double g = std::pow(10.0, -gainDb / 20.0);
    double low, high;
    xo.split(x[i], low, high);
    x[i] = float(low + high * g);
  }
}

}  // namespace sermon
