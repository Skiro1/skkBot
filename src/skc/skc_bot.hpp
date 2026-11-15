#pragma once

#include "skc_format.hpp"

#include <Geode/Geode.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace skk::skc {

enum class State {
    Disabled,
    Record,
    Playback,
};

enum class PendingAction {
    None,
    Record,
    Play,
};

class Bot {
public:
    static Bot& get();

    static void applySessionAccuracy(bool cbs, bool cos);

    static void restoreUserAccuracy();
    void reapplySessionAccuracy() const;


    void startRecording();

    void startPlayback();

    void armPlayback();

    enum class StartDecision {
        Ready,
        NoLevel,
        Running,
        NoInputs,
        MidAttempt,
        Ended,
        WrongMode,
    };

    struct PlaybackStart {
        StartDecision decision = StartDecision::NoLevel;
        uint32_t clockFrame = 0;
        uint32_t firstActionFrame = 0;
        const char* reason = "";
    };

    [[nodiscard]] PlaybackStart playbackStartCheck() const;

    bool startPlaybackIfReady(PlaybackStart* out);

    [[nodiscard]] bool platformerModeMismatch() const;

    [[nodiscard]] std::string describeStartBlock(const PlaybackStart& start) const;

    [[nodiscard]] bool isPlaybackArmed() const { return m_playbackArmed; }

    void stop();

    bool consumePendingAction(GJGameLevel* level);

    [[nodiscard]] bool isPlaybackExhausted() const {
        return m_state == State::Playback && m_inputIndex >= m_macro.actions.size();
    }
    [[nodiscard]] bool isPlaybackExhaustionNotified() const { return m_playbackExhaustionNotified; }

    void setState(State state) { m_state = state; }
    [[nodiscard]] State getState() const { return m_state; }
    [[nodiscard]] bool isRecording() const { return m_state == State::Record; }
    [[nodiscard]] bool isPlaying() const { return m_state == State::Playback; }
    [[nodiscard]] bool isDeathCapturePaused() const { return m_deathNoted; }
    void resumeCaptureAfterReset() { m_deathNoted = false; }
    [[nodiscard]] bool isEnabled() const { return m_state != State::Disabled; }

    void setPendingAction(PendingAction action) { m_pendingAction = action; }
    [[nodiscard]] PendingAction getPendingAction() const { return m_pendingAction; }
    void clearPendingAction() { m_pendingAction = PendingAction::None; }


    [[nodiscard]] size_t getInputCount() const { return m_macro.actions.size(); }
    [[nodiscard]] size_t getInputIndex() const { return m_inputIndex; }
    [[nodiscard]] uint32_t peekNextInputFrame() const {
        return m_inputIndex < m_macro.actions.size()
                   ? m_macro.actions[m_inputIndex].frame
                   : 0u;
    }
    [[nodiscard]] const std::vector<Action>& getActions() const { return m_macro.actions; }
    [[nodiscard]] const Macro& getMacro() const { return m_macro; }
    Macro& macroData() { return m_macro; }

    void noteRngTypedInput() { m_rngTypedInput = true; }
    [[nodiscard]] bool consumeRngTypedInput() {
        const bool value = m_rngTypedInput;
        m_rngTypedInput = false;
        return value;
    }

    [[nodiscard]] double getTps() const { return m_macro.header.tps; }
    void setTps(double tps) {
        if (tps > 0.0 && std::isfinite(tps)) m_macro.header.tps = tps;
    }

    [[nodiscard]] float getDisplayFps() const { return m_macro.header.fps; }
    void setDisplayFps(float fps) {
        if (fps >= 0.0f && std::isfinite(fps)) m_macro.header.fps = fps;
    }


    [[nodiscard]] const std::string& getMacroName() const { return m_macroName; }
    void setMacroName(const std::string& name) { m_macroName = name; }

    [[nodiscard]] bool hasInputs() const { return !m_macro.actions.empty(); }

    void setIgnoreInputs(bool ignore) { m_ignoreInputs = ignore; }
    [[nodiscard]] bool getIgnoreInputs() const { return m_ignoreInputs; }

    [[nodiscard]] bool ownsTiming() const;


    void recordInput(uint32_t frame, uint8_t button, bool player2, bool pressed);

    [[nodiscard]] std::optional<Action> poll(uint32_t frame);

    enum class SeamQueueMode {
        Record,
        Play,
    };

    struct SeamQueueReport {
        size_t inspected = 0;
        size_t cleared = 0;
        size_t extraRelease = 0;
        size_t extraPush = 0;
        size_t ghostRelease = 0;
        bool clearedQueue = false;
        const char* rule = nullptr;
    };

    SeamQueueReport sanitizeSeamQueue(std::vector<PlayerButtonCommand>& queue,
                                      uint32_t resumeFrame, SeamQueueMode mode,
                                      const char* site);

    struct HeldState {
        bool held[2][3] = {};
    };
    [[nodiscard]] HeldState heldBefore(uint32_t frame) const;

    enum class ResetReason {
        AttemptFromStart,
        PracticeRespawn,
        CheckpointRestore,
        LevelChanged,
        LevelExit,
        UserClear,
    };

    bool onResetEvent(ResetReason reason, uint32_t boundaryFrame);

    void restart();

    void noteInternalRestore(uint32_t frame);
    void reconcileCaptureRestore(uint32_t frame);
    void finishCaptureRestore(uint32_t frame);
    void resetHoldShadow();
    void syncHoldShadowToPlayer();

    void noteAttemptReset(uint32_t failedAtFrame);
    [[nodiscard]] uint32_t getAttemptCount() const { return m_attemptCount; }

    void seekTo(uint32_t frame);



    [[nodiscard]] bool save(const std::filesystem::path& path);
    [[nodiscard]] bool load(const std::filesystem::path& path);

    [[nodiscard]] bool adopt(Macro macro, std::string const& name,
                             uint32_t levelId = 0, std::string levelName = {});

    void onLevelExit();

    bool onLevelEnter(GJGameLevel* level);

    void bindLevel(GJGameLevel* level);

    void onPlaybackExhausted();

private:

    void prepareCaptureRestore(uint32_t frame);
    void closeSeamHolds(uint32_t frame);

    void clearInputs();

    Bot() = default;

    State m_state = State::Disabled;
    PendingAction m_pendingAction = PendingAction::None;
    bool m_playbackArmed = false;
    bool m_ignoreInputs = true;

    uint32_t m_attemptCount = 0;
    uint32_t m_lastFailureFrame = 0;
    bool m_deathNoted = false;
    bool m_playbackExhaustionNotified = false;
    size_t m_lastTrimmed = 0;
    size_t m_droppedEvents = 0;
    size_t m_duplicateEvents = 0;
    size_t m_suppressedEvents = 0;

    bool m_holdShadow[2][4] = {};

    uint32_t m_suppressUntilFrame = 0;
    uint32_t m_suppressFromFrame = 0;
    bool m_suppressionArmed = false;

    double m_savedTickRate = 240.0;

    Macro m_macro;
    std::string m_macroName;
    size_t m_inputIndex = 0;
    bool m_rngTypedInput = false;

    int32_t m_levelId = 0;
    std::string m_levelName;
};

}
