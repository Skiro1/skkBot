#include "skc_bot.hpp"
#include "skc_checkpoint.hpp"
#include "utils.hpp"

#include "core/autosave_recorder.hpp"
#include "skc_frame.hpp"
#include "skkBot.hpp"
#include "gui/skkbot_gui.hpp"
#include "hacks/physicsbypass.hpp"
#include "logger/logger.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <fstream>

using namespace geode::prelude;

namespace skk::skc {

namespace {

// GD game-variable keys for the two accuracy settings SKC forces to vanilla.
// Read from the same strings the rest of the mod uses (skkBot.hpp:1726).
constexpr char const* kCbsKey = "0177";
constexpr char const* kCosKey = "0176";

// The user's own CBS / COS, captured before the first write of a session.
//
// Two separate flags rather than one, because they cover different scopes:
// GameManager's variables are the persisted settings, while the layer fields are
// what GD actually steps this level with, and a layer can appear or disappear
// between the moment of the write and the moment of the restore. Saving a
// layer value that did not exist and restoring it later would invent a setting.
bool g_accuracySaved = false;
bool g_savedCbs = false;
bool g_savedCos = false;
bool g_layerSaved = false;
bool g_savedLayerCbs = false;
bool g_savedLayerCos = false;

// Push the macro's TPS into the one place the whole timing stack reads from.
// physicsbypass derives the step grid from runtimeTickRate(), so this single
// write is all SKC has to do to get arbitrary TPS - no scheduler of its own
// (the first skc module tried that and froze the game).
void applyTps(double tps) {
    if (!std::isfinite(tps) || tps <= 0.0)
        return;
    if (auto* engine = ReplayEngine::get()) {
        engine->tickRate = tps;
    }
}

// The display rate, applied on exactly the same terms as applyTps: the macro
// declares it, it goes live for the session, and endMacro() hands the menu's
// own setting back on every exit path.
//
// The macro has THREE possible things to say and all three are applied:
//   nothing (wire +0.0) -> FPS off for this run, then the menu state back
//   fps == tps (wire -0.0) -> FPS on, follow the tick rate
//   n (wire n)          -> FPS on at n
// Previously only the third engaged, so a macro recorded with FPS off played
// on whatever the menu happened to have, and a macro recorded with fps == tps
// did the same.
void applyDisplayFps(float storedFps) {
    const auto rate =
        skkbot2::fps::MacroDisplayRate::fromWire(static_cast<double>(storedFps));
    if (!skkbot2::fps::beginMacro(rate)) {
        // beginMacro engages for every state now, so this would mean the module
        // rejected its own caller. Log rather than swallow: a session silently
        // running at a display rate nobody asked for is the kind of thing that
        // gets noticed a week later as "the macro feels different today".
        skkbot2::log::warn("[TIMING] macro display rate refused (wire={:.1f})",
                           static_cast<double>(storedFps));
    }
}

} // namespace

void Bot::applySessionAccuracy(bool cbs, bool cos) {
    // Snapshot the user's own CBS / COS the first time this runs in a session,
    // and only then. This is re-asserted on every resetLevel (AGENTS.md 4), so
    // without the guard the "saved" value would be whatever we wrote last time -
    // false - and the restore below would hand back our own leftovers while
    // looking perfectly correct in the log.
    if (!g_accuracySaved) {
        if (auto* gm = GameManager::get()) {
            g_savedCbs = gm->getGameVariable(kCbsKey);
            g_savedCos = gm->getGameVariable(kCosKey);
            g_accuracySaved = true;
        }
        if (auto* layer = GJBaseGameLayer::get()) {
            g_savedLayerCbs = layer->m_clickBetweenSteps;
            g_savedLayerCos = layer->m_clickOnSteps;
            g_layerSaved = true;
        }
    }
    if (auto* gm = GameManager::get()) {
        gm->setGameVariable(kCbsKey, cbs);
        gm->setGameVariable(kCosKey, cos);
    }
    if (auto* layer = GJBaseGameLayer::get()) {
        layer->m_clickBetweenSteps = cbs;
        layer->m_clickOnSteps = cos;
    }
    if (auto* engine = ReplayEngine::get())
        engine->externalAccuracyEnvironmentActive = true;
}

void Bot::reapplySessionAccuracy() const {
    if (m_state == State::Disabled) return;
    // SKC is vanilla-only (see startRecording). Re-asserting vanilla on every
    // reset is the AGENTS.md rule 4 contract: GD re-reads the accuracy
    // settings on reset/practice respawn and would otherwise hand the run back
    // to whatever CBS/COS the user has enabled in the game.
    applySessionAccuracy(false, false);
}

void Bot::restoreUserAccuracy() {
    if (!g_accuracySaved) {
        return;
    }
    if (auto* gm = GameManager::get()) {
        gm->setGameVariable(kCbsKey, g_savedCbs);
        gm->setGameVariable(kCosKey, g_savedCos);
    }
    if (g_layerSaved) {
        if (auto* layer = GJBaseGameLayer::get()) {
            layer->m_clickBetweenSteps = g_savedLayerCbs;
            layer->m_clickOnSteps = g_savedLayerCos;
        }
    }
    // Cleared, not kept: the next session must snapshot GD as it is at THAT
    // moment. If the user changed these options in between, a stale snapshot
    // would overwrite their newer choice with an older one.
    g_accuracySaved = false;
    g_layerSaved = false;
    if (auto* engine = ReplayEngine::get())
        engine->externalAccuracyEnvironmentActive = false;
    skkbot2::log::info(
        "[SKC] GD's own accuracy settings restored: CBS={} COS={}", g_savedCbs,
        g_savedCos);
}

bool Bot::ownsTiming() const {
    return m_state != State::Disabled;
}

Bot& Bot::get() {
    static Bot instance;
    return instance;
}

bool Bot::platformerModeMismatch() const {
    // No declared mode = nothing to compare. Old .skc files, and formats that
    // do not carry a mode bit, stay playable on either kind of level.
    if (!m_macro.platformerKnown) return false;
    auto* playLayer = PlayLayer::get();
    if (!playLayer || !playLayer->m_levelSettings) return false;
    return m_macro.platformerMode != playLayer->m_levelSettings->m_platformerMode;
}

void Bot::startRecording() {
    double captureTps = m_macro.header.tps;
    if (m_state == State::Disabled) {
        if (auto* engine = ReplayEngine::get()) {
            m_savedTickRate = engine->tickRate;
            captureTps = engine->runtimeTickRate();
        }
    } else if (auto* engine = ReplayEngine::get()) {
        captureTps = engine->runtimeTickRate();
    }
    // SKC is VANILLA-ONLY. CBS/COS support was removed at the user's request
    // (01.11): a real record→play round trip with them on died every single
    // time, and the sub-step marks never survived it. The game's own switches
    // are forced OFF for the session (restoreUserAccuracy gives the user's
    // setting back when the session ends) and nothing about them is written to
    // the file.
    m_macro.cbsMode = false;
    m_macro.cosMode = false;
    m_macro.subtickEncoded = false;
    m_macro.header.cbsMode = false;
    m_macro.header.cosMode = false;
    m_macro.header.subtickEncoded = false;
    auto& gui = skkbot2::gui::get();
    m_macro.lockDeltaMode = gui.lockDelta && !gui.realTime;
    m_macro.realTimeMode = gui.realTime && !gui.lockDelta;
    m_macro.header.lockDeltaMode = m_macro.lockDeltaMode;
    m_macro.header.realTimeMode = m_macro.realTimeMode;
    applySessionAccuracy(false, false);
    const uint32_t startFrame = FrameClock::get().getFrame();
    m_state = State::Record;
    m_pendingAction = PendingAction::None;
    m_playbackArmed = false;
    m_attemptCount = 0;
    m_lastFailureFrame = 0;
    m_deathNoted = false;
    m_droppedEvents = 0;
    m_duplicateEvents = 0;
    m_suppressedEvents = 0;
    resetHoldShadow();
    m_suppressionArmed = false;
    // Bind to the level being RECORDED on. Only the init hook used to do this,
    // and only for a queued action started from outside the level - so a record
    // started from the in-game GUI kept whatever level was bound before, and
    // the next visit to that level treated this macro as belonging to it.
    if (auto* layer = PlayLayer::get()) {
        bindLevel(layer->m_level);
        // Record the mode this macro belongs to. SKK does the same in
        // initializeSkMacro (skkBot.hpp:944) and refuses a mismatched playback;
        // SKC had no field at all, so a platformer macro would happily arm on a
        // classic level and desync at the first left/right.
        if (layer->m_levelSettings) {
            m_macro.platformerMode = layer->m_levelSettings->m_platformerMode;
            m_macro.platformerKnown = true;
            // Keep the header view in step with the macro view: the GUI and
            // readHeader() show the header, encode() reads the macro.
            m_macro.header.platformerMode = m_macro.platformerMode;
            m_macro.header.platformerKnown = true;
        }
    }
    // The recording must inherit the live TPS selected by the user. Applying
    // the macro's default header here used to overwrite a live 360.75 TPS with
    // 240 before the first input, then serialize that accidental 240 into the
    // file. Playback correctly trusts the stored rate, so the bad capture
    // looked like a playback-only TPS regression.
    m_macro.header.tps = captureTps;
    m_macro.header.fps = 0.0f;
    applyTps(captureTps);
    // What the menu is running right now, which is the only thing worth
    // writing down: the rate the clicks were actually captured under. The
    // result is one of three wire states - see MacroDisplayRate:
    //   not stored  FPS was off while recording -> playback leaves it off
    //   follow tps  FPS on and equal to tps     -> stored as -0.0
    //   pinned n    FPS on at n != tps          -> stored as n
    //
    // Recorded, not applied. During a recording the menu's setting IS the rate
    // the clicks are being captured under, so pushing it back would be circular.
    // The apply belongs on the playback side, where the number arrives from the
    // file instead of from the menu.
    const auto capturedRate = skkbot2::fps::captureForStorage(
        static_cast<double>(m_macro.header.tps));
    m_macro.header.fps = static_cast<float>(capturedRate.toWire());
    restart();
    skkbot2::log::info(
        "[SKC] recording started (tps={}, fps={} ({:.0f}), mode={}, "
        "startFrame={})",
        m_macro.header.tps,
        !capturedRate.present() ? "not stored"
                                : (capturedRate.followsTps() ? "follow tps" : "pinned"),
        capturedRate.fps,
        !m_macro.platformerKnown ? "unknown"
                                 : (m_macro.platformerMode ? "platformer" : "classic"),
        startFrame);
    // SKK records the player's start POSITION and teleports there on playback,
    // so a mid-level recording is fine there. SKC has no position in the file,
    // so the only thing that makes a mid-level recording replayable is the
    // player re-running the same stretch from the spawn. Flag it loudly.
    if (startFrame > 240) {
        skkbot2::log::warn(
            "[SKC] recording started {} frames into the level: playback can only "
            "reproduce it if the player re-runs the same stretch from the spawn "
            "(SKC stores no start position)", startFrame);
    }
}

void Bot::startPlayback() {
    if (platformerModeMismatch()) {
        auto* layer = PlayLayer::get();
        const bool levelPlatformer = layer && layer->m_levelSettings &&
                                     layer->m_levelSettings->m_platformerMode;
        skkbot2::log::error(
            "[SKC] playback refused: the macro was recorded in {} mode, but this "
            "level is {} mode. Open a level of the matching mode.",
            m_macro.platformerMode ? "platformer" : "classic",
            levelPlatformer ? "platformer" : "classic");
        // Do not leave an armed macro pointing at a level it can never play.
        m_playbackArmed = false;
        m_pendingAction = PendingAction::None;
        return;
    }
    if (m_state == State::Disabled) {
        if (auto* engine = ReplayEngine::get()) {
            m_savedTickRate = engine->tickRate;
        }
    }
    // Vanilla-only, like the recording side: whatever the file says about
    // CBS/COS is ignored (the decoder already normalized it away).
    applySessionAccuracy(false, false);
    auto& gui = skkbot2::gui::get();
    gui.lockDelta = m_macro.header.lockDeltaMode;
    gui.realTime = m_macro.header.realTimeMode;
    dropCarry();
    m_state = State::Playback;
    m_pendingAction = PendingAction::None;
    m_playbackArmed = false;
    applyTps(m_macro.header.tps);
    applyDisplayFps(m_macro.header.fps);
    restart();
    const auto declaredRate =
        skkbot2::fps::MacroDisplayRate::fromWire(static_cast<double>(m_macro.header.fps));
    skkbot2::log::info(
        "[SKC] playback started: inputs={} firstFrame={} lastFrame={} tps={} "
        "fps={} ({:.0f}) mode={} timing={}",
        m_macro.actions.size(),
        m_macro.actions.empty() ? 0u : m_macro.actions.front().frame,
        m_macro.actions.empty() ? 0u : m_macro.actions.back().frame,
        m_macro.header.tps,
        !declaredRate.present()
            ? "not stored"
            : (declaredRate.followsTps() ? "follow tps" : "pinned"),
        declaredRate.fps,
        !m_macro.platformerKnown
            ? "unknown"
            : (m_macro.platformerMode ? "platformer" : "classic"),
        m_macro.header.realTimeMode ? "Real Time" : "Lock Delta");
    // The mode question, answered from the level that is actually open. Foreign
    // imports carry no mode bit, so the only honest statement is "what is in the
    // file vs what this level is" - and a macro full of left/right actions in a
    // classic level is the exact situation that produced a silent "PLAY does
    // nothing" report twice (Windy Road.slc): the actions are dispatched, GD
    // just has no use for buttons 2/3 outside platformer mode, so the run looks
    // like an unrelated death a few seconds in.
    if (auto* layer = PlayLayer::get()) {
        const bool levelPlatformer =
            layer->m_levelSettings && layer->m_levelSettings->m_platformerMode;
        // Which level is actually open, and which level (if any) the macro was
        // bound to. An imported file carries no level identity, so "dies at
        // 1.5 s" is only explainable once both sides are on the record: a macro
        // replayed in the wrong level dies exactly like a broken replay.
        if (auto* level = layer->m_level) {
            skkbot2::log::info(
                "[SKC] playback level: '{}' (id {}) platformer={} twoPlayer={} "
                "dual={} length={}; macro bound to '{}' (id {})",
                level->m_levelName.c_str(),
                static_cast<long long>(level->m_levelID), levelPlatformer,
                (layer->m_levelSettings &&
                 layer->m_levelSettings->m_twoPlayerMode) ? 1 : 0,
                layer->m_gameState.m_isDualMode ? 1 : 0,
                level->m_levelLength,
                m_levelName.empty() ? "<none>" : m_levelName.c_str(),
                static_cast<long long>(m_levelId));
        }
        size_t sideActions = 0;
        for (const auto& action : m_macro.actions) {
            if (action.button == 2 || action.button == 3) ++sideActions;
        }
        if (sideActions > 0 && !levelPlatformer) {
            skkbot2::log::info(
                "[SKC] note: this level is CLASSIC and the macro carries {} "
                "left/right action(s) - GD only uses those in platformer mode, "
                "so they are dispatched with no effect ({} jump actions are "
                "what plays)",
                sideActions, m_macro.actions.size() - sideActions);
        }
    }

    // The opening of the file, verbatim. When a playback dies before the log's
    // "state" lines even reach the second authored click, these are the only
    // frames the run ever had a chance to act on.
    for (size_t i = 0; i < std::min<size_t>(m_macro.actions.size(), 16); ++i) {
        const auto& action = m_macro.actions[i];
        skkbot2::log::info(
            "[SKC] macro input#{}: frame={} btn={} down={} p2={}", i,
            action.frame, action.button, action.pressed ? 1 : 0,
            action.player2 ? 1 : 0);
    }
}

void Bot::armPlayback() {
    // Only a loaded macro can be armed.
    if (m_macro.actions.empty()) {
        skkbot2::log::warn("[SKC] PLAY armed refused: no inputs loaded");
        return;
    }
    if (platformerModeMismatch()) {
        auto* layer = PlayLayer::get();
        const bool levelPlatformer = layer && layer->m_levelSettings &&
                                     layer->m_levelSettings->m_platformerMode;
        skkbot2::log::error(
            "[SKC] PLAY armed refused: the macro was recorded in {} mode, but "
            "this level is {} mode.",
            m_macro.platformerMode ? "platformer" : "classic",
            levelPlatformer ? "platformer" : "classic");
        return;
    }
    m_pendingAction = PendingAction::None;
    m_playbackArmed = true;
    skkbot2::log::info("[SKC] PLAY armed: takes over on the next attempt (no level restart)");
}

Bot::PlaybackStart Bot::playbackStartCheck() const {
    PlaybackStart out;
    out.clockFrame = FrameClock::get().getFrame();
    out.firstActionFrame = m_macro.actions.empty() ? 0u : m_macro.actions.front().frame;

    if (m_macro.actions.empty()) {
        out.decision = StartDecision::NoInputs;
        out.reason = "no macro loaded";
        return out;
    }
    if (isEnabled() || m_playbackArmed) {
        out.decision = StartDecision::Running;
        out.reason = "already recording or playing";
        return out;
    }

    auto* playLayer = PlayLayer::get();
    if (!playLayer) {
        out.decision = StartDecision::NoLevel;
        out.reason = "not inside a level";
        return out;
    }
    if (playLayer->m_levelEndAnimationStarted) {
        out.decision = StartDecision::Ended;
        out.reason = "level is finished";
        return out;
    }

    // Wrong game mode is not a timing problem: no attempt, however fresh, can
    // make a platformer macro play on a classic level or the other way round.
    // Checked before the clock so the answer does not depend on where we are.
    if (platformerModeMismatch()) {
        out.decision = StartDecision::WrongMode;
        out.reason = "the macro and the level are in different game modes";
        return out;
    }

    // Not started yet - the "tap to play" screen or the level fade-in. Nothing
    // has run, so the world is at frame 0 whatever the clock happens to say, and
    // this is checked BEFORE the clock. The tap screen is the single most common
    // place to press PLAY, and refusing there because some scheduler ticked
    // twice would reproduce exactly the bug this decision exists to remove.
    if (!playLayer->m_started) {
        out.decision = StartDecision::Ready;
        out.reason = "";
        return out;
    }

    // THE check that matters. FrameClock is SKC's own counter of physics steps,
    // reset by resetLevel and by editor playtest entry, monotone within an
    // attempt (see skc_frame.hpp). Zero means the attempt has not advanced - the
    // player is at the spawn and the macro's frame numbering lines up with the
    // world. Anything else and the clicks land on a player the macro never
    // positioned.
    if (FrameClock::get().getFrame() != 0) {
        out.decision = StartDecision::MidAttempt;
        out.reason = "the attempt is already under way";
        return out;
    }

    out.decision = StartDecision::Ready;
    out.reason = "";
    return out;
}

bool Bot::startPlaybackIfReady(PlaybackStart* out) {
    const PlaybackStart start = playbackStartCheck();
    if (out) {
        *out = start;
    }
    if (start.decision != StartDecision::Ready) {
        return false;
    }
    startPlayback();
    return true;
}

std::string Bot::describeStartBlock(const PlaybackStart& start) const {
    switch (start.decision) {
        case StartDecision::MidAttempt:
            // The numbers, not an adjective. "The attempt is under way" tells the
            // operator nothing they can act on; "you are at frame 3180, the
            // macro starts at frame 0" tells them exactly what to do about it.
            return fmt::format(
                "SKC: cannot start here - attempt is at frame {}, and the macro "
                "reproduces only from frame 0. Press PLAY right after the level "
                "starts, or restart the level.",
                start.clockFrame);
        case StartDecision::NoInputs:
            return "SKC: no macro loaded";
        case StartDecision::Running:
            return "SKC: already recording or playing";
        case StartDecision::Ended:
            return "SKC: the level is finished";
        case StartDecision::WrongMode:
            return "SKC: this macro was recorded in a different game mode - "
                   "open a classic level for a classic macro, or a platformer "
                   "level for a platformer macro";
        case StartDecision::NoLevel:
            return "SKC: not inside a level";
        case StartDecision::Ready:
        default:
            return "";
    }
}

void Bot::stop() {
    const bool wasHolding = m_state != State::Disabled;
    // Autosave contract (02.11, mirrors haltExecution): a rescue snapshot at
    // every record-off moment, so finishing a practice-stitched line via the
    // hotkey / leaving the level / a macro swap still ends with the file on
    // disk. flushRecording() gates itself: SKC branch fires only while the
    // bot still reports Record and drops zero-input captures.
    if (m_state == State::Record) {
        skkbot2::autosave::flushRecording();
    }
    m_state = State::Disabled;
    m_pendingAction = PendingAction::None;
    m_playbackArmed = false;
    // Same as SKK's haltExecution: a pinned display rate does not outlive
    // the session that pinned it, and SKC reaches its stop through several
    // paths (GUI, macro swap, editor exit, level leave).
    skkbot2::fps::endMacro();
    // And neither does the forced vanilla accuracy: GD persists these two keys
    // into the user's save file, so leaving them at false after a session is an
    // edit to their settings that nobody asked for.
    restoreUserAccuracy();
    if (auto* engine = ReplayEngine::get()) {
        engine->tickRate = m_savedTickRate;
    }
    // SKK's haltExecution releases whatever the macro was holding; without
    // this a stop mid-hold leaves the player pressing jump forever.
    if (wasHolding) {
        if (auto* playLayer = PlayLayer::get()) {
            if (playLayer->m_player1) playLayer->m_player1->releaseAllButtons();
            if (playLayer->m_player2) playLayer->m_player2->releaseAllButtons();
        }
    }
    skkbot2::log::info("[SKC] stopped");
    // SKC runs ticks through the same step-grid/lock-delta machinery as SKK,
    // so the session's plan/step audit is printed here too - the numbers were
    // previously lost on every stop that did not pass through SKK's
    // haltExecution (BUG-69 investigation had no epilogue to read).
    if (auto* engine = ReplayEngine::get()) {
        engine->logTimingEpilogues();
    }
    skkbot2::dumpPlayerInputTally("SKC run ended");
}

bool Bot::consumePendingAction(GJGameLevel* level) {
    if (m_pendingAction == PendingAction::None || !level)
        return false;

    const PendingAction action = m_pendingAction;
    m_pendingAction = PendingAction::None;

    if (action == PendingAction::Record) {
        skkbot2::log::info("[SKC] consuming queued RECORD on level entry");
        // This hook runs before PlayLayer::init(), so the layer's
        // m_levelSettings is not necessarily built yet. The GJGameLevel knows
        // the mode itself, and startRecording() only overwrites it when the
        // layer's settings are already available (with the same answer).
        m_macro.platformerMode = level->isPlatformer();
        m_macro.platformerKnown = true;
        m_macro.header.platformerMode = m_macro.platformerMode;
        m_macro.header.platformerKnown = true;
        startRecording();
        return true;
    }

    if (m_macro.actions.empty()) {
        skkbot2::log::warn("[SKC] queued PLAY skipped: no inputs loaded");
        return false;
    }
    // Same pre-init problem as RECORD: platformerModeMismatch() needs the live
    // layer's LevelSettingsObject, which PlayLayer::init() has not created yet.
    // Check the mode from the GJGameLevel that was handed to us instead.
    if (m_macro.platformerKnown &&
        m_macro.platformerMode != level->isPlatformer()) {
        skkbot2::log::error(
            "[SKC] queued PLAY refused: the macro was recorded in {} mode, but "
            "this level is {} mode.",
            m_macro.platformerMode ? "platformer" : "classic",
            level->isPlatformer() ? "platformer" : "classic");
        return false;
    }
    // Which level the macro is actually being played in. Imported macros carry
    // no level identity, so a replay started in the wrong level differs from a
    // broken replay only by this line.
    skkbot2::log::info(
        "[SKC] playing '{}' (id {}) platformer={} twoPlayer={} length={} "
        "difficulty={} downloads={}",
        level->m_levelName.c_str(), static_cast<long long>(level->m_levelID),
        level->isPlatformer() ? 1 : 0, level->m_twoPlayerMode ? 1 : 0,
        level->m_levelLength, static_cast<int>(level->m_difficulty),
        static_cast<long long>(level->m_downloads));
    skkbot2::log::info("[SKC] consuming queued PLAY on level entry");
    startPlayback();
    return true;
}

void Bot::bindLevel(GJGameLevel* level) {
    if (!level) return;
    m_levelId = level->m_levelID;
    m_levelName = level->m_levelName;
}

bool Bot::onLevelEnter(GJGameLevel* level) {
    if (!isEnabled() && !m_playbackArmed)
        return false;
    // An unbound macro (loaded from disk) has no level to compare against, so
    // a session on it is never stale.
    if (m_levelId == 0 && m_levelName.empty())
        return false;
    if (!level)
        return false;

// GDR2 level names come from other bots' own metadata and casing does not
    // match GD's ("rauchkammer 2" vs "Rauchkammer 2"), so the name comparison
    // is case-insensitive. An ID match is still preferred when both sides have
    // one - two different levels sharing a name is the only thing this relaxes.
    bool sameName = false;
    if (m_levelName.size() == level->m_levelName.size() && !m_levelName.empty()) {
        sameName = true;
        for (size_t i = 0; i < m_levelName.size(); ++i) {
            const auto a = static_cast<unsigned char>(m_levelName[i]);
            const auto b = static_cast<unsigned char>(level->m_levelName[i]);
            if (std::tolower(a) != std::tolower(b)) {
                sameName = false;
                break;
            }
        }
    }

    const bool sameLevel = (m_levelId > 0 && level->m_levelID > 0)
        ? (m_levelId == level->m_levelID)
        : sameName;

    if (sameLevel)
        return false;

    skkbot2::log::warn("[SKC] entered a different level - halting stale session "
        "(macro level {}/{}, now {}/{})",
        m_levelId, m_levelName, level->m_levelID, level->m_levelName);
    // A macro recorded on another level can never be valid here: SKC stores no
    // physics and no start position, so the same inputs on a different layout
    // die at whatever obstacle they happen to meet. stop() alone left both the
    // data AND the binding in place, so a later recording on this level kept
    // the OLD level's identity and re-entering that level treated the stale
    // macro as valid.
    onResetEvent(ResetReason::LevelChanged, 0);
    m_levelId = 0;
    m_levelName.clear();
    stop();
    return true;
}

void Bot::onPlaybackExhausted() {
    if (m_playbackExhaustionNotified) return;
    m_playbackExhaustionNotified = true;
    skkbot2::log::info("[SKC] all inputs dispatched - maintaining timing until level end");
    if (auto* playLayer = PlayLayer::get()) {
        if (playLayer->m_player1) playLayer->m_player1->releaseAllButtons();
        if (playLayer->m_player2) playLayer->m_player2->releaseAllButtons();
    }
}

void Bot::recordInput(uint32_t frame, uint8_t button, bool player2,
                      bool pressed) {
    // The death hook can run before GD flips m_isDead. Keep the latch as an
    // independent gate so callbacks later in that same transition are dropped.
    if (m_deathNoted) {
        ++m_suppressedEvents;
        if (m_suppressedEvents <= 5) {
            skkbot2::log::debug(
                "[SKC] drop [death-window] frame={} button={} p2={} down={}",
                frame, button, player2, pressed);
        }
        return;
    }

    // Inputs received during the death animation are not part of a clean line.
    // Some platformer paths keep calling handleButton after destroyPlayer and
    // before resetLevel; without this final gate those events survive into SKC.
    if (auto* layer = PlayLayer::get()) {
        if ((layer->m_player1 && layer->m_player1->m_isDead) ||
            (layer->m_player2 && layer->m_player2->m_isDead)) {
            ++m_suppressedEvents;
            if (m_suppressedEvents <= 5) {
                skkbot2::log::debug(
                    "[SKC] drop [dead-player] frame={} button={} p2={} down={} "
                    "(death animation is not recorded)",
                    frame, button, player2, pressed);
            }
            return;
        }
    }

    // Events arrive in frame order by construction (handleButton fires during
    // the physics step the clock just counted), so append is enough. An
    // out-of-order event would corrupt the delta stream, so guard anyway - but
    // loudly: a silent drop here is how a practice session turns into a macro
    // whose presses have no matching releases.
    const bool outOfOrder = !m_macro.actions.empty() &&
        frame < m_macro.actions.back().frame;
    if (outOfOrder) {
        ++m_droppedEvents;
        if (m_droppedEvents <= 5) {
            skkbot2::log::warn(
                "[SKC] dropped out-of-order input: frame {} < last recorded {} "
                "(dropped {} so far) - the frame clock moved backwards, which "
                "usually means a practice checkpoint respawn mid-hold",
                frame, m_macro.actions.back().frame, m_droppedEvents);
        } else if (m_droppedEvents == 6) {
            skkbot2::log::warn("[SKC] further out-of-order drops suppressed");
        }
        return;
    }

    // --- restore window: RELEASES ONLY (SKK: captureReleaseSuppressUntil) ---
    // After a checkpoint restore GD flushes stale buffered presses AND the
    // stale physical releases. SKK keeps the releases out of the macro stream
    // (replay_engine.cpp:2932, `if (!hold && ...)`) but deliberately lets
    // PRESSES through, and its variable name says so:
    // captureReleaseSuppressUntil.
    //
    // SKC suppressed BOTH. That silently ate the player's real presses for
    // 8 frames after every practice respawn, so the macro was missing input the
    // recording contained and playback diverged exactly at
    // boundary + 8 - measured at frame 3360 against a stitch at 3352.
    if (!pressed && m_suppressionArmed && frame >= m_suppressFromFrame &&
        frame <= m_suppressUntilFrame) {
        ++m_suppressedEvents;
        if (m_suppressedEvents <= 5) {
            skkbot2::log::debug(
                "[SKC] drop [restore-window] frame={} button={} p2={} "
                "(post-restore release, not recorded)",
                frame, button, player2);
        }
        return;
    }

    // --- real transitions only (SKK: recordedHoldShadow) ---
    // GD re-asserts the same down flag from internal paths; without the shadow
    // one held button became a stream of duplicate presses and the pairing
    // produced a macro full of orphans.
    if (button >= 1 && button <= 3) {
        bool& held = m_holdShadow[player2 ? 1 : 0][button];
        if (held == pressed) {
            ++m_duplicateEvents;
            if (m_duplicateEvents <= 5) {
                skkbot2::log::debug(
                    "[SKC] drop [no-transition] frame={} button={} p2={} "
                    "down={} (already {})", frame, button, player2, pressed,
                    pressed ? "down" : "up");
            }
            return;
        }
        held = pressed;
    }

    // Fields assigned by NAME, not by aggregate position: Action is
    // {frame, button, pressed, player2} while this signature is
    // {frame, button, player2, pressed}. An aggregate emplace_back swapped the
    // two bools, which stored every press as a release - the macro then replayed
    // only "release button 1" and the bot appeared to never click.
    Action action;
    action.frame = frame;
    action.button = button;
    action.pressed = pressed;
    action.player2 = player2;
    // SKC is vanilla-only: there is no sub-step position any more, so the mark
    // is always false. The field stays in the format so old files still decode.
    action.halfTick = false;
    m_macro.actions.push_back(action);
    skkbot2::log::debug(
        "[SKC] record frame={} button={} p2={} down={} (action {}/{})",
        frame, button, player2, pressed, m_macro.actions.size(),
        m_duplicateEvents + m_suppressedEvents + m_droppedEvents);
}

void Bot::syncHoldShadowToPlayer() {
    // BUG-103: the hold shadow must match the player's REAL button state at a
    // checkpoint restore. BUG-99 removed hold restore from the state bundle,
    // so the player comes back holding nothing - but `resetHoldShadow()` also
    // zeroed the shadow while the PLAYER could still legitimately be holding a
    // button (GD keeps m_holdingButtons across loadFromCheckpoint). A genuine
    // release inside the old suppression window then hit shadow=false and was
    // "dropped", leaving shadow out of sync; the next real press hit the
    // no-transition gate and was eaten too. Two events lost, macro diverges.
    // Measured 13:35 session: rec 306 events -> saved 304, press@1681 +
    // press@1695 gone after stitch at 1673.
    for (auto& slot : m_holdShadow) {
        for (bool& held : slot) {
            held = false;
        }
    }
    if (auto* playLayer = PlayLayer::get()) {
        for (int slot = 0; slot < 2; ++slot) {
            auto* player = slot == 0 ? playLayer->m_player1
                                     : playLayer->m_player2;
            if (!player) continue;
            for (int button = 1; button <= 3; ++button) {
                auto it = player->m_holdingButtons.find(button);
                m_holdShadow[slot][button] =
                    it != player->m_holdingButtons.end() && it->second;
            }
        }
    }
    skkbot2::log::debug(
        "[SKC] hold shadow synced to player at restore "
        "(p1={},{},{} p2={},{},{})",
        m_holdShadow[0][1], m_holdShadow[0][2], m_holdShadow[0][3],
        m_holdShadow[1][1], m_holdShadow[1][2], m_holdShadow[1][3]);
}

void Bot::noteInternalRestore(uint32_t frame) {
    // BUG-103: the 8-frame release-suppression window is GONE. It was meant to
    // filter GD's stale flush after a restore, but it also swallowed the
    // player's GENUINE release when a tap ended inside the window (rec#7:
    // stitch at 1673, tap release at 1681 dropped, shadow stayed DOWN, the
    // next press at 1695 was then eaten by the no-transition gate).
    // With the shadow synced to the player (syncHoldShadowToPlayer) the stale
    // flush is filtered correctly by the no-transition gate instead: a stale
    // release hits shadow==false and is dropped as a no-transition, while
    // genuine press/release transitions keep flowing into the macro.
    m_suppressionArmed = false;
    syncHoldShadowToPlayer();
    (void)frame;
}

void Bot::resetHoldShadow() {
    for (auto& slot : m_holdShadow) {
        for (bool& held : slot) {
            held = false;
        }
    }
}

std::optional<Action> Bot::poll(uint32_t frame) {
    if (m_inputIndex >= m_macro.actions.size())
        return std::nullopt;

    // `<=` not `==`: a physics step can legitimately span several recorded
    // events (two presses on the same tick, or the clock catching up after a
    // batched step), and dropping the trailing ones would silently shift the
    // whole rest of the macro.
    const Action& next = m_macro.actions[m_inputIndex];
    // SKC is vanilla-only, so an action belongs to its frame and nothing else.
    // The old half-tick gate (CBS) is gone: it made a marked action wait for a
    // half step that often never came on that frame, slipping the click one
    // physics step and desyncing the run.
    if (next.frame <= frame) {
        return m_macro.actions[m_inputIndex++];
    }

    return std::nullopt;
}

void Bot::clearInputs() {
    m_macro.actions.clear();
    m_inputIndex = 0;
}

Bot::HeldState Bot::heldBefore(uint32_t frame) const {
    HeldState state;
    // Append order is frame order by construction (recordInput rejects a
    // backwards stamp, the codec sorts on load), so the first action at or
    // after `frame` ends the fold.
    for (const Action& action : m_macro.actions) {
        if (action.frame >= frame) break;
        const int button = static_cast<int>(action.button) - 1;
        if (button < 0 || button > 2) continue;
        state.held[action.player2 ? 1 : 0][button] = action.pressed;
    }
    return state;
}

Bot::SeamQueueReport Bot::sanitizeSeamQueue(
    gd::vector<PlayerButtonCommand>& queue, uint32_t resumeFrame,
    SeamQueueMode mode, const char* site) {
    SeamQueueReport report;
    report.inspected = queue.size();
    if (queue.empty()) return report;

    const auto buttonLabel = [](int button) -> const char* {
        switch (button) {
            case 1: return "jump";
            case 2: return "left";
            case 3: return "right";
            default: return "?";
        }
    };

    // Compact digest of what is about to be judged. The command carries no
    // frame of its own (only the step it was queued in), so the order in the
    // queue is the only time information there is.
    std::string digest;
    for (size_t i = 0; i < queue.size() && i < 6; ++i) {
        if (i) digest += ' ';
        const PlayerButtonCommand& cmd = queue[i];
        digest += '[';
        digest += buttonLabel(static_cast<int>(cmd.m_button));
        digest += cmd.m_isPush ? " down" : " up";
        digest += cmd.m_isPlayer2 ? " p2]" : " p1]";
    }
    if (queue.size() > 6) digest += " ...";

    const HeldState stream = heldBefore(resumeFrame);
    // "The stream has not pressed anything yet" (rule 3) is about the whole
    // prefix, not one channel: an action at or after the resume frame belongs
    // to the retry, not to the attempt being stitched into.
    const bool anyActionBefore =
        !m_macro.actions.empty() && m_macro.actions.front().frame < resumeFrame;

    if (mode == SeamQueueMode::Record) {
        // Every command still queued here was queued during the attempt that
        // just ended: the step it was made in already drained the queue before
        // the physics that killed the player. Nothing left in it can belong to
        // the attempt being stitched into, so the whole queue goes - Xenon only
        // inspects the tail because its recorder reads the queue itself, while
        // SKC records from handleButton and would bake a phantom press into the
        // stitched suffix.
        report.rule = "record seam (all queued commands belong to the dead attempt)";
        for (const PlayerButtonCommand& cmd : queue) {
            const int button = static_cast<int>(cmd.m_button);
            if (button < 1 || button > 3) continue;
            const int slot = cmd.m_isPlayer2 ? 1 : 0;
            if (!cmd.m_isPush && !stream.held[slot][button - 1]) {
                // Same distinction Xenon draws, kept for the log even though
                // the disposition is identical in this mode.
                if (anyActionBefore) ++report.extraRelease;
                else ++report.ghostRelease;
            } else if (cmd.m_isPush && stream.held[slot][button - 1]) {
                ++report.extraPush;
            }
        }
        report.cleared = queue.size();
        report.clearedQueue = true;
        queue.clear();
    } else {
        // Play: Xenon's three rules, evaluated on the TAIL only. A command no
        // rule names - a real press during the death on a channel the stream is
        // not holding - is left in place, exactly like Xenon does.
        const PlayerButtonCommand& last = queue.back();
        const int button = static_cast<int>(last.m_button);
        if (button >= 1 && button <= 3) {
            const int slot = last.m_isPlayer2 ? 1 : 0;
            const bool held = stream.held[slot][button - 1];
            if (!last.m_isPush && !held) {
                if (anyActionBefore) {
                    ++report.extraRelease;
                    report.rule = "rule 1: release while the stream is already released";
                } else {
                    ++report.ghostRelease;
                    report.rule =
                        "rule 3: ghost release - the stream has not pressed yet";
                }
            } else if (last.m_isPush && held) {
                ++report.extraPush;
                report.rule =
                    "rule 2: press while the stream is still holding";
            }
        }

        if (report.rule) {
            report.cleared = queue.size();
            report.clearedQueue = true;
            queue.clear();
        }
    }

    if (report.clearedQueue) {
        skkbot2::log::info(
            "[SKC] seam-queue[{} site={} f={} ignoreInputs={}]: dropped {} of {} "
            "queued command(s) - {}: {}",
            mode == SeamQueueMode::Record ? "record" : "play", site, resumeFrame,
            m_ignoreInputs ? 1 : 0, report.cleared, report.inspected,
            report.rule ? report.rule : "tail rule", digest.c_str());
    } else {
        // Nothing fired: say so, because "the seam saw a queued press and kept
        // it" is the one case where a respawn-frame click is intended.
        skkbot2::log::info(
            "[SKC] seam-queue[{} site={} f={} ignoreInputs={}]: kept {} queued "
            "command(s) - tail does not contradict the stream: {}",
            mode == SeamQueueMode::Record ? "record" : "play", site, resumeFrame,
            m_ignoreInputs ? 1 : 0, report.inspected, digest.c_str());
    }
    return report;
}

bool Bot::onResetEvent(ResetReason reason, uint32_t boundaryFrame) {
    // The SKK wipe rule, checkpoint_system.cpp:1622-1647. SKC used to evaluate
    // a DIFFERENT rule at each of its six reset sites; this is now the only
    // place that decides whether recorded data survives.
    //
    // Wipe when: not a practice respawn, OR the attempt starts at the level
    // start, OR there is nothing to restore to. Otherwise stitch - cut the
    // dying attempt's tail at the boundary and close the dangling holds.
    const bool stitch = (reason == ResetReason::PracticeRespawn ||
                         reason == ResetReason::CheckpointRestore);
    const char* name = "unknown";
    switch (reason) {
        case ResetReason::AttemptFromStart:  name = "AttemptFromStart";  break;
        case ResetReason::PracticeRespawn:   name = "PracticeRespawn";   break;
        case ResetReason::CheckpointRestore: name = "CheckpointRestore"; break;
        case ResetReason::LevelChanged:      name = "LevelChanged";      break;
        case ResetReason::LevelExit:         name = "LevelExit";         break;
        case ResetReason::UserClear:         name = "UserClear";         break;
    }

    if (!stitch) {
        const size_t dropped = m_macro.actions.size();
        clearInputs();
        restart();
        resetHoldShadow();
        // The attempt clock is about to rewind to 0, so a restore window armed
        // on the dying attempt must not survive into the new one. The range
        // check in recordInput is the second line of defence; this is the
        // first, because "armed" with a stale absolute frame is meaningless
        // once the frame numbering starts over.
        m_suppressionArmed = false;
        m_suppressFromFrame = 0;
        m_suppressUntilFrame = 0;
        m_attemptCount = 0;
        m_droppedEvents = 0;
        m_duplicateEvents = 0;
        m_suppressedEvents = 0;
        skkbot2::log::info(
            "[SKC] reset[{}] WIPE - dropped all {} recorded input(s)", name,
            dropped);
        return dropped > 0;
    }

    // Stitch: keep [0 .. boundary-1] from the attempt that just died and let
    // the retry append from `boundary` on. Never wipe here - the prefix is
    // exactly the part the player proved they can survive.
    const size_t before = m_macro.actions.size();
    prepareCaptureRestore(boundaryFrame);
    // No frame correction. With the vanilla restore the state is EXACT - the
    // player comes back byte-identical to a run that never died - so the retry
    // simply continues on the same frame grid as the run it is stitched into.
    // The earlier `-1` compensated for SKK's state bundle holding a frame's
    // worth of extra movement; with that bundle gone the correction is a plain
    // one-frame error (measured: dx = -1 frame, dead constant).
    closeSeamHolds(boundaryFrame);
    // GD flushes buffered presses and stale releases right after the restore;
    // keep those out of the macro stream. BUG-103: noteInternalRestore now
    // syncs the hold shadow to the player instead of arming a suppression
    // window + zeroing the shadow (which ate a genuine tap pair after a
    // practice-restore stitch).
    noteInternalRestore(boundaryFrame);
    skkbot2::log::info(
        "[SKC] reset[{}] STITCH at frame {} - {} of {} input(s) kept",
        name, boundaryFrame, m_macro.actions.size(), before);
    return before != m_macro.actions.size();
}

void Bot::restart() {
    m_inputIndex = 0;
    m_playbackExhaustionNotified = false;
}

void Bot::noteAttemptReset(uint32_t failedAtFrame) {
    if (m_state != State::Record) return;
    if (m_deathNoted) return;
    m_deathNoted = true;
    ++m_attemptCount;
    m_lastFailureFrame = failedAtFrame;
    skkbot2::log::warn(
        "[SKC] capture paused on death at frame {} ({} live input(s) buffered); "
        "death is not added to the macro; checkpoint reset will trim to its "
        "saved boundary or a full restart will clear the line",
        failedAtFrame, m_macro.actions.size());
}

void Bot::closeSeamHolds(uint32_t frame) {
    if (m_macro.actions.empty()) return;

    // Port of ReplayEngine::closeSeamHoldEdges (checkpoint_system.cpp:451).
    // Forward scan of the retained tail: `pressed` opens an edge for
    // (player2, button), `released` closes it. A slot still open here means its
    // release was cut by the truncation - that press would replay as an
    // endless hold across the seam, and the click pairing would match it
    // against the retry's next press and emit an orphan. Close every dangling
    // edge exactly ON the boundary frame so the stitch reads
    // press..release(boundary), then the next attempt's re-press.
    bool open[2][3] = {};
    for (const Action& action : m_macro.actions) {
        if (action.frame > frame) break;
        const int slot = action.player2 ? 1 : 0;
        const int button = static_cast<int>(action.button) - 1;
        if (button < 0 || button > 2) continue;
        open[slot][button] = action.pressed;
    }

    int appended = 0;
    for (int slot = 0; slot < 2; ++slot) {
        for (int button = 0; button < 3; ++button) {
            if (!open[slot][button]) continue;
            Action release;
            release.frame = frame;
            release.button = static_cast<uint8_t>(button + 1);
            release.pressed = false;
            release.player2 = slot == 1;
            m_macro.actions.push_back(release);
            ++appended;
        }
    }
    if (appended > 0) {
        skkbot2::log::info(
            "[SKC] seam-close: boundary={} appended {} synthetic release(s) "
            "for dangling holds", frame, appended);
    }
}

void Bot::prepareCaptureRestore(uint32_t frame) {
    // SKK's boundary is EXCLUSIVE: ReplayEngine::truncateRecordedDataAfter pops
    // while `inputs.back().tick >= tick`. An input stamped exactly on the
    // boundary is dropped because the checkpoint restore itself re-pushes the
    // buttons that were held there (PracticeFix::apply does the re-push), so
    // replaying it would double-press.
    const size_t before = m_macro.actions.size();
    std::erase_if(m_macro.actions, [frame](const Action& action) {
        return action.frame >= frame;
    });
    m_lastTrimmed = before - m_macro.actions.size();

    if (m_inputIndex > m_macro.actions.size()) {
        m_inputIndex = m_macro.actions.size();
    }
    skkbot2::log::info(
        "[SKC] capture restore prepared at frame {}: dropped {} input(s), "
        "{} kept", frame, m_lastTrimmed, m_macro.actions.size());
}

void Bot::seekTo(uint32_t frame) {
    const auto it = std::lower_bound(
        m_macro.actions.begin(), m_macro.actions.end(), frame,
        [](const Action& action, uint32_t f) { return action.frame < f; });

    m_inputIndex = static_cast<size_t>(it - m_macro.actions.begin());
}

bool Bot::save(const std::filesystem::path& path) {
    m_macro.sortActions();

    // Refuse to write a corrupt macro. Every unusable .skc so far looked fine in
    // the GUI and only revealed itself by killing the bot a few hundred frames
    // in. Failing loudly here costs one click; shipping the file costs a
    // re-record and another round of log archaeology.
    std::string reason;
    if (!validateActions(m_macro.actions, reason)) {
        skkbot2::log::error(
            "[SKC] SAVE REFUSED - the recording is corrupt and cannot be "
            "replayed: {}. Nothing was written to {}.",
            reason, skk::pathToUtf8(path));
        skkbot2::log::error(
            "[SKC] hint: this is what a practice checkpoint respawn mid-hold "
            "produces - the frame clock moved back but the release never got "
            "recorded. Re-record a run WITHOUT dying, or from the level start.");
        return false;
    }

    if (!saveToFile(path, m_macro)) {
        skkbot2::log::error("[SKC] failed to save {}", skk::pathToUtf8(path));
        return false;
    }

    m_macroName = skk::pathToUtf8(path.stem());

    // The per-channel split, because "the macro does not replay" is otherwise
    // invisible until the file is decoded. A dual-mode level can file clicks
    // under P2 with no second player present, and P1/P2 are not the same
    // physics - so a macro with an unexpected P2 stream is already wrong before
    // it is ever played.
    std::size_t p1 = 0, p2 = 0;
    for (const auto& act : m_macro.actions) {
        if (act.player2) ++p2; else ++p1;
    }
    skkbot2::log::info("[SKC] saved {} actions (P1={}, P2={}) to {}", m_macro.actions.size(),
                       p1, p2, skk::pathToUtf8(path));
    if (p2 > 0) {
        skkbot2::log::warn(
            "[SKC] the macro has {} second-player action(s). If this level has no "
            "second player, they were misrouted during recording and the macro will "
            "not reproduce - see the [SKC] record P2 press lines for the flags.",
            p2);
    }
    return true;
}

