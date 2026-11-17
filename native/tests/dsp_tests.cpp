#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <vector>

#include "declip.h"
#include "declip_spade.h"
#include "leveler.h"
#include "loudness.h"
#include "voice_leveling.h"

static int failures = 0;
#define CHECK(cond, msg)                                         \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      failures++;                                                \
    }                                                            \
  } while (0)

static const double kPi = 3.14159265358979323846;

static double rms(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; i++) s += double(v[i]) * v[i];
  return std::sqrt(s / double(to - from));
}

static void testDeclipRepairsFlatTops() {
  std::vector<float> x(4800);
  for (size_t i = 0; i < x.size(); i++) x[i] = float(1.4 * std::sin(2 * kPi * 200 * i / 48000.0));
  std::vector<float> clipped = x;
  for (auto& s : clipped) s = std::clamp(s, -1.0f, 1.0f);
  size_t flat = 0;
  for (float s : clipped) flat += std::fabs(s) >= 0.98f;
  CHECK(flat > 0, "test signal should clip");

  sermon::declip(clipped.data(), clipped.size());
  size_t flatAfter = 0;
  for (float s : clipped) flatAfter += std::fabs(s) >= 0.98f;
  // Anchors are the samples just outside each run, so a run is replaced by a
  // curve between values below the threshold: no sample may remain pinned.
  CHECK(flatAfter == 0, "no samples left at/above threshold after declip");
  for (float s : clipped) CHECK(std::fabs(s) <= 1.0f, "declip stays within full scale");
}

static void testDeclipLeavesLongRunsAndEdges() {
  std::vector<float> x(1000, 0.0f);
  for (size_t i = 100; i < 100 + 300; i++) x[i] = 1.0f;  // 300 > maxRun(200) for Normal
  x[0] = 1.0f;                                          // run touching the start: no left anchor
  std::vector<float> before = x;
  sermon::declip(x.data(), x.size());
  CHECK(x == before, "over-long runs and edge runs are untouched");
}

static void testDeclipIgnoresSingleSample() {
  std::vector<float> x(100, 0.1f);
  x[50] = 1.0f;
  std::vector<float> before = x;
  sermon::declip(x.data(), x.size());
  CHECK(x == before, "single loud sample (run < 2) is untouched");
}

static void testLevelerCorrectsFaderPush() {
  const double sr = 48000;
  const size_t n = size_t(sr * 20);
  std::vector<float> in(n), out(n);
  // Quiet (-30 dB RMS-ish) first half, +15 dB louder second half.
  for (size_t i = 0; i < n; i++) {
    double amp = (i < n / 2) ? 0.0316 : 0.0316 * std::pow(10.0, 15.0 / 20);
    in[i] = float(amp * std::sqrt(2.0) * std::sin(2 * kPi * 220 * i / sr));
  }
  sermon::voiceLeveling(in.data(), out.data(), n, sr);
  const double targetRms = std::pow(10.0, -20.0 / 20);
  double early = rms(out, size_t(sr * 8), size_t(sr * 10));
  double late = rms(out, size_t(sr * 18), size_t(sr * 20));
  CHECK(std::fabs(20 * std::log10(early / targetRms)) < 1.0, "first half converges to -20 dB RMS");
  CHECK(std::fabs(20 * std::log10(late / targetRms)) < 1.0, "second half converges to -20 dB RMS");
}

static void testLevelerSilenceAndBounds() {
  std::vector<float> in(48000, 0.0f), out(48000, 1.0f);
  sermon::voiceLeveling(in.data(), out.data(), in.size(), 48000);
  for (float s : out) CHECK(s == 0.0f, "silence stays silent");

  // Very quiet input: gain must cap at +18 dB.
  std::vector<float> quiet(48000 * 5), q(48000 * 5);
  for (size_t i = 0; i < quiet.size(); i++) quiet[i] = float(0.0005 * std::sin(2 * kPi * 220 * i / 48000.0));
  sermon::voiceLeveling(quiet.data(), q.data(), quiet.size(), 48000);
  double gainDb = 20 * std::log10(rms(q, 48000 * 4, 48000 * 5) / rms(quiet, 48000 * 4, 48000 * 5));
  CHECK(gainDb <= 18.05, "boost capped at +18 dB");
}

