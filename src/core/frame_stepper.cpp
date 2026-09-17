#include "skkBot.hpp"
#include "hacks/physicsbypass.hpp"

#include <Geode/Bindings.hpp>

#include <algorithm>
#include <cmath>

// Lightweight skkBot2 implementation of the engine's frame-step helpers.
// The original skkBot module also drove music seeking and bridge the
// user's holds through the (removed) autoclicker; with no autoclicker and no
// renderer, user holds reach the player through the normal GD input path, so
// bridgeUserHoldsToPlayer is a no-op.

namespace {
    static void clearNativeQueuedButtons(PlayLayer* playLayer) {
        if (playLayer) {
            playLayer->m_queuedButtons.clear();
        }
    }

    static void releasePlayerButtons(PlayLayer* playLayer) {
        if (!playLayer) {
            return;
        }
        if (playLayer->m_player1) {
            playLayer->m_player1->releaseAllButtons();
        }
        if (playLayer->m_player2) {
            playLayer->m_player2->releaseAllButtons();
        }
    }
}

void ReplayEngine::clearFrameStepState() {
    singleTickStep = false;
    frameStepMusicSeekPending = false;
}

int ReplayEngine::computeFrameStepMusicTimeMS(PlayLayer* playLayer) const {
    if (!playLayer || !playLayer->m_levelSettings) {
        return 0;
    }
    return static_cast<int>(std::llround(playLayer->m_gameState.m_levelTime * 1000.0));
}

void ReplayEngine::requestFrameStepMusicSync(PlayLayer* playLayer) {
    frameStepPendingMusicTimeMs = computeFrameStepMusicTimeMS(playLayer);
    frameStepMusicSeekPending = true;
}

void ReplayEngine::syncFrameStepAudio(FMODAudioEngine* audio) {
    static_cast<void>(audio);
}

void ReplayEngine::setFrameStepEnabled(bool enabled, PlayLayer* playLayer) {
    tickStepping = enabled;
    singleTickStep = false;

    if (!enabled) {
        frameStepMusicSeekPending = false;
        clearFrameStepState();
        if (playLayer) {
            clearNativeQueuedButtons(playLayer);
            clearQueuedSubstepState();
            clearQueuedMacroCommands();
            queuedCaptureCommands.clear();
            cbsCaptureProcessingQueue = false;
        }
        resetTimingTracking();
        if (playLayer && engineMode == MODE_DISABLED) {
            resetDeferredInputState();
            releasePlayerButtons(playLayer);
        }
    }
}

void ReplayEngine::bridgeUserHoldsToPlayer(PlayLayer* playLayer) {
    static_cast<void>(playLayer);
}
