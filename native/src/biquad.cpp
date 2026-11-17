#include "biquad.h"

#include <cmath>

namespace sermon {

namespace {
constexpr double kPi = 3.14159265358979323846;

Biquad make(double b0, double b1, double b2, double a0, double a1, double a2) {
  Biquad f;
  f.b0 = b0 / a0;
  f.b1 = b1 / a0;
  f.b2 = b2 / a0;
  f.a1 = a1 / a0;
  f.a2 = a2 / a0;
  return f;
}
}  // namespace

double webAudioQ(double qDb) { return std::pow(10.0, qDb / 20.0); }

Biquad Biquad::lowpass(double fs, double freq, double q) {
  const double w0 = 2 * kPi * freq / fs, c = std::cos(w0), alpha = std::sin(w0) / (2 * q);
  return make((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

Biquad Biquad::highpass(double fs, double freq, double q) {
  const double w0 = 2 * kPi * freq / fs, c = std::cos(w0), alpha = std::sin(w0) / (2 * q);
  return make((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

Biquad Biquad::peaking(double fs, double freq, double q, double gainDb) {
  const double A = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2 * kPi * freq / fs, c = std::cos(w0), alpha = std::sin(w0) / (2 * q);
  return make(1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
}

Biquad Biquad::notch(double fs, double freq, double q) {
  const double w0 = 2 * kPi * freq / fs, c = std::cos(w0), alpha = std::sin(w0) / (2 * q);
  return make(1, -2 * c, 1, 1 + alpha, -2 * c, 1 - alpha);
}

Biquad Biquad::highShelf(double fs, double freq, double gainDb) {
  const double A = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2 * kPi * freq / fs, c = std::cos(w0);
  const double alpha = std::sin(w0) / 2 * std::sqrt(2.0);  // S = 1
  const double t = 2 * std::sqrt(A) * alpha;
  return make(A * ((A + 1) + (A - 1) * c + t), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - t),
              (A + 1) - (A - 1) * c + t, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - t);
}

Biquad Biquad::lowShelf(double fs, double freq, double gainDb) {
  const double A = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2 * kPi * freq / fs, c = std::cos(w0);
  const double alpha = std::sin(w0) / 2 * std::sqrt(2.0);  // S = 1
  const double t = 2 * std::sqrt(A) * alpha;
  return make(A * ((A + 1) - (A - 1) * c + t), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - t),
              (A + 1) + (A - 1) * c + t, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - t);
}

}  // namespace sermon
