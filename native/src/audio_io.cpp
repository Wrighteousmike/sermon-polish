#include "fsutil.h"
#include "audio_io.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#include "dr_wav.h"

namespace sermon {

namespace {
bool endsWith(std::string s, const std::string& suffix) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

AudioData deinterleave(const float* data, unsigned long long frames, unsigned channels, int rate) {
  AudioData a;
  a.sampleRate = rate;
  a.channels.assign(channels, std::vector<float>(size_t(frames)));
  for (size_t i = 0; i < frames; i++)
    for (unsigned c = 0; c < channels; c++) a.channels[c][i] = data[i * channels + c];
  return a;
}
}  // namespace

AudioData readAudioFile(const std::string& path) {
  // Decoded in chunks straight into per-channel arrays, so the whole file is never held twice.
  constexpr size_t kChunkFrames = 65536;
  if (endsWith(path, ".mp3")) {
    drmp3 mp3;
    if (!SERMON_MP3_OPEN(mp3, path)) throw std::runtime_error("could not decode MP3: " + path);
    AudioData a;
    a.sampleRate = int(mp3.sampleRate);
    a.channels.assign(mp3.channels, {});
    const drmp3_uint64 total = drmp3_get_pcm_frame_count(&mp3);
    drmp3_seek_to_pcm_frame(&mp3, 0);
    for (auto& c : a.channels) c.reserve(size_t(total));
    std::vector<float> buf(kChunkFrames * mp3.channels);
    for (;;) {
      const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3, kChunkFrames, buf.data());
      if (got == 0) break;
      for (size_t i = 0; i < got; i++)
        for (unsigned c = 0; c < mp3.channels; c++) a.channels[c].push_back(buf[i * mp3.channels + c]);
    }
    drmp3_uninit(&mp3);
    if (a.length() == 0) throw std::runtime_error("could not decode MP3: " + path);
    return a;
  }
  drwav wav;
  if (!SERMON_WAV_OPEN(wav, path)) throw std::runtime_error("could not decode WAV: " + path);
  AudioData a;
  a.sampleRate = int(wav.sampleRate);
  a.channels.assign(wav.channels, {});
  for (auto& c : a.channels) c.reserve(size_t(wav.totalPCMFrameCount));
  std::vector<float> buf(kChunkFrames * wav.channels);
  for (;;) {
    const drwav_uint64 got = drwav_read_pcm_frames_f32(&wav, kChunkFrames, buf.data());
    if (got == 0) break;
    for (size_t i = 0; i < got; i++)
      for (unsigned c = 0; c < wav.channels; c++) a.channels[c].push_back(buf[i * wav.channels + c]);
  }
  drwav_uninit(&wav);
  if (a.length() == 0) throw std::runtime_error("could not decode WAV: " + path);
  return a;
}

AudioData decodeMp3(const std::vector<uint8_t>& bytes) {
  drmp3_config cfg;
  drmp3_uint64 frames = 0;
  float* data = drmp3_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &cfg, &frames, nullptr);
  if (!data) throw std::runtime_error("could not decode MP3 from memory");
  AudioData a = deinterleave(data, frames, cfg.channels, int(cfg.sampleRate));
  drmp3_free(data, nullptr);
  return a;
}

void writeWav(const std::string& path, const AudioData& audio, int bits) {
  const bool pcm16 = bits == 16;
  drwav_data_format fmt;
  fmt.container = drwav_container_riff;
  fmt.format = pcm16 ? DR_WAVE_FORMAT_PCM : DR_WAVE_FORMAT_IEEE_FLOAT;
  fmt.channels = unsigned(audio.channels.size());
  fmt.sampleRate = unsigned(audio.sampleRate);
  fmt.bitsPerSample = pcm16 ? 16 : 32;
  drwav wav;
  if (!SERMON_WAV_CREATE(wav, path, fmt)) throw std::runtime_error("could not write: " + path);
  const size_t n = audio.length(), ch = audio.channels.size();
  std::vector<float> block(4096 * ch);
  std::vector<int16_t> block16(pcm16 ? 4096 * ch : 0);
  for (size_t i = 0; i < n; i += 4096) {
    const size_t m = std::min<size_t>(4096, n - i);
    for (size_t k = 0; k < m; k++)
      for (size_t c = 0; c < ch; c++) block[k * ch + c] = audio.channels[c][i + k];
    if (pcm16) {
      for (size_t j = 0; j < m * ch; j++) {
        const float v = std::max(-1.0f, std::min(1.0f, block[j]));
        block16[j] = static_cast<int16_t>(std::lrintf(v * 32767.0f));
      }
      drwav_write_pcm_frames(&wav, m, block16.data());
    } else {
      drwav_write_pcm_frames(&wav, m, block.data());
    }
  }
  drwav_uninit(&wav);
}

struct WavWriter::Impl {
  drwav wav;
  bool open = false;
  int channels = 1;
  std::vector<float> inter;
};

WavWriter::WavWriter(const std::string& path, int channels, int sampleRate) : impl_(new Impl) {
  drwav_data_format fmt;
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
  fmt.channels = unsigned(channels);
  fmt.sampleRate = unsigned(sampleRate);
  fmt.bitsPerSample = 32;
  if (!SERMON_WAV_CREATE(impl_->wav, path, fmt)) throw std::runtime_error("could not write: " + path);
  impl_->open = true;
  impl_->channels = channels;
}

WavWriter::~WavWriter() { close(); }

void WavWriter::close() {
  if (impl_ && impl_->open) {
    drwav_uninit(&impl_->wav);
    impl_->open = false;
  }
}

void WavWriter::write(const float* const* planar, int frames) {
  const size_t ch = size_t(impl_->channels);
  impl_->inter.resize(size_t(frames) * ch);
  for (int i = 0; i < frames; i++)
    for (size_t c = 0; c < ch; c++) impl_->inter[size_t(i) * ch + c] = planar[c][i];
  drwav_write_pcm_frames(&impl_->wav, size_t(frames), impl_->inter.data());
}

}  // namespace sermon
