#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "../audio_io.h"

namespace sermon::rt {

// Audio the engine reads in blocks (a file on disk in the app, a buffer in tests).
class SampleSource {
 public:
  virtual ~SampleSource() = default;
  virtual int channels() const = 0;
  virtual double sampleRate() const = 0;
  virtual int64_t length() const = 0;
  // Reads up to n samples starting at `position` into out[0..channels); returns how many were read.
  virtual int read(float* const* out, int n, int64_t position) = 0;
};

class MemorySource : public SampleSource {
 public:
  explicit MemorySource(const AudioData& a) : a_(a) {}
  int channels() const override { return int(a_.channels.size()); }
  double sampleRate() const override { return a_.sampleRate; }
  int64_t length() const override { return int64_t(a_.length()); }
  int read(float* const* out, int n, int64_t position) override {
    const int64_t avail = length() - position;
    const int got = int(std::max<int64_t>(0, std::min<int64_t>(n, avail)));
    for (size_t c = 0; c < a_.channels.size(); c++)
      if (got > 0) std::memcpy(out[c], a_.channels[c].data() + position, size_t(got) * sizeof(float));
    return got;
  }

 private:
  const AudioData& a_;
};

}  // namespace sermon::rt
