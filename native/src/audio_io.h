#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sermon {

struct AudioData {
  int sampleRate = 0;
  std::vector<std::vector<float>> channels;  // planar
  size_t length() const { return channels.empty() ? 0 : channels[0].size(); }
};

// Decode a WAV or MP3 file to planar float. Throws std::runtime_error.
AudioData readAudioFile(const std::string& path);

// Decode an MP3 held in memory. Throws std::runtime_error.
AudioData decodeMp3(const std::vector<uint8_t>& bytes);

// Write a WAV: 32-bit float (default, lossless intermediate) or 16-bit PCM (`bits` = 16, for
// playback in webviews; values are clipped to full scale). Throws on failure.
void writeWav(const std::string& path, const AudioData& audio, int bits = 32);

// Streaming WAV writer (32-bit float): write blocks as they are produced. Throws on failure.
class WavWriter {
 public:
  WavWriter(const std::string& path, int channels, int sampleRate);
  ~WavWriter();
  void write(const float* const* planar, int frames);
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sermon
