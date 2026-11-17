#include "noise_rt.h"

#include "../amount.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

#ifdef SERMON_HAVE_RNNOISE
#include "rnnoise.h"
#endif
#ifdef SERMON_HAVE_DEEPFILTERNET
#include "../df_capi.h"
#endif

namespace sermon::rt {

namespace {
constexpr int kFrame = 480;
constexpr int kDfLatency = 1440;   // measured: DeepFilterNet's output lags its input by 3 frames
constexpr int kRnnLatency = 959;   // measured for RNNoise
}  // namespace

struct NoiseStage::Chan {
#ifdef SERMON_HAVE_DEEPFILTERNET
  DFState* df = nullptr;
#endif
#ifdef SERMON_HAVE_RNNOISE
  DenoiseState* rnn = nullptr;
#endif
  std::deque<float> in, out;
};

NoiseStage::~NoiseStage() { destroy(); }

void NoiseStage::destroy() {
  for (Chan* c : chans_) {
#ifdef SERMON_HAVE_DEEPFILTERNET
    if (c->df) df_free(c->df);
#endif
#ifdef SERMON_HAVE_RNNOISE
    if (c->rnn) rnnoise_destroy(c->rnn);
#endif
    delete c;
  }
  chans_.clear();
}

void NoiseStage::build() {
  destroy();
  const NoiseBackend b = backend_ == NoiseBackend::Auto ? (noiseBackendAvailable(NoiseBackend::DeepFilterNet) ? NoiseBackend::DeepFilterNet : NoiseBackend::RNNoise) : backend_;
  useDeepFilter_ = b == NoiseBackend::DeepFilterNet;
  usable_ = noiseBackendAvailable(b) && fs_ == 48000.0;
  if (useDeepFilter_) {
    bool found = false;
    noiseModelPath(&found);
    usable_ = usable_ && found;
  }
  latency_ = kFrame + (useDeepFilter_ ? kDfLatency : kRnnLatency);
  for (int c = 0; c < channels_; c++) {
    Chan* ch = new Chan();
#ifdef SERMON_HAVE_DEEPFILTERNET
    if (usable_ && useDeepFilter_) ch->df = df_create(noiseModelPath().c_str(), 20.0f, nullptr);
#endif
#ifdef SERMON_HAVE_RNNOISE
    if (usable_ && !useDeepFilter_) ch->rnn = rnnoise_create(nullptr);
#endif
    ch->out.assign(size_t(kFrame), 0.0f);  // headroom so a block can always be served
    chans_.push_back(ch);
  }
  dry_.prepare(channels_, latency_);
  setIntensity(intensity_);
}

void NoiseStage::prepare(double fs, int channels) {
  fs_ = fs;
  channels_ = channels;
  build();
}

void NoiseStage::reset() { build(); }

void NoiseStage::softReset() {
  // The network keeps its recurrent state across a seek (there is no reset call); the caller pre-rolls
  // a little audio through to settle it. The FIFOs and dry path restart from silence.
  for (Chan* c : chans_) {
    c->in.clear();
    c->out.assign(size_t(kFrame), 0.0f);
  }
  dry_.reset();
}

void NoiseStage::setBackend(NoiseBackend backend) {
  if (backend == backend_ && !chans_.empty()) return;
  backend_ = backend;
  build();
}

void NoiseStage::setIntensity(Intensity intensity) {
  intensity_ = intensity;
#ifdef SERMON_HAVE_DEEPFILTERNET
  const float atten = float(amount::noiseAttenDb(intensity));
  for (Chan* c : chans_)
    if (c->df) df_set_atten_lim(c->df, atten);
#endif
}

void NoiseStage::process(float* const* ch, int n, int64_t) {
  const double wet = amount::noiseWet(intensity_);
  const bool blend = !useDeepFilter_ && wet < 1.0;
  std::vector<float> dryCopy[8];
  std::vector<float*> dryPtr;
  dryPtr.resize(size_t(channels_));
  if (blend) {
    for (int c = 0; c < channels_; c++) {
      dryCopy[c].assign(ch[c], ch[c] + n);
      dryPtr[size_t(c)] = dryCopy[c].data();
    }
    dry_.process(dryPtr.data(), n);
  }
  float frameIn[kFrame], frameOut[kFrame];
  for (int c = 0; c < channels_; c++) {
    Chan& s = *chans_[size_t(c)];
    for (int i = 0; i < n; i++) s.in.push_back(ch[c][i]);
    while (int(s.in.size()) >= kFrame) {
      for (int i = 0; i < kFrame; i++) { frameIn[i] = s.in.front(); s.in.pop_front(); }
#if defined(SERMON_HAVE_DEEPFILTERNET)
      if (s.df) {
        df_process_frame(s.df, frameIn, frameOut);
        for (int i = 0; i < kFrame; i++) s.out.push_back(frameOut[i]);
        continue;
      }
#endif
#if defined(SERMON_HAVE_RNNOISE)
      if (s.rnn) {
        for (int i = 0; i < kFrame; i++) frameIn[i] *= 32768.0f;
        rnnoise_process_frame(s.rnn, frameOut, frameIn);
        for (int i = 0; i < kFrame; i++) s.out.push_back(frameOut[i] / 32768.0f);
        continue;
      }
#endif
      for (int i = 0; i < kFrame; i++) s.out.push_back(frameIn[i]);  // no backend: pass through (delayed)
    }
    for (int i = 0; i < n; i++) {
      const float w = s.out.front();
      s.out.pop_front();
      ch[c][i] = blend ? float(wet * double(w) + (1.0 - wet) * double(dryCopy[c][size_t(i)])) : w;
    }
  }
}

}  // namespace sermon::rt
