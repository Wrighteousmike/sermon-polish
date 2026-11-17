#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "chain.h"
#include "corrected_source.h"

namespace sermon::rt {

struct RenderRequest {
  std::string sourceWav;  // working file, or the baked one (declip applied)
  std::string correctionWav;      // optional dereverb correction for sourceWav (see CorrectedSource)
  double dereverbStrength = 0;    // how much of it to mix in
  std::string outWav;     // 32-bit float WAV of the cleaned, loudness-matched, faded region
  double startSeconds = 0, endSeconds = 0;
  ChainParams params;     // the settings (gain and limiter fields are overridden here)
  std::shared_ptr<const FileModel> model;
  double targetLufs = -14.0, ceilingDb = -1.0;  // targetLufs NaN = no loudness matching (the limiter still runs)
  double pregainDb = 0;   // extra gain into the limiter after the loudness is matched (output ends up louder than the target)
  double fadeMs = 100;
};

struct RenderReport {
  double lufs = 0, truePeakDb = 0;
  int limiterPasses = 0;
};

// Export render through the same block engine that plays: pass 1 runs the chain (limiter off) over the
// kept region into a temporary file and measures its exact integrated loudness; pass 2 runs only the
// true-peak limiter with the gain that reaches the target (re-gaining by any shortfall the limiter
// causes, up to 4 dB extra, like the offline engine), then applies the trim fades. Throws
// std::runtime_error ("Cancelled" if `cancel` becomes true).
RenderReport renderExport(const RenderRequest& request, const std::function<void(const char* label, double fraction)>& progress,
                          const std::atomic<bool>& cancel);

}  // namespace sermon::rt
