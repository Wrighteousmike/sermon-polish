#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace sermon {

// Iterative radix-2 complex FFT of a fixed power-of-two size.
class Fft {
 public:
  explicit Fft(size_t n);
  size_t size() const { return n_; }
  // sign -1: forward (e^-i), +1: inverse without the 1/n scaling.
  void transform(std::vector<std::complex<double>>& a, int sign) const;

 private:
  size_t n_;
  std::vector<size_t> rev_;
  std::vector<std::complex<double>> tw_;
};

}  // namespace sermon
