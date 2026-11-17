#include "fft.h"

#include <cmath>
#include <utility>

namespace sermon {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

Fft::Fft(size_t n) : n_(n), rev_(n), tw_(n / 2) {
  size_t bits = 0;
  while ((size_t{1} << bits) < n) bits++;
  for (size_t i = 0; i < n; i++) {
    size_t r = 0;
    for (size_t b = 0; b < bits; b++)
      if (i & (size_t{1} << b)) r |= size_t{1} << (bits - 1 - b);
    rev_[i] = r;
  }
  for (size_t i = 0; i < n / 2; i++) tw_[i] = std::polar(1.0, -2.0 * kPi * double(i) / double(n));
}

void Fft::transform(std::vector<std::complex<double>>& a, int sign) const {
  using cd = std::complex<double>;
  for (size_t i = 0; i < n_; i++)
    if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
  for (size_t len = 2; len <= n_; len <<= 1) {
    const size_t step = n_ / len;
    for (size_t i = 0; i < n_; i += len) {
      for (size_t k = 0; k < len / 2; k++) {
        cd w = tw_[k * step];
        if (sign > 0) w = std::conj(w);
        const cd u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
      }
    }
  }
}

}  // namespace sermon
