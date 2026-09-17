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