// Speech-like test signal: harmonic tone with 4 Hz syllable modulation,
// phrases of 1.5s separated by 0.7s pauses holding quiet room tone.
// `gainDbAt(t)` models a fader.
template <class F>
static std::vector<float> fakeSpeech(double sr, double seconds, double peakAmp, F gainDbAt, unsigned seed = 1) {
  std::srand(seed);
  std::vector<float> x(size_t(sr * seconds));
  for (size_t i = 0; i < x.size(); i++) {
    double t = i / sr;
    double phrase = std::fmod(t, 2.2) < 1.5 ? 1.0 : 0.0;
    double syl = 0.6 + 0.4 * std::sin(2 * kPi * 4 * t);
    double voice = phrase * syl * (std::sin(2 * kPi * 140 * t) + 0.5 * std::sin(2 * kPi * 420 * t) +
                                   0.3 * std::sin(2 * kPi * 1100 * t)) / 1.8;
    double room = 0.003 * ((std::rand() / double(RAND_MAX)) - 0.5);
    x[i] = float((peakAmp * voice + room) * std::pow(10.0, gainDbAt(t) / 20));
  }
  return x;
}

static double speechRmsDb(const std::vector<float>& x, double sr, double from, double to) {
  // RMS over only the loud (phrase) samples in [from,to).
  double s = 0; size_t n = 0;
  for (size_t i = size_t(from * sr); i < size_t(to * sr); i++) {
    if (std::fabs(x[i]) > 0.02) { s += double(x[i]) * x[i]; n++; }
  }
  return 10 * std::log10(s / double(n));
}

static void testKWeightingCalibration() {
  // BS.1770: a 0 dBFS 997 Hz sine in one channel measures -3.01 LUFS.
  const double sr = 48000;
  std::vector<float> x(size_t(sr * 2));
  for (size_t i = 0; i < x.size(); i++) x[i] = float(std::sin(2 * kPi * 997 * i / sr));
  const float* ch[] = {x.data()};
  auto e = sermon::kWeightedHopEnergy(ch, 1, x.size(), sr, 4800);
  double mean = 0; for (size_t i = 2; i < e.size(); i++) mean += e[i];
  mean /= double(e.size() - 2);
  double lufs = sermon::energyToLufs(mean);
  CHECK(std::fabs(lufs - (-3.01)) < 0.05, "997Hz 0dBFS sine measures -3.01 LUFS");
  // Same at 44.1k: coefficients must be rate-correct.
  const double sr2 = 44100;
  std::vector<float> y(size_t(sr2 * 2));
  for (size_t i = 0; i < y.size(); i++) y[i] = float(std::sin(2 * kPi * 997 * i / sr2));
  const float* ch2[] = {y.data()};
  auto e2 = sermon::kWeightedHopEnergy(ch2, 1, y.size(), sr2, 4410);
  double m2 = 0; for (size_t i = 2; i < e2.size(); i++) m2 += e2[i];
  m2 /= double(e2.size() - 2);
  CHECK(std::fabs(sermon::energyToLufs(m2) - (-3.01)) < 0.05, "calibration holds at 44.1 kHz");
}

static void testNewLevelerFaderPush() {
  const double sr = 44100;
  // 60s; fader pushed +16 dB at t=30 and left there.
  auto x = fakeSpeech(sr, 60, 0.05, [](double t) { return t < 30 ? 0.0 : 16.0; });
  float* ch[] = {x.data()};
  sermon::levelVoice(ch, 1, x.size(), sr);
  double before = speechRmsDb(x, sr, 10, 25);
  double after = speechRmsDb(x, sr, 35, 55);
  CHECK(std::fabs(before - after) < 1.0, "fader push evened out to within 1 dB");
}

