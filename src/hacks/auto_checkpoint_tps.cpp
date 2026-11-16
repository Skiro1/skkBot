
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <cmath>

#include "logger/logger.hpp"
#include "skkBot.hpp"

using namespace geode::prelude;

class $modify(AutoCheckpointTps, PlayerObject) {
    void tryPlaceCheckpoint() {
        auto* engine = ReplayEngine::get();
        if (engine && engine->skMode && engine->activeSk &&
            engine->engineMode != MODE_DISABLED) {
            return;
        }
        const double tps = engine ? engine->runtimeTickRate() : 240.0;
        if (tps <= 240.5) {
            return PlayerObject::tryPlaceCheckpoint();
        }

        PlayLayer* playLayer = GameManager::sharedState()->m_playLayer;
        if (!playLayer) return;
        if (!playLayer->m_isPracticeMode) return;
        if (!GameManager::sharedState()->getGameVariable("0027") ||
            m_uniqueID != 1) {
            return;
        }

        bool shouldTry = false;
        if (isFlying()) {
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
        if (placed && isFlying() && placed->m_player1Checkpoint) {
            placed->m_player1Checkpoint->m_platformerCheckpoint = true;
        }
    }
};
