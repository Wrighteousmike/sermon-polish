// Real-time engine host for the desktop app. Reads one JSON command per line on stdin and writes
// one JSON event per line on stdout. Commands (flat objects, "cmd" selects):
//   open {path, work, peaks, nullDevice?, buffer?}   prepare + start the device
//   play | pause | shutdown
//   seek {t}  trim {start,end}  loop {on}  ab {original}
//   params {rumble,leveling,level,levelAmt,loudAmt,pregain,noise,dereverb,tone,toneAmt,deess,comp,lowpass,limiter,gain,ceiling}
//     noise/dereverb/deess/comp: off|less|normal|more|max or 0-100 (the sliders; 0 = off)
//     tone: off|1|2|3|ref, or toneAmt 0-100   leveling: true|false, or levelAmt 0-100   level 0..1
// Events: opened, state (20 Hz), loudness (the output gain solved for the target, -14 LUFS unless loudAmt says otherwise), progress, error.
// Unless params carries an explicit "gain", the host solves the output gain itself, from excerpts of
// the kept region run through the live chain (exact loudness is solved again at export).
#include "fsutil.h"
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "amount.h"
#include "audio_io.h"
#include "declip_spade.h"
#include "loudness.h"
#include "wpe.h"
#include "rt/host.h"
#include "rt/render.h"

// Timeline lines for diagnostics (the app collects stderr into its log).
static std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
static void hlog(const std::string& msg) {
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  std::fprintf(stderr, "[host %.1fs] %s\n", t, msg.c_str());
  std::fflush(stderr);
}
static double msSince(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }

using namespace sermon;
using namespace sermon::rt;

namespace {

std::mutex outMutex;

std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (static_cast<unsigned char>(c) >= 0x20) o += c;
  }
  return o;
}
void emit(const std::string& line) {
  std::lock_guard<std::mutex> lock(outMutex);
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}
void fail(const std::string& m) { emit("{\"event\":\"error\",\"message\":\"" + esc(m) + "\"}"); }

// Flat {"k":"v","n":1.5,"b":true} parser: enough for the commands above.
std::map<std::string, std::string> parseFlat(const std::string& s) {
  std::map<std::string, std::string> m;
  size_t i = 0;
  auto str = [&](std::string& out) {
    out.clear();
    i++;  // opening quote
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) {
        i++;
        out += s[i] == 'n' ? '\n' : s[i];
      } else out += s[i];
      i++;
    }
    i++;
  };
  while (i < s.size()) {
    if (s[i] != '"') { i++; continue; }
    std::string key, val;
    str(key);
    while (i < s.size() && (s[i] == ':' || s[i] == ' ')) i++;
    if (i < s.size() && s[i] == '"') str(val);
    else while (i < s.size() && s[i] != ',' && s[i] != '}') val += s[i++];
    m[key] = val;
  }
  return m;
}

double num(const std::map<std::string, std::string>& m, const char* k, double d) {
  auto it = m.find(k);
  return it == m.end() ? d : std::atof(it->second.c_str());
}
bool flag(const std::map<std::string, std::string>& m, const char* k, bool d) {
  auto it = m.find(k);
  return it == m.end() ? d : (it->second == "true" || it->second == "1");
}
std::string text(const std::map<std::string, std::string>& m, const char* k) {
  auto it = m.find(k);
  return it == m.end() ? std::string() : it->second;
}

std::optional<Intensity> intensity(const std::string& v, std::optional<Intensity> keep) {
  std::optional<Intensity> out = keep;
  parseIntensity(v, out);  // unrecognised or missing text keeps the current value
  return out;
}

