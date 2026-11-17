#pragma once

#include <cstddef>
#include <limits>

namespace sermon {

struct SpadeOptions {
  // Hop is frameLength/4 (75% overlap), Hann window. 0 = auto: ~46ms rounded
  // to a power of two (2048 at 44.1kHz, 1024 at 22.05kHz).
  size_t frameLength = 0;
  int maxIterations = 1500;
  double epsilon = 0.1;  // ADMM stop tolerance, in units where the clip level = 1
  // Frames where more than this fraction of samples are clipped are skipped
  // (too little reliable signal to reconstruct from).
  double maxClippedFraction = 0.3;
  // A frame is rejected (its clipped samples are left as they were) if the
  // reconstruction peaks more than this multiple of the clip level, or
  // fails to converge. Sparse declipping degrades badly under heavy
  // clipping and then invents huge peaks; this keeps a bad frame from
  // making things worse than doing nothing.
  double maxOvershoot = 2.0;
  // Clip levels. NaN (default) = auto-detect from the channel's own peaks.
  double thresholdPos = std::numeric_limits<double>::quiet_NaN();
  double thresholdNeg = std::numeric_limits<double>::quiet_NaN();
  unsigned threads = 0;  // 0 = hardware concurrency
};

struct SpadeStats {
  double thresholdPos = 0, thresholdNeg = 0;  // clip levels used (0 = side not clipped)
  size_t clippedSamples = 0;
  size_t framesProcessed = 0;  // accepted and used
  size_t framesSkipped = 0;    // too heavily clipped to attempt
  size_t framesRejected = 0;   // attempted, failed the sanity checks
};

struct ClipDetection {
  double thresholdPos = 0, thresholdNeg = 0;  // 0 = that side isn't clipped
  size_t clippedSamples = 0;
};

// Finds the clip levels (if any) without changing the audio. One channel.
ClipDetection detectClipping(const float* samples, size_t length);

// Sparse-model declipper (A-SPADE, Kitic/Bertin/Gribonval 2015): for every
// frame containing clipped samples, finds the signal that is sparsest in a
// Parseval-tight DFT frame while keeping the reliable samples fixed and
// the clipped ones at/above (below) the clip level. Frame estimates are
// combined by weighted overlap-add, and *only clipped samples are changed*
// — everything else stays bit-identical. Operates in place on one channel.
// Does nothing if no clipping is detected. Works best on light-to-moderate
// clipping (a few percent of samples); frames that are too heavily clipped
// to reconstruct reliably are left alone rather than guessed at.
SpadeStats declipSpade(float* samples, size_t length, double sampleRate, const SpadeOptions& options = {});

}  // namespace sermon
