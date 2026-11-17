#include <filesystem>
#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "amount.h"
#include "analysis.h"
#include "compressor.h"
#include "deesser.h"
#include "final_lowpass.h"
#include "leveler.h"
#include "limiter.h"
#include "loudness.h"
#include "mp3_export.h"
#include "noise_reduction.h"
#include "trim.h"
#include "normalize.h"
#include "resample.h"
#include "voice_eq.h"
#include "wpe.h"

static int failures = 0;
#define CHECK(cond, msg)                                          \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      failures++;                                                 \
    }                                                             \
  } while (0)
#define CHECK_NEAR(a, b, tol, msg)                                                                 \
  do {                                                                                             \
    double _a = (a), _b = (b);                                                                     \
    if (!(std::fabs(_a - _b) <= (tol))) {                                                          \
      std::printf("FAIL: %s: got %.3f expected %.3f +-%.3f (%s:%d)\n", msg, _a, _b, double(tol), __FILE__, __LINE__); \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static const double kPi = 3.14159265358979323846;

static std::vector<float> sine(double fs, double freq, double seconds, double amp = 1.0, double phase = 0) {
  std::vector<float> x(size_t(fs * seconds));
  for (size_t i = 0; i < x.size(); i++) x[i] = float(amp * std::sin(2 * kPi * freq * i / fs + phase));
  return x;
}
static double rmsDb(const std::vector<float>& x, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; i++) s += double(x[i]) * x[i];
  return 10 * std::log10(s / double(to - from) + 1e-30);
}
// Steady-state gain in dB of a one-channel in-place process on a sine.
template <class F>
static double gainDb(double fs, double freq, F process) {
  auto x = sine(fs, freq, 1.0);
  const double before = rmsDb(x, size_t(fs * 0.5), x.size());
  process(x);
  return rmsDb(x, size_t(fs * 0.5), x.size()) - before;
}

static void testVoiceEq() {
  const double fs = 48000;
  auto g = [&](double f, sermon::Tone t) { return gainDb(fs, f, [&](std::vector<float>& x) { sermon::voiceEq(x.data(), x.size(), fs, t); }); };
  CHECK(g(50, sermon::Tone::Tone1) < -12, "tone1: 50 Hz rumble strongly cut by highpass");
  CHECK_NEAR(g(200, sermon::Tone::Tone1), -6, 2.5, "tone1: ~-6 dB around 200 Hz mud cut");
  CHECK_NEAR(g(3000, sermon::Tone::Tone1), -2, 1.5, "tone1: ~-2 dB brightness shelf at 3 kHz");
  CHECK(g(15000, sermon::Tone::Tone1) < -6, "tone1: top end rolled off");
  CHECK(g(3000, sermon::Tone::Tone3) < g(3000, sermon::Tone::Tone1) - 2.5, "tone3 is darker than tone1");
  CHECK(g(60, sermon::Tone::Tone2) > g(60, sermon::Tone::Tone1), "tone2 lets more low end through than tone1");
}

static void testReferenceTone() {
  // Target points read off the traced Pro-Q 3 curve (docs/protools-reference-chain.md),
  // with the fit's -1.1 dB overall offset removed.
  const double fs = 48000;
  auto g = [&](double f) { return gainDb(fs, f, [&](std::vector<float>& x) { sermon::voiceEq(x.data(), x.size(), fs, sermon::Tone::Reference); }); };
  const struct { double hz, db; } target[] = {{71, -18.3}, {107, -5.4}, {161, -5.3}, {242, -5.4}, {364, -4.0}, {547, -4.5},
                                              {822, -6.1}, {1236, -6.8}, {1858, -4.0}, {2793, -6.8}, {4198, -12.9},
                                              {6310, -8.1}, {9484, -7.8}, {14255, -13.5}};
  for (const auto& t : target) {
    char msg[64];
    std::snprintf(msg, sizeof msg, "reference EQ at %.0f Hz", t.hz);
    CHECK_NEAR(g(t.hz), t.db, 1.0, msg);
  }
  CHECK(g(40) < -40, "reference: 36 dB/oct highpass buries 40 Hz");
  CHECK(g(18000) < -25, "reference: lowpass above 15 kHz");
}

static void testFinalLowpass() {
  const double fs = 48000;
  auto g = [&](double f) { return gainDb(fs, f, [&](std::vector<float>& x) { sermon::finalLowpass(x.data(), x.size(), fs); }); };
  CHECK_NEAR(g(1000), 0, 0.05, "lowpass passband flat");
  CHECK_NEAR(g(8000), 0, 0.3, "lowpass flat well below the corner (Butterworth)");
  CHECK_NEAR(g(11000), -3.01, 0.2, "lowpass -3 dB at the 11 kHz corner");
  // Bilinear warping makes it steeper than the analog 12th-order curve near Nyquist:
  // ~-71 dB at 16 kHz when fs = 48 kHz (~-84 dB at 44.1 kHz).
  CHECK_NEAR(g(16000), -71, 3, "lowpass ~-71 dB at 16 kHz");
  CHECK(g(22000) < -65, "lowpass below -65 dB at 22 kHz");
  CHECK(g(14000) < g(12000) && g(12000) < g(11000), "lowpass response is monotonic");
}

static void testResample() {
  for (auto rates : {std::pair<int, int>{44100, 48000}, {22050, 48000}, {48000, 44100}, {48000, 22050}}) {
    const int a = rates.first, b = rates.second;
    auto x = sine(a, 1000, 2.0, 0.5);
    auto y = sermon::resample(x.data(), x.size(), a, b);
    CHECK_NEAR(double(y.size()), double(x.size()) * b / a, 2, "resample length");
    std::vector<float> mid(y.begin() + long(y.size() / 4), y.end() - long(y.size() / 4));
    CHECK_NEAR(rmsDb(mid, 0, mid.size()), 20 * std::log10(0.5 / std::sqrt(2.0)), 0.05, "resample preserves a 1 kHz tone");
  }
  // Alias rejection: a 15 kHz tone at 48k must vanish when going to 22.05k (Nyquist 11.025k).
  auto hi = sine(48000, 15000, 2.0, 0.5);
  auto y = sermon::resample(hi.data(), hi.size(), 48000, 22050);
  std::vector<float> mid(y.begin() + long(y.size() / 4), y.end() - long(y.size() / 4));
  CHECK(rmsDb(mid, 0, mid.size()) < -85, "downsampling rejects content above the new Nyquist");
}

static void testLoudness() {
  const double fs = 48000;
  auto l = sine(fs, 997, 10, std::pow(10.0, -20.0 / 20)), r = l;
  const float* ch[] = {l.data(), r.data()};
  CHECK_NEAR(sermon::integratedLufs(ch, 2, l.size(), fs), -20.0, 0.1, "stereo -20 dBFS 997 Hz = -20 LUFS");
  // Silence appended must not change integrated loudness (gating).
  auto l2 = l; l2.resize(l.size() * 2, 0.0f); auto r2 = l2;
  const float* ch2[] = {l2.data(), r2.data()};
  CHECK_NEAR(sermon::integratedLufs(ch2, 2, l2.size(), fs), -20.0, 0.15, "silence is gated out");
}

static void testTruePeak() {
  // fs/4 sine at 45 degrees: samples are +-0.7071 but the real waveform peaks at 1.0.
  const double fs = 48000;
  auto x = sine(fs, fs / 4, 1.0, 1.0, kPi / 4);
  float sp = 0; for (float v : x) sp = std::max(sp, std::fabs(v));
  const float* ch[] = {x.data()};
  const double tp = sermon::truePeak(ch, 1, x.size());
  CHECK_NEAR(sp, 0.7071, 0.001, "sample peak is 0.707");
  CHECK_NEAR(tp, 1.0, 0.02, "true peak finds the inter-sample peak");
}

static void testNormalize() {
  auto x = sine(48000, 440, 1.0, 0.1);
  float* ch[] = {x.data()};
  sermon::normalizePeak(ch, 1, x.size());
  float pk = 0; for (float v : x) pk = std::max(pk, std::fabs(v));
  CHECK_NEAR(20 * std::log10(pk), -3.0, 0.05, "normalize to -3 dBFS");
  std::vector<float> quiet(48000, 1e-7f);
  float* q[] = {quiet.data()};
  sermon::normalizePeak(q, 1, quiet.size());
  CHECK(quiet[0] == 1e-7f, "near-silent file untouched");
}

static void testCompressor() {
  const double fs = 48000;
  // 3s quiet (-30 dBFS) then 3s loud (-12 dBFS), 300 Hz tone.
  auto x = sine(fs, 300, 6, 1.0);
  for (size_t i = 0; i < x.size(); i++) x[i] *= float(i < x.size() / 2 ? std::pow(10.0, -30 / 20.0) : std::pow(10.0, -12 / 20.0));
  const double rangeIn = rmsDb(x, size_t(fs * 4), size_t(fs * 6)) - rmsDb(x, size_t(fs * 1), size_t(fs * 3));
  float* ch[] = {x.data()};
  sermon::compress(ch, 1, x.size(), fs, sermon::Intensity::Normal);
  const double rangeOut = rmsDb(x, size_t(fs * 4), size_t(fs * 6)) - rmsDb(x, size_t(fs * 1), size_t(fs * 3));
  std::printf("compressor: loud-quiet gap %.1f dB -> %.1f dB\n", rangeIn, rangeOut);
  CHECK(rangeOut < rangeIn * 0.7, "compressor narrows the loud/quiet gap by >30%");
  for (float v : x) CHECK(std::isfinite(v), "compressor output finite");
}

// Voiced "speech" (150 Hz harmonics) with sibilant noise bursts and a hissy pause, one 1 s
// cycle each. The "s" strength ramps from mild to harsh over each 10 cycles (0.08 .. 0.44), so
// the signal has a spread of sibilance like a real speaker, and the strongest are what a
// de-esser exists to catch.
static std::vector<float> sibilantSpeech(double fs, double seconds, unsigned seed = 5) {
  std::vector<float> x(size_t(fs * seconds), 0.0f);
  std::vector<float> noise(x.size());
  for (auto& v : noise) { seed = seed * 1664525u + 1013904223u; v = float(double(seed >> 8) / double(1 << 24) - 0.5); }
  // crude 5-9 kHz band noise: difference twice (high-passes) then smooth the very top off
  std::vector<float> hf(x.size(), 0.0f);
  for (size_t i = 2; i < x.size(); i++) hf[i] = noise[i] - 2 * noise[i - 1] + noise[i - 2];
  for (size_t i = 0; i < x.size(); i++) {
    const double t = double(i) / fs, ph = std::fmod(t, 1.0);
    double v = 0;
    if (ph < 0.55) {  // voiced vowel
      for (int h = 1; h <= 12; h++) v += std::sin(2 * kPi * 150.0 * h * t) / h;
      const double fadeIn = ph < 0.02 ? 0.5 - 0.5 * std::cos(kPi * ph / 0.02) : 1.0;  // onset
      x[i] = float(0.12 * v * (0.7 + 0.3 * std::sin(2 * kPi * 3 * t)) * fadeIn);
    } else if (ph < 0.70) {  // sibilant "s": strong high-frequency noise, little body
      const double amp = 0.08 + 0.04 * (int(t) % 10);
      x[i] = float(amp * hf[i] + 0.01 * std::sin(2 * kPi * 150.0 * t));
    } else if (ph < 0.85) {  // next vowel, tapering off like a real voice (an abrupt cut would be a click)
      for (int h = 1; h <= 12; h++) v += std::sin(2 * kPi * 170.0 * h * t) / h;
      const double taper = ph > 0.82 ? 0.5 + 0.5 * std::cos(kPi * (ph - 0.82) / 0.03) : 1.0;
      x[i] = float(0.10 * v * taper);
    } else {  // pause with faint hiss
      x[i] = float(0.004 * hf[i]);
    }
  }
  return x;
}

static double rmsDbRange(const std::vector<float>& x, double fs, double from, double to) {
  double s = 0; size_t n = 0;
  for (size_t i = size_t(from * fs); i < size_t(to * fs) && i < x.size(); i++) { s += double(x[i]) * x[i]; n++; }
  return 10 * std::log10(s / double(std::max<size_t>(n, 1)) + 1e-30);
}

static void testDeEsser() {
  const double fs = 48000;
  const auto dry = sibilantSpeech(fs, 22);
  // Mean level change over a slice [a,b) of the 1 s cycle: harsh cycles are strength 7-9 of each
  // 10, mild ones 0-2.
  auto change = [&](const std::vector<float>& v, double a, double b, bool harsh) {
    double acc = 0; int k = 0;
    for (int sec = 0; sec < 22; sec++) {
      const int m = sec % 10;
      if (harsh ? m < 7 : m > 2) continue;
      acc += rmsDbRange(v, fs, sec + a, sec + b) - rmsDbRange(dry, fs, sec + a, sec + b);
      k++;
    }
    return acc / k;
  };
  double prevHarsh = 0;
  for (auto intensity : {sermon::Intensity::Less, sermon::Intensity::Normal, sermon::Intensity::More}) {
    auto x = dry;
    sermon::deEss(x.data(), x.size(), fs, intensity);
    const double harsh = change(x, 0.58, 0.68, true), ordinary = change(x, 0.58, 0.68, false);
    const double voiced = change(x, 0.10, 0.50, false), pause = change(x, 0.88, 0.98, false);
    std::printf("de-esser %-6s: harsh \"s\" %+.1f dB | mild \"s\" %+.1f dB | voiced vowel %+.2f dB | hissy pause %+.2f dB\n",
                intensity == sermon::Intensity::Less ? "less" : intensity == sermon::Intensity::Normal ? "normal" : "more", harsh, ordinary, voiced, pause);
    CHECK(voiced > -0.3, "de-esser leaves voiced speech alone");
    CHECK(pause > -0.3, "de-esser leaves hiss in pauses alone");
    CHECK(harsh < -0.5, "de-esser turns a harsh sibilant down");
    CHECK(harsh < ordinary - 0.3, "harsh sibilants are cut harder than mild ones (relative to this speaker)");
    if (intensity == sermon::Intensity::More) CHECK(harsh < -1.0, "'more' takes a measurable amount off the harshest sibilants");
    CHECK(harsh <= prevHarsh + 0.2, "stronger settings cut at least as much");
    prevHarsh = harsh;
  }
  auto tooShort = sibilantSpeech(fs, 0.05);
  auto copy = tooShort;
  sermon::deEss(copy.data(), copy.size(), fs);
  CHECK(copy == tooShort, "very short input untouched");
  std::vector<float> silent(48000 * 2, 0.0f);
  sermon::deEss(silent.data(), silent.size(), fs);
  for (float v : silent) CHECK(v == 0.0f, "silence stays silent");
}

static void testLimiter() {
  const double fs = 48000;
  // Speech-like: quiet tone with loud transient bursts that will overshoot after gain-up.
  std::vector<float> x(size_t(fs * 20));
  for (size_t i = 0; i < x.size(); i++) {
    double t = i / fs, env = 0.05 + (std::fmod(t, 2.0) < 0.05 ? 0.45 : 0.0);
    x[i] = float(env * std::sin(2 * kPi * 220 * t));
  }
  auto l = x; auto r = x;
  float* ch[] = {l.data(), r.data()};
  auto res = sermon::limitToStreaming(ch, 2, l.size(), fs);
  const float* cc[] = {l.data(), r.data()};
  const double tpDb = 20 * std::log10(sermon::truePeak(cc, 2, l.size()));
  const double lufs = sermon::integratedLufs(cc, 2, l.size(), fs);
  std::printf("limiter: measured %.1f LUFS, gain %+.1f dB -> %.1f LUFS, true peak %.2f dBTP\n", res.measuredLufs, res.appliedGainDb, lufs, tpDb);
  CHECK(tpDb <= -1.0 + 0.05, "true peak held at the -1 dBTP ceiling");
  CHECK(lufs > -16.5 && lufs < -13.5, "loudness lands near -14 LUFS");
  // Already-compliant material must not be touched by the limiter stage beyond gain.
  auto q = sine(fs, 300, 5, 0.1); auto q2 = q;
  float* qc[] = {q.data(), q2.data()};
  auto rq = sermon::limitToStreaming(qc, 2, q.size(), fs);
  const float* qcc[] = {q.data(), q2.data()};
  CHECK_NEAR(sermon::integratedLufs(qcc, 2, q.size(), fs), -14.0, 0.2, "steady tone gained exactly to -14 LUFS");
  (void)rq;
}

// Latency of each denoiser backend was calibrated on real speech (cross-correlation:
// RNNoise 959 samples, DeepFilterNet3 1440). Synthetic voices correlate too weakly with the
// denoised output to place the peak reliably, so this check only runs when a real-speech file is
// supplied: SERMON_TEST_SPEECH=<raw mono float32 file at 22.05 kHz>.
static void testNoiseReductionAlignment() {
  const char* path = std::getenv("SERMON_TEST_SPEECH");
  if (!path) { std::printf("skip: noise-reduction alignment (set SERMON_TEST_SPEECH to a raw f32 22.05 kHz speech file)\n"); return; }
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  const size_t total = size_t(f.tellg()) / 4;
  if (total < 22050u * 30) { std::printf("skip: speech file too short\n"); return; }
  f.seekg(0);
  std::vector<float> all(total);
  f.read(reinterpret_cast<char*>(all.data()), std::streamsize(total * 4));
  std::vector<float> seg(all.begin() + 22050 * 10, all.begin() + 22050 * 25);
  const auto in = sermon::resample(seg.data(), seg.size(), 22050, 48000);
  for (auto backend : {sermon::NoiseBackend::RNNoise, sermon::NoiseBackend::DeepFilterNet}) {
    if (!sermon::noiseBackendAvailable(backend)) continue;
    auto out = in;
    sermon::reduceNoise(out.data(), out.size(), 48000, sermon::Intensity::More, backend);
    double best = -2; int bestLag = 999;
    for (int lag = -2500; lag <= 2500; lag++) {
      double sxy = 0, sxx = 0, syy = 0;
      for (size_t i = 20000; i < in.size() - 20000; i += 3) { double a = in[i], b = out[i + lag]; sxy += a * b; sxx += a * a; syy += b * b; }
      const double c = sxy / std::sqrt(sxx * syy + 1e-30);
      if (c > best) { best = c; bestLag = lag; }
    }
    std::printf("noise reduction (%s): output vs input lag %d samples (correlation %.3f)\n", sermon::noiseBackendName(backend), bestLag, best);
    CHECK(std::abs(bestLag) <= 1, "denoiser output is sample-aligned with its input (+-1 sample)");
    CHECK(best > 0.9, "denoiser preserves real speech (correlation > 0.9 at \"more\")");
  }
}

// Speech-like phrases (1.0s) separated by silence (1.0s), 10s at 16 kHz.
static std::vector<float> phrases(double fs, double seconds) {
  std::vector<float> x(size_t(fs * seconds), 0.0f);
  unsigned seed = 99;
  double phase = 0;
  for (size_t i = 0; i < x.size(); i++) {
    double t = i / fs;
    if (std::fmod(t, 2.0) >= 1.0) continue;
    double f0 = 120 + 35 * std::sin(2 * kPi * 0.9 * t) + 10 * std::sin(2 * kPi * 3.1 * t);
    phase += 2 * kPi * f0 / fs;
    double v = 0;
    for (int h = 1; h <= 25; h++) {
      double fh = h * f0, formant = std::exp(-std::pow((fh - 600 - 150 * std::sin(2 * kPi * 1.3 * t)) / 350, 2)) +
                                    0.7 * std::exp(-std::pow((fh - 1700) / 500, 2)) + 0.25 * std::exp(-std::pow((fh - 3000) / 700, 2));
      v += formant * std::sin(h * phase + h) / std::sqrt(double(h));
    }
    seed = seed * 1664525u + 1013904223u;
    double breath = 0.05 * (double(seed >> 8) / double(1 << 24) - 0.5);
    double env = 0.5 + 0.5 * std::sin(2 * kPi * 4.5 * t);
    x[i] = float(0.25 * (env * v + breath));
  }
  return x;
}

// Room: direct path + sparse early reflections + exponentially decaying diffuse tail.
static std::vector<float> makeReverberant(const std::vector<float>& dry, double fs, double rt60) {
  const size_t len = size_t(rt60 * fs * 1.0);
  std::vector<double> h(len, 0.0);
  unsigned seed = 7;
  h[0] = 1.0;
  for (int k = 1; k <= 6; k++) h[size_t(k * 0.0045 * fs)] += 0.5 / k;
  for (size_t i = size_t(0.02 * fs); i < len; i++) {
    seed = seed * 1664525u + 1013904223u;
    double n = double(seed >> 8) / double(1 << 24) - 0.5;
    h[i] += 0.5 * n * std::exp(-6.9078 * double(i) / (rt60 * fs));
  }
  std::vector<float> y(dry.size(), 0.0f);
  for (size_t i = 0; i < dry.size(); i++) {
    if (dry[i] == 0.0f) continue;
    const size_t m = std::min(len, dry.size() - i);
    for (size_t k = 0; k < m; k++) y[i + k] += float(double(dry[i]) * h[k]);
  }
  return y;
}

// Tail energy (0.15-0.50 s after each phrase ends) relative to speech energy, in dB.
static double tailToSpeechDb(const std::vector<float>& x, double fs) {
  double tail = 0, speech = 0; size_t nt = 0, ns = 0;
  for (double start = 0; start + 2.0 <= x.size() / fs; start += 2.0) {
    for (size_t i = size_t((start + 0.1) * fs); i < size_t((start + 0.95) * fs); i++) { speech += double(x[i]) * x[i]; ns++; }
    for (size_t i = size_t((start + 1.15) * fs); i < size_t((start + 1.5) * fs); i++) { tail += double(x[i]) * x[i]; nt++; }
  }
  return 10 * std::log10((tail / nt) / (speech / ns));
}

static void testWpeDereverb() {
  const double fs = 16000;
  auto dry = phrases(fs, 12);
  auto wet = makeReverberant(dry, fs, 0.6);
  auto out = wet;
  sermon::WpeOptions o; o.threads = 4;
  sermon::dereverbWpe(out.data(), out.size(), fs, o);
  const double before = tailToSpeechDb(wet, fs), after = tailToSpeechDb(out, fs), clean = tailToSpeechDb(dry, fs);
  std::printf("WPE: reverb tail relative to speech: dry %.1f dB | reverberant %.1f dB | after WPE %.1f dB\n", clean, before, after);
  CHECK(after < before - 2.0, "WPE removes at least 2 dB of reverb tail energy (RT60 0.6 s, default filter length)");
  for (float v : out) CHECK(std::isfinite(v), "WPE output finite");
  CHECK(out.size() == wet.size(), "WPE preserves length");
}

// The live dereverb slider mixes a correction computed once at full strength: x + strength * C. That must
// match running dereverb at that strength directly, or the preview and the export would disagree.
static void testDereverbCorrectionIsLinear() {
  const double fs = 48000;
  auto dry = phrases(fs, 10);
  auto wet = makeReverberant(dry, fs, 0.6);
  auto full = wet;
  sermon::dereverb(full.data(), full.size(), fs, sermon::Intensity::More);  // strength 1.0
  std::vector<float> corr(wet.size());
  for (size_t i = 0; i < wet.size(); i++) corr[i] = full[i] - wet[i];
  for (double a : {0.25, 0.5, 1.0}) {
    auto direct = wet;
    sermon::dereverb(direct.data(), direct.size(), fs, sermon::Intensity(a));
    const double strength = sermon::amount::dereverbStrength(sermon::Intensity(a));
    double num = 0, err = 0;
    for (size_t i = 0; i < wet.size(); i++) {
      const double mixed = double(wet[i]) + strength * corr[i], d = mixed - direct[i];
      num += double(direct[i]) * direct[i]; err += d * d;
    }
    const double snr = 10 * std::log10(num / std::max(err, 1e-30));
    std::printf("dereverb correction mix vs direct at amount %.2f (strength %.2f): %.1f dB match\n", a, strength, snr);
    CHECK(snr > 30.0, "mixing strength * the full-strength correction matches dereverb at that strength");
  }
}

static void testWpeBandSplitAt48k() {
  // Band-split path (48 kHz in, WPE at 16 kHz): dry speech passes, HF above 8 kHz is untouched.
  const double fs = 48000;
  auto dry = phrases(fs, 8);
  for (size_t i = 0; i < dry.size(); i++) dry[i] += float(0.02 * std::sin(2 * kPi * 12000 * i / fs));
  auto out = dry;
  sermon::dereverb(out.data(), out.size(), fs, sermon::Intensity::Normal);
  double num = 0, err = 0;
  for (size_t i = 0; i < dry.size(); i++) { num += double(dry[i]) * dry[i]; double d = double(dry[i]) - out[i]; err += d * d; }
  const double sdr = 10 * std::log10(num / std::max(err, 1e-20));
  std::printf("WPE band-split at 48k on dry speech: SDR %.1f dB\n", sdr);
  CHECK(sdr > 10.0, "band-split dereverb does not mangle dry speech");
  CHECK(out.size() == dry.size(), "band-split preserves length");
}

static void testWpeLeavesDrySpeechAlone() {
  const double fs = 16000;
  auto dry = phrases(fs, 8);
  auto out = dry;
  sermon::WpeOptions o; o.threads = 4;
  sermon::dereverbWpe(out.data(), out.size(), fs, o);
  double num = 0, err = 0;
  for (size_t i = 0; i < dry.size(); i++) { num += double(dry[i]) * dry[i]; double d = double(dry[i]) - out[i]; err += d * d; }
  const double sdr = 10 * std::log10(num / std::max(err, 1e-20));
  std::printf("WPE on dry speech: SDR vs input %.1f dB\n", sdr);
  CHECK(sdr > 10.0, "WPE does not mangle dry speech (SDR > 10 dB)");
}

static void testWpeBlocksAndEdges() {
  const double fs = 16000;
  auto dry = phrases(fs, 30);
  auto wet = makeReverberant(dry, fs, 0.6);
  auto one = wet, blocked = wet;
  sermon::WpeOptions a; a.blockSeconds = 60; a.threads = 4;
  sermon::WpeOptions b; b.blockSeconds = 10; b.overlapSeconds = 2; b.threads = 4;
  sermon::dereverbWpe(one.data(), one.size(), fs, a);
  sermon::dereverbWpe(blocked.data(), blocked.size(), fs, b);
  double num = 0, err = 0;
  for (size_t i = 0; i < one.size(); i++) { num += double(one[i]) * one[i]; double d = double(one[i]) - blocked[i]; err += d * d; }
  CHECK(10 * std::log10(num / std::max(err, 1e-20)) > 10.0, "blockwise processing agrees with whole-file processing");
  std::vector<float> tiny(100, 0.1f), before = tiny;
  sermon::dereverbWpe(tiny.data(), tiny.size(), fs);
  CHECK(tiny == before, "very short input untouched");
  std::vector<float> silent(fs * 3, 0.0f);
  sermon::dereverbWpe(silent.data(), silent.size(), fs);
  for (float v : silent) CHECK(v == 0.0f, "silence stays silent");
}

static void testMp3PeakLoop() {
  if (!sermon::mp3EncodingAvailable()) { std::printf("skip: LAME not built\n"); return; }
  // Loud, peaky speech-like signal at the chain's 48 kHz, already limited to -1 dBTP.
  const double fs = 48000;
  sermon::AudioData a; a.sampleRate = 48000;
  a.channels.push_back(phrases(fs, 10));
  float* p[] = {a.channels[0].data()};
  sermon::limitToStreaming(p, 1, a.channels[0].size(), fs);
  for (const auto& preset : sermon::mp3Presets()) {
    sermon::Mp3Options off; off.peakCeilingDb = std::numeric_limits<double>::quiet_NaN();
    sermon::Mp3Report r0, r1;
    sermon::encodeMp3(a, preset, {}, off, &r0);
    sermon::encodeMp3(a, preset, {}, {}, &r1);
    std::printf("mp3 %-14s decoded true peak: plain %.2f dBTP -> closed loop %.2f dBTP (%d passes)\n", preset.label, r0.decodedTruePeakDb, r1.decodedTruePeakDb, r1.passes);
    CHECK(r1.decodedTruePeakDb <= -1.0 + 0.15, "closed-loop export keeps the decoded MP3 under the ceiling");
    CHECK(r1.decodedTruePeakDb <= r0.decodedTruePeakDb + 0.01, "closed loop never makes the peak worse");
  }
}

static sermon::AudioData mono(std::vector<float> x, int rate) {
  sermon::AudioData a; a.sampleRate = rate; a.channels.push_back(std::move(x)); return a;
}

static const sermon::Suggestion* find(const std::vector<sermon::Suggestion>& v, const char* stage) {
  for (const auto& s : v) if (s.stage == stage) return &s;
  return nullptr;
}

static void testAnalysisFindsProblems() {
  const double fs = 16000;
  // Phrases over a faint noise floor.
  auto base = phrases(fs, 24);
  unsigned seed = 3;
  for (auto& v : base) { seed = seed * 1664525u + 1013904223u; v += 0.0006f * (float(seed >> 8) / float(1 << 24) - 0.5f); }

  // Clean: nothing to fix.
  {
    const auto an = sermon::analyzeAudio(mono(base, 16000));
    sermon::ChainSettings s;
    const auto sug = sermon::suggestSettings(an, s);
    CHECK(an.clippedFraction < 0.0002, "clean: no clipping found");
    CHECK(!s.declip, "clean: declip off");
    CHECK(an.humDb < 8, "clean: no hum");
    CHECK(an.snrDb > 30, "clean: healthy SNR");
    CHECK(find(sug, "dereverb") && find(sug, "dereverb")->setting == "off", "clean/dry: dereverb off");
  }
  // Mains hum in the background.
  {
    auto x = base;
    for (size_t i = 0; i < x.size(); i++) x[i] += float(0.003 * std::sin(2 * kPi * 50.0 * double(i) / fs) + 0.001 * std::sin(2 * kPi * 100.0 * double(i) / fs));
    const auto an = sermon::analyzeAudio(mono(x, 16000));
    std::printf("analysis: hum %.1f dB at %.0f Hz\n", an.humDb, an.humHz);
    CHECK(an.humDb >= 8 && std::fabs(an.humHz - 50) < 1 || std::fabs(an.humHz - 100) < 1, "hum: detected at 50/100 Hz");
    sermon::ChainSettings s;
    CHECK(find(sermon::suggestSettings(an, s), "noise") != nullptr, "hum: noise suggestion present");
  }
  // Clipping.
  {
    auto x = base;
    float mx = 0; for (float v : x) mx = std::max(mx, std::fabs(v));
    for (auto& v : x) v = std::max(-0.6f * mx, std::min(0.6f * mx, v));
    const auto an = sermon::analyzeAudio(mono(x, 16000));
    sermon::ChainSettings s; s.declip = false;
    sermon::suggestSettings(an, s);
    std::printf("analysis: clipped fraction %.4f\n", an.clippedFraction);
    CHECK(an.clippedFraction > 0.0002 && s.declip, "clipped: declip suggested");
  }
  // Reverberant room.
  {
    const auto wet = makeReverberant(base, fs, 0.9);
    const auto an = sermon::analyzeAudio(mono(wet, 16000));
    sermon::ChainSettings s;
    const auto sug = sermon::suggestSettings(an, s);
    std::printf("analysis: echo tail %.1f dB over %d pauses\n", an.reverbTailDb, an.reverbEvents);
    CHECK(s.dereverb.has_value(), "reverberant: dereverb suggested");
    CHECK(find(sug, "dereverb") && find(sug, "dereverb")->setting != "off", "reverberant: dereverb setting not off");
  }
  // Very noisy.
  {
    auto x = base;
    seed = 9;
    for (auto& v : x) { seed = seed * 1664525u + 1013904223u; v += 0.05f * (float(seed >> 8) / float(1 << 24) - 0.5f); }
    const auto an = sermon::analyzeAudio(mono(x, 16000));
    sermon::ChainSettings s;
    sermon::suggestSettings(an, s);
    std::printf("analysis: noisy SNR %.1f dB\n", an.snrDb);
    CHECK(s.noiseReduction && s.noiseReduction->amount > sermon::Intensity::Less.amount, "noisy: stronger noise reduction");
  }
  // Near-identical stereo -> mono suggestion.
  {
    sermon::AudioData st; st.sampleRate = 16000; st.channels = {base, base};
    const auto an = sermon::analyzeAudio(st);
    sermon::ChainSettings s;
    CHECK(find(sermon::suggestSettings(an, s), "export") != nullptr, "dual-mono stereo: mono export suggested");
  }
}

static void testFinePeaks() {
  // 1 s of 8 kHz audio: first half silent, then a +0.5 / -0.25 square wave. 1 ms buckets = 8 samples.
  sermon::AudioData a; a.sampleRate = 8000;
  std::vector<float> x(8000, 0.0f);
  for (size_t i = 4000; i < 8000; i++) x[i] = (i / 4) % 2 ? -0.25f : 0.5f;
  a.channels.push_back(x);
  const auto peaks = sermon::computePeaksFine(a, 8);
  CHECK(peaks.size() == 2000, "1000 buckets, a min and max each");
  CHECK(peaks[0] == 0 && peaks[1] == 0, "silent buckets are 0/0");
  CHECK_NEAR(peaks[2 * 600 + 1] / 32767.0, 0.5, 0.001, "loud bucket max");
  CHECK_NEAR(peaks[2 * 600] / 32767.0, -0.25, 0.001, "loud bucket min");
  const std::string path = (std::filesystem::temp_directory_path() / "sermon-peaks-test.bin").string();
  sermon::writePeaksFine(path, a, 1.0);
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  CHECK(size_t(f.tellg()) == 16 + 2000 * 2, "binary file is header + int16 pairs");
  f.seekg(0);
  char magic[4];
  f.read(magic, 4);
  CHECK(std::string(magic, 4) == "SPK1", "file starts with the SPK1 magic");
  std::remove(path.c_str());
}


// The macro curves: the three original presets are fixed points, everything is monotonic and
// continuous, and 0/100 are the extremes.
static void testAmountCurves() {
  using sermon::Intensity;
  namespace am = sermon::amount;
  CHECK(am::noiseAttenDb(Intensity::Less) == 10 && am::noiseAttenDb(Intensity::Normal) == 20, "noise: Less/Normal keep the original attenuation limits");
  CHECK(am::dereverbStrength(Intensity::Less) == 0.5 && am::dereverbStrength(Intensity::Normal) == 0.8 && am::dereverbStrength(Intensity::More) == 1.0, "dereverb: original strengths at the old presets");
  const auto c = am::compressor(Intensity::Less), n = am::compressor(Intensity::Normal), m = am::compressor(Intensity::More);
  CHECK(c.ratio == 4 && c.makeupDb == 0 && n.ratio == 8 && n.makeupDb == 2 && m.ratio == 12 && m.makeupDb == 4, "compressor: original ratio/makeup at the old presets");
  const auto d = am::deEss(Intensity::Normal);
  CHECK(d.percentile == 0.93 && d.ratio == 3 && d.maxReductionDb == 7, "de-esser: original settings at Normal");
  double prevAtten = -1, prevStrength = -1, prevRatio = 0, prevDeEss = -1, prevTone = -1;
  bool monotonic = true;
  for (int i = 0; i <= 100; i++) {
    const Intensity a(i / 100.0);
    const double atten = am::noiseAttenDb(a), strength = am::dereverbStrength(a), ratio = am::compressor(a).ratio, dess = am::deEss(a).maxReductionDb, tone = am::toneScale(a);
    if (atten < prevAtten || strength < prevStrength || ratio < prevRatio || dess < prevDeEss || tone < prevTone) monotonic = false;
    prevAtten = atten; prevStrength = strength; prevRatio = ratio; prevDeEss = dess; prevTone = tone;
  }
  CHECK(monotonic, "every macro only ever gets stronger as the slider goes up");
  CHECK(am::dereverbStrength(Intensity::Max) > am::dereverbStrength(Intensity::More), "dereverb: Max is past the old More");
  CHECK(am::deEss(Intensity::Max).maxReductionDb > am::deEss(Intensity::More).maxReductionDb, "de-esser: Max is past the old More");
  CHECK(am::compressor(Intensity::Max).ratio > am::compressor(Intensity::More).ratio, "compressor: Max is past the old More");
  std::optional<Intensity> out = Intensity::More;
  CHECK(sermon::parseIntensity("62", out) && out && std::fabs(out->amount - 0.62) < 1e-9, "parse: a number is a percent");
  CHECK(sermon::parseIntensity("0", out) && !out, "parse: 0 is off");
  CHECK(sermon::parseIntensity("max", out) && out && *out == Intensity::Max, "parse: max");
  CHECK(!sermon::parseIntensity("banana", out), "parse: rejects nonsense");
}

int main() {
  testAmountCurves();
  testVoiceEq();
  testReferenceTone();
  testFinalLowpass();
  testResample();
  testLoudness();
  testTruePeak();
  testNormalize();
  testCompressor();
  testDeEsser();
  testLimiter();
  testNoiseReductionAlignment();
  testMp3PeakLoop();
  testWpeDereverb();
  testWpeLeavesDrySpeechAlone();
  testWpeBandSplitAt48k();
  testDereverbCorrectionIsLinear();
  testWpeBlocksAndEdges();
  testFinePeaks();
  testAnalysisFindsProblems();
  if (failures == 0) std::printf("all stage tests passed\n");
  return failures ? 1 : 0;
}
