#include "wpe.h"

#include "amount.h"

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <complex>
#include <thread>
#include <vector>

#include "fft.h"
#include "resample.h"

namespace sermon {

namespace {

using cd = std::complex<double>;
using cf = std::complex<float>;
constexpr double kPi = 3.14159265358979323846;

// Solves the Hermitian positive-definite system A x = b (K x K, row-major)
// by Gaussian elimination with partial pivoting; A and b are overwritten.
// Returns false if the matrix is numerically singular.
bool solve(std::vector<cd>& A, std::vector<cd>& b, size_t K) {
  for (size_t c = 0; c < K; c++) {
    size_t piv = c;
    double best = std::abs(A[c * K + c]);
    for (size_t r = c + 1; r < K; r++)
      if (std::abs(A[r * K + c]) > best) { best = std::abs(A[r * K + c]); piv = r; }
    if (best < 1e-300) return false;
    if (piv != c) {
      for (size_t k = 0; k < K; k++) std::swap(A[c * K + k], A[piv * K + k]);
      std::swap(b[c], b[piv]);
    }
    for (size_t r = c + 1; r < K; r++) {
      const cd f = A[r * K + c] / A[c * K + c];
      if (f == cd(0, 0)) continue;
      for (size_t k = c; k < K; k++) A[r * K + k] -= f * A[c * K + k];
      b[r] -= f * b[c];
    }
  }
  for (size_t c = K; c-- > 0;) {
    cd s = b[c];
    for (size_t k = c + 1; k < K; k++) s -= A[c * K + k] * b[k];
    b[c] = s / A[c * K + c];
  }
  return true;
}

// WPE for one frequency bin. Y: observed STFT row (length T). Writes the
// dereverberated row to X.
void wpeBin(const cf* Y, cf* X, size_t T, size_t K, size_t D, int iterations, double strength, double floorLin) {
  std::vector<double> lambda(T), power(T);
  std::vector<cd> R(K * K), P(K), ybar(K);
  std::vector<cf> pred(T);

  for (size_t t = 0; t < T; t++) X[t] = Y[t];
  double mean = 0;
  for (size_t t = 0; t < T; t++) mean += std::norm(cd(Y[t]));
  mean /= double(std::max<size_t>(T, 1));
  if (mean <= 1e-30) return;  // silent bin
  const double eps = std::max(1e-12, mean * 1e-6);

  for (int it = 0; it < iterations; it++) {
    // Power estimate from the current dereverberated signal, smoothed over
    // +-1 frame.
    for (size_t t = 0; t < T; t++) power[t] = std::norm(cd(X[t]));
    for (size_t t = 0; t < T; t++) {
      const double a = power[t > 0 ? t - 1 : 0], b = power[t], c = power[t + 1 < T ? t + 1 : T - 1];
      lambda[t] = std::max((a + b + c) / 3.0, eps);
    }
    std::fill(R.begin(), R.end(), cd(0, 0));
    std::fill(P.begin(), P.end(), cd(0, 0));
    for (size_t t = D; t < T; t++) {
      const double w = 1.0 / lambda[t];
      // ybar[k] = Y[t - D - k]
      size_t avail = std::min(K, t - D + 1);
      for (size_t k = 0; k < avail; k++) ybar[k] = cd(Y[t - D - k]);
      for (size_t k = avail; k < K; k++) ybar[k] = cd(0, 0);
      const cd yt = cd(Y[t]);
      for (size_t k = 0; k < avail; k++) {
        const cd wk = w * ybar[k];
        for (size_t l = 0; l <= k; l++) R[k * K + l] += wk * std::conj(ybar[l]);
        P[k] += wk * std::conj(yt);
      }
    }
    double trace = 0;
    for (size_t k = 0; k < K; k++) {
      for (size_t l = 0; l < k; l++) R[l * K + k] = std::conj(R[k * K + l]);
      trace += R[k * K + k].real();
    }
    const double reg = 1e-6 * trace / double(K) + 1e-30;
    for (size_t k = 0; k < K; k++) R[k * K + k] += reg;
    if (!solve(R, P, K)) break;  // P now holds g

    for (size_t t = 0; t < T; t++) {
      cd p(0, 0);
      if (t >= D) {
        const size_t avail = std::min(K, t - D + 1);
        for (size_t k = 0; k < avail; k++) p += std::conj(P[k]) * cd(Y[t - D - k]);
      }
      pred[t] = cf(p);
      X[t] = cf(cd(Y[t]) - p);
    }
  }

  // Apply the strength and the magnitude floor on the final estimate.
  for (size_t t = 0; t < T; t++) {
    cd x = cd(Y[t]) - strength * cd(pred[t]);
    const double my = std::abs(cd(Y[t])), mx = std::abs(x);
    if (mx < floorLin * my && mx > 1e-30) x *= floorLin * my / mx;
    else if (mx <= 1e-30) x = cd(Y[t]) * floorLin;
    X[t] = cf(x);
  }
}

// Dereverberates one block (no overlap handling).
std::vector<float> wpeBlock(const float* in, size_t m, const WpeOptions& opt, size_t N, size_t K, size_t D,
                            const Fft& fft, const std::vector<double>& win, unsigned threads) {
  const size_t hop = N / 4, F = N / 2 + 1;
  const std::ptrdiff_t lead = std::ptrdiff_t(N - hop);
  const size_t T = size_t((std::ptrdiff_t(m) + lead + std::ptrdiff_t(hop) - 1) / std::ptrdiff_t(hop)) + 1;

  // STFT, stored per frequency: Y[f * T + t].
  std::vector<cf> Y(F * T), X(F * T);
  std::vector<cd> buf(N);
  for (size_t t = 0; t < T; t++) {
    const std::ptrdiff_t start = std::ptrdiff_t(t * hop) - lead;
    for (size_t i = 0; i < N; i++) {
      const std::ptrdiff_t idx = start + std::ptrdiff_t(i);
      buf[i] = (idx >= 0 && idx < std::ptrdiff_t(m)) ? win[i] * double(in[idx]) : 0.0;
    }
    fft.transform(buf, -1);
    for (size_t f = 0; f < F; f++) Y[f * T + t] = cf(buf[f]);
  }

  std::atomic<size_t> next{0};
  const double floorLin = std::pow(10.0, opt.floorDb / 20.0);
  auto worker = [&]() {
    for (size_t f = next++; f < F; f = next++)
      wpeBin(&Y[f * T], &X[f * T], T, K, D, opt.iterations, opt.strength, floorLin);
  };
  std::vector<std::thread> pool;
  for (unsigned i = 1; i < threads; i++) pool.emplace_back(worker);
  worker();
  for (auto& th : pool) th.join();

  // Inverse STFT with weighted overlap-add (analysis and synthesis windows
  // are both sqrt-Hann, normalised by the summed squared window).
  std::vector<double> out(m, 0.0), wsum(m, 0.0);
  for (size_t t = 0; t < T; t++) {
    for (size_t f = 0; f < F; f++) buf[f] = cd(X[f * T + t]);
    for (size_t f = 1; f < N / 2; f++) buf[N - f] = std::conj(buf[f]);
    fft.transform(buf, +1);
    const std::ptrdiff_t start = std::ptrdiff_t(t * hop) - lead;
    for (size_t i = 0; i < N; i++) {
      const std::ptrdiff_t idx = start + std::ptrdiff_t(i);
      if (idx < 0 || idx >= std::ptrdiff_t(m)) continue;
      out[size_t(idx)] += win[i] * buf[i].real() / double(N);
      wsum[size_t(idx)] += win[i] * win[i];
    }
  }
  std::vector<float> res(m);
  for (size_t i = 0; i < m; i++) res[i] = wsum[i] > 1e-9 ? float(out[i] / wsum[i]) : in[i];
  return res;
}

}  // namespace

void dereverbWpe(float* x, size_t n, double fs, const WpeOptions& opt) {
  size_t N = opt.frameLength;
  if (N == 0) {
    N = 256;
    while (double(N) < 0.043 * fs && N < 8192) N <<= 1;
  }
  const size_t hop = N / 4;
  const double hopSeconds = double(hop) / fs;
  size_t K = opt.taps > 0 ? size_t(opt.taps) : size_t(std::lround(opt.tapSeconds / hopSeconds));
  size_t D = opt.delay > 0 ? size_t(opt.delay) : std::max<size_t>(3, size_t(std::lround(0.03 / hopSeconds)));
  K = std::max<size_t>(2, K);
  if (n < 4 * N || opt.strength <= 0) return;

  const unsigned threads = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
  const Fft fft(N);
  std::vector<double> win(N);
  for (size_t i = 0; i < N; i++) win[i] = std::sqrt(0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(N)));

