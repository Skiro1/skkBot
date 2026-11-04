# v0.1.3 (2026-11-04)
Timing & accuracy update: Real Time mode, SKC precision overhaul, checkpoint fixes, macro auto-save.

## New
- **Real Time timing mode**: playback can now follow the honest wall-clock frame gap instead of a locked physics delta
- **Lock Delta / Real Time switch in UI**: timing mode is selectable per session from the GUI for both SKK and SKC playback
- **Macro auto-save**: recordings are saved automatically (also fixed: autosave silently did nothing after a game restart)
- **Foreign imports auto-select Real Time**: imported third-party macros automatically switch playback to Real Time
- **Speedhack during render**: speedhack can now be used while a video render is running

## Improvements
- **Improved SKC compression**: LZ phrase prediction + predictor mixing in the range coder (fewer bytes per action on repetitive click patterns)
- **Updated formats**: SKC and SKK format revisions
- **Improved keybinds**: keybinds can now be unbound (no forced defaults)
- **UI polish**: fixed cursor rendering in the UI, macro name/search field placement, menu text cleanup, Open Menu opens the tab view by default

## Fixes
- **Fixed Lock Delta and TPS handling**: deterministic step grid for SKC, honest wall-gap feeding in Real Time
- **Fixed checkpoints for SKK and SKC**: hold bits / jump-buffer mirrors restored correctly on checkpoint restore, lost click pairs after practice stitching fixed, release-suppression window narrowed to no-ops
- **Fixed SKC playback accuracy**: gravity portal flip determinism, dash-stop icon rotation, wall-clock jump timers no longer restored on checkpoints
- **Fixed .echo import** (Echo Bot)
- **Fixed .slc v3 import** (Silicate)
- **Fixed music desync during render**: audio is now bound to the video timeline (measured drift ≤ 40 ms)

## Removed
- **ToastyReplay `.ttr3` import support** (ToastyReplay Lite `.ttrl` is still supported)
- **CBS/COS accuracy modes for SKC**: SKC is now vanilla-only (the modes may return in a future update)

---

# v0.1.2 (2026-10-24)
Major feature update introducing the ultra-compressed SKC format, universal third-party replay import, high-performance rendering pipeline, FPS bypass, redesigned trajectory simulation, and extensive customization.

## New
- **Ultra-compressed SKC format (`.skc`)**:
  - New inputs-only macro format using adaptive range coding (~1–2 bytes per input event) without storing heavy physics states.
  - Supports both recording and playback.
  - *Note: currently in active development / beta; platformer mode may experience desyncs on deaths/checkpoints; CBS and COS accuracy modes are not yet supported for SKC.*
- **Universal Replay Import & Playback**:
  - Full playback support for popular third-party macro formats: Echo (`.echo`), Astral (`.ast`), GucciBot (`.gbr`), GDR2 (MegaHack, EclipseMenu) (`.gdr2`), GDH (`.re4`), Neverhook (`.nhm`), Silicate (`.slc`), ToastyReplay Lite (`.ttrl`), and Xenon (`.xe`).
  - Pure input dispatch: external physics state injections are safely bypassed, allowing GD's native physics engine to simulate the macro deterministically.
- **Custom FPS Bypass**:
  - Added configurable Display FPS target (e.g. 60, 144, 240, 360, up to 1000 FPS) with automatic integration into GD's native custom FPS and vertical sync management.
- **Frame Stepper (Beta)**:
  - Step through physics ticks frame-by-frame using on-screen buttons or dedicated hotkeys for precise frame inspection.
- **Layout Mode**:
  - Added Layout Mode toggle for enhanced visibility and level debugging.
- **Keybinds System**:
  - Dedicated Keybinds screen with fully customizable hotkeys for all primary bot actions (Menu, Record, Play, Clear, Frame Stepper, Trajectory, Hitboxes, Layout Mode, Safe Mode).
- **UI Typography & Font Customization**:
  - Added options to select custom UI fonts and adjust UI Font Scale directly from the Settings tab.
- **Safe Mode**:
  - Added Safe Mode toggle to guard against accidental inputs and unintended state mutations.
- **Dynamic FFmpeg Downloader**:
  - FFmpeg binaries are no longer packed inside the `.geode` file (slimming package size down from hundreds of MBs to ~3 MB).
  - Built-in mirror speed benchmark automatically tests and downloads the required libraries from GitHub or Codeberg on demand.
- **SKK Format Updated (SKK3)**:
  - Added target display FPS field to `.skk` metadata alongside TPS and physics state bundles.

## Improvements & Performance
- **High-Performance Video Render Pipeline**:
  - Hardware encoding optimizations for Intel QSV (`preset=veryfast`, `async_depth=4`), AMD AMF (`quality=speed`, `usage=lowlatency`), and NVIDIA NVENC.
  - Expanded PBO pool to 16 slots with 16-frame ring buffer and fast batch ramp-up (`m_batchLimit=3`, `kGrowStreak=2`).
  - Zero-overhead OpenGL pipeline: eliminated per-frame `glGetError` stalls and redundant synchronous `glGetIntegerv` driver queries.
  - Independent render clock ensuring uniform frame delivery without speed fluctuations.
- **Enhanced TPS Engine**:
  - Improved step-grid midhooks and unified micro-substep accumulator for stable, deterministic execution at any tickrate.
- **Reworked Trajectory Preview (Beta)**:
  - Re-engineered lightweight dual-hypothesis trajectory predictor (Hold / Release).
  - Computation moved from physics ticks to Cocos scene rendering (`TrajectoryDrawNode::visit`), completely eliminating in-game physics lag.
