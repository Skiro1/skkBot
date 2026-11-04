#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture.hpp"
#include "ffmpeg.hpp"

class GJBaseGameLayer;
class PlayLayer;

namespace skkbot2 {
namespace render {

#ifndef GEODE_IS_WINDOWS
// FFmpeg types used as members are incomplete on non-Windows builds (the
// renderer itself is Windows-only).
struct AVStream;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVFormatContext;
#endif

// Music-id tracker + deferred song restart, implemented in
// render_hooks.cpp (FMODAudioEngine hooks). The renderer needs the current
// song's musicID to kick the level music from position 0 at capture start.
int getLastMusicId();
void setLastMusicId(int id);
void armMusicRestart();
void disarmMusicRestart();
bool tickPendingMusicRestart();
// True while the deferred song start is still waiting for the level song to be
// prepared. If that is still true at video pts 0 the song will begin at
// position 0 a few frames in, which is a real (small) audio offset - the
// render log names it instead of leaving it to be heard.
bool musicRestartPending();
// Position of the level song inside its own track (seconds), or < 0 when no
// music channel can be identified. Used to anchor the render audio timeline.
double musicPositionSeconds();

// Video renderer (v3 вЂ” async encode pipeline).
//
// The MAIN thread only captures: backbuffer copy + GPU NV12 conversion into a
// ring of NV12 buffers (free-slot acquire/commit, no per-frame copies). A
// BACKGROUND encode thread pops filled slots and runs NVENC + muxer write.
// This keeps the game view at full FPS during a render (the GPU encode
// overlaps the capture of the next frame instead of blocking the main thread).
//
// Design:
//   - physics runs per-tick (tps, same axis the bot's macro uses). After each
//     tick we map the bot's absolute frame to a video pts with
//         pts = (frame-1) * videoFps / tps
//   - when pts advances past the last delivered value we capture one scene
//     frame (a copy of the visible backbuffer, upscaled to the OUTPUT
//     resolution on the GPU; the visible view is never touched) and hand it
//     to the encode thread.
//   - drive() yields back to the game's render loop every few frames, so the
//     game view keeps animating while the render proceeds at codec speed.
//   - stop() closes the queue, JOINS the encode thread (which flushes the
//     codec and writes the trailer itself), then frees the encoder.
class Renderer {
   public:
    static Renderer& get() {
        static Renderer inst;
        return inst;
    }

    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool loadFFmpeg();
    // Forces a fresh probe on the next loadFFmpeg() call. Needed because the
    // probe result is cached: without it, a user who installs the DLLs while the
    // game is running had no way to retry short of a restart (RENDER tab had no
    // refresh at all).
    void reloadFFmpeg() {
        if (m_ffLoaded) return;  // modules already mapped: nothing to redo
        m_error.clear();
        m_ffDir.clear();
    }
    bool isFFmpegLoaded() const { return m_ffLoaded; }
    // Directory the DLLs were actually loaded from (empty until then), and the
    // one the RENDER tab tells the user to drop them into.
    const std::filesystem::path& ffmpegDir() const { return m_ffDir; }
    static std::filesystem::path preferredFfmpegDir();
    static std::vector<std::filesystem::path> ffmpegSearchPaths();
    static void prepareFfmpegDropIn();
    const std::string& lastError() const { return m_error; }

    // Begins a render using the currently loaded macro. PlayLayer must be live
    // and the bot must already be in Playback state.
    bool begin(PlayLayer* layer);

    // Main-thread pump: run physics ticks, capture and enqueue at video-pts
    // boundaries, bounded by the wall budget. Called from the scheduler hook
    // while the renderer is active. stepOne advances the game one physics tick
    // (call the original CCScheduler::update with 1/tps) вЂ” must NOT re-enter
    // this hook.
    void drive(GJBaseGameLayer* layer, std::function<void(float)> stepOne);
    // One video frame's worth of sim, up to the next pts boundary, then the
    // capture is queued. drive() calls it repeatedly inside one drawScene.
    // Returns false when it could not produce a frame (ring full, level paused,
    // level finished, render stopped) - that is also the batching stop signal.
    bool driveOneFrame(GJBaseGameLayer* layer, std::function<void(float)> stepOne);

