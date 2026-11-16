// BUG-110 (05.11.2026): GD's auto-checkpoint gate is STEP-based, so at a
// hacked tick rate the storm returns.
//
// Vanilla mechanics (GD-2.2081-Decompiled PlayerObject.cpp:7808-7844, fields
// verified against bindings 2.2091): PlayerObject::tryPlaceCheckpoint places
// a practice auto-checkpoint when
//     m_currentProgress - lastCheckpoint->m_commandIndex > 500
// (100 in quick-checkpoint mode). m_currentProgress grows with physics
// STEPS, so at 240 TPS the gate means "about one level second" - but at
// tps=10000 the same 500 units pass in ~25ms of level time and GD dumps a
// full-state checkpoint on every valid ground contact. Measured in the
// 22:31 record: auto-checkpoints every 1941-5234 ticks (0.19-0.52 s of
// level time at tps=10000), each one a full-state snapshot - the BUG-110
// storm that starves the frame thread at high TPS.
//
// Fix: replicate the vanilla gate with the progress distance SCALED by
// tps/240, so the level-time interval between auto-checkpoints stays what
// 240 TPS gives regardless of the tick rate. Everything else (practice
// mode, game variable 0027, player 1 only, flying tries, timeout) is copied
// verbatim from the decompile. Manual checkpoints (Z -> m_tryPlaceCheckpoint
// -> PlayLayer::processCheckpoints) and in-level checkpoint objects take a
// different path and are NOT touched.
//
// The hook passes through to vanilla whenever the effective tick rate is
// vanilla (<= 240), so normal gameplay is byte-identical to before.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <cmath>

#include "logger/logger.hpp"
#include "skkBot.hpp"

using namespace geode::prelude;

class $modify(AutoCheckpointTps, PlayerObject) {
    void tryPlaceCheckpoint() {
        auto* engine = ReplayEngine::get();
        // BUG-121 (09.11.2026): while an SKK session is active, GD's invisible
        // auto-checkpoints poison practice: every auto CP appends an RNG
        // boundary and a practiceFix remainder, and a death respawns the
        // player from a checkpoint the user never placed or saw (flying modes
        // only - that is why cube/robot never showed it: vanilla gates the
        // auto path behind isFlying() / ground-contact flags). Manual
        // checkpoints (Z -> processCheckpoints -> markCheckpoint) and in-level
        // setup checkpoint objects do NOT go through tryPlaceCheckpoint and
        // stay available. Outside SKK the hook keeps its vanilla behaviour.
        if (engine && engine->skMode && engine->activeSk &&
            engine->engineMode != MODE_DISABLED) {
            return;
        }
        const double tps = engine ? engine->runtimeTickRate() : 240.0;
        if (tps <= 240.5) {
            return PlayerObject::tryPlaceCheckpoint();
        }

        // Vanilla body (PlayerObject.cpp:7810-7843) with one number scaled.
        PlayLayer* playLayer = GameManager::sharedState()->m_playLayer;
        if (!playLayer) return;
        if (!playLayer->m_isPracticeMode) return;
        if (!GameManager::sharedState()->getGameVariable("0027") ||
            m_uniqueID != 1) {
            return;
        }

        bool shouldTry = false;
        if (isFlying()) {
            // Vanilla also writes PlayerObject::m_platformerCheckpoint here,
            // but the bindings place that field on PlayerCheckpoint only (see
            // the note at the markCheckpoint call below) - the flag lands on
            // the fresh checkpoint instead.
            shouldTry = true;
        } else {
            shouldTry = m_shouldTryPlacingCheckpoint || m_canPlaceCheckpoint;
        }
        if (!shouldTry) return;
        if (!isFlying()) m_shouldTryPlacingCheckpoint = false;

        CheckpointObject* lastCheckpoint = playLayer->getLastCheckpoint();
        if (lastCheckpoint) {
            const double scale = tps / 240.0;
            const int requiredProgress = static_cast<int>(
                std::llround((m_quickCheckpointMode ? 100.0 : 500.0) * scale));
            const int delta = static_cast<int>(
                playLayer->m_gameState.m_currentProgress -
                lastCheckpoint->m_commandIndex);
            if (delta <= requiredProgress) return;
        }

        m_checkpointTimeout = true;
        m_lastCheckpointTime =
            cocos2d::CCDirector::sharedDirector()->getTotalFrames();
        CheckpointObject* placed = playLayer->markCheckpoint();
        // The decompile's tryPlaceCheckpoint writes m_platformerCheckpoint
        // while flying, but the bindings (2.2081 AND 2.2091) place that field
        // on PlayerCheckpoint, not PlayerObject - the verified shape is
        // PlayLayer.cpp:2267: checkpoint->m_player1Checkpoint->m_platformer
        // Checkpoint = true. Mirror that on the fresh checkpoint so a flying
        // platformer auto-checkpoint still restores with zeroed velocity.
        if (placed && isFlying() && placed->m_player1Checkpoint) {
            placed->m_player1Checkpoint->m_platformerCheckpoint = true;
        }
    }
};
