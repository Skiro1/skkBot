#include "full_player_checkpoint.hpp"
#include "broken_object_tracker.hpp"

#include <Geode/Geode.hpp>
#include <cmath>

using namespace geode::prelude;

static bool holdButtonHeld(PlayerObject* player, int button) {
    if (!player) return false;
    auto it = player->m_holdingButtons.find(button);
    return it != player->m_holdingButtons.end() && it->second;
}

static uint64_t packFlags1(PlayerObject* p) {
    uint64_t f = 0;
    if (p->m_wasTeleported)           f |= 1ull << 0;
    if (p->m_fixGravityBug)          f |= 1ull << 1;
    if (p->m_reverseSync)            f |= 1ull << 2;
    if (p->m_justPlacedStreak)       f |= 1ull << 3;
    if (p->m_slopeSlidingMaybeRotated)f |= 1ull << 4;
    if (p->m_quickCheckpointMode)    f |= 1ull << 5;
    if (p->m_isCollidingWithSlope)   f |= 1ull << 6;
    if (p->m_isBallRotating)         f |= 1ull << 7;
    if (p->m_unk669)                 f |= 1ull << 8;
    if (p->m_isRotating)             f |= 1ull << 9;
    if (p->m_isBallRotating2)        f |= 1ull << 10;
    if (p->m_hasGlow)                f |= 1ull << 11;
    if (p->m_isHidden)               f |= 1ull << 12;
    if (p->m_padRingRelated)         f |= 1ull << 13;
    if (p->m_maybeReducedEffects)    f |= 1ull << 14;
    if (p->m_maybeIsFalling)         f |= 1ull << 15;
    if (p->m_shouldTryPlacingCheckpoint) f |= 1ull << 16;
    if (p->m_playEffects)            f |= 1ull << 17;
    if (p->m_maybeCanRunIntoBlocks)  f |= 1ull << 18;
    if (p->m_hasGroundParticles)     f |= 1ull << 19;
    if (p->m_hasShipParticles)       f |= 1ull << 20;
    if (p->m_isOnGround3)            f |= 1ull << 21;
    if (p->m_checkpointTimeout)      f |= 1ull << 22;
    if (p->m_isAccelerating)         f |= 1ull << 23;
    if (p->m_isCurrentSlopeTop)      f |= 1ull << 24;
    if (p->m_fadeOutStreak)          f |= 1ull << 25;
    if (p->m_canPlaceCheckpoint)     f |= 1ull << 26;
    if (p->m_hasCustomGlowColor)     f |= 1ull << 27;
    if (p->m_maybeIsColliding)       f |= 1ull << 28;
    if (p->m_jumpBuffered)           f |= 1ull << 29;
    if (p->m_stateRingJump)          f |= 1ull << 30;
    if (p->m_wasJumpBuffered)        f |= 1ull << 31;
    if (p->m_wasRobotJump)           f |= (1ull << 32);
    if (p->m_stateRingJump2)         f |= (1ull << 33);
    if (p->m_touchedRing)            f |= (1ull << 34);
    if (p->m_touchedCustomRing)      f |= (1ull << 35);
    if (p->m_touchedGravityPortal)   f |= (1ull << 36);
    if (p->m_maybeTouchedBreakableBlock) f |= (1ull << 37);
    if (p->m_touchedPad)             f |= (1ull << 38);
    if (p->m_slopeFlipGravityRelated)f |= (1ull << 39);
    if (p->m_maybeSpriteRelated)     f |= (1ull << 40);
    if (p->m_useLandParticles0)      f |= (1ull << 41);
    if (p->m_disableStreakTint)      f |= (1ull << 42);
    if (p->m_alwaysShowStreak)       f |= (1ull << 43);
    if (p->m_maybeIsBoosted)         f |= (1ull << 44);
    if (p->m_decreaseBoostSlide)     f |= (1ull << 45);
    if (p->m_isLocked)               f |= (1ull << 46);
    if (p->m_controlsDisabled)       f |= (1ull << 47);
    if (p->m_hasEverJumped)          f |= (1ull << 48);
    if (p->m_isSecondPlayer)         f |= (1ull << 50);
    if (p->m_isBeingSpawnedByDualPortal) f |= (1ull << 51);
    if (p->m_maybeHasStopped)        f |= (1ull << 52);
    if (p->m_maybeGoingCorrectSlopeDirection) f |= (1ull << 53);
    if (p->m_isSliding)              f |= (1ull << 54);
    if (p->m_isOnIce)                f |= (1ull << 55);
    if (p->m_isOnGround4)            f |= (1ull << 56);
    if (p->m_isMoving)               f |= (1ull << 57);
    if (p->m_platformerMovingLeft)    f |= (1ull << 58);
    if (p->m_platformerMovingRight)   f |= (1ull << 59);
    if (p->m_isSlidingRight)         f |= (1ull << 60);
    if (p->m_isPlatformer)           f |= (1ull << 61);
    if (p->m_affectedByForces)       f |= (1ull << 62);
    if (p->m_fixRobotJump)           f |= (1ull << 63);
    return f;
}

