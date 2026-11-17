#include "analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

#include "declip_spade.h"
#include "fft.h"
#include "loudness.h"

namespace sermon {

namespace {

using cd = std::complex<double>;

double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[size_t(p * double(v.size() - 1) + 0.5)];
}

std::string fmt(double v, int digits = 1) {
  char b[32];
  std::snprintf(b, sizeof b, "%.*f", digits, v);
  return b;
}

// Average power spectrum of the chosen frames (frame length N, Hann).
std::vector<double> averageSpectrum(const float* x, const std::vector<size_t>& starts, size_t N) {
  static thread_local std::vector<cd> buf;
  const Fft fft(N);
  std::vector<double> acc(N / 2 + 1, 0.0);
  buf.assign(N, cd(0, 0));
  for (size_t s : starts) {
    for (size_t i = 0; i < N; i++) buf[i] = double(x[s + i]) * (0.5 - 0.5 * std::cos(2 * 3.14159265358979 * double(i) / double(N)));
    fft.transform(buf, -1);
    for (size_t k = 0; k <= N / 2; k++) acc[k] += std::norm(buf[k]);
  }
  for (auto& v : acc) v /= double(std::max<size_t>(1, starts.size()));
  return acc;
}

double bandPower(const std::vector<double>& spec, double fs, double lo, double hi) {
  const size_t N = (spec.size() - 1) * 2;
  double p = 0;
  for (size_t k = 1; k < spec.size(); k++) {
    const double f = double(k) * fs / double(N);
    if (f >= lo && f < hi) p += spec[k];
  }
  return p;
}

double db(double p) { return 10.0 * std::log10(std::max(p, 1e-30)); }

}  // namespace

Analysis analyzeAudio(const AudioData& audio) { return analyzeAudio(audio, 0, audio.length()); }

