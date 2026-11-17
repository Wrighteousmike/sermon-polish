#include <filesystem>
// Tests for the block-based (real-time) engine: every streaming stage against its offline counterpart,
// and the properties a live engine must have (block-size independence, click-free bypass, alignment).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "../src/compressor.h"
#include "../src/deesser.h"
#include "../src/final_lowpass.h"
#include "../src/leveler.h"
#include "../src/limiter.h"
#include "../src/loudness.h"
#include "../src/noise_reduction.h"
#include "../src/resample.h"
#include "../src/rt/chain.h"
#include "../src/mp3_export.h"
#include "../src/rt/workfile.h"
#include "../src/voice_eq.h"

using namespace sermon;

static int failures = 0;
#define CHECK(cond, msg)                                           \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
      failures++;                                                  \
    }                                                              \
  } while (0)

static const double kPi = 3.14159265358979323846;

// Speech-like test audio: voiced phrases with formants, sibilant bursts of varying strength, pauses
// with a noise floor, and a level step part-way (a "fader push") so the leveler has work to do.
static AudioData makeSpeech(double fs, double seconds, int channels, unsigned seed = 11) {
  AudioData a;
  a.sampleRate = int(fs);
  a.channels.assign(size_t(channels), std::vector<float>(size_t(fs * seconds), 0.0f));
  unsigned s = seed;
  auto rnd = [&]() { s = s * 1664525u + 1013904223u; return double(s >> 8) / double(1 << 24) - 0.5; };
  std::vector<double> hf(a.channels[0].size());
  double n1 = 0, n2 = 0;
  for (auto& v : hf) { const double n = rnd(); v = n - 2 * n1 + n2; n2 = n1; n1 = n; }
  for (int c = 0; c < channels; c++) {
    double phase = 0;
    for (size_t i = 0; i < a.channels[0].size(); i++) {
      const double t = double(i) / fs, cyc = std::fmod(t, 1.4);
      const double f0 = (110 + 25 * std::sin(2 * kPi * 0.7 * t)) * (c ? 1.07 : 1.0);
      phase += 2 * kPi * f0 / fs;
      double v = 0;
      if (cyc < 0.7) {
        for (int h = 1; h <= 20; h++) {
          const double fh = h * f0;
          const double form = std::exp(-std::pow((fh - 650) / 350, 2)) + 0.6 * std::exp(-std::pow((fh - 1700) / 500, 2)) + 0.25 * std::exp(-std::pow((fh - 3000) / 700, 2));
          v += form * std::sin(h * phase) / std::sqrt(double(h));
        }
        v *= 0.18 * (0.6 + 0.4 * std::sin(2 * kPi * 4 * t)) * std::min(1.0, cyc / 0.02);
      } else if (cyc < 0.85) {
        v = (0.05 + 0.06 * (int(t / 1.4) % 5)) * hf[i];  // sibilant burst of varying strength
      }
      v += 0.003 * (c ? rnd() : -rnd());
      v *= t > seconds * 0.5 ? 2.2 : 1.0;  // level step
      a.channels[size_t(c)][i] = float(v);
    }
  }
  return a;
}

// Runs one stage over the audio in blocks, compensating its latency (the result has the input's length).
static AudioData runStage(rt::Processor& p, const AudioData& in, int block) {
  const int nCh = int(in.channels.size());
  const int64_t n = int64_t(in.length());
  p.prepare(in.sampleRate, nCh);
  const int L = p.latency();
  AudioData out;
  out.sampleRate = in.sampleRate;
  out.channels.assign(size_t(nCh), std::vector<float>(size_t(n), 0.0f));
  std::vector<std::vector<float>> buf;
  buf.assign(size_t(nCh), std::vector<float>());
  std::vector<float*> ptr;
  for (auto& b : buf) { b.resize(size_t(block)); ptr.push_back(b.data()); }
  for (int64_t pos = 0; pos < n + L; pos += block) {
    for (int c = 0; c < nCh; c++)
      for (int i = 0; i < block; i++) buf[size_t(c)][size_t(i)] = pos + i < n ? in.channels[size_t(c)][size_t(pos + i)] : 0.0f;
    p.process(ptr.data(), block, pos);
    for (int c = 0; c < nCh; c++)
      for (int i = 0; i < block; i++) {
        const int64_t o = pos + i - L;
        if (o >= 0 && o < n) out.channels[size_t(c)][size_t(o)] = buf[size_t(c)][size_t(i)];
      }
  }
  return out;
}

static double sdr(const AudioData& ref, const AudioData& test, double skipSeconds = 0) {
  double num = 0, err = 0;
  const size_t from = size_t(skipSeconds * ref.sampleRate), to = ref.length() - from;
  for (size_t c = 0; c < ref.channels.size(); c++)
    for (size_t i = from; i < to; i++) {
      const double x = ref.channels[c][i], y = test.channels[c][i];
      num += x * x;
      err += (x - y) * (x - y);
    }
  return 10 * std::log10(num / std::max(err, 1e-30));
}

