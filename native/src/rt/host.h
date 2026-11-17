#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chain.h"
#include "corrected_source.h"
#include "workfile.h"

namespace sermon::rt {

struct HostState {
  double position = 0;  // seconds, what is being heard
  bool applying = false;  // a settings change has been made but isn't audible yet
  bool playing = false;
  bool atEnd = false;
  double peakL = 0, peakR = 0;       // since the last poll, linear
  double momentaryLufs = -70, shortTermLufs = -70;
  Meters meters;
  uint64_t underruns = 0;
  double renderLoad = 0;  // render thread busy fraction (processing time / audio time)
};

// Plays a prepared (48 kHz float WAV) working file through the Chain in real time: a render thread
// keeps a ring buffer ahead of an audio device whose callback only copies out of it, so the device
// can use a large buffer (2048-4096 frames) and never waits on processing. Parameters change live;
// seeking, looping, trim limits and original-vs-processed switching are all handled here.
class Host {
 public:
  Host();
  ~Host();

  // Opens the working file and starts the device (nullDevice = no sound hardware, for tests).
  // `model` is built from the same file (see buildFileModel).
  bool open(const std::string& workWav, std::shared_ptr<const FileModel> model, bool nullDevice, int deviceBuffer, std::string* error);
  void close();

  // Plays from a different working file (e.g. one with declipping / dereverb baked in) from here on,
  // crossfading like a settings change. The A/B "original" keeps coming from the first file.
  // `correctionWav` (optional) is the dereverb correction for that file; see CorrectedSource.
  void swapSource(const std::string& workWav, const std::string& correctionWav = "");
  // How much of the correction is mixed in (WPE strength: 0 = none, 1 = full, up to 1.3). Live.
  void setDereverbStrength(double strength) {
    revStrength_ = strength;
    requested_++;
    refresh_ = true;
  }

  // With refresh, audio already rendered ahead is thrown away and re-rendered with the new settings
  // so the change is heard within a fraction of a second (refresh=false for changes that need not
  // be instant, like the loudness gain).
  void setParams(const ChainParams& params, bool refresh = true) {
    chain_.setParams(params);
    if (refresh) {
      requested_++;
      refresh_ = true;
    }
  }
  void play();
  void pause();
  void seek(double seconds);
  void setTrim(double startSeconds, double endSeconds);  // playback stays inside this range
  void setLoop(bool enabled) { loop_ = enabled; }
  void setOriginal(bool original) { abTarget_ = original ? 0.0f : 1.0f; }
  double duration() const { return double(length_) / 48000.0; }
  HostState state();  // also resets the peak hold
  void deviceCallbackPublic(float* out, uint32_t frames) { deviceCallback(out, frames); }

 private:
  struct Device;
  void renderLoop();
  void doSeek(int64_t target, bool clearRing = true);
  void deviceCallback(float* out, uint32_t frames);

  Chain chain_;
  std::unique_ptr<CorrectedSource> source_;  // processed input
  std::unique_ptr<WavFileSource> rawSource_;  // untouched original (for A/B)
  std::atomic<double> revStrength_{0};
  std::mutex swapMutex_;
  std::unique_ptr<CorrectedSource> pending_;
  std::shared_ptr<const FileModel> model_;
  int channels_ = 1;
  int64_t length_ = 0;
  int latency_ = 0;

  // transport (control threads write, render thread reads)
  std::atomic<bool> playing_{false}, running_{false}, loop_{false};
  std::atomic<int64_t> seekRequest_{-1};
  std::atomic<int64_t> trimStart_{0}, trimEnd_{0};
  std::atomic<float> abTarget_{1.0f};

  // ring buffer of interleaved stereo frames
  std::mutex ringMutex_;
  std::vector<float> ring_;
  size_t ringCap_ = 0;  // frames
  std::atomic<size_t> rd_{0}, wr_{0};
  std::atomic<int64_t> delivered_{0};  // frames played since the last seek
  std::atomic<int64_t> seekBase_{0};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<bool> atEnd_{false};
  std::atomic<bool> refresh_{false};
  std::atomic<uint64_t> requested_{0}, fresh_{0}, heard_{0};  // settings-change serials: asked for / being rendered / audible
  std::atomic<int64_t> junction_{0};  // frames delivered (since the last seek) at which re-rendered audio starts
  std::atomic<bool> seeking_{false};
  int deviceAhead_ = 0;  // frames the device itself holds queued
  std::atomic<float> playPeakL_{0}, playPeakR_{0};  // peaks of what the device actually played

  // meters
  std::mutex meterMutex_;
  double peakL_ = 0, peakR_ = 0, momentary_ = -70, shortTerm_ = -70, load_ = 0;
  Meters lastMeters_;

  std::thread renderThread_;
  std::unique_ptr<Device> device_;
};

}  // namespace sermon::rt
