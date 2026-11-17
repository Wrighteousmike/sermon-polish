#include "loudness.h"

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace sermon {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double z1 = 0, z2 = 0;
  double process(double x) {  // transposed direct form II
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// Stage 1: high shelf modelling the head's acoustic effect.
Biquad shelf(double fs) {
  const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
  const double K = std::tan(kPi * f0 / fs);
  const double Vh = std::pow(10.0, G / 20.0);
  const double Vb = std::pow(Vh, 0.4996667741545416);
  const double a0 = 1.0 + K / Q + K * K;
  Biquad f;
  f.b0 = (Vh + Vb * K / Q + K * K) / a0;
  f.b1 = 2.0 * (K * K - Vh) / a0;
  f.b2 = (Vh - Vb * K / Q + K * K) / a0;
  f.a1 = 2.0 * (K * K - 1.0) / a0;
  f.a2 = (1.0 - K / Q + K * K) / a0;
  return f;
}

// Stage 2: RLB highpass.
Biquad highpass(double fs) {
  const double f0 = 38.13547087602444, Q = 0.5003270373238773;
  const double K = std::tan(kPi * f0 / fs);
  const double a0 = 1.0 + K / Q + K * K;
  Biquad f;
  f.b0 = 1.0;
  f.b1 = -2.0;
  f.b2 = 1.0;
  f.a1 = 2.0 * (K * K - 1.0) / a0;
  f.a2 = (1.0 - K / Q + K * K) / a0;
  return f;
}

}  // namespace

std::vector<double> kWeightedHopEnergyGained(const float* const* channels, size_t numChannels, size_t length,
                                             double sampleRate, size_t hopSamples, double scale, const float* gain) {
  const size_t hops = hopSamples ? length / hopSamples : 0;
  std::vector<double> energy(hops, 0.0);
  for (size_t ch = 0; ch < numChannels; ch++) {
    Biquad s = shelf(sampleRate), h = highpass(sampleRate);
    const float* x = channels[ch];
    for (size_t hop = 0; hop < hops; hop++) {
      double sum = 0;
      for (size_t i = hop * hopSamples, end = i + hopSamples; i < end; i++) {
        const double v = gain ? double(x[i]) * scale * double(gain[i]) : double(x[i]) * scale;
        const double y = h.process(s.process(v));
        sum += y * y;
      }
      energy[hop] += sum / static_cast<double>(hopSamples);
    }
  }
  return energy;
}

std::vector<double> kWeightedHopEnergy(const float* const* channels, size_t numChannels, size_t length,
                                       double sampleRate, size_t hopSamples) {
  return kWeightedHopEnergyGained(channels, numChannels, length, sampleRate, hopSamples, 1.0, nullptr);
}

double energyToLufs(double meanSquare) {
  if (meanSquare <= 1e-20) return -200.0;
  return -0.691 + 10.0 * std::log10(meanSquare);
}


struct HopEnergyMeter::Impl {
  size_t hop, nCh, inHop = 0;
  std::vector<Biquad> shelfs, highs;
  std::vector<double> sums;
  Impl(double fs, size_t channels, size_t hopSamples) : hop(std::max<size_t>(1, hopSamples)), nCh(channels), shelfs(channels, shelf(fs)), highs(channels, highpass(fs)), sums(channels, 0.0) {}
};

HopEnergyMeter::HopEnergyMeter(double fs, size_t channels, size_t hopSamples) : impl_(new Impl(fs, channels, hopSamples)) {}
HopEnergyMeter::~HopEnergyMeter() { delete impl_; }

void HopEnergyMeter::feed(const float* const* ch, size_t n, const float* gain) {
  Impl& m = *impl_;
  for (size_t i = 0; i < n; i++) {
    for (size_t c = 0; c < m.nCh; c++) {
      const double v = gain ? double(ch[c][i]) * double(gain[i]) : double(ch[c][i]);
      const double y = m.highs[c].process(m.shelfs[c].process(v));
      m.sums[c] += y * y;
    }
    if (++m.inHop == m.hop) {
      double e = 0;
      for (size_t c = 0; c < m.nCh; c++) { e += m.sums[c] / double(m.hop); m.sums[c] = 0; }
      energy_.push_back(e);
      m.inHop = 0;
    }
  }
}

double integratedLufsFromHops(const std::vector<double>& e) {
  if (e.size() < 4) return -70;
  std::vector<double> blocks;
  for (size_t b = 0; b + 4 <= e.size(); b++) blocks.push_back((e[b] + e[b + 1] + e[b + 2] + e[b + 3]) / 4);
  double sum = 0;
  size_t n = 0;
  for (double m : blocks)
    if (energyToLufs(m) > -70) { sum += m; n++; }
  if (!n) return -70;
  const double gate = energyToLufs(sum / double(n)) - 10;
  double sum2 = 0;
  size_t n2 = 0;
  for (double m : blocks)
    if (energyToLufs(m) > -70 && energyToLufs(m) > gate) { sum2 += m; n2++; }
  return n2 ? energyToLufs(sum2 / double(n2)) : energyToLufs(sum / double(n));
}

double integratedLufs(const float* const* channels, size_t numChannels, size_t length, double sampleRate) {
  const size_t hop = std::max<size_t>(1, static_cast<size_t>(std::llround(0.1 * sampleRate)));
  return integratedLufsFromHops(kWeightedHopEnergy(channels, numChannels, length, sampleRate, hop));
}

namespace {
constexpr int kOversample = 4;
constexpr int kHalfTaps = 8;  // 16 taps per phase

double besselI0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 40; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}

struct Interp {
  double c[kOversample][2 * kHalfTaps];
  Interp() {
    const double beta = 9.0, pi = 3.14159265358979323846;
    for (int p = 0; p < kOversample; p++) {
      const double frac = double(p) / kOversample;
      double sum = 0;
      for (int k = -kHalfTaps + 1; k <= kHalfTaps; k++) {
        const double t = double(k) - frac;
        const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(pi * t) / (pi * t);
        const double r = t / kHalfTaps;
        const double w = std::fabs(r) < 1 ? besselI0(beta * std::sqrt(1 - r * r)) / besselI0(beta) : 0.0;
        c[p][k + kHalfTaps - 1] = sinc * w;
        sum += c[p][k + kHalfTaps - 1];
      }
      for (int k = 0; k < 2 * kHalfTaps; k++) c[p][k] /= sum;
    }
  }
};
}  // namespace

