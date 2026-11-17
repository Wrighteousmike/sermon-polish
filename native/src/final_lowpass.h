#pragma once

#include <cstddef>

namespace sermon {

// Steep (72 dB/octave) 11kHz lowpass run last, to tame the hiss MP3 encoding
// adds. A true 12th-order Butterworth built from 6 cascaded biquads with the
// standard pole-pair Q values (the browser version fed these Qs to Web
// Audio, which reads lowpass Q in dB, so it was not actually Butterworth).
// Processes one channel in place. No-op if the corner is at/above Nyquist.
void finalLowpass(float* samples, size_t length, double sampleRate, double cornerHz = 11000);

}  // namespace sermon
