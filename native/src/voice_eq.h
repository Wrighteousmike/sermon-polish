#pragma once

#include <cstddef>
#include <vector>

#include "biquad.h"

namespace sermon {

// Reference follows the Pro Tools Pro-Q 3 curve read from the user's
// screenshots (see docs/protools-reference-chain.md); bell widths are
// estimated from the curve shapes.
enum class Tone { Tone1, Tone2, Tone3, Reference };

// The biquads for a tone at a sample rate (one channel's worth, in processing order), shared by
// the offline voiceEq() and the live EQ stage.
// `gainScale` multiplies the gain of every cut/boost band of the Reference curve (1 = the fitted
// curve, 0 = flat apart from the filters' corners, >1 = more corrected); it is ignored for the other tones.
std::vector<Biquad> voiceEqChain(double sampleRate, Tone tone, double gainScale = 1.0);

// Static voice EQ curve: highpass, four peaking cuts (low mud, boxiness,
// honk, harshness), lowpass, a shared brightness shelf, and Tone 3's 1-10kHz
// tilt. Port of src/lib/dsp/voiceEq.ts, keeping Web Audio's Q-in-dB
// convention for the highpass/lowpass so the presets sound as tuned.
// Processes one channel in place.
void voiceEq(float* samples, size_t length, double sampleRate, Tone tone);

}  // namespace sermon