  const size_t block = std::max<size_t>(size_t(opt.blockSeconds * fs), 8 * N);
  const size_t overlap = std::min(block / 2, size_t(opt.overlapSeconds * fs));
  if (n <= block + overlap) {
    auto y = wpeBlock(x, n, opt, N, K, D, fft, win, threads);
    std::copy(y.begin(), y.end(), x);
    return;
  }

  // Blocks of `block` samples start every (block - overlap); each is
  // processed independently and crossfaded linearly over the overlaps.
  std::vector<float> result(n, 0.0f);
  const size_t stride = block - overlap;
  for (size_t start = 0; start < n; start += stride) {
    const size_t end = std::min(n, start + block);
    const bool first = start == 0, last = end == n;
    auto y = wpeBlock(x + start, end - start, opt, N, K, D, fft, win, threads);
    for (size_t i = 0; i < y.size(); i++) {
      double w = 1.0;
      if (!first && i < overlap) w = double(i) / double(overlap);
      if (!last && i + overlap >= y.size()) w = std::min(w, double(y.size() - 1 - i) / double(overlap));
      result[start + i] += float(w * y[i]);
    }
    if (last) break;
  }
  std::copy(result.begin(), result.end(), x);
}

namespace {

// What the band-limited WPE adds to a segment: up(WPE(down(x))) - up(down(x)), per sample.
void bandCorrection(const float* seg, size_t m, int rate, const WpeOptions& o, std::vector<float>& corr) {
  std::vector<float> low = resample(seg, m, rate, 16000);
  std::vector<float> processed = low;
  dereverbWpe(processed.data(), processed.size(), 16000.0, o);
  const std::vector<float> lowUp = resample(low.data(), low.size(), 16000, rate);
  const std::vector<float> procUp = resample(processed.data(), processed.size(), 16000, rate);
  corr.assign(m, 0.0f);
  for (size_t i = 0; i < m; i++) {
    const float l = i < lowUp.size() ? lowUp[i] : 0.0f, p = i < procUp.size() ? procUp[i] : 0.0f;
    corr[i] = p - l;
  }
}

}  // namespace

