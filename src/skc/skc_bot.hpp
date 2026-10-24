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

// ---------------------------------------------------------------------------
// SkcBot — the record/playback engine for the .skc format.
//
// Deliberately NOT built on the SKK ReplayEngine. SKK's engine is a
// physics-replay machine: its whole purpose is the per-frame pin, the anchor
// reconcile and the RNG boundary, and every one of those would be dead weight
// (and a source of drift) for a format that stores no physics at all. SKC is
// a sibling, not a client: it shares only the two things both genuinely need
// — the timing grid (physicsbypass) and the checkpoint snapshot type.
//
// Recording mirrors what every physics-free reference bot does: hook
// handleButton, stamp the event with the current frame counter. Playback
// polls the same counter from the processCommands hook — the one place where
// the clock advances, so cursor and frame can never drift apart — and pushes
// each event through vanilla handleButton, the same way SKK does.
//
// The clock comes from FrameClock, which counts physics steps rather than
// trusting m_currentProgress — see skc_frame.hpp for why. Counting steps also
// makes the format immune to the speedhack: gameSpeed scales wall-clock time,
// not the number of physics steps, so a click still lands on the same physics
// state at 0.4x and at 2x.
//
// Practice handling: on loadFromCheckpoint the recorded inputs after the
// checkpoint frame are trimmed (the future the player just undid must not be
// replayed), and PracticeFix re-pins the state GD's restore drops.
// ---------------------------------------------------------------------------
class Bot {
public:
    static Bot& get();

    // Force Click-Between-Steps / Click-On-Steps off: game variables AND the
    // live layer. Safe to call as often as you like - GD can re-read these on
    // resetLevel, and a practice respawn is a resetLevel, so a session that only
    // forced them once at start would start recording on half-steps after the
    // first death. EclipseMenu re-applies its settings from resetLevel for
    // exactly this reason (Bot.cpp:341).
    static void forceVanillaAccuracy();

    // Put the user's own CBS / COS back, exactly as they stood before the first
    // forceVanillaAccuracy() of this session, and forget the snapshot so the next
    // session re-reads GD rather than our own leftovers.
    //
    // Without this, forcing vanilla accuracy was not a session setting but a
    // permanent edit: GameManager game variables are persisted into the user's
    // save file, so every SKC run silently turned both options off for good, and
    // nothing in the mod ever said it had. SKK has had the matching pair for
    // months (beginReplayAccuracyEnvironment / endReplayAccuracyEnvironment,
    // skkBot.hpp:1739 / 1796) - SKC only ever got the first half.
    static void restoreUserAccuracy();

    // --- lifecycle ---------------------------------------------------------

    // Record: start capturing from the next handleButton. Does NOT restart the
    // level (same rule as SKK's beginCapture).
    void startRecording();

    // Playback, immediate variant: assumes the level was just (re)started, or
    // that the caller did it (beta_actions does the resetLevel the same way
    // SKK's beginExecutionImmediate does).
    void startPlayback();

    // Playback, armed variant: the macro is loaded and the bot is waiting for
    // a level (re)start to take over. Mirrors SKK's pendingPlaybackStart - the
    // run stays under manual control until the next attempt, no teleport.
    void armPlayback();

    // --- can playback start RIGHT HERE? ---------------------------------------
    //
    // SKC stores inputs and no physics, and every action carries a frame in the
    // FrameClock's own units counted from the start of the attempt. So the
    // macro reproduces one place and one place only: a fresh attempt. Anywhere
    // else the clicks would fire on a player who is somewhere the macro never
    // put them, and the failure is silent - the video looks like the bot ran
    // but nothing lines up.
    //
    // This is why the old code armed instead of starting: `runActive` was true
    // for the most common case of all - a normal level, attempt just started,
    // player alive - so pressing PLAY did nothing until the player died. The
    // clock tells the two apart, which runActive could not.
    enum class StartDecision {
        Ready,       // fresh attempt: start now, do NOT arm
        NoLevel,     // not inside a level: queue it
        Running,     // already recording or playing
        NoInputs,    // nothing loaded
        MidAttempt,  // the clock has advanced past the attempt start
        Ended,       // level finished / end animation running
    };