static uint64_t packFlags2(PlayerObject* p) {
    uint64_t f = 0;
    if (p->m_isShip)             f |= 1ull << 0;
    if (p->m_isBird)             f |= 1ull << 1;
    if (p->m_isBall)             f |= 1ull << 2;
    if (p->m_isDart)             f |= 1ull << 3;
    if (p->m_isRobot)            f |= 1ull << 4;
    if (p->m_isSpider)           f |= 1ull << 5;
    if (p->m_isUpsideDown)       f |= 1ull << 6;
    if (p->m_isSwing)            f |= 1ull << 7;
    if (p->m_isDead)             f |= 1ull << 8;
    if (p->m_isOnGround)         f |= 1ull << 9;
    if (p->m_isGoingLeft)        f |= 1ull << 10;
    if (p->m_isSideways)         f |= 1ull << 11;
    if (p->m_isDashing)          f |= 1ull << 12;
    if (p->m_maybeIsVehicleGlowing) f |= 1ull << 13;
    if (p->m_unkBool5)           f |= 1ull << 16;
    if (p->m_unk3e0)             f |= 1ull << 17;
    if (p->m_unk3e1)             f |= 1ull << 18;
    if (p->m_hasEverHitRing)     f |= (1ull << 19);
    if (p->m_defaultMiniIcon)    f |= (1ull << 20);
    if (p->m_swapColors)         f |= (1ull << 21);
    if (p->m_switchDashFireColor)f |= (1ull << 22);
    if (p->m_gv0123)             f |= (1ull << 23);
    if (p->m_robotAnimation1Enabled) f |= (1ull << 24);
    if (p->m_robotAnimation2Enabled) f |= (1ull << 25);
    if (p->m_spiderAnimationEnabled) f |= (1ull << 26);
    if (p->m_ignoreDamage)       f |= (1ull << 27);
    if (p->m_enable22Changes)    f |= (1ull << 28);
    if (p->m_disablePlayerSqueeze) f |= (1ull << 29);
    if (p->m_isOutOfBounds)      f |= (1ull << 30);
    return f;
}

