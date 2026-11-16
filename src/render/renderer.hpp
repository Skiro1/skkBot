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
struct AVStream;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVFormatContext;
#endif

int getLastMusicId();
void setLastMusicId(int id);
void armMusicRestart();
void disarmMusicRestart();
bool tickPendingMusicRestart();
bool musicRestartPending();
double musicPositionSeconds();

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
    void reloadFFmpeg() {
        if (m_ffLoaded) return;
        m_error.clear();
        m_ffDir.clear();
    }
    bool isFFmpegLoaded() const { return m_ffLoaded; }
    const std::filesystem::path& ffmpegDir() const { return m_ffDir; }
    static std::filesystem::path preferredFfmpegDir();
    static std::vector<std::filesystem::path> ffmpegSearchPaths();
    static void prepareFfmpegDropIn();
    const std::string& lastError() const { return m_error; }

    bool begin(PlayLayer* layer);

    void drive(GJBaseGameLayer* layer, std::function<void(float)> stepOne);
    bool driveOneFrame(GJBaseGameLayer* layer, std::function<void(float)> stepOne);

    bool onFrameDraw();

    void drawPreview();

    void acquireView();
    void restoreView();

    void useWindowViewForUi();

    bool handleFrameSizeChange(float width, float height);
    bool handleFramebufferSizeChange(int width, int height);
    bool handleWindowSizeChange();

    void restoreViewForPauseMenu();

    void stop();

    void noteMusicRestartFired() { m_audioKickResolved = true; }

    bool isRecording() const { return mRecording.load(); }

    bool levelPauseLogged() const { return mLevelPauseLogged; }
    void noteLevelPaused();
    void noteLevelResumed();
    [[nodiscard]] double pausedWallSeconds() const { return mPausedWallSecs; }

    struct RenderSettings {
        unsigned width = 1920;
        unsigned height = 1080;
        int fps = 60;
        int bitrateMbps = 50;
        bool audioEnabled = false;
        std::string audioCodec = "aac";
        int audioBitrate = 320;
        float musicVolume = 1.0f;
        float sfxVolume = 1.0f;
        double fadeInSeconds = 0.0;
        double fadeOutSeconds = 0.0;
        double audioFadeInSec = 0.0;
        double audioFadeOutSec = 0.0;
        bool monitorEnabled = false;
    };

    void applySettings(const RenderSettings& s);
    RenderSettings settings() const;

    const std::vector<std::string>& availableEncoders() const {
        return m_encoderNames;
    }
    void setEncoder(const std::string& name) { m_encoderSel = name; }
    const std::string& encoderSel() const { return m_encoderSel; }
    const std::string& encoderUsed() const { return m_encoderUsed; }

    bool hasAudioCodec(const std::string& name) const;
    const std::string& audioCodecUsed() const { return m_audioCodecUsed; }
    int audioSampleRate() const { return m_audioSampleRate; }
    int audioChannels() const { return m_audioChannels; }

    unsigned width() const { return mWidth; }
    unsigned height() const { return mHeight; }
    int fps() const { return mFps; }
    const std::string& lastOutputPath() const { return m_lastOutputPath; }

    unsigned previewTexture() const { return mCapture.sceneTexture(); }
    unsigned previewWidth() const { return mWidth; }
    unsigned previewHeight() const { return mHeight; }

    double renderStartWall() const { return m_renderStartWall; }

    std::atomic<uint64_t> mCapturedFrames{0};

    [[nodiscard]] double renderElapsedSeconds() const;
    [[nodiscard]] double videoSeconds() const;
    [[nodiscard]] bool levelPaused() const { return mLevelPaused; }
    [[nodiscard]] uint64_t capturedFrames() const { return mCapturedFrames.load(); }
    std::atomic<uint64_t> mEncodedFrames{0};

   private:
public:
    enum class Stage { Drive, Sim, Grab, Preview, ViewUi, Swap, Frame };
    void noteStage(double ms, Stage which);
    static double nowWall();

