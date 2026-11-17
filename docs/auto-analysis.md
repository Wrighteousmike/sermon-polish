# Auto analysis: measure the recording, suggest a chain (2026-10-29)

**Auto is the default mode** (owner's decision). `sermon_clean in.mp3 ...` analyses the file
(about 2 s for 46 minutes) and applies the suggested settings; any explicit option still
overrides them; `--manual` (alias `--no-auto`) uses the fixed standard settings instead.
Recordings under 10 s are not analysed and get the standard settings, with a note.
`--analyze` just prints the measurements and suggestions without processing. In `--json` mode
the `analysis` event carries the measurements and `[{stage, setting, reason, confidence}]` for
the UI to show; the `result` event echoes the settings actually used.

No machine learning: plain signal measurements and explicit rules, so every suggestion can be
explained in a sentence and the user can always override or reset to defaults.

## Measurements (src/analysis.cpp)

| Measure | How | Drives |
|---|---|---|
| SNR / noise floor | speech level (95th pct of 20 ms frames) minus quietest-decile level | noise reduction strength (>=45 dB less, 30-45 normal, <30 more) |
| Noise colour | noise-window energy above 4 kHz vs 100 Hz-4 kHz | wording only (hiss) |
| Hum | 50/60 Hz and harmonics vs neighbours, in windows that are quiet throughout | hum notches (note) |
| Clipping | clip-level detection (same as the declipper) | declip on/off (>0.02% of samples) |
| Level drift | spread of 20 s median speech level | wording (leveling always on) |
| Echo tail | energy 160-400 ms after speech stops, for stops followed by a non-rising decay | dereverb (> -16 dB more, > -24 normal, else off) |
| Sibilance | 4-9 kHz vs 0.5-4 kHz in speech frames | de-esser (> -8 more, > -14 normal, else less) |
| Loudness range | 400 ms K-weighted blocks, 95th - 10th pct | compressor (>10 LU more, 6-10 normal, <6 less) |
| Stereo correlation | L/R | mono export suggestion (>0.95) |

## Calibration status: be honest

Thresholds come from physics plus three real files and synthetic tests, not from a corpus.
Confidence labels reflect that: clipping, hum and the mono suggestion are "high"; SNR, level and
compressor "medium"; echo tail and sibilance "low".

Data points so far (Wash your wife original -> Pro Tools final): SNR 44 -> 43 dB; echo tail
-31 -> -28 dB (dry room, dereverb adds little); sibilance -10 -> -17 dB (the Pro Tools chain
pulled the 4-9 kHz band down ~7 dB); loudness range 13.7 -> 10.6 LU; level drift 2.8 -> 1.3 dB.
The synthetic reverberant room (RT60 0.9 s) measures -20.6 dB.

To calibrate properly: collect 10-20 recordings with the settings the owner would choose, and
fit the thresholds to those choices. The "overrides" the owner makes in the UI are themselves
that data and could be remembered.

## What part of the recording is analysed

The analysis covers **the region the user keeps**, not the whole upload and not a short sample:

- With a trim (`--trim START END`), exactly that region (the trim is applied before the analysis).
- Without one, on recordings over 10 minutes, everything except the first 60 s and last 30 s
  (announcements, the tech setting levels, outro).
- The result reports the range it used (`analyzedFrom` / `analyzedTo`).

Why not a short sample from the middle? Tested on the real file (whole file as the reference):

| Sample | SNR | Sibilance | Echo tail | Loudness range |
|---|---|---|---|---|
| whole file (46 min) | 44 dB | -10 dB | -31 dB (52 pauses) | 13.7 LU |
| 5 s, six places in the middle | 29-46 dB | -13 to -23 dB | impossible (0-1 pauses) | 7.5-16.8 LU |
| 60 s, six places in the middle | 36-46 dB | -11 to -16 dB | mostly impossible | 12-14 LU |
| first 2 s | 20 dB | -8 dB | impossible | 17.5 LU |

A single 5 s sample can land on a pause, a laugh or a loud passage: one of them would have
tripled the noise-reduction strength, and none can measure room echo. Percentiles over
the whole kept region are far steadier, and a minute of fiddling at the start is only a few
percent of them.

## What the app UI must do

- Auto selected by default; show each suggestion with its plain-language reason and confidence.
- Every setting editable. Each card shows its explanation pill only while *its own* setting is the
  suggested one: change it and that pill goes (the others stay); put it back and the pill returns.
  The banner says "Custom" only while something differs from the suggestions.
- "Reset to Auto" restores the suggestions; "Standard settings" selects the fixed defaults.
- Low-confidence suggestions (echo tail, sibilance) are shown as such.
- Optional: a 30 s before/after preview (trim + process an excerpt) so the user can hear a choice.

## Tests

`stage_tests` `testAnalysisFindsProblems`: clean audio gets no hum/clipping/dereverb flags; a
50 Hz hum is found; clipping turns declip on; a reverberant room turns dereverb on; heavy noise
raises noise reduction; dual-mono stereo suggests mono.

## Resolved: the de-esser did nothing on real speech (2026-10-29), rewritten the same day

On the first 15 minutes of the real file the sibilance measure (4-9 kHz vs 0.5-4 kHz in speech)
was -7 dB for de-ess off, less, normal AND more: the old absolute thresholds were never reached
after leveling. The Pro Tools chain took the same material from -10 to -17 dB.

The new de-esser (`native/src/deesser.cpp`) is level-independent: it scores each 2.5 ms slice by
sibilance relative to the voice body, learns what is normal for this speaker from the file's own
distribution, and turns down only slices above a percentile of it, with a Linkwitz-Riley crossover
at 4 kHz (the old subtract-the-band design could add energy through phase shift), 5 ms lookahead,
and a floor so pauses and hiss are never touched. On the real recording it moves the measure by
-2 / -3 / -6 dB (less / normal / more) and leaves everything below 3 kHz unchanged.
