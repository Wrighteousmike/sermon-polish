#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "../compressor.h"
#include "../noise_reduction.h"
#include "../voice_eq.h"
#include "deesser_rt.h"
#include "rt.h"
#include "source.h"
#include "stages_basic.h"
#include "limiter_rt.h"
#include "noise_rt.h"

namespace sermon::rt {

// Everything learned from the whole recording up front, so the live chain can apply file-aware
// processing (leveling lane, per-speaker sibilance threshold, loudness-derived thresholds) as it plays.
struct FileModel {
  double sampleRate = 48000;
  int channels = 1;
  int64_t length = 0;
  double rawLufs = -70;                       // the unprocessed file's integrated loudness
  std::shared_ptr<const LevelerLane> lane;    // leveler gain plan
  std::shared_ptr<const DeEssStats> deess;    // sibilance statistics (of the leveled, EQ'd audio)
  double compressorRefLufs = -20;             // loudness at the compressor's input
};

// Runs two streaming passes over the source and builds the model for the given tone.
std::shared_ptr<FileModel> buildFileModel(SampleSource& source, Tone tone, const std::function<void(double)>& progress = {});

struct ChainParams {
  bool rumble = true;
  bool leveling = true;
  double levelerAmount = 1.0;
  std::optional<Intensity> noise = Intensity::Normal;
  NoiseBackend noiseBackend = NoiseBackend::Auto;
  std::optional<Tone> tone = Tone::Reference;
  double toneScale = 1.0;                     // Studio-match gain multiplier (amount::toneScale)
  std::optional<Intensity> deess = Intensity::Normal;
  std::optional<Intensity> compressor = Intensity::Normal;
  bool lowpass = true;
  bool limiter = true;
  double outputGainDb = 0;  // gain into the limiter (the loudness solve sets this)
  double ceilingDb = -1;
};

struct Meters {
  double levelerGainDb = 0, compressorReductionDb = 0, deessReductionDb = 0, limiterReductionDb = 0;
};

// The processing chain, in order: rumble filter, leveler, hum notches, noise reduction, EQ, de-esser,
// compressor, lowpass, limiter. Every stage always runs (so state stays warm) and switching one off
// crossfades to a delay-matched copy of its input, so toggling is click-free and never shifts timing.
class Chain {
 public:
  void prepare(double sampleRate, int channels);
  void setModel(std::shared_ptr<const FileModel> model);
  // Takes effect at the start of the next process() call (safe to call from another thread).
  void setParams(const ChainParams& params);
  // Full reset (deterministic, for rendering) or soft reset (for seeking during playback).
  void reset(bool soft = false);
  int latency() const;
  void process(float* const* ch, int n, int64_t position);
  Meters meters() const;

 private:
  struct Slot {
    std::unique_ptr<Processor> proc;
    DelayLine dry;
    bool enabled = true;
    float mix = 1.0f;
    std::vector<std::vector<float>> dryBuf;
  };
  void applyPending();
  void runSlot(Slot& s, float* const* ch, int n, int64_t position);
  enum { kRumble, kLeveler, kHum, kNoise, kTone, kDeess, kComp, kLowpass, kLimiter, kSlots };

  double fs_ = 48000;
  int channels_ = 1;
  std::vector<Slot> slots_;
  std::shared_ptr<const FileModel> model_;
  ChainParams params_;
  std::mutex mutex_;
  bool dirty_ = true;
};

// Streams `source` through the chain in blocks of `block` samples and hands the latency-compensated
// output (exactly source.length() samples) to `write`. The result doesn't depend on the block size.
void renderChain(Chain& chain, SampleSource& source, int block, const std::function<void(const float* const*, int)>& write,
                 const std::function<void(double)>& progress = {});

}  // namespace sermon::rt