private:
    bool startEncoder(const std::string& outPath);
    void encodeOneFrame(const uint8_t* rgba, int64_t pts, bool alreadyOwned);
    void flush();
    void freeEncoder();

    bool setupAudioStream();
    void audioPump(double levelTime);
    void audioEncodePending();
    void writeAudioBlock(const float* block, int64_t pts);
    void logAvSync(GJBaseGameLayer* layer);
    void flushAudio();
    void freeAudio();

    bool tryAcquireSlot();
    bool ringFull();
    void commitSlot(const render::GrabResult& frame, int64_t pts);
    void encodeThreadMain();

    void storeWindowFbSize(float fbW, float fbH);

    static constexpr size_t RING_SIZE = 16;

    static constexpr int kStopTailWaitSeconds = 8;

    ff_t m_ff = {};
    bool m_ffLoaded = false;
    std::filesystem::path m_ffDir;
    std::string m_error;

    std::atomic<bool> mRecording{false};

    FboCapture mCapture;
    unsigned mWidth = 1920;
    unsigned mHeight = 1080;
    int mFps = 60;
    double mTps = 240.0;
    double mSpeed = 1.0;

    int64_t mLastPts = -1;
    uint64_t mLastFramed = 0;
    double mTrailerRemain = 0.0;
    uint64_t mPostMacroTicks = 0;
    uint32_t mGrabFailStreak = 0;
    uint32_t mRingFullFrames = 0;
    uint32_t mPboGateFrames = 0;

    uint64_t mBatchCalls = 0;
    uint64_t mBatchProduced = 0;
    uint64_t mBatchDeliveredFull = 0;
    uint64_t mBatchStopDrive = 0;
    uint64_t mBatchStopDraw = 0;

    bool m_pendingFrame = false;
    int64_t m_pendingPts = -1;
    float m_pendingFadeOut = 1.0f;

    int m_batchLimit = 3;
    static constexpr int kBatchMax = 14;
    int m_batchFullStreak = 0;
    int m_batchShortStreak = 0;
    static constexpr int kGrowStreak = 2;
    static constexpr int kShrinkStreak = 3;

    double m_renderStartWall = 0.0;
    uint64_t m_driveCalls = 0;
    uint64_t m_stepCount = 0;
    uint64_t m_grabFailTotal = 0;
    uint64_t m_pendingDrops = 0;
    uint64_t mClockTick = 0;
    int64_t mFrameTicksMin = 0;
    int64_t mFrameTicksMax = 0;
    int64_t mClockAskedSum = 0;
    int64_t mClockDoneSum = 0;
    uint64_t mClockMismatch = 0;
    uint64_t mClockZeroFrames = 0;
    uint64_t mClockCapHits = 0;
    uint64_t mClockStallFrames = 0;
    int mClockLogs = 0;
    double m_grabMs = 0.0;
    uint64_t m_grabN = 0;
    double m_grabMaxMs = 0.0;
    uint64_t m_grabMaxPts = 0;
    double m_lastStatsWall = 0.0;
    bool m_firstCaptureLogged = false;
    bool m_pauseLogged = false;
    int m_diagLogged = 0;
    bool mUsingNvenc = false;
    std::vector<std::string> m_encoderNames;
    std::string m_encoderSel;
    std::string m_encoderUsed;

    int mBitrateMbps = 50;
    double m_fadeInSeconds = 0.0;
    double m_fadeOutSeconds = 0.0;
    int64_t m_videoPts = 0;
    std::string m_lastOutputPath;

    int m_diagPackets = 0;
    int64_t m_lastSentPts = -1;

    bool m_audioWanted = false;
    bool m_audioEnabled = false;
    std::string m_audioCodecSel = "aac";
    int m_audioBitrate = 320;
    double m_audioFadeInSec = 0.0;
    double m_audioFadeOutSec = 0.0;
    double m_audioFadeOutRemain = 0.0;
    int m_audioFadeDiag = 0;
    std::string m_audioCodecUsed;
    float m_musicVolume = 1.0f;
    float m_sfxVolume = 1.0f;
    bool mLevelPauseLogged = false;
    bool mLevelPaused = false;
    double mPausedWallSecs = 0.0;
    double mPauseStartWall = 0.0;

    double m_stageMs[7] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    bool m_stageStallPending = false;
    double m_encCopyMs = 0.0;
    double m_encSwsMs = 0.0;
    double m_encSendMs = 0.0;
    double m_encDrainMs = 0.0;
    int64_t m_encEagain = 0;
    int m_stageN = 0;

    AVStream* m_audioStream = nullptr;
    const AVCodec* m_audioCodec = nullptr;
    AVCodecContext* m_audioCtx = nullptr;
    AVFrame* m_audioFrame = nullptr;
    AVPacket* m_audioPkt = nullptr;
    std::vector<float> m_audioAccum;
    int64_t m_audioPts = 0;
    double m_audioAnchor = 0.0;
    int64_t m_avSyncLogPts = -1;
    int64_t m_avSyncPrevPts = -1;
    double m_avSyncPrevSim = 0.0;
    bool m_avSyncHasPrev = false;
    bool m_audioKickUnknown = false;
    bool m_audioKickResolved = false;
    uint64_t m_audioPackets = 0;
    int m_audioSampleRate = 0;
    int m_audioChannels = 2;
    std::mutex m_muxMu;

    struct RingEntry {
        const uint8_t* ptr = nullptr;
        int slot = -1;
        bool preCopied = false;
    };
    std::vector<RingEntry> m_ring;
    std::vector<int64_t> m_ringPts;
    size_t m_rd = 0;
    size_t m_wr = 0;
    std::mutex m_qMu;
    std::condition_variable m_qCv;
    bool m_qClosed = false;
    std::thread m_encThread;
    std::atomic<bool> m_encRunning{false};

    const AVCodec* mCodec = nullptr;
    AVCodecContext* mCtx = nullptr;
    AVFormatContext* mFmt = nullptr;
    static constexpr int ENC_FRAMES = 8;
    AVFrame* mYuvFrames[ENC_FRAMES] = {};
    int m_yuvIdx = 0;
    AVPacket* mPkt = nullptr;

    SwsContext* m_sws = nullptr;
    AVFrame* m_convFrame = nullptr;

    float m_windowW = 0.0f;
    float m_windowH = 0.0f;
    float m_windowFbW = 0.0f;
    float m_windowFbH = 0.0f;
    float m_viewToFbScaleX = 1.0f;
    float m_viewToFbScaleY = 1.0f;
    bool m_viewResized = false;
};

}
}