Analysis analyzeAudio(const AudioData& audio, size_t start, size_t end) {
  Analysis a;
  const double fs = audio.sampleRate;
  end = std::min(end, audio.length());
  const size_t n = end > start ? end - start : 0, nCh = audio.channels.size();
  a.sampleRate = audio.sampleRate;
  a.channels = nCh;
  a.seconds = double(n) / fs;
  if (n < size_t(fs)) return a;

  std::vector<const float*> chp;
  for (auto& c : audio.channels) chp.push_back(c.data() + start);
  a.lufs = integratedLufs(chp.data(), nCh, n, fs);
  a.truePeakDb = 20.0 * std::log10(std::max(truePeak(chp.data(), nCh, n), 1e-9));

  // Mono mix (no copy at all for mono input).
  std::vector<float> monoStorage;
  if (nCh > 1) {
    monoStorage.assign(n, 0.0f);
    for (size_t c = 0; c < nCh; c++)
      for (size_t i = 0; i < n; i++) monoStorage[i] += chp[c][i] / float(nCh);
  }
  const float* mono = nCh > 1 ? monoStorage.data() : chp[0];

  if (nCh == 2) {
    double sxy = 0, sxx = 0, syy = 0;
    for (size_t i = 0; i < n; i += 4) { const double l = chp[0][i], r = chp[1][i]; sxy += l * r; sxx += l * l; syy += r * r; }
    a.channelCorrelation = sxy / std::sqrt(sxx * syy + 1e-30);
  }

  // Loudness range proxy and slow level drift from 400 ms blocks.
  const size_t hop = size_t(std::llround(0.1 * fs));
  const std::vector<double> e = kWeightedHopEnergy(chp.data(), nCh, n, fs, hop);
  std::vector<double> blocks;
  for (size_t b = 0; b + 4 <= e.size(); b++) blocks.push_back(energyToLufs((e[b] + e[b + 1] + e[b + 2] + e[b + 3]) / 4));
  {
    std::vector<double> gated;
    for (double v : blocks) if (v > -70) gated.push_back(v);
    const double rel = pct(gated, 0.95) - 20.0;
    std::vector<double> kept;
    for (double v : gated) if (v > rel) kept.push_back(v);
    a.loudnessRangeLu = pct(kept, 0.95) - pct(kept, 0.10);
    // slow drift: medians of speech blocks in 20 s windows
    const double top = pct(gated, 0.95);
    const size_t win = size_t(20.0 / 0.1);
    std::vector<double> medians;
    for (size_t s0 = 0; s0 + win <= blocks.size(); s0 += win / 2) {
      std::vector<double> w;
      for (size_t i = s0; i < s0 + win; i++) if (blocks[i] > top - 12) w.push_back(blocks[i]);
      if (w.size() > win / 6) medians.push_back(pct(w, 0.5));
    }
    a.levelDriftDb = medians.size() >= 3 ? pct(medians, 0.9) - pct(medians, 0.1) : 0.0;
  }

  // 20 ms frame levels: speech level, noise floor.
  const size_t fl = size_t(std::llround(0.02 * fs)), nf = n / fl;
  std::vector<double> lv(nf);
  for (size_t i = 0; i < nf; i++) {
    double s = 0;
    for (size_t k = 0; k < fl; k++) s += double(mono[i * fl + k]) * mono[i * fl + k];
    lv[i] = 10.0 * std::log10(s / double(fl) + 1e-20);
  }
  const double p95 = pct(lv, 0.95), p10 = pct(lv, 0.10);
  a.speechLevelDb = p95;
  a.noiseFloorDb = p10;
  a.snrDb = p95 - p10;

  // Spectra of noise frames and speech frames.
  size_t N = 4096;
  while (double(N) > 0.2 * fs && N > 1024) N >>= 1;
  std::vector<size_t> noiseStarts, speechStarts;
  const size_t span = N / fl + 1;  // frames covered by one analysis window
  for (size_t i = 0; i + span < nf && (noiseStarts.size() < 400 || speechStarts.size() < 400); i++) {
    const size_t start = i * fl;
    if (noiseStarts.size() < 400 && i % 3 == 0) {
      // Noise window: every frame it covers must be quiet, or phrase onsets (voice harmonics!)
      // would masquerade as hum.
      bool quiet = true;
      for (size_t k = i; k < i + span && quiet; k++) quiet = lv[k] <= p10 + 4.0;
      if (quiet) noiseStarts.push_back(start);
    }
    if (lv[i] >= p95 - 12 && speechStarts.size() < 400 && i % 5 == 0) speechStarts.push_back(start);
  }
  if (!noiseStarts.empty()) {
    const auto spec = averageSpectrum(mono, noiseStarts, N);
    a.noiseHfShareDb = db(bandPower(spec, fs, 4000, 0.48 * fs)) - db(bandPower(spec, fs, 100, 4000));
    const double binHz = fs / double(N);
    for (double base : {50.0, 60.0}) for (int h = 1; h <= 3; h++) {
      const double f = base * h;
      if (f > 400) continue;
      const size_t k = size_t(std::llround(f / binHz));
      double peak = 0;
      for (size_t j = k > 1 ? k - 1 : 1; j <= k + 1 && j < spec.size(); j++) peak = std::max(peak, spec[j]);
      std::vector<double> nb;
      for (size_t j = size_t(std::max(3.0, 12.0 / binHz)); j <= size_t(30.0 / binHz) + 3; j++) {
        if (k + j < spec.size()) nb.push_back(spec[k + j]);
        if (k > j) nb.push_back(spec[k - j]);
      }
      if (nb.empty()) continue;
      const double over = db(peak) - db(pct(nb, 0.5));
      if (over > a.humDb) { a.humDb = over; a.humHz = f; }
    }
  }
  if (!speechStarts.empty()) {
    const auto spec = averageSpectrum(mono, speechStarts, N);
    a.sibilanceDb = db(bandPower(spec, fs, 4000, std::min(9000.0, 0.48 * fs))) - db(bandPower(spec, fs, 500, 4000));
  }

  // Reverb tail: after speech stops, how much energy is left 160-400 ms later? An event is a stop
  // (level falls >5 dB below the preceding speech) followed by a decay that does not rise again,
  // so it works for slow decays in reverberant rooms as well as fast ones in dry rooms.
  {
    std::vector<double> ratios;
    auto pw = [&](size_t k) { return std::pow(10.0, lv[k] / 10.0); };
    size_t i = 12;
    while (i + 25 < nf) {
      double pre = 0;
      for (size_t k = i - 10; k < i; k++) pre += pw(k);
      pre /= 10;
      const double preDb = 10 * std::log10(pre + 1e-20);
      if (preDb > p95 - 10 && lv[i] < preDb - 5 && lv[i - 1] >= preDb - 5) {
        bool rises = false;
        for (size_t k = i + 1; k < i + 22; k++) if (lv[k] > lv[k - 1] + 2.0) { rises = true; break; }
        if (!rises) {
          double post = 0;
          for (size_t k = i + 8; k < i + 20; k++) post += pw(k);
          ratios.push_back(10 * std::log10(post / 12.0 / pre));
        }
        i += 25;
      } else i++;
    }
    a.reverbEvents = int(ratios.size());
    a.reverbTailDb = ratios.size() >= 5 ? pct(ratios, 0.5) : std::numeric_limits<double>::quiet_NaN();
  }

  // Clipping on the first channel.
  const ClipDetection cl = detectClipping(chp[0], n);
  a.clippedFraction = double(cl.clippedSamples) / double(n);
  return a;
}

