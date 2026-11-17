#pragma once

#include <string>
#include <vector>

#include "audio_io.h"
#include "pipeline.h"

namespace sermon {

// What the recording looks like. All levels in dB relative to full scale unless noted.
struct Analysis {
  double seconds = 0;
  int sampleRate = 0;
  size_t channels = 0;
  double lufs = 0, loudnessRangeLu = 0, truePeakDb = 0;
  double speechLevelDb = 0;       // RMS of the louder frames
  double noiseFloorDb = 0;        // RMS of the quietest frames (room tone / hiss)
  double snrDb = 0;               // speech level - noise floor
  double noiseHfShareDb = 0;      // noise energy above 4 kHz relative to 100 Hz-4 kHz (higher = hissier)
  double humDb = 0;               // strongest mains-hum line above its neighbours in the noise floor
  double humHz = 0;
  double clippedFraction = 0;     // share of samples sitting at a detected clip level
  double levelDriftDb = 0;        // spread of the slow (20 s) speech level across the file
  double reverbTailDb = 0;        // energy 160-400 ms after speech stops, relative to the speech (NaN if too few events)
  int reverbEvents = 0;
  double sibilanceDb = 0;         // 4-9 kHz energy relative to 0.5-4 kHz in speech (higher = sharper "s")
  double channelCorrelation = 1;  // stereo only
};

struct Suggestion {
  std::string stage;       // "noise", "dereverb", "deess", "compressor", "declip", "leveling", "export"
  std::string setting;     // human-readable value, e.g. "normal", "off", "mono"
  std::string reason;      // why, in plain language, with the measured value
  std::string confidence;  // "high" | "medium" | "low": how sure the rule is
  double amount = -1;      // the suggested slider position, 0-100, for stages that have one (else -1)
};

// Measures the recording (one pass over the file; seconds, not minutes).
Analysis analyzeAudio(const AudioData& audio);
// Same, for samples [start, end) only, without copying them.
Analysis analyzeAudio(const AudioData& audio, size_t start, size_t end);

// Recordings under 10 s are not analysed (one "auto" note, settings untouched).
// Turns measurements into chain settings plus an explanation for each choice. Writes the chosen
// settings into `settings` (leaving anything it has no opinion on at its current value).
std::vector<Suggestion> suggestSettings(const Analysis& analysis, ChainSettings& settings);

}  // namespace sermon
