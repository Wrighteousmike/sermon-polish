#include <cstdint>
#include "../fsutil.h"
#include "workfile.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <stdexcept>

#include "dr_mp3.h"
#include "dr_wav.h"

namespace sermon::rt {

namespace {
double besselI0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 50; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}
bool endsWithLower(std::string s, const std::string& suffix) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
}  // namespace

// ---------------- streaming resampler (same table and arithmetic as sermon::resample) ----------------

StreamResampler::StreamResampler(int srcRate, int dstRate) : passthrough_(srcRate == dstRate) {
  if (passthrough_) return;
  const long g = std::gcd(srcRate, dstRate);
  up_ = dstRate / g;
  down_ = srcRate / g;
  const double pi = 3.14159265358979323846, beta = 9.0;
  const double ratio = std::min(1.0, double(up_) / double(down_));
  const double fc = ratio * 0.96;
  half_ = static_cast<long>(std::ceil(16.0 / ratio));
  taps_ = 2 * half_;
  table_.resize(size_t(up_) * size_t(taps_));
  const double i0b = besselI0(beta);
  for (long ph = 0; ph < up_; ph++) {
    const double frac = double(ph) / double(up_);
    double sum = 0;
    std::vector<double> row(static_cast<size_t>(taps_));
    for (long k = -half_ + 1; k <= half_; k++) {
      const double t = double(k) - frac;
      const double x = pi * t * fc;
      const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
      const double r = t / double(half_);
      const double w = std::fabs(r) < 1 ? besselI0(beta * std::sqrt(1 - r * r)) / i0b : 0.0;
      row[size_t(k + half_ - 1)] = fc * sinc * w;
      sum += row[size_t(k + half_ - 1)];
    }
    for (long k = 0; k < taps_; k++) table_[size_t(ph) * size_t(taps_) + size_t(k)] = float(row[size_t(k)] / sum);
  }
}

void StreamResampler::produce(std::vector<float>& out, bool final) {
  const long long total = final ? (inCount_ * up_) / down_ : -1;  // total outputs, known only at the end
  for (;;) {
    const long long pos = outDone_ * down_;
    const long long i0 = pos / up_;
    const long ph = long(pos % up_);
    if (final) {
      if (outDone_ >= total) break;
    } else if (i0 + half_ >= base_ + (long long)buf_.size()) {
      break;  // the newest input this output needs hasn't arrived yet
    }
    const float* row = &table_[size_t(ph) * size_t(taps_)];
    double acc = 0;
    for (long k = -half_ + 1; k <= half_; k++) {
      const long long idx = i0 + k - base_;
      if (idx >= 0 && idx < (long long)buf_.size()) acc += double(buf_[size_t(idx)]) * row[k + half_ - 1];
    }
    out.push_back(float(acc));
    outDone_++;
  }
  // Drop input that no future output can use.
  const long long keepFrom = (outDone_ * down_) / up_ - half_ + 1;
  if (keepFrom > base_) {
    const long long drop = std::min<long long>(keepFrom - base_, (long long)buf_.size());
    buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)drop);
    base_ += drop;
  }
}

std::vector<float> StreamResampler::push(const float* in, size_t n) {
  if (passthrough_) return std::vector<float>(in, in + n);
  buf_.insert(buf_.end(), in, in + n);
  inCount_ += (long long)n;
  std::vector<float> out;
  produce(out, false);
  return out;
}

std::vector<float> StreamResampler::finish() {
  if (passthrough_) return {};
  std::vector<float> out;
  produce(out, true);
  return out;
}

// ---------------- working file ----------------