std::vector<Suggestion> suggestSettings(const Analysis& a, ChainSettings& s) {
  std::vector<Suggestion> out;
  // Too little audio to measure anything reliably: leave the standard defaults alone.
  if (a.seconds < 10) {
    out.push_back({"auto", "standard settings", "The recording is too short (" + fmt(a.seconds, 1) + " s) to analyse, so the standard settings are used.", "high"});
    return out;
  }
  // Each measurement maps onto the 0-100 slider by interpolating between anchor points. The anchors sit
  // where the old three-way rules switched (so the old Less/Normal/More decisions are the same points on
  // the scale) and continue past them, so a slightly worse recording gets slightly more.
  struct Pt { double x, pct; };
  auto interp = [](std::initializer_list<Pt> pts, double x) {
    const Pt* p = pts.begin();
    const size_t n = pts.size();
    if (x <= p[0].x) return p[0].pct;
    for (size_t i = 1; i < n; i++)
      if (x <= p[i].x) return p[i - 1].pct + (p[i].pct - p[i - 1].pct) * (x - p[i - 1].x) / (p[i].x - p[i - 1].x);
    return p[n - 1].pct;
  };
  auto word = [](double pct) {  // the nearest of the named positions, for display
    return pct < 12.5 ? "off" : pct < 37.5 ? "less" : pct < 62.5 ? "normal" : pct < 87.5 ? "more" : "max";
  };
  auto push = [&](const char* stage, double pct, const std::string& reason, const char* confidence) {
    pct = std::round(std::min(100.0, std::max(0.0, pct)));
    Suggestion g{stage, word(pct), reason, confidence};
    g.amount = pct;
    out.push_back(g);
    return pct;
  };

  // Noise: how far the speech sits above the room tone.
  {
    const double pct = push("noise", interp({{5, 90}, {15, 75}, {30, 50}, {45, 25}, {60, 10}}, a.snrDb),
                            "Speech sits " + fmt(a.snrDb, 0) + " dB above the background noise (" + fmt(a.noiseFloorDb, 0) + " dBFS)" +
                                (a.noiseHfShareDb > -8 ? "; the noise is mostly high-frequency hiss." : "."),
                            "medium");
    s.noiseReduction = Intensity(pct / 100.0);
  }
  if (a.humDb >= 8)
    out.push_back({"noise", "hum notches on",
                   "A steady " + fmt(a.humHz, 0) + " Hz hum line stands " + fmt(a.humDb, 0) + " dB above the surrounding noise.", "high"});

  // Clipping.
  if (a.clippedFraction > 0.0002) {
    s.declip = true;
    out.push_back({"declip", "on", fmt(a.clippedFraction * 100, 2) + "% of samples are flat-topped at the clip level.", "high"});
  } else {
    s.declip = false;
    out.push_back({"declip", "off", "No clipping detected.", "high"});
  }

  // Level changes through the recording.
  s.voiceLeveling = true;
  push("leveling", interp({{0, 30}, {3, 50}, {8, 75}, {14, 90}}, a.levelDriftDb),
       a.levelDriftDb > 3 ? "The speech level drifts by about " + fmt(a.levelDriftDb, 0) + " dB across the recording (mic distance or fader moves)."
                          : "Level is fairly steady (" + fmt(a.levelDriftDb, 1) + " dB drift); leveling will make only small corrections.",
       "medium");

  // Reverb.
  if (std::isnan(a.reverbTailDb)) {
    s.dereverb.reset();
    push("dereverb", 0, "Too few clean pauses to judge the room echo.", "low");
  } else {
    const double pct = push("dereverb", interp({{-34, 0}, {-28, 20}, {-24, 50}, {-16, 75}, {-8, 100}}, a.reverbTailDb),
                            a.reverbTailDb > -16   ? "Sound lingers strongly after each phrase (tail " + fmt(a.reverbTailDb, 0) + " dB below the speech): a reverberant room."
                            : a.reverbTailDb > -24 ? "Some room echo after each phrase (tail " + fmt(a.reverbTailDb, 0) + " dB below the speech)."
                                                   : "The room is fairly dry (echo tail " + fmt(a.reverbTailDb, 0) + " dB below the speech).",
                            "low");
    if (pct >= 1) s.dereverb = Intensity(pct / 100.0);
    else s.dereverb.reset();
  }

  // De-esser.
  {
    const double pct = push("deess", interp({{-24, 10}, {-18, 25}, {-11, 50}, {-5, 75}, {0, 90}}, a.sibilanceDb),
                            "High-frequency (4-9 kHz) energy is " + fmt(a.sibilanceDb, 0) + " dB relative to the voice body.", "low");
    s.deEsser = Intensity(pct / 100.0);
  }

  // Compressor: how wide the loudness varies.
  {
    const double pct = push("compressor", interp({{2, 10}, {4, 25}, {8, 50}, {12, 75}, {20, 90}}, a.loudnessRangeLu),
                            "Loudness varies over a range of " + fmt(a.loudnessRangeLu, 0) + " LU.", "medium");
    s.compressor = Intensity(pct / 100.0);
  }

  if (a.channels == 2 && a.channelCorrelation > 0.95)
    out.push_back({"export", "mono", "Left and right are nearly identical (correlation " + fmt(a.channelCorrelation, 2) + "), so mono loses nothing and halves the file size.", "high"});
  return out;
}

}  // namespace sermon
