#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
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
    bool isFFmpegLoaded() const { return m_ffLoaded; }
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

    // Called at the START of the ImGui draw callback (i.e. right after the
    // game drew the current scene and before the GUI overlay is rendered):
    // executes the capture pending from drive()'s last pts boundary. This way
    // the video gets a FRESH backbuffer with NO bot menu in it, while the menu
    // stays visible on screen during the whole render.
    void onFrameDraw();

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

    bool isRecording() const { return mRecording.load(); }

    // Pause-edge tracking: one-shot logging of a pause/resume that happens
    // while the render is running (both streams hold, nothing is recorded).
    bool levelPauseLogged() const { return mLevelPauseLogged; }
    void noteLevelPaused() { mLevelPauseLogged = true; }
    void noteLevelResumed() { mLevelPauseLogged = false; }

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
    std::atomic<uint64_t> mEncodedFrames{0};

   private:
    bool startEncoder(const std::string& outPath);
    void encodeOneFrame(uint8_t* rgba, int64_t pts);
    void flush();
    void freeEncoder();

    // Audio capture: FMOD master-group DSP -> offline mixer pumped to the
    // level's game time (see audiocap.hpp) -> AAC/MP3/Opus/FLAC stream
    // muxed into the same MP4 on the fly.
    bool setupAudioStream();
    void audioPump(double levelTime);
    void audioEncodePending();
    void writeAudioBlock(const float* block, int64_t pts);
    void flushAudio();
    void freeAudio();

// Ring-queue slot management (Silicat-style skip, no blocking): when the
    // ring is full (every buffer still being encoded) tryAcquireSlot() returns
    // nullptr and drive() HOLDS the sim at the current position instead of
    // stepping — the render rate naturally caps at the encoder rate with zero
    // dropped frames and the main thread never blocks. commitSlot hands the
    // filled buffer to the encode thread.
    uint8_t* tryAcquireSlot();
    bool ringFull();
    void commitSlot(int64_t pts);
    void encodeThreadMain();

    // Tracks the physical window framebuffer size while the render holds the
    // output view, so restoreView() restores the CURRENT window size (not the
    // one captured at render start).
    void storeWindowFbSize(float fbW, float fbH);

    static constexpr size_t RING_SIZE = 6;

    // How long stop() waits for the encoder thread's tail (drain + flush +
    // trailer) before detaching it. A stuck/slow encoder must never freeze
    // the game, so this is deliberately short.
    static constexpr int kStopTailWaitSeconds = 8;

    ff_t m_ff = {};
    bool m_ffLoaded = false;
    std::string m_error;

    std::atomic<bool> mRecording{false};

    FboCapture mCapture;
    unsigned mWidth = 1920;     // output (encoder) resolution
    unsigned mHeight = 1080;
    int mFps = 60;
    double mTps = 240.0;

    int64_t mLastPts = -1;
    uint64_t mLastFramed = 0;    // last bot frame stepped (backward-jump guard)
    double mTrailerRemain = 0.0;  // remaining after-end seconds
    uint64_t mPostMacroTicks = 0;  // ticks simmed after macro end (timeout)
    uint32_t mGrabFailStreak = 0;  // consecutive grab() failures (abort guard)
    uint32_t mRingFullFrames = 0;  // consecutive full-ring drive calls (abort
                                   // guard for a dead encoder)

    // Pending capture, set by drive() at a pts boundary, executed by
    // onFrameDraw() at the start of the ImGui draw callback.
    bool m_pendingFrame = false;   // a capture is queued for the draw callback
    int64_t m_pendingPts = -1;     // pts of the queued frame
    float m_pendingFadeOut = 1.0f;  // fade-out ramp for the live trailer

    // Diagnostics (reset in begin(), dumped periodically + at stop())
    double m_renderStartWall = 0.0;  // wall time of begin() (s)
    uint64_t m_driveCalls = 0;       // drive() invocations (scheduler rate)
    uint64_t m_stepCount = 0;        // physics ticks stepped total
    uint64_t m_grabFailTotal = 0;    // total grab() failures
    uint64_t m_pendingDrops = 0;     // pending captures overwritten (lost)
    double m_grabMs = 0.0;           // cumulative grab() wall time (ms)
    uint64_t m_grabN = 0;            // grab() executions
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

    // Audio encoder state (main thread creates/pumps; the muxer is shared
    // with the encode thread under m_muxMu).
    AVStream* m_audioStream = nullptr;
    const AVCodec* m_audioCodec = nullptr;
    AVCodecContext* m_audioCtx = nullptr;
    AVFrame* m_audioFrame = nullptr;
    AVPacket* m_audioPkt = nullptr;
    std::vector<float> m_audioAccum;  // interleaved FLT, partial frames only
    int64_t m_audioPts = 0;           // sample counter of the next audio block
    uint64_t m_audioPackets = 0;
    int m_audioSampleRate = 0;
    int m_audioChannels = 2;
    std::mutex m_muxMu;  // serializes av_interleaved_write_frame (video/audio)

    // Async encode pipeline
    std::vector<std::vector<uint8_t>> m_ring;  // [RING_SIZE] RGBA buffers
    std::vector<int64_t> m_ringPts;            // [RING_SIZE] pts per slot
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
    static constexpr int ENC_FRAMES = 4;
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