void writePeaksFineFile(const std::string& path, const std::vector<int16_t>& pairs, double bucketSeconds) {
  std::ofstream f(pathOf(path), std::ios::binary);
  if (!f) throw std::runtime_error("could not write: " + path);
  const uint32_t version = 1, count = uint32_t(pairs.size() / 2);
  const float bs = float(bucketSeconds);
  f.write("SPK1", 4);
  f.write(reinterpret_cast<const char*>(&version), 4);
  f.write(reinterpret_cast<const char*>(&count), 4);
  f.write(reinterpret_cast<const char*>(&bs), 4);
  f.write(reinterpret_cast<const char*>(pairs.data()), std::streamsize(pairs.size() * sizeof(int16_t)));
}

namespace {

constexpr int kRate = 48000;
constexpr size_t kChunk = 65536;
constexpr size_t kPeakBucket = 48;  // 1 ms at 48 kHz

// Collects the 1 ms min/max peaks of the channel mix from the 48 kHz stream.
struct PeakBuilder {
  std::vector<int16_t> pairs;
  float mn = 1.0f, mx = -1.0f;
  size_t inBucket = 0;
  void feed(const std::vector<std::vector<float>>& ch, size_t n) {
    for (size_t i = 0; i < n; i++) {
      float v = 0;
      for (const auto& c : ch) v += c[i];
      v /= float(ch.size());
      mn = std::min(mn, v);
      mx = std::max(mx, v);
      if (++inBucket == kPeakBucket) flush();
    }
  }
  void flush() {
    auto toInt = [](float v) { return int16_t(std::lrintf(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f)); };
    pairs.push_back(toInt(mn));
    pairs.push_back(toInt(mx));
    mn = 1.0f; mx = -1.0f; inBucket = 0;
  }
};

}  // namespace

