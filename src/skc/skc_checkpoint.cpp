#include "skc_checkpoint.hpp"

#include "skc_bot.hpp"
#include "skkBot.hpp"
#include "core/checkpoint_handler.hpp"
#include "core/game_object_physics_snapshot.hpp"
#include "core/rng_lock.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <logger/logger.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace skk::skc {

namespace {

void applyCollisionLogDict(
    cocos2d::CCDictionary* dict,
    const std::vector<std::pair<int, GameObject*>>& src) {
    if (!dict) return;
    dict->removeAllObjects();
    for (auto const& entry : src) {
        if (entry.second) dict->setObject(entry.second, entry.first);
    }
}

void bridgePlayerSeamState(PlayerObject& player,
                           const FullPlayerCheckpoint& snap) {
    player.setRotation(snap.p_rotation);
    player.m_rotationSpeed = snap.rotationSpeed;
    player.m_rotateSpeed = snap.rotateSpeed;
    player.m_isRotating = (snap.flags1 & (1ull << 9)) != 0;
    player.m_isBallRotating = (snap.flags1 & (1ull << 7)) != 0;
    player.m_isBallRotating2 = (snap.flags1 & (1ull << 10)) != 0;
    player.m_slopeRotation = snap.slopeRotation;
    player.m_shipRotation =
        cocos2d::CCPoint(snap.shipRotX, snap.shipRotY);
    player.m_unkAngle1 = snap.unkAngle1;

    player.m_slopeAngle = snap.slopeAngle;
    player.m_slopeAngleRadians = snap.slopeAngleRadians;
    player.m_slopeVelocity = snap.slopeVelocity;
    player.m_currentSlopeYVelocity = snap.currentSlopeYVelocity;
    player.m_isOnSlope = snap.isOnSlope;
    player.m_wasOnSlope = snap.wasOnSlope;
    player.m_slopeStartTime = snap.slopeStartTime;
    player.m_slopeEndTime = snap.slopeEndTime;
    player.m_maybeSlopeForce = snap.maybeSlopeForce;
    player.m_maybeChangedDirectionAngle = snap.maybeChangedDirectionAngle;
    player.m_changedDirectionsTime = snap.changedDirectionsTime;
    player.m_maybeSlidingTime = snap.maybeSlidingTime;
    player.m_maybeSlidingStartTime = snap.maybeSlidingStartTime;
    player.m_isSliding = (snap.flags1 & (1ull << 54)) != 0;
    player.m_isOnIce = (snap.flags1 & (1ull << 55)) != 0;
    player.m_isCollidingWithSlope = (snap.flags1 & (1ull << 6)) != 0;
    player.m_isCurrentSlopeTop = (snap.flags1 & (1ull << 24)) != 0;
    player.m_slopeSlidingMaybeRotated = (snap.flags1 & (1ull << 4)) != 0;
    player.m_slopeFlipGravityRelated = (snap.flags1 & (1ull << 39)) != 0;
    player.m_maybeUpsideDownSlope = snap.maybeUpsideDownSlope;
    player.m_maybeGoingCorrectSlopeDirection =
        (snap.flags1 & (1ull << 53)) != 0;
    player.m_collidingWithSlopeId = snap.collidingWithSlopeId;
    player.m_potentialSlopeMap = snap.potentialSlopeMap;
    player.m_rotateObjectsRelated = snap.rotateObjectsRelated;

    player.m_lastCollisionBottom = snap.lastCollisionBottom;
    player.m_lastCollisionTop = snap.lastCollisionTop;
    player.m_lastCollisionLeft = snap.lastCollisionLeft;
    player.m_lastCollisionRight = snap.lastCollisionRight;
    player.m_prevCollisionBottom = snap.prevCollisionBottom;
    player.m_prevCollisionTop = snap.prevCollisionTop;
    applyCollisionLogDict(player.m_collisionLogTop, snap.collisionLogTop);
    applyCollisionLogDict(player.m_collisionLogBottom, snap.collisionLogBottom);
    applyCollisionLogDict(player.m_collisionLogLeft, snap.collisionLogLeft);
    applyCollisionLogDict(player.m_collisionLogRight, snap.collisionLogRight);
    switch (snap.collisionLogTopAlias) {
        case 1: player.m_collisionLogTop = player.m_collisionLogBottom; break;
        case 2: player.m_collisionLogTop = player.m_collisionLogLeft; break;
        case 3: player.m_collisionLogTop = player.m_collisionLogRight; break;
        default: break;
    }
    player.m_unk50C = snap.unk50C;
    player.m_unk510 = snap.unk510;
    player.m_collidedTopMinY = snap.collidedTopMinY;
    player.m_collidedBottomMaxY = snap.collidedBottomMaxY;
    player.m_collidedLeftMaxX = snap.collidedLeftMaxX;
    player.m_collidedRightMinX = snap.collidedRightMinX;

    player.m_stateOnGround = snap.stateOnGround;
    player.m_stateBoostX = snap.stateBoostX;
    player.m_stateBoostY = snap.stateBoostY;
    player.m_maybeStateForce2 = snap.maybeStateForce2;
    player.m_stateScale = snap.stateScale;
    player.m_stateNoAutoJump = snap.stateNoAutoJump;
    player.m_stateDartSlide = snap.stateDartSlide;
    player.m_stateHitHead = snap.stateHitHead;
    player.m_stateFlipGravity = snap.stateFlipGravity;
    player.m_stateForce = snap.stateForce;
    player.m_stateForceVector = snap.stateForceVector;
    player.m_affectedByForces = (snap.flags1 & (1ull << 62)) != 0;
    player.m_maybeIsBoosted = (snap.flags1 & (1ull << 44)) != 0;
    player.m_decreaseBoostSlide = (snap.flags1 & (1ull << 45)) != 0;
    player.m_stateUnk = snap.stateUnk;
    player.m_stateNoStickX = snap.stateNoStickX;
    player.m_stateNoStickY = snap.stateNoStickY;
    player.m_stateUnk2 = snap.stateUnk2;

    player.m_jumpBuffered = (snap.flags1 & (1ull << 29)) != 0;
    player.m_wasJumpBuffered = (snap.flags1 & (1ull << 31)) != 0;
    player.m_stateJumpBuffered = snap.stateJumpBuffered;
    player.m_stateRingJump = (snap.flags1 & (1ull << 30)) != 0;
    player.m_stateRingJump2 = (snap.flags1 & (1ull << 33)) != 0;
    player.m_usedRingThisStep = snap.ringJumpRelated;
    player.m_touchedRing = (snap.flags1 & (1ull << 34)) != 0;
    player.m_touchedCustomRing = (snap.flags1 & (1ull << 35)) != 0;
    player.m_touchedPad = (snap.flags1 & (1ull << 38)) != 0;
    player.m_touchedGravityPortal = (snap.flags1 & (1ull << 36)) != 0;
    player.m_maybeTouchedBreakableBlock =
        (snap.flags1 & (1ull << 37)) != 0;

    player.m_isGoingLeft = (snap.flags2 & (1ull << 10)) != 0;
    player.m_isSideways = (snap.flags2 & (1ull << 11)) != 0;
    player.m_leftPressedFirst = snap.leftPressedFirst;
    player.m_holdingLeft = snap.holdingLeft;
    player.m_holdingRight = snap.holdingRight;
    player.m_lastGroundedPos =
        cocos2d::CCPoint(snap.lastGroundedX, snap.lastGroundedY);
    player.m_lastPortalPos =
        cocos2d::CCPoint(snap.lastPortalX, snap.lastPortalY);
    player.m_gravity = snap.gravity;
    player.m_gravityMod = snap.gravityMod;
    player.m_speedMultiplier = snap.speedMultiplier;
    player.m_playerSpeed = snap.playerSpeed;
    player.m_vehicleSize = snap.vehicleSize;
    player.m_physDeltaRelated = snap.physDeltaRelated;

    player.m_totalTime = snap.totalTime;
    player.m_gameModeChangedTime = snap.gameModeChangedTime;
    player.m_lastFlipTime = snap.lastFlipTime;
    player.m_lastLandTime = snap.lastLandTime;
    player.m_lastSpiderFlipTime = snap.lastSpiderFlipTime;
    player.m_flashTime = snap.flashTime;
    player.m_flashDuration = snap.flashDuration;
    player.m_flashDelay = snap.flashDelay;
    player.m_fallStartY = snap.fallStartY;
}

}

