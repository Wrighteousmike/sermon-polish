# Amount sliders and macro curves

Every stage of the chain is one slider from **0 (off) to 100 (max)** instead of a handful of preset buttons.
Auto still recommends where each slider should start (it shows as a small marker on the track), and "Reset to
Auto" puts everything back.

## The scale

| Position | Name | Meaning |
|---|---|---|
| 0 | Off | stage bypassed |
| 25 | Less | the old "Less" preset, exactly |
| 50 | Normal | the old "Normal" preset, exactly |
| 75 | More | the old "More" preset (noise: see below) |
| 100 | Max | new, past the old More |

Between the named positions everything is interpolated linearly. The old presets are fixed points, so settings
from before the change sound the same at the equivalent position.

## Macro curves (`native/src/amount.h`)

Each curve moves all of a stage's underlying parameters together. The offline stages and the real-time chain
both read the same curves, so the live preview and the export agree.

| Stage | Off | Less (25) | Normal (50) | More (75) | Max (100) |
|---|---|---|---|---|---|
| Noise: DeepFilterNet attenuation limit | 0 dB | 10 | 20 | 45 | no limit (100) |
| Noise: RNNoise wet mix | 0 | 0.5 | 0.75 | 1.0 | 1.0 |
| Noise: hum-notch Q | 12 | 12 | 12 | 8 | 8 |
| De-esser: trigger percentile | 0.99 | 0.95 | 0.93 | 0.90 | 0.85 |
| De-esser: ratio | 1 | 2 | 3 | 4 | 6 |
| De-esser: max reduction | 0 dB | 6 | 7 | 9 | 12 |
| Compressor: ratio | 1 | 4 | 8 | 12 | 16 |
| Compressor: makeup | 0 dB | 0 | 2 | 4 | 6 |
| Compressor: threshold below loudness | 6 dB | 6 | 6 | 6 | 9 |
| Dereverb: WPE strength | 0 | 0.5 | 0.8 | 1.0 | 1.3 |
| Tone: Studio-match band gain scale | 0 | 0.5 | 1.0 | 1.2 | 1.4 |
| Leveler: gain-move scale | 0 | 0.5 | 1.0 | 1.25 | 1.5 |
| Loudness target | unchanged | -18 LUFS | -14 | -12.5 | -11 |

### Why Max stops where it does

- **Noise.** DeepFilterNet's attenuation limit stops mattering past roughly 60 dB (the residual noise is never
  that far above its own floor), so Max is "no limit" with full wet signal. The old More was 100 dB; it now
  sits at 45 dB so the top quarter of the slider does something. In practice the two sound alike.
- **De-esser.** 12 dB at ratio 6 is the top of what keeps a voice natural. Past it, speech starts to lisp.
- **Compressor.** Ratio 16 with a lower threshold is heavy "radio" leveling, short of limiting.
- **Dereverb.** WPE subtracts its prediction of the late reverb; strength 1.0 subtracts exactly the
  prediction. 1.3 over-subtracts a little to dry out a very live room, at the cost of a slightly thinner tail.
  This is the least certain Max and needs listening on real rooms.
- **Tone.** Scales every cut and boost of the Studio-match curve; 1.4x means no band goes past about -8.6 dB.
  The highpass and lowpass corners do not move.
- **Leveler.** 1.5x follows the speaker more closely without audible pumping on the test material.
- **Loudness.** -11 LUFS is about as loud as speech gets before the limiter's work becomes audible.

## Live dereverb

WPE can't run block by block, so the first time the dereverb slider is raised the engine computes the
**correction** (what WPE removes at full strength) for the whole recording in the background, and writes it to a
file next to the working file. From then on playback and export mix `strength x correction` into the audio, so
the slider is instant and no re-processing happens when it moves. This works because WPE subtracts a prediction
of the reverb: `output = input - strength x prediction`, which is linear in strength. A unit test
(`testDereverbCorrectionIsLinear`) checks that the mix matches running WPE directly at that strength, to better
than 40 dB. Changing "Repair clipping" recomputes the correction (it is computed from the declipped audio).

## Auto

The analysis maps each measurement onto the slider by interpolating between anchor points that sit where the
old three-way rules switched. Suggestions carry `amount` (0-100) as well as the old word.

## Repair clipping stays on/off

Clip repair isn't something to tune by ear: if the recording clipped you want it repaired, and if it didn't the
stage does nothing. It stays a switch, set by Auto.
