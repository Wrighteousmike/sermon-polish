#pragma once

#include <memory>
#include <vector>

#include "../biquad.h"
#include "../leveler.h"
#include "../voice_eq.h"
#include "rt.h"

namespace sermon::rt {

// 40 Hz 4th-order highpass.
class RumbleFilter : public Processor {
 public:
  void prepare(double fs, int channels) override;
  void reset() override;
  void process(float* const* ch, int n, int64_t position) override;

 private:
  std::vector<Biquad> a_, b_;  // per channel
};

// Mains-hum notches at 50/100/150 Hz.
class HumNotch : public Processor {
 public:
  void prepare(double fs, int channels) override;
  void reset() override;
  void setQ(double q);  // 12 normally, 8 for "more"; keeps the filter state
  void process(float* const* ch, int n, int64_t position) override;

 private:
  double fs_ = 48000;
  double q_ = 12;
  std::vector<std::vector<Biquad>> notches_;  // [channel][3]
};

// Static voice EQ; changing the tone crossfades between the old and new filters.
class ToneEq : public Processor {
 public:
  void prepare(double fs, int channels) override;
  void reset() override;
  // `gainScale` scales the Studio-match curve's bands (see voiceEqChain). A change of scale alone
  // updates the filters in place (state kept, so dragging a slider is smooth); a change of tone crossfades.
  void setTone(Tone tone, double gainScale = 1.0);
  void process(float* const* ch, int n, int64_t position) override;

 private:
  using Chain = std::vector<std::vector<Biquad>>;  // [channel][biquad]
  Chain make(Tone tone, double gainScale) const;
  double fs_ = 48000;
  int channels_ = 1;
  Tone tone_ = Tone::Tone1;
  double scale_ = 1.0;
  Chain cur_, old_;
  int fadeLeft_ = 0;
  std::vector<float> scratch_;
};

// 12th-order Butterworth lowpass at 11 kHz (encoder hiss).
class FinalLowpassStage : public Processor {
 public:
  void prepare(double fs, int channels) override;
  void reset() override;
  void process(float* const* ch, int n, int64_t position) override;

 private:
  std::vector<std::vector<Biquad>> sections_;  // [channel][6]
};

// Soft-knee vocal compressor (25 dB knee, 20 ms attack, 200 ms release) with a threshold the caller
// derives from the recording's loudness.
class CompressorStage : public Processor {
 public:
  void prepare(double fs, int channels) override;
  void reset() override;
  void setParams(double thresholdDb, double ratio, double makeupDb);
  double gainReductionDb() const { return -gainDb_; }
  void process(float* const* ch, int n, int64_t position) override;

 private:
  double fs_ = 48000, aCoeff_ = 0, rCoeff_ = 0;
  double thresholdDb_ = -20, ratio_ = 8, makeup_ = 1;
  double gainDb_ = 0;
  int channelCount_ = 1;
};

// Applies the leveler's planned gain lane at the playback position; `amount` scales the dB.
class LevelerStage : public Processor {
 public:
  void prepare(double fs, int channels) override;
  void reset() override {}
  void setLane(std::shared_ptr<const LevelerLane> lane) { lane_ = std::move(lane); }
  void setAmount(double amount) { amount_ = amount; }
  double currentGainDb() const { return gainDb_; }
  void process(float* const* ch, int n, int64_t position) override;

 private:
  std::shared_ptr<const LevelerLane> lane_;
  double amount_ = 1.0;
  double gainDb_ = 0;
  int channelCount_ = 1;
};

}  // namespace sermon::rt
