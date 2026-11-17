#include "stages_basic.h"

#include <algorithm>
#include <cmath>

namespace sermon::rt {

namespace {
constexpr double kPi = 3.14159265358979323846;
void zeroState(Biquad& f) { f.z1 = f.z2 = 0; }
void setCoeffs(Biquad& dst, const Biquad& src) {
  dst.b0 = src.b0; dst.b1 = src.b1; dst.b2 = src.b2; dst.a1 = src.a1; dst.a2 = src.a2;
}
}  // namespace

// ---- RumbleFilter ----
void RumbleFilter::prepare(double fs, int channels) {
  a_.assign(size_t(channels), Biquad::highpass(fs, 40, 0.5412));
  b_.assign(size_t(channels), Biquad::highpass(fs, 40, 1.3066));
}
void RumbleFilter::reset() {
  for (auto& f : a_) zeroState(f);
  for (auto& f : b_) zeroState(f);
}
void RumbleFilter::process(float* const* ch, int n, int64_t) {
  for (size_t c = 0; c < a_.size(); c++)
    for (int i = 0; i < n; i++) ch[c][i] = float(b_[c].process(a_[c].process(ch[c][i])));
}

// ---- HumNotch ----
void HumNotch::prepare(double fs, int channels) {
  fs_ = fs;
  notches_.assign(size_t(channels), {});
  for (auto& v : notches_)
    for (double hz : {50.0, 100.0, 150.0}) v.push_back(Biquad::notch(fs, hz, q_));
}
void HumNotch::setQ(double q) {
  q_ = q;
  for (auto& v : notches_) {
    size_t k = 0;
    for (double hz : {50.0, 100.0, 150.0}) setCoeffs(v[k++], Biquad::notch(fs_, hz, q_));
  }
}
void HumNotch::reset() {
  for (auto& v : notches_) for (auto& f : v) zeroState(f);
}
void HumNotch::process(float* const* ch, int n, int64_t) {
  for (size_t c = 0; c < notches_.size(); c++)
    for (int i = 0; i < n; i++) {
      double x = ch[c][i];
      for (auto& f : notches_[c]) x = f.process(x);
      ch[c][i] = float(x);
    }
}

// ---- ToneEq ----
ToneEq::Chain ToneEq::make(Tone tone, double gainScale) const {
  Chain c;
  c.resize(size_t(channels_));
  for (auto& v : c) v = voiceEqChain(fs_, tone, gainScale);
  return c;
}
void ToneEq::prepare(double fs, int channels) {
  fs_ = fs;
  channels_ = channels;
  cur_ = make(tone_, scale_);
  old_.clear();
  fadeLeft_ = 0;
}
void ToneEq::reset() {
  for (auto& v : cur_) for (auto& f : v) zeroState(f);
  old_.clear();
  fadeLeft_ = 0;
}
void ToneEq::setTone(Tone tone, double gainScale) {
  if (tone == tone_ && !cur_.empty()) {
    if (gainScale == scale_ || tone != Tone::Reference) return;
    scale_ = gainScale;  // same filters, new gains: swap coefficients, keep each filter's state
    Chain next = make(tone, scale_);
    for (size_t c = 0; c < next.size() && c < cur_.size(); c++)
      for (size_t i = 0; i < next[c].size() && i < cur_[c].size(); i++) {
        next[c][i].z1 = cur_[c][i].z1;
        next[c][i].z2 = cur_[c][i].z2;
      }
    cur_ = std::move(next);
    return;
  }
  tone_ = tone;
  scale_ = gainScale;
  old_ = std::move(cur_);
  cur_ = make(tone, scale_);
  fadeLeft_ = 1024;  // ~21 ms crossfade between the old and new curve
}
void ToneEq::process(float* const* ch, int n, int64_t) {
  for (int c = 0; c < channels_; c++) {
    float* x = ch[c];
    if (fadeLeft_ > 0 && !old_.empty()) {
      scratch_.assign(x, x + n);
      for (int i = 0; i < n; i++) {
        double a = x[i], b = scratch_[size_t(i)];
        for (auto& f : cur_[size_t(c)]) a = f.process(a);
        for (auto& f : old_[size_t(c)]) b = f.process(b);
        const int left = fadeLeft_ - i;
        const double w = left > 0 ? double(left) / 1024.0 : 0.0;  // weight of the old curve
        x[i] = float(a * (1.0 - w) + b * w);
      }
    } else {
      for (int i = 0; i < n; i++) {
        double v = x[i];
        for (auto& f : cur_[size_t(c)]) v = f.process(v);
        x[i] = float(v);
      }
    }
  }
  if (fadeLeft_ > 0) {
    fadeLeft_ = std::max(0, fadeLeft_ - n);
    if (fadeLeft_ == 0) old_.clear();
  }
}

// ---- FinalLowpassStage ----
void FinalLowpassStage::prepare(double fs, int channels) {
  sections_.assign(size_t(channels), {});
  if (11000 >= 0.49 * fs) return;
  constexpr int kOrder = 12;
  for (auto& v : sections_)
    for (int k = 1; k <= kOrder / 2; k++) {
      const double theta = (2.0 * k - 1.0) * kPi / (2.0 * kOrder);
      v.push_back(Biquad::lowpass(fs, 11000, 1.0 / (2.0 * std::cos(theta))));
    }
}
void FinalLowpassStage::reset() {
  for (auto& v : sections_) for (auto& f : v) zeroState(f);
}
void FinalLowpassStage::process(float* const* ch, int n, int64_t) {
  for (size_t c = 0; c < sections_.size(); c++) {
    if (sections_[c].empty()) continue;
    // Section by section over the block, as the offline filter does (identical numerics).
    for (auto& f : sections_[c]) f.processBlock(ch[c], size_t(n));
  }
}

// ---- CompressorStage ----
void CompressorStage::prepare(double fs, int channels) {
  fs_ = fs;
  channelCount_ = channels;
  aCoeff_ = std::exp(-1.0 / (0.02 * fs));
  rCoeff_ = std::exp(-1.0 / (0.2 * fs));
  gainDb_ = 0;
}
void CompressorStage::reset() { gainDb_ = 0; }
void CompressorStage::setParams(double thresholdDb, double ratio, double makeupDb) {
  thresholdDb_ = thresholdDb;
  ratio_ = ratio;
  makeup_ = std::pow(10.0, makeupDb / 20.0);
}
void CompressorStage::process(float* const* ch, int n, int64_t) {
  constexpr double kKneeDb = 25;
  // Channel count comes from the caller's array; peak detection is linked across channels.
  for (int i = 0; i < n; i++) {
    double peak = std::fabs(double(ch[0][i]));
    for (int c = 1; c < channelCount_; c++) peak = std::max(peak, std::fabs(double(ch[c][i])));
    const double levelDb = 20.0 * std::log10(std::max(peak, 1e-8));
    const double over = levelDb - thresholdDb_;
    double outDb;
    if (2 * over < -kKneeDb) outDb = levelDb;
    else if (2 * std::fabs(over) <= kKneeDb) {
      const double t = over + kKneeDb / 2;
      outDb = levelDb + (1.0 / ratio_ - 1.0) * t * t / (2 * kKneeDb);
    } else outDb = thresholdDb_ + over / ratio_;
    const double targetDb = outDb - levelDb;
    const double coeff = targetDb < gainDb_ ? aCoeff_ : rCoeff_;
    gainDb_ = targetDb + (gainDb_ - targetDb) * coeff;
    const double g = std::pow(10.0, gainDb_ / 20.0) * makeup_;
    for (int c = 0; c < channelCount_; c++) ch[c][i] = float(ch[c][i] * g);
  }
}

// ---- LevelerStage ----
void LevelerStage::prepare(double, int channels) { channelCount_ = channels; }
void LevelerStage::process(float* const* ch, int n, int64_t position) {
  if (!lane_ || lane_->empty() || amount_ <= 0) return;
  const LevelerLane& lane = *lane_;
  const size_t blocks = lane.gainDb.size();
  const double hop = double(lane.hop);
  // Gains at the two block centres around the current segment are cached and re-derived only when
  // the segment changes (about every 100 ms), not for every sample.
  int64_t segment = -2;
  double a = 1, b = 1;
  auto lin = [&](size_t k) { return std::pow(10.0, amount_ * lane.gainDb[k] / 20.0); };
  for (int i = 0; i < n; i++) {
    const double pos = (double(position + i) - double(lane.firstCentre)) / hop;
    double g;
    if (pos <= 0) {
      if (segment != -1) { a = lin(0); segment = -1; }
      g = a;
    } else if (pos >= double(blocks - 1)) {
      if (segment != -3) { a = lin(blocks - 1); segment = -3; }
      g = a;
    } else {
      const int64_t k = int64_t(pos);
      if (segment != k) { a = lin(size_t(k)); b = lin(size_t(k + 1)); segment = k; }
      g = a + (b - a) * (pos - double(k));
    }
    for (int c = 0; c < channelCount_; c++) ch[c][i] = float(ch[c][i] * g);
    gainDb_ = 20.0 * std::log10(std::max(g, 1e-6));
  }
}

}  // namespace sermon::rt