static double maxDiff(const AudioData& a, const AudioData& b) {
  double m = 0;
  for (size_t c = 0; c < a.channels.size(); c++)
    for (size_t i = 0; i < a.length(); i++) m = std::max(m, std::fabs(double(a.channels[c][i]) - b.channels[c][i]));
  return m;
}

static std::vector<float*> ptrs(AudioData& a) {
  std::vector<float*> p;
  for (auto& c : a.channels) p.push_back(c.data());
  return p;
}

static void testStagesMatchOffline() {
  const double fs = 48000;
  const AudioData x = makeSpeech(fs, 14, 2);

  { rt::ToneEq st; st.setTone(Tone::Tone3);
    AudioData ref = x; auto p = ptrs(ref); for (auto* c : p) voiceEq(c, ref.length(), fs, Tone::Tone3);
    const double v = sdr(ref, runStage(st, x, 2048)); std::printf("EQ (tone 3):        SDR vs offline %.1f dB\n", v); CHECK(v > 100, "streaming EQ matches offline"); }
  { rt::ToneEq st; st.setTone(Tone::Reference);
    AudioData ref = x; auto p = ptrs(ref); for (auto* c : p) voiceEq(c, ref.length(), fs, Tone::Reference);
    const double v = sdr(ref, runStage(st, x, 480)); std::printf("EQ (reference):     SDR vs offline %.1f dB\n", v); CHECK(v > 100, "streaming reference EQ matches offline"); }
  { rt::FinalLowpassStage st;
    AudioData ref = x; auto p = ptrs(ref); for (auto* c : p) finalLowpass(c, ref.length(), fs);
    const double v = sdr(ref, runStage(st, x, 1000)); std::printf("lowpass:            SDR vs offline %.1f dB\n", v); CHECK(v > 100, "streaming lowpass matches offline"); }
  { rt::HumNotch st;
    AudioData ref = x; auto p = ptrs(ref); for (auto* c : p) removeHum(c, ref.length(), fs, Intensity::Normal);
    const double v = sdr(ref, runStage(st, x, 333)); std::printf("hum notches:        SDR vs offline %.1f dB\n", v); CHECK(v > 100, "streaming hum notches match offline"); }
  { // compressor: same threshold the offline version derives
    AudioData ref = x; auto p = ptrs(ref);
    std::vector<const float*> cp(p.begin(), p.end());
    const double lufs = integratedLufs(cp.data(), cp.size(), ref.length(), fs);
    compress(p.data(), p.size(), ref.length(), fs, Intensity::Normal);
    rt::CompressorStage st; st.setParams(std::min(-10.0, std::max(-40.0, lufs - 6.0)), 8, 2);
    const double v = sdr(ref, runStage(st, x, 2048)); std::printf("compressor:         SDR vs offline %.1f dB\n", v); CHECK(v > 100, "streaming compressor matches offline"); }
  { // leveler: plan from the same hop energies
    AudioData ref = x; auto p = ptrs(ref); levelVoice(p.data(), p.size(), ref.length(), fs);
    std::vector<const float*> cp = {x.channels[0].data(), x.channels[1].data()};
    const size_t hop = size_t(0.1 * fs);
    auto lane = std::make_shared<LevelerLane>(planLevelerLane(kWeightedHopEnergy(cp.data(), 2, x.length(), fs, hop), hop));
    rt::LevelerStage st; st.setLane(lane);
    const double v = sdr(ref, runStage(st, x, 4096)); std::printf("leveler:            SDR vs offline %.1f dB\n", v); CHECK(v > 100, "streaming leveler matches offline"); }
  { // de-esser: stats from the same audio
    rt::DeEssStatsBuilder b(fs);
    std::vector<float> mono(x.length());
    for (size_t i = 0; i < mono.size(); i++) mono[i] = x.channels[0][i];
    // the offline function analyses each channel on its own, so compare channel 0 alone
    AudioData one; one.sampleRate = x.sampleRate; one.channels = {x.channels[0]};
    AudioData ref = one; deEss(ref.channels[0].data(), ref.length(), fs, Intensity::More);
    b.feed(mono.data(), mono.size());
    rt::DeEsserStage st; st.setStats(b.finish()); st.setIntensity(Intensity::More);
    const double v = sdr(ref, runStage(st, one, 2048), 0.3);
    std::printf("de-esser:           SDR vs offline %.1f dB (edges skipped)\n", v); CHECK(v > 60, "streaming de-esser matches offline"); }
  { // limiter at a fixed +12 dB input gain
    AudioData ref = x; auto p = ptrs(ref);
    for (auto& c : ref.channels) for (auto& v : c) v *= 3.98107f;
    enforceTruePeak(p.data(), p.size(), ref.length(), fs, -1.0);
    rt::LimiterStage st; st.setParams(12.0, -1.0);
    st.prepare(fs, 2);
    st.reset();
    const double v = sdr(ref, runStage(st, x, 2048), 0.1);
    std::printf("limiter (+12 dB):   SDR vs offline %.1f dB\n", v); CHECK(v > 50, "streaming limiter matches offline"); }
  if (noiseBackendAvailable(NoiseBackend::DeepFilterNet)) {
    AudioData one; one.sampleRate = x.sampleRate; one.channels = {x.channels[0]};
    AudioData ref = one; reduceNoise(ref.channels[0].data(), ref.length(), fs, Intensity::Normal, NoiseBackend::DeepFilterNet);
    rt::NoiseStage st; st.setBackend(NoiseBackend::DeepFilterNet); st.setIntensity(Intensity::Normal);
    const double v = sdr(ref, runStage(st, one, 2048), 0.5);
    std::printf("noise (DeepFilter): SDR vs offline %.1f dB\n", v); CHECK(v > 60, "streaming noise reduction matches offline"); }
}

