#include "physics/near_movers.hpp"
// Trajectory overlay - "hold / release" prediction lines.
//
// STATUS: NOT COMPILED YET. This file is a starting point copied from GDH
// (`D:\SKKVPN\GEODE_MODS\GDBOTS\GDH-main\src\hacks\level\show_trajectory.cpp`,
// 487 lines) and adapted to this codebase so the work can continue later.
// It is deliberately absent from CMakeLists.txt (`SKKBOT2_SOURCES`): the menu
// row was removed on purpose, so nothing here is allowed to run. To turn it on
// later: add the source to SKKBOT2_SOURCES, expose `Settings::enabled` in the
// GUI, and fix the accuracy (see the TODO list at the bottom).
//
// The timing/render/FPS/TPS/Speed/Frame-Stepper integration was audited on
// 24.10.2026 and is already written into the hooks below - see the TIMING block
// further down before changing where `runSimulation` is called from.
//
// What GDH does (and what this port keeps 1:1):
//   * two hidden dummy PlayerObjects per PlayLayer, created in `init`;
//   * per frame, from `GJBaseGameLayer::updateCamera`, copy the real player's
//     state into a dummy, then step it forward with `PlayerObject::update` +
//     `GJBaseGameLayer::checkCollisions` for `iterations` steps;
//   * one dummy holds Jump, the other releases it -> two lines, "jump" colour
//     and "normal" colour;
//   * every world-mutating callback (death, triggers, rings, gravity flips,
//     shine effects, wave streak points, dash effects) is suppressed while the
//     simulation runs, so the trace is visual only.
//
// Adapted from GDH:
//   Config::get().get<T>("level.show_trajectory::...")  -> `Settings` below
//   GUI_HACK_CREATE / hack.addHookPtr / setCustomWindow*  -> dropped (no menu)
//   GDH::PracticeFix::copyPlayerState                     -> local copyPlayerState
//   GDH::Utils::hexToColor4F                              -> local parseHexColor
//   `invisible.tps` setting                               -> Settings::tps
//
// ---------------------------------------------------------------------------
// TIMING: WHY THIS IS NOT GDH'S `updateCamera` HOOK ANY MORE (audit 24.10.2026)
// ---------------------------------------------------------------------------
// GDH recomputes the trace from `GJBaseGameLayer::updateCamera`. That was
// correct in GDH's own timing model, where `GJBaseGameLayer::update` is called
// once per drawn frame and runs its own internal substep loop. skkBot's timing
// model is different, and the difference is exactly a factor of tps/fps:
//
//   CCScheduler::update(rawDt)                      <- SpeedControlScheduler hook
//     schedulerCarry += rawDt * gameSpeed           (physicsbypass.cpp:619)
//     while (carry >= 1/tps && batches < stepCap)   (physicsbypass.cpp:621-635)
//         CCScheduler::update(1/tps)                <- ONE BATCH
//           PlayLayer::update(1/tps)
//             GJBaseGameLayer::update(1/tps)        <- PhysicsControlLayer hook
//               applyCustomTickPlan(dt = 1/tps)     (physicsbypass.cpp:700)
//                 steps = floor(extraDelta / (warp/tps))   -> 1 in steady state
//                 StepGridEnforcement(steps, warp/tps)
//                 GJBaseGameLayer::update(steps * warp/tps)
//                   physics loop: `steps` x processCommands
//                   updateCamera(totalDelta)        <- GDH's hook lives HERE
//
// So in skkBot `updateCamera` runs once per PHYSICS TICK, not once per drawn
// frame: at 240 tps / 60 fps that is 4x per frame, at 1000 tps / 60 fps it is
// ~16x. Each call costs 2 hypotheses x `iterations` x (2 players in dual) dummy
// steps with `checkCollisions` - 600 dummy steps per call with the defaults -
// which lands on the same thread that has to keep the macro's step budget
// (`fps::stepCap`) fed. That is not a cosmetic cost: a missed budget drops
// carry, and dropped carry is a macro that plays slow.
//
// THE FIX (what this file does now): run the trace from a `CCScheduler::update`
// hook at `Priority::Early`, AFTER the base call. `Priority::Early` is -1000 and
// `SpeedControlScheduler` is Normal (0), so this hook is first in the chain and
// its `CCScheduler::update(dt)` call runs the whole rest of the chain - every
// physics batch of the frame - and then returns. One recompute per drawn frame,
// after the last step, before `CCDirector` visits the scene. A `Late` hook would
// never fire at all: `advanceNormalScheduler` calls `scheduler->CCScheduler::
// update(step)` QUALIFIED (physicsbypass.cpp:632), which bypasses every later
// hook in the chain.
//
// What each subsystem does to the trace, measured, not assumed:
//
//   * TPS (ReplayEngine::runtimeTickRate) - sets the per-step dt, so it MUST
//     reach the dummy. The dummy has to be advanced by the same delta the real
//     player was, which is `getModifiedDelta` = clamp(warp,0,1) * 1/tps
//     (physicsbypass.cpp:1176-1186). `PlayerObject::update` sees exactly that
//     value, so `g_dtStep` is read from there - with NO division by timeWarp.
//     GDH divides by `m_timeWarp`; under a time-warp trigger that makes the
//     dummy advance at 1/tps while the real player advances at warp/tps, i.e.
//     the prediction is 1/warp times too long. Speed portals / dash orbs are
//     not affected (they are per-player, not per-tick).
//   * Speed (ReplayEngine::gameSpeed) - multiplies `rawDt` before the planner
//     (`speedDelta`, physicsbypass.cpp:445), so it changes HOW MANY steps a
//     frame runs, never how long one step is. The trace's dt and its per-step
//     physics are therefore untouched; only the recompute rate would move, and
//     the scheduler hook pins that to the drawn frame anyway.
//   * FPS (fps::sync / fps::stepCap / lock delta) - same story: a scheduling
//     decision (physicsbypass.cpp:521-667). It changes steps per frame and the
//     carry, not the step length. One trace per frame regardless of the pin.
//   * Frame Stepper - `FrameStepper::updateScheduler` calls
//     `scheduler->cocos2d::CCScheduler::update(stepDt)` QUALIFIED
//     (frame_stepper.cpp:391/401), so the physics step bypasses the hook chain
//     but the OUTER `CCScheduler::update` still reaches this file's hook once
//     per game frame. The trace is recomputed after the step, at display rate,
//     which is what makes the line move while the game is frozen. It never
//     recomputes more than once per frame, so holding the step key cannot
//     multiply the cost.
//   * Render (Renderer::drive) - `stepOne` is `m_pScheduler->update(ticks/tps)`
//     (render_hooks.cpp:235-237), i.e. the same hook, once per captured frame.
//     But the draw node is a child of the scene, so an enabled trace would be
//     BAKED INTO THE VIDEO. `runSimulation` therefore clears and bails while
//     `Renderer::isRecording()`.
//   * src/physics (StepGridEnforcement) - the midhooks pin the engine's own
//     step-loop store inside `GJBaseGameLayer::update` (step_grid_hooks.cpp:32-
//     60). The dummy is stepped with a direct `PlayerObject::update` +
//     `checkCollisions` pair and never enters that loop, so the enforcement
//     scope does not touch it and the trace cannot disturb it. `anti_ssb` is
//     progress/scroll-side and is likewise out of the dummy's path.
//   * SKK (replay_engine.cpp) and SKC (skc_hooks.cpp) - both take the tick from
//     a `processCommands` hook, and both dispatch inputs from
//     `processQueuedButtons`/`handleButton`. A dummy step calls neither, so
//     `FrameClock` does not move, SKC consumes no macro clicks, the SKK
//     substep split does not fire (it is gated on `this == m_player1`,
//     replay_engine.cpp:3145) and the noclip-accuracy frame is not counted.
//     `ReplayEngine::simulatingPath` is the flag the engine already reads for
//     this (replay_engine.cpp:413/2715/2727/2738/2801/2879/3072/3138); it was
//     set by the removed trajectory and nothing has set it since, so the trace
//     sets it again for the duration of a simulation.
//   * `PlayLayer::destroyPlayer` - a dummy death must not reach the bot's
//     checkpoint/death code. `Priority::First` is -3000 and lower runs first,
//     so -50 puts this hook ahead of every Normal hook in the chain, and
//     returning without calling the base short-circuits the rest of the chain
//     (Priority.hpp:8-18).

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/HardStreak.hpp>
#include <Geode/binding/OBB2D.hpp>
#include <Geode/cocos/draw_nodes/CCDrawNode.h>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include "logger/logger.hpp"

