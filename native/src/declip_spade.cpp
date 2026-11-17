#include "declip_spade.h"

#include "fft.h"

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <thread>
#include <vector>

namespace sermon {

namespace {

using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

enum : uint8_t { kReliable = 0, kHigh = 1, kLow = 2 };

// A-SPADE on one (windowed, clip-level-normalised) frame. Analysis operator
// A = zero-pad to M=2N, DFT, scale by 1/sqrt(M): a Parseval tight frame
// (A^H A = I), which makes the constrained least-squares step a plain
// projection of A^H(z - u) onto the consistency set.
struct FrameResult {
  bool converged = false;  // residual reached epsilon before the iteration cap
  size_t sparsity = 0;     // unique spectral bins in use when it stopped
  double residual = 0;
};

FrameResult spadeFrame(const Fft& fft, const std::vector<double>& y, const std::vector<uint8_t>& mask,
                       std::vector<double>& x, const SpadeOptions& opt) {
  FrameResult result;
  const size_t N = y.size(), M = 2 * N, half = M / 2;
  const double inv = 1.0 / std::sqrt(double(M));
  x = y;

  std::vector<cd> spec(M), z(M), u(M, cd(0, 0)), w(M);
  std::vector<double> mags(half + 1), scratch;

  auto analyse = [&]() {
    std::fill(spec.begin(), spec.end(), cd(0, 0));
    for (size_t i = 0; i < N; i++) spec[i] = x[i];
    fft.transform(spec, -1);
    for (auto& c : spec) c *= inv;
  };
  analyse();

  size_t k = 1;
  for (int it = 0; it < opt.maxIterations; it++) {
    // z = H_k(A x + u) on the unique half-spectrum, mirrored (real signal).
    for (size_t i = 0; i <= half; i++) mags[i] = std::abs(spec[i] + u[i]);
    scratch = mags;
    const size_t kk = std::min(k, half + 1);
    std::nth_element(scratch.begin(), scratch.begin() + (kk - 1), scratch.end(), std::greater<double>());
    const double thr = scratch[kk - 1];
    for (size_t i = 0; i <= half; i++) z[i] = mags[i] >= thr ? spec[i] + u[i] : cd(0, 0);
    for (size_t i = 1; i < half; i++) z[M - i] = std::conj(z[i]);

    // x = proj_Gamma(A^H (z - u))
    for (size_t i = 0; i < M; i++) w[i] = z[i] - u[i];
    fft.transform(w, +1);
    for (size_t i = 0; i < N; i++) {
      const double v = w[i].real() * inv;
      switch (mask[i]) {
        case kReliable: x[i] = y[i]; break;
        case kHigh: x[i] = std::max(v, y[i]); break;
        default: x[i] = std::min(v, y[i]); break;
      }
    }

    analyse();
    double res = 0;
    for (size_t i = 0; i < M; i++) {
      const cd d = spec[i] - z[i];
      u[i] += d;
      res += std::norm(d);
    }
    result.residual = std::sqrt(res);
    result.sparsity = kk;
    if (result.residual <= opt.epsilon) {
      result.converged = true;
      break;
    }
    k++;
  }
  return result;
}

struct Detected {
  double pos = 0, neg = 0;  // 0 = that side isn't clipped
};

constexpr double kDetectTolerance = 1e-3;  // relative to the peak
constexpr int kMinClipEvents = 3;          // runs of >=2 samples at the peak
// Sum of squared window weights a sample needs from accepted frames (a full
// set of overlapping Hann frames gives 1.5).
constexpr double kMinOverlapWeight = 0.4;

Detected detect(const float* x, size_t n) {
  float mx = 0, mn = 0;
  for (size_t i = 0; i < n; i++) {
    mx = std::max(mx, x[i]);
    mn = std::min(mn, x[i]);
  }
  Detected d;
  auto countEvents = [&](double level, bool positive) {
    int events = 0;
    size_t run = 0;
    for (size_t i = 0; i <= n; i++) {
      const bool at = i < n && (positive ? x[i] >= level : x[i] <= level);
      if (at) {
        run++;
      } else {
        if (run >= 2) events++;
        run = 0;
      }
    }
    return events;
  };
  if (mx > 0 && countEvents(mx * (1 - kDetectTolerance), true) >= kMinClipEvents) d.pos = mx * (1 - kDetectTolerance);
  if (mn < 0 && countEvents(mn * (1 - kDetectTolerance), false) >= kMinClipEvents) d.neg = mn * (1 - kDetectTolerance);
  return d;
}

}  // namespace

ClipDetection detectClipping(const float* x, size_t n) {
  ClipDetection r;
  const Detected d = detect(x, n);
  r.thresholdPos = d.pos;
  r.thresholdNeg = d.neg;
  for (size_t i = 0; i < n; i++)
    if ((d.pos > 0 && x[i] >= d.pos) || (d.neg < 0 && x[i] <= d.neg)) r.clippedSamples++;
  return r;
}

SpadeStats declipSpade(float* samples, size_t length, double sampleRate, const SpadeOptions& opt) {
  SpadeStats stats;
  size_t N = opt.frameLength;
  if (N == 0) {
    N = 512;
    while (double(N) < 0.046 * sampleRate && N < 8192) N <<= 1;
  }
  const size_t hop = N / 4;
  if (length < 2 || N < 8) return stats;

  Detected det = detect(samples, length);
  if (!std::isnan(opt.thresholdPos)) det.pos = opt.thresholdPos;
  if (!std::isnan(opt.thresholdNeg)) det.neg = opt.thresholdNeg;
  stats.thresholdPos = det.pos;
  stats.thresholdNeg = det.neg;
  if (det.pos <= 0 && det.neg >= 0) return stats;

  auto isHigh = [&](float s) { return det.pos > 0 && s >= det.pos; };
  auto isLow = [&](float s) { return det.neg < 0 && s <= det.neg; };
  std::vector<uint8_t> clipped(length, kReliable);
  for (size_t i = 0; i < length; i++) {
    if (isHigh(samples[i])) clipped[i] = kHigh;
    else if (isLow(samples[i])) clipped[i] = kLow;
    if (clipped[i]) stats.clippedSamples++;
  }
  if (!stats.clippedSamples) return stats;

  const double scale = std::max(det.pos, -det.neg);
  std::vector<double> win(N);
  for (size_t i = 0; i < N; i++) win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(N));  // periodic Hann