static void unpackFlags1(PlayerObject* p, uint64_t f) {
    p->m_wasTeleported = (f & (1ull << 0)) != 0;
    p->m_fixGravityBug = (f & (1ull << 1)) != 0;
    p->m_reverseSync = (f & (1ull << 2)) != 0;
    p->m_justPlacedStreak = (f & (1ull << 3)) != 0;
    p->m_slopeSlidingMaybeRotated = (f & (1ull << 4)) != 0;
    p->m_quickCheckpointMode = (f & (1ull << 5)) != 0;
    p->m_isCollidingWithSlope = (f & (1ull << 6)) != 0;
    p->m_isBallRotating = (f & (1ull << 7)) != 0;
    p->m_unk669 = (f & (1ull << 8)) != 0;
    p->m_isRotating = (f & (1ull << 9)) != 0;
    p->m_isBallRotating2 = (f & (1ull << 10)) != 0;
    p->m_hasGlow = (f & (1ull << 11)) != 0;
    p->m_isHidden = (f & (1ull << 12)) != 0;
    p->m_padRingRelated = (f & (1ull << 13)) != 0;
    p->m_maybeReducedEffects = (f & (1ull << 14)) != 0;
    p->m_maybeIsFalling = (f & (1ull << 15)) != 0;
    p->m_shouldTryPlacingCheckpoint = (f & (1ull << 16)) != 0;
    p->m_playEffects = (f & (1ull << 17)) != 0;
    p->m_maybeCanRunIntoBlocks = (f & (1ull << 18)) != 0;
    p->m_hasGroundParticles = (f & (1ull << 19)) != 0;
    p->m_hasShipParticles = (f & (1ull << 20)) != 0;
    p->m_isOnGround3 = (f & (1ull << 21)) != 0;
    p->m_checkpointTimeout = (f & (1ull << 22)) != 0;
    p->m_isAccelerating = (f & (1ull << 23)) != 0;
    p->m_isCurrentSlopeTop = (f & (1ull << 24)) != 0;
    p->m_fadeOutStreak = (f & (1ull << 25)) != 0;
    p->m_canPlaceCheckpoint = (f & (1ull << 26)) != 0;
    p->m_hasCustomGlowColor = (f & (1ull << 27)) != 0;
    p->m_maybeIsColliding = (f & (1ull << 28)) != 0;
    p->m_jumpBuffered = (f & (1ull << 29)) != 0;
    p->m_stateRingJump = (f & (1ull << 30)) != 0;
    p->m_wasJumpBuffered = (f & (1ull << 31)) != 0;
    p->m_wasRobotJump = (f & (1ull << 32)) != 0;
    p->m_stateRingJump2 = (f & (1ull << 33)) != 0;
    p->m_touchedRing = (f & (1ull << 34)) != 0;
    p->m_touchedCustomRing = (f & (1ull << 35)) != 0;
    p->m_touchedGravityPortal = (f & (1ull << 36)) != 0;
    p->m_maybeTouchedBreakableBlock = (f & (1ull << 37)) != 0;
    p->m_touchedPad = (f & (1ull << 38)) != 0;
    p->m_slopeFlipGravityRelated = (f & (1ull << 39)) != 0;
    p->m_maybeSpriteRelated = (f & (1ull << 40)) != 0;
    p->m_useLandParticles0 = (f & (1ull << 41)) != 0;
    p->m_disableStreakTint = (f & (1ull << 42)) != 0;
    p->m_alwaysShowStreak = (f & (1ull << 43)) != 0;
    p->m_maybeIsBoosted = (f & (1ull << 44)) != 0;
    p->m_decreaseBoostSlide = (f & (1ull << 45)) != 0;
    p->m_isLocked = (f & (1ull << 46)) != 0;
    p->m_controlsDisabled = (f & (1ull << 47)) != 0;
    p->m_hasEverJumped = (f & (1ull << 48)) != 0;
    p->m_isSecondPlayer = (f & (1ull << 50)) != 0;
    p->m_isBeingSpawnedByDualPortal = (f & (1ull << 51)) != 0;
    p->m_maybeHasStopped = (f & (1ull << 52)) != 0;
    p->m_maybeGoingCorrectSlopeDirection = (f & (1ull << 53)) != 0;
    p->m_isSliding = (f & (1ull << 54)) != 0;
    p->m_isOnIce = (f & (1ull << 55)) != 0;
    p->m_isOnGround4 = (f & (1ull << 56)) != 0;
    p->m_isMoving = (f & (1ull << 57)) != 0;
    p->m_platformerMovingLeft = (f & (1ull << 58)) != 0;
    p->m_platformerMovingRight = (f & (1ull << 59)) != 0;
    p->m_isSlidingRight = (f & (1ull << 60)) != 0;
    p->m_isPlatformer = (f & (1ull << 61)) != 0;
    p->m_affectedByForces = (f & (1ull << 62)) != 0;
    p->m_fixRobotJump = (f & (1ull << 63)) != 0;
}

