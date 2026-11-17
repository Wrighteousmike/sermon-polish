#pragma once

#include <deque>
#include <vector>

#include "../compressor.h"        // Intensity
#include "../noise_reduction.h"   // NoiseBackend
#include "rt.h"

namespace sermon::rt {

// Frame-based noise reduction (DeepFilterNet3 or RNNoise) behind a small FIFO, so it accepts blocks
// of any size. The output lags the input by latency() samples (the network's own delay plus one
// frame). Intensity: DeepFilterNet = attenuation limit (10 / 20 dB / unlimited); RNNoise = wet/dry
// blend (50 / 75 / 100%) against a delay-matched dry signal.
class NoiseStage : public Processor {
 public:
  ~NoiseStage() override;
  void prepare(double fs, int channels) override;
  void reset() override;      // rebuilds the network: ~160 ms, fully deterministic
  void softReset() override;  // clears the FIFOs only (use with a pre-roll)
  int latency() const override { return latency_; }
  void setBackend(NoiseBackend backend);
  void setIntensity(Intensity intensity);
  // False if the requested backend isn't compiled in (the stage then just delays the audio).
  bool available() const { return usable_; }
  void process(float* const* ch, int n, int64_t position) override;

 private:
  struct Chan;
  void build();
  void destroy();
  NoiseBackend backend_ = NoiseBackend::Auto;
  Intensity intensity_ = Intensity::Normal;
  bool useDeepFilter_ = false, usable_ = false;
  int channels_ = 1, latency_ = 480;
  double fs_ = 48000;
  std::vector<Chan*> chans_;
  DelayLine dry_;
};

}  // namespace sermon::rt
