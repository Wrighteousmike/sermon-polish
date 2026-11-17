#include <cstdint>
#include "fsutil.h"
#include "trim.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace sermon {

void trimWithFades(AudioData& audio, double startSeconds, double endSeconds, double fadeMs) {
  if (audio.channels.empty()) return;
  const size_t n = audio.length();
  const double fs = audio.sampleRate;
  size_t a = size_t(std::max(0.0, std::floor(startSeconds * fs)));
  size_t b = size_t(std::max(0.0, std::ceil(endSeconds * fs)));
  a = std::min(a, n);
  b = std::min(std::max(b, a), n);
  const size_t kept = b - a;
  const size_t fade = std::min(size_t(std::llround(fadeMs / 1000.0 * fs)), kept / 2);
  constexpr double kPi = 3.14159265358979323846;
  for (auto& c : audio.channels) {
    std::vector<float> out(c.begin() + std::ptrdiff_t(a), c.begin() + std::ptrdiff_t(b));
    for (size_t i = 0; i < fade; i++) {
      const double g = 0.5 - 0.5 * std::cos(kPi * (double(i) + 0.5) / double(fade));
      out[i] = float(out[i] * g);
      out[kept - 1 - i] = float(out[kept - 1 - i] * g);
    }
    c = std::move(out);
  }
}

std::vector<float> computePeaks(const AudioData& audio, size_t buckets) {
  std::vector<float> peaks(buckets * 2, 0.0f);
  const size_t n = audio.length(), nCh = audio.channels.size();
  if (n == 0 || nCh == 0 || buckets == 0) return peaks;
  for (size_t k = 0; k < buckets; k++) {
    const size_t from = k * n / buckets, to = std::max(from + 1, (k + 1) * n / buckets);
    float mn = 1.0f, mx = -1.0f;
    for (size_t i = from; i < to && i < n; i++) {
      float v = 0;
      for (size_t c = 0; c < nCh; c++) v += audio.channels[c][i];
      v /= float(nCh);
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    peaks[2 * k] = std::max(-1.0f, std::min(1.0f, mn));
    peaks[2 * k + 1] = std::max(-1.0f, std::min(1.0f, mx));
  }
  return peaks;
}

std::vector<int16_t> computePeaksFine(const AudioData& audio, size_t bucketSamples) {
  const size_t n = audio.length(), nCh = audio.channels.size();
  bucketSamples = std::max<size_t>(1, bucketSamples);
  const size_t buckets = (n + bucketSamples - 1) / bucketSamples;
  std::vector<int16_t> peaks(buckets * 2, 0);
  if (n == 0 || nCh == 0) return peaks;
  auto toInt = [](float v) { return int16_t(std::lrintf(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f)); };
  for (size_t k = 0; k < buckets; k++) {
    const size_t from = k * bucketSamples, to = std::min(n, from + bucketSamples);
    float mn = 1.0f, mx = -1.0f;
    for (size_t i = from; i < to; i++) {
      float v = 0;
      for (size_t c = 0; c < nCh; c++) v += audio.channels[c][i];
      v /= float(nCh);
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    peaks[2 * k] = toInt(mn);
    peaks[2 * k + 1] = toInt(mx);
  }
  return peaks;
}

void writePeaksFine(const std::string& path, const AudioData& audio, double bucketMs) {
  const size_t bucketSamples = std::max<size_t>(1, size_t(std::llround(bucketMs / 1000.0 * audio.sampleRate)));
  const std::vector<int16_t> peaks = computePeaksFine(audio, bucketSamples);
  std::ofstream f(pathOf(path), std::ios::binary);
  if (!f) throw std::runtime_error("could not write: " + path);
  const uint32_t version = 1, count = uint32_t(peaks.size() / 2);
  const float bucketSeconds = float(double(bucketSamples) / double(audio.sampleRate));
  f.write("SPK1", 4);
  f.write(reinterpret_cast<const char*>(&version), 4);  // little-endian on every platform we ship
  f.write(reinterpret_cast<const char*>(&count), 4);
  f.write(reinterpret_cast<const char*>(&bucketSeconds), 4);
  f.write(reinterpret_cast<const char*>(peaks.data()), std::streamsize(peaks.size() * sizeof(int16_t)));
}

}  // namespace sermon