static void unpackFlags2(PlayerObject* p, uint64_t f) {
    p->m_isShip = (f & 1ull) != 0;
    p->m_isBird = (f & (1ull << 1)) != 0;
    p->m_isBall = (f & (1ull << 2)) != 0;
    p->m_isDart = (f & (1ull << 3)) != 0;
    p->m_isRobot = (f & (1ull << 4)) != 0;
    p->m_isSpider = (f & (1ull << 5)) != 0;
    p->m_isUpsideDown = (f & (1ull << 6)) != 0;
    p->m_isSwing = (f & (1ull << 7)) != 0;
    p->m_isDead = (f & (1ull << 8)) != 0;
    p->m_isOnGround = (f & (1ull << 9)) != 0;
    p->m_isGoingLeft = (f & (1ull << 10)) != 0;
    p->m_isSideways = (f & (1ull << 11)) != 0;
    p->m_isDashing = (f & (1ull << 12)) != 0;
    p->m_maybeIsVehicleGlowing = (f & (1ull << 13)) != 0;
    p->m_unkBool5 = (f & (1ull << 16)) != 0;
    p->m_unk3e0 = (f & (1ull << 17)) != 0;
    p->m_unk3e1 = (f & (1ull << 18)) != 0;
    p->m_hasEverHitRing = (f & (1ull << 19)) != 0;
    p->m_defaultMiniIcon = (f & (1ull << 20)) != 0;
    p->m_swapColors = (f & (1ull << 21)) != 0;
    p->m_switchDashFireColor = (f & (1ull << 22)) != 0;
    p->m_gv0123 = (f & (1ull << 23)) != 0;
    p->m_robotAnimation1Enabled = (f & (1ull << 24)) != 0;
    p->m_robotAnimation2Enabled = (f & (1ull << 25)) != 0;
    p->m_spiderAnimationEnabled = (f & (1ull << 26)) != 0;
    p->m_ignoreDamage = (f & (1ull << 27)) != 0;
    p->m_enable22Changes = (f & (1ull << 28)) != 0;
    p->m_disablePlayerSqueeze = (f & (1ull << 29)) != 0;
    p->m_isOutOfBounds = (f & (1ull << 30)) != 0;
}