WorkFileInfo prepareWorkingFile(const std::string& srcPath, const std::string& wavOut, const std::string& finePeaksOut,
                                const std::function<void(double)>& progress) {
  const bool isMp3 = endsWithLower(srcPath, ".mp3");
  drmp3 mp3;
  drwav wav;
  int srcCh = 0, srcRate = 0;
  long long totalFrames = 0;
  if (isMp3) {
    if (!SERMON_MP3_OPEN(mp3, srcPath)) throw std::runtime_error("could not decode MP3: " + srcPath);
    srcCh = int(mp3.channels);
    srcRate = int(mp3.sampleRate);
    totalFrames = (long long)drmp3_get_pcm_frame_count(&mp3);
    drmp3_seek_to_pcm_frame(&mp3, 0);
  } else {
    if (!SERMON_WAV_OPEN(wav, srcPath)) throw std::runtime_error("could not decode WAV: " + srcPath);
    srcCh = int(wav.channels);
    srcRate = int(wav.sampleRate);
    totalFrames = (long long)wav.totalPCMFrameCount;
  }
  const int outCh = srcCh >= 2 ? 2 : 1;

  drwav_data_format fmt;
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
  fmt.channels = unsigned(outCh);
  fmt.sampleRate = kRate;
  fmt.bitsPerSample = 32;
  drwav out;
  if (!SERMON_WAV_CREATE(out, wavOut, fmt)) {
    if (isMp3) drmp3_uninit(&mp3); else drwav_uninit(&wav);
    throw std::runtime_error("could not write: " + wavOut);
  }

  std::vector<StreamResampler> rs;
  for (int c = 0; c < outCh; c++) rs.emplace_back(srcRate, kRate);
  PeakBuilder peaks;
  std::vector<float> interleaved(kChunk * size_t(srcCh));
  std::vector<std::vector<float>> chans(static_cast<size_t>(outCh));
  long long framesOut = 0, framesIn = 0;

  auto emit = [&](std::vector<std::vector<float>>& produced) {
    const size_t n = produced[0].size();
    if (n == 0) return;
    std::vector<float> mixed(n * size_t(outCh));
    for (size_t i = 0; i < n; i++)
      for (int c = 0; c < outCh; c++) mixed[i * size_t(outCh) + size_t(c)] = produced[size_t(c)][i];
    drwav_write_pcm_frames(&out, n, mixed.data());
    peaks.feed(produced, n);
    framesOut += (long long)n;
  };

  for (;;) {
    const drwav_uint64 got = isMp3 ? drmp3_read_pcm_frames_f32(&mp3, kChunk, interleaved.data())
                                   : drwav_read_pcm_frames_f32(&wav, kChunk, interleaved.data());
    if (got == 0) break;
    for (int c = 0; c < outCh; c++) chans[size_t(c)].assign(size_t(got), 0.0f);
    for (size_t i = 0; i < got; i++) {
      if (srcCh <= 2) {
        for (int c = 0; c < outCh; c++) chans[size_t(c)][i] = interleaved[i * size_t(srcCh) + size_t(c)];
      } else {  // mix surround down: even channels to the left, odd to the right
        double l = 0, r = 0; int nl = 0, nr = 0;
        for (int c = 0; c < srcCh; c++) { if (c % 2 == 0) { l += interleaved[i * size_t(srcCh) + size_t(c)]; nl++; } else { r += interleaved[i * size_t(srcCh) + size_t(c)]; nr++; } }
        chans[0][i] = float(l / nl); chans[1][i] = float(r / std::max(nr, 1));
      }
    }
    std::vector<std::vector<float>> produced(static_cast<size_t>(outCh));
    for (int c = 0; c < outCh; c++) produced[size_t(c)] = rs[size_t(c)].push(chans[size_t(c)].data(), size_t(got));
    emit(produced);
    framesIn += (long long)got;
    if (progress && totalFrames > 0) progress(std::min(1.0, double(framesIn) / double(totalFrames)));
  }
  std::vector<std::vector<float>> tail(static_cast<size_t>(outCh));
  for (int c = 0; c < outCh; c++) tail[size_t(c)] = rs[size_t(c)].finish();
  emit(tail);
  if (peaks.inBucket > 0) peaks.flush();

  drwav_uninit(&out);
  if (isMp3) drmp3_uninit(&mp3); else drwav_uninit(&wav);
  if (framesOut == 0) throw std::runtime_error("no audio found in: " + srcPath);
  writePeaksFineFile(finePeaksOut, peaks.pairs, double(kPeakBucket) / double(kRate));
  WorkFileInfo info;
  info.channels = outCh;
  info.originalSampleRate = srcRate;
  info.frames = framesOut;
  info.durationSeconds = double(framesOut) / double(kRate);
  return info;
}

// ---------------- reading the working file ----------------

struct WavFileSource::Impl {
  drwav wav;
  std::vector<float> inter;
  int64_t next = 0;
};

WavFileSource::WavFileSource(const std::string& path) : impl_(new Impl()) {
  if (!SERMON_WAV_OPEN(impl_->wav, path)) {
    delete impl_;
    throw std::runtime_error("could not open: " + path);
  }
  channels_ = int(impl_->wav.channels);
  sampleRate_ = double(impl_->wav.sampleRate);
  length_ = int64_t(impl_->wav.totalPCMFrameCount);
}

WavFileSource::~WavFileSource() {
  drwav_uninit(&impl_->wav);
  delete impl_;
}

int WavFileSource::read(float* const* out, int n, int64_t position) {
  if (position < 0 || position >= length_) return 0;
  if (position != impl_->next) drwav_seek_to_pcm_frame(&impl_->wav, drwav_uint64(position));
  const int want = int(std::min<int64_t>(n, length_ - position));
  impl_->inter.resize(size_t(want) * size_t(channels_));
  const drwav_uint64 got = drwav_read_pcm_frames_f32(&impl_->wav, drwav_uint64(want), impl_->inter.data());
  for (size_t i = 0; i < got; i++)
    for (int c = 0; c < channels_; c++) out[c][i] = impl_->inter[i * size_t(channels_) + size_t(c)];
  impl_->next = position + int64_t(got);
  return int(got);
}

}  // namespace sermon::rt
