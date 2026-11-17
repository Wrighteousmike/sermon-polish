#include "limiter_rt.h"

#include <algorithm>
#include <cmath>

#include "../loudness.h"

namespace sermon::rt {

void StreamSmoother::prepare(double fs) {
  look_ = std::max<size_t>(1, size_t(std::llround(0.005 * fs)));
  release_ = std::exp(-1.0 / (0.1 * fs));
  reset();
}

void StreamSmoother::reset() {
  window_.clear();
  fmRing_.assign(look_, 1.0f);
  count_ = 0;
  run_ = 0;
  cur_ = 1;
}

bool StreamSmoother::push(float need, double& gain) {
  const size_t k = count_++;
  while (!window_.empty() && window_.back().second >= need) window_.pop_back();
  window_.emplace_back(k, need);
  if (k + 1 < look_) return false;
  const size_t i = k + 1 - look_;  // the sample whose window [i, k] is now complete
  while (window_.front().first < i) window_.pop_front();
  const float fm = window_.front().second;
  run_ += fm;
  if (i >= look_) run_ -= fmRing_[i % look_];
  fmRing_[i % look_] = fm;
  const double target = run_ / double(std::min(i + 1, look_));
  cur_ = target < cur_ ? target : target + (cur_ - target) * release_;
  gain = cur_;
  return true;
}

void LimiterStage::prepare(double fs, int channels) {
  fs_ = fs;
  channels_ = channels;
  smoother_.prepare(fs);
  ring_ = 64;
  while (ring_ < smoother_.lookahead() + 32) ring_ <<= 1;
  gainCoeff_ = std::exp(-1.0 / (0.02 * fs));
  reset();
}

void LimiterStage::reset() {
  buf_.assign(size_t(channels_), std::vector<float>(ring_, 0.0f));
  smoother_.reset();
  t_ = 0;
  gainCur_ = gainTarget_;
  reductionDb_ = 0;
}

void LimiterStage::setParams(double inputGainDb, double ceilingDb) {
  gainTarget_ = std::pow(10.0, inputGainDb / 20.0);
  ceiling_ = std::pow(10.0, ceilingDb / 20.0);
}

void LimiterStage::process(float* const* ch, int n, int64_t) {
  const size_t look = smoother_.lookahead();
  const int64_t outLag = int64_t(look) - 1 + 8;  // look-ahead of the smoother + the 8 future samples a true peak needs
  float w[16];
  double minGain = 1.0;
  for (int i = 0; i < n; i++) {
    gainCur_ = gainTarget_ + (gainCur_ - gainTarget_) * gainCoeff_;
    const size_t slot = size_t(t_) & (ring_ - 1);
    for (int c = 0; c < channels_; c++) buf_[size_t(c)][slot] = float(double(ch[c][i]) * gainCur_);
    const int64_t k = t_ - 8;  // sample whose true peak is now computable
    if (k >= 0) {
      double tp = 0;
      for (int c = 0; c < channels_; c++) {
        for (int j = 0; j < 16; j++) {
          const int64_t idx = k - 7 + j;
          w[j] = idx < 0 ? 0.0f : buf_[size_t(c)][size_t(idx) & (ring_ - 1)];
        }
        tp = std::max(tp, truePeakWindow(w));
      }
      const float need = tp > ceiling_ ? float(ceiling_ / tp) : 1.0f;
      double g;
      if (smoother_.push(need, g)) {
        const int64_t outIdx = t_ - outLag;  // = the sample this gain belongs to
        for (int c = 0; c < channels_; c++) ch[c][i] = float(double(buf_[size_t(c)][size_t(outIdx) & (ring_ - 1)]) * g);
        minGain = std::min(minGain, g);
        t_++;
        continue;
      }
    }
    for (int c = 0; c < channels_; c++) ch[c][i] = 0.0f;  // still filling the look-ahead
    t_++;
  }
  reductionDb_ = -20.0 * std::log10(std::max(minGain, 1e-6));
}

}  // namespace sermon::rt
