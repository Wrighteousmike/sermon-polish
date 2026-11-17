# Desktop app architecture (decided 2026-10-29)

**Tauri v2 shell + a React UI + the C++ engine as sidecar processes.**

```
 React UI (Studio: transport, waveform + trim, Simple/Advanced settings, Export screen)
        |  Tauri commands / events (Rust, thin: run_sidecar, write_sidecar, cancel_engine)
        v
 sermon_host (C++, long-lived sidecar)  <- native/ ; JSON lines in (stdin) and out (stdout)
        |-- block engine (native/src/rt): plays the recording through the live chain via miniaudio,
        |   bakes declip/dereverb in the background, solves loudness, renders the export
        |-- DeepFilterNet3, RNNoise, LAME, our DSP (all statically linked)
 sermon_clean (C++, one-shot sidecar)   <- probe, peaks, Auto analysis, MP3/WAV encoding
 ffmpeg (LGPL, one-shot sidecar)        <- converts any audio/video file to WAV
```

## Why this, and not the alternatives

- **Web UI skills and tooling** give a polished cross-platform UI cheaply (the studio was redesigned from
  the old browser wizard).
- **One codebase for macOS and Windows**, small installers (system webview instead of a bundled
  browser), built from one CI matrix.
- **The engine is isolated.** A 46-minute file is ~530 MB of float audio and the chain makes
  copies; a separate process keeps that memory and any crash away from the UI, can be killed to
  cancel, and is trivially testable from a terminal (it already is: `sermon_clean`).
