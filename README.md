# Sermon Polish

Free, open-source audio clean-up for recorded sermons and talks. Everything runs on your own computer; nothing is uploaded.

Drop in a recording (MP3, WAV, M4A, or a video file) and clean it up with simple Off-to-Max sliders: noise, room echo, de-essing, compression, tone and loudness. Auto suggests starting settings, and a final limiter holds the peaks at -1 dB for YouTube and Spotify.

## Download

Go to the **[latest release](../../releases/latest)** and pick the file for your computer:

- **Mac, Apple silicon** (M1/M2/M3/M4): `Sermon Polish ... (Apple silicon).dmg`
- **Mac, Intel**: `Sermon Polish ... (Intel).dmg`
- **Windows 10/11**: the `setup.exe` installer, once it is attached to the release

## First-time open

The app is not signed with a paid developer certificate (that keeps it free), so you will see a one-time warning.

- **Mac:** open the .dmg and drag Sermon Polish to Applications. Right-click the app, choose Open, then Open again. If macOS refuses, go to System Settings > Privacy & Security and click "Open Anyway".
- **Windows:** run the installer. If "Windows protected your PC" appears, click "More info", then "Run anyway".

## Status

Alpha. The Mac version has had the most testing; the Intel Mac and Windows versions have not been tried on real machines yet. Feedback is welcome in the Issues tab.


## Updates

Once a day the app asks GitHub whether a newer release exists, and tells you in the sidebar if so; you can also press **Check for updates** there. Nothing about you or your recordings is sent, and nothing is installed automatically. You download the new version yourself and install it over the old one.

## Feedback and feature requests

Found a bug or want a feature? Open an [issue](../../issues) here, or email devcrest.support@gmail.com. When reporting a problem, say whether you're on Mac or Windows, and what you were doing when it happened.

## Source code and building

Sermon Polish is free software under the GPL-3.0 (see `LICENSE`). The text below is for developers who want to build it.

A free, open-source ministry tool: drop in a raw sermon recording (audio or video), and play it
through a live, DAW-style cleaning chain (noise, hum, echo, level, tone, sharp S sounds, loudness)
while you change any setting; set the start and end, and export WAV or MP3. It runs entirely on your computer; nothing is uploaded.

**Status: desktop alpha for Mac (Apple silicon); Intel Mac and Windows builds are being prepared (`docs/release.md`).** See `docs/alpha-notes.md` for what works, what
hasn't been tested, and the known limits. Install files and instructions are in `release/`.

### How it's built

- **Desktop shell:** Tauri v2 (`src-tauri/`) with the React UI (`src/`).
- **Audio engine:** C++17 (`native/`), a real-time block engine run as a sidecar process; DeepFilterNet3 and RNNoise for
  noise, our own declipper, leveler, dereverb (WPE), EQ, de-esser, compressor and true-peak
  limiter, and LAME for MP3. See `native/README.md` and `docs/app-architecture.md`.
- **Auto mode:** the engine analyses the recording and suggests every setting with a reason; the
  user can override anything (`docs/auto-analysis.md`).

### Building from source

```sh
(cd native && ./scripts/fetch_deps.sh && ./scripts/build_lame.sh && ./scripts/build_deepfilternet.sh)
cmake -S native -B native/build && cmake --build native/build       # the engine
./scripts/prepare-sidecar.sh                                         # copy it into the app
npm install && npx tauri build --bundles app                         # needs a Rust toolchain
```

Rust, downloads and build output are kept on the external drive in this setup (see
`native/README.md`). The old browser-only version is in git history (before the native pivot).

### License

Intended GPL-3.