PracticeFix& PracticeFix::get() {
    static PracticeFix instance;
    return instance;
}

void PracticeFix::capture(uint32_t frame, PlayLayer* playLayer,
                          CheckpointObject* checkpoint, CheckpointStateBundle state) {
    if (!playLayer) return;

    auto* engine = ReplayEngine::get();

    SkcCheckpoint cp;
    cp.object = checkpoint;
    cp.frame = frame;
    cp.progressFrame =
        checkpoint ? checkpoint->m_gameState.m_currentProgress / 2 : 0;
    const double tickRate = engine ? engine->runtimeTickRate() : 240.0;
    const double levelTime =
        checkpoint ? static_cast<double>(checkpoint->m_gameState.m_levelTime)
                   : 0.0;
    cp.timeFrame = static_cast<uint32_t>(std::llround(levelTime * tickRate));
    cp.platformer = playLayer->m_levelSettings &&
                    playLayer->m_levelSettings->m_platformerMode;
    cp.state = std::move(state);

    skkbot2::log::info(
        "[SKC] practice snapshot via SKK CheckpointStateManager: frame={} "
        "progress={} timeFrame={} mode={} hasFull={} fullP2={} varianceSlots={}",
        cp.frame, cp.progressFrame, cp.timeFrame,
        cp.platformer ? "platformer" : "classic",
        cp.state.hasFullState, cp.state.hasFullPlayer2, cp.state.variance.size());
    ++m_seams;

    m_checkpoints.push_back(std::move(cp));
}

