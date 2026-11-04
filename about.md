# skkBot2
skkBot2 is an input-first macro bot for Geometry Dash (the skkBot rewrite). It records
real GD inputs and replays them through GD's native path, with a native ImGui menu
that replaces the cocos frontend of the old skkBot.

## Features
- **Input-first macro recording/playback** with per-substep (sub-tick) input dispatch
  through GD's native pipeline (`.skk`, SKK3 format)
- **LockDelta**: physics-delta freeze (`warp*physicsDt` per substep, step-grid hooks) —
  verified playback at 1.00–1.13 plan/tick; Lock Delta / Real Time selectable in the UI
- **Accuracy levels**: Vanilla and CBS (Click-Between-Steps) + COS helper (vanilla GD
  mechanics, applied natively). External accuracy mods (SIP, Click-Between-Frames) are
  declared as incompatibilities
- **Triple RNG lock** (shake + teleport + per-object 2068), velocity fix (always on),
  gravity/up-down/dash state correction from player-visual bits
- **Per-frame persistence-attempt** recording + playback, practice-mode support,
  frame stepper (forward), dead-cell hold-sync on checkpoints
- **Checkpoint system** with `PlayerStateBundle` (54 fields) and Rubber-Banding reconcile
- **Supported Macro Formats**:
  - **.skk (SKK3)**: native state-verified binary format with physics snapshots, checkpoints, persistence attempts, triple-RNG boundaries, and target display FPS (Recording & Playback).
  - **.skc (SKC)**: ultra-compressed inputs-only format (~1–2 bytes/action) using adaptive range coding without heavy physics snapshots (Recording & Playback, beta).
  - **Third-Party Formats (Playback only)**: Echo (`.echo`), Astral (`.ast`), GucciBot (`.gbr`), GDR2 (MegaHack, EclipseMenu) (`.gdr2`), GDH (`.re4`), Neverhook (`.nhm`), Silicate (`.slc`), ToastyReplay Lite (`.ttrl`), Xenon (`.xe`). External physics injection is safely ignored.
- **Built-in video renderer** (FFmpeg runtime): FBO + PBO ring, GPU NV12 output,
  full encoder list (libx264, libx265, NVENC, AMF, QSV, AV1, VP9/VP8, MPEG-4, ProRes,
  MJPEG, ...), presets 144p–8K, 1–240 FPS, bitrate 5–200 Mbps, audio AAC/MP3/Opus/Vorbis/
  FLAC/ALAC/AC3/E-AC3; **level audio is captured from block 0**.
  FFmpeg DLLs are kept outside the .geode file to minimize size. The bot can automatically test
  mirror download speed (GitHub vs Codeberg), download and install the required DLLs to
  `geode/mods/skiro1.skk-bot/ffmpeg/` on the fly.
  Manual links: [Codeberg dll.zip](https://codeberg.org/Skiro1/skkBot/releases/download/ffmpeg/dll.zip), [GitHub dll.zip](https://github.com/Skiro1/skkBot/releases/download/ffmpeg/dll.zip).
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

## Acknowledgements

skkBot2 was built standing on the shoulders of the GD botting & modding community.
Heartfelt thanks to the authors of these projects — their ideas, formats, reverse-engineering
work and open code shaped many parts of this bot (inspiration, techniques and format
compatibility):

**Bots & mods:**
- **[Echo](https://echogd.com/)** — replay codec ideas, drift detection, step tracing, timing insights
- **[EclipseMenu](https://eclipse.menu/)** — GDR2 macro format compatibility
- **[GDH](https://gitlab.com/tobyadd/GDH/-/releases)** — replay format compatibility
- **[GucciBot](https://github.com/Nigelx1/GucciBot)** — GBR format, practice range & tooling concepts
- **[Neverhook](https://github.com/DeMaxi1337/Neverhook)** — replay format compatibility
- **[Silicate](https://geode-sdk.org/mods/peony.silicate)** — recording & rendering techniques (FBO capture, audio pumping)
- **[ToastyReplay](https://toastyreplay.xyz/#pricing)** / **[ToastyReplay Lite](https://geode-sdk.org/mods/toastexgd.toastyreplay-lite)** — replay format compatibility
- **[Vbot](https://github.com/Vbot-Macro/Vbot)** — TPS bypass approach, step patching, frame-window tooling
- **[Xenon](https://xenon.baleszka.dev/)** — CBF handling, NOP patches, queue sanitization practices
- **[XeniaHack](https://github.com/noxoraid/XeniaHack/tree/testdriver/setup-tests)** — half-tick macro support insights

**Research & reverse-engineering projects:**
- **[GD-2.2081-Decompiled](https://github.com/iArtie/GD-2.2081-Decompiled)** — full readable decompilation of Geometry Dash 2.2081
- **[open-source-gd-project](https://github.com/StarryDawn72/open-source-gd-project)** — verified reconstructions of GD functions and bindings