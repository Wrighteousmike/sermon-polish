#include "../fsutil.h"
#include "render.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../audio_io.h"
#include "../loudness.h"
#include "limiter_rt.h"
#include "workfile.h"

namespace sermon::rt {

namespace {

constexpr int kBlock = 4096;

void checkCancel(const std::atomic<bool>& cancel) {
  if (cancel) throw std::runtime_error("Cancelled");
}

struct Measured {
  double lufs = -70, truePeakDb = -120;
};

// Pass 1: chain over [a, b) -> temp file; returns exact loudness and true peak of the result.
//
// The region is cut into pieces that run on as many threads as the machine has (each with its own
// chain). A piece starts 10 s early so its filters, compressor and denoiser have settled by the time it
// reaches its own part, and carries on 0.5 s past its end; the joins are crossfaded over that overlap, so
// the pieces blend into one continuous result. Loudness and peak are then measured on the joined result,
// so they are exact whatever the number of pieces.
constexpr int64_t kRunIn = 480000;      // 10 s of warm-up before a piece's own part (the denoiser needs several seconds to settle)
constexpr int64_t kOverlap = 24000;     // 0.5 s crossfade at each join
constexpr int64_t kMinPiece = 60 * 48000;  // pieces shorter than this aren't worth the warm-up

int pieceCount(int64_t length) {
  unsigned hw = std::thread::hardware_concurrency();
  if (const char* env = std::getenv("SERMON_RENDER_THREADS")) hw = unsigned(std::max(1, std::atoi(env)));
  const int64_t byLength = std::max<int64_t>(1, length / kMinPiece);
  return int(std::max<int64_t>(1, std::min<int64_t>(std::min<unsigned>(std::max(1u, hw), 32), byLength)));
}

Measured passOne(const RenderRequest& r, int64_t a, int64_t b, const std::string& tempWav, const std::function<void(const char*, double)>& progress,
                 const std::atomic<bool>& cancel) {
  int nCh = 0;
  {
    WavFileSource probe(r.sourceWav);
    nCh = probe.channels();
  }
  const int64_t length = b - a;
  const int n = pieceCount(length);
  struct Piece {
    int64_t start = 0, from = 0, to = 0, end = 0;  // input read from `start` (the warm-up); own part [from, to); written up to `end` (to + overlap, except the last)
    std::string path;
  };
  std::vector<Piece> pieces(static_cast<size_t>(n));
  int64_t totalOut = 0;
  for (int k = 0; k < n; k++) {
    Piece& pc = pieces[size_t(k)];
    pc.from = a + length * k / n;
    pc.to = a + length * (k + 1) / n;
    pc.end = k == n - 1 ? pc.to : pc.to + kOverlap;
    pc.start = std::max<int64_t>(0, pc.from - (k == 0 ? std::min<int64_t>(pc.from, 24000) : kRunIn));
    pc.path = tempWav + ".piece" + std::to_string(k) + ".wav";
    totalOut += (pc.from - pc.start) + (pc.end - pc.from);  // the warm-up counts too, so progress moves steadily from the start
  }
  struct Cleanup {
    std::vector<Piece>& p;
    ~Cleanup() { for (auto& x : p) removeFile(x.path); }
  } cleanup{pieces};

  std::atomic<int64_t> done{0};
  std::atomic<int> finished{0};
  std::atomic<bool> failed{false};
  std::vector<std::exception_ptr> errors(static_cast<size_t>(n));
  const std::atomic<double> strength{r.dereverbStrength};

  auto work = [&](int k) {
    try {
      const Piece& pc = pieces[size_t(k)];
      CorrectedSource src(std::make_unique<WavFileSource>(r.sourceWav),
                          r.correctionWav.empty() ? nullptr : std::make_unique<WavFileSource>(r.correctionWav), &strength);
      Chain chain;
      chain.prepare(48000, nCh);
      chain.setModel(r.model);
      ChainParams p = r.params;
      p.limiter = false;
      p.outputGainDb = 0;
      chain.setParams(p);
      chain.reset(false);
      const int64_t latency = chain.latency();
      WavWriter writer(pc.path, nCh, 48000);
      std::vector<std::vector<float>> buf(static_cast<size_t>(nCh), std::vector<float>(kBlock));
      std::vector<float*> ptr;
      for (auto& v : buf) ptr.push_back(v.data());
      const int64_t want = pc.end - pc.from;
      int64_t written = 0;
      int64_t warmDone = 0;
      for (int64_t pos = pc.start; written < want; pos += kBlock) {
        if (cancel || failed) throw std::runtime_error("Cancelled");
        const int got = pos < b ? src.read(ptr.data(), int(std::min<int64_t>(kBlock, b - pos)), pos) : 0;
        for (int c = 0; c < nCh; c++) std::fill(buf[size_t(c)].begin() + got, buf[size_t(c)].end(), 0.0f);
        chain.process(ptr.data(), kBlock, pos);
        const int64_t warm = std::min<int64_t>(pc.from - pc.start, pos + kBlock - pc.start);  // warm-up processed so far
        if (warm > warmDone) {
          done += warm - warmDone;
          warmDone = warm;
        }
        const int64_t outStart = pos - latency;  // this block's output covers [outStart, outStart + kBlock)
        const int64_t from = std::max<int64_t>(0, pc.from - outStart);
        const int64_t to = std::min<int64_t>(kBlock, pc.end - outStart);
        if (to > from) {
          std::vector<const float*> cp;
          for (int c = 0; c < nCh; c++) cp.push_back(ptr[size_t(c)] + from);
          writer.write(cp.data(), int(to - from));
          written += to - from;
          done += to - from;
        }
      }
      writer.close();
    } catch (...) {
      errors[size_t(k)] = std::current_exception();
      failed = true;
    }
    finished++;
  };

  std::vector<std::thread> pool;
  for (int k = 1; k < n; k++) pool.emplace_back(work, k);
  // The calling thread works too (piece 0) and reports progress between blocks of its own work via the
  // shared counter; the others are polled below once it finishes.
  std::thread reporter([&] {
    while (finished.load() < n) {
      progress("Cleaning the recording…", 0.80 * double(done.load()) / double(std::max<int64_t>(1, totalOut)));
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
  });
  work(0);
  for (auto& t : pool) t.join();
  reporter.join();
  for (auto& e : errors)
    if (e) std::rethrow_exception(e);
  checkCancel(cancel);

  // Join the pieces into the temp file, crossfading the overlaps, and measure the result.
  WavWriter writer(tempWav, nCh, 48000);
  HopEnergyMeter meter(48000, size_t(nCh), 4800);
  double peak = 0;
  int64_t joined = 0;
  std::vector<std::vector<float>> buf(static_cast<size_t>(nCh), std::vector<float>(kBlock)), buf2 = buf;
  std::vector<float*> ptr, ptr2;
  for (auto& v : buf) ptr.push_back(v.data());
  for (auto& v : buf2) ptr2.push_back(v.data());
  auto emit = [&](int count) {
    std::vector<const float*> cp;
    for (int c = 0; c < nCh; c++) cp.push_back(ptr[size_t(c)]);
    writer.write(cp.data(), count);
    meter.feed(cp.data(), size_t(count));
    for (int c = 0; c < nCh; c++)
      for (int i = 0; i < count; i++) peak = std::max(peak, double(std::fabs(buf[size_t(c)][size_t(i)])));
    joined += count;
    progress("Joining the pieces…", 0.80 + 0.05 * double(joined) / double(std::max<int64_t>(1, length)));
  };
  for (int k = 0; k < n; k++) {
    checkCancel(cancel);
    WavFileSource cur(pieces[size_t(k)].path);
    const int64_t bodyFrom = k == 0 ? 0 : kOverlap;
    const int64_t bodyTo = pieces[size_t(k)].to - pieces[size_t(k)].from;
    for (int64_t pos = bodyFrom; pos < bodyTo; pos += kBlock) {
      const int got = cur.read(ptr.data(), int(std::min<int64_t>(kBlock, bodyTo - pos)), pos);
      if (got <= 0) break;
      emit(got);
    }
    if (k < n - 1) {
      WavFileSource next(pieces[size_t(k + 1)].path);
      for (int64_t pos = 0; pos < kOverlap; pos += kBlock) {
        const int cnt = int(std::min<int64_t>(kBlock, kOverlap - pos));
        const int g1 = cur.read(ptr.data(), cnt, bodyTo + pos), g2 = next.read(ptr2.data(), cnt, pos);
        const int got = std::min(g1, g2);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
          const double w = double(pos + i + 0.5) / double(kOverlap);
          for (int c = 0; c < nCh; c++) buf[size_t(c)][size_t(i)] = float((1.0 - w) * double(buf[size_t(c)][size_t(i)]) + w * double(buf2[size_t(c)][size_t(i)]));
        }
        emit(got);
      }
    }
  }
  writer.close();
  Measured m;
  m.lufs = integratedLufsFromHops(meter.energies());
  m.truePeakDb = 20.0 * std::log10(std::max(peak, 1e-9));  // sample peak; the limiter handles true peak
  return m;
}

// Pass 2: limiter only (temp -> out), with the trim fades; returns loudness and true peak of `out`.
Measured passTwo(const RenderRequest& r, const std::string& tempWav, double gainDb, double fadeMs, double base, double span,
                 const std::function<void(const char*, double)>& progress, const std::atomic<bool>& cancel) {
  WavFileSource src(tempWav);
  const int nCh = src.channels();
  LimiterStage lim;
  lim.prepare(48000, nCh);
  lim.setParams(gainDb, r.ceilingDb);
  lim.reset();
  const int64_t latency = lim.latency(), length = src.length();
  const int64_t fade = std::min<int64_t>(int64_t(fadeMs / 1000.0 * 48000.0 + 0.5), length / 2);
  constexpr double kPi = 3.14159265358979323846;
  WavWriter writer(r.outWav, nCh, 48000);
  HopEnergyMeter meter(48000, size_t(nCh), 4800);
  std::vector<std::vector<float>> buf;
  buf.assign(static_cast<size_t>(nCh), std::vector<float>(kBlock));
  std::vector<float*> ptr;
  for (auto& v : buf) ptr.push_back(v.data());
  double tpLin = 0;
  const double kTpBound = truePeakBound();
  int64_t written = 0;
  for (int64_t pos = 0; written < length; pos += kBlock) {
    checkCancel(cancel);
    const int got = pos < length ? src.read(ptr.data(), kBlock, pos) : 0;
    for (int c = 0; c < nCh; c++) std::fill(buf[size_t(c)].begin() + got, buf[size_t(c)].end(), 0.0f);
    lim.process(ptr.data(), kBlock, pos);
    const int64_t outStart = pos - latency;
    const int64_t from = std::max<int64_t>(0, -outStart);
    const int64_t to = std::min<int64_t>(kBlock, length - outStart);
    if (to > from) {
      for (int64_t i = from; i < to; i++) {
        const int64_t t = outStart + i;
        double g = 1.0;
        if (t < fade) g = 0.5 - 0.5 * std::cos(kPi * (double(t) + 0.5) / double(fade));
        else if (length - 1 - t < fade) g = 0.5 - 0.5 * std::cos(kPi * (double(length - 1 - t) + 0.5) / double(fade));
        if (g != 1.0)
          for (int c = 0; c < nCh; c++) buf[size_t(c)][size_t(i)] = float(buf[size_t(c)][size_t(i)] * g);
      }
      std::vector<const float*> cp;
      for (int c = 0; c < nCh; c++) cp.push_back(ptr[size_t(c)] + from);
      writer.write(cp.data(), int(to - from));
      meter.feed(cp.data(), size_t(to - from));
      for (int64_t i = from; i < to; i++) {
        // 16-sample window x[k-7..k+8] centred on i; samples outside this block are approximated by the sample peak
        if (i >= 7 && i + 8 < kBlock) {
          for (int c = 0; c < nCh; c++) {
            float w[16];
            float mx = 0;
            for (int j = 0; j < 16; j++) {
              w[j] = buf[size_t(c)][size_t(i - 7 + j)];
              mx = std::max(mx, std::fabs(w[j]));
            }
            if (double(mx) * kTpBound > tpLin) tpLin = std::max(tpLin, truePeakWindow(w));  // (a quieter window can't beat the peak so far)
          }
        } else {
          for (int c = 0; c < nCh; c++) tpLin = std::max(tpLin, double(std::fabs(buf[size_t(c)][size_t(i)])));
        }
      }
      written += to - from;
      progress("Setting the loudness…", base + span * double(written) / double(length));
    }
  }
  writer.close();
  Measured m;
  m.lufs = integratedLufsFromHops(meter.energies());
  m.truePeakDb = 20.0 * std::log10(std::max(tpLin, 1e-9));
  return m;
}

}  // namespace

RenderReport renderExport(const RenderRequest& r, const std::function<void(const char*, double)>& progress, const std::atomic<bool>& cancel) {
  int64_t length = 0;
  {
    WavFileSource probe(r.sourceWav);
    length = probe.length();
  }
  const int64_t a = std::max<int64_t>(0, int64_t(r.startSeconds * 48000.0));
  const int64_t b = std::min<int64_t>(length, int64_t(r.endSeconds * 48000.0));
  if (b - a < 4800) throw std::runtime_error("The selected part is too short to export.");

  const std::string tempWav = r.outWav + ".pass1.wav";
  struct Cleanup {
    std::string p;
    ~Cleanup() { removeFile(p); }
  } cleanup{tempWav};

  const Measured m1 = passOne(r, a, b, tempWav, progress, cancel);
  if (!std::isfinite(m1.lufs) || m1.lufs < -69) throw std::runtime_error("This part of the recording is silent.");

  RenderReport rep;
  // A NaN target means loudness matching is off: the level is left as it is, but the final true-peak
  // limiter still runs, so the output never exceeds the ceiling.
  const double target = std::isnan(r.targetLufs) ? m1.lufs : r.targetLufs;
  double gain = target - m1.lufs, extra = 0;
  Measured m2;
  const auto trace0 = std::chrono::steady_clock::now();
  const bool trace = std::getenv("SERMON_RENDER_TRACE") != nullptr;
  for (int pass = 0; pass < 5; pass++) {
    m2 = passTwo(r, tempWav, gain + extra, r.fadeMs, 0.85 + 0.03 * pass, 0.03, progress, cancel);
    rep.limiterPasses = pass + 1;
    if (trace) std::fprintf(stderr, "[render] limiter pass %d done at +%.1fs (gain %.2f dB, shortfall %.2f dB)\n", pass + 1, std::chrono::duration<double>(std::chrono::steady_clock::now() - trace0).count(), gain + extra, target - m2.lufs);
    const double shortfall = target - m2.lufs;
    if (shortfall <= 0.12 || extra >= 4.0) break;  // within 0.12 dB of the target is as exact as any loudness meter
    extra = std::min(4.0, extra + shortfall);  // the limiter cost loudness: add it back
  }
  if (r.pregainDb > 0) {
    // Manual drive into the limiter, after the loudness has been matched: the limiter takes the extra
    // as gain reduction on the peaks, and the result is louder than the target by design.
    m2 = passTwo(r, tempWav, gain + extra + r.pregainDb, r.fadeMs, 0.97, 0.03, progress, cancel);
    rep.limiterPasses++;
  }
  rep.lufs = m2.lufs;
  rep.truePeakDb = m2.truePeakDb;
  progress("Done", 1.0);
  return rep;
}

}  // namespace sermon::rt