static AudioData render(rt::Chain& chain, const AudioData& a, int block) {
  rt::MemorySource src(a);
  AudioData out;
  out.sampleRate = a.sampleRate;
  out.channels.assign(a.channels.size(), {});
  chain.reset();
  rt::renderChain(chain, src, block, [&](const float* const* ch, int n) {
    for (size_t c = 0; c < out.channels.size(); c++) out.channels[c].insert(out.channels[c].end(), ch[c], ch[c] + n);
  });
  return out;
}

static void testChainProperties() {
  const double fs = 48000;
  const AudioData x = makeSpeech(fs, 12, 2);
  rt::MemorySource src(x);
  auto model = rt::buildFileModel(src, Tone::Tone1);
  std::printf("model: raw %.1f LUFS, compressor reference %.1f LUFS, %zu leveler blocks, %zu sibilance slices\n", model->rawLufs,
              model->compressorRefLufs, model->lane->gainDb.size(), model->deess->sortedRatiosDb.size());
  CHECK(model->lane && !model->lane->empty(), "model has a leveler lane");
  CHECK(model->deess && model->deess->valid(), "model has sibilance statistics");

  rt::Chain chain;
  chain.prepare(fs, 2);
  chain.setModel(model);
  rt::ChainParams params;
  params.outputGainDb = 6;
  chain.setParams(params);

  // 1. the result doesn't depend on the block size
  const AudioData ref = render(chain, x, 2048);
  CHECK(ref.length() == x.length(), "render returns exactly the input length");
  for (int block : {64, 480, 1000, 4096, 7}) {
    const AudioData other = render(chain, x, block);
    const double d = maxDiff(ref, other);
    std::printf("block size %5d: max difference vs 2048 = %.3g\n", block, d);
    CHECK(d < 1e-5, "output is independent of block size");
  }

  // 2. everything off is a bit-exact, perfectly aligned copy of the input
  rt::ChainParams off;
  off.rumble = false; off.leveling = false; off.noise.reset(); off.tone.reset(); off.deess.reset(); off.compressor.reset();
  off.lowpass = false; off.limiter = false;
  chain.setParams(off);
  const AudioData bypass = render(chain, x, 2048);
  const double bd = maxDiff(x, bypass);
  std::printf("all stages off: max difference from the input = %.3g (latency %d samples compensated)\n", bd, chain.latency());
  CHECK(bd < 1e-6, "with everything off the chain is transparent and time-aligned");

  // 3. switching a stage off mid-stream is click-free
  chain.setParams(params);
  chain.reset();
  AudioData out;
  out.sampleRate = x.sampleRate;
  out.channels.assign(2, {});
  rt::MemorySource src2(x);
  int blocks = 0;
  const int toggleAt = int(6 * fs / 2048);
  rt::ChainParams noComp = params; noComp.compressor.reset(); noComp.deess.reset(); noComp.tone.reset();
  rt::renderChain(chain, src2, 2048, [&](const float* const* ch, int n) {
    if (blocks == toggleAt) chain.setParams(noComp);
    for (size_t c = 0; c < 2; c++) out.channels[c].insert(out.channels[c].end(), ch[c], ch[c] + n);
    blocks++;
  });
  auto maxJump = [&](size_t from, size_t to) { double m = 0; for (size_t i = from + 1; i < to; i++) m = std::max(m, std::fabs(double(out.channels[0][i]) - out.channels[0][i - 1])); return m; };
  const size_t at = size_t(toggleAt) * 2048 + size_t(chain.latency());
  const double around = maxJump(at - 300, at + 3000), normal = maxJump(size_t(3 * fs), size_t(5 * fs));
  std::printf("toggling three stages off mid-stream: biggest sample jump %.4f (ordinary speech: %.4f)\n", around, normal);
  CHECK(around < 2.5 * normal + 0.02, "switching stages off doesn't click");
}

