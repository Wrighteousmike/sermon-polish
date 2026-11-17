# Making a release

Everything ships **unsigned** (decision: keep the app free). Users get the right-click-Open (Mac) or
"More info, Run anyway" (Windows) steps in the install notes.

## What has and hasn't been verified

| Target | Status |
|---|---|
| macOS Apple silicon | Built and self-tested locally many times (`Sermon Polish --selftest <file>` prints PASS). |
| macOS Intel | Cross-compiled on Apple silicon (`ARCH=x86_64`, see below); the engine runs under Rosetta. **Not run on a real Intel Mac.** |
| Windows 10/11 x64 | Code made portable (UTF-8 paths, binary stdio, MinGW static link) and a CI job written (`.github/workflows/release.yml`). **Never built or run**: the first CI run is a shakedown. |

## Local macOS builds

Rust, downloads and scratch live on the external drive (see `native/README.md`).

```sh
# one-time: sources and third-party libraries for this Mac's architecture
native/scripts/fetch_deps.sh
native/scripts/build_lame.sh && native/scripts/build_ffmpeg.sh && native/scripts/build_deepfilternet.sh
cmake -S native -B native/build && cmake --build native/build
scripts/prepare-sidecar.sh                 # copies sermon_clean, sermon_host, ffmpeg + the model into src-tauri/
npx tauri build --bundles app              # then package the .app as dmg/zip (hdiutil, ditto)
"src-tauri/target/release/bundle/macos/Sermon Polish.app/Contents/MacOS/sermon-polish" --selftest some.wav
```

### Intel build on an Apple silicon Mac

```sh
rustup target add x86_64-apple-darwin
ARCH=x86_64 native/scripts/build_lame.sh
ARCH=x86_64 native/scripts/build_ffmpeg.sh
ARCH=x86_64 native/scripts/build_deepfilternet.sh
cmake -S native -B native/build-x86_64 -DCMAKE_OSX_ARCHITECTURES=x86_64 -DSERMON_ARCH=x86_64 -DSERMON_DF_TARGET=x86_64-apple-darwin
cmake --build native/build-x86_64
TARGET_TRIPLE=x86_64-apple-darwin NATIVE_BUILD=native/build-x86_64 FFMPEG_DIR=native/third_party/ffmpeg-install-x86_64 scripts/prepare-sidecar.sh
npx tauri build --target x86_64-apple-darwin --bundles app
arch -x86_64 native/build-x86_64/rt_tests        # run the engine tests under Rosetta
```

All macOS builds pin `MACOSX_DEPLOYMENT_TARGET=11.0` (the scripts and CMake do), so the app runs on macOS 11+.

## CI

`.github/workflows/release.yml` (run by hand or on a `v*` tag) builds Apple silicon, Intel and Windows
installers and uploads them as artifacts. Windows builds the C++ engine, FFmpeg and LAME with MinGW-w64
(static, so the sidecar `.exe`s need no DLLs) and DeepFilterNet for `x86_64-pc-windows-gnu`; Tauri uses MSVC.

Things most likely to need fixing on the first Windows run: the FFmpeg/LAME `configure` flags under MSYS2,
linking `libdf.a` (Rust std system libraries are listed in `native/CMakeLists.txt`), and audio device
selection in miniaudio (WASAPI is the default).

## Before publishing

- Bump the version in `src-tauri/tauri.conf.json`, `src-tauri/Cargo.toml`, `package.json`.
- Re-read `docs/THIRD_PARTY_NOTICES.md` (bundled: DeepFilterNet3 model + libDF, RNNoise, LAME, FFmpeg LGPL,
  dr_libs, miniaudio) and make sure each bundled program's licence text ships with the app.
- Install notes: `release/INSTALL.txt` (Mac); write the equivalent for Windows ("More info, Run anyway").
- The app is GPL-3: publish the source for exactly what you ship (FFmpeg is LGPL: ship the build script
  and the pinned tag, `native/scripts/build_ffmpeg.sh`).
