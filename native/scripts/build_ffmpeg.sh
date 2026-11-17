#!/bin/sh
# Builds a small LGPL FFmpeg (static, no network, no auto-detected system libraries) used by the
# desktop app to convert any audio/video file to WAV before processing. Output:
#   third_party/ffmpeg-install/bin/ffmpeg
# Source: https://github.com/FFmpeg/FFmpeg at a pinned tag. LGPL-2.1+ (no --enable-gpl or
# --enable-nonfree), so it can ship alongside the GPL-3 app as a separate executable.
set -e
TAG=n8.1.3
cd "$(dirname "$0")/../third_party"
[ -d ffmpeg-src ] || git clone --depth 1 --branch "$TAG" https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
cd ffmpeg-src
# ARCH=x86_64 cross-builds the Intel version on an Apple silicon Mac (own source copy, into ffmpeg-install-x86_64).
SUFFIX=""
if [ -n "$ARCH" ] && [ "$ARCH" != "$(uname -m)" ]; then SUFFIX="-$ARCH"; fi
PREFIX="$(cd .. && pwd)/ffmpeg-install$SUFFIX"
export MACOSX_DEPLOYMENT_TARGET=11.0
if [ -n "$SUFFIX" ]; then  # own copy of the source per architecture, built through a space-free path
  cd ..; rm -rf "ffmpeg-src$SUFFIX"; cp -R ffmpeg-src "ffmpeg-src$SUFFIX"
  LINK="${TMPDIR:-/tmp}/sermon-ffmpeg-link$SUFFIX"; rm -f "$LINK"; ln -s "$(pwd)/ffmpeg-src$SUFFIX" "$LINK"
  cd "$LINK"; make distclean >/dev/null 2>&1 || true
fi
set -- ./configure --prefix="$PREFIX" \
  --disable-doc --disable-ffplay --disable-ffprobe --enable-ffmpeg \
  --disable-shared --enable-static --disable-debug --disable-network --disable-autodetect \
  --enable-zlib \
  --disable-encoders --enable-encoder=pcm_s16le,pcm_s24le,pcm_f32le \
  --disable-muxers --enable-muxer=wav \
  --disable-hwaccels
if [ -n "$SUFFIX" ]; then
  set -- "$@" --enable-cross-compile --arch="$ARCH" --target-os=darwin "--cc=clang -arch $ARCH" "--extra-ldflags=-arch $ARCH" --disable-asm
fi
# Windows (MinGW): link everything in, so ffmpeg.exe needs no libwinpthread-1.dll / libz-1.dll next to it.
case "$(uname -s)" in MINGW*|MSYS*) set -- "$@" "--extra-ldflags=-static" ;; esac
"$@"
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
mkdir -p "$PREFIX/bin"
EXE=""; [ -f ffmpeg.exe ] && EXE=".exe"   # MinGW (Windows) produces ffmpeg.exe
cp "ffmpeg$EXE" "$PREFIX/bin/ffmpeg$EXE"
[ -z "$SUFFIX" ] && "$PREFIX/bin/ffmpeg$EXE" -hide_banner -version | head -3 || file "$PREFIX/bin/ffmpeg$EXE"
