#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "audio_io.h"

namespace sermon {

struct Mp3Preset {
  const char* id;
  const char* label;
  const char* description;
  int channels;     // 0 = same as the source (mono stays mono, anything else is stereo)
  int sampleRate;
  int bitrateKbps;  // CBR bitrate, or 0 for VBR
  int vbrQuality;   // LAME VBR quality 0 (best) .. 9 (smallest); used when bitrateKbps == 0
};

// The app's three download presets (same as the browser app).
const std::vector<Mp3Preset>& mp3Presets();

struct Mp3Tags {
  std::string title, artist, album;  // empty = omitted
};

// True if this build includes LAME.
bool mp3EncodingAvailable();

struct Mp3Options {
  // Highest true peak (dBTP) the *decoded* MP3 may have. Lossy encoding and
  // resampling add 1-3 dB of overshoot, so the encoder runs closed-loop: encode,
  // decode, measure the true peak, and if it's over, tighten the pre-encode
  // limit by the excess and try again. NaN disables the loop.
  double peakCeilingDb = -1.0;
  int maxPasses = 8;
  // Each correction aims this far under the ceiling, because the re-encode moves
  // quantization noise onto peaks that were just under it.
  double marginDb = 0.5;  // 0.3: 7 passes, -0.2 LU; 0.8: 5 passes, -0.5 LU; 1.8: 2 passes, -1.1 LU (22 kHz VBR)
};

struct Mp3Report {
  double decodedTruePeakDb = 0;  // of the returned file, as decoded
  int passes = 0;
};

// Resamples to the preset rate, downmixes if needed, encodes with LAME, and
// writes a Xing/Info header so players show the right duration. Throws
// std::runtime_error if LAME isn't built in or encoding fails.
std::vector<uint8_t> encodeMp3(const AudioData& audio, const Mp3Preset& preset, const Mp3Tags& tags = {},
                               const Mp3Options& options = {}, Mp3Report* report = nullptr);

// Estimated output size in bytes without encoding (exact for CBR).
size_t estimateMp3Size(const AudioData& audio, const Mp3Preset& preset);

}  // namespace sermon
