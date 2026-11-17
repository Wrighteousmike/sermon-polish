#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "source.h"

namespace sermon::rt {

// A resampler that accepts its input in chunks of any size and returns the same samples as the
// whole-buffer resample() (Kaiser-windowed sinc, rational ratio). Memory is a few thousand samples.
class StreamResampler {
 public:
  StreamResampler(int srcRate, int dstRate);
  // Appends input; returns the output samples that can now be produced.
  std::vector<float> push(const float* in, size_t n);
  // No more input: returns the remaining output (the total is floor(inputCount * dst / src)).
  std::vector<float> finish();

 private:
  void produce(std::vector<float>& out, bool final);
  bool passthrough_;
  long up_ = 1, down_ = 1, half_ = 1, taps_ = 2;
  std::vector<float> table_;
  std::vector<float> buf_;  // input from index base_ onward
  long long base_ = 0;      // absolute index of buf_[0]
  long long inCount_ = 0, outDone_ = 0;
};

struct WorkFileInfo {
  double durationSeconds = 0;
  int channels = 0;
  int originalSampleRate = 0;
  int64_t frames = 0;  // at 48 kHz
};

// Decodes a WAV or MP3 in chunks, resamples to 48 kHz (mono or stereo; more channels are mixed down),
// writes a 32-bit float WAV, and builds the 1 ms waveform peaks file (see writePeaksFine) on the way.
// Memory stays small however long the recording is. Throws std::runtime_error.
WorkFileInfo prepareWorkingFile(const std::string& srcPath, const std::string& wavOut, const std::string& finePeaksOut,
                                const std::function<void(double)>& progress = {});

// Reads a float WAV (the working file) block by block with random access.
class WavFileSource : public SampleSource {
 public:
  explicit WavFileSource(const std::string& path);
  ~WavFileSource() override;
  int channels() const override { return channels_; }
  double sampleRate() const override { return sampleRate_; }
  int64_t length() const override { return length_; }
  int read(float* const* out, int n, int64_t position) override;

 private:
  struct Impl;
  Impl* impl_;
  int channels_ = 0;
  double sampleRate_ = 0;
  int64_t length_ = 0;
};

// Writes int16 (min, max) peak pairs in the SPK1 format.
void writePeaksFineFile(const std::string& path, const std::vector<int16_t>& pairs, double bucketSeconds);

}  // namespace sermon::rt
