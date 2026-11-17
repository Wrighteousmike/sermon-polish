#include "pipeline.h"

#include <vector>

#include "biquad.h"
#include "compressor.h"
#include "deesser.h"
#include "declip_spade.h"
#include "final_lowpass.h"
#include "leveler.h"
#include "limiter.h"
#include "noise_reduction.h"
#include "normalize.h"
#include "resample.h"
#include "voice_eq.h"
#include "wpe.h"

namespace sermon {

namespace {
constexpr int kInternalRate = 48000;

std::vector<float*> pointers(AudioData& a) {
  std::vector<float*> p;
  for (auto& c : a.channels) p.push_back(c.data());
  return p;
}
}  // namespace

ChainReport runChain(AudioData& audio, const ChainSettings& s, const ProgressFn& progress) {
  ChainReport report;
  if (audio.channels.empty() || audio.length() == 0) return report;
  int total = 1;  // the always-on lowpass
  if (audio.sampleRate != kInternalRate) total++;
  total += int(s.normalize) + int(s.declip) + int(s.subsonicFilter) + int(s.voiceLeveling);
  if (s.noiseReduction) total += 1 + int(noiseBackendAvailable(s.noiseBackend));
  total += int(s.dereverb.has_value()) + int(s.tone.has_value()) + int(s.deEsser.has_value()) + int(s.compressor.has_value()) + int(s.limiter);
  int step = 0;
  auto say = [&](const char* l) { if (progress) progress(l, ++step, total); };

  if (audio.sampleRate != kInternalRate) {
    say("Resampling to 48 kHz…");
    for (auto& c : audio.channels) c = resample(c.data(), c.size(), audio.sampleRate, kInternalRate);
    audio.sampleRate = kInternalRate;
  }
  const double fs = audio.sampleRate;
  const size_t n = audio.length(), nCh = audio.channels.size();
  auto ch = pointers(audio);

  if (s.normalize) { say("Setting starting volume…"); normalizePeak(ch.data(), nCh, n); }
  // Declip before anything that changes the waveform: SPADE relies on the
  // flat tops sitting at the exact clip level.
  if (s.declip) { say("Repairing clipping…"); for (auto* c : ch) declipSpade(c, n, fs); }
  if (s.subsonicFilter) {
    say("Removing rumble…");
    for (auto* c : ch) { Biquad hp1 = Biquad::highpass(fs, 40, 0.5412), hp2 = Biquad::highpass(fs, 40, 1.3066); hp1.processBlock(c, n); hp2.processBlock(c, n); }
  }
  if (s.voiceLeveling) { say("Evening out volume…"); levelVoice(ch.data(), nCh, n, fs); }

  auto denoise = [&]() {
    if (!s.noiseReduction) return;
    say("Removing hum…");
    for (auto* c : ch) removeHum(c, n, fs, *s.noiseReduction);
    if (noiseBackendAvailable(s.noiseBackend)) {
      say("Removing background noise…");
      for (auto* c : ch) reduceNoise(c, n, fs, *s.noiseReduction, s.noiseBackend);
    }
  };
  auto dereverbStage = [&]() {
    if (!s.dereverb) return;
    say("Reducing room echo…");
    for (auto* c : ch) dereverb(c, n, fs, *s.dereverb);
  };
  if (s.dereverbBeforeDenoise) { dereverbStage(); denoise(); } else { denoise(); dereverbStage(); }

  if (s.tone) { say("Shaping tone…"); for (auto* c : ch) voiceEq(c, n, fs, *s.tone); }
  if (s.deEsser) { say("Softening sharp S sounds…"); for (auto* c : ch) deEss(c, n, fs, *s.deEsser); }
  if (s.compressor) { say("Compressing…"); compress(ch.data(), nCh, n, fs, *s.compressor); }

  // The steep lowpass is the last *tonal* stage. It runs before the limiter
  // so loudness is measured on exactly what gets encoded (running it after
  // costs ~0.8 LU and can add overshoots); the limiter only applies smooth
  // gain, which doesn't bring high-frequency content back.
  say("Reducing encoder hiss…");
  for (auto* c : ch) finalLowpass(c, n, fs);
  if (s.limiter) {
    say("Matching streaming loudness…");
    const LimiterResult lr = limitToStreaming(ch.data(), nCh, n, fs, s.targetLufs, s.ceilingDb, s.limiterMaxExtraDb);
    report.limiterFinalLufs = lr.finalLufs;
    report.limiterMedianReductionDb = lr.medianReductionDb;
    report.limiterP90ReductionDb = lr.p90ReductionDb;
    report.limiterP99ReductionDb = lr.p99ReductionDb;
    report.limiterMaxReductionDb = lr.maxReductionDb;
  }
  return report;
}

}  // namespace sermon