static void testNewLevelerNoLagAtStep() {
  const double sr = 44100;
  auto orig = fakeSpeech(sr, 40, 0.05, [](double t) { return t < 20 ? 0.0 : 16.0; });
  auto x = orig;
  float* ch[] = {x.data()};
  sermon::levelVoice(ch, 1, x.size(), sr);
  // Speech in the first phrase after the step (t=22.0-23.5) must already be corrected.
  double early = speechRmsDb(x, sr, 22.0, 23.5);
  double ref = speechRmsDb(x, sr, 10, 18);
  CHECK(std::fabs(early - ref) < 1.5, "step corrected immediately (lookahead, no detection lag)");
}

static void testNewLevelerLeavesPausesAlone() {
  const double sr = 44100;
  auto orig = fakeSpeech(sr, 30, 0.05, [](double) { return 0.0; });
  auto x = orig;
  float* ch[] = {x.data()};
  sermon::levelVoice(ch, 1, x.size(), sr);
  // Steady material: gain should be ~constant, so room tone in a pause is not pumped up.
  double maxRatioDb = 0;
  for (size_t i = size_t(sr * 5); i < size_t(sr * 25); i++)
    if (std::fabs(orig[i]) > 1e-4) maxRatioDb = std::max(maxRatioDb, std::fabs(20.0 * std::log10(std::fabs(double(x[i]) / orig[i]))));
  CHECK(maxRatioDb < 1.0, "steady speech with pauses: gain stays within 1 dB (pauses not boosted)");
}

static void testNewLevelerSmoothAndStereoLinked() {
  const double sr = 44100;
  auto l = fakeSpeech(sr, 40, 0.05, [](double t) { return t < 20 ? 0.0 : 16.0; });
  std::vector<float> r(l.size());
  for (size_t i = 0; i < l.size(); i++) r[i] = 0.5f * l[i];  // R is half of L
  auto l0 = l, r0 = r;
  float* ch[] = {l.data(), r.data()};
  sermon::levelVoice(ch, 2, l.size(), sr);
  // Same gain on both channels -> the 2:1 relationship must be exactly preserved.
  double worst = 0;
  for (size_t i = 0; i < l.size(); i++) worst = std::max(worst, std::fabs(double(r[i]) - 0.5 * l[i]));
  CHECK(worst < 1e-6, "stereo channels share one gain (image preserved)");
  // Gain slew: max change per second (in dB) while the step is corrected.
  double maxSlew = 0;
  const size_t w = size_t(sr * 0.1);
  for (size_t i = w; i + w < l.size(); i += w) {
    double a = 0, b = 0, na = 0, nb = 0;
    for (size_t k = 0; k < w; k++) {
      if (std::fabs(l0[i - w + k]) > 0.02) { a += std::fabs(l[i - w + k] / l0[i - w + k]); na++; }
      if (std::fabs(l0[i + k]) > 0.02) { b += std::fabs(l[i + k] / l0[i + k]); nb++; }
    }
    if (na > 100 && nb > 100) maxSlew = std::max(maxSlew, std::fabs(20 * std::log10((b / nb) / (a / na))) / 0.1);
  }
  CHECK(maxSlew < 30.0, "gain moves smoothly (<30 dB/s)");
}

