#pragma once

#include <cstddef>
#include <vector>

namespace sermon {

// ITU-R BS.1770 K-weighting (shelf + RLB highpass, coefficients derived for
// any sample rate), applied to each channel and summed as power (all
// channels weighted 1.0, i.e. L/R/mono — no surround weighting).
//
// Returns the mean-square K-weighted energy of each consecutive `hopSamples`
// chunk (the last partial chunk is dropped).
std::vector<double> kWeightedHopEnergy(const float* const* channels, size_t numChannels, size_t length,
                                       double sampleRate, size_t hopSamples);

// Same, but measures the signal as (sample * scale * gain[i]) without materialising it (gain may
// be null). Lets a limiter's loudness be measured from the untouched audio plus its gain curve.
std::vector<double> kWeightedHopEnergyGained(const float* const* channels, size_t numChannels, size_t length,
                                             double sampleRate, size_t hopSamples, double scale, const float* gain);

// Incremental version of kWeightedHopEnergyGained(): feed the audio in blocks of any size, get the same
// hop energies. Memory stays tiny however long the recording is.
class HopEnergyMeter {
 public:
  HopEnergyMeter(double sampleRate, size_t channels, size_t hopSamples);
  ~HopEnergyMeter();
  // gain: optional per-sample multiplier for this block (null = 1).
  void feed(const float* const* channels, size_t n, const float* gain = nullptr);
  const std::vector<double>& energies() const { return energy_; }

 private:
  struct Impl;
  Impl* impl_;
  std::vector<double> energy_;
};

// BS.1770 gating (absolute -70 LUFS, relative -10 LU) over 400 ms blocks built from 100 ms hop energies.
double integratedLufsFromHops(const std::vector<double>& hopEnergy);

// True-peak magnitude around sample k (4x oversampled, all channels), the same value truePeak()
// reports per sample, computed on demand.
double truePeakAt(const float* const* channels, size_t numChannels, size_t length, size_t k);

// True-peak magnitude at the centre of a 16-sample window w[0..15] = x[k-7 .. k+8] (one channel), the
// same value truePeakAt() gives, for streaming code that only holds a few samples.
double truePeakWindow(const float* w);
// The most any window's interpolated value can exceed its largest sample, as a factor (the sum of the
// interpolation filter's absolute taps). A window whose largest sample times this is no more than the true
// peak found so far cannot raise it, so it needn't be interpolated.
double truePeakBound();

// Mean-square energy -> LUFS (BS.1770 offset). Silence returns -infinity-ish
// (-200).
double energyToLufs(double meanSquare);

// BS.1770 integrated loudness: 400ms blocks (75% overlap), absolute gate at
// -70 LUFS then a relative gate 10 LU below the mean of what's left. Returns
// -70 for silence.
double integratedLufs(const float* const* channels, size_t numChannels, size_t length, double sampleRate);

// True peak (linear) via 4x oversampling with a windowed-sinc interpolator,
// across all channels. Also fills `perSample` (if non-null, length `length`)
// with the max true-peak magnitude around each sample — used by the limiter.
double truePeak(const float* const* channels, size_t numChannels, size_t length, float* perSample = nullptr);

}  // namespace sermon
