#pragma once

#include <string>

#include "audio_io.h"

namespace sermon {

// The "WAV" export: mono, 16-bit PCM at `sampleRate` (44.1 kHz). Downmixes by averaging, resamples,
// lowers the level only if the resampled true peak is over `ceilingDb` (dBTP), and dithers (TPDF)
// into 16 bits. Returns the true peak (dBTP) of what was written. Throws std::runtime_error.
double exportWavMono16(const AudioData& audio, const std::string& path, int sampleRate = 44100, double ceilingDb = -1.0);

}  // namespace sermon