static void testNewLevelerEdgeCases() {
  const double sr = 44100;
  std::vector<float> silent(size_t(sr * 5), 0.0f);
  float* c1[] = {silent.data()};
  sermon::levelVoice(c1, 1, silent.size(), sr);
  for (float s : silent) CHECK(s == 0.0f, "silence stays silent");
  std::vector<float> tiny(1000, 0.1f), before = tiny;
  float* c2[] = {tiny.data()};
  sermon::levelVoice(c2, 1, tiny.size(), sr);
  CHECK(tiny == before, "file shorter than one block is untouched");
  // Very quiet speech: boost capped at +18 dB.
  auto q = fakeSpeech(sr, 20, 0.0005, [](double) { return 0.0; });
  auto q0 = q;
  sermon::LevelerOptions o; o.targetLufs = -20;
  float* c3[] = {q.data()};
  sermon::levelVoice(c3, 1, q.size(), sr, o);
  double worst = 0;
  for (size_t i = size_t(sr * 5); i < size_t(sr * 15); i++)
    if (std::fabs(q0[i]) > 1e-4) worst = std::max(worst, 20.0 * std::log10(std::fabs(double(q[i]) / q0[i])));
  CHECK(worst <= 18.05, "boost capped at +18 dB");
}

// Voiced-speech-like signal: gliding pitch, decaying harmonics, phrase envelope, light noise.
static std::vector<float> voicedSpeech(double sr, double seconds, double peak, unsigned seed = 7) {
  std::srand(seed);
  std::vector<float> x(size_t(sr * seconds));
  double phase = 0;
  for (size_t i = 0; i < x.size(); i++) {
    double t = i / sr;
    double f0 = 120 + 40 * std::sin(2 * kPi * 0.7 * t);
    phase += 2 * kPi * f0 / sr;
    double env = 0.55 + 0.45 * std::sin(2 * kPi * 0.9 * t + 1.0);
    double v = 0;
    for (int h = 1; h <= 20; h++) v += std::sin(h * phase + 0.3 * h) / (h * 0.8);
    x[i] = float(env * v + 0.01 * ((std::rand() / double(RAND_MAX)) - 0.5));
  }
  float mx = 0; for (float s : x) mx = std::max(mx, std::fabs(s));
  for (auto& s : x) s = s / mx * float(peak);
  return x;
}

static double sdrDb(const std::vector<float>& ref, const std::vector<float>& est) {
  double num = 0, err = 0;
  for (size_t i = 0; i < ref.size(); i++) { num += double(ref[i]) * ref[i]; double d = double(ref[i]) - est[i]; err += d * d; }
  return 10 * std::log10(num / std::max(err, 1e-20));
}

static void testSpadeBeatsSmoothstep() {
  const double sr = 44100;
  auto clean = voicedSpeech(sr, 4, 1.0);
  std::vector<float> clipped = clean;
  size_t nClipped = 0;
  for (auto& s : clipped) { float c = std::clamp(s, -0.8f, 0.8f); nClipped += c != s; s = c; }
  CHECK(nClipped > 500, "test signal is meaningfully clipped");

  // The smoothstep declipper only sees levels near full scale, so give it
  // the same signal scaled so the clip level sits at 0.99.
  auto old = clipped;
  { const float g = 0.99f / 0.8f; for (auto& s : old) s *= g; sermon::declip(old.data(), old.size()); for (auto& s : old) s /= g; }
  auto sp = clipped;
  auto stats = sermon::declipSpade(sp.data(), sp.size(), sr);

  double in = sdrDb(clean, clipped), o = sdrDb(clean, old), n = sdrDb(clean, sp);
  std::printf("light clipping SDR vs clean: clipped %.1f dB | smoothstep %.1f dB | SPADE %.1f dB (%zu clipped samples, %zu frames)\n",
              in, o, n, nClipped, stats.framesProcessed);
  CHECK(stats.clippedSamples > 0 && stats.thresholdPos > 0.75, "detects clip level automatically");
  CHECK(n > in + 10.0, "SPADE improves SDR by >10 dB on light clipping");
  CHECK(n > o + 10.0, "SPADE beats the smoothstep declipper by >10 dB");
}

