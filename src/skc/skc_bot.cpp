#include "skc_bot.hpp"
#include "core/physical_keys.hpp"
#include "core/seam_phase_probe.hpp"
#include "skc_checkpoint.hpp"
#include "skc_rng.hpp"
#include "utils.hpp"

#include "core/autosave_recorder.hpp"
#include "skc_frame.hpp"
#include "skkBot.hpp"
#include "gui/skkbot_gui.hpp"
#include "hacks/physicsbypass.hpp"
#include "logger/logger.hpp"
#include "core/page_heap_debug.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <fstream>

using namespace geode::prelude;

namespace skk::skc {

namespace {

unsigned g_captureReconciliationDepth = 0;
struct ScopedCaptureReconciliation {
    ScopedCaptureReconciliation() { ++g_captureReconciliationDepth; }
    ~ScopedCaptureReconciliation() { --g_captureReconciliationDepth; }
    ScopedCaptureReconciliation(const ScopedCaptureReconciliation&) = delete;
    ScopedCaptureReconciliation& operator=(const ScopedCaptureReconciliation&) = delete;
};

constexpr char const* kCbsKey = "0177";
constexpr char const* kCosKey = "0176";

bool g_accuracySaved = false;
bool g_savedCbs = false;
bool g_savedCos = false;
bool g_layerSaved = false;
bool g_savedLayerCbs = false;
bool g_savedLayerCos = false;

void applyTps(double tps) {
    if (!std::isfinite(tps) || tps <= 0.0)
        return;
    if (auto* engine = ReplayEngine::get()) {
        engine->tickRate = tps;
    }
}

void applyDisplayFps(float storedFps) {
    const auto rate =
        skkbot2::fps::MacroDisplayRate::fromWire(static_cast<double>(storedFps));
    if (!skkbot2::fps::beginMacro(rate)) {
        skkbot2::log::warn("[TIMING] macro display rate refused (wire={:.1f})",
                           static_cast<double>(storedFps));
    }
}

}

void Bot::applySessionAccuracy(bool cbs, bool cos) {
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
    if (!m_macro.platformerKnown) return false;
    auto* playLayer = PlayLayer::get();
    if (!playLayer || !playLayer->m_levelSettings) return false;
    return m_macro.platformerMode != playLayer->m_levelSettings->m_platformerMode;
}

void Bot::startRecording() {
    skkbot2::seam_probe::skcCenters.clear();
    PracticeFix::get().clear();
    skkbot2::debug::setFullPageHeap(true);
    double captureTps = m_macro.header.tps;
    if (m_state == State::Disabled) {
        if (auto* engine = ReplayEngine::get()) {
            m_savedTickRate = engine->tickRate;
            captureTps = engine->runtimeTickRate();
        }
    } else if (auto* engine = ReplayEngine::get()) {
        captureTps = engine->runtimeTickRate();
    }
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
    onResetEvent(ResetReason::UserClear, 0);
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
    if (auto* layer = PlayLayer::get()) {
        bindLevel(layer->m_level);
        if (layer->m_levelSettings) {
            m_macro.platformerMode = layer->m_levelSettings->m_platformerMode;
            m_macro.platformerKnown = true;
            m_macro.header.platformerMode = m_macro.platformerMode;
            m_macro.header.platformerKnown = true;
        }
    }
    m_macro.header.tps = captureTps;
    m_macro.header.fps = 0.0f;
    applyTps(captureTps);
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
    if (startFrame > 240) {
        skkbot2::log::warn(
            "[SKC] recording started {} frames into the level: playback can only "
            "reproduce it if the player re-runs the same stretch from the spawn "
            "(SKC stores no start position)", startFrame);
    }
}

void Bot::startPlayback() {
    skkbot2::debug::setFullPageHeap(true);
    skkbot2::log::info("[SKC] startPlayback: after setFullPageHeap");
    skkbot2::Logger::get().flush();
    if (platformerModeMismatch()) {
        auto* layer = PlayLayer::get();
        const bool levelPlatformer = layer && layer->m_levelSettings &&
                                     layer->m_levelSettings->m_platformerMode;
        skkbot2::log::error(
            "[SKC] playback refused: the macro was recorded in {} mode, but this "
            "level is {} mode. Open a level of the matching mode.",
            m_macro.platformerMode ? "platformer" : "classic",
            levelPlatformer ? "platformer" : "classic");
        m_playbackArmed = false;
        m_pendingAction = PendingAction::None;
        return;
    }
    if (m_state == State::Disabled) {
        if (auto* engine = ReplayEngine::get()) {
            m_savedTickRate = engine->tickRate;
        }
    }
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
    skkbot2::log::info("[SKC] startPlayback: after restart()");
    skkbot2::Logger::get().flush();
    journalResetCursor();
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
    if (auto* layer = PlayLayer::get()) {
        const bool levelPlatformer =
            layer->m_levelSettings && layer->m_levelSettings->m_platformerMode;
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

    for (size_t i = 0; i < std::min<size_t>(m_macro.actions.size(), 16); ++i) {
        const auto& action = m_macro.actions[i];
        skkbot2::log::info(
            "[SKC] macro input#{}: frame={} btn={} down={} p2={}", i,
            action.frame, action.button, action.pressed ? 1 : 0,
            action.player2 ? 1 : 0);
    }
}

void Bot::armPlayback() {
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

    if (platformerModeMismatch()) {
        out.decision = StartDecision::WrongMode;
        out.reason = "the macro and the level are in different game modes";
        return out;
    }

    if (!playLayer->m_started) {
        out.decision = StartDecision::Ready;
        out.reason = "";
        return out;
    }

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
    skkbot2::log::info("[SKC] stop: entry (state={})", static_cast<int>(m_state));
    skkbot2::Logger::get().flush();
    if (m_state == State::Record) {
        skkbot2::autosave::flushRecording();
        auto& gui = skkbot2::gui::get();
        if (gui.macroName[0] == '\0') {
            const std::string autoName = skkbot2::autosave::skcAutosaveName();
            if (!autoName.empty()) {
                std::snprintf(gui.macroName, sizeof(gui.macroName), "%s", autoName.c_str());
                skkbot2::log::info("[SKC] stop: empty macro name auto-filled from autosave ('{}')", autoName);
            }
        }
    }
    m_state = State::Disabled;
    m_pendingAction = PendingAction::None;
    m_playbackArmed = false;
    if (wasHolding) skkbot2::fps::endMacro();
    skkbot2::debug::setFullPageHeap(false);
    restoreUserAccuracy();
    if (wasHolding) {
        if (auto* engine = ReplayEngine::get()) {
            engine->tickRate = m_savedTickRate;
        }
    }
    if (wasHolding) {
        if (auto* playLayer = PlayLayer::get()) {
            if (playLayer->m_player1) playLayer->m_player1->releaseAllButtons();
            if (playLayer->m_player2) playLayer->m_player2->releaseAllButtons();
        }
    }
    skkbot2::log::info("[SKC] stopped");
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
    if (m_macro.platformerKnown &&
        m_macro.platformerMode != level->isPlatformer()) {
        skkbot2::log::error(
            "[SKC] queued PLAY refused: the macro was recorded in {} mode, but "
            "this level is {} mode.",
            m_macro.platformerMode ? "platformer" : "classic",
            level->isPlatformer() ? "platformer" : "classic");
        return false;
    }
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
    if (m_levelId == 0 && m_levelName.empty())
        return false;
    if (!level)
        return false;

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
    if (g_captureReconciliationDepth != 0) {
        skkbot2::log::debug(
            "[SKC] capture [internal-reconciliation] frame={} button={} p2={} "
            "down={} depth={} (единственная запись — напрямую)",
            frame, button, player2, pressed, g_captureReconciliationDepth);
        return;
    }
    if (m_deathNoted) {
        ++m_suppressedEvents;
        if (m_suppressedEvents <= 5) {
            skkbot2::log::debug(
                "[SKC] drop [death-window] frame={} button={} p2={} down={}",
                frame, button, player2, pressed);
        }
        return;
    }

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

    Action action;
    action.frame = frame;
    action.button = button;
    action.pressed = pressed;
    action.player2 = player2;
    action.halfTick = false;
    m_macro.actions.push_back(action);
    m_rngTypedInput = true;
    skkbot2::log::debug(
        "[SKC] record frame={} button={} p2={} down={} (action {}/{})",
        frame, button, player2, pressed, m_macro.actions.size(),
        m_duplicateEvents + m_suppressedEvents + m_droppedEvents);
}

void Bot::syncHoldShadowToPlayer() {
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
    m_suppressionArmed = false;
    syncHoldShadowToPlayer();
    (void)frame;
}

void Bot::reconcileCaptureRestore(uint32_t frame) {
    if (m_state != State::Record) return;
    auto* layer = PlayLayer::get();
    if (!layer || !layer->m_player1) return;

    const auto mode = currentPhysicalHoldMode(layer);
    int transitions = 0;
    for (int slot = 0; slot < 2; ++slot) {
        auto* player = slot == 0 ? layer->m_player1 : layer->m_player2;
        if (!player) continue;
        for (int button = 1; button <= 3; ++button) {
            const auto heldIt = player->m_holdingButtons.find(button);
            const bool snapshotHeld = heldIt != player->m_holdingButtons.end() &&
                                      heldIt->second;
            const bool physicalHeld = physicalHoldActive(
                mode, button, slot == 1, player->m_controlsDisabled);
            if (snapshotHeld == physicalHeld) {
                m_holdShadow[slot][button] = snapshotHeld;
                continue;
            }

            bool streamHeld = false;
            for (auto it = m_macro.actions.rbegin(); it != m_macro.actions.rend(); ++it) {
                if (it->button == button && it->player2 == (slot == 1)) {
                    streamHeld = it->pressed;
                    break;
                }
            }
            if (streamHeld == physicalHeld) {
                skkbot2::log::warn(
                    "[SKC] capture reconciliation repeated command: frame={} "
                    "p2={} button={} down={} streamHeld={} snapshotHeld={}; "
                    "preserved for native replay verification",
                    frame, slot == 1, button, physicalHeld, streamHeld, snapshotHeld);
            }
            const bool rawP2 = handleButtonPlayer2Param(slot == 1,
                                                        controlFlipActive(layer));
            const size_t actionsBefore = m_macro.actions.size();
            {
                ScopedCaptureReconciliation internalInput;
                layer->GJBaseGameLayer::handleButton(physicalHeld, button, rawP2);
            }
            const size_t hookAppended = m_macro.actions.size() - actionsBefore;
            if (hookAppended != 0) {
                skkbot2::log::warn(
                    "[SKC] reconciliation: внутренний handleButton изменил стрим "
                    "frame={} p2={} button={} hookAppended={}",
                    frame, slot == 1, button, hookAppended);
            }
            Action action;
            action.frame = frame;
            action.button = static_cast<uint8_t>(button);
            action.player2 = slot == 1;
            action.pressed = physicalHeld;
            m_macro.actions.push_back(action);
            m_rngTypedInput = true;
            m_holdShadow[slot][button] = physicalHeld;
            ++transitions;
            skkbot2::log::info(
                "[SKC] capture restore reconciliation frame={} p2={} rawP2={} button={} snapshot={} physical={} recorded={} hookAppended={}",
                frame, slot == 1, rawP2, button, snapshotHeld, physicalHeld,
                physicalHeld ? "press" : "release", hookAppended);
        }
    }
    if (transitions == 0) {
        skkbot2::log::info(
            "[SKC] capture restore reconciliation frame={}: snapshot holds match physical keys",
            frame);
    }
}

void Bot::finishCaptureRestore(uint32_t frame) {
    if (m_state != State::Record) return;
    noteInternalRestore(frame);
    closeSeamHolds(frame);
    reconcileCaptureRestore(frame);
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

    const Action& next = m_macro.actions[m_inputIndex];
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
    for (const Action& action : m_macro.actions) {
        if (action.frame >= frame) break;
        const int button = static_cast<int>(action.button) - 1;
        if (button < 0 || button > 2) continue;
        state.held[action.player2 ? 1 : 0][button] = action.pressed;
    }
    return state;
}

Bot::SeamQueueReport Bot::sanitizeSeamQueue(
    std::vector<PlayerButtonCommand>& queue, uint32_t resumeFrame,
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
    const bool anyActionBefore =
        !m_macro.actions.empty() && m_macro.actions.front().frame < resumeFrame;

    if (mode == SeamQueueMode::Record) {
        report.rule = "record seam (all queued commands belong to the dead attempt)";
        for (const PlayerButtonCommand& cmd : queue) {
            const int button = static_cast<int>(cmd.m_button);
            if (button < 1 || button > 3) continue;
            const int slot = cmd.m_isPlayer2 ? 1 : 0;
            if (!cmd.m_isPush && !stream.held[slot][button - 1]) {
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
        skkbot2::log::info(
            "[SKC] seam-queue[{} site={} f={} ignoreInputs={}]: kept {} queued "
            "command(s) - tail does not contradict the stream: {}",
            mode == SeamQueueMode::Record ? "record" : "play", site, resumeFrame,
            m_ignoreInputs ? 1 : 0, report.inspected, digest.c_str());
    }
    return report;
}

bool Bot::onResetEvent(ResetReason reason, uint32_t boundaryFrame) {
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
        journalResetAttempt(0, true);
        return dropped > 0;
    }

    const size_t before = m_macro.actions.size();
    prepareCaptureRestore(boundaryFrame);
    skkbot2::log::info(
        "[SKC] reset[{}] STITCH at frame {} - {} of {} input(s) kept",
        name, boundaryFrame, m_macro.actions.size(), before);
    journalResetAttempt(boundaryFrame, false);
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

    bool open[2][3] = {};
    for (const Action& action : m_macro.actions) {
        if (action.frame > frame) break;
        const int slot = action.player2 ? 1 : 0;
        const int button = static_cast<int>(action.button) - 1;
        if (button < 0 || button > 2) continue;
        open[slot][button] = action.pressed;
    }

    int appended = 0;
    auto* layer = PlayLayer::get();
    for (int slot = 0; slot < 2; ++slot) {
        auto* player = layer ? (slot == 0 ? layer->m_player1 : layer->m_player2) : nullptr;
        for (int button = 0; button < 3; ++button) {
            if (!open[slot][button]) continue;
            const int logicalButton = button + 1;
            bool snapshotHeld = false;
            if (player) {
                const auto heldIt = player->m_holdingButtons.find(logicalButton);
                snapshotHeld = heldIt != player->m_holdingButtons.end() &&
                               heldIt->second;
            }
            if (snapshotHeld) continue;
            Action release;
            release.frame = frame;
            release.button = static_cast<uint8_t>(logicalButton);
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

    std::string reason;
    if (!validateActions(m_macro.actions, reason, true)) {
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

    std::string reason;
    if (!validateActions(loaded->actions, reason, true)) {
        skkbot2::log::error(
            "[SKC] LOAD REFUSED - {} decodes but cannot be replayed: {}.",
            skk::pathToUtf8(path.filename()), reason);
        return false;
    }

    stop();
    PracticeFix::get().clear();
    m_macro = std::move(*loaded);
    m_inputIndex = 0;
    m_levelId = 0;
    m_levelName.clear();
    journalResetCursor();
    if (!m_macro.rngJournal.empty()) {
        skkbot2::log::info("[SKC] file carries a rng journal ({} anchor(s))",
                           m_macro.rngJournal.size());
    }
    m_macroName = skk::pathToUtf8(path.stem());
    bool held[2][3] = {};
    size_t repeatedPresses = 0;
    uint32_t firstRepeatedFrame = 0;
    for (const auto& action : m_macro.actions) {
        const int slot = action.player2 ? 1 : 0;
        const int button = action.button - 1;
        if (action.pressed && held[slot][button]) {
            if (repeatedPresses++ == 0) firstRepeatedFrame = action.frame;
        }
        held[slot][button] = action.pressed;
    }
    if (repeatedPresses) {
        skkbot2::log::warn(
            "[SKC] native repeated press preserved: count={} firstFrame={}; "
            "no releases inserted, source capture needs verification",
            repeatedPresses, firstRepeatedFrame);
    }
    skkbot2::log::info(
        "[SKC] loaded {} actions from {} (tps={}, mode={})",
        m_macro.actions.size(), skk::pathToUtf8(path), m_macro.header.tps,
        !m_macro.platformerKnown ? "unknown"
                                 : (m_macro.platformerMode ? "platformer" : "classic"));
    skkbot2::Logger::get().flush();
    return true;
}

bool Bot::adopt(Macro macro, std::string const& name, uint32_t levelId,
                std::string levelName) {
    std::string reason;
    if (!validateActions(macro.actions, reason)) {
        skkbot2::log::error("[SKC] IMPORT REFUSED - {}: {}", name, reason);
        return false;
    }
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
    stop();
    PracticeFix::get().clear();
    m_macro = std::move(macro);
    m_inputIndex = 0;
    journalResetCursor();
    m_macroName = name;
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
    stop();
}

}
