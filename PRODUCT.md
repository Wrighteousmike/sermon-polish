# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users
Church sound and media volunteers, cleaning up a Sunday recording during the week on a home or office laptop (often an older Windows machine, sometimes a Mac). Mostly non-technical: they know a sermon should sound clear, not what a de-esser or LUFS is. Occasionally an audio-savvy helper who wants to open Advanced. Pastors doing it themselves are a secondary case.

## Product Purpose
Sermon Polish turns a raw sermon recording (audio, or a video it takes the audio from) into a clean file ready for YouTube, Spotify or a church website: noise, room echo, harsh S sounds, uneven volume and loudness are handled, and the speaker's start and end are topped and tailed. Success is a volunteer who trusts the result and exports it without needing to learn audio.

## Positioning
Free and open source, runs entirely on the user's own computer (nothing uploaded, no account). Calm and guided: Auto listens and suggests a starting point for every step, every step is a single Off-to-Max slider, and Original/Cleaned can be flipped instantly so the result can be trusted by ear. Pro-grade underneath (real loudness targets, a -1 dB true-peak limiter, offline-quality export), with depth available under Advanced.

## Operating Context
Desktop app (Tauri shell with a React UI, C++ real-time engine). Flow: drop a file, top and tail on the waveform (with a fine-trim zoom for each end), save the trim, Auto analyses and sets the sliders, listen while changing anything, export to MP3 or WAV with an estimated time remaining and an "Export complete / Show file" dialog. A first-run tour explains trimming. Echo reduction takes a minute or two to prepare the first time; a "Processing…" indicator lists background work. Installs unsigned on Mac (Apple silicon and Intel) and Windows.

## Capabilities and Constraints
Stages: repair clipping, noise, room echo, tone, de-ess, even volume (leveler/compressor), loudness (streaming standards) and a limiter pre-gain. Every stage is an Off-to-Max slider with Auto's pick shown as a tick. Simple (three sliders) and Advanced (every stage) views. Exports always wait for Auto. Long recordings (35+ minutes) are normal. Must stay usable on modest hardware. Brand mark: the "oneflock" wordmark appears in the header; the app name is Sermon Polish.

## Brand Commitments
Name: Sermon Polish. oneflock wordmark in the header. Free, open source, private by design. Tone: plain, warm, never technical for its own sake.

## Evidence on Hand
Working app and screens in `src/` (upload, Studio, Export), a first-run tour, a real 35-minute test sermon. No testimonials, user numbers or external proof exist; none may be invented.

## Product Principles
1. One clear next step at a time; the screen always shows what to do now.
2. Auto first, hands-on second; never make a beginner choose before they have heard something.
3. Let people verify by ear: Original vs Cleaned is always one press away.
4. Say what is happening: processing, waiting and time remaining are always visible and honest.
5. Depth is available but never in the way.

## Accessibility & Inclusion
Older and non-technical volunteers: large readable text, strong contrast, big targets, plain words, full keyboard use for trimming and playback. No colour-only status.
