# skkBot2
skkBot2 is an input-first macro bot for Geometry Dash (the skkBot rewrite). It records
real GD inputs and replays them through GD's native path, with a native ImGui menu
that replaces the cocos frontend of the old skkBot.

## Features
- **Input-first macro recording/playback** with per-substep (sub-tick) input dispatch
  through GD's native pipeline (`.skk`, SKK3 format)
- **LockDelta**: physics-delta freeze (`warp*physicsDt` per substep, step-grid hooks) —
  verified playback at 1.00–1.13 plan/tick
- **Accuracy levels**: Vanilla and CBS (Click-Between-Steps) + COS helper (vanilla GD
  mechanics, applied natively). External accuracy mods (SIP, Click-Between-Frames) are
  declared as incompatibilities
- **Triple RNG lock** (shake + teleport + per-object 2068), velocity fix (always on),
  gravity/up-down/dash state correction from player-visual bits
- **Per-frame persistence-attempt** recording + playback, practice-mode support,
  frame stepper (forward), dead-cell hold-sync on checkpoints
- **Checkpoint system** with `PlayerStateBundle` (54 fields) and Rubber-Banding reconcile
- **Single macro format `.skk` (SKK3)**: zstd-22 compression, dense wire encoding
  (varint/zigzag/XOR + RLE), per-frame delta layout, checkpoints, persistence,
  triple-RNG boundaries
- **Built-in video renderer** (bundled FFmpeg): FBO + PBO ring, GPU NV12 output,
  full encoder list (libx264, libx265, NVENC, AMF, QSV, AV1, VP9/VP8, MPEG-4, ProRes,
  MJPEG, ...), presets 144p–8K, 1–240 FPS, bitrate 5–200 Mbps, audio AAC/MP3/Opus/Vorbis/
  FLAC/ALAC/AC3/E-AC3; **level audio is captured from block 0**
- **Modern ImGui GUI**: intuitive modern interface with tab navigation (Record,
  Assist, Video, Settings), macro search, Solid background toggle, and classic window fallback
- **Assist & Visuals**: Hitbox overlay (isolated from shaders), real-time trajectory simulation,
  and Wave Trail Fix (draw + drag fixes)
- **Logging system** (None/Error/Warn/Info/All): Geode console + optional file logging,
  split Record/Play log files

## Changelog
See [changelog.md](changelog.md)

## Compatibility
Won't load together with: `chizz.superb-input-precision`, `syzzi.click_between_frames`

---

Thanks to everyone who helped test and improve skkBot