    // Called at the START of the ImGui draw callback (i.e. right after the
    // game drew the current scene and before the GUI overlay is rendered):
    // executes the capture pending from drive()'s last pts boundary. This way
    // the video gets a FRESH backbuffer with NO bot menu in it, while the menu
    // stays visible on screen during the whole render.
    // Commits the queued capture. Called by drive() once per batched frame, and
    // still by the ImGui draw callback as a fallback for anything drive() left
    // pending (pause menu, trailer). Returns true when a video frame was
    // committed - that is the batching stop signal.
    bool onFrameDraw();

    // Silicate-style: during a render the level is not drawn to the window.
    // Instead the CCDirector::drawScene hook calls drive()+drawPreview().
    // drawPreview blits the last captured scene letterboxed into the default
    // framebuffer so the window shows a live preview of the video.
    void drawPreview();

    // Silicate parity: applies the OUTPUT-size view ONCE for the whole render
    // (acquireView) instead of toggling it per capture frame. The window (and
    // imgui-cocos, which sizes the bot menu off getFrameSize()) is therefore
    // render-sized for the whole recording — the onFrameDraw() capture no
    // longer has to flip the viewport back and forth. Called from the first
    // render-frame drawScene; restoreView() returns the view to the real
    // window size (pause menu / stop).
    void acquireView();
    void restoreView();

    // Temporarily returns the cocos view to the REAL window size while a
    // render is active, WITHOUT touching the shader-layer texture or the
    // m_viewResized gate. Used around the imgui menu draw: imgui-cocos sizes
    // io.DisplaySize off getFrameSize() and converts its vertices by the
    // CURRENT view in renderFrame(), so at the output-size view the bot menu
    // would be drawn render-scaled (tiny/huge depending on the resolution).
    // The output view is re-applied by acquireView() before the next capture.
    // No-ops until acquireView() has armed the output view AND captured a real
    // window size: the imgui swapBuffers hook also calls this on every wall
    // frame (including the level intro before the first drive frame), where a
    // zero window size must never reach the cocos view.
    void useWindowViewForUi();

    // CCEGLView hooks (render_hooks.cpp): while a render holds the view at the
    // OUTPUT size, user window-resize events must NOT reach the cocos view
    // (they would fight the render size). Returning true swallows the event;
    // the new window size is remembered so restoreView() can still restore the
    // real window. Outside a render these return false (the event passes
    // through untouched).
    bool handleFrameSizeChange(float width, float height);
    bool handleFramebufferSizeChange(int width, int height);
    bool handleWindowSizeChange();

    // Pause-menu variant of restoreView(): returns the view from the output
    // size to the real window so the pause dialog renders normally. The output
    // view is re-applied lazily by acquireView() on the next render frame.
    void restoreViewForPauseMenu();

    // Main-thread finalize: close the queue, join the encode thread (it
    // flushes the codec and writes the trailer), free the encoder. Always
    // completes. Safe to call multiple times.
    void stop();

    // Audio-kick audit: the deferred song restart fires from the FMOD-update
    // hook (render_hooks.cpp tickPendingMusicRestart), not from the renderer's
    // own drive path - this is how that side reports the success, so the
    // "NEVER fired" badge in av-sync/Done lines is a fact, not a guess.
    void noteMusicRestartFired() { m_audioKickResolved = true; }

    bool isRecording() const { return mRecording.load(); }

    // Pause-edge tracking: one-shot logging of a pause/resume that happens
    // while the render is running (both streams hold, nothing is recorded).
    // The hold itself is timed too: a pause produces no frames and no sim
    // work, so it is dead time, not render time. renderElapsedSeconds() bills
    // it to mPausedWallSecs and the wall clock in the HUD/Summary shows what
    // the render actually cost.
    bool levelPauseLogged() const { return mLevelPauseLogged; }
    void noteLevelPaused();
    void noteLevelResumed();
    [[nodiscard]] double pausedWallSeconds() const { return mPausedWallSecs; }