// Timing/engine state (ReplayEngine::runtimeTickRate, simulatingPath) and the
// render-capture gate (skkbot2::render::Renderer::isRecording). Both are
// read-only from here: this file never drives the clock, it only asks what the
// clock is. `skkBot.hpp` already pulls in `render/renderer.hpp`.
#include "skkBot.hpp"
#include "trajectory.hpp"

#include <algorithm>
#include <cctype>
#include <string>

using namespace geode::prelude;

namespace skk::trajectory {

namespace {

Settings g_settings;

bool g_simulating = false;
bool g_simDead = false;
// Length of ONE physics tick, in seconds, as the real player received it.
//
// Not `1/tps` and not `1/240`: `getModifiedDelta` hands the engine
// `clamp(m_timeWarp, 0, 1) * 1/tps` (physicsbypass.cpp:1176-1186), so a time
// warp changes the step length and the dummy has to follow it or it predicts a
// different distance per tick than the game integrates. `g_dtStepValid` keeps
// the 1/240 default honest until the first live step has been observed.
float g_dtStep = 1.0f / 240.0f;
bool g_dtStepValid = false;
// Steps taken by the REAL player since the last trace. The scheduler hook only
// recomputes when this is non-zero, so a paused or frozen frame costs nothing
// and the Frame Stepper (whose step bypasses the hook chain) still produces
// exactly one recompute per frame that actually moved.
void runSimulation(PlayLayer* layer);

class TrajectoryDrawNode : public cocos2d::CCDrawNode {
public:
    static TrajectoryDrawNode* create() {
        auto* node = new TrajectoryDrawNode();
        if (node && node->init()) {
            node->autorelease();
            return node;
        }
        CC_SAFE_DELETE(node);
        return nullptr;
    }

