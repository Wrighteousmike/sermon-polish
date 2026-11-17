#pragma once

#include <cstddef>

#include "compressor.h"

namespace sermon {

// Level-independent de-esser. Every 2.5 ms slice of speech is scored by how sibilant it is
// relative to the voice body (energy above 4 kHz vs 300 Hz-3 kHz). What counts as "normal" is
// learned from the file itself (a percentile of that score over the active speech), and only
// slices that stick out above it are turned down: only the band above 4 kHz is reduced (through a
// Linkwitz-Riley crossover, so the voice body is untouched), with 5 ms of lookahead, a fast attack and a short release, and never in
// pauses/hiss (slices far below the speech level are skipped).
//   less:   triggers above the 95th percentile, ratio 2, at most 6 dB
//   normal: 93rd percentile, ratio 3, at most 7 dB
//   more:   90th percentile, ratio 4, at most 9 dB
// Calibrated on a real sermon recording (4-9 kHz vs voice-body measure): about -2 / -3 / -6 dB,
// with nothing below 3 kHz changing; 5-10 kHz comes down roughly 2 / 3-4 / 5-7 dB on average.
// One channel, in place.
void deEss(float* samples, size_t length, double sampleRate, Intensity intensity = Intensity::Normal);

}  // namespace sermon