ChainParams applyParams(ChainParams p, const std::map<std::string, std::string>& m) {
  p.rumble = flag(m, "rumble", p.rumble);
  p.leveling = flag(m, "leveling", p.leveling);
  p.levelerAmount = num(m, "level", p.levelerAmount);
  p.noise = intensity(text(m, "noise"), p.noise);
  p.deess = intensity(text(m, "deess"), p.deess);
  p.compressor = intensity(text(m, "comp"), p.compressor);
  const std::string tone = text(m, "tone");
  if (tone == "off") p.tone = std::nullopt;
  else if (tone == "1") p.tone = Tone::Tone1;
  else if (tone == "2") p.tone = Tone::Tone2;
  else if (tone == "3") p.tone = Tone::Tone3;
  else if (tone == "ref") { p.tone = Tone::Reference; p.toneScale = 1.0; }
  // Amounts from the sliders (0-100): the Studio-match EQ scaled by the tone macro; 0 is off.
  std::optional<Intensity> toneAmt = Intensity::Normal;
  if (m.count("toneAmt") && parseIntensity(text(m, "toneAmt"), toneAmt)) {
    if (toneAmt) { p.tone = Tone::Reference; p.toneScale = amount::toneScale(*toneAmt); }
    else p.tone = std::nullopt;
  }
  std::optional<Intensity> levelAmt = Intensity::Normal;
  if (m.count("levelAmt") && parseIntensity(text(m, "levelAmt"), levelAmt)) {
    p.leveling = levelAmt.has_value();
    if (levelAmt) p.levelerAmount = amount::levelerScale(*levelAmt);
  }
  p.lowpass = flag(m, "lowpass", p.lowpass);
  p.limiter = flag(m, "limiter", p.limiter);
  p.outputGainDb = num(m, "gain", p.outputGainDb);
  p.ceilingDb = num(m, "ceiling", p.ceilingDb);
  return p;
}

std::string db(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f", v < -120 ? -120.0 : v);
  return b;
}

// Works out the output gain that brings the chain's loudness to the target, on a worker thread: runs a
// private copy of the chain over ~10 excerpts spread across the kept region and measures integrated
// loudness. Restarts whenever the settings or the trim change.
class GainSolver {
 public:
  using Done = std::function<void(double gainDb, double lufs)>;
  GainSolver(std::string work, std::shared_ptr<const FileModel> model, Done done) : work_(std::move(work)), model_(std::move(model)), done_(std::move(done)) {
    thread_ = std::thread([this] { loop(); });
  }
  ~GainSolver() {
    {
      std::lock_guard<std::mutex> l(m_);
      quit_ = true;
      gen_++;
    }
    cv_.notify_all();
    thread_.join();
  }
  // While an export renders, the live loudness estimate is not worked out (all threads go to the export).
  void suspend() {
    std::lock_guard<std::mutex> l(m_);
    suspended_ = true;
    pending_ = false;
    gen_++;  // abandons a measurement already under way
  }
  void resume() {
    {
      std::lock_guard<std::mutex> l(m_);
      suspended_ = false;
      if (have_) {
        pending_ = true;
        gen_++;
      }
    }
    cv_.notify_all();
  }
  void request(const ChainParams& p, double start, double end, double target) {
    {
      std::lock_guard<std::mutex> l(m_);
      params_ = p;
      target_ = target;
      have_ = true;
      start_ = start;
      end_ = end;
      pending_ = !suspended_;
      gen_++;
    }
    cv_.notify_all();
  }