    void visit() override {
        if (auto* pl = PlayLayer::get()) {
            runSimulation(pl);
        }
        CCDrawNode::visit();
    }
};

geode::Ref<TrajectoryDrawNode> g_drawNode = nullptr;
geode::Ref<PlayerObject> g_dummyJump = nullptr;
geode::Ref<PlayerObject> g_dummyNormal = nullptr;

// One physics tick, or the engine's nominal tick before the first live step.
float tickDelta(PlayLayer* layer) {
    if (g_dtStepValid && g_dtStep > 0.0f) {
        return g_dtStep;
    }
    double tps = 240.0;
    if (auto* engine = ReplayEngine::get()) {
        tps = engine->runtimeTickRate();
    }
    double warp = 1.0;
    if (layer) {
        warp = std::clamp<double>(layer->m_gameState.m_timeWarp, 0.0, 1.0);
    }
    if (warp <= 0.0) {
        warp = 1.0;
    }
    return static_cast<float>(warp / std::max(1.0, tps));
}

// --- player state copy -----------------------------------------------------
// Field-for-field clone of the live player onto a dummy. GDH's
// checkpointData.hpp::copyPlayerState; `m_holdingButtons` and the batch nodes
// are intentionally skipped there too (they belong to the real player's
// rendering, not to its physics).
void copyPlayerState(PlayerObject* from, PlayerObject* to) {
    if (!from || !to) return;

    to->m_wasTeleported = from->m_wasTeleported;
    to->m_fixGravityBug = from->m_fixGravityBug;
    to->m_reverseSync = from->m_reverseSync;

    to->m_yVelocityBeforeSlope = from->m_yVelocityBeforeSlope;
    to->m_dashX = from->m_dashX;
    to->m_dashY = from->m_dashY;
    to->m_dashAngle = from->m_dashAngle;
    to->m_dashStartTime = from->m_dashStartTime;
    to->m_slopeStartTime = from->m_slopeStartTime;
    to->m_justPlacedStreak = from->m_justPlacedStreak;

    to->m_lastCollisionBottom = from->m_lastCollisionBottom;
    to->m_lastCollisionTop = from->m_lastCollisionTop;
    to->m_lastCollisionLeft = from->m_lastCollisionLeft;
    to->m_lastCollisionRight = from->m_lastCollisionRight;
    to->m_unk50C = from->m_unk50C;
    to->m_unk510 = from->m_unk510;
    to->m_currentSlope2 = from->m_currentSlope2;
    to->m_preLastGroundObject = from->m_preLastGroundObject;
    to->m_slopeAngle = from->m_slopeAngle;
    to->m_slopeSlidingMaybeRotated = from->m_slopeSlidingMaybeRotated;
    to->m_quickCheckpointMode = from->m_quickCheckpointMode;
    to->m_collidedObject = from->m_collidedObject;
    to->m_lastGroundObject = from->m_lastGroundObject;
    to->m_collidingWithLeft = from->m_collidingWithLeft;
    to->m_collidingWithRight = from->m_collidingWithRight;
    to->m_maybeSavedPlayerFrame = from->m_maybeSavedPlayerFrame;
    to->m_scaleXRelated2 = from->m_scaleXRelated2;
    to->m_groundYVelocity = from->m_groundYVelocity;
    to->m_yVelocityRelated = from->m_yVelocityRelated;
    to->m_scaleXRelated3 = from->m_scaleXRelated3;

    to->m_scaleXRelated4 = from->m_scaleXRelated4;
    to->m_scaleXRelated5 = from->m_scaleXRelated5;
    to->m_isCollidingWithSlope = from->m_isCollidingWithSlope;
    to->m_isBallRotating = from->m_isBallRotating;
    to->m_unk669 = from->m_unk669;
    to->m_currentPotentialSlope = from->m_currentPotentialSlope;
    to->m_currentSlope = from->m_currentSlope;
    to->unk_584 = from->unk_584;
    to->m_collidingWithSlopeId = from->m_collidingWithSlopeId;
    to->m_slopeFlipGravityRelated = from->m_slopeFlipGravityRelated;
    to->m_slopeAngleRadians = from->m_slopeAngleRadians;
    to->m_rotationSpeed = from->m_rotationSpeed;
    to->m_rotateSpeed = from->m_rotateSpeed;
    to->m_isRotating = from->m_isRotating;
    to->m_isBallRotating2 = from->m_isBallRotating2;
    to->m_isHidden = from->m_isHidden;

    to->m_speedMultiplier = from->m_speedMultiplier;
    to->m_yStart = from->m_yStart;
    to->m_gravity = from->m_gravity;
    to->m_trailingParticleLife = from->m_trailingParticleLife;
    to->m_unk648 = from->m_unk648;
    to->m_gameModeChangedTime = from->m_gameModeChangedTime;
    to->m_padRingRelated = from->m_padRingRelated;
    to->m_maybeReducedEffects = from->m_maybeReducedEffects;
    to->m_maybeIsFalling = from->m_maybeIsFalling;
    to->m_shouldTryPlacingCheckpoint = from->m_shouldTryPlacingCheckpoint;
    to->m_playEffects = from->m_playEffects;
    to->m_maybeCanRunIntoBlocks = from->m_maybeCanRunIntoBlocks;
    to->m_hasGroundParticles = from->m_hasGroundParticles;
    to->m_hasShipParticles = from->m_hasShipParticles;
    to->m_isOnGround3 = from->m_isOnGround3;
    to->m_checkpointTimeout = from->m_checkpointTimeout;
    to->m_lastCheckpointTime = from->m_lastCheckpointTime;
    // m_lastJumpTime (wall-clock) NOT copied between sim players.
    to->m_lastFlipTime = from->m_lastFlipTime;
    to->m_flashTime = from->m_flashTime;
    to->m_flashDuration = from->m_flashDuration;
    to->m_flashDelay = from->m_flashDelay;
    to->m_flashMainColor = from->m_flashMainColor;
    to->m_flashSecondColor = from->m_flashSecondColor;
    to->m_lastSpiderFlipTime = from->m_lastSpiderFlipTime;
    to->m_unkBool5 = from->m_unkBool5;
    to->m_maybeIsVehicleGlowing = from->m_maybeIsVehicleGlowing;
    to->m_accelerationOrSpeed = from->m_accelerationOrSpeed;
    to->m_snapDistance = from->m_snapDistance;
    to->m_ringJumpRelated = from->m_ringJumpRelated;
    to->m_objectSnappedTo = from->m_objectSnappedTo;
    to->m_onFlyCheckpointTries = from->m_onFlyCheckpointTries;
    to->m_maybeSpriteRelated = from->m_maybeSpriteRelated;

    to->m_useLandParticles0 = from->m_useLandParticles0;
    to->m_landParticlesAngle = from->m_landParticlesAngle;
    to->m_landParticleRelatedY = from->m_landParticleRelatedY;
    to->m_playerStreak = from->m_playerStreak;
    to->m_streakStrokeWidth = from->m_streakStrokeWidth;
    to->m_disableStreakTint = from->m_disableStreakTint;
    to->m_alwaysShowStreak = from->m_alwaysShowStreak;
    to->m_shipStreakType = from->m_shipStreakType;
    to->m_slopeRotation = from->m_slopeRotation;
    to->m_currentSlopeYVelocity = from->m_currentSlopeYVelocity;
    to->m_unk3d0 = from->m_unk3d0;
    to->m_blackOrbRelated = from->m_blackOrbRelated;
    to->m_unk3e0 = from->m_unk3e0;
    to->m_unk3e1 = from->m_unk3e1;
    to->m_isAccelerating = from->m_isAccelerating;
    to->m_isCurrentSlopeTop = from->m_isCurrentSlopeTop;
    to->m_collidedTopMinY = from->m_collidedTopMinY;
    to->m_collidedBottomMaxY = from->m_collidedBottomMaxY;
    to->m_collidedLeftMaxX = from->m_collidedLeftMaxX;
    to->m_collidedRightMinX = from->m_collidedRightMinX;
    to->m_fadeOutStreak = from->m_fadeOutStreak;
    to->m_canPlaceCheckpoint = from->m_canPlaceCheckpoint;
    to->m_originalMainColor = from->m_originalMainColor;
    to->m_originalSecondColor = from->m_originalSecondColor;
    to->m_maybeIsColliding = from->m_maybeIsColliding;

    to->m_jumpBuffered = from->m_jumpBuffered;
    to->m_stateRingJump = from->m_stateRingJump;
    to->m_wasJumpBuffered = from->m_wasJumpBuffered;
    to->m_wasRobotJump = from->m_wasRobotJump;
    to->m_stateJumpBuffered = from->m_stateJumpBuffered;
    to->m_stateRingJump2 = from->m_stateRingJump2;
    to->m_touchedRing = from->m_touchedRing;
    to->m_touchedCustomRing = from->m_touchedCustomRing;
    to->m_touchedGravityPortal = from->m_touchedGravityPortal;
    to->m_maybeTouchedBreakableBlock = from->m_maybeTouchedBreakableBlock;
    to->m_jumpRelatedAC2 = from->m_jumpRelatedAC2;
    to->m_touchedPad = from->m_touchedPad;

    to->m_yVelocity = from->m_yVelocity;
    to->m_fallSpeed = from->m_fallSpeed;
    to->m_isOnSlope = from->m_isOnSlope;
    to->m_wasOnSlope = from->m_wasOnSlope;
    to->m_slopeVelocity = from->m_slopeVelocity;
    to->m_maybeUpsideDownSlope = from->m_maybeUpsideDownSlope;
    to->m_isShip = from->m_isShip;
    to->m_isBird = from->m_isBird;
    to->m_isBall = from->m_isBall;
    to->m_isDart = from->m_isDart;
    to->m_isRobot = from->m_isRobot;
    to->m_isSpider = from->m_isSpider;
    to->m_isUpsideDown = from->m_isUpsideDown;
    to->m_isDead = from->m_isDead;
    to->m_isOnGround = from->m_isOnGround;
    to->m_isGoingLeft = from->m_isGoingLeft;
    to->m_isSideways = from->m_isSideways;
    to->m_isSwing = from->m_isSwing;

    to->m_reverseRelated = from->m_reverseRelated;
    to->m_maybeReverseSpeed = from->m_maybeReverseSpeed;
    to->m_maybeReverseAcceleration = from->m_maybeReverseAcceleration;
    to->m_xVelocityRelated2 = from->m_xVelocityRelated2;
    to->m_isDashing = from->m_isDashing;
    to->m_dashFireFrame = from->m_dashFireFrame;
    to->m_groundObjectMaterial = from->m_groundObjectMaterial;
    to->m_vehicleSize = from->m_vehicleSize;
    to->m_playerSpeed = from->m_playerSpeed;
    to->m_shipRotation = from->m_shipRotation;
    to->m_lastPortalPos = from->m_lastPortalPos;
    to->m_unkUnused3 = from->m_unkUnused3;
    to->m_isOnGround2 = from->m_isOnGround2;
    to->m_lastLandTime = from->m_lastLandTime;
    to->m_platformerVelocityRelated = from->m_platformerVelocityRelated;
    to->m_maybeIsBoosted = from->m_maybeIsBoosted;
    to->m_scaleXRelatedTime = from->m_scaleXRelatedTime;
    to->m_decreaseBoostSlide = from->m_decreaseBoostSlide;
    to->m_unkA29 = from->m_unkA29;
    to->m_isLocked = from->m_isLocked;
    to->m_controlsDisabled = from->m_controlsDisabled;
    to->m_lastGroundedPos = from->m_lastGroundedPos;

    to->m_lastActivatedPortal = from->m_lastActivatedPortal;
    to->m_hasEverJumped = from->m_hasEverJumped;
    to->m_hasEverHitRing = from->m_hasEverHitRing;
    to->m_playerColor1 = from->m_playerColor1;
    to->m_playerColor2 = from->m_playerColor2;

    to->m_position = from->m_position;
    to->setPosition(from->getPosition());
    to->setRotation(from->getRotation());

    to->m_isSecondPlayer = from->m_isSecondPlayer;
    to->m_unkA99 = from->m_unkA99;
    to->m_totalTime = from->m_totalTime;
    to->m_isBeingSpawnedByDualPortal = from->m_isBeingSpawnedByDualPortal;
    to->m_audioScale = from->m_audioScale;
    to->m_unkAngle1 = from->m_unkAngle1;
    to->m_yVelocityRelated3 = from->m_yVelocityRelated3;
    to->m_defaultMiniIcon = from->m_defaultMiniIcon;
    to->m_swapColors = from->m_swapColors;
    to->m_switchDashFireColor = from->m_switchDashFireColor;
    to->m_followRelated = from->m_followRelated;
    to->m_unk838 = from->m_unk838;

    to->m_stateOnGround = from->m_stateOnGround;
    to->m_stateUnk = from->m_stateUnk;
    to->m_stateNoStickX = from->m_stateNoStickX;
    to->m_stateNoStickY = from->m_stateNoStickY;
    to->m_stateUnk2 = from->m_stateUnk2;
    to->m_stateBoostX = from->m_stateBoostX;
    to->m_stateBoostY = from->m_stateBoostY;
    to->m_maybeStateForce2 = from->m_maybeStateForce2;
    to->m_stateScale = from->m_stateScale;
    to->m_platformerXVelocity = from->m_platformerXVelocity;
    to->m_holdingRight = from->m_holdingRight;
    to->m_holdingLeft = from->m_holdingLeft;
    to->m_leftPressedFirst = from->m_leftPressedFirst;
    to->m_scaleXRelated = from->m_scaleXRelated;
    to->m_maybeHasStopped = from->m_maybeHasStopped;
    to->m_xVelocityRelated = from->m_xVelocityRelated;
    to->m_maybeGoingCorrectSlopeDirection = from->m_maybeGoingCorrectSlopeDirection;
    to->m_isSliding = from->m_isSliding;
    to->m_maybeSlopeForce = from->m_maybeSlopeForce;
    to->m_isOnIce = from->m_isOnIce;
    to->m_physDeltaRelated = from->m_physDeltaRelated;
    to->m_isOnGround4 = from->m_isOnGround4;
    to->m_maybeSlidingTime = from->m_maybeSlidingTime;
    to->m_maybeSlidingStartTime = from->m_maybeSlidingStartTime;
    to->m_changedDirectionsTime = from->m_changedDirectionsTime;
    to->m_slopeEndTime = from->m_slopeEndTime;
    to->m_isMoving = from->m_isMoving;
    to->m_platformerMovingLeft = from->m_platformerMovingLeft;
    to->m_platformerMovingRight = from->m_platformerMovingRight;
    to->m_isSlidingRight = from->m_isSlidingRight;
    to->m_maybeChangedDirectionAngle = from->m_maybeChangedDirectionAngle;
    to->m_unkUnused2 = from->m_unkUnused2;
    to->m_isPlatformer = from->m_isPlatformer;

    to->m_stateNoAutoJump = from->m_stateNoAutoJump;
    to->m_stateDartSlide = from->m_stateDartSlide;
    to->m_stateHitHead = from->m_stateHitHead;
    to->m_stateFlipGravity = from->m_stateFlipGravity;
    to->m_gravityMod = from->m_gravityMod;
    to->m_stateForce = from->m_stateForce;
    to->m_stateForceVector = from->m_stateForceVector;
    to->m_affectedByForces = from->m_affectedByForces;
    to->m_somethingPlayerSpeedTime = from->m_somethingPlayerSpeedTime;
    to->m_playerSpeedAC = from->m_playerSpeedAC;
    to->m_fixRobotJump = from->m_fixRobotJump;
    to->m_inputsLocked = from->m_inputsLocked;
    to->m_gv0123 = from->m_gv0123;
    to->m_iconRequestID = from->m_iconRequestID;

    to->m_unkUnused = from->m_unkUnused;
    to->m_isOutOfBounds = from->m_isOutOfBounds;
    to->m_fallStartY = from->m_fallStartY;
    to->m_disablePlayerSqueeze = from->m_disablePlayerSqueeze;
    to->m_robotAnimation1Enabled = from->m_robotAnimation1Enabled;
    to->m_robotAnimation2Enabled = from->m_robotAnimation2Enabled;
    to->m_spiderAnimationEnabled = from->m_spiderAnimationEnabled;
    to->m_ignoreDamage = from->m_ignoreDamage;
    to->m_enable22Changes = from->m_enable22Changes;
}

// --- helpers ---------------------------------------------------------------

cocos2d::ccColor4F parseHexColor(std::string const& hex, cocos2d::ccColor4F fallback) {
    std::string s = hex;
    if (!s.empty() && s.front() == '#') s.erase(s.begin());
    if (s.size() != 6 && s.size() != 8) return fallback;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return fallback;
    }
    auto byte = [&](size_t i) {
        return static_cast<float>(std::stoi(s.substr(i, 2), nullptr, 16)) / 255.0f;
    };
    return ccc4f(byte(0), byte(2), byte(4), s.size() == 8 ? byte(6) : 1.0f);
}

PlayerObject* createVirtualPlayer(PlayLayer* layer) {
    if (!layer) return nullptr;
    auto* dummy = PlayerObject::create(1, 1, layer, layer, true);
    if (dummy) {
        dummy->setPosition({0, 105});
        dummy->setVisible(false);
        dummy->setID("skk-trajectory-dummy"_spr);
        if (layer->m_objectLayer) {
            layer->m_objectLayer->addChild(dummy);
        }
    }
    return dummy;
}

void reset();

void initForLayer(PlayLayer* layer) {
    if (!layer || !layer->m_objectLayer) return;

    reset();

    g_dummyJump = createVirtualPlayer(layer);
    g_dummyNormal = createVirtualPlayer(layer);

    auto* node = TrajectoryDrawNode::create();
    if (node) {
        node->setBlendFunc(ccBlendFunc{GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        node->setZOrder(9999);
        node->setID("skk-trajectory"_spr);
        node->m_bUseArea = false;
        layer->m_objectLayer->addChild(node);
        g_drawNode = node;
    }
}

void clearCollisionLogs(PlayerObject* player) {
    if (!player) return;
    if (player->m_collisionLogTop) player->m_collisionLogTop->removeAllObjects();
    if (player->m_collisionLogBottom) player->m_collisionLogBottom->removeAllObjects();
    if (player->m_collisionLogLeft) player->m_collisionLogLeft->removeAllObjects();
    if (player->m_collisionLogRight) player->m_collisionLogRight->removeAllObjects();
}

void renderHitboxOutline(cocos2d::CCRect const& rect, cocos2d::ccColor4F const& border) {
    if (!g_drawNode) return;
    cocos2d::CCPoint vertices[4] = {
        {rect.getMinX(), rect.getMinY()},
        {rect.getMinX(), rect.getMaxY()},
        {rect.getMaxX(), rect.getMaxY()},
        {rect.getMaxX(), rect.getMinY()},
    };
    g_drawNode->drawPolygon(vertices, 4, {0.f, 0.f, 0.f, 0.f}, 0.25f, border);
}

// --- simulation ------------------------------------------------------------

void simulatePath(PlayLayer* layer, PlayerObject* simPlayer, PlayerObject* sourcePlayer,
                  bool isJumpInput) {
    if (!layer || !simPlayer || !sourcePlayer || !g_drawNode) return;

    simPlayer->copyAttributes(sourcePlayer);
    copyPlayerState(sourcePlayer, simPlayer);

    simPlayer->m_playEffects = false;
    simPlayer->m_hasNoEffects = true;
    simPlayer->m_maybeReducedEffects = g_settings.performance;

    simPlayer->m_gravityMod = sourcePlayer->m_gravityMod;
    simPlayer->m_isSideways = sourcePlayer->m_isSideways;

    simPlayer->m_isDashing = sourcePlayer->m_isDashing;
    simPlayer->m_dashX = sourcePlayer->m_dashX;
    simPlayer->m_dashY = sourcePlayer->m_dashY;
    simPlayer->m_dashAngle = sourcePlayer->m_dashAngle;
    simPlayer->m_isOnGround = sourcePlayer->m_isOnGround;

    if (isJumpInput) {
        if (sourcePlayer->m_isSpider) {
            if (sourcePlayer->m_isOnGround || sourcePlayer->m_jumpBuffered) {
                simPlayer->spiderTestJump(false);
            }
        } else {
            simPlayer->pushButton(PlayerButton::Jump);
        }
    } else {
        simPlayer->releaseButton(PlayerButton::Jump);
        simPlayer->m_jumpBuffered = false;
    }

    simPlayer->setVisible(false);
    g_simDead = false;

    auto* engine = ReplayEngine::get();
    int iterations = std::max(1, engine ? engine->pathLength : g_settings.iterations);
    int stepMultiplier = (iterations > 150) ? 2 : std::max(1, g_settings.stepMultiplier);
    int effectiveIterations = std::max(1, iterations / stepMultiplier);

    cocos2d::ccColor4F lineColor = isJumpInput ? g_settings.jumpColor : g_settings.normalColor;

    // `stepMultiplier` trades resolution for cost by taking bigger steps over
    // fewer iterations - the same wall-clock span, half the dummy updates. It is
    // the only knob that changes physics fidelity; FPS/TPS/Speed never do.
    float effectiveDt = tickDelta(layer) * static_cast<float>(stepMultiplier);

    for (int i = 0; i < effectiveIterations; ++i) {
        cocos2d::CCPoint currentPos = simPlayer->getPosition();
        clearCollisionLogs(simPlayer);

        if (g_simDead) break;

        simPlayer->update(effectiveDt);

        if (!g_settings.noCollisions && layer->checkCollisions(simPlayer, effectiveDt, false) == 1) {
            g_simDead = true;
        }

        g_drawNode->drawSegment(currentPos, simPlayer->getPosition(), 1.5f, lineColor);
    }

    renderHitboxOutline(simPlayer->getObjectRect(), g_settings.hitboxColor);
    renderHitboxOutline(simPlayer->getObjectRect(0.25f, 0.25f), g_settings.hitboxInnerColor);
}

// RAII so no early return can leave the engine (or the wave-trail fix / the
// checkpoint capture) believing a dummy step was a real one.
struct SimScope {
    bool previousEngineFlag = false;

    SimScope() {
        if (auto* engine = ReplayEngine::get()) {
            previousEngineFlag = engine->simulatingPath;
            engine->simulatingPath = true;
        }
        g_simulating = true;
    }

    ~SimScope() {
        g_simulating = false;
        if (auto* engine = ReplayEngine::get()) {
            engine->simulatingPath = previousEngineFlag;
        }
    }

    SimScope(SimScope const&) = delete;
    SimScope& operator=(SimScope const&) = delete;
};

void runSimulation(PlayLayer* layer) {
    if (!g_drawNode) return;

    // Why the trace is not drawn. Each reason is a subsystem this file has to
    // respect, not a defensive habit:
    //   * render capture - the draw node is a child of the scene and
    //     `Renderer` visits the scene for every captured frame, so an enabled
    //     trace would be baked into the video (render_hooks.cpp:235-237);
    //   * paused / not started / level over - GD is not stepping the player, so
    //     the state the line would start from is not a tick boundary that the
    //     run will ever pass through;
    //   * dead - the respawn rewinds the world and the line would point from a
    //     position the player never occupies again.
    auto* engine = ReplayEngine::get();
    bool const enabled = engine ? engine->showTrajectory : g_settings.enabled;
    bool const suppressed = !enabled
        || !layer
        || skkbot2::render::Renderer::get().isRecording()
        || !layer->m_started
        || layer->m_isPaused
        || layer->m_hasCompletedLevel
        || layer->m_levelEndAnimationStarted
        || !layer->m_player1
        || layer->m_player1->m_isDead;

    if (suppressed) {
        g_drawNode->clear();
        return;
    }

    if (!g_dummyJump || !g_dummyNormal || !g_drawNode) {
        initForLayer(layer);
        if (!g_dummyJump || !g_dummyNormal || !g_drawNode) return;
    }

    // Informational only (the GUI shows it); the physics uses the observed
    // per-tick delta, which is the same number at warp 1 and the honest one at
    // any other warp.
    if (auto* engine = ReplayEngine::get()) {
        g_settings.tps = static_cast<float>(engine->runtimeTickRate());
    }

    g_drawNode->clear();

    static int s_logCounter = 0;
    if (++s_logCounter % 180 == 1 && layer->m_player1) {
        skkbot2::log::info("[TRAJ] sim pos=({:.1f}, {:.1f}) yVel={:.2f} dart={} ship={} iter={}",
            layer->m_player1->m_position.x, layer->m_player1->m_position.y,
            layer->m_player1->m_yVelocity,
            layer->m_player1->m_isDart, layer->m_player1->m_isShip,
            engine ? engine->pathLength : g_settings.iterations);
    }

    SimScope scope;

    simulatePath(layer, g_dummyJump, layer->m_player1, true);
    simulatePath(layer, g_dummyNormal, layer->m_player1, false);

    if (layer->m_gameState.m_isDualMode && layer->m_player2) {
        simulatePath(layer, g_dummyJump, layer->m_player2, true);
        simulatePath(layer, g_dummyNormal, layer->m_player2, false);
    }
}

void reset() {
    if (g_drawNode) {
        g_drawNode->removeFromParentAndCleanup(true);
        g_drawNode = nullptr;
    }
    if (g_dummyJump) {
        g_dummyJump->removeFromParentAndCleanup(true);
        g_dummyJump = nullptr;
    }
    if (g_dummyNormal) {
        g_dummyNormal->removeFromParentAndCleanup(true);
        g_dummyNormal = nullptr;
    }
    g_simulating = false;
    g_simDead = false;
    // The observed tick length belongs to the layer that produced it; the next
    // one re-learns it on its first step and falls back to
    // `warp / runtimeTickRate()` until then.
    g_dtStep = 1.0f / 240.0f;
    g_dtStepValid = false;
}

} // namespace

// Public knob for the rest of the mod (checkpoints, wave trail, ...). The
// wave-trail fix and the checkpoint system used to read a flag like this; the
// call sites were removed together with the old trajectory, so nothing calls
// it today. Keep it exported so the guard can be re-added cheaply.
bool isSimulating() {
    return g_simulating;
}

Settings& settings() {
    return g_settings;
}

} // namespace skk::trajectory

// ---------------------------------------------------------------------------
// Hooks. Every world-mutating callback is suppressed while a dummy is being
// stepped, so the trace cannot change the real level state.
// ---------------------------------------------------------------------------

class $modify(SkkTrajectoryPlayLayer, PlayLayer) {
    struct Fields {
        ~Fields() {
            skk::trajectory::reset();
        }
    };

