#pragma once

#include <memory>
#include <vector>

#include "../compressor.h"  // Intensity
#include "../biquad.h"
#include "rt.h"

namespace sermon::rt {

// What the de-esser learns about a recording up front: how sibilant each 2.5 ms slice of the
// active speech is relative to the voice body (energy above 4 kHz vs 300 Hz-3 kHz, in dB), as a
// sorted list, plus the loud end of the file's level so pauses and hiss can be told apart.
struct DeEssStats {
  double loudRefDb = 0;
  std::vector<float> sortedRatiosDb;
  bool valid() const { return sortedRatiosDb.size() >= 40; }
  double percentile(double p) const;
};

// Streaming de-esser: the same design as the offline deEss() (per-slice sibilance relative to the
// voice, threshold from this speaker's own distribution, Linkwitz-Riley crossover at 4 kHz so only
// the high band is turned down, 5 ms look-ahead), made incremental. Needs the recording's stats; with
// none it passes audio through.
class DeEsserStage : public Processor {
 public:
  DeEsserStage();
  ~DeEsserStage() override;
  void prepare(double fs, int channels) override;
  void reset() override;
  int latency() const override { return delaySamples_; }
  void setStats(std::shared_ptr<const DeEssStats> stats);
  void setIntensity(Intensity intensity);
  double gainReductionDb() const { return reductionDb_; }
  void process(float* const* ch, int n, int64_t position) override;

 private:
  struct Channel;
  void startFrame(Channel& c);
  double fs_ = 48000;
  int channels_ = 1;
  size_t hop_ = 120;
  int delaySamples_ = 480;
  std::shared_ptr<const DeEssStats> stats_;
  double percentile_ = 0.93, ratio_ = 3, maxReductionDb_ = 7, threshold_ = 0;
  double reductionDb_ = 0;
  std::vector<std::unique_ptr<Channel>> ch_;
};

// Computes the stats from audio the caller streams through `feed` in any block sizes.
class DeEssStatsBuilder {
 public:
  explicit DeEssStatsBuilder(double sampleRate);
  void feed(const float* x, size_t n);
  std::shared_ptr<DeEssStats> finish();

 private:
  double fs_;
  size_t hop_;
  Biquad lp1_, lp2_, hp1_, hp2_, bA1_, bA2_, bB1_, bB2_;
  size_t inHop_ = 0;
  double ss_ = 0, bb_ = 0, tt_ = 0;
  std::vector<double> hs_, hb_, ht_;
};

}  // namespace sermon::rt
