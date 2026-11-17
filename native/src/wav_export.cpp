#include "wav_export.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "loudness.h"
#include "resample.h"

namespace sermon {

double exportWavMono16(const AudioData& audio, const std::string& path, int sampleRate, double ceilingDb) {
  AudioData out;
  out.sampleRate = sampleRate;
  std::vector<float> mono;
  for (const auto& c : audio.channels) {
    std::vector<float> r = resample(c.data(), c.size(), audio.sampleRate, sampleRate);
    if (mono.empty()) mono.assign(r.size(), 0.0f);
    const float w = 1.0f / float(audio.channels.size());
    for (size_t i = 0; i < mono.size() && i < r.size(); i++) mono[i] += r[i] * w;
  }
  out.channels.push_back(std::move(mono));

  auto peakDb = [&] {
    const float* p = out.channels[0].data();
    return 20.0 * std::log10(std::max(truePeak(&p, 1, out.length()), 1e-9));
  };
  double tp = peakDb();
  if (tp > ceilingDb) {
    const float g = float(std::pow(10.0, (ceilingDb - tp) / 20.0));
    for (auto& v : out.channels[0]) v *= g;
    tp = ceilingDb;
  }
  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> u(-0.5f, 0.5f);
  for (auto& v : out.channels[0]) v += (u(rng) + u(rng)) / 32767.0f;  // TPDF dither, +-1 LSB
  writeWav(path, out, 16);
  return tp;
}

}  // namespace sermon