    struct PlaybackStart {
        StartDecision decision = StartDecision::NoLevel;
        uint32_t clockFrame = 0;       // where the SKC clock actually is
        uint32_t firstActionFrame = 0; // where the macro's first click is
        const char* reason = "";
    };

    // Pure inspection - touches nothing, changes no state. The GUI calls it to
    // decide between starting, queueing and arming, and to tell the operator
    // WHY it will not start rather than leaving them waiting for a death.
    [[nodiscard]] PlaybackStart playbackStartCheck() const;

    // startPlayback() when playbackStartCheck() says Ready, otherwise leave the
    // bot alone. Returns what it did through `out`.
    bool startPlaybackIfReady(PlaybackStart* out);

    // One line for the notification: says where the clock is and where the
    // macro starts, so "it will not play" becomes a number the user can act on.
    [[nodiscard]] std::string describeStartBlock(const PlaybackStart& start) const;

    // True when the macro is loaded and armed for the next attempt. The GUI
    // transport gates and the editor playtest hook consult this.
    [[nodiscard]] bool isPlaybackArmed() const { return m_playbackArmed; }

    void stop();

    // Called from the PlayLayer::init hook: fire a Record/Play that was queued
    // from outside a level, exactly like ReplayEngine::consumePendingAction.
    // Returns true when an action was consumed.
    bool consumePendingAction(GJGameLevel* level);

    // True when the loaded macro is exhausted mid-playback while the player is
    // still alive. Mirrors SKK's playbackExhaustionNotified: control returns to
    // vanilla GD instead of freezing the player.
    [[nodiscard]] bool isPlaybackExhausted() const {
        return m_state == State::Playback && m_inputIndex >= m_macro.actions.size();
    }

    void setState(State state) { m_state = state; }
    [[nodiscard]] State getState() const { return m_state; }
    [[nodiscard]] bool isRecording() const { return m_state == State::Record; }
    [[nodiscard]] bool isPlaying() const { return m_state == State::Playback; }
    [[nodiscard]] bool isEnabled() const { return m_state != State::Disabled; }

    void setPendingAction(PendingAction action) { m_pendingAction = action; }
    [[nodiscard]] PendingAction getPendingAction() const { return m_pendingAction; }
    void clearPendingAction() { m_pendingAction = PendingAction::None; }

    // --- macro data --------------------------------------------------------

    [[nodiscard]] size_t getInputCount() const { return m_macro.actions.size(); }
    [[nodiscard]] size_t getInputIndex() const { return m_inputIndex; }
    [[nodiscard]] const std::vector<Action>& getActions() const { return m_macro.actions; }
    [[nodiscard]] const Macro& getMacro() const { return m_macro; }

    [[nodiscard]] float getTps() const { return m_macro.header.tps; }
    void setTps(float tps) {
        if (tps > 0.0f && std::isfinite(tps)) m_macro.header.tps = tps;
    }

    // The display rate the macro was recorded under. 0 = follow TPS, which is
    // also what gets written for the overwhelming majority of macros: a session
    // that ran at the tick rate carries no second number because it would be the
    // same number.
    [[nodiscard]] float getDisplayFps() const { return m_macro.header.fps; }
    void setDisplayFps(float fps) {
        if (fps >= 0.0f && std::isfinite(fps)) m_macro.header.fps = fps;
    }

    // The seed accessor pair is gone with the field: SKC does no RNG locking
    // (it replays inputs on vanilla physics), so a stored seed was never read
    // by anything and only cost 4 header bytes.

    [[nodiscard]] const std::string& getMacroName() const { return m_macroName; }
    void setMacroName(const std::string& name) { m_macroName = name; }

    [[nodiscard]] bool hasInputs() const { return !m_macro.actions.empty(); }