    bool Bot::load(const std::filesystem::path& path) {
   auto loaded = loadFromFile(path);
   if (!loaded) {
       skkbot2::log::error("[SKC] failed to load {}", skk::pathToUtf8(path));
       return false;
   }

    m_macro = std::move(*loaded);
    m_inputIndex = 0;

    // A file written by an older build (or by a corrupted capture) can decode
    // cleanly yet still be unplayable. Reject it here rather than let it die
    // silently at frame ~230.
    std::string reason;
    if (!validateActions(m_macro.actions, reason)) {
        skkbot2::log::error(
            "[SKC] LOAD REFUSED - {} decodes but cannot be replayed: {}.",
            skk::pathToUtf8(path.filename()), reason);
        m_macro.actions.clear();
        m_macroName.clear();
        return false;
    }

    m_macroName = skk::pathToUtf8(path.stem());
    skkbot2::log::info(
        "[SKC] loaded {} actions from {} (tps={}, mode={})",
        m_macro.actions.size(), skk::pathToUtf8(path), m_macro.header.tps,
        !m_macro.platformerKnown ? "unknown"
                                 : (m_macro.platformerMode ? "platformer" : "classic"));
    return true;
}

bool Bot::adopt(Macro macro, std::string const& name, uint32_t levelId,
                std::string levelName) {
    // Same acceptance gate as load(): an imported macro must survive exactly the
    // checks a native .skc does, or foreign files get to fail where ours would not.
    std::string reason;
    if (!validateActions(macro.actions, reason)) {
        skkbot2::log::error("[SKC] IMPORT REFUSED - {}: {}", name, reason);
        return false;
    }
    // Formats without a mode bit (Echo, SLC, NHM, RE4, AST) have to be
    // classified from the click stream itself. The old rule - "any button 2/3
    // event is proof of platformer" - was wrong: classic GD IGNORES the
    // left/right keys, but a recorder hooks the raw button, so a stray arrow
    // press during a classic run still lands in the file. Measured case: the
    // Schafs .echo carries 42,864 jump presses (P1+P2, a dual classic level)
    // and exactly 3 stray right presses, and the old rule branded the whole
    // macro platformer, so PLAY refused it in its own classic level.
    //
    // A platformer run MOVES horizontally: left/right presses are a substantial
    // share of the stream AND both directions appear, because walking one way
    // for a whole minute is not what a platformer recording looks like. Both
    // gates are required - a count gate (a handful of stray presses stays
    // noise), the both-directions gate, and a share gate (even a jump-spammy
    // platformer presses sideways far more often than a classic player
    // accidentally mashes arrows).
    //
    // The answer is logged and NOTHING ELSE. This is a guess, and a guess must
    // never gate playback: the second measured case is Windy Road.slc, a
    // 60-second classic attempt whose second half spams left (975 left presses,
    // ZERO right presses, 191 jumps, nothing left-of-centre in the first 31
    // seconds). The one-direction rule called that platformer and PLAY then
    // refused it in its own classic level - "PLAY does not turn on at all".
    // Only a format that really stores a mode (.skc's platformer flag) is
    // allowed to refuse; see platformerModeMismatch(), which reads
    // platformerKnown, and platformerKnown stays false here.
    if (!macro.platformerKnown) {
        size_t jumpPresses = 0;
        size_t leftPresses = 0;
        size_t rightPresses = 0;
        for (const auto& action : macro.actions) {
            if (!action.pressed) continue;
            if (action.button == 1) {
                ++jumpPresses;
            } else if (action.button == 2) {
                ++leftPresses;
            } else if (action.button == 3) {
                ++rightPresses;
            }
        }
        const size_t sidePresses = leftPresses + rightPresses;
        const bool bothDirections = leftPresses >= 16 && rightPresses >= 16;
        const bool platformer =
            bothDirections && sidePresses * 20 >= jumpPresses;
        skkbot2::log::info(
            "[SKC] mode heuristic for '{}': {} jump, {} left, {} right "
            "press(es) -> {}",
            name, jumpPresses, leftPresses, rightPresses,
            platformer ? "looks platformer (guess only - playback is not "
                        "gated on it)"
                       : "no platformer evidence (plays in any mode)");
    }
    m_macro = std::move(macro);
    m_inputIndex = 0;
    m_macroName = name;
    // Bind the source level. An imported macro that stays unbound is exactly the
    // case onLevelEnter() waves through ("an unbound macro has no level to
    // compare against"), so playing a foreign replay on the wrong level fails as
    // an unexplained death instead of being reported as the level mismatch it
    // is. GDR2's levelId is often 0 - EclipseMenu wrote 0 for Rauchkammer 2 -
    // so the name is the binding that actually carries.
    m_levelId = levelId;
    m_levelName = std::move(levelName);
    skkbot2::log::info(
        "[SKC] adopted {} actions as '{}' (tps={}, mode={}, level {}/{})",
        m_macro.actions.size(), name, m_macro.header.tps,
        !m_macro.platformerKnown ? "unknown"
                                 : (m_macro.platformerMode ? "platformer" : "classic"),
        m_levelId, m_levelName.empty() ? "<unbound>" : m_levelName);
    return true;
}

void Bot::onLevelExit() {
    // Halt the session but KEEP the macro in memory (unsaved), exactly like
    // SKK's onQuit: the user can still press SAVE after leaving the level.
    stop();
}

} // namespace skk::skc
