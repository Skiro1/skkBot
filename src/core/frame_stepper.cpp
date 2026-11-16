#include "frame_stepper.hpp"
#include "skkBot.hpp"
#include "hacks/autoclicker.hpp"
#include "hacks/physicsbypass.hpp"
#include "logger/logger.hpp"
#include "render/renderer.hpp"
#include "skc/skc_bot.hpp"

#include <Geode/Bindings.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>

#include <algorithm>
#include <cmath>

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

    static bool playerWantsHold(PlayerObject* player) {
        if (!player) return false;
        return player->m_holdingButtons[1];
    }

    static bool playerCanReceiveLiveInput(PlayerObject* player) {
        if (!player) return false;
        if (player->m_isDead) return false;
        return true;
    }

    static void applyJumpHoldDelta(PlayerObject* player, bool desired) {
        if (!playerCanReceiveLiveInput(player)) {
            return;
        }

        bool currentlyHeld = playerWantsHold(player);
        if (desired == currentlyHeld) {
            return;
        }

        if (desired) {
            player->pushButton(PlayerButton::Jump);
        } else {
            player->releaseButton(PlayerButton::Jump);
        }
    }

    static bool canStepNow() {
        auto* pl = PlayLayer::get();
        if (!pl) return false;
        if (pl->m_isPaused) return false;
        if (!pl->m_started) return false;
        if (pl->m_hasCompletedLevel || pl->m_levelEndAnimationStarted) return false;
        return true;
    }
}


FrameStepper::FrameStepper() {
    auto* mod = Mod::get();
    if (mod) {
        m_stepCount = mod->getSavedValue<int>("stepper_step_count", 1);
        m_autoPauseOnDeath = mod->getSavedValue<bool>("stepper_auto_pause_death", false);
        m_holdEnabled = mod->getSavedValue<bool>("stepper_hold", true);
        m_holdDelay = mod->getSavedValue<float>("stepper_hold_delay", 0.25f);
        m_holdSpeed = mod->getSavedValue<int>("stepper_hold_speed", 1);
        if ((m_holdDelay >= 0.149f && m_holdDelay <= 0.151f) ||
            (m_holdDelay >= 0.249f && m_holdDelay <= 0.251f)) {
            m_holdDelay = 0.25f;
            mod->setSavedValue<float>("stepper_hold_delay", m_holdDelay);
        }
        if (m_holdSpeed == 3) {
            m_holdSpeed = 1;
            mod->setSavedValue<int>("stepper_hold_speed", m_holdSpeed);
        }
    }
}

bool FrameStepper::isEnabled() const {
    return ReplayEngine::get()->tickStepping;
}

void FrameStepper::setEnabled(bool enabled) {
    if (enabled) {
        if (auto* engine = ReplayEngine::get()) {
            if (engine->engineMode == MODE_EXECUTE ||
                engine->pendingAction == ReplayEngine::PendingAction::Play) {
                skkbot2::log::info("Frame Stepper: refused - playback is active");
                return;
            }
        }
        auto& bot = skk::skc::Bot::get();
        if (bot.getState() == skk::skc::State::Playback || bot.isPlaybackArmed() ||
            bot.getPendingAction() == skk::skc::PendingAction::Play) {
            skkbot2::log::info("Frame Stepper: refused - SKC playback is active");
            return;
        }
    }
    ReplayEngine::get()->setFrameStepEnabled(enabled, PlayLayer::get());
    if (!enabled) {
        m_nextFrame = false;
        m_pendingClick = 0;
    }
}

void FrameStepper::toggle() {
    setEnabled(!isEnabled());
}

void FrameStepper::setStepCount(int count) {
    m_stepCount = std::clamp(count, 1, 1000);
    if (auto* mod = Mod::get()) {
        mod->setSavedValue<int>("stepper_step_count", m_stepCount);
    }
}

void FrameStepper::setHoldToStepEnabled(bool enabled) {
    m_holdEnabled = enabled;
    if (!enabled) {
        m_holdActive = false;
        m_keyHeld = false;
        m_holdTimer = 0.0f;
        m_holdTickCounter = 0;
    }
    if (auto* mod = Mod::get()) {
        mod->setSavedValue<bool>("stepper_hold", enabled);
    }
}

void FrameStepper::setHoldDelay(float delay) {
    m_holdDelay = std::max(0.01f, delay);
    if (auto* mod = Mod::get()) {
        mod->setSavedValue<float>("stepper_hold_delay", m_holdDelay);
    }
}

