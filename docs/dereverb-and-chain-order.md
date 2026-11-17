# Dereverb options and recommended chain order (research, 2026-10-28)

Evidence quality: mostly project pages and papers; no rigorous head-to-head on
real reverberant sermon recordings was found. Treat rankings as hypotheses to
test on `Audio/Wash your wife ORIGINAL.mp3`.

## Dereverb candidates (no turnkey open-source plugin exists)

| Option | License | Dereverb evidence | Cost / catch |
|---|---|---|---|
| DeepFilterNet3 | MIT / Apache-2.0 | trained to reduce reverb alongside noise; a 2025 paper (Rosenbaum et al., Sensors) says its dereverb is limited | real-time on CPU; already planned for noise. Free mild dereverb. |
| WPE (NARA-WPE, MIT, Python; BTK has a C++ one, license unchecked) | MIT | classical linear-prediction method, deterministic, no hallucination | single-channel gain is modest; port to C++ ourselves (like SPADE). Noise hurts it slightly. |
| SGMSE+ | code MIT; checkpoint license not stated | strongest published dereverb results (WSJ0-REVERB, EARS-Reverb 48 kHz) | diffusion (~50 steps), PyTorch, no ONNX documented: slow on CPU; generative, can invent detail. |
| anvuew dereverb_room (Mel-Band RoFormer, ONNX fp16 ~60 MB) | GPL-3.0 (compatible with a GPL-3 app) | built for vocals in music; speech unverified | transformer on CPU for 46 min is unmeasured. Other RoFormer dereverb checkpoints are CC-BY-NC-SA: avoid. |
| Resemble Enhance | MIT | denoise + bandwidth extension; dereverb not confirmed | slow on CPU, generative |
| VoiceFixer | not checked | claims dereverb/declip/denoise | 122M params, old |
| ClearerVoice-Studio (FRCRN etc.) | Apache-2.0 code | denoising only; no documented dereverb | not a dereverb answer |

Recommended path: DeepFilterNet3 first (noise + mild dereverb), then our own
C++ WPE as a safe, deterministic dereverb stage, then trial anvuew ONNX and
SGMSE+ offline on the test file as optional "strong" modes if WPE is not enough.

## Order

Papers disagree: denoise-first has a signal-model argument (noise is added
after reverb), but cascades work best in order of the dominant distortion, and
separate stages can over-estimate each distortion. So make the dereverb/denoise
order switchable and test both on real files; default denoise then dereverb.

1. Decode, resample to 48 kHz float.
2. Declip (SPADE) first: it relies on flat tops at the exact clip level, which
   any gain, EQ or noise reduction would blur. (The browser order had it later.)
3. Subsonic highpass (~30-60 Hz) and mains-hum notches.
4. Offline leveler: neural stages and later thresholds see a consistent level.
5. Noise reduction (DeepFilterNet3, RNNoise fallback).
6. Dereverb (WPE; optional neural).
7. Corrective EQ (Tone presets / Reference): after enhancement, so the models
   see the natural spectrum.
8. De-esser.
9. Compressor.
10. Steep lowpass (encoder hiss).
11. Limiter: loudness target, true-peak ceiling.
12. Export: resample, LAME. Leave ~1 dB true-peak headroom for encoder overshoot.
