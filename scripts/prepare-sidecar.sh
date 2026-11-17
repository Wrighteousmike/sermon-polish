#!/bin/sh
# Copies the built C++ engine, the bundled ffmpeg converter and the DeepFilterNet model into the
# Tauri project. Tauri sidecars are named <name>-<target triple>.
#   Native build:      cmake -S native -B native/build && cmake --build native/build && scripts/prepare-sidecar.sh
#   Intel on Apple silicon (cross):
#     ARCH=x86_64 native/scripts/build_lame.sh / build_ffmpeg.sh / build_deepfilternet.sh
#     cmake -S native -B native/build-x86_64 -DCMAKE_OSX_ARCHITECTURES=x86_64 -DSERMON_ARCH=x86_64 -DSERMON_DF_TARGET=x86_64-apple-darwin && cmake --build native/build-x86_64
#     TARGET_TRIPLE=x86_64-apple-darwin NATIVE_BUILD=native/build-x86_64 FFMPEG_DIR=native/third_party/ffmpeg-install-x86_64 scripts/prepare-sidecar.sh
set -e
cd "$(dirname "$0")/.."
TRIPLE="${TARGET_TRIPLE:-$(rustc -vV | sed -n 's/^host: //p')}"
EXT=""; case "$TRIPLE" in *windows*) EXT=".exe";; esac
BUILD="${NATIVE_BUILD:-native/build}"
FFMPEG_DIR="${FFMPEG_DIR:-native/third_party/ffmpeg-install}"
mkdir -p src-tauri/binaries src-tauri/resources
cp "$BUILD/sermon_clean$EXT" "src-tauri/binaries/sermon_clean-$TRIPLE$EXT"
cp "$BUILD/sermon_host$EXT" "src-tauri/binaries/sermon_host-$TRIPLE$EXT"
cp "$FFMPEG_DIR/bin/ffmpeg$EXT" "src-tauri/binaries/ffmpeg-$TRIPLE$EXT"
cp native/third_party/DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz src-tauri/resources/
cp docs/THIRD_PARTY_NOTICES.md src-tauri/resources/
echo "sidecars for $TRIPLE: src-tauri/binaries/{sermon_clean,sermon_host,ffmpeg}-$TRIPLE$EXT"