    struct RenderSettings {
        unsigned width = 1920;
        unsigned height = 1080;
        int fps = 60;
        int bitrateMbps = 50;  // ABR target; higher = better quality/bigger file
        bool audioEnabled = false;  // capture level audio into the MP4
        std::string audioCodec = "aac";  // aac | libmp3lame | libopus | flac | ...
        int audioBitrate = 320;          // kbps
        float musicVolume = 1.0f;   // in-game Music volume forced for the render
        float sfxVolume = 1.0f;     // in-game SFX volume forced for the render
        double fadeInSeconds = 0.0;   // fade from black at video start (0 = off)
        double fadeOutSeconds = 0.0;  // fade to black after level complete (0 = off)
        // Audio fade (applied to the captured mixer output, independent of the
        // video fade): ramp the level in at the start of the track and out
        // over the live trailer at the end.
        double audioFadeInSec = 0.0;
        double audioFadeOutSec = 0.0;
    };

    void applySettings(const RenderSettings& s);
    RenderSettings settings() const;

    // Enumerates encoders present in the bundled FFmpeg (via av_codec_iterate,
    // names like "libx264", "h264_nvenc", "libx265", "hevc_nvenc", ...).
    // Empty until loadFFmpeg() succeeds.
    const std::vector<std::string>& availableEncoders() const {
        return m_encoderNames;
    }
    // Selects the encoder used by the next begin(). No auto-detection: the
    // exact encoder the user picked is used (the GUI resolves the default:
    // libx264 вЂ” works on any machine, GPU or not).
    void setEncoder(const std::string& name) { m_encoderSel = name; }
    const std::string& encoderSel() const { return m_encoderSel; }
    const std::string& encoderUsed() const { return m_encoderUsed; }

    // True when the bundled FFmpeg build provides the named audio codec
    // (only meaningful after loadFFmpeg() succeeded).
    bool hasAudioCodec(const std::string& name) const;
    const std::string& audioCodecUsed() const { return m_audioCodecUsed; }
    int audioSampleRate() const { return m_audioSampleRate; }
    int audioChannels() const { return m_audioChannels; }

    unsigned width() const { return mWidth; }
    unsigned height() const { return mHeight; }
    int fps() const { return mFps; }
    const std::string& lastOutputPath() const { return m_lastOutputPath; }

    // Live in-window preview: texture id (ImGui::Image) of the last scene
    // captured + its resolution. 0 while no render has been started.
    unsigned previewTexture() const { return mCapture.sceneTexture(); }
    unsigned previewWidth() const { return mWidth; }
    unsigned previewHeight() const { return mHeight; }

    // Wall-clock seconds of the last begin() (0 before any render). Lets the
    // GUI compute elapsed/ETA.
    double renderStartWall() const { return m_renderStartWall; }

    std::atomic<uint64_t> mCapturedFrames{0};

    // Wall seconds since the render started, and the length of the video already
    // produced. The two diverge whenever the level pauses: wall time keeps
    // running (drive() is skipped, no frames are captured) while the video
    // stands still. The HUD shows BOTH - a ticking wall clock next to a frozen
    // frame count is what made a paused render look like it was still running.
    [[nodiscard]] double renderElapsedSeconds() const;
    [[nodiscard]] double videoSeconds() const;
    [[nodiscard]] bool levelPaused() const { return mLevelPaused; }
    [[nodiscard]] uint64_t capturedFrames() const { return mCapturedFrames.load(); }
    std::atomic<uint64_t> mEncodedFrames{0};

   private:
    // Per-stage CPU cost, sampled per wall frame. The GL calls only SUBMIT work
    // - scene->visit() returns long before the GPU has rasterised anything - so
    // these numbers describe command submission, not GPU time. Their sum being
    // far below the wall-frame time is the signal that the GPU, not this code,
    // sets the rate. Frame covers the whole hijacked drawScene; gdLoop in the
    // stage line is whatever is left over, i.e. GD's own main loop.
public:
    enum class Stage { Drive, Sim, Grab, Preview, ViewUi, Swap, Frame };
    void noteStage(double ms, Stage which);
    static double nowWall();

private:
    bool startEncoder(const std::string& outPath);
    // alreadyOwned skips the copy out of `rgba` and encodes it as-is: only the
    // sync-readback fallback sets it, because that path already copied on the
    // main thread (it has to - the GL context is there and nowhere else).
    void encodeOneFrame(const uint8_t* rgba, int64_t pts, bool alreadyOwned);
    void flush();
    void freeEncoder();

