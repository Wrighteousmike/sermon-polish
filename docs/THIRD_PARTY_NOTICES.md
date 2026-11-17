# Third-party software in Sermon Polish

Sermon Polish is free software (GPL-3). It includes or ships with the following, each under its own
license. Source code for everything below is available from the links given.

| Component | Used for | License | Source |
|---|---|---|---|
| FFmpeg n8.1.3 (built as an unmodified LGPL build, run as a separate program) | Reading audio from video and non-WAV/MP3 files | LGPL-2.1+ | https://ffmpeg.org / https://github.com/FFmpeg/FFmpeg (tag n8.1.3); our build options are in `native/scripts/build_ffmpeg.sh` |
| LAME 3.100 | MP3 encoding | LGPL-2.0+ | https://lame.sourceforge.io |
| DeepFilterNet3 | Noise reduction | MIT or Apache-2.0 | https://github.com/Rikorose/DeepFilterNet |
| RNNoise | Fallback noise reduction | BSD-3-Clause | https://github.com/xiph/rnnoise |
| dr_wav, dr_mp3 | Reading WAV and MP3 | Public domain / MIT-0 | https://github.com/mackron/dr_libs |
| Tauri | Application shell | MIT or Apache-2.0 | https://tauri.app |
| WaveSurfer.js | Waveform display | BSD-3-Clause | https://wavesurfer.xyz |
| React | User interface | MIT | https://react.dev |

The FFmpeg and LAME libraries are LGPL: you may replace them with your own builds. The program
`ffmpeg` inside this app is the unmodified FFmpeg project build described above.