void FrameStepper::setHoldSpeed(int speed) {
    m_holdSpeed = std::clamp(speed, 1, 30);
    if (auto* mod = Mod::get()) {
        mod->setSavedValue<int>("stepper_hold_speed", m_holdSpeed);
    }
}

void FrameStepper::setAutoPauseOnDeathEnabled(bool enabled) {
    m_autoPauseOnDeath = enabled;
    if (!enabled) {
        m_autoPaused = false;
    }
    if (auto* mod = Mod::get()) {
        mod->setSavedValue<bool>("stepper_auto_pause_death", enabled);
    }
}

void FrameStepper::updateAutoPause() {
    if (!m_autoPauseOnDeath) {
        return;
    }

    auto* pl = PlayLayer::get();
    if (!pl) {
        m_autoPaused = false;
        return;
    }

    const bool dead = pl->m_playerDied || (pl->m_player1 && pl->m_player1->m_isDead);

    if (dead) {
        if (isEnabled() && canStepNow() && !m_autoPaused) {
            m_autoPaused = true;
            setEnabled(false);
        }
        return;
    }

    if (m_autoPaused) {
        if (pl->m_started && !pl->m_isPaused && !pl->m_hasCompletedLevel
            && !pl->m_levelEndAnimationStarted) {
            m_autoPaused = false;
            setEnabled(true);
        }
    }
}

#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>

void FrameStepper::stepForward(int frames, bool fromMenuButton) {
    if (!canStepNow()) {
        return;
    }

    if (!isEnabled()) {
        setEnabled(true);
    }

    if (fromMenuButton && m_holdEnabled) {
        m_nextFrame = false;
        m_pendingClick = 0;
        m_holdTimer = 0.0f;
        m_holdTickCounter = 0;
        return;
    }

    int count = frames > 0 ? frames : m_stepCount;
    if (count <= 0) count = 1;

    if (!fromMenuButton) {
        m_keyTapPending = count;
        return;
    }

    m_nextFrame = true;
    m_pendingClick = count - 1;
}

bool FrameStepper::updateScheduler(cocos2d::CCScheduler* scheduler, float dt) {
    auto* pl = PlayLayer::get();
    if (!pl || pl->m_isPaused || !pl->m_started || pl->m_levelEndAnimationStarted
        || pl->m_resumeTimer > 0.0f) {
        return false;
    }

    const bool mouseDown = m_holdProbe ? m_holdProbe() : false;
    const bool menuHold = m_holdEnabled && m_holdActive && (mouseDown || !m_holdProbe);
    const bool engaged = m_keyHeld || menuHold;

    if (engaged) {
        m_holdTimer += dt;
        if (m_holdTimer >= m_holdDelay && m_pendingClick > 0) {
            m_pendingClick = 0;
        }
    } else {
        m_holdTimer = 0.0f;
        m_holdTickCounter = 0;
    }

    auto* engine = ReplayEngine::get();
    float stepDt = dt;
    if (engine) {
        float warped = static_cast<float>(engine->fixedPhysicsDelta());
        if (auto* pl2 = PlayLayer::get()) {
            const double warp =
                std::clamp(static_cast<double>(pl2->m_gameState.m_timeWarp), 0.0, 1.0);
            const float scaled = static_cast<float>(engine->fixedPhysicsDelta(warp));
            if (scaled > 0.0f && std::isfinite(scaled)) {
                warped = scaled;
            }
            static float lastLoggedWarp = -1.0f;
            const float warpF = static_cast<float>(warp);
            if (std::fabs(warpF - lastLoggedWarp) > 0.001f) {
                lastLoggedWarp = warpF;
                skkbot2::log::debug(
                    "stepper dt: warp={:.3f} tickRate={:.0f} fixed={:.8f} stepDt={:.8f}",
                    warpF, engine->runtimeTickRate(),
                    engine->fixedSimulationDelta(), warped);
            }
        }
        stepDt = warped;
    }

    if (m_keyTapPending > 0) {
        if (engaged) {
            if (m_holdTimer >= m_holdDelay) {
                m_keyTapPending = 0;
            }
            return true;
        }
        const int count = m_keyTapPending;
        m_keyTapPending = 0;
        m_nextFrame = true;
        m_pendingClick = count - 1;
    }

    if (m_nextFrame) {
        m_nextFrame = false;
        pl->m_resumeTimer = 0.0f;
        scheduler->cocos2d::CCScheduler::update(stepDt);
        return true;
    }

    if (m_pendingClick > 0) {
        m_pendingClick--;
        pl->m_resumeTimer = 0.0f;
        scheduler->cocos2d::CCScheduler::update(stepDt);
        return true;
    }

    if (engaged && m_holdTimer >= m_holdDelay) {
        m_holdTimer = m_holdDelay;
        if (++m_holdTickCounter >= m_holdSpeed) {
            m_holdTickCounter = 0;
            m_nextFrame = true;
        }
    }

    return true;
}

