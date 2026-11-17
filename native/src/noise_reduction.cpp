#include "fsutil.h"
#include "noise_reduction.h"

#include "amount.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "biquad.h"

#ifdef SERMON_HAVE_RNNOISE
#include "rnnoise.h"
#endif
#ifdef SERMON_HAVE_DEEPFILTERNET
#include "df_capi.h"
#endif

namespace sermon {

namespace {
struct Settings { double wet, notchQ; };
Settings settingsFor(Intensity i) { return {amount::noiseWet(i), amount::humNotchQ(i)}; }
}  // namespace

void removeHum(float* x, size_t n, double fs, Intensity intensity) {
  const Settings s = settingsFor(intensity);
  for (double hz : {50.0, 100.0, 150.0}) Biquad::notch(fs, hz, s.notchQ).processBlock(x, n);
}

#ifdef SERMON_HAVE_RNNOISE
constexpr bool kHaveRnnoise = true;
#else
constexpr bool kHaveRnnoise = false;
#endif
#ifdef SERMON_HAVE_DEEPFILTERNET
constexpr bool kHaveDf = true;
#else
constexpr bool kHaveDf = false;
#endif

bool noiseBackendAvailable(NoiseBackend b) {
  switch (b) {
    case NoiseBackend::RNNoise: return kHaveRnnoise;
    case NoiseBackend::DeepFilterNet: return kHaveDf;
    default: return kHaveRnnoise || kHaveDf;
  }
}

bool noiseReductionAvailable() { return noiseBackendAvailable(NoiseBackend::Auto); }

namespace {
NoiseBackend resolve(NoiseBackend b) {
  if (b == NoiseBackend::Auto) return kHaveDf ? NoiseBackend::DeepFilterNet : NoiseBackend::RNNoise;
  return b;
}
}  // namespace

std::string noiseModelPath(bool* found) {
#ifdef SERMON_HAVE_DEEPFILTERNET
  const char* env = std::getenv("SERMON_DF_MODEL");
  const std::string path = env ? env : SERMON_DF_MODEL_PATH;
  if (found) *found = std::ifstream(pathOf(path)).good();
  return path;
#else
  if (found) *found = false;
  return "";
#endif
}

const char* noiseBackendName(NoiseBackend b) {
  if (!noiseBackendAvailable(b)) return "none";
  return resolve(b) == NoiseBackend::DeepFilterNet ? "DeepFilterNet3" : "RNNoise";
}

#ifdef SERMON_HAVE_RNNOISE

// RNNoise processes 10ms (480-sample) frames at 48kHz, expecting samples
// scaled to 16-bit range. Its output lags its input by a fixed number of
// samples (kLatency, measured by cross-correlating against real speech); the
// dry path is delayed to match before blending, so the result is
// sample-aligned with the input. (Hum notches are a separate stage, see
// removeHum.)
constexpr size_t kFrame = 480;
constexpr size_t kLatency = 959;  // measured on real speech: output aligns with input at lag 0

static void reduceNoiseRnnoise(float* x, size_t n, Intensity intensity) {
  const Settings s = settingsFor(intensity);

  // In place, frame by frame. The output for input time p arrives kLatency samples later, i.e. at
  // least a whole frame behind the read position, so writing it back never overwrites audio that
  // hasn't been read yet, and x[p] still holds the dry original when it is blended with the wet.
  static_assert(kLatency >= kFrame, "output must lag by at least one frame");
  DenoiseState* st = rnnoise_create(nullptr);
  float frameIn[kFrame], frameOut[kFrame];
  const size_t frames = (n + kLatency + kFrame - 1) / kFrame;  // enough to flush the latency tail
  for (size_t f = 0; f < frames; f++) {
    const size_t off = f * kFrame;
    for (size_t i = 0; i < kFrame; i++) frameIn[i] = (off + i < n ? x[off + i] : 0.0f) * 32768.0f;
    rnnoise_process_frame(st, frameOut, frameIn);
    for (size_t i = 0; i < kFrame; i++) {
      const size_t j = off + i;
      if (j < kLatency) continue;
      const size_t p = j - kLatency;
      if (p >= n) break;
      const double w = frameOut[i] / 32768.0f;
      x[p] = float(s.wet * w + (1.0 - s.wet) * double(x[p]));
    }
  }
  rnnoise_destroy(st);
}

#else

static void reduceNoiseRnnoise(float*, size_t, Intensity) {}

#endif

#ifdef SERMON_HAVE_DEEPFILTERNET

// DeepFilterNet3 works in 480-sample hops at 48 kHz on +-1.0 float audio. Its
// output lags the input by kDfLatency samples (measured by cross-correlation
// against real speech).
constexpr size_t kDfLatency = 1440;  // three 10 ms hops; measured on real speech (lag 0 after compensation)

static void reduceNoiseDeepFilter(float* x, size_t n, Intensity intensity) {
  const char* envPath = std::getenv("SERMON_DF_MODEL");
  const std::string model = envPath ? envPath : SERMON_DF_MODEL_PATH;
  if (!std::ifstream(pathOf(model)).good()) return;  // df_create would abort the process on a bad path
  const float atten = float(amount::noiseAttenDb(intensity));
  DFState* st = df_create(model.c_str(), atten, nullptr);
  if (!st) return;
  const size_t frame = df_get_frame_length(st);
  static_assert(kDfLatency >= 480, "output must lag by at least one frame");
  // In place, frame by frame (see the RNNoise version): output for time p arrives kDfLatency
  // samples later, always behind the read position.
  std::vector<float> in(frame), out(frame);
  const size_t frames = (n + kDfLatency + frame - 1) / frame;
  for (size_t f = 0; f < frames; f++) {
    const size_t off = f * frame;
    for (size_t i = 0; i < frame; i++) in[i] = off + i < n ? x[off + i] : 0.0f;
    df_process_frame(st, in.data(), out.data());
    for (size_t i = 0; i < frame; i++) {
      const size_t j = off + i;
      if (j < kDfLatency) continue;
      const size_t p = j - kDfLatency;
      if (p >= n) break;
      x[p] = out[i];
    }
  }
  df_free(st);
}

#else

static void reduceNoiseDeepFilter(float*, size_t, Intensity) {}

#endif

void reduceNoise(float* x, size_t n, double fs, Intensity intensity, NoiseBackend backend) {
  if (fs != 48000.0 || n == 0 || !noiseBackendAvailable(backend)) return;
  if (resolve(backend) == NoiseBackend::DeepFilterNet) reduceNoiseDeepFilter(x, n, intensity);
  else reduceNoiseRnnoise(x, n, intensity);
}

}  // namespace sermon
