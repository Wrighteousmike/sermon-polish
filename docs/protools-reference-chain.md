# Reference chain: the maintainer's Pro Tools sermon chain

Read from screenshots in `Plugin settings/` (gitignored; taken 2026-10-28).
Numbers marked ~ are estimated from node positions or knob angles, not read
from a numeric field. Confirm before tuning against them.

## Insert order

Track "AHTW'F R5_15":

| Slot | Plugin | Role |
|---|---|---|
| A | FabFilter Pro-Q 3 | corrective EQ ("Vocal", Zero Latency) — **read below** |
| B | FabFilter Pro-Q 3 | **bypassed** (confirmed by the maintainer) |
| C | Waves Vocal Rider | automatic level riding |
| D | Waves Clarity Vx Pro | neural noise/ambience removal (preset "Remove HF Noise") |
| E | (empty) | |
| F | Waves Clarity Vx DeReverb | neural dereverb |
| G | (empty) | |
| H | FabFilter Pro-DS | de-esser |
| I | Waves RVox | compressor/gate |
| J | X-FDBK (inactive) | feedback killer, bypassed |

Master: F Pro-Q 3 (flat, used only as an RTA analyser), G Pro-Q 3 (bypassed), H FabFilter Pro-L 2. So the master EQs do nothing.

Order differs from the browser app: EQ first, then leveling, then noise
reduction, dereverb, de-ess, compression; EQ and limiting at the master. No
declipper and no always-on lowpass (the Pro-Q lowpass does that job).

## A: Pro-Q 3 (calibrated from the axis labels; frequency +-3%, gain +-0.5 dB)

| Band | Freq | Gain | Notes |
|---|---|---|---|
| highpass | ~97 Hz | n/a | very steep (curve is gone below ~50 Hz) |
| low shelf | ~130 Hz | ~-1.5 dB | |
| bell | ~200 Hz | ~-5.6 dB | wide |
| bell | ~600 Hz | ~-2.7 dB | |
| bell | ~1.09 kHz | ~-6.2 dB | |
| bell | ~1.77 kHz | ~0 dB | no net effect |
| bell | ~3.3 kHz | ~-3.8 dB | |
| bell (narrow) | ~4.0 kHz | ~-6.5 dB | notch; output dips ~-8.5 dB here |
| bell | ~6.9 kHz | ~-7.8 dB | wide |
| bell (greyed out = disabled) | ~7.3 kHz | ~-4.9 dB | |
| lowpass | ~14.7 kHz | n/a | very steep, curve drops by ~17 kHz |

The app's Tone 1 already matches the first five cuts (200/610/1080 Hz). It
does not have the 3.3 kHz, 4 kHz or 6.9 kHz cuts or the 14.7 kHz lowpass.

## C: Waves Vocal Rider (preset "A: Auto VCA")

Range 12.7 dB, output 0 dB, vocal/music sensitivity both 0, **Fast**, **Target
position +2** (confirmed by the maintainer). From the range fader scale the rider's
window is roughly +7 dB up / -5.6 dB down (about 12.7 dB total), so it only
corrects moderate drift, not a 15-20 dB fader push.

## D: Waves Clarity Vx Pro (preset "A: Remove HF Noise")

Neural network "Broad 1", analysis Single, sensitivity Normal, reflections 0,
ambience gate attack/release 15/15, large Ambience<->Voice dial near the
Voice end. The six band panels show the lower three bands left alone and the
upper three (2 kHz, 5 kHz, 15 kHz) pushed up: it removes noise mostly in the
high frequencies.

## F: Waves Clarity Vx DeReverb

Neural network "Dialogue 1", analysis Single/Stereo, Presence ~45 (slider
about half-way); the main dial's value is not readable.

## H: FabFilter Pro-DS

Mode Single Vocal, **Wide Band** (the whole signal is turned down, not just
the sibilant band), side chain In. Confirmed by the maintainer: **threshold -45 dB,
range +24 dB, lookahead 15 ms, detection range 4-18 kHz**. A -45 dB threshold
with 24 dB of range is very aggressive: it will act on most voiced HF energy,
not only on harsh "s" sounds. Prime suspect for the high-frequency gap below.

## I: Waves RVox

Gate off (Energy -Inf), Comp threshold -15.0, output gain 0.0.

## Master H: FabFilter Pro-L 2 (preset "Loud General Purpose Limiting")

Style Modern, gain +10.0 dB, **lookahead 5 ms, attack 0 ms, auto release,
True Peak Limiting with ceiling -0.1 dBTP** (confirmed by the maintainer), oversampling
off, dither off, loudness meter Short Term with a -14 (Strm) marker. Note the
real final measures +1.0 dBFS true peak after the 38 kbps MP3 re-encode; the
native app keeps its -1 dBTP ceiling to leave room for encoder overshoot.

## Still needed

- Stage-by-stage bounces of one stretch (ideally 5 minutes, WAV): after Pro-Q 3 +
  Vocal Rider, then + Clarity Vx Pro, + DeReverb, + Pro-DS, + RVox, then the
  final. That would show exactly what each plugin does so each can be matched.
- Clarity Vx Pro advanced band values and the DeReverb main dial.

## Measured against the real bounce ("Wash your wife", 2026-10-28)

`Audio/Wash your wife ORIGINAL.mp3` vs `... FINAL.mp3` (both 22.05 kHz mono
MP3, 46 min; final is 9 s shorter). Original: -27.4 LUFS, LRA 7.4, peak
-2.5 dBFS. Final: -14.2 LUFS, LRA 4.6.

Pro-Q 3 (insert A) was fitted to its own traced response curve
(`native/tools/trace_proq_curve.py`, `fit_proq_curve.py`; 0.17 dB rms): 6th-order
highpass at 97 Hz, 4th-order lowpass at 12.9 kHz, low shelf 141 Hz -1.5 dB,
bells (Hz / dB / Q) 199/-5.1/0.70, 631/-2.1/1.15, 1087/-6.1/1.41, 1864/+0.7/0.56,
3431/-4.0/2.03, 4020/-5.9/3.18, 9302/-7.7/0.32. Native `Tone::Reference`.

Speech spectral shape (relative to overall level), native chain with
Reference EQ minus the real final, on the first 15 minutes:

| Hz | 63 | 125 | 250 | 500 | 1k | 2k | 3.15k | 4k | 5k | 6.3k | 8k | 10k |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| dB | -5.7 | -1.5 | 0 | -0.5 | -0.7 | -0.3 | +1.4 | +6.6 | +10.1 | +9.9 | +10.2 | +8.9 |

Below 3 kHz the shape matches within ~1.5 dB (apart from 63 Hz). Above 4 kHz
the real final is ~7-10 dB darker, and it has ~6 dB more at 63 Hz. The
other Pro-Q 3s turned out to be bypassed/flat, so the cause is among Clarity Vx
Pro's high-frequency processing, DeReverb, Pro-DS (wide band, -45 dB
threshold, 24 dB range) and the 38 kbps re-encode. Not yet confirmed.
