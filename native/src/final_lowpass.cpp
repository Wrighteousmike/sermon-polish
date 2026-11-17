#include "final_lowpass.h"

#include <cmath>

#include "biquad.h"

namespace sermon {

void finalLowpass(float* x, size_t n, double fs, double cornerHz) {
  if (cornerHz >= 0.49 * fs) return;
  constexpr double kPi = 3.14159265358979323846;
  constexpr int kOrder = 12;
  for (int k = 1; k <= kOrder / 2; k++) {
    const double theta = (2.0 * k - 1.0) * kPi / (2.0 * kOrder);
    Biquad f = Biquad::lowpass(fs, cornerHz, 1.0 / (2.0 * std::cos(theta)));
    f.processBlock(x, n);
  }
}

}  // namespace sermon
