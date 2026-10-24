#include "skkBot.hpp"
#include "skc/skc_bot.hpp"
#include "core/gameplay_layer.hpp"
#include "core/playback_align.hpp"
#include "hacks/physicsbypass.hpp"

#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>

using namespace geode::prelude;

namespace skk::editor {

    // Fresh-spawn alignment for an editor playtest: mirrors the PlayLayer
    // attempt reset path (applyReplayResetState) through the shared
    // playback_align helpers. Called at most once per playtest; the
    // (MODE_EXECUTE && !startPosActive && isEditorPlaytest()) loop-guard in
    // processCommands guarantees that:
    //  - spawn IS covered by the recording -> re-anchor cursors + tickOffset
    //    exactly there (start-pos and mid-run macros both get their route);
    //  - spawn is NOT covered (off-route start pos) -> park the engine in the
    //    armed wait; the live takeover (tryLivePlaybackTakeover) arms the moment
    //    the player physically reaches a spot the macro covers (no teleport).
    void attemptPlaytestArm(GJBaseGameLayer* layer, ReplayEngine* engine) {
        if (!layer || !engine || !engine->skMode || !engine->activeSk) {
            // Legacy (GDR) macros have no position streams: keep the classic
            // tick-0 playback and skip the coverage gate entirely.
            return;
        }
        if (engine->engineMode != MODE_EXECUTE || engine->startPosActive) {
            return;
        }

        auto* p = layer->m_player1;
        if (!p || p->m_isDead || layer->m_levelEndAnimationStarted) {
            return;
        }

        const float refX = p->getPositionX();
        const float refY = p->getPositionY();

        std::vector<PlaybackAnchor> emptyAnchors;
        auto* anchors = engine->activeAnchors();
        const int targetTick = skk::playback_align::matchStartPosTargetTick(
            engine, anchors ? *anchors : emptyAnchors, refX, refY);

        if (targetTick < 0) {
            // Spawn off the recorded route: hold the armed wait - the live
            // takeover arms when the player reaches covered ground.
            skkbot2::log::info("EDITOR PLAY wait: spawn=({:.0f},{:.0f}) not covered by macro - holding until live coverage",
                refX, refY);
            engine->parkPlaybackPending();
            return;
        }

        const int baselineTick = skk::playback_align::playbackBaselineTick(layer, engine);
        engine->tickOffset = targetTick - baselineTick;
        engine->startPosActive = true;
        if (auto* a = engine->activeAnchors(); a && !a->empty()) {
            engine->playbackAnchorIndex = skk::playback_align::findFirstAnchorAtTick(*a, targetTick);
        }
        if (auto* f = engine->activePerFrames(); f && !f->empty()) {
            engine->perFramePlaybackIndex = skk::playback_align::findFirstPerFrameAtTick(*f, targetTick);
        }
        if (auto* il = engine->activeSkInputs()) {
            engine->executeIndex = skk::playback_align::findFirstSkInputAtTick(*il, targetTick);
        } else if (engine->activeMacro) {
            engine->executeIndex = skk::playback_align::findFirstInputAtTick(engine->activeMacro->inputs, targetTick);
        }
        engine->applyVarianceOverrides();
        engine->clearStartPosWarning();
        skkbot2::log::info("EDITOR PLAY aligned: spawn=({:.0f},{:.0f}) -> targetTick={} baseline={} offset={} anchors={}",
            refX, refY, targetTick, baselineTick, engine->tickOffset,
            anchors ? static_cast<unsigned long long>(anchors->size()) : 0);
    }
}

class $modify(RecordGuardPauseLayer, PauseLayer) {
    void goEdit() {
        auto* engine = ReplayEngine::get();
        if (engine) {
            if (engine->engineMode == MODE_CAPTURE) {
                skkbot2::log::info("PAUSE: goEdit clicked - RECORD disabled");
                engine->haltExecution();
                skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
            }
            engine->clearPendingAction();
        }
        auto& bot = skk::skc::Bot::get();
        if (bot.getState() == skk::skc::State::Record) {
            skkbot2::log::info("PAUSE: goEdit clicked - SKC RECORD disabled");
            bot.stop();
        }
        bot.clearPendingAction();
        PauseLayer::goEdit();
    }
};

class $modify(MacroEditorLayer, LevelEditorLayer) {
    struct Fields {};

    bool isBotEngaged() const {
        auto* engine = ReplayEngine::get();
        auto& bot = skk::skc::Bot::get();
        return (engine && (engine->engineMode == MODE_EXECUTE || engine->pendingPlaybackStart)) ||
               bot.getState() == skk::skc::State::Playback || bot.isPlaybackArmed();
    }

