#pragma once

#include <cstddef>

namespace sermon {

enum class DeclipIntensity { Less, Normal, More };

// Basic declipper: finds runs of consecutive near-full-scale samples and
// replaces each flat-topped run with a smoothstep curve between the clean
// samples just outside it. Port of src/lib/dsp/declip.ts. Operates in place
// on one channel; call once per channel.
void declip(float* samples, size_t length, DeclipIntensity intensity = DeclipIntensity::Normal);

}  // namespace sermon
