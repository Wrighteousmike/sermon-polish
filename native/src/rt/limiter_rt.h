#pragma once

#include <deque>
#include <utility>
#include <vector>

#include "rt.h"

namespace sermon::rt {

// The limiter's smoothing as an incremental object: feed it the gain *requirement* for each sample in
// order; once `lookahead` of them are in, it yields the smoothed gain for the sample `lookahead-1`
// behind (a causal box average of the forward window minimum, then a 100 ms release). Identical
// arithmetic to the offline limiter (native/src/limiter.cpp).
class StreamSmoother {
 public:
  void prepare(double fs);
  void reset();
  size_t lookahead() const { return look_; }
  // Returns true and sets `gain` when the gain for sample (pushed count - lookahead) is ready.
  bool push(float need, double& gain);

 private:
  size_t look_ = 240;
  double release_ = 0;
  std::deque<std::pair<size_t, float>> window_;
  std::vector<float> fmRing_;
  size_t count_ = 0;
  double run_ = 0, cur_ = 1;
};

// True-peak look-ahead limiter: input gain (smoothed), then the gain needed to hold the 4x-oversampled
// true peak of all channels at the ceiling. 5 ms look-ahead, 100 ms release. Linked across channels.
class LimiterStage : public Processor {
 public:
  void prepare(double fs, int channels) override;
  void reset() override;
  int latency() const override { return int(smoother_.lookahead()) + 7; }
  void setParams(double inputGainDb, double ceilingDb);
  double gainReductionDb() const { return reductionDb_; }
  void process(float* const* ch, int n, int64_t position) override;

 private:
  int channels_ = 1;
  double fs_ = 48000;
  StreamSmoother smoother_;
  size_t ring_ = 1024;
  std::vector<std::vector<float>> buf_;  // gained input, per channel
  int64_t t_ = 0;                        // samples pushed so far
  double gainTarget_ = 1, gainCur_ = 1, gainCoeff_ = 0, ceiling_ = 0.891;
  double reductionDb_ = 0;
};

}  // namespace sermon::rt
