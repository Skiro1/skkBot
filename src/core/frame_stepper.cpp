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

    // A step is only legal inside a RUNNING attempt: not in a menu, not while the
    // pause menu owns the level, not before the run starts and not after it ends.
    // The old checks only looked at m_hasCompletedLevel, so pressing the key or
    // the button in the pause menu queued frames that fired the moment the game
    // resumed.
    static bool canStepNow() {
        auto* pl = PlayLayer::get();
        if (!pl) return false;
        if (pl->m_isPaused) return false;
        if (!pl->m_started) return false;
        if (pl->m_hasCompletedLevel || pl->m_levelEndAnimationStarted) return false;
        return true;
    }
}

// ---------------------------------------------------------------------------
// FrameStepper singleton вЂ” forward-only frame stepping (reference: GDH
// frame_stepper.cpp / ToastyReplay tickStepping).
//
// The game is frozen by simply NOT calling CCScheduler::update. A step lets
// exactly ONE fixed-size simulation tick through, then re-freezes. GDH keeps a
// separate g_pause gate; we treat "stepper enabled" as that gate, since the mod
// only ever exposes freeze/step anyway.
// ---------------------------------------------------------------------------

FrameStepper::FrameStepper() {
    auto* mod = Mod::get();
    if (mod) {
        m_stepCount = mod->getSavedValue<int>("stepper_step_count", 1);
        m_autoPauseOnDeath = mod->getSavedValue<bool>("stepper_auto_pause_death", false);
        m_holdEnabled = mod->getSavedValue<bool>("stepper_hold", true);
        m_holdDelay = mod->getSavedValue<float>("stepper_hold_delay", 0.25f);
        m_holdSpeed = mod->getSavedValue<int>("stepper_hold_speed", 1);
        // The delay is what separates a tap from a hold: past it, the discrete
        // backlog is dropped and the repeat takes over. 0.15s sat right inside a
        // normal human tap (150-250ms), so ordinary presses were misread as holds
        // and paid a single frame instead of the configured count. 0.25s gives a
        // real tap room to finish. Stored values matching either shipped default
        // are migrated rather than treated as a deliberate choice.
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
    // The stepper drives CCScheduler::update one tick at a time; playback drives
    // it for real. Both owning it at once corrupts the run, so refuse to arm
    // while a macro is executing or about to start (the menu disables the toggle
    // in that state too, this is the enforcement).
    if (enabled) {
        if (auto* engine = ReplayEngine::get()) {
            // Only genuinely active or genuinely queued playback blocks the
            // stepper. pendingPlaybackStart is an arming latch that outlives a
            // cancelled PLAY, so consulting it here would refuse to arm the
            // stepper for the rest of the session.
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

    // A frozen stepper also freezes the death animation, so GD can never finish
    // the respawn. Detect the death first and RELEASE the freeze; the level then
    // runs normally until the respawn lands, and the second half re-arms the
    // freeze so stepping continues from the checkpoint.
    const bool dead = pl->m_playerDied || (pl->m_player1 && pl->m_player1->m_isDead);

    if (dead) {
        // Only trip from a state where stepping is legal, otherwise a death that
        // happens on its own (no stepper involved) would still be "handled" and
        // would silently freeze the level on respawn.
        if (isEnabled() && canStepNow() && !m_autoPaused) {
            m_autoPaused = true;
            setEnabled(false);
        }
        return;
    }

    // Alive again: only re-freeze if the level is actually runnable, otherwise
    // we would latch the stepper on while the pause menu or the completion
    // animation still owns the level.
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

    // Stepping always implies the freeze (GDH: every step keybind sets
    // g_pause = true before arming the advance).
    if (!isEnabled()) {
        setEnabled(true);
    }

    // Only the menu button obeys the Hold To Step switch. With hold on, a press
    // must arm nothing so the repeat below is the sole source of steps. The
    // keybind never consults the switch: press and hold both work from the key
    // unconditionally.
    if (fromMenuButton && m_holdEnabled) {
        m_nextFrame = false;
        m_pendingClick = 0;
        m_holdTimer = 0.0f;
        m_holdTickCounter = 0;
        return;
    }

    int count = frames > 0 ? frames : m_stepCount;
    if (count <= 0) count = 1;

    // The keybind waits: the press only OWES frames here, it does not produce
    // them. updateScheduler decides tap-vs-hold once the key is released or the
    // hold delay elapses, so holding never lurches forward by the tap count
    // before the repeat settles in.
    if (!fromMenuButton) {
        m_keyTapPending = count;
        return;
    }

    // The press owes exactly `count` frames. This queue is drained first and is
    // never cancelled, so holding cannot add to it вЂ” it only continues into the
    // repeat once the queue is empty.
    m_nextFrame = true;
    m_pendingClick = count - 1;
}

bool FrameStepper::updateScheduler(cocos2d::CCScheduler* scheduler, float dt) {
    auto* pl = PlayLayer::get();
    // Normal game loop allowed when:
    // - Not in PlayLayer
    // - Paused (pause menu animates normally)
    // - Not started yet (level initialization/spawn runs normally)
    // - Level completed
    // - Resume timer countdown active (after unpause or restart, counts down in
    //   real time; the unfreeze must finish before it can be frozen again)
    if (!pl || pl->m_isPaused || !pl->m_started || pl->m_levelEndAnimationStarted
        || pl->m_resumeTimer > 0.0f) {
        return false;
    }

    // Hold to step: after m_holdDelay, repeat one frame every m_holdSpeed ticks.
    //
    // Hold detection. The keybind ALWAYS holds: press gives the configured step
    // count and keeping the key down repeats, no matter what the Hold To Step
    // switch says. The menu button needs BOTH signals and the switch:
    // m_holdActive says the STEP button itself is down, and the OS probe confirms
    // it inside the same tick (the menu is drawn after the scheduler, so the flag
    // alone overshoots by one step on release). The probe reads the global
    // VK_LBUTTON state and cannot tell WHERE the click happened, so the flag is
    // not optional вЂ” trusting the probe alone made any left click in the game
    // start hold-stepping.
    const bool mouseDown = m_holdProbe ? m_holdProbe() : false;
    const bool menuHold = m_holdEnabled && m_holdActive && (mouseDown || !m_holdProbe);
    const bool engaged = m_keyHeld || menuHold;

    if (engaged) {
        m_holdTimer += dt;
        if (m_holdTimer >= m_holdDelay && m_pendingClick > 0) {
            // Past the delay with the key/button still down, this is a HOLD and
            // not a tap: drop the discrete backlog. Without this, holding first
            // lurched forward by the entire tap count and only then settled into
            // the smooth repeat вЂ” the "activation as if it were a single click"
            // that made a hold jump by N frames before it started stepping.
            // A genuine tap releases well before m_holdDelay, so it still pays
            // its whole count.
            m_pendingClick = 0;
        }
    } else {
        m_holdTimer = 0.0f;
        m_holdTickCounter = 0;
    }

    // One tick is released per scheduler call; everything else stays frozen.
    // Exactly one fixed-size substep is integrated per pass, because
    // PhysicsControlLayer clamps to `setExpectedTicks(1)` while tickStepping.
    //
    // The step size must match what PLAYBACK advances for the same tick, and
    // both playback paths already fold in the level's time warp:
    //   physicsbypass.cpp:500-501  timestep = clamp(m_timeWarp, 0, 1) * fixedDelta
    //   physicsbypass.cpp:780-781  same, in the getModifiedDelta hook
    //   GDH replay.cpp:291         timestep = min(m_timeWarp, 1.0) * fixed_dt
    // Using a bare fixedSimulationDelta() here skipped the warp factor, so on a
    // slowed-down level (timeWarp < 1) a manual step advanced more game time than
    // the replayed tick вЂ” the frame you inspect under the stepper was not the
    // frame that actually gets played. TimeWarp above 1 is deliberately NOT
    // scaled, exactly as in GDH: the fast levels multiply internally.
    auto* engine = ReplayEngine::get();
    float stepDt = dt;
    if (engine) {
        float warped = engine->fixedSimulationDelta();
        if (auto* pl2 = PlayLayer::get()) {
            const double warp =
                std::clamp(static_cast<double>(pl2->m_gameState.m_timeWarp), 0.0, 1.0);
            const float scaled =
                static_cast<float>(warp * static_cast<double>(engine->fixedSimulationDelta()));
            if (scaled > 0.0f && std::isfinite(scaled)) {
                warped = scaled;
            }
            // One-shot per warp value: confirms the factor the stepper actually
            // integrates is the level's, and that a slow level really shrinks it.
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

    // A key press is not acted on until it is known to be a tap or a hold. Inside
    // the window nothing steps at all вЂ” that is the wait the user asked for, and
    // it is why holding no longer starts with a lurch by the tap count.
    if (m_keyTapPending > 0) {
        if (engaged) {
            if (m_holdTimer >= m_holdDelay) {
                // Committed to a hold: drop the tap, the repeat takes over.
                m_keyTapPending = 0;
            }
            return true;  // still inside the window - stay frozen
        }
        // Released before the delay -> a tap. Pay the whole count now.
        const int count = m_keyTapPending;
        m_keyTapPending = 0;
        m_nextFrame = true;
        m_pendingClick = count - 1;
    }

    // 1) The immediate frame armed by the press itself (tap mode only).
    if (m_nextFrame) {
        m_nextFrame = false;
        pl->m_resumeTimer = 0.0f;
        scheduler->cocos2d::CCScheduler::update(stepDt);
        return true;
    }

    // 2) The rest of the frames the press OWED us. Drained FIRST and never
    //    cancelled: a tap always delivers the configured step count, and in tap
    //    mode holding the button/key cannot queue anything more.
    if (m_pendingClick > 0) {
        m_pendingClick--;
        pl->m_resumeTimer = 0.0f;
        scheduler->cocos2d::CCScheduler::update(stepDt);
        return true;
    }

    // 3) The tap is fully paid off. Only a HELD key/mouse turns into a repeat,
    //    and only in hold mode. Releasing stops it on the very next tick because
    //    engaged goes false and nothing is left in the queue.
    if (engaged && m_holdTimer >= m_holdDelay) {
        // Saturate instead of resetting: resetting re-armed the full
        // m_holdDelay before EVERY repeat, so the real step period was
        // m_holdDelay * m_holdSpeed (0.25s * 3 = 0.75s per frame вЂ” the "moves too
        // slowly" the hold showed).
        m_holdTimer = m_holdDelay;
        if (++m_holdTickCounter >= m_holdSpeed) {
            m_holdTickCounter = 0;
            m_nextFrame = true;
        }
    }

    // Frozen state: do not advance the scheduler.
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

// ---------------------------------------------------------------------------
// ReplayEngine timing and audio helpers
// ---------------------------------------------------------------------------

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