    void setIgnoreInputs(bool ignore) { m_ignoreInputs = ignore; }
    [[nodiscard]] bool getIgnoreInputs() const { return m_ignoreInputs; }

    // True while an SKC session owns the clock. The GUI consults this so its
    // per-frame `engine->tickRate = ...` write does not stomp the macro's TPS,
    // and physicsbypass consults it to keep the step grid engaged.
    [[nodiscard]] bool ownsTiming() const;

    // --- recording & playback ---------------------------------------------

    // Stamp one input event at the given frame. Called from the handleButton
    // hook. Duplicate-consecutive protection is the caller's job: handleButton
    // already fires exactly once per state change.
    void recordInput(uint32_t frame, uint8_t button, bool player2, bool pressed);

    // Return the next event at or before `frame`, advancing the cursor. Called
    // once per physics step from the processCommands hook (the same place the
    // clock advances); a step can carry several events, so the caller loops
    // until it returns nullopt.
    [[nodiscard]] std::optional<Action> poll(uint32_t frame);

    // --- single owner of macro-data deletion --------------------------------
    //
    // SKK has exactly TWO data operations and ONE rule that picks between them
    // (checkpoint_system.cpp:1224, :498-535):
    //   wipe    - the attempt's recorded future is invalid in its entirety
    //   stitch  - cut the stream at a boundary and close the dangling holds
    // and the rule is `!inPractice || tick <= 1 || noRestorePoints`.
    //
    // SKC grew a DIFFERENT rule at every one of its six reset call sites, which
    // is why deleting macro data was the single largest source of corrupt
    // macros: practice respawn truncated but never closed the seam, the level
    // change kept the binding, a checkpoint restore ran the truncation twice,
    // and delayedResetLevel zeroed the clock under a live recording.
    //
    // Every event that can invalidate recorded input now routes through
    // onResetEvent(). Nothing else is allowed to mutate the action stream -
    // not clearInputs(), not the hooks.
    enum class ResetReason {
        AttemptFromStart,   // restart at the level start
        PracticeRespawn,    // respawn from a checkpoint (stitch, not wipe)
        CheckpointRestore,  // player explicitly loaded a checkpoint
        LevelChanged,       // entered a level this session is not bound to
        LevelExit,          // quit the level
        UserClear,          // explicit "Clear" in the GUI
    };

    // `boundaryFrame` is the stitch boundary; ignored for wipe reasons.
    // Returns true if recorded data was dropped.
    bool onResetEvent(ResetReason reason, uint32_t boundaryFrame);

    void restart();

    // Track a hold shadow per (player2, button) and only record REAL transitions.
    //
    // GD does not call handleButton once per physical press. It calls it again
    // whenever an internal path re-asserts the input state - auto-hold, the
    // flush after a checkpoint load, another mod's hold sync - and it happily
    // calls it with the SAME down flag that is already in effect. SKC used to
    // append every single call, so one held button produced a stream of
    // duplicate "clicks" and the pairing turned them into garbage.
    //
    // SKK gates the same way (replay_engine.cpp:2953-2960): keep a shadow of
    // what was really recorded and store an event only when the shadow flips.
    void noteInternalRestore(uint32_t frame);
    void resetHoldShadow();

    // Attempt bookkeeping for recording. SKC has no death chain, so every death
    // restarts an empty timeline - the previous attempt's events are dropped.
    // Without a counter the only trace of that is a macro that quietly plays
    // back the last *failed* try, which looks exactly like a replay bug.
    void noteAttemptReset(uint32_t failedAtFrame);
    [[nodiscard]] uint32_t getAttemptCount() const { return m_attemptCount; }

    // Rewind the playback cursor to the first event at or after `frame`
    // (binary search, same intent as silicate's onReset). Moves an INDEX only -
    // it never deletes recorded data.
    void seekTo(uint32_t frame);

    // Rewind the cursor to the first event at or after `frame` (binary search,
    // same intent as silicate's onReset). Playback only - never deletes.

    // --- persistence -------------------------------------------------------