    // Entering the editor straight from a level: drop any level-bound PLAY
    // or RECORD (executing, armed-waiting, or queued for the next level entry).
    // carried-over state must not persist into the editor.
    bool init(GJGameLevel* level, bool noUI) {
        auto* engine = ReplayEngine::get();
        if (engine) {
            const bool hadPlayback =
                engine->engineMode == MODE_EXECUTE ||
                engine->pendingPlaybackStart ||
                engine->pendingAction == ReplayEngine::PendingAction::Play;
            if (hadPlayback) {
                engine->haltExecution();
                engine->clearPendingAction();
                skkbot2::log::info("EDITOR: entered from a level - PLAY disabled (mode={} pending={} queued={})",
                    static_cast<int>(engine->engineMode),
                    engine->pendingPlaybackStart ? 1 : 0,
                    engine->pendingAction == ReplayEngine::PendingAction::Play ? 1 : 0);
            }
            const bool hadRecord =
                engine->engineMode == MODE_CAPTURE ||
                engine->pendingAction == ReplayEngine::PendingAction::Record;
            if (hadRecord) {
                engine->haltExecution();
                engine->clearPendingAction();
                skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
                skkbot2::log::info("EDITOR: entered from a level - RECORD disabled (mode={} queued={})",
                    static_cast<int>(engine->engineMode),
                    engine->pendingAction == ReplayEngine::PendingAction::Record ? 1 : 0);
            }
        }
        auto& bot = skk::skc::Bot::get();
        if (bot.getState() == skk::skc::State::Playback || bot.isPlaybackArmed() ||
            bot.getPendingAction() == skk::skc::PendingAction::Play) {
            bot.stop();
            bot.clearPendingAction();
            skkbot2::log::info("EDITOR: entered from a level - SKC PLAY disabled");
        }
        if (bot.getState() == skk::skc::State::Record || bot.getPendingAction() == skk::skc::PendingAction::Record) {
            bot.stop();
            bot.clearPendingAction();
            skkbot2::log::info("EDITOR: entered from a level - SKC RECORD disabled");
        }
        return LevelEditorLayer::init(level, noUI);
    }

    void onPlaytest() {
        LevelEditorLayer::onPlaytest();
        skk::gameplay::setEditorPlaytestActive(true);
        auto& bot = skk::skc::Bot::get();
        if (bot.getState() == skk::skc::State::Playback) {
            bot.restart();
            skkbot2::log::info("EDITOR PLAY: SKC playtest started ({} actions)", bot.getInputCount());
        }
        auto* engine = ReplayEngine::get();
        skkbot2::log::info("EDITOR PLAY: playtest started engine={}",
            engine ? static_cast<int>(engine->engineMode) : -1);
        if (engine && engine->activeSkInputs() && !engine->activeSkInputs()->empty()) {
            skkbot2::log::info("EDITOR PLAY: stream [0].tick={} last.tick={}",
                (*engine->activeSkInputs())[0].tick, engine->activeSkInputs()->back().tick);
        }
    }

    void onStopPlaytest() {
        skk::gameplay::setEditorPlaytestActive(false);
        auto& bot = skk::skc::Bot::get();
        // FULL stop, not a cursor reset. It used to call bot.restart() and leave
        // m_state == Playback, so the bot stayed "playing" with no level: the
        // transport kept reading PLAYING, a queued action could still be armed,
        // and any button holding survived the exit. Bot::stop() clears the state,
        // the pending action AND the armed flag, restores the tick rate, and
        // releases both players' buttons - which matters most here, because a
        // jump held at the moment you leave playtest is exactly the thing that
        // makes the editor feel broken afterwards.
        //
        // The recorded macro itself is NOT discarded: Bot::stop() keeps m_macro,
        // so a run recorded in the editor and exited mid-flight can still be
        // saved. Only the session ends.
        const bool skcWasLive =
            bot.isEnabled() || bot.isPlaybackArmed() ||
            bot.getPendingAction() != skk::skc::PendingAction::None;
        if (skcWasLive) {
            skkbot2::log::info(
                "EDITOR PLAY: SKC session ended on playtest exit - state={} "
                "armed={} pending={} -> cleared, macro kept ({} actions)",
                static_cast<int>(bot.getState()), bot.isPlaybackArmed(),
                static_cast<int>(bot.getPendingAction()), bot.getInputCount());
            bot.stop();
            bot.restart();
        }
        auto* engine = ReplayEngine::get();
        if (engine && (engine->engineMode != MODE_DISABLED ||
                       engine->pendingAction != ReplayEngine::PendingAction::None)) {
            skkbot2::log::info("EDITOR PLAY: playtest stopped - halting playback");
            engine->haltExecution();
            // haltExecution() clears pendingPlaybackStart but NOT the queued
            // Record/Play. A queue left over from the editor would otherwise sit
            // there and fire on the next real PlayLayer::init - i.e. the next
            // level the user opens, unasked. That is the same class of "state
            // outlives the session" this whole function exists to prevent.
            engine->clearPendingAction();
        }
        // Drop any custom timing state (expected-ticks patch, scheduler carry)
        // a TPS/Speed override left engaged, so it cannot leak into the editor
        // build mode or a later level.
        resetSimulationTimingState();
        LevelEditorLayer::onStopPlaytest();
    }

    // The editor playtest ends on player death. While the bot is engaged
    // (armed-waiting or actively replaying) the character must survive to the
    // end of the recorded route, so swallow death for the playtest session.
    void playerTookDamage(PlayerObject* player) {
        if (skk::gameplay::isEditorPlaytest() && isBotEngaged()) {
            skkbot2::log::debug("EDITOR PLAY: swallowed player damage");
            return;
        }
        LevelEditorLayer::playerTookDamage(player);
    }
};