#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include "workfile.h"

namespace sermon::rt {

// A working file plus, optionally, a second file holding a correction signal (what dereverb would
// remove, computed once at full strength). Reads return base + strength * correction, so the amount of
// dereverb is a live control: moving it never needs the correction recomputed. The strength is read
// from an atomic owned by the caller and ramped across each block, so changes don't click.
class CorrectedSource : public SampleSource {
 public:
  CorrectedSource(std::unique_ptr<WavFileSource> base, std::unique_ptr<WavFileSource> correction, const std::atomic<double>* strength)
      : base_(std::move(base)), corr_(std::move(correction)), strength_(strength) {
    if (corr_ && (corr_->channels() != base_->channels() || corr_->length() != base_->length())) corr_.reset();
    scratch_.assign(size_t(base_->channels()), std::vector<float>(8192));
  }
  int channels() const override { return base_->channels(); }
  double sampleRate() const override { return base_->sampleRate(); }
  int64_t length() const override { return base_->length(); }
  bool hasCorrection() const { return corr_ != nullptr; }

  int read(float* const* out, int n, int64_t position) override {
    const int got = base_->read(out, n, position);
    if (!corr_ || !strength_ || got <= 0) return got;
    const double target = strength_->load(std::memory_order_relaxed);
    if (target == 0 && last_ == 0) return got;
    const int nCh = base_->channels();
    for (int done = 0; done < got;) {
      const int part = std::min<int>(got - done, int(scratch_[0].size()));
      std::vector<float*> p;
      for (auto& v : scratch_) p.push_back(v.data());
      const int cg = corr_->read(p.data(), part, position + done);
      for (int c = 0; c < nCh; c++)
        for (int i = 0; i < cg; i++) {
          const double w = last_ + (target - last_) * double(done + i + 1) / double(got);
          out[c][done + i] += float(w * double(scratch_[size_t(c)][size_t(i)]));
        }
      done += part;
    }
    last_ = target;
    return got;
  }

 private:
  std::unique_ptr<WavFileSource> base_, corr_;
  const std::atomic<double>* strength_;
  double last_ = 0;
  std::vector<std::vector<float>> scratch_;
};

}  // namespace sermon::rt
