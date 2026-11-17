#include "deesser_rt.h"

#include "../amount.h"

#include <algorithm>
#include <cmath>

namespace sermon::rt {

namespace {

constexpr double kHopSeconds = 0.0025;
constexpr double kAttackSeconds = 0.0015, kReleaseSeconds = 0.025;
constexpr double kActiveRangeDb = 30;
constexpr size_t kRing = 8;

using Settings = amount::DeEssParams;
Settings settingsFor(Intensity i) { return amount::deEss(i); }

double db(double p) { return 10.0 * std::log10(std::max(p, 1e-20)); }

// Same filters as the offline de-esser (native/src/deesser.cpp).
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

double DeEssStats::percentile(double p) const {
  if (sortedRatiosDb.empty()) return 0;
  return sortedRatiosDb[size_t(p * double(sortedRatiosDb.size() - 1) + 0.5)];
}

// ---------------- stats builder ----------------

DeEssStatsBuilder::DeEssStatsBuilder(double fs)
    : fs_(fs), hop_(std::max<size_t>(1, size_t(std::llround(kHopSeconds * fs)))) {
  Crossover xo(fs);
  lp1_ = xo.lp1; lp2_ = xo.lp2; hp1_ = xo.hp1; hp2_ = xo.hp2;
  BodyBand body(fs);
  bA1_ = body.hpA; bA2_ = body.lpA; bB1_ = body.hpB; bB2_ = body.lpB;
}

void DeEssStatsBuilder::feed(const float* x, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const double v = x[i];
    const double high = hp2_.process(hp1_.process(v));
    const double body = bB2_.process(bB1_.process(bA2_.process(bA1_.process(v))));
    ss_ += high * high;
    bb_ += body * body;
    tt_ += v * v;
    if (++inHop_ == hop_) {
      hs_.push_back(ss_); hb_.push_back(bb_); ht_.push_back(tt_);
      ss_ = bb_ = tt_ = 0;
      inHop_ = 0;
    }
  }
  // (the low band isn't needed for statistics; keep the filter objects consistent anyway)
}

std::shared_ptr<DeEssStats> DeEssStatsBuilder::finish() {
  // The last, partial hop counts too (as in the offline analysis).
  const size_t total = hs_.size() * hop_ + inHop_;
  std::vector<double> hs = hs_, hb = hb_, ht = ht_;
  if (inHop_ > 0) { hs.push_back(ss_); hb.push_back(bb_); ht.push_back(tt_); }
  const size_t hops = hs.size(), frames = total / hop_;
  auto stats = std::make_shared<DeEssStats>();
  if (frames < 40) return stats;
  std::vector<double> tDb(frames), ratio(frames);
  for (size_t f = 0; f < frames; f++) {
    const size_t a = f * hop_, b = std::min(total, a + 2 * hop_);
    const double m = double(b - a);
    const double ss = hs[f] + (f + 1 < hops ? hs[f + 1] : 0.0), bb = hb[f] + (f + 1 < hops ? hb[f + 1] : 0.0),
                 tt = ht[f] + (f + 1 < hops ? ht[f + 1] : 0.0);
    ratio[f] = db(ss / m) - db(bb / m);
    tDb[f] = db(tt / m);
  }
  std::vector<double> sorted = tDb;
  std::sort(sorted.begin(), sorted.end());
  stats->loudRefDb = sorted[size_t(0.95 * double(sorted.size() - 1) + 0.5)];
  for (size_t f = 0; f < frames; f++)
    if (tDb[f] >= stats->loudRefDb - kActiveRangeDb) stats->sortedRatiosDb.push_back(float(ratio[f]));
  std::sort(stats->sortedRatiosDb.begin(), stats->sortedRatiosDb.end());
  return stats;
}

// ---------------- the stage ----------------

struct DeEsserStage::Channel {
  Crossover xo;
  BodyBand body;
  // current hop accumulators
  double ss = 0, bb = 0, tt = 0;
  size_t inHop = 0;
  // completed hops (ring) and per-frame reductions (ring)
  double hopS[kRing] = {}, hopB[kRing] = {}, hopT[kRing] = {};
  double red[kRing] = {}, ahead[kRing] = {};
  int64_t hopsDone = 0;
  // delay line of (low, high) and the per-sample gain smoother
  std::vector<float> lowD, highD;
  size_t dpos = 0;
  int64_t sampleCount = 0;
  double gainDb = 0;
  Channel(double fs, size_t delay) : xo(fs), body(fs), lowD(delay, 0.0f), highD(delay, 0.0f) {}
};

DeEsserStage::DeEsserStage() = default;
DeEsserStage::~DeEsserStage() = default;

void DeEsserStage::prepare(double fs, int channels) {
  fs_ = fs;
  channels_ = channels;
  hop_ = std::max<size_t>(1, size_t(std::llround(kHopSeconds * fs)));
  delaySamples_ = int(4 * hop_);  // enough to see the frames the interpolation needs
  ch_.clear();
  for (int c = 0; c < channels; c++) ch_.push_back(std::make_unique<Channel>(fs, size_t(delaySamples_)));
}

void DeEsserStage::reset() {
  for (auto& c : ch_) c = std::make_unique<Channel>(fs_, size_t(delaySamples_));
  reductionDb_ = 0;
}

void DeEsserStage::setStats(std::shared_ptr<const DeEssStats> stats) {
  stats_ = std::move(stats);
  if (stats_ && stats_->valid()) threshold_ = stats_->percentile(percentile_);
}

void DeEsserStage::setIntensity(Intensity intensity) {
  const Settings s = settingsFor(intensity);
  percentile_ = s.percentile;
  ratio_ = s.ratio;
  maxReductionDb_ = s.maxReductionDb;
  if (stats_ && stats_->valid()) threshold_ = stats_->percentile(percentile_);
}

void DeEsserStage::process(float* const* chp, int n, int64_t) {
  const bool active = stats_ && stats_->valid();
  const double slope = 1.0 - 1.0 / ratio_;
  const double aCoeff = std::exp(-1.0 / (kAttackSeconds * fs_)), rCoeff = std::exp(-1.0 / (kReleaseSeconds * fs_));
  const size_t D = size_t(delaySamples_);
  double lastReduction = 0;
  for (int c = 0; c < channels_; c++) {
    Channel& ch = *ch_[size_t(c)];
    float* x = chp[c];
    for (int i = 0; i < n; i++) {
      double low, high;
      ch.xo.split(x[i], low, high);
      const double b = ch.body.process(x[i]);
      ch.ss += high * high;
      ch.bb += b * b;
      ch.tt += double(x[i]) * x[i];
      if (++ch.inHop == hop_) {
        const int64_t h = ch.hopsDone++;
        ch.hopS[h % kRing] = ch.ss; ch.hopB[h % kRing] = ch.bb; ch.hopT[h % kRing] = ch.tt;
        ch.ss = ch.bb = ch.tt = 0;
        ch.inHop = 0;
        if (h >= 1) {
          const int64_t f = h - 1;  // frame f spans hops f and f+1
          double r = 0;
          if (active) {
            const double m = double(2 * hop_);
            const double sDb = db((ch.hopS[f % kRing] + ch.hopS[(f + 1) % kRing]) / m);
            const double bDb = db((ch.hopB[f % kRing] + ch.hopB[(f + 1) % kRing]) / m);
            const double tDb = db((ch.hopT[f % kRing] + ch.hopT[(f + 1) % kRing]) / m);
            const double ratio = sDb - bDb;
            if (tDb >= stats_->loudRefDb - kActiveRangeDb && ratio > threshold_) r = std::min(maxReductionDb_, (ratio - threshold_) * slope);
          }
          ch.red[f % kRing] = r;
          if (f >= 1) ch.ahead[(f - 1) % kRing] = std::max(ch.red[(f - 1) % kRing], ch.red[f % kRing]);
        }
      }
      // delay the (low, high) pair by D samples
      const float oldLow = ch.lowD[ch.dpos], oldHigh = ch.highD[ch.dpos];
      ch.lowD[ch.dpos] = float(low);
      ch.highD[ch.dpos] = float(high);
      ch.dpos = (ch.dpos + 1) % D;
      const int64_t idx = ch.sampleCount - int64_t(D);  // index of the sample leaving the delay
      ch.sampleCount++;
      if (idx < 0) { x[i] = 0.0f; continue; }
      // reduction target at `idx`: interpolate between slice centres, as offline
      const double pos = (double(idx) - double(hop_)) / double(hop_);
      double target;
      if (!active) target = 0;
      else if (pos <= 0) target = ch.ahead[0];
      else {
        const int64_t k = int64_t(pos);
        const double t = pos - double(k);
        target = ch.ahead[k % kRing] + (ch.ahead[(k + 1) % kRing] - ch.ahead[k % kRing]) * t;
      }
      ch.gainDb = target + (ch.gainDb - target) * (target > ch.gainDb ? aCoeff : rCoeff);
      const double g = std::pow(10.0, -ch.gainDb / 20.0);
      x[i] = float(double(oldLow) + double(oldHigh) * g);
      lastReduction = ch.gainDb;
    }
  }
  reductionDb_ = lastReduction;
}

}  // namespace sermon::rt
