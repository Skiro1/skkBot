# skkBot2

An input-first macro bot for Geometry Dash (the skkBot rewrite). It records real
GD inputs and replays them through GD's native path, with a native ImGui menu
that replaces the cocos frontend of the old skkBot. Ships a built-in video
renderer using bundled FFmpeg — no extra downloads needed.

## Features
- **Input-first macro recording/playback** with per-substep (sub-tick) input dispatch
  through GD's native pipeline (`.skk`, SKK3 format)
- **LockDelta**: physics-delta freeze (`warp*physicsDt` per substep, step-grid hooks) —
  verified playback at 1.00–1.13 plan/tick
- **Accuracy levels**: Vanilla and CBS (Click-Between-Steps) + COS helper — applied through
  vanilla GD mechanics; external accuracy mods (Superb Input Precision, Click-Between-Frames)
  are declared as incompatibilities
- **Triple RNG lock** (shake + teleport + per-object), velocity fix (always on),
  gravity/up-down/dash state correction from player-visual bits
- **Per-frame persistence-attempt** recording + playback, practice-mode support (practice
  fix for broken objects), frame stepper (forward)
- **Checkpoint system** with `PlayerStateBundle` (54 fields) and Rubber-Banding reconcile,
  hold-restore after checkpoint restore
- **Single macro format `.skk` (SKK3)**: zstd-22 compression, dense wire encoding
  (varint/zigzag/XOR + RLE), per-frame delta layout, checkpoints, persistence,
  triple-RNG boundaries
- **Built-in video renderer** (FFmpeg runtime): FBO + PBO ring, GPU NV12 output, the codec
  list shows every video encoder available in the build (NVENC, AMF, QSV, libx264,
  libx265, AV1, VP9/VP8, MPEG-4, ProRes, MJPEG, ...); resolution presets (720p–8K),
  1–240 FPS, bitrate 5–200 Mbps, fade in/out.
  *FFmpeg DLLs are not bloated inside the .geode file*; the mod can automatically benchmark
  mirror speeds (GitHub vs Codeberg) and download/unpack them directly into `geode/mods/skiro1.skk-bot/ffmpeg/` on demand.
- **Audio recording**: AAC, MP3, Opus, Vorbis, FLAC, ALAC, AC3, E-AC3 (as available in the
  FFmpeg build); **level audio is captured from block 0**
- **Modern ImGui GUI**: intuitive modern interface enabled by default with tab
  navigation (Record, Assist, Video, Settings), macro search, Solid background toggle, and classic window fallback
- **Assist & Visuals**: Hitbox overlay (isolated from shaders), real-time trajectory simulation,
  and Wave Trail Fix (draw + drag fixes)
- **Logging system** (None/Error/Warn/Info/All): Geode console + optional file logging,
  split Record/Play log files; off by default

## Installation
1. Install [Geode](https://geode-sdk.org)
2. Download `skiro1.skk-bot2.geode` from [Releases](https://codeberg.org/Skiro1/skkBot/releases)
3. Place it in `GeometryDash\geode\mods`
4. Launch Geometry Dash
5. **Video Rendering (FFmpeg)**:
   - When opening the **Render** tab for the first time, click **DOWNLOAD FFMPEG**. The mod automatically compares download speeds between mirrors (GitHub vs Codeberg), selects the faster one, and unpacks the 7 required DLLs into:
     `%localappdata%\GeometryDash\geode\mods\skiro1.skk-bot\ffmpeg\`
   - If automatic download fails, you can download `dll.zip` or `dll.rar` manually from:
     - [Codeberg Release (dll.zip)](https://codeberg.org/Skiro1/skkBot/releases/download/ffmpeg/dll.zip)
     - [GitHub Release (dll.zip)](https://github.com/Skiro1/skkBot/releases/download/ffmpeg/dll.zip)
     - [Codeberg Release (dll.rar)](https://codeberg.org/Skiro1/skkBot/releases/download/ffmpeg/dll.rar)
     - [GitHub Release (dll.rar)](https://github.com/Skiro1/skkBot/releases/download/ffmpeg/dll.rar)
     and extract the 7 DLL files directly into the `ffmpeg` folder, then click **RELOAD FFMPEG**.

## Usage
- Open the bot menu with the menu keybind (configurable in **Settings → Menu Keybind**;
  default Right Alt)
- **Record:** click **Record** in the skkBot2 window, play the level, click **Stop**
- **Playback:** type the macro name (or pick it in **Macro List**) and click **Play**
- **Save / Load / Clear / List:** manage the active macro; the macro list has an **Open
  Macro Folder** button and per-file **Delete**
- **Render:** open **Video Render**, pick codec/resolution/bitrate and click **Start**
  (load and play a macro first, then start a render while playing)
- Save / Load / Clear are locked while a record or play session is active

## Compatibility
This mod won't load together with:
- `chizz.superb-input-precision`
- `syzzi.click_between_frames`

## Formats
- **.skk** (SKK3) — native binary format: input events with per-substep click phases, per-frame physics + camera data, checkpoints, persistence attempts, RNG boundaries, and target display FPS. Supports both recording and playback. Only the latest format is supported — old files must be re-recorded.
- **.skc** (SKC) — ultra-compressed inputs-only format: range-coded compact macro stream (~1–2 bytes per action) without heavy physics snapshots. Supports both recording and playback. *(Note: currently in active development / beta; platformer mode requires caution with deaths/checkpoints; does not support CBS and COS).*
- **Third-Party Replay Formats (Playback only)** — full playback support for macros exported from other bots:
  - Echo (`.echo`)
  - Astral (`.ast`)
  - GucciBot (`.gbr`)
  - GDR2 (MegaHack, EclipseMenu) (`.gdr2`)
  - GDH (`.re4`)
  - Neverhook (`.nhm`)
  - Silicate (`.slc`)
  - ToastyReplay (`.ttr3`)
  - ToastyReplay Lite (`.ttrl`)
  - Xenon (`.xe`)
  *(Note: for external formats that store physics coordinates, third-party physics injection is ignored and inputs are processed purely through GD's native engine to prevent physics conflicts).*

## Building
Requires [Geode CLI](https://docs.geode-sdk.org/getting-started) (Windows build with the
Geode SDK). `CMakeLists.txt` is included; a release `.geode` package is produced by the
Geode build setup.

## Links
- [Codeberg](https://codeberg.org/Skiro1/skkBot)
- [Issues](https://codeberg.org/Skiro1/skkBot/issues)

## License

This project is licensed under the **GNU General Public License v3.0 or later**
(`GPL-3.0-or-later`). The full text is in [LICENSE](LICENSE).

Copyright (C) 2026 Skiro1.