  // Frames start at -(N-hop), step hop, until they pass the end.
  const std::ptrdiff_t first = -std::ptrdiff_t(N - hop);
  std::vector<std::ptrdiff_t> starts;
  for (std::ptrdiff_t s = first; s < std::ptrdiff_t(length); s += std::ptrdiff_t(hop)) {
    size_t count = 0;
    for (std::ptrdiff_t i = std::max(std::ptrdiff_t(0), s); i < std::min(std::ptrdiff_t(length), s + std::ptrdiff_t(N)); i++) count += clipped[size_t(i)] != 0;
    if (!count) continue;
    if (double(count) > opt.maxClippedFraction * double(N)) {
      stats.framesSkipped++;
      continue;
    }
    starts.push_back(s);
  }

  const Fft fft(2 * N);
  std::vector<double> num(length, 0.0), den(length, 0.0);
  unsigned nThreads = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
  const size_t batch = std::max<size_t>(1, 8 * nThreads);

  for (size_t b0 = 0; b0 < starts.size(); b0 += batch) {
    const size_t b1 = std::min(starts.size(), b0 + batch);
    std::vector<std::vector<double>> results(b1 - b0);
    std::atomic<size_t> next{b0};
    auto worker = [&]() {
      std::vector<double> y(N), x;
      std::vector<uint8_t> mask(N);
      for (size_t f = next++; f < b1; f = next++) {
        const std::ptrdiff_t s = starts[f];
        for (size_t i = 0; i < N; i++) {
          const std::ptrdiff_t idx = s + std::ptrdiff_t(i);
          if (idx < 0 || idx >= std::ptrdiff_t(length)) {
            y[i] = 0;
            mask[i] = kReliable;
          } else {
            y[i] = win[i] * double(samples[idx]) / scale;
            mask[i] = clipped[size_t(idx)];
          }
        }
        const FrameResult fr = spadeFrame(fft, y, mask, x, opt);
        bool ok = fr.converged;
        for (size_t i = 0; ok && i < N; i++)
          if (mask[i] && win[i] > 0.05 && std::fabs(x[i]) / win[i] > opt.maxOvershoot) ok = false;
        if (ok) results[f - b0] = x;  // empty = rejected
      }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nThreads && t < b1 - b0; t++) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();

    // Weighted overlap-add, accumulated only at clipped samples.
    for (size_t f = b0; f < b1; f++) {
      if (results[f - b0].empty()) {
        stats.framesRejected++;
        continue;
      }
      const std::ptrdiff_t s = starts[f];
      for (size_t i = 0; i < N; i++) {
        const std::ptrdiff_t idx = s + std::ptrdiff_t(i);
        if (idx < 0 || idx >= std::ptrdiff_t(length) || !clipped[size_t(idx)]) continue;
        num[size_t(idx)] += win[i] * results[f - b0][i] * scale;
        den[size_t(idx)] += win[i] * win[i];
      }
      stats.framesProcessed++;
    }
  }

  for (size_t i = 0; i < length; i++) {
    if (!clipped[i] || den[i] < kMinOverlapWeight) continue;
    const double est = num[i] / den[i];
    if (!std::isfinite(est)) continue;
    // Consistency: never come back below (above) the observed clip value.
    samples[i] = clipped[i] == kHigh ? float(std::max(est, double(samples[i]))) : float(std::min(est, double(samples[i])));
  }
  return stats;
}

}  // namespace sermon
