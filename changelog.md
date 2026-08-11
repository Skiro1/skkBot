# v0.0.3 (2026-08-11)
Keybinds + render codec overhaul + logging off by default.

## New
- **Record/Play keybinds**: F6 = Record/Stop, F7 = Play/Stop (defaults, saved per mod); right-click the Record/Play buttons to open the rebind popup
- **No auto-config for the video renderer**: the exact codec you pick in the GUI is used — no silent fallback chain (NVENC → x264) and no "Auto" option; a failing codec reports a clear error
- **Full encoder list**: the codec combo shows every video encoder available in the bundled FFmpeg (h264/x264/x265/hevc/av1/vp9/vp8/mpeg4/prores/mjpeg/mpeg2/vc1 families — NVENC, AMF, QSV and software)
- **NV12 → YUV420P conversion** via swscale for codecs that don't accept NV12 directly (libx265, SVT-AV1, VP9, MPEG-4, ...)
- **Default video codec libx264** — works on any machine regardless of GPU
- **More audio codecs**: Vorbis (libvorbis), ALAC, AC3, E-AC3 (all mux into MP4)
- Video bitrate slider up to **200 Mbps** (default 50)
- Fade in/out default **0** (off)

## Changes
- Removed the "Ready" status text and the "Used: <codec>" lines from the Render tab
- Default log level = None, file logging and verbose logging off by default (no log file is created until enabled in Settings)
- Version v0.0.3 displayed in the window title and logs

## Temporary (to be re-added later)
- **CUDA temporarily removed** — pixel readback no longer uses CUDA
- **Only one macro format supported** — GDR/GDR2/SLC support removed; only .skk remains
- **.skk format fully redesigned** (v31)
- **Macro recording and playback fully redesigned**
- **Renderer fully redesigned, Silicate-style**

## Fixes
- Bottom hint line showed the same key for Record and Play (keyName() returns a pointer to a shared static buffer — all format slots read the last-written key; each key name is now copied into a string immediately)
- libx264 missing from the codec list (the encoder filter matched "h264" but not "x264")

# v0.0.2 (2026-07-09)
Major audio system rewrite + multi-format replay support + render stability.

## New
- **Audio rewrite**: offline song decode via `FMOD::Sound::lock()` instead of DSP callback — no more 1.39×/0.70× drift
- **Perfect audio sync**: audio prepared once at render start, trimmed/padded to exact video duration
- **`m_startGameTime`**: game-time offset captured at render start — correct sync when recording from pause menu
- **Restart support**: renderer stays alive when restarting level during recording; `m_levelTime` and `m_gameTimeOffset` reset properly
- **GDR format**: read-only playback support (`.gdr`, `.gdr.json`)
- **GDR2 format**: read-only playback support (`.gdr2`)
- **SLC format**: auto-detect V1/V2/V3 by magic bytes (`.slc`)
- **Multi-format picker**: file dialog filters by extension, auto-detection fallback
- **SKK v2 read support**: backwards-compatible with older .skk format
- **Debug WAV**: writes `.debug.wav` next to rendered video for audio verification

## Changes
- Removed FMOD DSP callback, `FMODAudioEngine::update` hook, carry buffer, per-frame audio collect
- `onQuit()` no longer kills renderer during active recording
- Song offset applied before trim (correct order)
- `m_levelTime` changed from `float` to `double` for precision
- GDR2 playback uses `+1` frame conversion instead of `+1.0000001`

## Infrastructure
- Standalone `gdr2_playback.hpp` — separate engine for GDR2/SLC input dispatch
- `AudioCapture::setGameTimeOffset()` — reset game offset on restart
- Cleaned up `hooks.cpp`: removed FMOD, DSP, `trackPreRollIfNeeded`, `muteSfx`

# v0.0.1 (2026-07-08)
Initial release of skkBot. A macro bot with per-frame physics recording and built-in video renderer.

What's new:
- Macro recording/playback with per-frame physics and loop compression (.skk v3 format)
- Video renderer via FFmpeg API — 37+ codecs including NVENC, AMF, QSV, D3D12VA, Vulkan, libx264, libx265, VP9, AV1
- CUDA-accelerated pixel readback (D3D9 → CUDA → RAM)
- Audio recording via FMOD DSP (AAC, MP3, FLAC, Opus, PCM)
- ImGui GUI with render/audio settings and resolution presets (144p–8K)
- TPS Bypass, Seedhack, Practice Fix
- Logging system with levels (None/Error/Warn/Info/All) and file output
- Auto-download FFmpeg if not installed
- Performance presets with one-click bitrate selection

Bug fixes:
- Fixed CUDA 13.3 crash — replaced removed `cuMemcpy2DFromArray` with `cuMemcpy2D`
- Fixed audio crash with pcm_s16le (frame_size=0)
- Fixed render stopping early — now waits for actual level end
- Fixed GD notifications appearing in rendered video
- Fixed preset button layout and bitrate progression
- Fixed tooltips in GUI

Changes:
- Default settings: log=None/file=off, lock delta=on, audio=on (AAC 192k), render=1080p60 50Mbps libx264
- Removed music/SFX volume sliders (didn't affect render)
- Replaced GD notifications with internal logging

Infrastructure:
- CMakeLists.txt with DONT_INSTALL flag
- CUDA 13.3 driver support (GTX 1650)

Thanks to everyone who tested the early builds
