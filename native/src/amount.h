#pragma once

// Macro curves: one 0-1 amount per stage -> every underlying parameter of that stage.
//
// Each curve is a table of values at the five fixed positions
//     off(0)  less(0.25)  normal(0.5)  more(0.75)  max(1)
// interpolated linearly in between. Less/Normal/More are the app's original three presets, so those
// positions give exactly the old values; "off" is the stage barely doing anything (it is bypassed
// outright at 0), and "max" is new. The offline stages and the real-time chain both use these, so
// the live preview and the export always agree.
//
// Where "max" lands, and why (limits are set by audio practice, not just by what the code allows):
//  - Noise: DeepFilterNet's attenuation limit stops mattering past ~60 dB (the noise floor is
//    never that far above its own residual), so max is "no limit" with full wet signal.
//  - De-esser: 12 dB at ratio 6 is the top of what keeps a voice natural; past it speech lisps.
//  - Compressor: ratio 16 with a lower threshold is heavy sermon "radio" leveling short of limiting.
//  - Dereverb: WPE strength 1.0 subtracts exactly the predicted late reverb; 1.3 over-subtracts a
//    little to dry up a very live room, at the cost of a slightly thinner tail.
//  - Tone: scales the Studio-match EQ's cuts and boosts; 1.4x is a clearly more corrected voice
//    without any single band going past about -8.6 dB.
//  - Leveler: scales the planned gain moves; 1.5x evens out more without audible pumping.

#include <algorithm>
#include "intensity.h"

namespace sermon::amount {

struct Knots {
  double v[5];  // at 0, 0.25, 0.5, 0.75, 1
};

inline double at(const Knots& k, double a) {
  a = std::min(1.0, std::max(0.0, a));
  const double pos = a * 4.0;
  const int i = std::min(3, int(pos));
  const double t = pos - i;
  return k.v[i] + (k.v[i + 1] - k.v[i]) * t;
}
inline double at(const Knots& k, Intensity i) { return at(k, i.amount); }

// ---- Noise reduction ----
// DeepFilterNet attenuation limit (dB). Less 10, Normal 20, More 100 originally; More now sits at
// 45 and the last quarter opens the limit up (see the note above).
inline double noiseAttenDb(Intensity i) { return at({{0, 10, 20, 45, 100}}, i); }
// RNNoise has no attenuation limit, so it blends dry and wet.
inline double noiseWet(Intensity i) { return at({{0, 0.5, 0.75, 1.0, 1.0}}, i); }
// Hum notch Q: 12 normally, wider (8) at "more" and above to catch drifting mains hum.
inline double humNotchQ(Intensity i) { return at({{12, 12, 12, 8, 8}}, i); }

// ---- De-esser ----
struct DeEssParams { double percentile, ratio, maxReductionDb; };
inline DeEssParams deEss(Intensity i) {
  return {at({{0.99, 0.95, 0.93, 0.90, 0.85}}, i), at({{1.0, 2, 3, 4, 6}}, i), at({{0, 6, 7, 9, 12}}, i)};
}

// ---- Compressor ----
struct CompressorParams { double ratio, makeupDb, thresholdOffsetDb; };
inline CompressorParams compressor(Intensity i) {
  // Threshold offset is how far below the recording's loudness the threshold sits (6 dB originally).
  return {at({{1.0, 4, 8, 12, 16}}, i), at({{0, 0, 2, 4, 6}}, i), at({{6, 6, 6, 6, 9}}, i)};
}

// ---- Dereverb (WPE) ----
// Fraction of the predicted late reverb that is subtracted.
inline double dereverbStrength(Intensity i) { return at({{0, 0.5, 0.8, 1.0, 1.3}}, i); }

// ---- Tone (Studio-match EQ) ----
// Multiplier on every band's gain in the Studio-match curve (1.0 = the curve fitted to the owner's
// Pro Tools chain). The highpass and lowpass corners stay put.
inline double toneScale(Intensity i) { return at({{0, 0.5, 1.0, 1.2, 1.4}}, i); }

// ---- Leveler ----
// Multiplier on the planned gain moves (the leveler's own 1.0 is the original behaviour).
inline double levelerScale(Intensity i) { return at({{0, 0.5, 1.0, 1.25, 1.5}}, i); }

// ---- Loudness ----
// The integrated loudness (LUFS) the export is brought to. Normal is the streaming standard (-14);
// -18 is a quiet, dynamic result, -11 is about as loud as speech can go before the limiter's work
// becomes audible. (Off = leave the level alone; the limiter then only guards the true-peak ceiling.)
inline double targetLufs(Intensity i) { return at({{-14, -18, -14, -12.5, -11}}, i); }

}  // namespace sermon::amount