FullPlayerCheckpoint FullPlayerCheckpoint::store(PlayerObject* p) {
    FullPlayerCheckpoint cp;
    if (!p) return cp;

    cp.p_x = p->m_position.x;
    cp.p_y = p->m_position.y;
    cp.p_rotation = p->getRotation();
    cp.rotationSpeed = p->m_rotationSpeed;
    cp.shipRotX = p->m_shipRotation.x;
    cp.shipRotY = p->m_shipRotation.y;
    cp.lastPortalX = p->m_lastPortalPos.x;
    cp.lastPortalY = p->m_lastPortalPos.y;
    cp.lastGroundedX = p->m_lastGroundedPos.x;
    cp.lastGroundedY = p->m_lastGroundedPos.y;

    cp.yVelocity = p->m_yVelocity;
    cp.fallSpeed = p->m_fallSpeed;
    cp.platformerXVelocity = p->m_platformerXVelocity;
    cp.groundYVelocity = p->m_groundYVelocity;
    cp.yVelocityBeforeSlope = p->m_yVelocityBeforeSlope;
    cp.yVelocityRelated = p->m_yVelocityRelated;
    cp.scaleXRelated = p->m_scaleXRelated;
    cp.scaleXRelated2 = p->m_scaleXRelated2;
    cp.scaleXRelated3 = p->m_scaleXRelated3;
    cp.scaleXRelated4 = p->m_scaleXRelated4;
    cp.scaleXRelated5 = p->m_scaleXRelated5;
    cp.scaleXRelatedTime = p->m_scaleXRelatedTime;
    cp.playerSpeed = p->m_playerSpeed;
    cp.vehicleSize = p->m_vehicleSize;
    cp.platformerVelocityRelated = p->m_platformerVelocityRelated;
    cp.xVelocityRelated = p->m_xVelocityRelated;
    cp.xVelocityRelated2 = p->m_xVelocityRelated2;
    cp.yVelocityRelated3 = p->m_yVelocityRelated3;
    cp.slopeVelocity = p->m_slopeVelocity;

    cp.gravity = p->m_gravity;
    cp.speedMultiplier = p->m_speedMultiplier;
    cp.snapDistance = p->m_snapDistance;
    cp.accelerationOrSpeed = p->m_accelerationOrSpeed;
    cp.yStart = p->m_yStart;
    cp.unk_584 = p->unk_584;
    cp.unk3d0 = p->m_unk3d0;
    cp.gravityMod = p->m_gravityMod;
    cp.unk648 = p->m_unk648;
    cp.unk838 = p->m_unk838;
    cp.somethingPlayerSpeedTime = p->m_somethingPlayerSpeedTime;
    cp.playerSpeedAC = p->m_playerSpeedAC;

    cp.slopeAngle = p->m_slopeAngle;
    cp.slopeAngleRadians = p->m_slopeAngleRadians;
    cp.slopeRotation = p->m_slopeRotation;
    cp.currentSlopeYVelocity = p->m_currentSlopeYVelocity;
    cp.slopeStartTime = p->m_slopeStartTime;
    cp.maybeSlopeForce = p->m_maybeSlopeForce;
    cp.physDeltaRelated = p->m_physDeltaRelated;
    cp.changedDirectionsTime = p->m_changedDirectionsTime;
    cp.slopeEndTime = p->m_slopeEndTime;
    cp.maybeChangedDirectionAngle = p->m_maybeChangedDirectionAngle;
    cp.unkUnused2 = p->m_unkUnused2;

    cp.dashX = p->m_dashX;
    cp.dashY = p->m_dashY;
    cp.dashAngle = p->m_dashAngle;
    cp.dashStartTime = p->m_dashStartTime;
    cp.blackOrbRelated = p->m_blackOrbRelated;

    cp.totalTime = p->m_totalTime;
    cp.lastCheckpointTime = p->m_lastCheckpointTime;
    cp.lastJumpTime = p->m_lastJumpTime;
    cp.lastFlipTime = p->m_lastFlipTime;
    cp.lastSpiderFlipTime = p->m_lastSpiderFlipTime;
    cp.gameModeChangedTime = p->m_gameModeChangedTime;
    cp.lastLandTime = p->m_lastLandTime;
    cp.flashTime = p->m_flashTime;
    cp.flashDuration = p->m_flashDuration;
    cp.flashDelay = p->m_flashDelay;
    cp.audioScale = p->m_audioScale;
    cp.unkAngle1 = p->m_unkAngle1;

    cp.lastCollisionBottom = p->m_lastCollisionBottom;
    cp.lastCollisionTop = p->m_lastCollisionTop;
    cp.lastCollisionLeft = p->m_lastCollisionLeft;
    cp.lastCollisionRight = p->m_lastCollisionRight;
    cp.unk50C = p->m_unk50C;
    cp.unk510 = p->m_unk510;
    cp.collidingWithSlopeId = p->m_collidingWithSlopeId;
    cp.collidedTopMinY = p->m_collidedTopMinY;
    cp.collidedBottomMaxY = p->m_collidedBottomMaxY;
    cp.collidedLeftMaxX = p->m_collidedLeftMaxX;
    cp.collidedRightMinX = p->m_collidedRightMinX;
    cp.groundObjectMaterial = p->m_groundObjectMaterial;
    cp.maybeSavedPlayerFrame = p->m_maybeSavedPlayerFrame;
    cp.maybeSlidingTime = p->m_maybeSlidingTime;

    cp.stateOnGround = p->m_stateOnGround;
    cp.stateBoostX = p->m_stateBoostX;
    cp.stateBoostY = p->m_stateBoostY;
    cp.maybeStateForce2 = p->m_maybeStateForce2;
    cp.stateScale = p->m_stateScale;
    cp.stateNoAutoJump = p->m_stateNoAutoJump;
    cp.stateDartSlide = p->m_stateDartSlide;
    cp.stateHitHead = p->m_stateHitHead;
    cp.stateFlipGravity = p->m_stateFlipGravity;
    cp.stateForce = p->m_stateForce;
    cp.reverseRelated = p->m_reverseRelated;
    cp.maybeReverseSpeed = p->m_maybeReverseSpeed;
    cp.maybeReverseAcceleration = p->m_maybeReverseAcceleration;
    cp.onFlyCheckpointTries = p->m_onFlyCheckpointTries;
    cp.playerStreak = p->m_playerStreak;
    cp.streakStrokeWidth = p->m_streakStrokeWidth;
    cp.shipStreakType = static_cast<int>(p->m_shipStreakType);
    cp.trailingParticleLife = p->m_trailingParticleLife;
    cp.landParticlesAngle = p->m_landParticlesAngle;
    cp.landParticleRelatedY = p->m_landParticleRelatedY;
    cp.followRelated = p->m_followRelated;
    cp.iconRequestID = p->m_iconRequestID;
    cp.unkUnused = p->m_unkUnused;
    cp.unkUnused3 = p->m_unkUnused3;
    cp.fallStartY = p->m_fallStartY;

    cp.flags1 = packFlags1(p);
    cp.flags2 = packFlags2(p);

    cp.rotateSpeed = p->m_rotateSpeed;
    cp.dashFireFrame = p->m_dashFireFrame;
    cp.holdingRight = p->m_holdingRight;
    cp.holdingLeft = p->m_holdingLeft;
    cp.leftPressedFirst = p->m_leftPressedFirst;
    cp.wasOnSlope = p->m_wasOnSlope;
    cp.maybeUpsideDownSlope = p->m_maybeUpsideDownSlope;
    cp.stateJumpBuffered = p->m_stateJumpBuffered;
    cp.stateUnk = p->m_stateUnk;
    cp.stateNoStickX = p->m_stateNoStickX;
    cp.stateNoStickY = p->m_stateNoStickY;
    cp.stateUnk2 = p->m_stateUnk2;
    cp.ringJumpRelated = p->m_ringJumpRelated;
    cp.unkA29 = p->m_unkA29;
    cp.unkA99 = p->m_unkA99;

    cp.clickReleased = {
        !holdButtonHeld(p, 1),
        !holdButtonHeld(p, 2),
        !holdButtonHeld(p, 3)
    };

    cp.playerColor1 = p->m_playerColor1;
    cp.playerColor2 = p->m_playerColor2;
    cp.originalMainColor = p->m_originalMainColor;
    cp.originalSecondColor = p->m_originalSecondColor;

    if (p->m_waveTrail && p->m_waveTrail->m_pointArray) {
        auto* arr = p->m_waveTrail->m_pointArray;
        for (unsigned int i = 0; i < arr->count(); ++i) {
            auto* node = static_cast<PointNode*>(arr->objectAtIndex(i));
            cp.waveTrailPoints.push_back(node->m_point);
        }
        // The streak's own draw state, not just its points: without these the
        // rebuilt HardStreak renders with defaults (see header note).
        cp.waveTrailCurrentPoint = p->m_waveTrail->m_currentPoint;
        cp.waveTrailWaveSize = p->m_waveTrail->m_waveSize;
        cp.waveTrailPulseSize = p->m_waveTrail->m_pulseSize;
        cp.waveTrailIsSolid = p->m_waveTrail->m_isSolid;
        cp.waveTrailIsFlipped = p->m_waveTrail->m_isFlipped;
        cp.waveTrailDrawStreak = p->m_waveTrail->m_drawStreak;
    }

    return cp;
}

