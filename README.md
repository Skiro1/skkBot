# skkBot

A macro bot for Geometry Dash with a built-in video renderer.

## Features
- Macro recording/playback with per-frame physics
- Built-in video renderer via bundled FFmpeg — the codec list shows every video encoder available in the FFmpeg build (NVENC, AMF, QSV, libx264, libx265, AV1, VP9/VP8, MPEG-4, ProRes, MJPEG, ...)
- Default codec **libx264** — works on any machine regardless of GPU; no auto-detection, the exact codec you pick is used
- Video bitrate 5–200 Mbps (default 50), fade in/out (0–3s, off by default), resolution presets (144p–8K), 1–240 FPS
- Audio recording: AAC, MP3, Opus, Vorbis, FLAC, ALAC, AC3, E-AC3 (as available in the bundled FFmpeg)
- Keybinds: **F6 = Record/Stop**, **F7 = Play/Stop** — right-click the Record/Play buttons to rebind
- Practice mode support
- ImGui GUI with render/audio settings
- Bundled FFmpeg — no downloads needed
- Logging fully off by default (optional file logging in Settings)
- Single macro format (.skk)

## Installation
1. Install [Geode](https://geode-sdk.org)
2. Download `skiro1.skk-bot.geode` from [Releases](https://codeberg.org/Skiro1/skkBot/releases)
3. Place it in `GeometryDash\geode\mods`
4. Launch Geometry Dash

## Usage
- **Record:** press F6 (or click Record in the skkBot window), play the level, press F6 again
- **Playback:** select a macro and press F7 (or click Play)
- **Keybinds:** right-click the Record/Play buttons to open the rebind popup
- **Render:** pick codec/resolution/bitrate and click Start Render (load a macro and play it first)

## Formats
- **.skk** — v31: native binary format with input events, per-frame physics data, metadata (only supported format)

## Links
- [Codeberg](https://codeberg.org/Skiro1/skkBot)
- [Issues](https://codeberg.org/Skiro1/skkBot/issues)

## License
MIT