 private:
  void loop() {
    WavFileSource src(work_);
    const int ch = src.channels();
    Chain chain;
    chain.prepare(48000, ch);
    chain.setModel(model_);
    for (;;) {
      ChainParams p;
      double a, b, target;
      uint64_t gen;
      {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return quit_ || pending_; });
        if (quit_) return;
        pending_ = false;
        p = params_; a = start_; b = end_; target = target_; gen = gen_;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let a burst of changes settle
      {
        std::lock_guard<std::mutex> l(m_);
        if (gen != gen_) continue;
      }
      p.outputGainDb = 0;
      chain.setParams(p);
      const auto t0 = std::chrono::steady_clock::now();
      hlog("loudness measurement started");
      const double lufs = measure(chain, src, ch, a, b, gen);
      hlog(std::isfinite(lufs) ? "loudness measured in " + std::to_string(int(msSince(t0))) + " ms" : "loudness measurement abandoned after " + std::to_string(int(msSince(t0))) + " ms (settings or trim changed)");
      std::lock_guard<std::mutex> l(m_);
      if (gen == gen_ && std::isfinite(lufs)) done_(std::isnan(target) ? 0.0 : std::max(-60.0, std::min(60.0, target - lufs)), lufs);
    }
  }
  bool stale(uint64_t gen) {
    std::lock_guard<std::mutex> l(m_);
    return gen != gen_;
  }
  double measure(Chain& chain, WavFileSource& src, int ch, double a, double b, uint64_t gen) {
    const int64_t first = int64_t(a * 48000), last = std::min<int64_t>(src.length(), int64_t(b * 48000));
    const int64_t span = last - first, len = 8 * 48000;
    const int n = span < 8 * len ? 1 : 6;
    const int64_t each = n == 1 ? span : len;
    std::vector<std::vector<float>> buf(size_t(ch), std::vector<float>(1024));
    std::vector<float*> p;
    for (auto& v : buf) p.push_back(v.data());
    std::vector<double> hops;
    chain.reset(false);
    for (int k = 0; k < n; k++) {
      const int64_t start = n == 1 ? first : first + int64_t((double(k) + 0.5) * double(span) / n) - each / 2;
      const int64_t pre = std::min<int64_t>(start, 24000);
      if (k > 0) chain.reset(true);
      HopEnergyMeter meter(48000, size_t(ch), 4800);
      for (int64_t pos = start - pre; pos < start + each; pos += 1024) {
        if (stale(gen)) return std::nan("");
        const int got = src.read(p.data(), 1024, pos);
        for (int c = 0; c < ch; c++) std::fill(buf[size_t(c)].begin() + got, buf[size_t(c)].end(), 0.0f);
        chain.process(p.data(), 1024, pos);
        if (pos >= start) {
          const float* cp[2] = {buf[0].data(), buf[size_t(ch - 1)].data()};
          meter.feed(cp, 1024);
        }
      }
      hops.insert(hops.end(), meter.energies().begin(), meter.energies().end());
    }
    return integratedLufsFromHops(hops);
  }

  double target_ = -14.0;
  std::string work_;
  std::shared_ptr<const FileModel> model_;
  Done done_;
  std::thread thread_;
  std::mutex m_;
  std::condition_variable cv_;
  bool quit_ = false, pending_ = false, suspended_ = false, have_ = false;
  uint64_t gen_ = 0;
  ChainParams params_;
  double start_ = 0, end_ = 0;
};


// Declipping and dereverb can't run block by block, so they are computed once on a worker thread
// (about a minute for a long recording) and then mixed in live:
//  - declip is baked into a second working file the host switches to;
//  - dereverb is stored as a correction signal (what WPE removes at full strength), and the host mixes
//    strength * correction into the audio as it plays. So once the correction exists, moving the
//    dereverb slider costs nothing and is heard at once; only the first use (or a change to declip,
//    which the correction is computed from) needs the wait.
class Baker {
 public:
  struct Files {
    std::string base;        // the file to play (the working file, or the declipped bake of it)
    std::string correction;  // dereverb correction for `base`, or empty if none has been computed
  };
  using Ready = std::function<void(const Files&)>;
  using Status = std::function<void(const char* state)>;
  Baker(std::string work, Ready ready, Status status) : work_(std::move(work)), ready_(std::move(ready)), status_(std::move(status)) {
    current_.base = work_;
    thread_ = std::thread([this] { loop(); });
  }
  ~Baker() {
    {
      std::lock_guard<std::mutex> l(m_);
      quit_ = true;
      gen_++;
    }
    cv_.notify_all();
    thread_.join();
    removeFile(bakedPath());
    removeFile(revPath(false));
    removeFile(revPath(true));
  }
  // Blocks until everything requested has been computed; returns what to read (the plain working
  // file if nothing is baked or baking failed).
  Files await() {
    std::unique_lock<std::mutex> l(m_);
    cv_idle_.wait(l, [this] { return !pending_ && !working_; });
    return current_;
  }
  // `dereverb`: whether any dereverb is wanted now (computes the correction the first time).
  void request(bool declip, bool dereverb) {
    {
      std::lock_guard<std::mutex> l(m_);
      if (have_ && declip == declip_ && dereverb == dereverb_) return;
      have_ = true;
      declip_ = declip;
      dereverb_ = dereverb;
      pending_ = true;
      gen_++;
    }
    cv_.notify_all();
  }

