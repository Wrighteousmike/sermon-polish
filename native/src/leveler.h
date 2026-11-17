#pragma once

#include <cstddef>
#include <vector>
#include <limits>

namespace sermon {

struct LevelerOptions {
  // Loudness (LUFS, K-weighted) the speech is leveled toward. NaN (default)
  // = the file's own typical speech level, so the leveler only evens things
  // out and leaves overall loudness to the limiter stage.
  double targetLufs = std::numeric_limits<double>::quiet_NaN();
  double maxBoostDb = 18;
  double maxCutDb = 18;
};

// The leveler's plan for a whole recording: a gain (dB) at the centre of every 400 ms block, derived
// from 100 ms hop loudness energies. Small (a few KB per hour), so it can be computed in one
// streaming pass up front and then applied live, sample by sample, at any playback position.
struct LevelerLane {
  size_t hop = 0;          // samples per 100 ms hop
  size_t firstCentre = 0;  // sample index of the first block's centre
  std::vector<double> gainDb;
  bool empty() const { return gainDb.empty(); }
};

// Plans the lane from K-weighted hop energies (see kWeightedHopEnergy). Empty lane = leave alone.
LevelerLane planLevelerLane(const std::vector<double>& hopEnergy, size_t hopSamples, const LevelerOptions& options = {});

// Linear gain at sample `i`: linear interpolation between block centres (held flat outside), with
// the lane's dB values scaled by `amount` (1 = as planned, 0 = off).
double levelerGainAt(const LevelerLane& lane, size_t i, double amount = 1.0);

// Offline, whole-file voice leveler. Replaces the real-time voice_leveling:
//  1. measures K-weighted momentary loudness (400ms blocks, 100ms hop),
//  2. estimates the *local speech level* at each point as the median of
//     nearby blocks that are actually speech (pauses/breaths/room tone are
//     excluded by a local gate, so they never get boosted or drag the
//     estimate down),
//  3. plans a single smooth gain curve (lookahead — a fader push is
//     corrected at the moment it happens, with no detection lag),
//  4. applies the same gain to every channel (stereo image is preserved).
// Phrase-to-phrase dynamics are deliberately left for the compressor.
// Processes in place. Files shorter than 400ms are left untouched.
void levelVoice(float* const* channels, size_t numChannels, size_t length, double sampleRate,
                const LevelerOptions& options = {});

}  // namespace sermon