void FrameStepper::onResetLevel() {
    m_pendingClick = 0;
    m_nextFrame = false;
}

void FrameStepper::onPlayLayerDestroy() {
    m_pendingClick = 0;
    m_keyTapPending = 0;
    m_nextFrame = false;
    m_holdActive = false;
    m_keyHeld = false;
    m_holdTimer = 0.0f;
    m_holdTickCounter = 0;
    if (auto* engine = ReplayEngine::get()) {
        engine->syncFrameStepAudio(FMODAudioEngine::sharedEngine());
    }
}

void FrameStepper::setHoldState(bool active, bool forward) {
    if (m_holdActive != active || m_holdForward != forward) {
        m_holdActive = active;
        m_holdForward = forward;
        if (!active && !m_keyHeld) {
            m_holdTimer = 0.0f;
            m_holdTickCounter = 0;
        }
    }
}

void FrameStepper::setKeyHoldState(bool held, bool forward) {
    m_keyHeld = held;
    m_keyForward = forward;
    if (!held && !m_holdActive) {
        m_holdTimer = 0.0f;
        m_holdTickCounter = 0;
    }
}

void FrameStepper::setHoldProbe(std::function<bool()> probe) {
    m_holdProbe = std::move(probe);
}


void ReplayEngine::clearFrameStepState() {
    singleTickStep = false;
    frameStepMusicSeekPending = false;
}

int ReplayEngine::computeFrameStepMusicTimeMS(PlayLayer* playLayer) const {
    if (!playLayer || !playLayer->m_levelSettings) {
        return 0;
    }

    double seconds = static_cast<double>(playLayer->m_gameState.m_levelTime)
        + static_cast<double>(playLayer->m_levelSettings->m_songOffset);
    int musicTime = static_cast<int>(std::llround(seconds * 1000.0));

    if (auto* audio = FMODAudioEngine::sharedEngine()) {
        musicTime += audio->m_musicOffset;
    }

    return std::max(0, musicTime);
}

void ReplayEngine::requestFrameStepMusicSync(PlayLayer* playLayer) {
    frameStepPendingMusicTimeMs = computeFrameStepMusicTimeMS(playLayer);
    frameStepMusicSeekPending = true;
}

void ReplayEngine::syncFrameStepAudio(FMODAudioEngine* audio) {
    if (!audio) audio = FMODAudioEngine::sharedEngine();
    if (!audio) return;

    auto* pl = PlayLayer::get();
    bool shouldPause = audio
        && pl
        && pl->m_started
        && !pl->m_isPaused
        && tickStepping
        && !skkbot2::render::Renderer::get().isRecording();

    if (shouldPause) {
        if (!frameStepAudioActive) {
            audio->pauseAllMusic(true);
            frameStepAudioActive = true;
        }
    } else {
        if (frameStepAudioActive) {
            audio->resumeAllMusic();
            frameStepAudioActive = false;
        }
    }
}

void ReplayEngine::setFrameStepEnabled(bool enabled, PlayLayer* playLayer) {
    tickStepping = enabled;
    singleTickStep = false;
    frameStepMusicSeekPending = false;

    if (!enabled) {
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

    syncFrameStepAudio(FMODAudioEngine::sharedEngine());
}

void ReplayEngine::bridgeUserHoldsToPlayer(PlayLayer* playLayer) {
    if (!playLayer) return;
    if (engineMode == MODE_EXECUTE) return;
    auto* ac = Autoclicker::get();
    if (!ac || ac->isAutoclickerInput) return;

    applyJumpHoldDelta(playLayer->m_player1, ac->userHoldingP1);

    bool dualOrTwoPlayer = playLayer->m_levelSettings && (
        playLayer->m_levelSettings->m_twoPlayerMode ||
        playLayer->m_gameState.m_isDualMode
    );
    if (dualOrTwoPlayer) {
        applyJumpHoldDelta(playLayer->m_player2, ac->userHoldingP2);
    }
}