- **Licensing is clean.** Tauri is MIT/Apache; the engine and its models are GPL-compatible.
- Rejected: **JUCE** (AGPL-3 for open-source use, and the whole UI, waveform and trim view would
  be rewritten in C++; its strength is real-time audio and plugin hosting, which we don't need),
  **Electron** (150 MB+ installers for no gain here), **Qt/Flutter** (UI rewrite).

## Engine protocol (implemented: `sermon_clean --json`)

One JSON object per line on stdout. The UI never parses human text.

| Invocation | Events |
|---|---|
| `sermon_clean in.mp3 --probe --peaks peaks.json --json` | `probe` {sampleRate, channels, seconds, lufs, truePeakDb, peaks} |
| `sermon_clean in.mp3 - --trim 12.5 2700 --mp3 out/name --preset mono-small [chain options] --json` | `progress` {label, step, total} ..., `result` {chainSeconds, input/output loudness, limiter reduction percentiles, outputs[{preset, path, bytes, decodedTruePeakDb, passes}]} |
| `sermon_clean in.mp3 --analyze --json` | `analysis` {measurements, suggestions[{stage, setting, reason, confidence}]} (see `docs/auto-analysis.md`) |
| add `--auto` to a process run | the `analysis` event, then progress/result with `settings` echoed |
| any failure | `error` {message} (exit code 1) |

Waveform: `peaks.json` is `{buckets, duration, peaks:[min0,max0,...]}`, which WaveSurfer.js can
draw directly (precomputed peaks, no browser decoding of a 46-minute file). Preview of the
processed result: render an excerpt with `--trim` and play the WAV in an `<audio>` element.
Cancel: kill the process.

## Distribution: the one real cost

Keeping the *app* free is easy; making it *install without scary warnings* is not.

- **macOS:** no free route to a Developer ID signature. Apple's Developer Program is $99/year (from
  memory; my search did not confirm the figure). Unsigned apps are blocked by Gatekeeper unless
  the user right-click-opens or changes a setting ([Apple's help](https://help.apple.com/xcode/mac/current/en.lproj/dev033e997ca.html)).
- **Windows:** [SignPath Foundation](https://signpath.org/) offers free signing for qualifying
  open-source projects (OSI license + public repo; confirm current terms). Otherwise SmartScreen
  warns on first run.
- Tauri sidecars need one binary per target triple (`sermon_clean-aarch64-apple-darwin`,
  `-x86_64-apple-darwin`, `-x86_64-pc-windows-msvc.exe`) and each must be signed too
  ([Tauri sidecar docs](https://v2.tauri.app/develop/sidecar)).

**Decision (owner, 2026-10-29): ship unsigned to keep it free.** Releases get plain-language
install instructions (Mac: right-click, Open; Windows: More info, Run anyway). Revisit Windows
signing via SignPath Foundation if the SmartScreen warning proves a barrier, and the $99 Apple
program only if Mac installs prove too hard for users.

## Cross-platform status

- Engine portability fixes done (no GCC-only builtins; 64-bit sample indices on Windows).
- **Windows build is untested** (no Windows machine here): `.github/workflows/native.yml` is a
  first CI matrix (macOS arm64 + Windows) that builds the engine without the optional backends
  and runs the tests. Next: LAME, RNNoise and DeepFilterNet (Rust, MSVC target) on Windows.
- macOS: pin `MACOSX_DEPLOYMENT_TARGET` (e.g. 11.0) for the Rust and C++ builds so the app runs
  on older systems (the current local build links objects built for a newer macOS).
- WebView2 is preinstalled on Windows 11; Tauri's installer bootstraps it on older Windows.

## Processing model: real-time block engine (since 2026-10-29)

The studio is DAW-style: the recording plays through the chain live (Rumble, Leveler, Hum, Noise, Tone,
De-ess, Compressor, Lowpass, Limiter) and every setting can be changed while it plays. Everything the
whole file is needed for (leveler gain plan, de-esser statistics, compressor reference loudness) is learned in
two streaming passes when the file is opened, then applied block by block.

- **Working file:** the source is decoded once to a 48 kHz float WAV on disk (`prepareWorkingFile`) and read in
  blocks, so memory does not depend on length.
- **Playback:** a render thread keeps ~0.3 s ahead in a ring buffer; the audio callback (2048-frame period)
  only copies from it, so it never waits on processing. Seeks re-prime the chain with 0.5 s of pre-roll.
- **Settings changes:** ~0.17 s of already-queued audio keeps playing while the audio after it is re-rendered with
  the new settings and crossfaded in (5 ms), so a change is heard in ~0.15 s without a gap.
- **Declip and dereverb** cannot run block by block; a `Baker` thread processes the working file once (about a
  minute for a long recording) into a second file and the host swaps to it with the same crossfade. They run
  before noise reduction, and the export uses the same order. "Original" (A/B) always plays the untouched file.
- **Loudness:** a solver thread runs a private chain over six 8 s excerpts of the kept region to set the output
  gain for -14 LUFS while playing. The export does it exactly: pass 1 runs the chain (limiter off) over the kept
  region and measures integrated loudness; pass 2 runs the true-peak limiter with the gain that reaches
  -14 LUFS (re-gaining by any shortfall, up to 4 dB), then applies the 100 ms trim fades.
- **Export formats** (Export screen): mono 16-bit 44.1 kHz WAV (dithered), MP3 mono 44.1 kHz 320 kbps, 192 kbps,
  and 22.05 kHz VBR 8, all with the decoded-true-peak control. The cleaned recording is rendered once and reused.
- **Host protocol** (`native/tools/sermon_host.cpp`): commands `open, play, pause, seek, trim, loop, ab, params,
  render, cancelRender, shutdown`; events `opened, state (20 Hz), loudness, bake, render, rendered, render_error,
  progress, error`.
- Tests: `native/tests/rt_tests.cpp` (stage-vs-offline equivalence, block-size invariance, click-free toggling,
  resampler, working file), `native/tools/host_test.cpp` (headless playback on a null device).

The old whole-file pipeline (`native/src/pipeline.cpp`, `sermon_clean`) remains as the command-line tool and is used
by the app for probing, Auto analysis and encoding.

## File conversion

The engine reads WAV and MP3. Anything else goes through a bundled, unmodified LGPL FFmpeg sidecar
(`native/scripts/build_ffmpeg.sh`, n8.1.3, static, no network): the UI inspects the file, extracts the
first audio track to a 24-bit WAV in the scratch folder (surround mixed down to stereo), then probes
that. WAV/MP3 that the engine can't read fall back to the same path. See `docs/THIRD_PARTY_NOTICES.md`.
