#pragma once

#include <cstddef>

#include "compressor.h"  // Intensity

namespace sermon {

struct WpeOptions {
  // STFT frame length. 0 = auto (~43ms rounded to a power of two; hop is a
  // quarter of it).
  size_t frameLength = 0;
  // Prediction taps per frequency bin and delay, in frames. 0 = auto: taps
  // span ~1s and the delay is ~30ms (at least 3 frames) (so the direct sound and early
  // reflections are never predicted away, only the late tail).
  int taps = 0;
  int delay = 0;
  int iterations = 2;
  // Length of the prediction filter in seconds when `taps` is 0. Cost grows with its square.
  double tapSeconds = 0.8;
  // 1 = subtract the full predicted late reverberation, 0 = nothing.
  double strength = 1.0;
  // Never remove more than this much of a bin's magnitude (avoids the
  // "underwater" over-subtraction artefacts).
  double floorDb = -15;
  // The filter is re-estimated per block so a changing room/mic position is
  // tracked; blocks are crossfaded.
  double blockSeconds = 20, overlapSeconds = 2;
  unsigned threads = 0;  // 0 = hardware concurrency
};

// Single-channel Weighted Prediction Error dereverberation (Nakatani/
// Yoshioka 2010; iterative STFT-domain formulation as in NARA-WPE, MIT).
// Predicts each frequency bin's late reverberation from its own past and
// subtracts it. Linear and deterministic: no model weights, nothing is
// invented. One channel, in place.
void dereverbWpe(float* samples, size_t length, double sampleRate, const WpeOptions& options = {});

// Convenience: intensity presets (less 0.5, normal 0.8, more 1.0 strength). At
// sample rates above 16 kHz it processes a 16 kHz band-limited copy (taps spanning
// ~1 s are too costly at 48 kHz) and leaves content above ~7.7 kHz untouched.
void dereverb(float* samples, size_t length, double sampleRate, Intensity intensity = Intensity::Normal);

}  // namespace sermon
