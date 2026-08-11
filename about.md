# skkBot
skkBot is a macro bot for Geometry Dash with a built-in video renderer

## Features
- Macro recording/playback with per-frame physics
- Video renderer via bundled FFmpeg — the codec list shows every video encoder available in the FFmpeg build (NVENC, AMF, QSV, libx264, libx265, AV1, VP9/VP8, MPEG-4, ProRes, MJPEG, ...)
- Default codec libx264 — works on any machine, GPU or not; no auto-detection, the exact codec you pick is used
- Bitrate 5–200 Mbps (default 50), fade in/out (0–3s, off by default), resolution presets (144p–8K), 1–240 FPS
- Audio recording: AAC, MP3, Opus, Vorbis, FLAC, ALAC, AC3, E-AC3 (as available in the bundled FFmpeg)
- Keybinds: F6 = Record/Stop, F7 = Play/Stop — rebind by right-clicking the buttons
- Single macro format (.skk)
- Practice mode support
- Logging fully off by default (optional file logging in Settings)

## Changelog
See [changelog.md](changelog.md)

- **v0.0.3** — keybinds (F6/F7 with right-click rebinding), render codec overhaul (no auto-config, full encoder list, default libx264, bitrate up to 200 Mbps, fades default 0), more audio codecs (Vorbis/ALAC/AC3/E-AC3), logging off by default
- **v0.0.2** — major audio system rewrite, multi-format replay support (GDR/GDR2/SLC), render stability fixes
- **v0.0.1** — initial release: per-frame physics recording, video renderer, audio recording
---

Thanks to everyone who helped test and improve skkBot
