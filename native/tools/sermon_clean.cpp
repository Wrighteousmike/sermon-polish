// sermon_clean: runs a recording through the cleanup chain from the command line.
//
// Two output styles:
//  - human-readable (default), for people at a terminal
//  - --json: one JSON object per line on stdout, which is the engine protocol the desktop
//    app's UI reads (see docs/app-architecture.md). Events: probe, progress, result, error.
#include <algorithm>
#include "fsutil.h"
#include <chrono>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include "analysis.h"
#include "audio_io.h"
#include "loudness.h"
#include "mp3_export.h"
#include "wav_export.h"
#include "noise_reduction.h"
#include "pipeline.h"
#include "trim.h"

namespace {

void usage() {
  std::printf(
      "usage: sermon_clean <in.wav|in.mp3> [<out.wav>|-] [options]\n"
      "  --probe              only inspect the input: duration, loudness, waveform peaks\n"
      "  --analyze            measure the recording and print suggested settings (no processing)\n"
      "  (default)            Auto mode: analyse the recording and apply the suggested settings;\n"
      "                       any explicit option below still overrides them\n"
      "  --manual             skip the analysis and use the fixed standard settings (alias --no-auto)\n"
      "  --peaks FILE         write waveform peaks (JSON) to FILE; --peaks-buckets N (default 2000)\n"
      "  --peaks-bin FILE     write 1 ms-resolution peaks (binary) for the zoomed trim views\n"
      "  --trim START END     keep only START..END seconds, with fades (--fade-ms N, default 100)\n"
      "  --seconds N          process only the first N seconds (quick test)\n"
      "  --noise off|less|normal|more|max|0-100 (default normal)\n"
      "  --nr-backend auto|rnnoise|df     (default auto: DeepFilterNet if built)\n"
      "  --dereverb off|less|normal|more|max|0-100 (default normal); --dereverb-first\n"
      "  --tone 1|2|3|ref|off             (default 1; ref = Pro Tools Pro-Q curve)\n"
      "  --deess off|less|normal|more|max|0-100 (default normal)\n"
      "  --compress off|less|normal|more|max|0-100 (default normal)\n"
      "  --lufs N  --ceiling N  --limiter-extra N   loudness target / true-peak ceiling / extra push\n"
      "  --no-normalize --no-declip --no-level --no-limit --no-subsonic\n"
      "  --wav16              write the output WAV as 16-bit PCM (for playback) instead of 32-bit float\n"
      "  --mp3 <prefix>       also export MP3s as <prefix>-<preset>.mp3 (--preset ID for just one)\n"
      "  --wav-mono16 PATH    write a mono 16-bit 44.1 kHz WAV (dithered, peak kept under the export ceiling)\n"
      "  --mp3-out PATH       with --preset ID: write that one MP3 to exactly PATH\n"
      "  --no-chain           skip analysis and processing (just trim/convert/export what was read)\n"
      "  --export-ceiling N   true-peak ceiling of the *decoded* MP3s in dBTP (default -1)\n"
      "  --title T --artist A --album L   ID3 tags\n"
      "  --json               machine-readable events, one JSON object per line\n");
}

bool parseIntensity(const char* v, std::optional<sermon::Intensity>& out) { return sermon::parseIntensity(std::string(v), out); }

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
  }
  return o;
}

// JSON numbers can't be NaN/inf.
std::string num(double v) {
  if (!std::isfinite(v)) return "null";
  char b[64];
  std::snprintf(b, sizeof b, "%.4f", v);
  return b;
}

std::string intensityName(const std::optional<sermon::Intensity>& i) {
  if (!i) return "off";
  char b[16];
  std::snprintf(b, sizeof b, "%.0f", i->amount * 100.0);
  return b;
}

const char* toneName(const std::optional<sermon::Tone>& t) {
  if (!t) return "off";
  switch (*t) {
    case sermon::Tone::Tone1: return "1";
    case sermon::Tone::Tone2: return "2";
    case sermon::Tone::Tone3: return "3";
    default: return "ref";
  }
}