    [[nodiscard]] bool save(const std::filesystem::path& path);
    [[nodiscard]] bool load(const std::filesystem::path& path);

    // Takes a macro that did not come from a file. Used by the GDR2 importer,
    // which converts another bot's replay into SKC's representation in memory.
    // Passes the same validateActions() gate load() does, so a foreign file
    // cannot get past checks a native .skc would have to pass too. The source
    // level identity is bound so the stale-session guard in onLevelEnter()
    // applies to imports too.
    [[nodiscard]] bool adopt(Macro macro, std::string const& name,
                             uint32_t levelId = 0, std::string levelName = {});

    // Called by the hooks on level exit / full restart so a stale session can
    // never bleed into the next level.
    void onLevelExit();

    // PlayLayer::init hook: a session bound to a DIFFERENT level must not
    // survive into it (SKK's sameLevel check in its own init hook). Returns
    // true when a stale session was halted.
    bool onLevelEnter(GJGameLevel* level);

    // Bind the in-memory macro to a level so onLevelEnter can tell "the same
    // level again" (keep running) from "a different level" (halt).
    void bindLevel(GJGameLevel* level);

    // Hand control back to vanilla GD when a playback macro runs out mid-run,
    // same as SKK's playbackExhaustionNotified path: release the buttons the
    // macro was holding, otherwise the player stays frozen mid-air.
    void onPlaybackExhausted();

private:
    // --- the only code allowed to DELETE recorded data ---
    //
    // Reachable exclusively through onResetEvent(). Making them private is the
    // enforcement mechanism for the single-owner rule: when every reset site
    // invented its own deletion policy, practice respawn truncated without
    // closing the seam, the level change kept the binding, and
    // delayedResetLevel zeroed the clock under a live recording - each one a
    // separate source of unplayable macros.

    // Port of ReplayEngine::prepareCaptureRestore (checkpoint_system.cpp:537).
    // Truncates to the boundary AND closes dangling holds - both halves are
    // required, a truncation alone leaves a press with no release.
    void prepareCaptureRestore(uint32_t frame);
    void closeSeamHolds(uint32_t frame);

    void clearInputs();

    Bot() = default;

    State m_state = State::Disabled;
    PendingAction m_pendingAction = PendingAction::None;
    bool m_playbackArmed = false;
    bool m_ignoreInputs = true;

    // Failed attempts discarded while recording, and the frame the player died
    // at. A completed recording always ends with attempt 1 surviving.
    uint32_t m_attemptCount = 0;
    uint32_t m_lastFailureFrame = 0;
    size_t m_lastTrimmed = 0;
    size_t m_droppedEvents = 0;
    size_t m_duplicateEvents = 0;
    size_t m_suppressedEvents = 0;

    // What we believe the player actually holds, per (player2, button).
    // Index 0 is unused so the button value indexes directly. A recorded event
    // is one where this shadow flips - see noteInternalRestore().
    bool m_holdShadow[2][4] = {};

    // Frames up to and including this one ignore incoming RELEASES. GD flushes
    // buffered presses and stale releases right after a checkpoint restore, and
    // SKK's InputStateRestorer re-pushes the held buttons through the same
    // handleButton. SKC's first version suppressed PRESSES here too and lost
    // the player's real input for 8 frames after every practice respawn -
    // playback diverged exactly at boundary+8. Mirrors SKK's
    // captureReleaseSuppressUntil, which is releases-only by name and by code.
    uint32_t m_suppressUntilFrame = 0;
    bool m_suppressionArmed = false;

    // tickRate before an SKC session claimed it, restored on stop so the menu's
    // own TPS survives a record/play round-trip.
    double m_savedTickRate = 240.0;

    Macro m_macro;
    std::string m_macroName;
    size_t m_inputIndex = 0;

    // Which level the in-memory macro belongs to. 0 / empty means "unknown"
    // (a macro loaded from disk carries no level binding yet), in which case
    // onLevelEnter never treats a session as stale.
    int32_t m_levelId = 0;
    std::string m_levelName;
};

} // namespace skk::skc