static void testStreamResampler() {
  for (auto rates : {std::pair<int, int>{22050, 48000}, {44100, 48000}, {48000, 48000}, {96000, 48000}}) {
    const AudioData x = makeSpeech(rates.first, 6, 1);
    const std::vector<float> ref = resample(x.channels[0].data(), x.length(), rates.first, rates.second);
    rt::StreamResampler r(rates.first, rates.second);
    std::vector<float> got;
    for (size_t pos = 0; pos < x.length(); pos += 4099) {  // awkward chunk size on purpose
      auto o = r.push(x.channels[0].data() + pos, std::min<size_t>(4099, x.length() - pos));
      got.insert(got.end(), o.begin(), o.end());
    }
    auto tail = r.finish();
    got.insert(got.end(), tail.begin(), tail.end());
    double err = 0;
    for (size_t i = 0; i < std::min(ref.size(), got.size()); i++) err = std::max(err, std::fabs(double(ref[i]) - got[i]));
    std::printf("stream resampler %5d -> %d: lengths %zu vs %zu, max diff %.2g\n", rates.first, rates.second, ref.size(), got.size(), err);
    CHECK(ref.size() == got.size(), "streaming resampler returns the same number of samples");
    CHECK(err < 1e-6, "streaming resampler matches the whole-buffer one");
  }
}

static void testWorkingFile() {
  const AudioData x = makeSpeech(44100, 8, 2);
  const std::string dir = (std::filesystem::temp_directory_path() / "sermon-rt-test-").string();
  writeWav(dir + "in.wav", x);
  auto info = rt::prepareWorkingFile(dir + "in.wav", dir + "work.wav", dir + "peaks.bin");
  std::printf("working file: %.2f s, %d ch, orig %d Hz, %lld frames\n", info.durationSeconds, info.channels, info.originalSampleRate, (long long)info.frames);
  CHECK(info.channels == 2 && info.originalSampleRate == 44100, "working file keeps channels, reports original rate");
  rt::WavFileSource src(dir + "work.wav");
  CHECK(src.sampleRate() == 48000 && src.length() == info.frames, "working file is 48 kHz and the reported length");
  const std::vector<float> ref = resample(x.channels[1].data(), x.length(), 44100, 48000);
  CHECK(size_t(src.length()) == ref.size(), "working file has the whole-file resampler's length");
  std::vector<std::vector<float>> buf; buf.assign(2, std::vector<float>());
  for (auto& b : buf) b.resize(3000);
  float* p[2] = {buf[0].data(), buf[1].data()};
  double err = 0;
  for (int64_t pos : {int64_t(0), int64_t(100000), int64_t(src.length() - 3000)}) {  // random access
    const int got = src.read(p, 3000, pos);
    for (int i = 0; i < got; i++) err = std::max(err, std::fabs(double(buf[1][size_t(i)]) - ref[size_t(pos + i)]));
  }
  std::printf("random-access reads vs whole-file resample: max diff %.2g\n", err);
  CHECK(err < 1e-6, "working file content matches the whole-file resampler");
  std::ifstream f(dir + "peaks.bin", std::ios::binary | std::ios::ate);
  CHECK(size_t(f.tellg()) == 16 + size_t(info.frames / 48) * 4 + (info.frames % 48 ? 4 : 0), "peaks file has one pair per millisecond");
  if (mp3EncodingAvailable()) {
    const auto bytes = encodeMp3(x, mp3Presets()[1], {}, [] { Mp3Options o; o.peakCeilingDb = std::nan(""); return o; }());
    std::ofstream(dir + "in.mp3", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    auto mi = rt::prepareWorkingFile(dir + "in.mp3", dir + "work2.wav", dir + "peaks2.bin");
    std::printf("mp3 working file: %.2f s, %d ch\n", mi.durationSeconds, mi.channels);
    CHECK(mi.channels == 1 && std::fabs(mi.durationSeconds - 8.0) < 0.1, "MP3 input (a mono preset) prepares to the right length");
  }
  for (const char* n : {"in.wav", "work.wav", "peaks.bin", "in.mp3", "work2.wav", "peaks2.bin"}) std::remove((dir + n).c_str());
}

int main() {
  testWorkingFile();
  testStreamResampler();
  testStagesMatchOffline();
  testChainProperties();
  if (failures == 0) std::printf("all real-time engine tests passed\n");
  return failures ? 1 : 0;
}