void FullPlayerCheckpoint::apply(PlayerObject* p) const {
    if (!p) return;

    p->m_position = cocos2d::CCPoint(p_x, p_y);
    p->setPosition(p->m_position);
    p->setRotation(p_rotation);
    p->m_rotationSpeed = rotationSpeed;
    p->m_shipRotation = cocos2d::CCPoint(shipRotX, shipRotY);
    p->m_lastPortalPos = cocos2d::CCPoint(lastPortalX, lastPortalY);
    p->m_lastGroundedPos = cocos2d::CCPoint(lastGroundedX, lastGroundedY);

    p->m_yVelocity = yVelocity;
    p->m_fallSpeed = fallSpeed;
    p->m_platformerXVelocity = platformerXVelocity;
    p->m_groundYVelocity = groundYVelocity;
    p->m_yVelocityBeforeSlope = yVelocityBeforeSlope;
    p->m_yVelocityRelated = yVelocityRelated;
    p->m_scaleXRelated = scaleXRelated;
    p->m_scaleXRelated2 = scaleXRelated2;
    p->m_scaleXRelated3 = scaleXRelated3;
    p->m_scaleXRelated4 = scaleXRelated4;
    p->m_scaleXRelated5 = scaleXRelated5;
    p->m_scaleXRelatedTime = scaleXRelatedTime;
    p->m_playerSpeed = playerSpeed;
    p->m_vehicleSize = vehicleSize;
    p->m_platformerVelocityRelated = platformerVelocityRelated;
    p->m_xVelocityRelated = xVelocityRelated;
    p->m_xVelocityRelated2 = xVelocityRelated2;
    p->m_yVelocityRelated3 = yVelocityRelated3;
    p->m_slopeVelocity = slopeVelocity;

    p->m_gravity = gravity;
    p->m_speedMultiplier = speedMultiplier;
    p->m_snapDistance = snapDistance;
    p->m_accelerationOrSpeed = accelerationOrSpeed;
    p->m_yStart = yStart;
    p->unk_584 = unk_584;
    p->m_unk3d0 = unk3d0;
    p->m_gravityMod = gravityMod;
    p->m_unk648 = unk648;
    p->m_unk838 = unk838;
    p->m_somethingPlayerSpeedTime = somethingPlayerSpeedTime;
    p->m_playerSpeedAC = playerSpeedAC;

    p->m_slopeAngle = slopeAngle;
    p->m_slopeAngleRadians = slopeAngleRadians;
    p->m_slopeRotation = slopeRotation;
    p->m_currentSlopeYVelocity = currentSlopeYVelocity;
    p->m_slopeStartTime = slopeStartTime;
    p->m_maybeSlopeForce = maybeSlopeForce;
    p->m_physDeltaRelated = physDeltaRelated;
    p->m_changedDirectionsTime = changedDirectionsTime;
    p->m_slopeEndTime = slopeEndTime;
    p->m_maybeChangedDirectionAngle = maybeChangedDirectionAngle;
    p->m_unkUnused2 = unkUnused2;

    p->m_dashX = dashX;
    p->m_dashY = dashY;
    p->m_dashAngle = dashAngle;
    p->m_dashStartTime = dashStartTime;
    p->m_blackOrbRelated = blackOrbRelated;

    p->m_totalTime = totalTime;
    p->m_lastCheckpointTime = lastCheckpointTime;
    p->m_lastJumpTime = lastJumpTime;
    p->m_lastFlipTime = lastFlipTime;
    p->m_lastSpiderFlipTime = lastSpiderFlipTime;
    p->m_gameModeChangedTime = gameModeChangedTime;
    p->m_lastLandTime = lastLandTime;
    p->m_flashTime = flashTime;
    p->m_flashDuration = flashDuration;
    p->m_flashDelay = flashDelay;
    p->m_audioScale = audioScale;
    p->m_unkAngle1 = unkAngle1;

    p->m_lastCollisionBottom = lastCollisionBottom;
    p->m_lastCollisionTop = lastCollisionTop;
    p->m_lastCollisionLeft = lastCollisionLeft;
    p->m_lastCollisionRight = lastCollisionRight;
    p->m_unk50C = unk50C;
    p->m_unk510 = unk510;
    p->m_collidingWithSlopeId = collidingWithSlopeId;
    p->m_collidedTopMinY = collidedTopMinY;
    p->m_collidedBottomMaxY = collidedBottomMaxY;
    p->m_collidedLeftMaxX = collidedLeftMaxX;
    p->m_collidedRightMinX = collidedRightMinX;
    p->m_groundObjectMaterial = groundObjectMaterial;
    p->m_maybeSavedPlayerFrame = maybeSavedPlayerFrame;
    p->m_maybeSlidingTime = maybeSlidingTime;

    p->m_stateOnGround = stateOnGround;
    p->m_stateBoostX = stateBoostX;
    p->m_stateBoostY = stateBoostY;
    p->m_maybeStateForce2 = maybeStateForce2;
    p->m_stateScale = stateScale;
    p->m_stateNoAutoJump = stateNoAutoJump;
    p->m_stateDartSlide = stateDartSlide;
    p->m_stateHitHead = stateHitHead;
    p->m_stateFlipGravity = stateFlipGravity;
    p->m_stateForce = stateForce;
    p->m_reverseRelated = reverseRelated;
    p->m_maybeReverseSpeed = maybeReverseSpeed;
    p->m_maybeReverseAcceleration = maybeReverseAcceleration;
    p->m_onFlyCheckpointTries = onFlyCheckpointTries;
    p->m_playerStreak = playerStreak;
    p->m_streakStrokeWidth = streakStrokeWidth;
    p->m_shipStreakType = static_cast<ShipStreak>(shipStreakType);
    p->m_trailingParticleLife = trailingParticleLife;
    p->m_landParticlesAngle = landParticlesAngle;
    p->m_landParticleRelatedY = landParticleRelatedY;
    p->m_followRelated = followRelated;
    p->m_iconRequestID = iconRequestID;
    p->m_unkUnused = unkUnused;
    p->m_unkUnused3 = unkUnused3;
    p->m_fallStartY = fallStartY;

    unpackFlags1(p, flags1);
    unpackFlags2(p, flags2);

    p->m_rotateSpeed = rotateSpeed;
    p->m_dashFireFrame = dashFireFrame;
    p->m_holdingRight = holdingRight;
    p->m_holdingLeft = holdingLeft;
    p->m_leftPressedFirst = leftPressedFirst;
    p->m_wasOnSlope = wasOnSlope;
    p->m_maybeUpsideDownSlope = maybeUpsideDownSlope;
    p->m_stateJumpBuffered = stateJumpBuffered;
    p->m_stateUnk = stateUnk;
    p->m_stateNoStickX = stateNoStickX;
    p->m_stateNoStickY = stateNoStickY;
    p->m_stateUnk2 = stateUnk2;
    p->m_ringJumpRelated = ringJumpRelated;
    p->m_unkA29 = unkA29;
    p->m_unkA99 = unkA99;

    p->m_playerColor1 = playerColor1;
    p->m_playerColor2 = playerColor2;
    p->m_originalMainColor = originalMainColor;
    p->m_originalSecondColor = originalSecondColor;

    if (p->m_waveTrail && p->m_waveTrail->m_pointArray) {
        auto* arr = p->m_waveTrail->m_pointArray;
        arr->removeAllObjects();
        for (auto& pt : waveTrailPoints) {
            auto* node = PointNode::create(pt);
            if (node) arr->addObject(node);
        }
        // Re-pin the streak's draw state. This apply runs AFTER
        // forceCurrentModeVisual, which rebuilt m_waveTrail via
        // toggleDartMode(true) — a fresh HardStreak with reset flags. Without
        // m_drawStreak/m_currentPoint/m_waveSize the respawned wave trail
        // either does not draw at all or starts from the wrong origin, so the
        // trail visibly vanishes/jumps after a practice restart.
        p->m_waveTrail->m_currentPoint = waveTrailCurrentPoint;
        p->m_waveTrail->m_waveSize = waveTrailWaveSize;
        p->m_waveTrail->m_pulseSize = waveTrailPulseSize;
        p->m_waveTrail->m_isSolid = waveTrailIsSolid;
        p->m_waveTrail->m_isFlipped = waveTrailIsFlipped;
        p->m_waveTrail->m_drawStreak = waveTrailDrawStreak;
        // HardStreak is a CCNode whose own position is the streak's origin.
        // The rebuilt streak inherits no position, so the first frame after a
        // restore would draw from (0,0); pin it to the player like GD's own
        // update does for darts (silicate does the same in its render hook).
        p->m_waveTrail->setPosition(p->m_position);
    }
}