- **Project License**:
  - Official project license updated to **GNU General Public License v3.0 or later** (GPL-3.0-or-later).

## Fixes
- **Windows Cyrillic & Non-ASCII Path Crash**:
  - Replaced MSVC `path.string()` calls with exception-safe UTF-8 conversions (`skk::pathToUtf8`), resolving fatal game crashes on Windows when user profiles or level names contain Cyrillic or non-ASCII characters.
- **UI Visual Glitches & Contrast Polish**:
  - Fixed dark text on disabled action buttons (such as "START RENDER") by making disabled button labels crisp white.
  - Fixed `AUDIO_DEFERRED_WAIT` log spam when leaving levels or canceling renders.
  - Fixed hitboxes toggle triggering on text clicks.
  - Standardized accordion sections (`SPEED`, `NOCLIP`, `FRAME STEPPER`, etc.) with unified metrics, smooth animations, and click-gating.
- Checkpoint restore stability and memory leak prevention during rapid restarts.

---

# v0.1.1 (2026-10-04)
Minor update bringing redesigned modern UI, engine stability fixes and expanded customization.

## New
- **Modern Beta UI enabled by default**: sleek Liquid Glass interface with full
  tab navigation (Record, Assist, Video, Settings), macro library search & management
- **Solid background**: opaque panel mode for the beta UI (default on, configurable
  under Settings → Cosmetics)
- **Assist tab & cosmetics**: wave trail fix section reorganized as Cosmetic
- **Expanded controls**: TPS limit extended up to 1,000,000; Speedhack lower limit
  extended down to 0.01x
- **Accuracy synchronization**: real-time two-way synchronization of CBS and COS
  settings directly with vanilla Geometry Dash options

## Fixes
- Crash on CCDrawNode creation and level exit in hitbox overlay
- Trajectory ghost robot visual glitch on dual mode
- Hitbox overlay affected by game shader effects
- FMOD stream thread crash during normal gameplay and level editing
- Out-of-bounds access on slope collision detection and section traversal
- Automatic Record stop when entering Level Editor

---

# v0.1.0 (2026-09-17)
Initial release of **skkBot2** — the input-first rewrite of skkBot (SKK3 `.skk` format,
native ImGui menu, bundled video renderer).

## New
- **Input-first macro engine**: records real GD inputs and replays them through GD's
  native path with per-substep (sub-tick) input dispatch
- **LockDelta**: physics-delta freeze (`warp*physicsDt` per substep) + step-grid
  midhooks — playback verified at 1.00–1.13 plan/tick, resume window ≤17 plans
- **Accuracy levels**: Vanilla / CBS (Click-Between-Steps) with COS helper — applied
  through vanilla GD mechanics; external accuracy mods (SIP, Click-Between-Frames)
  are blocked via mod.json incompatibilities
- **Triple RNG lock** (shake + teleport + per-object), velocity fix (always on),
  gravity/up-down/dash state correction from player visuals
- **Per-frame persistence-attempt** recording/playback; practice mode; frame stepper
- **Checkpoints**: `PlayerStateBundle` (54 fields) with Rubber-Banding reconcile,
  hold-restore after checkpoint restore
- **SKK3 format**: zstd-22 compression, dense wire v6 (varint/zigzag/XOR, RLE input
  repeats), per-frame delta layout + record RLE, flags for
  anchors/checkpoints/persistence/tpsEvents/perFrame/variance/practiceFix/rngBoundaries
- **Video renderer** (ported from skkBot v0.0.4): FBO + PBO ring, GPU NV12, FFmpeg
  loaded at runtime from the mod bundle, separate "Video Render" window with live
  preview, `.mp4` output
  - Black-screen fix: `dt=1/fps`, `m_started` gate in drawScene, binary TPS-bypass
    patch disabled during recording; tested 1080p@60 (1.52×) and 8K@60 (0.44×)
  - **Level audio from block 0** (v59.7): FMOD kick cycle
    `startMusic + startMusic(0) + pauseAllAudio/resumeAllAudio` restarts the stopped
    music channel — verified by two consecutive renders with near-identical audio
    (residual to AAC noise)
- **GUI**: native ImGui menu — main (Record/Play/Status), Features, Bot Settings,
  Macro List; Render window; menu keybind; accuracy dropdown (Vanilla/CBS); input
  counter with `[chain]` truncation labels
- **Logging**: None/Error/Warn/Info/All levels, console + file, split Record/Play logs

## TPS Bypass
- TPS bypass + anti-SSB, step-grid midhooks, practice-fix P1 (broken-object tracker),
  start-position policy, freeze/exhaustion fix

## Fixes
- Exhaustion freeze after playback pipeline shutdown (`6e9e4c8`)
- Hold buttons not restored after checkpoint restore (`13191a1`)
- T-pose / missing animation after checkpoint restore (`cc598bd`)
- Duplicate SKK3 section on save (OOB write) — `seenKnown[9]`
- PerFrame V2/V3 read-order mismatch (P2 dying in void) (`e3656c1`)
- Death loop at level restart (`e2d0322`)
- Press-suppression leftover — only release-suppression kept (`a9cdaa5`)
- Upside-down preview in the Render window (V-flip UVs in ImGui `AddImage`)
- Level audio silence in renders — FMOD paused-channel restart (v59.7)

## Known issues
- BUG-04: cosmetic spider visual gravity (`m_isUpsideDown`) — open, cosmetic
- BUG-06: rare teleport/pop on orb arcs during playback — source identified, realign
  from practice checkpoint still open
- Backward stepping, multi-act recording, autoclicker, trajectory — on the road-map

---

Thanks to everyone who helped test and improve skkBot