 private:
  std::string bakedPath() const { return work_ + ".baked.wav"; }
  std::string revPath(bool declipped) const { return work_ + (declipped ? ".rev1.wav" : ".rev0.wav"); }
  bool stale(uint64_t gen) {
    std::lock_guard<std::mutex> l(m_);
    return gen != gen_;
  }
  static AudioData load(const std::string& path) {
    AudioData a;
    WavFileSource src(path);
    a.sampleRate = 48000;
    a.channels.assign(size_t(src.channels()), std::vector<float>(size_t(src.length())));
    std::vector<float*> p;
    for (auto& c : a.channels) p.push_back(c.data());
    for (int64_t pos = 0; pos < src.length(); pos += 1 << 16) {
      std::vector<float*> q;
      for (auto* c : p) q.push_back(c + pos);
      src.read(q.data(), int(std::min<int64_t>(1 << 16, src.length() - pos)), pos);
    }
    return a;
  }
  void loop() {
    for (;;) {
      bool declip, rev;
      uint64_t gen;
      {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return quit_ || pending_; });
        if (quit_) return;
        pending_ = false;
        declip = declip_;
        rev = dereverb_;
        gen = gen_;
      }
      const bool needBake = declip && !bakedDone_;
      const bool needRev = rev && !revDone_[declip ? 1 : 0];
      if (!needBake && !needRev) {  // everything wanted already exists: just publish it
        publish(declip);
        continue;
      }
      { std::lock_guard<std::mutex> l(m_); working_ = true; }
      status_("working");
      struct Idle { Baker* b; ~Idle() { std::lock_guard<std::mutex> l(b->m_); b->working_ = false; b->cv_idle_.notify_all(); } } idle{this};
      try {
        hlog(std::string("preparing in the background: ") + (needBake ? "clip repair " : "") + (needRev ? "echo reduction" : ""));
        AudioData a = load(declip && bakedDone_ ? bakedPath() : work_);
        if (needBake) {
          for (auto& c : a.channels) {
            if (stale(gen)) break;
            declipSpade(c.data(), c.size(), 48000);
          }
          if (stale(gen)) continue;
          writeWav(bakedPath(), a, 32);
          bakedDone_ = true;
          hlog("clip repair finished");
        }
        if (needRev) {
          // Correction = WPE(x) - x at full strength, one channel at a time to keep memory down.
          AudioData corr;
          corr.sampleRate = 48000;
          for (auto& c : a.channels) {
            if (stale(gen)) break;
            std::vector<float> t = c;
            dereverb(t.data(), t.size(), 48000, Intensity::More);  // More = full-strength subtraction (1.0)
            for (size_t i = 0; i < t.size(); i++) t[i] -= c[i];
            corr.channels.push_back(std::move(t));
          }
          if (stale(gen)) continue;
          writeWav(revPath(declip), corr, 32);
          revDone_[declip ? 1 : 0] = true;
          hlog("echo reduction prepared");
        }
        if (stale(gen)) continue;
        publish(declip);
      } catch (const std::exception&) {
        { std::lock_guard<std::mutex> l(m_); current_ = Files{work_, ""}; }
        status_("failed");
      }
    }
  }
  void publish(bool declip) {
    Files f;
    f.base = declip && bakedDone_ ? bakedPath() : work_;
    if (revDone_[declip ? 1 : 0]) f.correction = revPath(declip);
    {
      std::lock_guard<std::mutex> l(m_);
      current_ = f;
    }
    status_(f.base == work_ && f.correction.empty() ? "off" : "ready");
    ready_(f);
    std::lock_guard<std::mutex> l(m_);
    cv_idle_.notify_all();
  }
  std::string work_;
  Files current_;
  std::condition_variable cv_idle_;
  bool working_ = false;
  Ready ready_;
  Status status_;
  std::thread thread_;
  std::mutex m_;
  std::condition_variable cv_;
  bool quit_ = false, pending_ = false, have_ = false, declip_ = false, dereverb_ = false;
  bool bakedDone_ = false, revDone_[2] = {false, false};  // [declipped?]
  uint64_t gen_ = 0;
};


}  // namespace

