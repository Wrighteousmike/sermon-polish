#!/bin/sh
# Fetches third-party sources into native/third_party (gitignored). Downloads
# land in $DOWNLOAD_DIR (default: a sermon-downloads folder in the temp directory).
set -e
mkdir -p "$(dirname "$0")/../third_party"
cd "$(dirname "$0")/../third_party"
DEFAULT_DL="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/sermon-downloads"
DL="${DOWNLOAD_DIR:-$DEFAULT_DL}"
DL="$(cygpath -u "$DL" 2>/dev/null || echo "$DL")"   # Windows (MSYS2): tar reads "D:\..." as a remote host
mkdir -p "$DL"

# RNNoise (BSD-3) pinned, plus its model weights (checksum-verified).
[ -d rnnoise ] || git clone https://github.com/xiph/rnnoise.git rnnoise
git -C rnnoise checkout -q 70f1d256acd4b34a572f999a05c87bf00b67730d
H=$(cat rnnoise/model_version)
M="$DL/rnnoise_data-$H.tar.gz"
[ -f "$M" ] || curl -L -o "$M" "https://media.xiph.org/rnnoise/models/rnnoise_data-$H.tar.gz"
if command -v shasum >/dev/null 2>&1; then echo "$H  $M" | shasum -a 256 -c; else echo "$H  $M" | sha256sum -c; fi
tar xzf "$M" -C rnnoise

# dr_libs single-header decoders (public domain / MIT-0).
curl -sL -o dr_wav.h https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h
curl -sL -o dr_mp3.h https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h
curl -sL -o miniaudio.h https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h   # audio device I/O for the real-time host

# LAME 3.100 (LGPL) MP3 encoder.
L="$DL/lame-3.100.tar.gz"
[ -f "$L" ] || curl -L -o "$L" https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
[ -d lame-3.100 ] || tar xzf "$L"
