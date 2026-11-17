# Alpha notes (0.1.0)

Install files are in `release/` (gitignored): the DMG, a zip, and `INSTALL.txt`. Mac, Apple
silicon, macOS 11+. Unsigned (right-click, Open the first time).

## What was and was not tested

Tested by the developer (Claude), with automated checks:
- The engine: ~120 unit/stage tests, sanitizer runs, real-recording measurements.
- The packaged app finds its engine and the DeepFilterNet model *inside the bundle* and streams
  results (`Sermon Polish --selftest <file>` prints PASS).
- Every screen (upload, trim, clean-up with Auto, processing progress, download) rendered in a
  headless browser against a mocked engine; the Auto analysis flow and the progress UI work there.

**Not tested** (the developer cannot drive the real window or hear audio):
- The real window: file picker and drag-and-drop of a file, the save dialog, and quitting mid-way.
- Waveform loading of a long MP3 through the app's asset protocol, and playback from it.
- "Hear original / Hear cleaned" playback in the macOS webview (16-bit WAV preview).
- How anything *sounds*, apart from the numbers (loudness, peaks, spectra).
- Windows (not built at all).
- The conversion screen in the real window (rendered against a fake converter, and the packaged
  app's converter was tested through the self-test).

Trim: besides the overview, each marker has a zoomed view (2 minutes down to half a second across),
click/drag or nudge by 1 s / 0.1 s (arrow keys: 0.1 s, Shift 1 s, Alt 0.01 s), "Set to playhead", and
"Hear from here" / "Hear up to here". Checked against a fake recording and the real peaks file; the
dragging itself is untested in the real window.

First things to try: open an MP3 or video, watch the Auto suggestions appear, press Space and change settings while it plays (Simple or
Advanced view, Cleaned/Original), set the start and end (Fine trim zooms in), Export and save a format.

## Known limits

- **Any audio or video file works** (M4A/AAC, FLAC, OGG/Opus, WMA, AIFF, and video such as
  MP4/MOV/MKV/AVI/WebM): the app converts it first with a bundled FFmpeg. Verified on 12 formats
  and with video files; untested through the real window. Videos with several audio tracks use the
  first track; surround is mixed down to stereo.
- **Memory:** a 43-minute mono sermon now peaks at ~1.0 GB (was 3.3 GB); a 1-hour stereo one
  should need ~2.5 GB. The pipeline still processes the whole file stage by stage (see
  docs/app-architecture.md) rather than streaming block by block.
- **Disk:** converted files and the cleaned audio are kept as lossless temp files (~0.5 GB per 45
  min mono; a 2-hour stereo video converts to ~2 GB) until "Start again"/quit; leftovers are cleared
  at next launch.
- **Speed:** playback uses about 3-6% of a core; the export renders at roughly 10-15x realtime (a 45 min sermon in a few minutes); declip/dereverb baking takes about a minute for a long recording.
- Apple silicon only so far. Intel Mac and Windows builds are prepared but unbuilt/untested (see docs/release.md).
- Tone default is "Studio match" (the Pro-Q fit; Tones 1-3 are sharper at the top end, per the user). Dereverb defaults to off unless Auto
  measures real echo. The new de-esser (rewritten today) is calibrated on one recording.
- Auto's thresholds rest on three recordings; echo and sibilance suggestions are marked "rough guess".
- No undo history for settings or trim.
- Live loudness while playing is an estimate from sampled excerpts; the exported file is exact (-14 LUFS).

## Feedback that helps most

For each recording: what Auto chose, whether you changed anything and why, and what you'd call
wrong about the result in plain words ("too dull", "still hissy at the start", "pumping").
Those overrides are the data that makes Auto better.