void PracticeFix::trackPlatformerCheckpoint(CheckpointObject* checkpoint) {
    if (!checkpoint) return;
    for (auto* saved : m_platformerCheckpoints)
        if (saved == checkpoint) return;
    checkpoint->retain();
    m_platformerCheckpoints.push_back(checkpoint);
}

CheckpointObject* PracticeFix::latestPlatformerCheckpoint() const {
    return m_platformerCheckpoints.empty() ? nullptr : m_platformerCheckpoints.back();
}

std::optional<uint32_t> PracticeFix::apply(
    PlayLayer* playLayer, CheckpointObject* checkpoint) {

    if (!playLayer || m_checkpoints.empty())
        return std::nullopt;

    const uint32_t targetProgress =
        checkpoint ? checkpoint->m_gameState.m_currentProgress / 2 : 0;
    auto* engine = ReplayEngine::get();
    const double tickRate = engine ? engine->runtimeTickRate() : 240.0;
    const double levelTime =
        checkpoint ? static_cast<double>(checkpoint->m_gameState.m_levelTime)
                   : 0.0;
    const uint32_t targetTime =
        static_cast<uint32_t>(std::llround(levelTime * tickRate));

    SkcCheckpoint* best = nullptr;
    for (auto& cp : m_checkpoints) {
        if (checkpoint && cp.object == checkpoint) {
            best = &cp;
        }
    }
    for (auto& cp : m_checkpoints) {
        if (checkpoint || best) break;
        const bool match = cp.platformer ? (cp.timeFrame == targetTime)
                                         : (cp.progressFrame == targetProgress);
        if (match) {
            best = &cp;
        }
    }
    if (!best) return std::nullopt;

    CheckpointStateBundle& state = best->state;

    PlayerObject* const p1 = playLayer->m_player1;

    struct Tracked {
        int currentStepBefore, currentStepStored;
        double extraDeltaBefore, extraDeltaStored;
        int unk32d0Before, unk32d0Stored;
        int unk32ecBefore, unk32ecStored;
        float unk3380Before, unk3380Stored;
        float rotBefore, rotStored;
        float rotSpeedBefore, rotSpeedStored;
        float unkAngle1Before, unkAngle1Stored;
        double slopeRotationBefore, slopeRotationStored;
        double slopeVelBefore, slopeVelStored;
        float rotateSpeedBefore, rotateSpeedStored;
        bool rotatingBefore, rotatingStored;
        float curSlopeYVelBefore, curSlopeYVelStored;
        float slopeAngleBefore, slopeAngleStored;
        int cwSlopeIdBefore, cwSlopeIdStored;
        int lastCBBefore, lastCBStored;
        int stateForceBefore, stateForceStored;
        bool jumpBufBefore, jumpBufStored;
        bool touchedPortalBefore, touchedPortalStored;
        bool slopeOnBefore, slopeOnStored;
        bool wasOnSlopeBefore, wasOnSlopeStored;
        int varianceSlotsBefore, varianceSlotsStored;
        double vyBefore, vyStored;
        double fallSpeedBefore, fallSpeedStored;
        double groundYVelBefore, groundYVelStored;
        double yVelBeforeSlopeBefore, yVelBeforeSlopeStored;
        double yVelRelatedBefore, yVelRelatedStored;
        double platXVelBefore, platXVelStored;
        bool onGround2Before, onGround2Stored;
        float playerVelXBefore, playerVelXStored;
        int prevCBBefore, prevCBStored;
        int prevCTBefore, prevCTStored;
        int logTopBefore, logTopStored;
        int logBottomBefore, logBottomStored;
        int logLeftBefore, logLeftStored;
        int logRightBefore, logRightStored;
        int logAliasBefore, logAliasStored;
        float timeWarpBefore, timeWarpStored;
        float queuedTimeWarpBefore, queuedTimeWarpStored;
        float timeWarpRelatedBefore, timeWarpRelatedStored;
        float timeModRelatedBefore, timeModRelatedStored;
        bool timeModRelated2Before, timeModRelated2Stored;
        bool gravityRelatedBefore, gravityRelatedStored;
        double attemptTimeBefore, attemptTimeStored;
    } tracked{};

    tracked.currentStepBefore = playLayer->m_currentStep;
    tracked.currentStepStored = state.currentStep;
    tracked.extraDeltaBefore = playLayer->m_extraDelta;
    tracked.extraDeltaStored = state.extraDelta;
    tracked.unk32d0Before = playLayer->m_unk32d0;
    tracked.unk32d0Stored = state.unk32d0;
    tracked.unk32ecBefore = playLayer->m_unk32ec;
    tracked.unk32ecStored = state.unk32ec;
    tracked.unk3380Before = playLayer->m_unk3380;
    tracked.unk3380Stored = state.unk3380;
    tracked.vyBefore = p1 ? static_cast<double>(p1->m_yVelocity) : 0.0;
    tracked.vyStored =
        state.hasFullPlayer ? state.fullPlayer1.yVelocity : 0.0;
    tracked.fallSpeedBefore =
        p1 ? static_cast<double>(p1->m_fallSpeed) : 0.0;
    tracked.fallSpeedStored =
        state.hasFullPlayer ? state.fullPlayer1.fallSpeed : 0.0;
    tracked.groundYVelBefore =
        p1 ? static_cast<double>(p1->m_groundYVelocity) : 0.0;
    tracked.groundYVelStored =
        state.hasFullPlayer ? state.fullPlayer1.groundYVelocity : 0.0;
    tracked.yVelBeforeSlopeBefore =
        p1 ? static_cast<double>(p1->m_yVelocityBeforeSlope) : 0.0;
    tracked.yVelBeforeSlopeStored =
        state.hasFullPlayer ? state.fullPlayer1.yVelocityBeforeSlope : 0.0;
    tracked.yVelRelatedBefore =
        p1 ? static_cast<double>(p1->m_yVelocityRelated) : 0.0;
    tracked.yVelRelatedStored =
        state.hasFullPlayer ? state.fullPlayer1.yVelocityRelated : 0.0;
    tracked.platXVelBefore =
        p1 ? static_cast<double>(p1->m_platformerXVelocity) : 0.0;
    tracked.platXVelStored =
        state.hasFullPlayer ? state.fullPlayer1.platformerXVelocity : 0.0;
    tracked.playerVelXBefore = p1 ? p1->m_xVelocityRelated : 0.0f;
    tracked.playerVelXStored =
        state.hasFullPlayer ? state.fullPlayer1.xVelocityRelated : 0.0f;
    tracked.onGround2Before = p1 ? p1->m_isOnGround2 : false;
    tracked.onGround2Stored =
        state.hasFullPlayer ? state.fullPlayer1.isOnGround2 : false;
    tracked.prevCBBefore = p1 ? p1->m_prevCollisionBottom : 0;
    tracked.prevCBStored = state.hasFullPlayer
                               ? state.fullPlayer1.prevCollisionBottom
                               : 0;
    tracked.prevCTBefore = p1 ? p1->m_prevCollisionTop : 0;
    tracked.prevCTStored =
        state.hasFullPlayer ? state.fullPlayer1.prevCollisionTop : 0;
    tracked.logTopBefore = p1 && p1->m_collisionLogTop
                               ? static_cast<int>(p1->m_collisionLogTop->count())
                               : 0;
    tracked.logTopStored = state.hasFullPlayer
                               ? static_cast<int>(
                                     state.fullPlayer1.collisionLogTop.size())
                               : 0;
    tracked.logBottomBefore =
        p1 && p1->m_collisionLogBottom
            ? static_cast<int>(p1->m_collisionLogBottom->count())
            : 0;
    tracked.logBottomStored =
        state.hasFullPlayer
            ? static_cast<int>(state.fullPlayer1.collisionLogBottom.size())
            : 0;
    tracked.logLeftBefore = p1 && p1->m_collisionLogLeft
                                ? static_cast<int>(p1->m_collisionLogLeft->count())
                                : 0;
    tracked.logLeftStored = state.hasFullPlayer
                                ? static_cast<int>(
                                      state.fullPlayer1.collisionLogLeft.size())
                                : 0;
    tracked.logRightBefore =
        p1 && p1->m_collisionLogRight
            ? static_cast<int>(p1->m_collisionLogRight->count())
            : 0;
    tracked.logRightStored =
        state.hasFullPlayer
            ? static_cast<int>(state.fullPlayer1.collisionLogRight.size())
            : 0;
    tracked.logAliasBefore =
        p1 ? (p1->m_collisionLogTop == p1->m_collisionLogBottom
                  ? 1
                  : (p1->m_collisionLogTop == p1->m_collisionLogLeft
                         ? 2
                         : (p1->m_collisionLogTop == p1->m_collisionLogRight
                                ? 3
                                : 0)))
           : 0;
    tracked.logAliasStored =
        state.hasFullPlayer ? state.fullPlayer1.collisionLogTopAlias : 0;
    tracked.timeWarpBefore = playLayer->m_gameState.m_timeWarp;
    tracked.timeWarpStored = state.timeWarp;
    tracked.queuedTimeWarpBefore = playLayer->m_gameState.m_queuedTimeWarp;
    tracked.queuedTimeWarpStored = state.queuedTimeWarp;
    tracked.timeWarpRelatedBefore = playLayer->m_gameState.m_timeWarpRelated;
    tracked.timeWarpRelatedStored = state.timeWarpRelated;
    tracked.timeModRelatedBefore = playLayer->m_gameState.m_timeModRelated;
    tracked.timeModRelatedStored = state.timeModRelated;
    tracked.timeModRelated2Before = playLayer->m_gameState.m_timeModRelated2;
    tracked.timeModRelated2Stored = state.timeModRelated2;
    tracked.gravityRelatedBefore = playLayer->m_gameState.m_gravityRelated;
    tracked.gravityRelatedStored = state.gravityRelated;
    if (auto* pl = PlayLayer::get()) {
        tracked.attemptTimeBefore = pl->m_attemptTime;
    }
    tracked.attemptTimeStored = state.attemptTime;
    tracked.rotBefore = p1 ? p1->getRotation() : 0.0f;
    tracked.rotStored =
        state.hasFullPlayer ? state.fullPlayer1.p_rotation : 0.0f;
    tracked.rotSpeedBefore = p1 ? p1->m_rotationSpeed : 0.0f;
    tracked.rotSpeedStored =
        state.hasFullPlayer ? state.fullPlayer1.rotationSpeed : 0.0f;
    tracked.unkAngle1Before = p1 ? p1->m_unkAngle1 : 0.0f;
    tracked.unkAngle1Stored =
        state.hasFullPlayer ? state.fullPlayer1.unkAngle1 : 0.0f;
    tracked.slopeRotationBefore = p1 ? p1->m_slopeRotation : 0.0;
    tracked.slopeRotationStored =
        state.hasFullPlayer ? state.fullPlayer1.slopeRotation : 0.0;
    tracked.varianceSlotsBefore = static_cast<int>(
        std::count_if(playLayer->m_varianceValues.begin(),
                      playLayer->m_varianceValues.end(),
                      [](float v) { return v != 0.0f; }));
    tracked.varianceSlotsStored = static_cast<int>(state.variance.size());

    playLayer->m_player1CollisionBlock = state.player1CollisionBlock;
    playLayer->m_player2CollisionBlock = state.player2CollisionBlock;
    playLayer->m_extraDelta = state.extraDelta;
    playLayer->m_unk32d0 = state.unk32d0;
    playLayer->m_unk32ec = state.unk32ec;
    playLayer->m_currentStep = state.currentStep;
    playLayer->m_unk3380 = state.unk3380;
    skk::restoreGameObjectPhysics(state.gameObjectPhysics, playLayer->m_gameState.m_gameObjectPhysics);

    playLayer->m_gameState.m_timeWarp = state.timeWarp;
    playLayer->m_gameState.m_queuedTimeWarp = state.queuedTimeWarp;
    playLayer->m_gameState.m_timeWarpRelated = state.timeWarpRelated;
    playLayer->m_gameState.m_timeModRelated = state.timeModRelated;
    playLayer->m_gameState.m_timeModRelated2 = state.timeModRelated2;
    playLayer->m_gameState.m_gravityRelated = state.gravityRelated;
    if (auto* pl = PlayLayer::get()) {
        pl->m_attemptTime = state.attemptTime;
    }

    for (float& value : playLayer->m_varianceValues) {
        value = 0.0f;
    }
    for (auto const& entry : state.variance) {
        if (entry.first < playLayer->m_varianceValues.size()) {
            playLayer->m_varianceValues[entry.first] = entry.second;
        }
    }

    if (engine) {
        engine->restoreLockedRngState(state.rng.fastRandState);
        engine->restoreLockedShakeState(state.rng.shakeState);
        if (engine->rngLocked) {
            skk::restoreTripleRng({state.rng.fastRandState,
                                   state.rng.teleportState});
        }
    }

    if (p1 && state.hasFullPlayer) {
        tracked.slopeOnBefore = p1->m_isOnSlope;
        tracked.slopeOnStored = state.fullPlayer1.isOnSlope;
        tracked.wasOnSlopeBefore = p1->m_wasOnSlope;
        tracked.wasOnSlopeStored = state.fullPlayer1.wasOnSlope;
        tracked.slopeVelBefore = p1->m_slopeVelocity;
        tracked.slopeVelStored = state.fullPlayer1.slopeVelocity;
        tracked.rotatingBefore = p1->m_isRotating;
        tracked.rotatingStored = (state.fullPlayer1.flags1 & (1ull << 9)) != 0;
        tracked.rotateSpeedBefore = p1->m_rotateSpeed;
        tracked.rotateSpeedStored = state.fullPlayer1.rotateSpeed;
        tracked.curSlopeYVelBefore = p1->m_currentSlopeYVelocity;
        tracked.curSlopeYVelStored = state.fullPlayer1.currentSlopeYVelocity;
        tracked.slopeAngleBefore = p1->m_slopeAngle;
        tracked.slopeAngleStored = state.fullPlayer1.slopeAngle;
        tracked.cwSlopeIdBefore = p1->m_collidingWithSlopeId;
        tracked.cwSlopeIdStored = state.fullPlayer1.collidingWithSlopeId;
        tracked.lastCBBefore = p1->m_lastCollisionBottom;
        tracked.lastCBStored = state.fullPlayer1.lastCollisionBottom;
        tracked.stateForceBefore = p1->m_stateForce;
        tracked.stateForceStored = state.fullPlayer1.stateForce;
        tracked.jumpBufBefore = p1->m_jumpBuffered;
        tracked.jumpBufStored = (state.fullPlayer1.flags1 & (1ull << 29)) != 0;
        tracked.touchedPortalBefore = p1->m_touchedGravityPortal;
        tracked.touchedPortalStored =
            (state.fullPlayer1.flags1 & (1ull << 36)) != 0;

        bridgePlayerSeamState(*p1, state.fullPlayer1);
    }
    if (playLayer->m_player2 && state.hasFullPlayer2) {
        bridgePlayerSeamState(*playLayer->m_player2, state.fullPlayer2);
    }
    InputStateRestorer::restoreInputState(playLayer, state);

    skkbot2::log::info(
        "[SKC] seam bridge frame={} | step {}->{} (snap {}) "
        "extraDelta {:a}->{:a} (snap {:a}) unk32d0 {}->{} (snap {}) "
        "unk32ec {}->{} (snap {}) unk3380 {:a}->{:a} (snap {:a}) | "
        "rot {:.4f}->{:.4f} (snap {:.4f}) rotSpeed {:a}->{:a} (snap {:a}) "
        "unkAngle1 {:a}->{:a} (snap {:a}) slopeRot {:.4f}->{:.4f} "
        "(snap {:.4f}) | variance {}->{} slots (snap {}) rng "
        "snap={:#x} teleportSnap={:#x} physMap={}",
        best->frame,
        tracked.currentStepBefore, playLayer->m_currentStep,
        tracked.currentStepStored,
        tracked.extraDeltaBefore, playLayer->m_extraDelta,
        tracked.extraDeltaStored,
        tracked.unk32d0Before, playLayer->m_unk32d0, tracked.unk32d0Stored,
        tracked.unk32ecBefore, playLayer->m_unk32ec, tracked.unk32ecStored,
        tracked.unk3380Before, playLayer->m_unk3380, tracked.unk3380Stored,
        static_cast<double>(tracked.rotBefore),
        static_cast<double>(p1 ? p1->getRotation() : 0.0f),
        static_cast<double>(tracked.rotStored),
        tracked.rotSpeedBefore, p1 ? p1->m_rotationSpeed : 0.0f,
        tracked.rotSpeedStored,
        tracked.unkAngle1Before, p1 ? p1->m_unkAngle1 : 0.0f,
        tracked.unkAngle1Stored,
        tracked.slopeRotationBefore,
        p1 ? p1->m_slopeRotation : 0.0, tracked.slopeRotationStored,
        tracked.varianceSlotsBefore,
        static_cast<int>(std::count_if(playLayer->m_varianceValues.begin(),
                                       playLayer->m_varianceValues.end(),
                                       [](float v) { return v != 0.0f; })),
        tracked.varianceSlotsStored,
        state.rng.fastRandState, state.rng.teleportState,
        state.gameObjectPhysics.size());

    skkbot2::log::info(
        "[SKC] seam bridge family frame={} | slopeOn {}->{} (snap {}) "
        "wasOnSlope {}->{} (snap {}) slopeVel {:a}->{:a} (snap {:a}) "
        "slopeAngle {:.1f}->{:.1f} (snap {:.1f}) curSlopeYVel {:a}->{:a} "
        "(snap {:a}) rotating {}->{} (snap {}) rotateSpeed {:a}->{:a} "
        "(snap {:a}) cwSlopeId {}->{} (snap {}) lastCB {}->{} (snap {}) "
        "stateForce {}->{} (snap {}) jumpBuf {}->{} (snap {}) "
        "touchedGravityPortal {}->{} (snap {})",
        best->frame,
        tracked.slopeOnBefore, p1 ? p1->m_isOnSlope : false,
        tracked.slopeOnStored,
        tracked.wasOnSlopeBefore, p1 ? p1->m_wasOnSlope : false,
        tracked.wasOnSlopeStored,
        tracked.slopeVelBefore, p1 ? p1->m_slopeVelocity : 0.0f,
        tracked.slopeVelStored,
        static_cast<double>(tracked.slopeAngleBefore),
        static_cast<double>(p1 ? p1->m_slopeAngle : 0.0f),
        static_cast<double>(tracked.slopeAngleStored),
        tracked.curSlopeYVelBefore,
        p1 ? p1->m_currentSlopeYVelocity : 0.0f,
        tracked.curSlopeYVelStored,
        tracked.rotatingBefore, p1 ? p1->m_isRotating : false,
        tracked.rotatingStored,
        tracked.rotateSpeedBefore, p1 ? p1->m_rotateSpeed : 0.0f,
        tracked.rotateSpeedStored,
        tracked.cwSlopeIdBefore, p1 ? p1->m_collidingWithSlopeId : 0,
        tracked.cwSlopeIdStored,
        tracked.lastCBBefore, p1 ? p1->m_lastCollisionBottom : 0,
        tracked.lastCBStored,
        tracked.stateForceBefore, p1 ? p1->m_stateForce : 0,
        tracked.stateForceStored,
        tracked.jumpBufBefore, p1 ? p1->m_jumpBuffered : false,
        tracked.jumpBufStored,
        tracked.touchedPortalBefore,
        p1 ? p1->m_touchedGravityPortal : false,
        tracked.touchedPortalStored);

    skkbot2::log::info(
        "[SKC] seam bridge vy-meter frame={} | vy {:a} vs snap {:a} "
        "fallSpeed {:a} vs {:a} groundYVel {:a} vs {:a} "
        "yVelBeforeSlope {:a} vs {:a} yVelRelated {:a} vs {:a} "
        "platXVel {:a} vs {:a} xVelRelated {:a} vs {:a} "
        "onGround2 {} vs {} pos {:.4f},{:.4f}",
        best->frame,
        tracked.vyBefore, tracked.vyStored,
        tracked.fallSpeedBefore, tracked.fallSpeedStored,
        tracked.groundYVelBefore, tracked.groundYVelStored,
        tracked.yVelBeforeSlopeBefore, tracked.yVelBeforeSlopeStored,
        tracked.yVelRelatedBefore, tracked.yVelRelatedStored,
        tracked.platXVelBefore, tracked.platXVelStored,
        tracked.playerVelXBefore, tracked.playerVelXStored,
        tracked.onGround2Before, tracked.onGround2Stored,
        p1 ? p1->getPositionX() : 0.0f, p1 ? p1->getPositionY() : 0.0f);

    skkbot2::log::info(
        "[SKC] seam bridge bug136 frame={} | prevCB {}->{} (snap {}) "
        "prevCT {}->{} (snap {}) | log t/b/l/r before {} {} {} {} "
        "(snap {} {} {} {}) alias {}->{} (snap {}) | "
        "timeWarp {:a}->{:a} (snap {:a}) queued {:a}->{:a} (snap {:a}) "
        "warpRel {:a}->{:a} (snap {:a}) timeMod {:a}->{:a} (snap {:a}) "
        "mod2 {}->{} (snap {}) gravRel {}->{} (snap {}) "
        "attemptTime {:a} vs snap {:a}",
        best->frame,
        tracked.prevCBBefore, p1 ? p1->m_prevCollisionBottom : 0,
        tracked.prevCBStored,
        tracked.prevCTBefore, p1 ? p1->m_prevCollisionTop : 0,
        tracked.prevCTStored,
        tracked.logTopBefore, tracked.logBottomBefore,
        tracked.logLeftBefore, tracked.logRightBefore,
        tracked.logTopStored, tracked.logBottomStored,
        tracked.logLeftStored, tracked.logRightStored,
        tracked.logAliasBefore,
        p1 ? (p1->m_collisionLogTop == p1->m_collisionLogBottom
                  ? 1
                  : (p1->m_collisionLogTop == p1->m_collisionLogLeft
                         ? 2
                         : (p1->m_collisionLogTop == p1->m_collisionLogRight
                                ? 3
                                : 0)))
           : 0,
        tracked.logAliasStored,
        tracked.timeWarpBefore, playLayer->m_gameState.m_timeWarp,
        tracked.timeWarpStored,
        tracked.queuedTimeWarpBefore,
        playLayer->m_gameState.m_queuedTimeWarp,
        tracked.queuedTimeWarpStored,
        tracked.timeWarpRelatedBefore,
        playLayer->m_gameState.m_timeWarpRelated,
        tracked.timeWarpRelatedStored,
        tracked.timeModRelatedBefore,
        playLayer->m_gameState.m_timeModRelated,
        tracked.timeModRelatedStored,
        tracked.timeModRelated2Before,
        playLayer->m_gameState.m_timeModRelated2,
        tracked.timeModRelated2Stored,
        tracked.gravityRelatedBefore,
        playLayer->m_gameState.m_gravityRelated,
        tracked.gravityRelatedStored,
        tracked.attemptTimeBefore, tracked.attemptTimeStored);

    skkbot2::log::info(
        "[SKC] practice restore (vanilla state + seam bridge): "
        "frame={} progress={} timeFrame={} mode={} snapshots={}",
        best->frame, best->progressFrame, best->timeFrame,
        best->platformer ? "platformer" : "classic", m_checkpoints.size());

    return best->frame;
}

std::optional<uint32_t> PracticeFix::frameFor(CheckpointObject* checkpoint) const {
    for (auto it = m_checkpoints.rbegin(); it != m_checkpoints.rend(); ++it)
        if (checkpoint && it->object == checkpoint) return it->frame;
    return std::nullopt;
}

void PracticeFix::syncStoredCheckpoints(PlayLayer* playLayer) {
    auto* array = playLayer ? playLayer->m_checkpointArray : nullptr;
    const auto present = [array](CheckpointObject* cp) {
        return array && cp && array->containsObject(cp);
    };
    std::erase_if(m_checkpoints, [&](auto const& cp) { return !present(cp.object); });
    std::erase_if(m_platformerCheckpoints, [&](auto* cp) {
        if (present(cp)) return false;
        cp->release();
        return true;
    });
}

void PracticeFix::clear() {
    ++m_generation;
    for (auto* checkpoint : m_platformerCheckpoints)
        if (checkpoint) checkpoint->release();
    m_platformerCheckpoints.clear();
    m_checkpoints.clear();
    m_seams = 0;
}

uint32_t PracticeFix::lastCheckpointFrame() const {
    if (m_checkpoints.empty()) return 0;
    return m_checkpoints.back().frame;
}

}
