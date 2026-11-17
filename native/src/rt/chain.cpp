#include "chain.h"

#include "../amount.h"

#include <algorithm>
#include <cmath>

#include "../loudness.h"

namespace sermon::rt {

// ---------------- file model ----------------

std::shared_ptr<FileModel> buildFileModel(SampleSource& src, Tone tone, const std::function<void(double)>& progress) {
  constexpr int kBlock = 16384;
  const int nCh = src.channels();
  const double fs = src.sampleRate();
  auto model = std::make_shared<FileModel>();
  model->sampleRate = fs;
  model->channels = nCh;
  model->length = src.length();

  std::vector<std::vector<float>> buf;
  buf.assign(size_t(nCh), std::vector<float>());
  for (auto& b : buf) b.resize(kBlock);
  std::vector<float*> ptr;
  for (auto& b : buf) ptr.push_back(b.data());
  const size_t hop = std::max<size_t>(1, size_t(std::llround(0.1 * fs)));

  // Pass 1: loudness of the raw file and the leveler's gain plan.
  {
    HopEnergyMeter meter(fs, size_t(nCh), hop);
    for (int64_t pos = 0; pos < model->length;) {
      const int got = src.read(ptr.data(), kBlock, pos);
      if (got <= 0) break;
      meter.feed(ptr.data(), size_t(got));
      pos += got;
      if (progress) progress(0.5 * double(pos) / double(std::max<int64_t>(1, model->length)));
    }
    model->rawLufs = integratedLufsFromHops(meter.energies());
    model->lane = std::make_shared<LevelerLane>(planLevelerLane(meter.energies(), hop));
  }

  // Pass 2: the leveled, EQ'd audio gives the compressor's reference loudness and the de-esser's
  // sibilance statistics (noise reduction is left out: it doesn't change either meaningfully).
  {
    LevelerStage leveler;
    leveler.prepare(fs, nCh);
    leveler.setLane(model->lane);
    ToneEq eq;
    eq.prepare(fs, nCh);
    eq.setTone(tone);
    eq.reset();  // no crossfade needed for analysis
    HopEnergyMeter meter(fs, size_t(nCh), hop);
    DeEssStatsBuilder stats(fs);
    std::vector<float> mono(kBlock);
    for (int64_t pos = 0; pos < model->length;) {
      const int got = src.read(ptr.data(), kBlock, pos);
      if (got <= 0) break;
      leveler.process(ptr.data(), got, pos);
      eq.process(ptr.data(), got, pos);
      meter.feed(ptr.data(), size_t(got));
      for (int i = 0; i < got; i++) {
        float m = 0;
        for (int c = 0; c < nCh; c++) m += ptr[size_t(c)][i];
        mono[size_t(i)] = m / float(nCh);
      }
      stats.feed(mono.data(), size_t(got));
      pos += got;
      if (progress) progress(0.5 + 0.5 * double(pos) / double(std::max<int64_t>(1, model->length)));
    }
    model->compressorRefLufs = integratedLufsFromHops(meter.energies());
    model->deess = stats.finish();
  }
  return model;
}

// ---------------- chain ----------------

void Chain::prepare(double fs, int channels) {
  fs_ = fs;
  channels_ = channels;
  slots_.clear();
  slots_.resize(kSlots);
  slots_[kRumble].proc = std::make_unique<RumbleFilter>();
  slots_[kLeveler].proc = std::make_unique<LevelerStage>();
  slots_[kHum].proc = std::make_unique<HumNotch>();
  slots_[kNoise].proc = std::make_unique<NoiseStage>();
  slots_[kTone].proc = std::make_unique<ToneEq>();
  slots_[kDeess].proc = std::make_unique<DeEsserStage>();
  slots_[kComp].proc = std::make_unique<CompressorStage>();
  slots_[kLowpass].proc = std::make_unique<FinalLowpassStage>();
  slots_[kLimiter].proc = std::make_unique<LimiterStage>();
  for (auto& s : slots_) {
    s.proc->prepare(fs, channels);
    s.dry.prepare(channels, s.proc->latency());
    s.enabled = true;
    s.mix = 1.0f;
  }
  dirty_ = true;
}

void Chain::setModel(std::shared_ptr<const FileModel> model) {
  std::lock_guard<std::mutex> lock(mutex_);
  model_ = std::move(model);
  dirty_ = true;
}

void Chain::setParams(const ChainParams& params) {
  std::lock_guard<std::mutex> lock(mutex_);
  params_ = params;
  dirty_ = true;
}

void Chain::applyPending() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!dirty_) return;
  dirty_ = false;
  const ChainParams& p = params_;
  slots_[kRumble].enabled = p.rumble;

  auto* leveler = static_cast<LevelerStage*>(slots_[kLeveler].proc.get());
  leveler->setLane(model_ ? model_->lane : nullptr);
  leveler->setAmount(p.levelerAmount);
  slots_[kLeveler].enabled = p.leveling && model_ && model_->lane && !model_->lane->empty();

  const bool noiseOn = p.noise.has_value();
  auto* noise = static_cast<NoiseStage*>(slots_[kNoise].proc.get());
  const int before = noise->latency();
  noise->setBackend(p.noiseBackend);
  noise->setIntensity(p.noise.value_or(Intensity::Normal));
  if (noise->latency() != before) slots_[kNoise].dry.prepare(channels_, noise->latency());
  slots_[kNoise].enabled = noiseOn && noise->available();
  slots_[kHum].enabled = noiseOn;
  static_cast<HumNotch*>(slots_[kHum].proc.get())->setQ(amount::humNotchQ(p.noise.value_or(Intensity::Normal)));

  auto* tone = static_cast<ToneEq*>(slots_[kTone].proc.get());
  tone->setTone(p.tone.value_or(Tone::Tone1), p.toneScale);
  slots_[kTone].enabled = p.tone.has_value();

  auto* deess = static_cast<DeEsserStage*>(slots_[kDeess].proc.get());
  deess->setStats(model_ ? model_->deess : nullptr);
  deess->setIntensity(p.deess.value_or(Intensity::Normal));
  slots_[kDeess].enabled = p.deess.has_value();

  auto* comp = static_cast<CompressorStage*>(slots_[kComp].proc.get());
  const double ref = model_ ? model_->compressorRefLufs : -20.0;
  const Intensity ci = p.compressor.value_or(Intensity::Normal);
  const amount::CompressorParams cp = amount::compressor(ci);
  comp->setParams(std::min(-10.0, std::max(-40.0, ref - cp.thresholdOffsetDb)), cp.ratio, cp.makeupDb);
  slots_[kComp].enabled = p.compressor.has_value();

  slots_[kLowpass].enabled = p.lowpass;
  static_cast<LimiterStage*>(slots_[kLimiter].proc.get())->setParams(p.outputGainDb, p.ceilingDb);
  slots_[kLimiter].enabled = p.limiter;
}