void dereverb(float* x, size_t n, double fs, Intensity intensity) {
  WpeOptions o;
  o.strength = amount::dereverbStrength(intensity);
  if (fs <= 16000.0 + 1) {
    dereverbWpe(x, n, fs, o);
    return;
  }
  // Long taps are expensive, and reverb matters mostly below 8 kHz: run WPE on a 16 kHz version and
  // put back what the resampling removed (> ~7.7 kHz is left untouched):
  //   out = x + (up(WPE(down(x))) - up(down(x))).
  // Done in 2-minute segments (4 s overlap, linear crossfade of the correction) so memory stays
  // small however long the recording is; WPE itself already re-estimates its filter per block.
  const int rate = static_cast<int>(std::lround(fs));
  const size_t chunk = size_t(120.0 * fs), overlap = size_t(4.0 * fs);
  std::vector<float> corr;
  for (size_t start = 0; start < n;) {
    const size_t end = std::min(n, start + chunk);
    const bool first = start == 0, last = end == n;
    bandCorrection(x + start, end - start, rate, o, corr);
    for (size_t i = 0; i < corr.size(); i++) {
      double w = 1.0;
      if (!first && i < overlap) w = double(i) / double(overlap);
      if (!last && i + overlap >= corr.size()) w = std::min(w, double(corr.size() - 1 - i) / double(overlap));
      x[start + i] += float(w * double(corr[i]));
    }
    if (last) break;
    start += chunk - overlap;
  }
}

}  // namespace sermon