    // Audio capture: FMOD master-group DSP -> offline mixer pumped to the
    // level's game time (see audiocap.hpp) -> AAC/MP3/Opus/FLAC stream
    // muxed into the same MP4 on the fly.
    bool setupAudioStream();
    void audioPump(double levelTime);
    void audioEncodePending();
    void writeAudioBlock(const float* block, int64_t pts);
    // Periodic A/V drift audit: video time (pts/fps), sim time (level time
    // minus the anchor), written audio time and the mixer's own DSP clock.
    // Any growing gap between them names the broken stage in the log instead
    // of in the finished video.
    void logAvSync(GJBaseGameLayer* layer);
    void flushAudio();
    void freeAudio();

// Ring-queue slot management (Silicat-style skip, no blocking): when the
    // ring is full (every frame still being encoded) tryAcquireSlot() returns
    // false and drive() HOLDS the sim at the current position instead of
    // stepping — the render rate naturally caps at the encoder rate with zero
    // dropped frames and the main thread never blocks. commitSlot hands the
    // filled frame to the encode thread.
    //
    // The queue no longer owns pixel buffers. It carries a POINTER into a mapped
    // PBO slot plus the slot index, because the main thread stopped copying out
    // of that mapping: at 4K that memcpy was 24.9 MB, 2.6 ms, a quarter of a
    // GRIEF frame. The encoder thread does exactly the same bytes of copying as
    // before, just from the mapping instead of from a ring buffer, and then hands
    // the slot back with FboCapture::releaseSlot.
    bool tryAcquireSlot();
    bool ringFull();
    void commitSlot(const render::GrabResult& frame, int64_t pts);
    void encodeThreadMain();

    // Tracks the physical window framebuffer size while the render holds the
    // output view, so restoreView() restores the CURRENT window size (not the
    // one captured at render start).
    void storeWindowFbSize(float fbW, float fbH);

    // Depth of the main->encoder queue. 6 was right for a reason that outlived its
    // original justification, so the number is 6 and the reason is now explicit.
    //
    // HISTORY, because the wrong version of this comment is instructive. At 16
    // the ring stopped throttling the batch exactly as predicted - batchAvg went
    // 3.9 -> 4.8 on full GRIEF 4K runs - but the wall time did NOT improve
    // (101.6 s -> 105.3 s, and that gap is inside the +-2% run-to-run spread),
    // grabMax went 12.9 ms -> 93.9 ms with a 216 ms drawScene stall, and
    // pboGate went 0 -> 310.
    //
    // What 16 actually did was release the main thread far enough ahead that it
    // stopped fitting in the 12-slot PBO pool. So the binding constraint was
    // never the ring - it is the POOL, and the ring was a second limiter sitting
    // in front of it. Loosening the second one only moved the wall to the first.
    //
    // A deeper ring would pay off if PBO_COUNT grew with it, but that is
    // ~25 MB per slot at 4K and the ceiling is close anyway: at frame=9.96 ms
    // and 10200 frames the wall time is 101.6 s to the decimal, and 63% of that
    // frame is GD's own physics. Not worth 200 MB to chase the last 3%.
    static constexpr size_t RING_SIZE = 16;

    // How long stop() waits for the encoder thread's tail (drain + flush +
    // trailer) before detaching it. A stuck/slow encoder must never freeze
    // the game, so this is deliberately short.
    static constexpr int kStopTailWaitSeconds = 8;

    ff_t m_ff = {};
    bool m_ffLoaded = false;
    std::filesystem::path m_ffDir;
    std::string m_error;

    std::atomic<bool> mRecording{false};

