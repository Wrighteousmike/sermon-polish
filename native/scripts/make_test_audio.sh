#!/bin/sh
# Writes a 60 s synthetic stereo test file (48 kHz float WAV; amplitude-modulated tone plus noise).
# Not speech, but enough to exercise the host, export and installer self-test without real audio.
# Usage: make_test_audio.sh out.wav
set -e
FF="$(dirname "$0")/../third_party/ffmpeg-install/bin/ffmpeg"
"$FF" -v error -y -f lavfi -i "sine=f=180:d=60:r=48000" -f lavfi -i "anoisesrc=d=60:r=48000:a=0.02" \
  -filter_complex "[0][1]amix=inputs=2,tremolo=f=4:d=0.8,aformat=sample_fmts=flt:channel_layouts=stereo" -c:a pcm_f32le "$1"