struct Loudness { double lufs, truePeakDb; };

Loudness measure(const sermon::AudioData& a) {
  std::vector<const float*> p;
  for (auto& c : a.channels) p.push_back(c.data());
  return {sermon::integratedLufs(p.data(), p.size(), a.length(), a.sampleRate),
          20 * std::log10(std::max(sermon::truePeak(p.data(), p.size(), a.length()), 1e-9))};
}

void writePeaks(const std::string& path, const sermon::AudioData& a, size_t buckets) {
  const std::vector<float> peaks = sermon::computePeaks(a, buckets);
  std::ofstream f(sermon::pathOf(path));
  f << "{\"buckets\":" << buckets << ",\"duration\":" << num(double(a.length()) / a.sampleRate) << ",\"peaks\":[";
  for (size_t i = 0; i < peaks.size(); i++) f << (i ? "," : "") << num(peaks[i]);
  f << "]}\n";
}

}  // namespace

int main(int argc, char** argv) {
  sermon::binaryStdio();
  const std::vector<std::string> utf8Argv = sermon::utf8Args(argc, argv);
  std::vector<char*> argvPtrs;
  for (const auto& a : utf8Argv) argvPtrs.push_back(const_cast<char*>(a.c_str()));
  argv = argvPtrs.data();
  if (argc < 2) { usage(); return 2; }
  sermon::ChainSettings s;
  std::string inPath = argv[1], outWav, mp3Prefix, onlyPreset, peaksPath, peaksBinPath;
  double seconds = 0, trimStart = -1, trimEnd = -1, fadeMs = 100;
  size_t peakBuckets = 2000;
  std::string wavMono16Path;
  bool json = false, probe = false, noChain = false, wav16 = false;
  std::string mp3OutPath;
  sermon::Mp3Tags tags;
  sermon::Mp3Options mp3Options;

  int firstOption = 2;
  if (firstOption < argc && argv[firstOption][0] != '-') outWav = argv[firstOption++];
  else if (firstOption < argc && std::strcmp(argv[firstOption], "-") == 0) firstOption++;
  if (outWav == "-") outWav.clear();
  bool analyzeOnly = false, autoSettings = true;  // Auto is the default
  // Parses the options into `target` (re-run on top of the suggested settings for --auto).
  auto parseOptions = [&](sermon::ChainSettings& target) -> bool {
  for (int i = firstOption; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--json") json = true;
    else if (a == "--analyze") analyzeOnly = true;
    else if (a == "--auto") autoSettings = true;
    else if (a == "--manual" || a == "--no-auto") autoSettings = false;
    else if (a == "--probe") probe = true;
    else if (a == "--peaks") peaksPath = next();
    else if (a == "--peaks-bin") peaksBinPath = next();
    else if (a == "--peaks-buckets") peakBuckets = size_t(std::max(1, std::atoi(next())));
    else if (a == "--trim") { trimStart = std::atof(next()); trimEnd = std::atof(next()); }
    else if (a == "--fade-ms") fadeMs = std::atof(next());
    else if (a == "--seconds") seconds = std::atof(next());
    else if (a == "--noise") { if (!parseIntensity(next(), target.noiseReduction)) { usage(); return false; } }
    else if (a == "--nr-backend") {
      const std::string v = next();
      if (v == "auto") target.noiseBackend = sermon::NoiseBackend::Auto;
      else if (v == "rnnoise") target.noiseBackend = sermon::NoiseBackend::RNNoise;
      else if (v == "df") target.noiseBackend = sermon::NoiseBackend::DeepFilterNet;
      else { usage(); return false; }
    }
    else if (a == "--dereverb") { if (!parseIntensity(next(), target.dereverb)) { usage(); return false; } }
    else if (a == "--dereverb-first") target.dereverbBeforeDenoise = true;
    else if (a == "--tone") {
      const std::string v = next();
      if (v == "1") target.tone = sermon::Tone::Tone1;
      else if (v == "2") target.tone = sermon::Tone::Tone2;
      else if (v == "3") target.tone = sermon::Tone::Tone3;
      else if (v == "ref") target.tone = sermon::Tone::Reference;
      else if (v == "off") target.tone.reset();
      else { usage(); return false; }
    }
    else if (a == "--deess") { if (!parseIntensity(next(), target.deEsser)) { usage(); return false; } }
    else if (a == "--compress") { if (!parseIntensity(next(), target.compressor)) { usage(); return false; } }
    else if (a == "--lufs") target.targetLufs = std::atof(next());
    else if (a == "--ceiling") target.ceilingDb = std::atof(next());
    else if (a == "--limiter-extra") target.limiterMaxExtraDb = std::atof(next());
    else if (a == "--no-normalize") target.normalize = false;
    else if (a == "--no-declip") target.declip = false;
    else if (a == "--no-level") target.voiceLeveling = false;
    else if (a == "--no-limit") target.limiter = false;
    else if (a == "--no-subsonic") target.subsonicFilter = false;
    else if (a == "--mp3") mp3Prefix = next();
    else if (a == "--preset") onlyPreset = next();
    else if (a == "--wav-mono16") wavMono16Path = next();
    else if (a == "--mp3-out") mp3OutPath = next();
    else if (a == "--no-chain") noChain = true;
    else if (a == "--wav16") wav16 = true;
    else if (a == "--export-ceiling") mp3Options.peakCeilingDb = std::atof(next());
    else if (a == "--title") tags.title = next();
    else if (a == "--artist") tags.artist = next();
    else if (a == "--album") tags.album = next();
    else { usage(); return false; }
  }
  return true;
  };
  if (!parseOptions(s)) return 2;

  auto fail = [&](const std::string& msg) {
    if (json) std::printf("{\"event\":\"error\",\"message\":\"%s\"}\n", jsonEscape(msg).c_str());
    else std::printf("error: %s\n", msg.c_str());
    std::fflush(stdout);
    return 1;
  };

  try {
    sermon::AudioData audio = sermon::readAudioFile(inPath);
    if (seconds > 0)
      for (auto& c : audio.channels) c.resize(std::min(c.size(), size_t(seconds * audio.sampleRate)));
    const bool probeLike = probe || !peaksPath.empty() || !peaksBinPath.empty();
    const bool userTrimmed = trimStart >= 0 && trimEnd > trimStart;
    // Trim first, so the analysis (and the report) cover only what the user keeps: the intro, the
    // tech setting levels, and the outro don't belong in the measurements.
    if (!probeLike && userTrimmed) sermon::trimWithFades(audio, trimStart, trimEnd, fadeMs);
    const Loudness in = measure(audio);
    const double inSeconds = double(audio.length()) / audio.sampleRate;

    if (analyzeOnly || (autoSettings && !probe && !noChain)) {
      // Untrimmed long recordings: skip the first minute and the last 30 s (announcements, level
      // setting, outro). A user trim already excludes them.
      constexpr double kLongSeconds = 600, kSkipStart = 60, kSkipEnd = 30;
      double fromS = userTrimmed ? trimStart : 0, toS = userTrimmed ? trimEnd : inSeconds;
      size_t fromSample = 0, toSample = audio.length();
      if (!userTrimmed && inSeconds > kLongSeconds) {
        fromSample = size_t(kSkipStart * audio.sampleRate);
        toSample = audio.length() - size_t(kSkipEnd * audio.sampleRate);
        fromS = kSkipStart;
        toS = inSeconds - kSkipEnd;
      }
      const sermon::Analysis an = sermon::analyzeAudio(audio, fromSample, toSample);
      sermon::ChainSettings suggested;
      const auto suggestions = sermon::suggestSettings(an, suggested);
      if (json) {
        std::printf("{\"event\":\"analysis\",\"analyzedFrom\":%s,\"analyzedTo\":%s,\"measurements\":{\"seconds\":%s,\"sampleRate\":%d,\"channels\":%zu,\"lufs\":%s,\"loudnessRangeLu\":%s,\"truePeakDb\":%s,\"speechLevelDb\":%s,\"noiseFloorDb\":%s,\"snrDb\":%s,\"noiseHfShareDb\":%s,\"humDb\":%s,\"humHz\":%s,\"clippedFraction\":%s,\"levelDriftDb\":%s,\"reverbTailDb\":%s,\"reverbEvents\":%d,\"sibilanceDb\":%s,\"channelCorrelation\":%s},\"suggestions\":[",
                    num(fromS).c_str(), num(toS).c_str(), num(an.seconds).c_str(), an.sampleRate, an.channels, num(an.lufs).c_str(), num(an.loudnessRangeLu).c_str(), num(an.truePeakDb).c_str(),
                    num(an.speechLevelDb).c_str(), num(an.noiseFloorDb).c_str(), num(an.snrDb).c_str(), num(an.noiseHfShareDb).c_str(), num(an.humDb).c_str(),
                    num(an.humHz).c_str(), num(an.clippedFraction).c_str(), num(an.levelDriftDb).c_str(), num(an.reverbTailDb).c_str(), an.reverbEvents,
                    num(an.sibilanceDb).c_str(), num(an.channelCorrelation).c_str());
        for (size_t k = 0; k < suggestions.size(); k++)
          std::printf("%s{\"stage\":\"%s\",\"setting\":\"%s\",\"amount\":%.0f,\"reason\":\"%s\",\"confidence\":\"%s\"}", k ? "," : "", jsonEscape(suggestions[k].stage).c_str(),
                      jsonEscape(suggestions[k].setting).c_str(), suggestions[k].amount, jsonEscape(suggestions[k].reason).c_str(), suggestions[k].confidence.c_str());
        std::printf("]}\n");
      } else {
        std::printf("analysis of %s, %02d:%02d to %02d:%02d (%.0f s, %d Hz, %zu ch)\n", inPath.c_str(), int(fromS) / 60, int(fromS) % 60, int(toS) / 60, int(toS) % 60, an.seconds, an.sampleRate, an.channels);
        std::printf("  loudness %.1f LUFS (range %.1f LU), true peak %.1f dBTP | speech %.1f dBFS, noise floor %.1f dBFS, SNR %.0f dB\n", an.lufs, an.loudnessRangeLu, an.truePeakDb, an.speechLevelDb, an.noiseFloorDb, an.snrDb);
        std::printf("  hum %.0f dB at %.0f Hz | noise HF share %.0f dB | sibilance %.0f dB | level drift %.1f dB | clipped %.3f%% | echo tail %s dB (%d pauses)\n", an.humDb, an.humHz, an.noiseHfShareDb, an.sibilanceDb, an.levelDriftDb, an.clippedFraction * 100, std::isnan(an.reverbTailDb) ? "n/a" : num(an.reverbTailDb).c_str(), an.reverbEvents);
        std::printf("suggested settings:\n");
        for (const auto& g : suggestions) std::printf("  %-10s %-14s [%s] %s\n", g.stage.c_str(), g.setting.c_str(), g.confidence.c_str(), g.reason.c_str());
      }
      if (analyzeOnly) return 0;
      s = suggested;
      if (!parseOptions(s)) return 2;  // explicit options override the suggestions
    }

    if (!peaksPath.empty() || !peaksBinPath.empty() || probe) {
      if (!peaksPath.empty()) writePeaks(peaksPath, audio, peakBuckets);
      if (!peaksBinPath.empty()) sermon::writePeaksFine(peaksBinPath, audio, 1.0);
      if (json)
        std::printf("{\"event\":\"probe\",\"path\":\"%s\",\"sampleRate\":%d,\"channels\":%zu,\"seconds\":%s,\"lufs\":%s,\"truePeakDb\":%s,\"peaks\":%s,\"peaksBin\":%s}\n",
                    jsonEscape(inPath).c_str(), audio.sampleRate, audio.channels.size(), num(inSeconds).c_str(), num(in.lufs).c_str(),
                    num(in.truePeakDb).c_str(), peaksPath.empty() ? "null" : ("\"" + jsonEscape(peaksPath) + "\"").c_str(),
                    peaksBinPath.empty() ? "null" : ("\"" + jsonEscape(peaksBinPath) + "\"").c_str());
      else
        std::printf("input    %d Hz, %zu ch, %.1f s | %.1f LUFS, true peak %.2f dBTP\n", audio.sampleRate, audio.channels.size(), inSeconds, in.lufs, in.truePeakDb);
      if (probe) return 0;
    }

    if (probeLike && userTrimmed) sermon::trimWithFades(audio, trimStart, trimEnd, fadeMs);
    if (!json) {
      std::printf("input    %d Hz, %zu ch, %.1f s | %.1f LUFS, true peak %.2f dBTP\n", audio.sampleRate, audio.channels.size(), double(audio.length()) / audio.sampleRate, in.lufs, in.truePeakDb);
      if (s.noiseReduction && !sermon::noiseBackendAvailable(s.noiseBackend))
        std::printf("note: requested noise backend not built in, skipping noise reduction\n");
      else if (s.noiseReduction)
        std::printf("noise reduction: %s\n", sermon::noiseBackendName(s.noiseBackend));
    }

    const auto t0 = std::chrono::steady_clock::now();
    const bool memTrace = std::getenv("SERMON_MEM_TRACE") != nullptr;  // development: peak memory per stage
    #ifdef _WIN32
    auto peakMb = [] { return 0.0; };  // memory trace is macOS/Linux only
#else
    auto peakMb = [] { struct rusage u; getrusage(RUSAGE_SELF, &u); return double(u.ru_maxrss) / (1024.0 * 1024.0); };  // bytes on macOS
#endif
    if (memTrace) std::fprintf(stderr, "[mem] before chain: peak %.0f MB (the audio itself is %.0f MB as float)\n", peakMb(), double(audio.length() * audio.channels.size() * 4) / 1048576.0);
    sermon::ChainReport rep;
    if (!noChain) rep = sermon::runChain(audio, s, [&](const char* label, int step, int total) {
      if (memTrace) std::fprintf(stderr, "[mem] starting '%s': peak so far %.0f MB\n", label, peakMb());
      if (json) std::printf("{\"event\":\"progress\",\"label\":\"%s\",\"step\":%d,\"total\":%d}\n", jsonEscape(label).c_str(), step, total);
      else std::printf("  [%d/%d] %s\n", step, total, label);
      std::fflush(stdout);
    });
    const double chainSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (memTrace) std::fprintf(stderr, "[mem] chain finished: peak %.0f MB\n", peakMb());
    const Loudness out = measure(audio);

    if (!outWav.empty()) sermon::writeWav(outWav, audio, wav16 ? 16 : 32);

    if (!wavMono16Path.empty()) {
      if (json) std::printf("{\"event\":\"progress\",\"label\":\"Writing WAV…\",\"step\":0,\"total\":0}\n");
      std::fflush(stdout);
      sermon::exportWavMono16(audio, wavMono16Path, 44100, mp3Options.peakCeilingDb);
    }

    struct Output { std::string preset, path; size_t bytes; double decodedPeak; int passes; };
    std::vector<Output> outputs;
    if (!mp3Prefix.empty() || !mp3OutPath.empty()) {
      for (const auto& p : sermon::mp3Presets()) {
        if (!onlyPreset.empty() && onlyPreset != p.id) continue;
        if (mp3Prefix.empty() && onlyPreset.empty()) continue;  // --mp3-out needs --preset
        if (json) std::printf("{\"event\":\"progress\",\"label\":\"Encoding %s…\",\"step\":0,\"total\":0}\n", jsonEscape(p.label).c_str());
        std::fflush(stdout);
        sermon::Mp3Report mrep;
        const auto bytes = sermon::encodeMp3(audio, p, tags, mp3Options, &mrep);
        const std::string path = !mp3OutPath.empty() ? mp3OutPath : mp3Prefix + "-" + p.id + ".mp3";
        std::ofstream(sermon::pathOf(path), std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        outputs.push_back({p.id, path, bytes.size(), mrep.decodedTruePeakDb, mrep.passes});
      }
    }

    if (json) {
      bool modelFound = false;
      const std::string modelPath = sermon::noiseModelPath(&modelFound);
      std::printf("{\"event\":\"result\",\"noiseModel\":\"%s\",\"noiseModelFound\":%s,\"settings\":{\"tone\":\"%s\",\"noise\":\"%s\",\"dereverb\":\"%s\",\"deess\":\"%s\",\"compressor\":\"%s\",\"declip\":%s,\"leveling\":%s,\"targetLufs\":%s},\"chainSeconds\":%s,\"noiseBackend\":\"%s\",\"input\":{\"lufs\":%s,\"truePeakDb\":%s},"
                  "\"output\":{\"sampleRate\":%d,\"seconds\":%s,\"lufs\":%s,\"truePeakDb\":%s,\"wav\":%s},"
                  "\"limiter\":{\"finalLufs\":%s,\"reductionDb\":{\"median\":%s,\"p90\":%s,\"p99\":%s,\"max\":%s}},\"outputs\":[",
                  jsonEscape(modelPath).c_str(), modelFound ? "true" : "false",
                  toneName(s.tone), intensityName(s.noiseReduction).c_str(), intensityName(s.dereverb).c_str(), intensityName(s.deEsser).c_str(), intensityName(s.compressor).c_str(),
                  s.declip ? "true" : "false", s.voiceLeveling ? "true" : "false", num(s.targetLufs).c_str(),
                  num(chainSeconds).c_str(), sermon::noiseBackendName(s.noiseBackend), num(in.lufs).c_str(), num(in.truePeakDb).c_str(),
                  audio.sampleRate, num(double(audio.length()) / audio.sampleRate).c_str(), num(out.lufs).c_str(), num(out.truePeakDb).c_str(),
                  outWav.empty() ? "null" : ("\"" + jsonEscape(outWav) + "\"").c_str(), num(rep.limiterFinalLufs).c_str(),
                  num(rep.limiterMedianReductionDb).c_str(), num(rep.limiterP90ReductionDb).c_str(), num(rep.limiterP99ReductionDb).c_str(),
                  num(rep.limiterMaxReductionDb).c_str());
      for (size_t k = 0; k < outputs.size(); k++)
        std::printf("%s{\"preset\":\"%s\",\"path\":\"%s\",\"bytes\":%zu,\"decodedTruePeakDb\":%s,\"passes\":%d}", k ? "," : "",
                    outputs[k].preset.c_str(), jsonEscape(outputs[k].path).c_str(), outputs[k].bytes, num(outputs[k].decodedPeak).c_str(), outputs[k].passes);
      std::printf("]}\n");
    } else {
      if (s.limiter)
        std::printf("limiter: reached %.2f LUFS | gain reduction per 10 ms of speech: median %.1f dB, 90th pct %.1f dB, 99th pct %.1f dB, max %.1f dB\n",
                    rep.limiterFinalLufs, rep.limiterMedianReductionDb, rep.limiterP90ReductionDb, rep.limiterP99ReductionDb, rep.limiterMaxReductionDb);
      std::printf("chain time %.1f s\n", chainSeconds);
      std::printf("output   %d Hz, %zu ch, %.1f s | %.1f LUFS, true peak %.2f dBTP\n", audio.sampleRate, audio.channels.size(), double(audio.length()) / audio.sampleRate, out.lufs, out.truePeakDb);
      for (const auto& o : outputs)
        std::printf("  %-14s %.1f MB, decoded true peak %.2f dBTP (%d pass%s) -> %s\n", o.preset.c_str(), o.bytes / 1e6, o.decodedPeak, o.passes, o.passes == 1 ? "" : "es", o.path.c_str());
    }
    std::fflush(stdout);
    return 0;
  } catch (const std::exception& e) {
    return fail(e.what());
  }
}