static void testSpadeHeavyClippingDoesNoHarm() {
  const double sr = 44100;
  auto clean = voicedSpeech(sr, 4, 1.0);
  std::vector<float> clipped = clean;
  for (auto& s : clipped) s = std::clamp(s, -0.4f, 0.4f);  // ~20%+ of samples clipped
  auto sp = clipped;
  auto stats = sermon::declipSpade(sp.data(), sp.size(), sr);
  double in = sdrDb(clean, clipped), n = sdrDb(clean, sp);
  std::printf("heavy clipping SDR: clipped %.1f dB -> SPADE %.1f dB (%zu rejected, %zu skipped)\n", in, n,
              stats.framesRejected, stats.framesSkipped);
  CHECK(n >= in - 0.1, "heavily clipped audio is not made worse");
}

static void testSpadeOnlyTouchesClippedSamples() {
  const double sr = 44100;
  auto clean = voicedSpeech(sr, 2, 1.0);
  std::vector<float> clipped = clean;
  for (auto& s : clipped) s = std::clamp(s, -0.85f, 0.85f);
  auto out = clipped;
  sermon::declipSpade(out.data(), out.size(), sr);
  size_t changedReliable = 0, violations = 0, changedClipped = 0;
  for (size_t i = 0; i < out.size(); i++) {
    bool wasClipped = std::fabs(clipped[i]) >= 0.85f * 0.999f;
    if (!wasClipped && out[i] != clipped[i]) changedReliable++;
    if (wasClipped && out[i] != clipped[i]) changedClipped++;
    if (clipped[i] >= 0.85f * 0.999f && out[i] < clipped[i]) violations++;
    if (clipped[i] <= -0.85f * 0.999f && out[i] > clipped[i]) violations++;
  }
  CHECK(changedClipped > 0, "clipped samples were repaired");
  CHECK(changedReliable == 0, "unclipped samples are bit-identical");
  CHECK(violations == 0, "repaired samples stay beyond the clip level");
}

static void testSpadeNoClippingNoChange() {
  const double sr = 44100;
  auto x = voicedSpeech(sr, 2, 0.8);
  auto before = x;
  auto stats = sermon::declipSpade(x.data(), x.size(), sr);
  CHECK(x == before && stats.framesProcessed == 0, "clean audio is left untouched");
  std::vector<float> silent(10000, 0.0f);
  sermon::declipSpade(silent.data(), silent.size(), sr);
  std::vector<float> tiny(3, 1.0f);
  sermon::declipSpade(tiny.data(), tiny.size(), sr);
}

static void testSpadeAsymmetricAndLowRate() {
  for (double sr : {44100.0, 22050.0}) {
    auto clean = voicedSpeech(sr, 3, 1.0);
    std::vector<float> clipped = clean;
    for (auto& s : clipped) s = std::clamp(s, -0.75f, 0.85f);  // different levels per side
    auto out = clipped;
    auto stats = sermon::declipSpade(out.data(), out.size(), sr);
    CHECK(stats.thresholdPos > 0.8 && stats.thresholdNeg < -0.7, "detects separate positive and negative clip levels");
    CHECK(sdrDb(clean, out) > sdrDb(clean, clipped) + 5.0, "asymmetric clipping improves");
    for (float s : out) CHECK(std::isfinite(s), "output is finite");
  }
}

int main() {
  testDeclipRepairsFlatTops();
  testDeclipLeavesLongRunsAndEdges();
  testDeclipIgnoresSingleSample();
  testLevelerCorrectsFaderPush();
  testLevelerSilenceAndBounds();
  testKWeightingCalibration();
  testNewLevelerFaderPush();
  testNewLevelerNoLagAtStep();
  testNewLevelerLeavesPausesAlone();
  testNewLevelerSmoothAndStereoLinked();
  testNewLevelerEdgeCases();
  testSpadeBeatsSmoothstep();
  testSpadeHeavyClippingDoesNoHarm();
  testSpadeOnlyTouchesClippedSamples();
  testSpadeNoClippingNoChange();
  testSpadeAsymmetricAndLowRate();
  if (failures == 0) std::printf("all tests passed\n");
  return failures ? 1 : 0;
}
