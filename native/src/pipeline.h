#pragma once

#include <functional>
#include <optional>

#include "audio_io.h"
#include "compressor.h"
#include "noise_reduction.h"
#include "voice_eq.h"

namespace sermon {

// Which stages run and how strongly. Defaults match the browser app's
// defaults (everything on, "normal", Tone 1).
struct ChainSettings {
  bool normalize = true;
  std::optional<Intensity> noiseReduction = Intensity::Normal;
  NoiseBackend noiseBackend = NoiseBackend::Auto;  // DeepFilterNet3 if built, else RNNoise
  bool declip = true;  // SPADE (auto-detects; does nothing if no clipping)
  bool subsonicFilter = true;  // 40 Hz 4th-order highpass (rumble)
  std::optional<Tone> tone = Tone::Reference;
  std::optional<Intensity> deEsser = Intensity::Normal;
  bool voiceLeveling = true;
  std::optional<Intensity> dereverb = Intensity::Normal;  // WPE
  // Research is split on which comes first; the default is denoise, then dereverb.
  bool dereverbBeforeDenoise = false;
  std::optional<Intensity> compressor = Intensity::Normal;
  bool limiter = true;  // -14 LUFS / -1 dBTP
  double targetLufs = -14;
  double ceilingDb = -1;  // true-peak ceiling; leaves headroom for MP3 encoder overshoot
  double limiterMaxExtraDb = 4.0;  // most the limiter may push beyond plain gain to reach the target
};

struct ChainReport {
  double limiterFinalLufs = 0;
  double limiterMedianReductionDb = 0, limiterP90ReductionDb = 0, limiterP99ReductionDb = 0, limiterMaxReductionDb = 0;
};

// label, 1-based step, total steps (the stages actually enabled, plus resampling when needed).
using ProgressFn = std::function<void(const char* label, int step, int totalSteps)>;

// Runs the chain: normalize -> declip (first, while the flat tops are
// intact) -> subsonic highpass -> leveling (neural stages see a consistent
// level) -> hum notches -> noise reduction -> dereverb -> EQ -> de-ess ->
// compression -> always-on steep 11kHz lowpass -> limiter (loudness + true
// peak). See docs/dereverb-and-chain-order.md for the reasoning.
// Resamples to 48 kHz first (what RNNoise needs and a good internal rate);
// the result is at 48 kHz.
ChainReport runChain(AudioData& audio, const ChainSettings& settings, const ProgressFn& progress = nullptr);

}  // namespace sermon
