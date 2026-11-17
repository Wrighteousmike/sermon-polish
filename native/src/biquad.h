#pragma once

#include <cstddef>

namespace sermon {

// RBJ cookbook biquad, double precision state, transposed direct form II.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double z1 = 0, z2 = 0;

  // Q for lowpass/highpass is a *linear* Q here. (Web Audio interprets it in
  // dB; callers porting browser presets convert with webAudioQ().)
  static Biquad lowpass(double fs, double freq, double q);
  static Biquad highpass(double fs, double freq, double q);
  static Biquad peaking(double fs, double freq, double q, double gainDb);
  static Biquad notch(double fs, double freq, double q);
  static Biquad highShelf(double fs, double freq, double gainDb);  // slope S = 1
  static Biquad lowShelf(double fs, double freq, double gainDb);   // slope S = 1
  static Biquad identity() { return Biquad(); }

  double process(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void processBlock(float* x, size_t n) {
    for (size_t i = 0; i < n; i++) x[i] = static_cast<float>(process(x[i]));
  }
  void reset() { z1 = z2 = 0; }
};

// Web Audio lowpass/highpass Q is expressed in dB.
double webAudioQ(double qDb);

}  // namespace sermon
