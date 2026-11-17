#pragma once

// The block-based ("real-time") engine. Every stage processes a block of any size, in place, keeps
// only a little state between blocks, and reports its latency, so the same chain can run live in
// an audio callback (large 2048-4096 sample blocks) or render a whole file in a loop. Rendering the
// same audio with any block size gives the same result.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sermon::rt {

class Processor {
 public:
  virtual ~Processor() = default;
  // Allocates and clears all state.
  virtual void prepare(double sampleRate, int channels) = 0;
  // Clears all state: afterwards the stage behaves exactly as if freshly prepared (deterministic).
  virtual void reset() = 0;
  // Cheaper reset for seeking during playback: clears the audio path (look-ahead, FIFOs) but may keep
  // slow-moving learned state (a neural network's recurrent state), which a short pre-roll re-settles.
  virtual void softReset() { reset(); }
  // Samples by which the output lags the input.
  virtual int latency() const { return 0; }
  // Processes `n` samples in place. `position` is the file position (in samples) of the first input
  // sample, for stages driven by the file model (the leveler); it is only meaningful for stages that
  // sit upstream of every stage with latency.
  virtual void process(float* const* ch, int n, int64_t position) = 0;
};

// A fixed delay (in samples) for every channel.
class DelayLine {
 public:
  void prepare(int channels, int delay) {
    delay_ = delay;
    buf_.assign(size_t(channels), std::vector<float>(size_t(delay) + 1, 0.0f));
    pos_ = 0;
  }
  void reset() {
    for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
    pos_ = 0;
  }
  int delay() const { return delay_; }
  // Delays `n` samples of every channel in place.
  void process(float* const* ch, int n) {
    if (delay_ == 0) return;
    const size_t size = size_t(delay_) + 1;
    for (size_t c = 0; c < buf_.size(); c++) {
      size_t p = pos_;
      float* x = ch[c];
      float* b = buf_[c].data();
      for (int i = 0; i < n; i++) {
        const float in = x[i];
        b[p] = in;
        p++;
        if (p == size) p = 0;
        x[i] = b[p];  // the sample written `delay` steps ago
      }
    }
    pos_ = (pos_ + size_t(n)) % size;
  }

 private:
  int delay_ = 0;
  std::vector<std::vector<float>> buf_;
  size_t pos_ = 0;
};

}  // namespace sermon::rt
