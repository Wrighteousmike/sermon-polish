#include <cstdint>
#include "host.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#include "miniaudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "../loudness.h"

namespace sermon::rt {

namespace {
constexpr int kRate = 48000;
constexpr int kBlock = 1024;                      // render granularity (frames)
constexpr double kPrerollSeconds = 0.5;           // re-settles the neural network after a seek
constexpr size_t kRingFrames = 1 << 15;           // ring capacity (power of two)
constexpr size_t kAheadFrames = 14400;            // how far ahead of the device we render: 0.3 s
constexpr size_t kKeepFrames = 8192;              // on a settings change, audio already queued that keeps playing (0.17 s)
constexpr size_t kBlendFrames = 240;              // crossfade into the re-rendered audio (5 ms)
}  // namespace

struct Host::Device {
  ma_context context{};
  ma_device device{};
  bool haveContext = false, haveDevice = false;
};

Host::Host() = default;
Host::~Host() { close(); }

static void dataCallback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
  static_cast<Host*>(dev->pUserData)->deviceCallbackPublic(static_cast<float*>(out), frames);
}

bool Host::open(const std::string& workWav, std::shared_ptr<const FileModel> model, bool nullDevice, int deviceBuffer, std::string* error) {
  close();
  try {
    source_ = std::make_unique<CorrectedSource>(std::make_unique<WavFileSource>(workWav), nullptr, &revStrength_);
    rawSource_ = std::make_unique<WavFileSource>(workWav);
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
  if (source_->sampleRate() != kRate) {
    if (error) *error = "working file must be 48 kHz";
    return false;
  }
  model_ = std::move(model);
  channels_ = source_->channels();
  length_ = source_->length();
  chain_.prepare(kRate, channels_);
  chain_.setModel(model_);
  chain_.reset();
  latency_ = chain_.latency();
  trimStart_ = 0;
  trimEnd_ = length_;
  ringCap_ = kRingFrames;
  ring_.assign(ringCap_ * 2, 0.0f);
  rd_ = wr_ = 0;
  delivered_ = 0;
  seekBase_ = 0;
  underruns_ = 0;
  atEnd_ = false;
  playing_ = false;

  device_ = std::make_unique<Device>();
  ma_context_config cc = ma_context_config_init();
  ma_backend nullBackend[] = {ma_backend_null};
  ma_result r = nullDevice ? ma_context_init(nullBackend, 1, &cc, &device_->context) : ma_context_init(nullptr, 0, &cc, &device_->context);
  if (r != MA_SUCCESS) {
    if (error) *error = "could not start the audio system";
    return false;
  }
  device_->haveContext = true;
  ma_device_config dc = ma_device_config_init(ma_device_type_playback);
  dc.playback.format = ma_format_f32;
  dc.playback.channels = 2;
  dc.sampleRate = kRate;
  dc.periodSizeInFrames = ma_uint32(deviceBuffer);
  dc.periods = 3;
  deviceAhead_ = deviceBuffer * 2;
  dc.dataCallback = dataCallback;
  dc.pUserData = this;
  if (ma_device_init(&device_->context, &dc, &device_->device) != MA_SUCCESS) {
    if (error) *error = "could not open the audio output";
    return false;
  }
  device_->haveDevice = true;

  running_ = true;
  renderThread_ = std::thread([this] { renderLoop(); });
  if (ma_device_start(&device_->device) != MA_SUCCESS) {
    if (error) *error = "could not start audio playback";
    return false;
  }
  return true;
}

void Host::close() {
  running_ = false;
  if (device_ && device_->haveDevice) ma_device_uninit(&device_->device);
  if (renderThread_.joinable()) renderThread_.join();
  if (device_ && device_->haveContext) ma_context_uninit(&device_->context);
  device_.reset();
  source_.reset();
  rawSource_.reset();
  pending_.reset();
}

void Host::swapSource(const std::string& workWav, const std::string& correctionWav) {
  auto next = std::make_unique<CorrectedSource>(std::make_unique<WavFileSource>(workWav),
                                                correctionWav.empty() ? nullptr : std::make_unique<WavFileSource>(correctionWav), &revStrength_);
  {
    std::lock_guard<std::mutex> lock(swapMutex_);
    pending_ = std::move(next);
  }
  requested_++;
  refresh_ = true;
}

void Host::play() {
  if (atEnd_) {
    seek(double(trimStart_) / kRate);
    atEnd_ = false;
  }
  playing_ = true;
}
void Host::pause() { playing_ = false; }
void Host::seek(double seconds) {
  int64_t t = int64_t(std::llround(seconds * kRate));
  t = std::max<int64_t>(0, std::min<int64_t>(t, length_));
  atEnd_ = false;
  seekRequest_ = t;
}
void Host::setTrim(double startSeconds, double endSeconds) {
  trimStart_ = std::max<int64_t>(0, int64_t(std::llround(startSeconds * kRate)));
  trimEnd_ = std::min<int64_t>(length_, int64_t(std::llround(endSeconds * kRate)));
}

// ---- audio callback: only copies out of the ring (never waits on processing) ----
void Host::deviceCallback(float* out, uint32_t frames) {
  std::unique_lock<std::mutex> lock(ringMutex_, std::try_to_lock);
  if (!lock.owns_lock()) {  // a seek is resetting the ring: output silence this period
    std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
    return;
  }
  const size_t r = rd_.load(std::memory_order_relaxed), w = wr_.load(std::memory_order_acquire);
  const size_t avail = (w - r) & (ringCap_ * 2 - 1);  // indexes run modulo 2*cap to tell full from empty
  const size_t n = std::min<size_t>(frames, avail);
  for (size_t i = 0; i < n; i++) {
    const size_t idx = (r + i) % ringCap_;
    out[2 * i] = ring_[2 * idx];
    out[2 * i + 1] = ring_[2 * idx + 1];
  }
  rd_.store((r + n) & (ringCap_ * 2 - 1), std::memory_order_release);
  delivered_ += int64_t(n);
  if (n > 0 && delivered_.load() >= junction_.load()) heard_ = fresh_.load();
  float pl = 0, pr = 0;
  for (size_t i = 0; i < n; i++) { pl = std::max(pl, std::fabs(out[2 * i])); pr = std::max(pr, std::fabs(out[2 * i + 1])); }
  for (float cur = playPeakL_.load(); pl > cur && !playPeakL_.compare_exchange_weak(cur, pl);) {}
  for (float cur = playPeakR_.load(); pr > cur && !playPeakR_.compare_exchange_weak(cur, pr);) {}
  if (n < frames) {
    std::memset(out + 2 * n, 0, (frames - n) * 2 * sizeof(float));
    if (playing_ && !atEnd_ && !seeking_) underruns_++;
  }
}

// ---- render thread ----
void Host::doSeek(int64_t target, bool clearRing) {
  seeking_ = true;
  const uint64_t serial = requested_.load();
  const int64_t preroll = std::min<int64_t>(target, int64_t(kPrerollSeconds * kRate));
  if (clearRing) {
    std::lock_guard<std::mutex> lock(ringMutex_);
    rd_ = wr_ = 0;
    delivered_ = 0;
    seekBase_ = target;
    fresh_ = serial;
    junction_ = 0;
  }
  chain_.reset(true);
  // Run `preroll` seconds before the target through the chain and throw the result away.
  std::vector<std::vector<float>> buf;
  buf.assign(size_t(channels_), std::vector<float>());
  std::vector<float*> p;
  for (auto& b : buf) { b.resize(kBlock); p.push_back(b.data()); }
  for (int64_t pos = target - preroll; pos < target; pos += kBlock) {
    const int n = int(std::min<int64_t>(kBlock, target - pos));
    int got = source_->read(p.data(), n, pos);
    for (int c = 0; c < channels_; c++) std::fill(buf[size_t(c)].begin() + got, buf[size_t(c)].begin() + kBlock, 0.0f);
    chain_.process(p.data(), kBlock, pos);
  }
  seeking_ = false;
}

void Host::renderLoop() {
  std::vector<std::vector<float>> buf, raw;
  buf.assign(size_t(channels_), std::vector<float>());
  raw.assign(size_t(channels_), std::vector<float>());
  std::vector<float*> pb, pr;
  for (int c = 0; c < channels_; c++) {
    buf[size_t(c)].resize(kBlock);
    raw[size_t(c)].resize(kBlock);
    pb.push_back(buf[size_t(c)].data());
    pr.push_back(raw[size_t(c)].data());
  }
  DelayLine rawDelay;
  rawDelay.prepare(channels_, latency_);
  auto loud = std::make_unique<HopEnergyMeter>(kRate, size_t(channels_), kRate / 10);
  float ab = abTarget_;
  int64_t inPos = 0, nextOut = 0;  // next input position to read / next output position to emit
  bool needSeek = true;
  int64_t pendingSeek = 0;
  size_t blend = 0;  // frames still to crossfade with the old audio still in the ring

  while (running_) {
    const int64_t req = seekRequest_.exchange(-1);
    if (req >= 0) { pendingSeek = req; needSeek = true; }
    {
      std::lock_guard<std::mutex> lock(swapMutex_);
      if (pending_) source_ = std::move(pending_);
    }
    bool splice = false;
    if (refresh_.exchange(false) && !needSeek) {
      // New settings: keep the audio already queued playing (so there's no gap), re-render from just
      // after it with the new settings, and crossfade at the join.
      const size_t mask = ringCap_ * 2 - 1;
      std::lock_guard<std::mutex> lock(ringMutex_);
      const size_t r = rd_.load(), w = wr_.load();
      const size_t keep = std::min<size_t>((w - r) & mask, kKeepFrames);
      if (keep >= 2 * kBlendFrames && !atEnd_) {
        wr_ = (r + keep - kBlendFrames) & mask;
        const int64_t base = seekBase_.load() + delivered_.load();
        pendingSeek = base + int64_t(keep) - int64_t(kBlendFrames);
        fresh_ = requested_.load();
        junction_ = delivered_.load() + int64_t(keep) - int64_t(kBlendFrames);
        splice = true;
        needSeek = true;
        blend = kBlendFrames;
      } else {
        pendingSeek = std::min<int64_t>(length_, seekBase_.load() + delivered_.load() + deviceAhead_);
        needSeek = true;
      }
    }
    if (needSeek) {
      if (!splice) blend = 0;
      doSeek(pendingSeek, !splice);
      rawDelay.reset();
      loud = std::make_unique<HopEnergyMeter>(kRate, size_t(channels_), kRate / 10);
      inPos = pendingSeek;
      nextOut = pendingSeek;
      needSeek = false;
    }
    // keep about 0.5 s ahead; otherwise rest
    const size_t used = (wr_.load() - rd_.load()) & (ringCap_ * 2 - 1);
    if (!playing_ || used + kBlock > kAheadFrames || atEnd_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    const auto t0 = std::chrono::steady_clock::now();

    int got = source_->read(pb.data(), kBlock, inPos);
    for (int c = 0; c < channels_; c++) std::fill(buf[size_t(c)].begin() + got, buf[size_t(c)].end(), 0.0f);
    {
      const int rgot = rawSource_->read(pr.data(), kBlock, inPos);
      for (int c = 0; c < channels_; c++) std::fill(raw[size_t(c)].begin() + rgot, raw[size_t(c)].end(), 0.0f);
    }
    chain_.process(pb.data(), kBlock, inPos);
    rawDelay.process(pr.data(), kBlock);

    // this block's output covers positions [inPos - latency, inPos - latency + kBlock)
    const int64_t outStart = inPos - latency_;
    const int64_t endLimit = trimEnd_.load();
    int from = int(std::max<int64_t>(0, nextOut - outStart));
    int to = int(std::min<int64_t>(kBlock, endLimit - outStart));
    const float abGoal = abTarget_;
    const float step = 1.0f / (0.02f * kRate);
    double blockPeakL = 0, blockPeakR = 0;
    std::vector<float> outFrames;
    for (int i = from; i < to; i++) {
      ab += std::max(-step, std::min(step, abGoal - ab));
      float l, r;
      const float pl = buf[0][size_t(i)], pr2 = buf[size_t(channels_ - 1)][size_t(i)];
      const float rl = raw[0][size_t(i)], rr = raw[size_t(channels_ - 1)][size_t(i)];
      l = rl + (pl - rl) * ab;
      r = rr + (pr2 - rr) * ab;
      outFrames.push_back(l);
      outFrames.push_back(r);
      blockPeakL = std::max(blockPeakL, std::fabs(double(l)));
      blockPeakR = std::max(blockPeakR, std::fabs(double(r)));
    }
    if (!outFrames.empty()) {
      const size_t n = outFrames.size() / 2;
      size_t w = wr_.load(std::memory_order_relaxed);
      for (size_t i = 0; i < n; i++) {
        const size_t idx = (w + i) % ringCap_;
        if (blend > 0) {
          const float a = float(kBlendFrames - blend + 1) / float(kBlendFrames + 1);
          ring_[2 * idx] = ring_[2 * idx] * (1 - a) + outFrames[2 * i] * a;
          ring_[2 * idx + 1] = ring_[2 * idx + 1] * (1 - a) + outFrames[2 * i + 1] * a;
          blend--;
        } else {
          ring_[2 * idx] = outFrames[2 * i];
          ring_[2 * idx + 1] = outFrames[2 * i + 1];
        }
      }
      wr_.store((w + n) & (ringCap_ * 2 - 1), std::memory_order_release);
      // loudness of what is being heard (K-weighted, 100 ms hops)
      std::vector<float> lm(n), rm(n);
      for (size_t i = 0; i < n; i++) { lm[i] = outFrames[2 * i]; rm[i] = outFrames[2 * i + 1]; }
      const float* cp[2] = {lm.data(), rm.data()};
      loud->feed(cp, n);
      nextOut += int64_t(n);
    }
    inPos += kBlock;

    {
      std::lock_guard<std::mutex> lock(meterMutex_);
      const auto& e = loud->energies();
      auto avg = [&](size_t count) { double s = 0; size_t k = 0; for (size_t i = e.size() >= count ? e.size() - count : 0; i < e.size(); i++, k++) s += e[i]; return k ? s / double(k) : 0.0; };
      if (e.size() >= 4) momentary_ = energyToLufs(avg(4));
      if (e.size() >= 30) shortTerm_ = energyToLufs(avg(30));
      lastMeters_ = chain_.meters();
      const double busy = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      load_ = 0.9 * load_ + 0.1 * (busy / (double(kBlock) / kRate));
    }

    if (nextOut >= endLimit) {  // reached the end of the trimmed range
      if (loop_) {
        pendingSeek = trimStart_.load();
        needSeek = true;
      } else {
        atEnd_ = true;
        playing_ = false;
      }
    }
  }
}

HostState Host::state() {
  HostState s;
  s.position = double(seekBase_.load() + delivered_.load()) / kRate;
  s.playing = playing_;
  s.atEnd = atEnd_;
  s.applying = playing_ && heard_.load() != requested_.load();
  s.underruns = underruns_;
  std::lock_guard<std::mutex> lock(meterMutex_);
  s.peakL = playPeakL_.exchange(0);
  s.peakR = playPeakR_.exchange(0);
  s.momentaryLufs = momentary_;
  s.shortTermLufs = shortTerm_;
  s.meters = lastMeters_;
  s.renderLoad = load_;
  return s;
}

}  // namespace sermon::rt
