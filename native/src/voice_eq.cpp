#include <cmath>
#include "voice_eq.h"

#include <vector>

#include "biquad.h"

namespace sermon {

namespace {

struct ToneParams {
  double highpassHz, lowpassHz, shelfDb;
  bool tilt;
};

ToneParams paramsFor(Tone t) {
  switch (t) {
    case Tone::Tone2: return {80, 9500, -2, false};
    case Tone::Tone3: return {112, 9500, -5, true};
    case Tone::Reference:  // handled before paramsFor() is used
      return {112, 11600, -2, false};
    case Tone::Tone1:
    default: return {112, 11600, -2, false};
  }
}

constexpr double kBrightnessShelfHz = 1500;

}  // namespace

namespace {
// Even-order Butterworth as cascaded biquads (steep Pro-Q style filters).
void butterworth(std::vector<Biquad>& chain, double fs, double hz, int order, bool highpass) {
  constexpr double kPi = 3.14159265358979323846;
  for (int k = 1; k <= order / 2; k++) {
    const double q = 1.0 / (2.0 * std::cos((2.0 * k - 1.0) * kPi / (2.0 * order)));
    chain.push_back(highpass ? Biquad::highpass(fs, hz, q) : Biquad::lowpass(fs, hz, q));
  }
}

// Fitted to the response curve traced from the user's Pro-Q 3 screenshot
// (tools/trace_proq_curve.py, tools/fit_proq_curve.py): 0.17 dB rms against
// the traced curve at 48 kHz. The fit is for the total response, so the band
// parameters are equivalent rather than identical to the plugin's own
// bands. The fit's -1.1 dB overall offset is dropped (level is set later).
void referenceChain(std::vector<Biquad>& chain, double fs, double k) {
  butterworth(chain, fs, 97.05, 6, true);
  chain.push_back(Biquad::lowShelf(fs, 141.6, -1.48 * k));
  chain.push_back(Biquad::peaking(fs, 199.1, 0.703, -5.12 * k));
  chain.push_back(Biquad::peaking(fs, 631.0, 1.148, -2.09 * k));
  chain.push_back(Biquad::peaking(fs, 1086.9, 1.412, -6.11 * k));
  chain.push_back(Biquad::peaking(fs, 1863.9, 0.557, 0.72 * k));
  chain.push_back(Biquad::peaking(fs, 3431.3, 2.026, -4.02 * k));
  chain.push_back(Biquad::peaking(fs, 4019.9, 3.178, -5.85 * k));
  chain.push_back(Biquad::peaking(fs, 9302.4, 0.321, -7.73 * k));
  if (12930 < 0.49 * fs) butterworth(chain, fs, 12930, 4, false);
}
}  // namespace

std::vector<Biquad> voiceEqChain(double fs, Tone tone, double gainScale) {
  std::vector<Biquad> chain;
  if (tone == Tone::Reference) {
    referenceChain(chain, fs, gainScale);
    return chain;
  }
  const ToneParams p = paramsFor(tone);
  const double nyquistGuard = 0.49 * fs;
  chain.push_back(Biquad::highpass(fs, p.highpassHz, webAudioQ(0.7)));
  chain.push_back(Biquad::peaking(fs, 200, 1.0, -6));
  chain.push_back(Biquad::peaking(fs, 610, 0.8, -3.2));
  chain.push_back(Biquad::peaking(fs, 1080, 1.8, -6.7));
  chain.push_back(Biquad::peaking(fs, 7600, 1.0, -3.4));
  if (p.lowpassHz < nyquistGuard) chain.push_back(Biquad::lowpass(fs, p.lowpassHz, webAudioQ(0.7)));
  chain.push_back(Biquad::highShelf(fs, kBrightnessShelfHz, p.shelfDb));
  if (p.tilt) {
    // Net ~-1dB between 1kHz and 10kHz, flat outside.
    chain.push_back(Biquad::highShelf(fs, 1000, -1));
    if (10000 < nyquistGuard) chain.push_back(Biquad::highShelf(fs, 10000, 1));
  }
  return chain;
}

void voiceEq(float* x, size_t n, double fs, Tone tone) {
  auto chain = voiceEqChain(fs, tone);
  for (auto& f : chain) f.processBlock(x, n);
}

}  // namespace sermon