    FboCapture mCapture;
    unsigned mWidth = 1920;     // output (encoder) resolution
    unsigned mHeight = 1080;
    int mFps = 60;
    double mTps = 240.0;
    // BUG-96 stage 8: the speedhack value CAPTURED in begin(). The render
    // clock multiplies the video timeline by it, so footage shows the level
    // running at speed x video time INDEPENDENT of the live timing mode
    // (Lock Delta / Real Time). Read once per render: a mid-render slider
    // change must not re-time the video.
    double mSpeed = 1.0;

    int64_t mLastPts = -1;
    uint64_t mLastFramed = 0;    // last bot frame stepped (backward-jump guard)
    double mTrailerRemain = 0.0;  // remaining after-end seconds
    uint64_t mPostMacroTicks = 0;  // ticks simmed after macro end (timeout)
    uint32_t mGrabFailStreak = 0;  // consecutive grab() failures (abort guard)
    uint32_t mRingFullFrames = 0;  // consecutive full-ring drive calls (abort
                                   // guard for a dead encoder)
    // TOTAL (not consecutive) drive calls turned away by hasFreeSlot() alone.
    // Zero is the healthy value; anything else means the PBO pool is the thing
    // pacing the render, which is a design fact worth seeing in the summary
    // rather than inferring from a wall clock that could have many causes.
    uint32_t mPboGateFrames = 0;

    // WHY the batch stops early. `batch=6` with 4.1 produced per call means
    // the loop is breaking before its limit, and "why" was the one part of the
    // pacing nobody could answer: the two stop signals are distinct failures
    // with distinct fixes, and lumping them into a single batch number hid
    // both. mBatchStopDrive = sim/backpressure, mBatchStopDraw = the capture
    // itself could not be committed, mBatchDeliveredFull = the limit was hit.
    uint64_t mBatchCalls = 0;
    uint64_t mBatchProduced = 0;
    uint64_t mBatchDeliveredFull = 0;
    uint64_t mBatchStopDrive = 0;
    uint64_t mBatchStopDraw = 0;

    // Pending capture, set by drive() at a pts boundary, executed by
    // onFrameDraw() at the start of the ImGui draw callback.
    bool m_pendingFrame = false;   // a capture is queued for the draw callback
    int64_t m_pendingPts = -1;     // pts of the queued frame
    float m_pendingFadeOut = 1.0f;  // fade-out ramp for the live trailer

    // Batched capture: how many video frames one drawScene may produce.
    //
    // Stereo Madness measured driveHz=181 at frame=5.52ms on a 180 Hz panel -
    // the wall frame was pinned to the display refresh while the video only
    // needed 60 frames per second, so three drawScenes were spent waiting for a
    // vertical blank to deliver one needed frame. GRIEF ran the same code at
    // 81 Hz on a 12.34ms frame, still 1.35x more often than the video needs.
    //
    // Producing K frames per drawScene turns that idle wait into work. It is
    // safe because issueReadback() re-renders the scene into its own FBO rather
    // than reading the presented backbuffer, so a captured frame does not have
    // to coincide with a drawScene.
    //
    // Adaptive, not a constant: NVENC has a throughput ceiling, and overshooting
    // it shows up as a permanently full encoder ring. Grown while a full batch
    // completed without backpressure, halved the moment the ring fills.
    int m_batchLimit = 3;
    // PBO_COUNT - 2, sized to pool depth (16 - 2 = 14).
    static constexpr int kBatchMax = 14;
    // Hysteresis, in consecutive drawScenes. Fast ramp-up (2 full batches to grow).
    int m_batchFullStreak = 0;
    int m_batchShortStreak = 0;
    static constexpr int kGrowStreak = 2;
    static constexpr int kShrinkStreak = 3;

