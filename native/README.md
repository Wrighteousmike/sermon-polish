# Native sermon-cleanup engine

C++17 audio engine for the native Mac/Windows app. Framework-agnostic: no
JUCE or GUI dependency. Free and open source (GPL-3 intended).

## Build

```sh
./scripts/fetch_deps.sh     # RNNoise (+ model), dr_libs, LAME sources into third_party/ (gitignored)
./scripts/build_lame.sh     # static LAME into third_party/lame-install
./scripts/build_ffmpeg.sh   # LGPL ffmpeg (the app's file converter) into third_party/ffmpeg-install
# DeepFilterNet needs Rust. It lives on the external drive (nothing in ~):
export RUSTUP_HOME="$HOME/toolchains/rustup" CARGO_HOME="$HOME/toolchains/cargo"
export PATH="$CARGO_HOME/bin:$PATH"
./scripts/build_deepfilternet.sh   # libdf.a (~3 min); model ships in the repo
cmake -S . -B build && cmake --build build
./build/dsp_tests && ./build/stage_tests
```

Without the third-party sources the library still builds: noise reduction
and MP3 export compile out (`noiseReductionAvailable()` /
`mp3EncodingAvailable()` report it). Downloads and builds stay on the
external drive; nothing is installed system-wide.

## Real-time engine (src/rt)

The app uses `sermon_host`, a long-lived process that plays a recording through the chain in real time and
renders the export (see `docs/app-architecture.md`). `host_test` plays a working file on a silent device and
reports underruns/CPU; `rt_tests` checks every streaming stage against the offline one, block-size invariance
and the resampler. `scripts/make_test_audio.sh out.wav` makes a synthetic test file.

## Run a file through the chain (command line)

```sh
./build/sermon_clean in.mp3 out.wav --mp3 out-prefix --tone ref --title "My sermon"
```

Options: `--noise/--deess/--compress/--dereverb less|normal|more|off`,
`--tone 1|2|3|ref|off`, `--dereverb-first`, `--no-normalize --no-declip
--no-level --no-limit --no-subsonic`, `--seconds N` (first N seconds only),
`--mp3 <prefix>` (writes all three presets). Run it with no arguments for usage.

## Auto mode (default)

`sermon_clean` analyses the recording first (`src/analysis.cpp`, ~2 s for 46 min) and applies
the suggested chain settings, printing the reason for each; explicit options override them;
`--manual` uses the fixed defaults; `--analyze` only reports. See `docs/auto-analysis.md`.

## The chain (src/pipeline.cpp)

All at 48 kHz float internally (input is resampled).

1. peak normalize (-3 dBFS)
2. **declip** (`declip_spade`): sparse-model A-SPADE; auto-detects clip
   levels; frames too heavily clipped are left alone
3. subsonic highpass (40 Hz, 4th order)
4. **leveler** (`leveler`): offline lookahead; K-weighted loudness, local
   speech level with a pause-aware gate, one smooth gain curve for all channels
5. hum notches (50/100/150 Hz) then **noise reduction**: DeepFilterNet3 by default
   (intensity = attenuation limit 10 / 20 dB / unlimited), RNNoise as fallback
   (`--nr-backend rnnoise`; intensity = wet/dry blend)
6. **dereverb** (`wpe`): single-channel WPE on a 16 kHz band; order vs noise
   reduction is switchable (`--dereverb-first`)
7. EQ (`voice_eq`): Tone 1/2/3 from the browser app, or `ref`: fitted to the
   user's Pro-Q 3 curve (see `docs/protools-reference-chain.md`)
8. de-esser, 9. compressor (threshold derived from integrated loudness)
10. 12th-order Butterworth 11 kHz lowpass (encoder hiss)
11. limiter: -14 LUFS integrated, -1 dBTP **true** peak (4x oversampled); if
    limiting costs loudness it re-gains by the shortfall (up to 4 dB extra) and
    reports gain-reduction percentiles (`--lufs`, `--ceiling`, `--limiter-extra`)
12. export (`mp3_export`): LAME, three presets, ID3 tags, Xing header

Reasoning for the order: `docs/dereverb-and-chain-order.md`.

## Layout

- `src/` engine; `tests/` (`dsp_tests`: leveler, declipper; `stage_tests`:
  everything else, each stage with a measurable property)
- `tools/sermon_clean.cpp` CLI; `tools/trace_proq_curve.py` and
  `tools/fit_proq_curve.py` (trace a Pro-Q 3 screenshot's response curve and
  fit biquads to it)
- `scripts/` dependency fetch/build

## Known limits / open items

- Noise reduction: DeepFilterNet3 keeps the speech spectrum within ~0.2 dB but
  removes little high-frequency hiss (6-10 kHz); RNNoise removes more hiss but
  dulls speech and gates pauses to silence. A dedicated HF-hiss stage is an idea.
  Backend latencies (RNNoise 959, DeepFilterNet 1440 samples) were calibrated
  on real speech; `SERMON_TEST_SPEECH=<raw f32 file> ./build/stage_tests`
  re-checks them.
- WPE dereverb is modest (about 3.5 dB of tail on simulated RT60 0.6 s; about
  1 dB on the real test file) and adds ~110 s per 46 minutes. Stronger neural
  options are listed in `docs/dereverb-and-chain-order.md`.
- Declipper helps light/moderate clipping only (a few % of samples); it does
  nothing for MP3-decoded overshoot (no flat tops).
- The command-line pipeline and the baking of declip/dereverb hold the whole file in memory (a 46-minute
  mono file is ~530 MB as 48 kHz float); the real-time engine itself streams from disk.
- Windows and Intel Mac builds are untested.