void Chain::reset(bool soft) {
  applyPending();  // so the on/off flags are current before the crossfade state is set from them
  for (auto& s : slots_) {
    if (soft) s.proc->softReset(); else s.proc->reset();
    s.dry.reset();
    s.mix = s.enabled ? 1.0f : 0.0f;
  }
}

int Chain::latency() const {
  int total = 0;
  for (const auto& s : slots_) total += s.proc->latency();
  return total;
}

void Chain::runSlot(Slot& s, float* const* ch, int n, int64_t position) {
  const float target = s.enabled ? 1.0f : 0.0f;
  if (s.mix == 1.0f && target == 1.0f) {
    s.proc->process(ch, n, position);
    return;
  }
  // Switching on/off (or off): keep the stage running so its state stays warm, but output a mix of
  // its result and a delay-matched copy of its input.
  s.dryBuf.resize(size_t(channels_));
  std::vector<float*> dryPtr;
  dryPtr.resize(size_t(channels_));
  for (int c = 0; c < channels_; c++) {
    s.dryBuf[size_t(c)].assign(ch[c], ch[c] + n);
    dryPtr[size_t(c)] = s.dryBuf[size_t(c)].data();
  }
  s.proc->process(ch, n, position);
  s.dry.process(dryPtr.data(), n);
  constexpr float kStep = 1.0f / 256.0f;
  for (int i = 0; i < n; i++) {
    s.mix += std::max(-kStep, std::min(kStep, target - s.mix));
    for (int c = 0; c < channels_; c++) ch[c][i] = dryPtr[size_t(c)][i] + (ch[c][i] - dryPtr[size_t(c)][i]) * s.mix;
  }
}

void Chain::process(float* const* ch, int n, int64_t position) {
  applyPending();
  for (auto& s : slots_) runSlot(s, ch, n, position);
}

Meters Chain::meters() const {
  Meters m;
  m.levelerGainDb = static_cast<LevelerStage*>(slots_[kLeveler].proc.get())->currentGainDb();
  m.compressorReductionDb = static_cast<CompressorStage*>(slots_[kComp].proc.get())->gainReductionDb();
  m.deessReductionDb = static_cast<DeEsserStage*>(slots_[kDeess].proc.get())->gainReductionDb();
  m.limiterReductionDb = static_cast<LimiterStage*>(slots_[kLimiter].proc.get())->gainReductionDb();
  return m;
}

// ---------------- offline render ----------------

void renderChain(Chain& chain, SampleSource& source, int block, const std::function<void(const float* const*, int)>& write,
                 const std::function<void(double)>& progress) {
  const int nCh = source.channels();
  const int64_t length = source.length(), latency = chain.latency();
  std::vector<std::vector<float>> buf;
  buf.assign(size_t(nCh), std::vector<float>());
  for (auto& b : buf) b.resize(size_t(block));
  std::vector<float*> ptr;
  for (auto& b : buf) ptr.push_back(b.data());
  int64_t written = 0;
  for (int64_t pos = 0; written < length; pos += block) {
    int got = pos < length ? source.read(ptr.data(), block, pos) : 0;
    for (int c = 0; c < nCh; c++) std::fill(buf[size_t(c)].begin() + got, buf[size_t(c)].end(), 0.0f);  // flush the latency tail with silence
    chain.process(ptr.data(), block, pos);
    // This block's output covers input positions [pos - latency, pos - latency + block).
    const int64_t outStart = pos - latency;
    int64_t from = std::max<int64_t>(0, -outStart);  // skip the priming samples
    int64_t to = std::min<int64_t>(block, length - outStart);
    if (to > from) {
      std::vector<const float*> cp;
      for (int c = 0; c < nCh; c++) cp.push_back(ptr[size_t(c)] + from);
      write(cp.data(), int(to - from));
      written += to - from;
    }
    if (progress) progress(double(std::min(written, length)) / double(std::max<int64_t>(1, length)));
  }
}

}  // namespace sermon::rt