    // Diagnostics (reset in begin(), dumped periodically + at stop())
    double m_renderStartWall = 0.0;  // wall time of begin() (s)
    uint64_t m_driveCalls = 0;       // drive() invocations (scheduler rate)
    uint64_t m_stepCount = 0;        // physics ticks stepped total
    uint64_t m_grabFailTotal = 0;    // total grab() failures
    uint64_t m_pendingDrops = 0;     // pending captures overwritten (lost)
    // ---- Render clock (see driveOneFrame) ----
    // mClockTick counts the physics ticks this render has actually stepped.
    // Video frame k targets round(k * tps / fps) ticks, so the content time is
    // a function of the FRAME INDEX instead of a measurement of where the sim
    // happened to land. That is what makes a captured frame unable to straddle
    // two pts slots - the old tick-derived pts could jump by two when
    // tps/fps was not an integer, and the swallowed slot was the "sometimes
    // faster, sometimes slower" judder of a high-TPS render.
    uint64_t mClockTick = 0;
    int64_t mFrameTicksMin = 0;      // min ticks in one captured frame (0=unset)
    int64_t mFrameTicksMax = 0;      // max ticks in one captured frame
    int64_t mClockAskedSum = 0;      // sum of requested ticks (per stats window)
    int64_t mClockDoneSum = 0;       // sum of ticks the sim actually advanced
    uint64_t mClockMismatch = 0;     // frames where the sim did not advance by n
    uint64_t mClockZeroFrames = 0;   // frames that stepped zero ticks
    uint64_t mClockCapHits = 0;      // frames clamped by kClockTicksCap
    uint64_t mClockStallFrames = 0;  // consecutive frames with no sim progress
    int mClockLogs = 0;
    double m_grabMs = 0.0;           // cumulative grab() wall time (ms)
    uint64_t m_grabN = 0;            // grab() executions
    // Worst single grab. An AVERAGE cannot show a stall: one 617ms grab spread
    // over 10199 frames adds 0.06ms to grabAvg=2.6ms and is invisible. The only
    // symptom was a Stage::Frame stall whose own text said "grab is NOT counted
    // here" - so the one place the stall could have been was the one place with
    // no number able to show it. Hence the max.
    double m_grabMaxMs = 0.0;
    uint64_t m_grabMaxPts = 0;
    double m_lastStatsWall = 0.0;    // last periodic stats dump (wall s)
    bool m_firstCaptureLogged = false;
    bool m_pauseLogged = false;  // pause-menu notice logged once per render
    int m_diagLogged = 0;
    bool mUsingNvenc = false;
    std::vector<std::string> m_encoderNames;
    std::string m_encoderSel;
    std::string m_encoderUsed;

    int mBitrateMbps = 50;
    double m_fadeInSeconds = 0.0;
    double m_fadeOutSeconds = 0.0;
    // Sequential video pts (video FRAME units, starting at 0), assigned on the
    // encode thread instead of trusting the capture-pipeline labels: the
    // pipeline can mislabel the first frames after a grab failure / GL
    // hiccup (observed: frame 2 written at pts 613 instead of 256, sinking
    // the container average below the nominal fps). A strict counter keeps
    // the stream at EXACTLY mFps, uniformly, from pts 0. Reset to 0 in
    // begin() - a leftover count from a previous render session shifts the
    // whole video track by the old length (movie duration doubled, audio/video
    // desync; 04:00 session wrote 802 extra pts).
    int64_t m_videoPts = 0;
    std::string m_lastOutputPath;

    // pts diagnostics (TEMP): logs the first few encode->muxer hops so the
    // fps deviation can be traced end-to-end (which pts the counter fed the
    // encoder, what the codec returned, what the rescale produced).
    int m_diagPackets = 0;
    int64_t m_lastSentPts = -1;