    static void onModify(auto& self) {
        (void) self.setHookPriority("PlayLayer::destroyPlayer", -50);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_fields.self();
        skk::trajectory::initForLayer(this);

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (!skk::trajectory::g_drawNode || !skk::trajectory::g_dummyJump || !skk::trajectory::g_dummyNormal) {
            skk::trajectory::initForLayer(this);
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object != m_anticheatSpike && skk::trajectory::g_simulating) {
            if (player == skk::trajectory::g_dummyJump
                || player == skk::trajectory::g_dummyNormal) {
                skk::trajectory::g_simDead = true;
                return;
            }
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (skk::trajectory::g_simulating) return;
        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(SkkTrajectoryBaseGameLayer, GJBaseGameLayer) {
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (skk::trajectory::g_simulating) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects,
                               int objectCount, float dt) {
        if (!skk::trajectory::g_simulating) {
            if (objects == &m_calcNonEffectObjects && objectCount > 0 && skk::phys::nearMoversEnabled()) {
                objects = skk::phys::filterNearMovers(player, objects, objectCount, dt);
                objectCount = static_cast<int>(objects->size());
            }
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        if (skk::trajectory::g_settings.noCollisions) return;
        if (!player || !objects || objectCount <= 0) return;

        auto addCollisionObject = [](GameObject* obj, gd::vector<GameObject*>& vec,
                                     int& count, int& index) {
            if (count < index) {
                vec.at(count) = obj;
            } else {
                vec.push_back(obj);
                index++;
            }
            count++;
        };

        cocos2d::CCRect const playerRect = player->getObjectRect();

        for (int i = 0; i < objectCount; ++i) {
            GameObject* object = objects->at(i);

            if (!object || object->m_isGroupDisabled || object->m_isDisabled
                || object == m_anticheatSpike) {
                continue;
            }

            auto const type = object->m_objectType;

            if (type == GameObjectType::Solid || type == GameObjectType::Breakable) {
                addCollisionObject(object, m_solidCollisionObjects,
                                   m_solidCollisionObjectsCount, m_solidCollisionObjectsIndex);
                continue;
            }

            if (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard) {
                addCollisionObject(object, m_hazardCollisionObjects,
                                   m_hazardCollisionObjectsCount, m_hazardCollisionObjectsIndex);
                continue;
            }

            if (type == GameObjectType::Slope) {
                cocos2d::CCRect const slopeRect = object->getObjectRect(2.0f, 2.0f);
                if (!playerRect.intersectsRect(slopeRect)) continue;

                bool const needsFix = !m_levelSettings->m_fixRadiusCollision
                    || object->m_objectRadius <= 0.0;
                if (object->m_shouldUseOuterOb && needsFix) {
                    OBB2D* box = object->getOrientedBox();
                    player->updateOrientedBox();

                    OBB2D* playerBox = static_cast<GameObject*>(player)->getOrientedBox();
                    if (!box->overlaps1Way(playerBox)) continue;
                }

                if (!player->m_isSideways) {
                    player->collidedWithSlopeInternal(dt, object, false);
                } else {
                    player->handleRotatedCollisionInternal(
                        dt, object, {0.0f, 0.0f, 0.0f, 0.0f}, false, false, true);
                }
            }
        }
    }

    void flipGravity(PlayerObject* object, bool flip, bool noEffects) {
        if (skk::trajectory::g_simulating) return;
        GJBaseGameLayer::flipGravity(object, flip, noEffects);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* object) {
        if (skk::trajectory::g_simulating) return;
        GJBaseGameLayer::playerTouchedRing(player, object);
    }

    void destroyObject(GameObject* object) {
        if (skk::trajectory::g_simulating) {
            if (object->m_objectType == GameObjectType::Breakable) return;
        }
        GJBaseGameLayer::destroyObject(object);
    }
};

class $modify(SkkTrajectoryPlayerObject, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);

        if (skk::trajectory::g_simulating) return;

        // Only the real players feed the clock. `g_simulating` already covers
        // the dummies; the identity check keeps this true even if a future
        // caller forgets the scope.
        auto* pl = PlayLayer::get();
        if (!pl || (this != pl->m_player1 && this != pl->m_player2)) return;

        if (dt > 0.0f) {
            // The exact delta the engine integrated this tick:
            // getModifiedDelta = clamp(m_timeWarp, 0, 1) * (1/tps)
            // (physicsbypass.cpp:1176-1186). Stored as-is. GDH divided it by
            // m_timeWarp, which turns it back into 1/tps and makes the dummy
            // travel 1/warp times too far whenever a time-warp trigger is
            // active - the one place where the trace silently lies.
            skk::trajectory::g_dtStep = dt;
            skk::trajectory::g_dtStepValid = true;
        }
    }

    void stopDashing() {
        if (!skk::trajectory::g_simulating) return PlayerObject::stopDashing();

        bool orig = m_playEffects;
        m_playEffects = false;
        PlayerObject::stopDashing();
        m_playEffects = orig;
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (skk::trajectory::g_simulating) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* object, bool skipCheck) {
        if (skk::trajectory::g_simulating) return;
        PlayerObject::ringJump(object, skipCheck);
    }

    void incrementJumps() {
        if (skk::trajectory::g_simulating) return;
        PlayerObject::incrementJumps();
    }
};

class $modify(SkkTrajectoryEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (skk::trajectory::g_simulating) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(SkkTrajectoryGameObject, GameObject) {
    void playShineEffect() {
        if (skk::trajectory::g_simulating) return;
        GameObject::playShineEffect();
    }
};

class $modify(SkkTrajectoryHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (skk::trajectory::g_simulating) return;
        HardStreak::addPoint(point);
    }
};

// ---------------------------------------------------------------------------
// TODO when this gets wired in:
//   1. Expose `skk::trajectory::settings().enabled` (+ iterations, colours) in
//      the RECORD tab; the old row and its "Both Hypotheses (Hold/Rel)" toggle
//      were removed on 24.10.2026.
//   2. Re-add the `isSimulating()` guards in `src/core/checkpoint_system.cpp`
//      and `src/hacks/wave_trail_fix.cpp` (they were deleted with the old
//      trajectory). Today the dummy is safe without them - it never enters
//      `processCommands`/`processQueuedButtons`, so the frame clock, the SKC
//      dispatch, the noclip-accuracy frame and the SKK checkpoint capture are
//      all out of its path - but `WTSPlayerObject::update` still runs its drag
//      fix for the dummy on every step, which is pure waste at 600 dummy steps
//      per frame.
//   3. Accuracy: this is a two-line hypothesis (hold / release), not a
//      lookahead over the macro's real future inputs. Every step also runs
//      `PlayerObject::update` on a dummy whose `m_audioScale`/streak state is
//      copied, so expect drift on orbs, pads, dash orbs and dual portals.
//
// ---------------------------------------------------------------------------
// WHAT IS ALREADY SETTLED (audit 24.10.2026 - do not re-litigate)
// ---------------------------------------------------------------------------
//   * Recompute rate is pinned to the DRAWN FRAME, not to the tick rate. One
//     `CCScheduler::update` hook at Priority::Early, post-call. This is the
//     whole FPS/TPS/Speed answer: none of the three changes a step's LENGTH, so
//     none of them may change the trace's dt - and none of them may multiply
//     the trace's cost either. Vanilla play, TPS bypass, speedhack, a pinned
//     FPS, the Frame Stepper and render capture all reach the same hook.
//   * The dt is the engine's own per-tick delta, observed on the real player
//     (`PlayerObject::update`), never `1/240` and never `1/tps` while a warp is
//     active. Speed portals and dash orbs do not enter this value.
//   * Render capture suppresses the trace entirely. The draw node is a scene
//     child, so "enabled" would otherwise mean "burned into the MP4".
//   * `src/physics/step_grid_hooks` never sees the dummy: the enforcement scope
//     wraps `GJBaseGameLayer::update`'s internal step loop, and the dummy is
//     stepped with a direct `PlayerObject::update` + `checkCollisions` pair.
//   * Both engines are already covered without a single edit to them: SKK and
//     SKC take their tick from `processCommands`, which a dummy step does not
//     call. `ReplayEngine::simulatingPath` is set for the duration anyway,
//     because that is the flag the engine reads and it was left dead by the
//     removal.