int main() {
  binaryStdio();
  Host host;
  ChainParams params;
  std::atomic<bool> open{false}, quit{false};
  double duration = 0, trimStart = 0, trimEnd = 0, autoGainDb = 0;
  double pregainDb = 0;  // extra gain into the limiter on top of the loudness solve (0-12 dB)
  bool autoGain = true, bakeDeclip = false;
  double targetLufs = -14.0;  // what the loudness slider asks for
  bool matchLoudness = true;  // false = leave the level alone (the true-peak limiter still runs)
  auto solveTarget = [&] { return matchLoudness ? targetLufs : std::nan(""); };
  std::optional<Intensity> bakeDereverb;  // the dereverb slider
  std::mutex pm;  // guards params, autoGain, autoGainDb (the solver reports from its own thread)
  std::unique_ptr<GainSolver> solver;
  std::unique_ptr<Baker> baker;
  std::shared_ptr<const FileModel> fileModel;
  std::string workPath;
  std::thread renderThread;
  std::atomic<bool> renderCancel{false}, rendering{false};

  std::thread ticker([&] {
    while (!quit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (!open) continue;
      HostState s = host.state();
      const double pk = std::max(s.peakL, s.peakR);
      emit("{\"event\":\"state\",\"position\":" + db(s.position) + ",\"playing\":" + (s.playing ? "true" : "false") +
           ",\"applying\":" + std::string(s.applying ? "true" : "false") + ",\"atEnd\":" + (s.atEnd ? "true" : "false") + ",\"peakDb\":" + db(pk > 0 ? 20 * std::log10(pk) : -120) +
           ",\"momentary\":" + db(s.momentaryLufs) + ",\"shortTerm\":" + db(s.shortTermLufs) +
           ",\"leveler\":" + db(s.meters.levelerGainDb) + ",\"comp\":" + db(s.meters.compressorReductionDb) +
           ",\"deess\":" + db(s.meters.deessReductionDb) + ",\"limiter\":" + db(s.meters.limiterReductionDb) +
           ",\"underruns\":" + std::to_string(s.underruns) + ",\"load\":" + db(s.renderLoad) + "}");
    }
  });

  std::string line;
  while (std::getline(std::cin, line)) {
    auto m = parseFlat(line);
    const std::string cmd = text(m, "cmd");
    try {
      if (cmd == "open") {
        open = false;
        solver.reset();
        baker.reset();
        host.close();
        const std::string work = text(m, "work"), peaks = text(m, "peaks");
        const auto tOpen = std::chrono::steady_clock::now();
        hlog("open: preparing the working file");
        auto info = prepareWorkingFile(text(m, "path"), work, peaks, [](double f) {
          emit("{\"event\":\"progress\",\"label\":\"Preparing audio…\",\"fraction\":" + db(f) + "}");
        });
        (void)info;
        hlog("open: working file ready after " + std::to_string(int(msSince(tOpen))) + " ms");
        WavFileSource src(work);
        std::shared_ptr<const FileModel> model = buildFileModel(src, Tone::Reference, [](double f) {
          emit("{\"event\":\"progress\",\"label\":\"Analysing…\",\"fraction\":" + db(f) + "}");
        });
        hlog("open: analysis done after " + std::to_string(int(msSince(tOpen))) + " ms; opening the audio device");
        std::string err;
        if (!host.open(work, model, flag(m, "nullDevice", false), int(num(m, "buffer", 2048)), &err)) { hlog("open failed: " + err); fail(err); continue; }
        hlog("open: ready after " + std::to_string(int(msSince(tOpen))) + " ms");
        host.setParams(params);
        duration = host.duration();
        trimStart = 0;
        trimEnd = duration;
        fileModel = model;
        workPath = work;
        solver = std::make_unique<GainSolver>(work, model, [&](double gain, double lufs) {
          std::lock_guard<std::mutex> l(pm);
          autoGainDb = gain;
          if (autoGain) {
            params.outputGainDb = gain + pregainDb;
            host.setParams(params, false);
          }
          emit("{\"event\":\"loudness\",\"gainDb\":" + db(gain) + ",\"lufs\":" + db(lufs) + "}");
        });
        solver->request(params, trimStart, trimEnd, solveTarget());
        baker = std::make_unique<Baker>(
            work, [&](const Baker::Files& f) { host.swapSource(f.base, f.correction); },
            [](const char* state) { emit(std::string("{\"event\":\"bake\",\"state\":\"") + state + "\"}"); });
        baker->request(bakeDeclip, bakeDereverb.has_value());
        host.setDereverbStrength(bakeDereverb ? amount::dereverbStrength(*bakeDereverb) : 0.0);
        open = true;
        emit("{\"event\":\"opened\",\"duration\":" + db(duration) + ",\"peaks\":\"" + esc(peaks) + "\"}");
      } else if (cmd == "play") host.play();
      else if (cmd == "pause") host.pause();
      else if (cmd == "seek") host.seek(num(m, "t", 0));
      else if (cmd == "trim") {
        trimStart = num(m, "start", 0);
        trimEnd = num(m, "end", duration);
        host.setTrim(trimStart, trimEnd);
        std::lock_guard<std::mutex> l(pm);
        if (solver && autoGain) solver->request(params, trimStart, trimEnd, solveTarget());
      }
      else if (cmd == "loop") host.setLoop(flag(m, "on", false));
      else if (cmd == "ab") host.setOriginal(flag(m, "original", false));
      else if (cmd == "params") {
        std::lock_guard<std::mutex> l(pm);
        params = applyParams(params, m);
        if (m.count("pregain")) pregainDb = std::max(0.0, std::min(12.0, num(m, "pregain", 0)));
        bakeDeclip = flag(m, "declip", bakeDeclip);
        std::optional<Intensity> loud = Intensity::Normal;
        if (m.count("loudAmt") && parseIntensity(text(m, "loudAmt"), loud)) {  // the loudness slider; 0 = leave the level alone
          matchLoudness = loud.has_value();  // the limiter itself always stays on
          if (loud) targetLufs = amount::targetLufs(*loud);
        }
        bakeDereverb = intensity(text(m, "dereverb"), bakeDereverb);
        if (baker) baker->request(bakeDeclip, bakeDereverb.has_value());
        if (m.count("gain")) autoGain = false;
        else if (autoGain) params.outputGainDb = autoGainDb + pregainDb;
        if (open) {
          host.setDereverbStrength(bakeDereverb ? amount::dereverbStrength(*bakeDereverb) : 0.0);
          host.setParams(params);
          if (solver && autoGain) solver->request(params, trimStart, trimEnd, solveTarget());
          }
      }
      else if (cmd == "render") {
        if (!open || rendering) { fail("cannot render now"); continue; }
        if (renderThread.joinable()) renderThread.join();
        renderCancel = false;
        rendering = true;
        ChainParams p;
        double p_rev = 0, p_target = -14.0, p_pregain = 0;
        { std::lock_guard<std::mutex> l(pm); p = params; p_target = solveTarget(); p_pregain = pregainDb; p_rev = bakeDereverb ? amount::dereverbStrength(*bakeDereverb) : 0.0; }
        const std::string out = text(m, "out");
        const double a = num(m, "start", 0), b = num(m, "end", duration);
        if (solver) solver->suspend();  // every thread goes to the export
        renderThread = std::thread([&, p, p_rev, p_target, p_pregain, out, a, b] {
          try {
            RenderRequest rq;
            emit("{\"event\":\"render\",\"label\":\"Preparing…\",\"fraction\":0}");
            if (baker) {
              const Baker::Files f = baker->await();
              rq.sourceWav = f.base;
              rq.correctionWav = f.correction;
            } else rq.sourceWav = workPath;
            rq.dereverbStrength = p_rev;
            rq.outWav = out;
            rq.startSeconds = a;
            rq.endSeconds = b;
            rq.params = p;
            rq.targetLufs = p_target;
            rq.pregainDb = p_pregain;
            rq.model = fileModel;
            double lastSent = -1;
            const RenderReport rep = renderExport(rq, [&lastSent](const char* label, double f) {
              if (f < 1.0 && f - lastSent < 0.01 && f >= lastSent) return;  // at most ~100 updates
              lastSent = f;
              emit(std::string("{\"event\":\"render\",\"label\":\"") + esc(label) + "\",\"fraction\":" + db(f) + "}");
            }, renderCancel);
            emit("{\"event\":\"rendered\",\"path\":\"" + esc(out) + "\",\"seconds\":" + db(b - a) + ",\"lufs\":" + db(rep.lufs) + ",\"truePeakDb\":" + db(rep.truePeakDb) + "}");
          } catch (const std::exception& e) {
            emit(std::string("{\"event\":\"render_error\",\"message\":\"") + esc(e.what()) + "\"}");
          }
          rendering = false;
          std::lock_guard<std::mutex> l(pm);
          if (solver) solver->resume();
        });
      } else if (cmd == "cancelRender") renderCancel = true;
      else if (cmd == "shutdown") break;
      else fail("unknown command: " + cmd);
    } catch (const std::exception& e) {
      fail(e.what());
    }
  }
  renderCancel = true;
  if (renderThread.joinable()) renderThread.join();
  quit = true;
  ticker.join();
  open = false;
  solver.reset();
  baker.reset();
  host.close();
  return 0;
}