    // Audio settings (from applySettings/begin)
    // m_audioWanted is the pure SETTING (Capture Audio on/off) and survives a
    // render: stop() must not clear it. m_audioEnabled is the RUNTIME latch
    // ("capture is attached for THIS render") — begin() seeds it from
    // m_audioWanted, and stop()/failure paths clear it. Before the split, the
    // setting and the runtime flag were the same member, so after a render
    // with audio the flag stayed false forever and every later render started
    // video-only unless the user happened to edit a render setting (which
    // re-ran applySettings) before pressing START.
    bool m_audioWanted = false;
    bool m_audioEnabled = false;
    std::string m_audioCodecSel = "aac";
    int m_audioBitrate = 320;  // kbps
    double m_audioFadeInSec = 0.0;
    double m_audioFadeOutSec = 0.0;
    // Counting-down mirror of the video trailer's mTrailerRemain, decremented
    // in the same trailer path so the audio fade-out ramp lands on the same
    // frames the video fade does.
    double m_audioFadeOutRemain = 0.0;
    // AUDIO-FADE DIAGNOSTIC: counter of logged audio-fade samples per render.
    int m_audioFadeDiag = 0;
    std::string m_audioCodecUsed;
    // In-game volumes forced on FMODAudioEngine for the render (silicate
    // parity: attach() saves the previous values and detach() restores them).
    float m_musicVolume = 1.0f;
    float m_sfxVolume = 1.0f;
    bool mLevelPauseLogged = false;
    // Mirrors the level's own paused state, sampled from PlayLayer rather than
    // inferred from the log-once flag above.
    bool mLevelPaused = false;
    // Wall time spent inside a level pause, plus when the current hold began.
    // Subtracted from renderElapsedSeconds() so the reported wall time is the
    // time the renderer actually worked.
    double mPausedWallSecs = 0.0;
    double mPauseStartWall = 0.0;

    double m_stageMs[7] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    // Set when a named stage blows past the stall threshold in the current
    // frame, cleared at the top of drive(). Lets the Stage::Frame check tell
    // "one of ours was slow" apart from "the frame was slow and none of ours
    // were", which is the difference between a bug in this file and time spent
    // inside GD's own main loop.
    bool m_stageStallPending = false;
    // Encoder-thread breakdown, in milliseconds, cumulative over the render.
    // Separate storage from m_stageMs on purpose: the GL thread writes that one
    // every frame while this is written by the encoder thread, and putting them
    // in one array would be a data race on 4K renders.
    //
    // Added because the flat-memcpy fix could not be evaluated: grabAvg measures
    // the GL thread (issueReadback) and the copy runs here, so the optimisation
    // was invisible in every number the log already carried.
    double m_encCopyMs = 0.0;   // NV12 -> the encoder frame's own buffer
    double m_encSwsMs = 0.0;    // optional NV12 -> YUV420P (software codecs)
    double m_encSendMs = 0.0;   // avcodec_send_frame, drains excluded
    double m_encDrainMs = 0.0;  // receive_packet + interleaved_write_frame
    int64_t m_encEagain = 0;    // send_frame EAGAIN retries (codec queue full)
    int m_stageN = 0;

    // Audio encoder state (main thread creates/pumps; the muxer is shared
    // with the encode thread under m_muxMu).
    AVStream* m_audioStream = nullptr;
    const AVCodec* m_audioCodec = nullptr;
    AVCodecContext* m_audioCtx = nullptr;
    AVFrame* m_audioFrame = nullptr;
    AVPacket* m_audioPkt = nullptr;
    std::vector<float> m_audioAccum;  // interleaved FLT, partial frames only
    int64_t m_audioPts = 0;           // sample counter of the next audio block
    // Level time of the first captured frame = video pts 0. The audio pump is
    // driven by the VIDEO timeline (anchor + pts/fps), not by the raw level
    // time: that keeps the produced sample count equal to the frame count by
    // construction, with no drift from sim-clock mismatches or respawn
    // rewinds. AudioCapture drops the matching pre-roll so the music starts at
    // the same position inside the track that the footage starts at.
    double m_audioAnchor = 0.0;
    int64_t m_avSyncLogPts = -1;
    // Content-speed meter (02.11): sim advance between consecutive av-sync
    // grid points / their video-time advance. A healthy render reads the
    // captured speedhack rate for the whole session; anything else is the
    // wobble the eye reported and drift(sim-video) can miss between polls.
    int64_t m_avSyncPrevPts = -1;
    double m_avSyncPrevSim = 0.0;
    bool m_avSyncHasPrev = false;
    // Audio-kick audit (02.11): did begin() fail to start the song (musicID
    // unknown / not prepared), and did the deferred restart actually fire.
    bool m_audioKickUnknown = false;
    bool m_audioKickResolved = false;
    uint64_t m_audioPackets = 0;
    int m_audioSampleRate = 0;
    int m_audioChannels = 2;
    std::mutex m_muxMu;  // serializes av_interleaved_write_frame (video/audio)

