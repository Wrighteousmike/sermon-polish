#include "mp3_export.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "limiter.h"
#include "loudness.h"
#include "resample.h"

#ifdef SERMON_HAVE_LAME
#include <lame/lame.h>
#endif

namespace sermon {

const std::vector<Mp3Preset>& mp3Presets() {
  static const std::vector<Mp3Preset> presets = {
      {"mono-320", "Highest quality", "Mono, 44.1kHz, 320kbps", 1, 44100, 320, 0},
      {"mono-192", "Standard", "Mono, 44.1kHz, 192kbps", 1, 44100, 192, 0},
      {"mono-small", "Smallest file", "Mono, 22.05kHz, variable bitrate", 1, 22050, 0, 8},
  };
  return presets;
}

namespace {
int resolveChannels(const AudioData& a, const Mp3Preset& p) {
  return p.channels ? p.channels : (a.channels.size() >= 2 ? 2 : 1);
}
}  // namespace

size_t estimateMp3Size(const AudioData& a, const Mp3Preset& p) {
  const double seconds = double(a.length()) / double(a.sampleRate);
  if (p.bitrateKbps) return size_t(seconds * p.bitrateKbps * 1000.0 / 8.0 + 0.999);
  // VBR: V8 at 22.05 kHz mono measured ~36 kbps on a 46-minute sermon.
  return size_t(seconds * 36000.0 / 8.0);
}

#ifdef SERMON_HAVE_LAME

bool mp3EncodingAvailable() { return true; }

namespace {

// Resample to the preset rate and fix up the channel count.
std::vector<std::vector<float>> preparePcm(const AudioData& audio, const Mp3Preset& preset, int ch) {
  std::vector<std::vector<float>> pcm;
  for (const auto& c : audio.channels) pcm.push_back(resample(c.data(), c.size(), audio.sampleRate, preset.sampleRate));
  if (ch == 1 && pcm.size() > 1) {
    std::vector<float> mono(pcm[0].size(), 0.0f);
    for (auto& c : pcm)
      for (size_t i = 0; i < mono.size(); i++) mono[i] += c[i] / float(pcm.size());
    pcm.assign(1, std::move(mono));
  } else if (ch == 2 && pcm.size() == 1) {
    pcm.push_back(pcm[0]);
  }
  return pcm;
}

std::vector<uint8_t> encodeOnce(const std::vector<std::vector<float>>& pcm, const Mp3Preset& preset, int ch, const Mp3Tags& tags) {
  const size_t n = pcm[0].size();
  lame_t g = lame_init();
  if (!g) throw std::runtime_error("lame_init failed");
  lame_set_in_samplerate(g, preset.sampleRate);
  lame_set_out_samplerate(g, preset.sampleRate);
  lame_set_num_channels(g, ch);
  lame_set_mode(g, ch == 1 ? MONO : JOINT_STEREO);
  if (preset.bitrateKbps) {
    lame_set_VBR(g, vbr_off);
    lame_set_brate(g, preset.bitrateKbps);
  } else {
    lame_set_VBR(g, vbr_mtrh);
    lame_set_VBR_q(g, preset.vbrQuality);
  }
  lame_set_quality(g, 2);  // slow/high-quality algorithm; speed is not a concern offline
  lame_set_bWriteVbrTag(g, 1);
  id3tag_init(g);
  id3tag_add_v2(g);
  id3tag_v2_only(g);
  if (!tags.title.empty()) id3tag_set_title(g, tags.title.c_str());
  if (!tags.artist.empty()) id3tag_set_artist(g, tags.artist.c_str());
  if (!tags.album.empty()) id3tag_set_album(g, tags.album.c_str());
  if (lame_init_params(g) < 0) {
    lame_close(g);
    throw std::runtime_error("lame_init_params failed (unsupported rate/bitrate combination?)");
  }

  std::vector<uint8_t> out;
  const size_t id3Len = lame_get_id3v2_tag(g, nullptr, 0);
  if (id3Len) {
    out.resize(id3Len);
    lame_get_id3v2_tag(g, out.data(), id3Len);
  }

  constexpr size_t kBlock = 8192;
  std::vector<uint8_t> buf(size_t(1.25 * kBlock) + 7200);
  for (size_t i = 0; i < n; i += kBlock) {
    const int m = int(std::min(kBlock, n - i));
    const int bytes = ch == 1 ? lame_encode_buffer_ieee_float(g, pcm[0].data() + i, nullptr, m, buf.data(), int(buf.size()))
                              : lame_encode_buffer_ieee_float(g, pcm[0].data() + i, pcm[1].data() + i, m, buf.data(), int(buf.size()));
    if (bytes < 0) { lame_close(g); throw std::runtime_error("lame encode error " + std::to_string(bytes)); }
    out.insert(out.end(), buf.begin(), buf.begin() + bytes);
  }
  const int tail = lame_encode_flush(g, buf.data(), int(buf.size()));
  if (tail > 0) out.insert(out.end(), buf.begin(), buf.begin() + tail);

  // Overwrite the placeholder first frame with the finished Xing/Info header.
  std::vector<uint8_t> tagFrame(16384);
  const size_t tagLen = lame_get_lametag_frame(g, tagFrame.data(), tagFrame.size());
  if (tagLen > 0 && tagLen <= tagFrame.size() && id3Len + tagLen <= out.size())
    std::memcpy(out.data() + id3Len, tagFrame.data(), tagLen);
  lame_close(g);
  return out;
}

double decodedTruePeakDb(const std::vector<uint8_t>& mp3) {
  const AudioData d = decodeMp3(mp3);
  std::vector<const float*> p;
  for (const auto& c : d.channels) p.push_back(c.data());
  return 20.0 * std::log10(std::max(truePeak(p.data(), p.size(), d.length()), 1e-9));
}

}  // namespace

std::vector<uint8_t> encodeMp3(const AudioData& audio, const Mp3Preset& preset, const Mp3Tags& tags, const Mp3Options& opt,
                               Mp3Report* report) {
  if (audio.channels.empty()) throw std::runtime_error("no audio to encode");
  const int ch = resolveChannels(audio, preset);
  std::vector<std::vector<float>> pcm = preparePcm(audio, preset, ch);

  if (std::isnan(opt.peakCeilingDb)) {
    auto bytes = encodeOnce(pcm, preset, ch, tags);
    if (report) { report->passes = 1; report->decodedTruePeakDb = decodedTruePeakDb(bytes); }
    return bytes;
  }

  // Closed loop. Lossy encoding inflates a few transients, so instead of lowering the global
  // limit (which costs loudness everywhere), decode the result, find where the decoded true
  // peak is over the ceiling, and dip the gain only there before re-encoding.
  const double kMarginDb = opt.marginDb;
  const double ceilingLin = std::pow(10.0, (opt.peakCeilingDb - kMarginDb) / 20.0);
  std::vector<float*> ptrs;
  for (auto& c : pcm) ptrs.push_back(c.data());
  const size_t n = pcm[0].size();
  enforceTruePeak(ptrs.data(), ptrs.size(), n, preset.sampleRate, opt.peakCeilingDb - kMarginDb);

  std::vector<uint8_t> best;
  double bestPeak = 1e9;
  int pass = 0;
  for (; pass < std::max(1, opt.maxPasses); pass++) {
    auto bytes = encodeOnce(pcm, preset, ch, tags);
    const AudioData dec = decodeMp3(bytes);
    std::vector<const float*> dp;
    for (const auto& c : dec.channels) dp.push_back(c.data());
    const double peak = 20.0 * std::log10(std::max(truePeak(dp.data(), dp.size(), dec.length()), 1e-9));
    if (std::getenv("SERMON_MP3_DEBUG")) std::fprintf(stderr, "  [mp3 %s pass %d] decoded true peak %.2f dBTP\n", preset.id, pass, peak);
    if (peak < bestPeak) { bestPeak = peak; best = std::move(bytes); }
    if (peak <= opt.peakCeilingDb + 0.05) { pass++; break; }

    // The decoder output lags the input (encoder + decoder delay): find it by correlation on a
    // loud stretch, then map each overshoot back to the sample that caused it.
    const size_t win = std::min<size_t>(preset.sampleRate, n / 4);
    size_t at = n / 2;  // fall back to the middle
    double bestE = 0;
    for (size_t s0 = 0; s0 + win < n; s0 += win) {  // loudest 1 s window
      double e = 0;
      for (size_t i = s0; i < s0 + win; i++) e += double(pcm[0][i]) * pcm[0][i];
      if (e > bestE) { bestE = e; at = s0; }
    }
    long lag = 0;
    double bestC = -1e30;
    for (long l = 0; l < 4000; l += 1) {
      if (at + win + size_t(l) >= dec.length()) break;
      double c = 0;
      for (size_t i = 0; i < win; i += 2) c += double(pcm[0][at + i]) * dec.channels[0][at + i + size_t(l)];
      if (c > bestC) { bestC = c; lag = l; }
    }
    const size_t decLen = dec.length();
    applyRequiredGainFn(ptrs.data(), ptrs.size(), n, preset.sampleRate, [&](size_t i) {
      const size_t j = i + size_t(lag);
      if (j >= decLen) return 1.0f;
      const double tp = truePeakAt(dp.data(), dp.size(), decLen, j);
      return tp > ceilingLin ? float(ceilingLin / tp) : 1.0f;
    });
  }
  if (report) { report->passes = pass; report->decodedTruePeakDb = bestPeak; }
  return best;
}

#else

bool mp3EncodingAvailable() { return false; }

std::vector<uint8_t> encodeMp3(const AudioData&, const Mp3Preset&, const Mp3Tags&) {
  throw std::runtime_error("this build has no MP3 encoder (LAME not found; run scripts/build_lame.sh)");
}

#endif

}  // namespace sermon