double truePeak(const float* const* ch, size_t nCh, size_t n, float* perSample) {
  static const Interp interp;
  double peak = 0;
  if (perSample) std::fill(perSample, perSample + n, 0.0f);
  for (size_t c = 0; c < nCh; c++) {
    const float* x = ch[c];
    for (size_t i = 0; i < n; i++) {
      double m = std::fabs(double(x[i]));
      for (int p = 1; p < kOversample; p++) {
        double y = 0;
        for (int k = -kHalfTaps + 1; k <= kHalfTaps; k++) {
          const std::ptrdiff_t idx = std::ptrdiff_t(i) + k;
          if (idx >= 0 && idx < std::ptrdiff_t(n)) y += x[idx] * interp.c[p][k + kHalfTaps - 1];
        }
        m = std::max(m, std::fabs(y));
      }
      peak = std::max(peak, m);
      if (perSample) perSample[i] = std::max(perSample[i], float(m));
    }
  }
  return peak;
}

double truePeakBound() {
  static const Interp interp;
  static const double bound = [] {
    double best = 1.0;
    for (int p = 1; p < kOversample; p++) {
      double sum = 0;
      for (int j = 0; j < 2 * kHalfTaps; j++) sum += std::fabs(interp.c[p][j]);
      best = std::max(best, sum);
    }
    return best;
  }();
  return bound;
}

double truePeakWindow(const float* w) {
  static const Interp interp;
  double m = std::fabs(double(w[kHalfTaps - 1]));  // x[k]
  for (int p = 1; p < kOversample; p++) {
    double y = 0;
    for (int j = 0; j < 2 * kHalfTaps; j++) y += w[j] * interp.c[p][j];
    m = std::max(m, std::fabs(y));
  }
  return m;
}

double truePeakAt(const float* const* ch, size_t nCh, size_t n, size_t i) {
  static const Interp interp;
  double m = 0;
  for (size_t c = 0; c < nCh; c++) {
    const float* x = ch[c];
    double mc = std::fabs(double(x[i]));
    for (int p = 1; p < kOversample; p++) {
      double y = 0;
      for (int k = -kHalfTaps + 1; k <= kHalfTaps; k++) {
        const std::ptrdiff_t idx = std::ptrdiff_t(i) + k;
        if (idx >= 0 && idx < std::ptrdiff_t(n)) y += x[idx] * interp.c[p][k + kHalfTaps - 1];
      }
      mc = std::max(mc, std::fabs(y));
    }
    m = std::max(m, mc);
  }
  return m;
}

}  // namespace sermon