    // Async encode pipeline. The ring holds DESCRIPTORS, not buffers: `ptr`
    // points into a mapped PBO slot (or, on the rare sync fallback, into
    // FboCapture's own scratch buffer - hence preCopied), and `slot` is what
    // gets handed back to FboCapture once the encoder has finished reading it.
    struct RingEntry {
        const uint8_t* ptr = nullptr;
        int slot = -1;
        bool preCopied = false;
    };
    std::vector<RingEntry> m_ring;               // [RING_SIZE] frame descriptors
    std::vector<int64_t> m_ringPts;              // [RING_SIZE] pts per slot
    size_t m_rd = 0;  // encode-thread read cursor
    size_t m_wr = 0;  // main-thread write cursor
    std::mutex m_qMu;
    std::condition_variable m_qCv;
    bool m_qClosed = false;
    std::thread m_encThread;
    // True while the encoder thread is alive (set at its start, cleared once
    // it has flushed + written the trailer + freed its ffmpeg contexts). It is
    // the ONLY ownership flag: stop() waits on it with a timeout (a stuck
    // NVENC send_frame must never hang the game by join()-ing forever), and
    // begin() refuses to start a new render while it is still true, because a
    // second render would reassign the SAME ffmpeg contexts (mFmt/mCtx/audio)
    // that the still-running thread frees.
    std::atomic<bool> m_encRunning{false};

    const AVCodec* mCodec = nullptr;
    AVCodecContext* mCtx = nullptr;
    AVFormatContext* mFmt = nullptr;
    // Refcounted NV12 input frames. The encode thread copies each ring slot's
    // NV12 data into the writable buffer of one of these frames BEFORE the
    // ring slot is released, so the slot can be reused immediately while
    // NVENC's async copy is still reading the frame's buffer (kept alive by
    // the AVFrame refcount).
    // Back to 4. It was raised to 8 on the theory that an async NVENC holds
    // several frames in flight and a short ring would force reallocation or
    // drops. The encoder-thread timers then measured the case that theory was
    // made of: eagain=0 over a full 4794-frame 4K render, i.e. the codec queue
    // never once filled up. The extra four slots cost ~50 MB and bought nothing.
    static constexpr int ENC_FRAMES = 8;
    AVFrame* mYuvFrames[ENC_FRAMES] = {};
    int m_yuvIdx = 0;
    AVPacket* mPkt = nullptr;

    // NV12 -> YUV420P conversion (swscale) for encoders that do not accept
    // NV12 directly (libx265, libsvtav1, libvpx-vp9, mpeg4, mjpeg, prores, ...).
    // Created in startEncoder() only when the codec rejected NV12; the
    // converted frame is reused per-frame (software encoders are synchronous).
    SwsContext* m_sws = nullptr;
    AVFrame* m_convFrame = nullptr;

    // Silicate-style view change during the render: the cocos view size is
    // silently set to the OUTPUT size for the WHOLE render (acquireView) so
    // the visit()-based capture (capture pass 0) gets the level at the exact
    // output resolution. The physical window is never resized (protocol
    // setFrameSize only); its size is remembered in m_windowW/m_windowH and
    // restored in stop()/restoreView(). User window-size changes while the
    // render holds the output view are swallowed by handle*SizeChange
    // (render_hooks.cpp), with the new window framebuffer size tracked for a
    // correct restore.
    float m_windowW = 0.0f;
    float m_windowH = 0.0f;
    float m_windowFbW = 0.0f;  // physical window framebuffer (client) size
    float m_windowFbH = 0.0f;
    float m_viewToFbScaleX = 1.0f;  // cocos frame-size / framebuffer scale
    float m_viewToFbScaleY = 1.0f;
    bool m_viewResized = false;
};

}  // namespace render
}  // namespace skkbot2
