#include "resample.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <numeric>

namespace sermon {

namespace {
double besselI0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 50; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}
}  // namespace

std::vector<float> resample(const float* in, size_t n, int srcRate, int dstRate) {
  if (srcRate == dstRate) return std::vector<float>(in, in + n);
  const int g = std::gcd(srcRate, dstRate);
  const std::ptrdiff_t up = dstRate / g, down = srcRate / g;
  const double pi = 3.14159265358979323846, beta = 9.0;
  const double fc = std::min(1.0, double(up) / double(down)) * 0.96;  // cutoff, fraction of input Nyquist
  const std::ptrdiff_t half = static_cast<std::ptrdiff_t>(std::ceil(16.0 / std::min(1.0, double(up) / double(down))));
  const std::ptrdiff_t taps = 2 * half;

  // table[phase][k] for k in [-half+1, half]
  std::vector<float> table(size_t(up) * size_t(taps));
  const double i0b = besselI0(beta);
  for (std::ptrdiff_t ph = 0; ph < up; ph++) {
    const double frac = double(ph) / double(up);
    double sum = 0;
    std::vector<double> row(taps);
    for (std::ptrdiff_t k = -half + 1; k <= half; k++) {
      const double t = double(k) - frac;
      const double x = pi * t * fc;
      const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
      const double r = t / double(half);
      const double w = std::fabs(r) < 1 ? besselI0(beta * std::sqrt(1 - r * r)) / i0b : 0.0;
      row[size_t(k + half - 1)] = fc * sinc * w;
      sum += row[size_t(k + half - 1)];
    }
    for (std::ptrdiff_t k = 0; k < taps; k++) table[size_t(ph) * size_t(taps) + size_t(k)] = float(row[size_t(k)] / sum);
  }

  const size_t outLen = size_t(double(n) * double(up) / double(down));
  std::vector<float> out(outLen);
  for (size_t m = 0; m < outLen; m++) {
    const std::ptrdiff_t pos = std::ptrdiff_t(m) * down;
    const std::ptrdiff_t i0 = pos / up, ph = pos % up;
    const float* row = &table[size_t(ph) * size_t(taps)];
    double acc = 0;
    for (std::ptrdiff_t k = -half + 1; k <= half; k++) {
      const std::ptrdiff_t idx = i0 + k;
      if (idx >= 0 && idx < std::ptrdiff_t(n)) acc += double(in[idx]) * row[k + half - 1];
    }
    out[m] = float(acc);
  }
  return out;
}

}  // namespace sermon
