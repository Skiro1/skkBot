#include "core/checkpoint_handler.hpp"
#include "core/full_player_checkpoint.hpp"
#include "core/broken_object_tracker.hpp"
#include "hacks/autoclicker.hpp"
#include "logger/logger.hpp"

#include <cmath>

namespace {
    static bool holdButtonHeld(PlayerObject* player, int button) {
        if (!player) {
            return false;
        }
        // find(), not operator[]: reading a map key that is absent must not
        // insert a phantom 'false' entry into the live player's state.
        const auto& hb = player->m_holdingButtons;
        auto it = hb.find(button);
        return it != hb.end() && it->second;
    }

    static uint8_t encodeButtonHoldMask(PlayerObject* player) {
        if (!player) {
            return 0;
        }
        uint8_t mask = 0;
        if (holdButtonHeld(player, 1)) mask |= 1 << 0;
        if (holdButtonHeld(player, 2)) mask |= 1 << 1;
        if (holdButtonHeld(player, 3)) mask |= 1 << 2;
        return mask;
    }

    static void setHoldButton(PlayerObject* player, int button, bool held) {
        // GD's natural representation: held = key present with true; not
        // held = key ABSENT (operator[] would insert a fake 'false' entry
        // that no live game state ever has - orb/ring pressed-animations
        // iterate the map and rendered differently under playback).
        auto& hb = player->m_holdingButtons;
        if (held) {
            hb[button] = true;
        } else {
            hb.erase(button);
        }
    }

    static void applyButtonHoldMask(PlayerObject* player, uint8_t mask) {
        if (!player) {
            return;
        }
        setHoldButton(player, 1, (mask & (1 << 0)) != 0);
        setHoldButton(player, 2, (mask & (1 << 1)) != 0);
        setHoldButton(player, 3, (mask & (1 << 2)) != 0);
    }

    static void deactivateActiveVehicleVisual(PlayerObject* player) {
        if (player->m_isShip)   player->toggleFlyMode(false, true);
        if (player->m_isBall)   player->toggleRollMode(false, true);
        if (player->m_isBird)   player->toggleBirdMode(false, true);
        if (player->m_isDart)   player->toggleDartMode(false, true);
        if (player->m_isSpider) player->toggleSpiderMode(false, true);
        if (player->m_isSwing)  player->toggleSwingMode(false, true);
        if (player->m_isRobot)  player->toggleRobotMode(false, true);
    }

    static void reconcileVehicleVisual(PlayerObject* player, PlayerStateBundle const& state) {
        bool sameMode = player->m_isShip == state.flags.ship
            && player->m_isBird == state.flags.bird
            && player->m_isBall == state.flags.ball
            && player->m_isDart == state.flags.wave
            && player->m_isRobot == state.flags.robot
            && player->m_isSpider == state.flags.spider
            && player->m_isSwing == state.flags.swing;
        if (sameMode) {
            return;
        }

        deactivateActiveVehicleVisual(player);

        if (state.flags.ship)        player->toggleFlyMode(true, true);
        else if (state.flags.ball)   player->toggleRollMode(true, true);
        else if (state.flags.bird)   player->toggleBirdMode(true, true);
        else if (state.flags.wave)   player->toggleDartMode(true, true);
        else if (state.flags.spider) player->toggleSpiderMode(true, true);
        else if (state.flags.swing)  player->toggleSwingMode(true, true);
        else if (state.flags.robot)  player->toggleRobotMode(true, true);

        // Reference parity: this function is byte-for-byte
        // ToastyReplay-main/src/core/checkpoint_handler.cpp:37-58. It
        // deliberately does NOT touch m_isUpsideDown, any sprite visibility or
        // updatePlayerScale() — restoreState writes m_isUpsideDown itself, and
        // GDH/silicate/ToastyReplay all leave the mode sprites entirely to
        // GD's own toggle. The extra machinery this function used to carry
        // (flipGravity + updatePlayerScale + enforceModeSpriteVisibility) hid
        // the robot for the whole session after a single restore.
        skkbot2::log::debug(
            "mode reconcile: was(rob={} shp={} bal={} drt={} spd={} swg={}) -> now(rob={}) robotSprite={}",
            player->m_isRobot ? 1 : 0, player->m_isShip ? 1 : 0, player->m_isBall ? 1 : 0,
            player->m_isDart ? 1 : 0, player->m_isSpider ? 1 : 0, player->m_isSwing ? 1 : 0,
            player->m_isRobot ? 1 : 0, player->m_robotSprite ? (player->m_robotSprite->isVisible() ? "vis" : "hidden") : "null");
    }
}

uint8_t InputStateRestorer::captureLatchMask(PlayerObject* player) {
    return encodeButtonHoldMask(player);
}

void InputStateRestorer::restoreInputState(GJBaseGameLayer* playLayer, PlaybackAnchor const& anchor) {
    if (!playLayer || !playLayer->m_player1) {
        return;
    }

    applyButtonHoldMask(playLayer->m_player1, anchor.player1LatchMask);
    playLayer->m_player1->m_holdingLeft = anchor.player1.flags.holdingLeft;
    playLayer->m_player1->m_holdingRight = anchor.player1.flags.holdingRight;

    if (anchor.hasPlayer2 && playLayer->m_player2) {
        applyButtonHoldMask(playLayer->m_player2, anchor.player2LatchMask);
        playLayer->m_player2->m_holdingLeft = anchor.player2.flags.holdingLeft;
        playLayer->m_player2->m_holdingRight = anchor.player2.flags.holdingRight;
    }
}

void InputStateRestorer::restoreInputState(GJBaseGameLayer* playLayer, CheckpointStateBundle const& checkpoint) {
    if (!playLayer || !playLayer->m_player1) {
        return;
    }

    applyButtonHoldMask(playLayer->m_player1, checkpoint.player1LatchMask);
    playLayer->m_player1->m_holdingLeft = checkpoint.player1.flags.holdingLeft;
    playLayer->m_player1->m_holdingRight = checkpoint.player1.flags.holdingRight;

    if (playLayer->m_player2) {
        applyButtonHoldMask(playLayer->m_player2, checkpoint.player2LatchMask);
        playLayer->m_player2->m_holdingLeft = checkpoint.player2.flags.holdingLeft;
        playLayer->m_player2->m_holdingRight = checkpoint.player2.flags.holdingRight;
    }
}

PlayerStateBundle PlayerStateRestorer::captureState(PlayerObject* player, bool isPlatformer, bool isDual, bool isTwoPlayer) {
    PlayerStateBundle state;
    if (!player) {
        return state;
    }

    state.motion.position = player->getPosition();
    state.motion.rotation = player->getRotation();
    state.motion.verticalVelocity = player->m_yVelocity;
    state.motion.preSlopeVerticalVelocity = player->m_yVelocityBeforeSlope;
    state.motion.horizontalVelocity = isPlatformer ? player->m_platformerXVelocity : 0.0;
    state.motion.dashX = player->m_dashX;
    state.motion.dashY = player->m_dashY;
    state.motion.dashAngle = player->m_dashAngle;
    state.motion.dashStartTime = player->m_dashStartTime;
    state.motion.slopeStartTime = player->m_slopeStartTime;
    state.motion.fallSpeed = player->m_fallSpeed;
    state.motion.slopeVelocity = player->m_slopeVelocity;
    state.motion.shipRotation = player->m_shipRotation;
    state.motion.lastPortalPosition = player->m_lastPortalPos;
    state.motion.stateForceVector = player->m_stateForceVector;

    state.flags.upsideDown = player->m_isUpsideDown;
    state.flags.holdingLeft = player->m_holdingLeft;
    state.flags.holdingRight = player->m_holdingRight;
    state.flags.platformer = isPlatformer;
    state.flags.dead = player->m_isDead;
    state.flags.ship = player->m_isShip;
    state.flags.bird = player->m_isBird;
    state.flags.ball = player->m_isBall;
    state.flags.wave = player->m_isDart;
    state.flags.robot = player->m_isRobot;
    state.flags.spider = player->m_isSpider;
    state.flags.swing = player->m_isSwing;
    state.flags.sideways = player->m_isSideways;
    state.flags.dashing = player->m_isDashing;
    state.flags.onSlope = player->m_isOnSlope;
    state.flags.wasOnSlope = player->m_wasOnSlope;
    state.flags.onGround = player->m_isOnGround;
    state.flags.goingLeft = player->m_isGoingLeft;
    state.flags.platformerMovingRight = player->m_platformerMovingRight;
    state.flags.slidingRight = player->m_isSlidingRight;
    state.flags.accelerating = player->m_isAccelerating;
    state.flags.affectedByForces = player->m_affectedByForces;
    state.flags.jumpBuffered = player->m_jumpBuffered;
    state.flags.slopeFlipGravityRelated = player->m_slopeFlipGravityRelated;
    state.flags.buttonHolds[0] = holdButtonHeld(player, 1);
    state.flags.buttonHolds[1] = holdButtonHeld(player, 2);
    state.flags.buttonHolds[2] = holdButtonHeld(player, 3);

    state.environment.gravity = player->m_gravity;
    state.environment.gravityMod = player->m_gravityMod;
    state.environment.playerSpeed = player->m_playerSpeed;
    state.environment.playerSpeedAC = player->m_playerSpeedAC;
    state.environment.speedMultiplier = player->m_speedMultiplier;
    state.environment.vehicleSize = player->m_vehicleSize;
    state.environment.reverseRelated = player->m_reverseRelated;
    state.environment.stateDartSlide = player->m_stateDartSlide;
    state.environment.stateFlipGravity = player->m_stateFlipGravity;
    state.environment.stateForce = player->m_stateForce;
    state.environment.lastFlipTime = player->m_lastFlipTime;
    state.environment.lastSpiderFlipTime = player->m_lastSpiderFlipTime;
    state.environment.dualContext = isDual;
    state.environment.twoPlayerContext = isTwoPlayer;
    state.environment.extendedState = true;
    return state;
}

void PlayerStateRestorer::restoreState(PlayerObject* player, PlayerStateBundle const& state) {
    if (!player) {
        return;
    }

    if (state.environment.extendedState) {
        if (player->m_isDashing && !state.flags.dashing) {
            player->stopDashing();
        }
        reconcileVehicleVisual(player, state);
    }

    player->setPosition(state.motion.position);
    player->setRotation(state.motion.rotation);
    player->m_yVelocity = state.motion.verticalVelocity;
    player->m_yVelocityBeforeSlope = state.motion.preSlopeVerticalVelocity;
    if (state.flags.platformer) {
        player->m_platformerXVelocity = state.motion.horizontalVelocity;
    }

    player->m_isUpsideDown = state.flags.upsideDown;
    player->m_holdingLeft = state.flags.holdingLeft;
    player->m_holdingRight = state.flags.holdingRight;
    player->m_isDead = state.flags.dead;
    setHoldButton(player, 1, state.flags.buttonHolds[0]);
    setHoldButton(player, 2, state.flags.buttonHolds[1]);
    setHoldButton(player, 3, state.flags.buttonHolds[2]);

    if (state.environment.gravity != 0.0) {
        player->m_gravity = state.environment.gravity;
    }

    if (state.environment.extendedState) {
        player->m_dashX = state.motion.dashX;
        player->m_dashY = state.motion.dashY;
        player->m_dashAngle = state.motion.dashAngle;
        player->m_dashStartTime = state.motion.dashStartTime;
        player->m_slopeStartTime = state.motion.slopeStartTime;
        player->m_fallSpeed = state.motion.fallSpeed;
        player->m_slopeVelocity = state.motion.slopeVelocity;
        player->m_shipRotation = state.motion.shipRotation;
        player->m_lastPortalPos = state.motion.lastPortalPosition;
        player->m_stateForceVector = state.motion.stateForceVector;

        player->m_isShip = state.flags.ship;
        player->m_isBird = state.flags.bird;
        player->m_isBall = state.flags.ball;
        player->m_isDart = state.flags.wave;
        player->m_isRobot = state.flags.robot;
        player->m_isSpider = state.flags.spider;
        player->m_isSwing = state.flags.swing;
        player->m_isSideways = state.flags.sideways;
        player->m_isDashing = state.flags.dashing;
        player->m_isOnSlope = state.flags.onSlope;
        player->m_wasOnSlope = state.flags.wasOnSlope;
        player->m_isOnGround = state.flags.onGround;
        player->m_isGoingLeft = state.flags.goingLeft;
        player->m_platformerMovingRight = state.flags.platformerMovingRight;
        player->m_isSlidingRight = state.flags.slidingRight;
        player->m_isAccelerating = state.flags.accelerating;
        player->m_affectedByForces = state.flags.affectedByForces;
        player->m_slopeFlipGravityRelated = state.flags.slopeFlipGravityRelated;
        // m_jumpBuffered is NOT restored from the snapshot. Vanilla GD clears
        // it via releaseAllButtons() during loadFromCheckpoint; the snapshot
        // was captured mid-input (jump buffered while holding the button), so
        // restoring it re-latches "jump on next ground touch" and the player
        // auto-jumps forever after a practice respawn (hold shows 0, yet the
        // character jumps by itself). The live attempt must start input-clean.

        player->m_gravityMod = state.environment.gravityMod;
        player->m_playerSpeed = state.environment.playerSpeed;
        player->m_playerSpeedAC = state.environment.playerSpeedAC;
        player->m_speedMultiplier = state.environment.speedMultiplier;
        player->m_vehicleSize = state.environment.vehicleSize;
        player->m_reverseRelated = state.environment.reverseRelated;
        player->m_stateDartSlide = state.environment.stateDartSlide;
        player->m_stateFlipGravity = state.environment.stateFlipGravity;
        player->m_stateForce = state.environment.stateForce;
        player->m_lastFlipTime = state.environment.lastFlipTime;
        player->m_lastSpiderFlipTime = state.environment.lastSpiderFlipTime;
    }
}

float PlayerStateRestorer::positionalDrift(PlayerObject* player, PlayerStateBundle const& state) {
    if (!player) {
        return 0.0f;
    }

    auto position = player->getPosition();
    float dx = position.x - state.motion.position.x;
    float dy = position.y - state.motion.position.y;
    return std::sqrt(dx * dx + dy * dy);
}

float PlayerStateRestorer::rotationDrift(PlayerObject* player, PlayerStateBundle const& state) {
    if (!player) {
        return 0.0f;
    }

    return std::abs(player->getRotation() - state.motion.rotation);
}

float PlayerStateRestorer::velocityDrift(PlayerObject* player, PlayerStateBundle const& state) {
    if (!player) {
        return 0.0f;
    }

    double yDiff = std::abs(player->m_yVelocity - state.motion.verticalVelocity);
    double slopeDiff = std::abs(player->m_yVelocityBeforeSlope - state.motion.preSlopeVerticalVelocity);
    double xDiff = state.flags.platformer
        ? std::abs(player->m_platformerXVelocity - state.motion.horizontalVelocity)
        : 0.0;
    return static_cast<float>(std::max({ yDiff, slopeDiff, xDiff }));
}

bool PlayerStateRestorer::needsReconciliation(
    PlayerObject* player,
    PlayerStateBundle const& state,
    float positionTolerance,
    float rotationTolerance,
    float velocityTolerance
) {
    if (!player) {
        return false;
    }

    if (positionalDrift(player, state) > positionTolerance) {
        return true;
    }
    if (rotationDrift(player, state) > rotationTolerance) {
        return true;
    }
    if (velocityDrift(player, state) > velocityTolerance) {
        return true;
    }
    if (player->m_isUpsideDown != state.flags.upsideDown) {
        return true;
    }
    if (!state.environment.extendedState) {
        return false;
    }
    return player->m_isShip != state.flags.ship
        || player->m_isBird != state.flags.bird
        || player->m_isBall != state.flags.ball
        || player->m_isDart != state.flags.wave
        || player->m_isRobot != state.flags.robot
        || player->m_isSpider != state.flags.spider
        || player->m_isSwing != state.flags.swing
        || player->m_isSideways != state.flags.sideways
        || player->m_isDashing != state.flags.dashing
        || player->m_isGoingLeft != state.flags.goingLeft;
}

bool PlayerStateRestorer::needsModeReconciliation(
    PlayerObject* player,
    PlayerStateBundle const& state
) {
    // Structural divergence only: vehicle mode, gravity/level orientation,
    // death. Position/rotation/velocity are deliberately EXCLUDED - they are
    // either pinned per-tick by the per-frame stream or derived by GD's own
    // physics from replayed inputs (GDH model). Restoring the full bundle on
    // mere positional drift fought GD's sub-step state machines and killed the
    // dash-ring hold animation and spider-sprite orientation.
    if (!player) {
        return false;
    }
    if (player->m_isDead != state.flags.dead) {
        return true;
    }
    if (player->m_isUpsideDown != state.flags.upsideDown) {
        return true;
    }
    if ((player->m_gravity < 0.0) != (state.environment.gravity < 0.0)) {
        return true;
    }
    if (!state.environment.extendedState) {
        return false;
    }
    return player->m_isShip != state.flags.ship
        || player->m_isBird != state.flags.bird
        || player->m_isBall != state.flags.ball
        || player->m_isDart != state.flags.wave
        || player->m_isRobot != state.flags.robot
        || player->m_isSpider != state.flags.spider
        || player->m_isSwing != state.flags.swing
        || player->m_isSideways != state.flags.sideways
        || player->m_isDashing != state.flags.dashing
        || player->m_isGoingLeft != state.flags.goingLeft;
}

PlaybackAnchor AnchorReconciler::captureAnchor(
    int tick,
    GJBaseGameLayer* playLayer,
    PlayerObject* player1,
    PlayerObject* player2,
    AnchorRngState const& rng
) {
    PlaybackAnchor anchor;
    bool isPlatformer = playLayer && playLayer->m_levelSettings
        ? playLayer->m_levelSettings->m_platformerMode
        : false;
    bool isTwoPlayer = playLayer && playLayer->m_levelSettings
        ? playLayer->m_levelSettings->m_twoPlayerMode
        : false;
    bool isDual = playLayer
        ? (playLayer->m_gameState.m_isDualMode || isTwoPlayer)
        : false;

    anchor.tick = tick;
    anchor.hasPlayer2 = isDual;
    anchor.rng = rng;
    anchor.player1 = PlayerStateRestorer::captureState(player1, isPlatformer, isDual, isTwoPlayer);
    anchor.player1LatchMask = InputStateRestorer::captureLatchMask(player1);
    if (isDual) {
        anchor.player2 = PlayerStateRestorer::captureState(player2, isPlatformer, isDual, isTwoPlayer);
        anchor.player2LatchMask = InputStateRestorer::captureLatchMask(player2);
    }
    return anchor;
}

bool AnchorReconciler::reconcile(
    GJBaseGameLayer* playLayer,
    PlayerObject* player1,
    PlayerObject* player2,
    PlaybackAnchor const& anchor,
    bool force,
    bool modeOnly
) {
    bool changed = false;
    bool syncPlayer1 = force || (modeOnly
        ? PlayerStateRestorer::needsModeReconciliation(player1, anchor.player1)
        : PlayerStateRestorer::needsReconciliation(player1, anchor.player1));
    bool syncPlayer2 = anchor.hasPlayer2 && (force || (modeOnly
        ? PlayerStateRestorer::needsModeReconciliation(player2, anchor.player2)
        : PlayerStateRestorer::needsReconciliation(player2, anchor.player2)));
    if (syncPlayer1) {
        PlayerStateRestorer::restoreState(player1, anchor.player1);
        changed = true;
    }

    if (syncPlayer2) {
        PlayerStateRestorer::restoreState(player2, anchor.player2);
        changed = true;
    }

    if (changed) {
        InputStateRestorer::restoreInputState(playLayer, anchor);
    }

    return changed;
}

CheckpointStateBundle CheckpointStateManager::capture(
    int tick,
    int priorTick,
    GJBaseGameLayer* playLayer,
    AnchorRngState const& rng
) {
    CheckpointStateBundle checkpoint;
    checkpoint.tick = tick;
    checkpoint.priorTick = priorTick;
    checkpoint.rng = rng;

    if (!playLayer) {
        return checkpoint;
    }

    bool isPlatformer = playLayer->m_levelSettings && playLayer->m_levelSettings->m_platformerMode;
    bool isTwoPlayer = playLayer->m_levelSettings && playLayer->m_levelSettings->m_twoPlayerMode;
    bool isDual = playLayer->m_gameState.m_isDualMode || isTwoPlayer;

    checkpoint.player1 = PlayerStateRestorer::captureState(playLayer->m_player1, isPlatformer, isDual, isTwoPlayer);
    checkpoint.player2 = PlayerStateRestorer::captureState(playLayer->m_player2, isPlatformer, isDual, isTwoPlayer);
    checkpoint.player1LatchMask = InputStateRestorer::captureLatchMask(playLayer->m_player1);
    checkpoint.player2LatchMask = InputStateRestorer::captureLatchMask(playLayer->m_player2);

    size_t varianceIndex = 0;
    for (const float& value : playLayer->m_varianceValues) {
        if (value != 0.0f && std::isfinite(value) && varianceIndex < playLayer->m_varianceValues.size()) {
            checkpoint.variance.emplace_back(static_cast<uint16_t>(varianceIndex), value);
        }
        ++varianceIndex;
    }

    auto* autoclicker = Autoclicker::get();
    checkpoint.timedAutoclickerActive = autoclicker->enabled && autoclicker->isTimedMode();
    checkpoint.timedAutoclicker[0] = {
        autoclicker->captureTimedPlayerState(false).holding,
        autoclicker->captureTimedPlayerState(false).userHolding,
        autoclicker->captureTimedPlayerState(false).timeUntilNextEdge
    };
    checkpoint.timedAutoclicker[1] = {
        autoclicker->captureTimedPlayerState(true).holding,
        autoclicker->captureTimedPlayerState(true).userHolding,
        autoclicker->captureTimedPlayerState(true).timeUntilNextEdge
    };

    // --- Practice-fix extended capture ---
    checkpoint.hasFullState = true;

    // Full P1 state
    auto storeFullP1 = [&](auto& fp, PlayerObject* p) {
        if (!p) return;
        fp.p_x = p->m_position.x;
        fp.p_y = p->m_position.y;
        fp.p_rotation = p->getRotation();
        fp.yVelocity = p->m_yVelocity;
        fp.fallSpeed = p->m_fallSpeed;
        fp.platformerXVelocity = p->m_platformerXVelocity;
        fp.groundYVelocity = p->m_groundYVelocity;
        fp.yVelocityBeforeSlope = p->m_yVelocityBeforeSlope;
        fp.yVelocityRelated = p->m_yVelocityRelated;
        fp.scaleXRelated = p->m_scaleXRelated;
        fp.scaleXRelated2 = p->m_scaleXRelated2;
        fp.scaleXRelated3 = p->m_scaleXRelated3;
        fp.scaleXRelated4 = p->m_scaleXRelated4;
        fp.scaleXRelated5 = p->m_scaleXRelated5;
        fp.scaleXRelatedTime = p->m_scaleXRelatedTime;
        fp.playerSpeed = p->m_playerSpeed;
        fp.vehicleSize = p->m_vehicleSize;
        fp.platformerVelocityRelated = p->m_platformerVelocityRelated;
        fp.xVelocityRelated = p->m_xVelocityRelated;
        fp.xVelocityRelated2 = p->m_xVelocityRelated2;
        fp.yVelocityRelated3 = p->m_yVelocityRelated3;
        fp.slopeVelocity = p->m_slopeVelocity;
        fp.gravity = p->m_gravity;
        fp.speedMultiplier = p->m_speedMultiplier;
        fp.snapDistance = p->m_snapDistance;
        fp.accelerationOrSpeed = p->m_accelerationOrSpeed;
        fp.yStart = p->m_yStart;
        fp.gravityMod = p->m_gravityMod;
        fp.slopeAngle = p->m_slopeAngle;
        fp.slopeAngleRadians = p->m_slopeAngleRadians;
        fp.slopeRotation = p->m_slopeRotation;
        fp.currentSlopeYVelocity = p->m_currentSlopeYVelocity;
        fp.slopeStartTime = p->m_slopeStartTime;
        fp.dashX = p->m_dashX;
        fp.dashY = p->m_dashY;
        fp.dashAngle = p->m_dashAngle;
        fp.dashStartTime = p->m_dashStartTime;
        fp.totalTime = p->m_totalTime;
        fp.lastCheckpointTime = p->m_lastCheckpointTime;
        fp.lastJumpTime = p->m_lastJumpTime;
        fp.lastFlipTime = p->m_lastFlipTime;
        fp.lastSpiderFlipTime = p->m_lastSpiderFlipTime;
        fp.gameModeChangedTime = p->m_gameModeChangedTime;
        fp.lastLandTime = p->m_lastLandTime;
        fp.lastCollisionBottom = p->m_lastCollisionBottom;
        fp.lastCollisionTop = p->m_lastCollisionTop;
        fp.lastCollisionLeft = p->m_lastCollisionLeft;
        fp.lastCollisionRight = p->m_lastCollisionRight;
        fp.stateOnGround = p->m_stateOnGround;
        fp.stateForce = p->m_stateForce;
        fp.stateFlipGravity = p->m_stateFlipGravity;
        fp.reverseRelated = p->m_reverseRelated;
        fp.holdingRight = p->m_holdingRight;
        fp.holdingLeft = p->m_holdingLeft;

        // Pack key flags
        uint64_t f1 = 0;
        if (p->m_isShip) f1 |= 1ull << 0;
        if (p->m_isBird) f1 |= 1ull << 1;
        if (p->m_isBall) f1 |= 1ull << 2;
        if (p->m_isDart) f1 |= 1ull << 3;
        if (p->m_isRobot) f1 |= 1ull << 4;
        if (p->m_isSpider) f1 |= 1ull << 5;
        if (p->m_isUpsideDown) f1 |= 1ull << 6;
        if (p->m_isSwing) f1 |= 1ull << 7;
        if (p->m_isDead) f1 |= 1ull << 8;
        if (p->m_isOnGround) f1 |= 1ull << 9;
        if (p->m_isGoingLeft) f1 |= 1ull << 10;
        if (p->m_isSideways) f1 |= 1ull << 11;
        if (p->m_isDashing) f1 |= 1ull << 12;
        if (p->m_isSlidingRight) f1 |= 1ull << 13;
        if (p->m_isOnSlope) f1 |= 1ull << 14;
        if (p->m_wasOnSlope) f1 |= 1ull << 15;
        if (p->m_isAccelerating) f1 |= 1ull << 16;
        if (p->m_affectedByForces) f1 |= 1ull << 17;
        if (p->m_jumpBuffered) f1 |= 1ull << 18;
        if (p->m_isMoving) f1 |= 1ull << 19;
        if (p->m_platformerMovingRight) f1 |= 1ull << 20;
        if (p->m_platformerMovingLeft) f1 |= 1ull << 21;
        if (p->m_isSliding) f1 |= 1ull << 22;
        if (p->m_isOnIce) f1 |= 1ull << 23;
        if (p->m_isOnGround4) f1 |= 1ull << 24;
        if (p->m_isPlatformer) f1 |= 1ull << 25;
        if (p->m_controlsDisabled) f1 |= 1ull << 26;
        if (p->m_isLocked) f1 |= 1ull << 27;
        if (p->m_hasEverJumped) f1 |= 1ull << 28;
        if (p->m_isBeingSpawnedByDualPortal) f1 |= 1ull << 29;
        if (p->m_isSecondPlayer) f1 |= 1ull << 30;
        if (p->m_maybeIsBoosted) f1 |= (1ull << 31);
        fp.flags1 = f1;

        uint64_t f2 = 0;
        if (p->m_wasRobotJump) f2 |= 1ull << 0;
        if (p->m_wasJumpBuffered) f2 |= 1ull << 1;
        if (p->m_touchedRing) f2 |= 1ull << 2;
        if (p->m_touchedCustomRing) f2 |= 1ull << 3;
        if (p->m_touchedGravityPortal) f2 |= 1ull << 4;
        if (p->m_touchedPad) f2 |= 1ull << 5;
        if (p->m_maybeTouchedBreakableBlock) f2 |= 1ull << 6;
        if (p->m_isCollidingWithSlope) f2 |= 1ull << 7;
        if (p->m_slopeFlipGravityRelated) f2 |= 1ull << 8;
        if (p->m_quickCheckpointMode) f2 |= 1ull << 9;
        if (p->m_hasGlow) f2 |= 1ull << 10;
        if (p->m_isHidden) f2 |= 1ull << 11;
        if (p->m_useLandParticles0) f2 |= 1ull << 12;
        if (p->m_canPlaceCheckpoint) f2 |= 1ull << 13;
        if (p->m_fadeOutStreak) f2 |= 1ull << 14;
        if (p->m_maybeIsColliding) f2 |= 1ull << 15;
        if (p->m_isRotating) f2 |= 1ull << 16;
        if (p->m_isBallRotating) f2 |= 1ull << 17;
        if (p->m_isBallRotating2) f2 |= 1ull << 18;
        fp.flags2 = f2;
    };

    storeFullP1(checkpoint.fullP1, playLayer->m_player1);
    if (isDual && playLayer->m_player2) {
        storeFullP1(checkpoint.fullP2, playLayer->m_player2);
    }

    // BUG-13: the reduced fullP1 set above round-trips the SKK3 wire but omits
    // the SHIP FLIGHT ANGLE (m_shipRotation/m_rotationSpeed), m_fallStartY and
    // m_unkAngle1. A practice respawn on the ship restored the player LEVEL
    // instead of banked, so the trajectory curved differently and the moment
    // had to be redone from scratch. The complete FullPlayerCheckpoint carries
    // every field; it is stored alongside (hasFullPlayer) and applied LAST in
    // restore(), after forceCurrentModeVisual rebuilds the vehicle sprite —
    // that rebuild can reset transforms the reduced apply just pinned, so the
    // full re-pin must run after it.
    checkpoint.fullPlayer1 = FullPlayerCheckpoint::store(playLayer->m_player1);
    if (isDual && playLayer->m_player2) {
        checkpoint.fullPlayer2 = FullPlayerCheckpoint::store(playLayer->m_player2);
        checkpoint.hasFullPlayer2 = true;
    }
    checkpoint.hasFullPlayer = true;

    // RNG state
    checkpoint.seedState = rng.fastRandState;

    // Teleport-portal RNG
    // (accessed via GD base address + offset, stored if available)

    // Broken objects
    checkpoint.brokenObjects = BrokenObjectTracker::get().getBrokenObjects();

    // EMS persistent items
    if (playLayer->m_effectManager) {
        for (auto& [id, count] : playLayer->m_effectManager->m_persistentItemCountMap) {
            checkpoint.persistentItems.emplace_back(id, count);
        }
    }

    // Click release state (6 bools: 3 per player)
    auto isHeld = [](PlayerObject* p, int btn) -> bool {
        if (!p) return false;
        auto it = p->m_holdingButtons.find(btn);
        return it != p->m_holdingButtons.end() && it->second;
    };
    checkpoint.clickReleased[0] = !isHeld(playLayer->m_player1, 1);
    checkpoint.clickReleased[1] = !isHeld(playLayer->m_player1, 2);
    checkpoint.clickReleased[2] = !isHeld(playLayer->m_player1, 3);
    checkpoint.clickReleased[3] = !isHeld(playLayer->m_player2, 1);
    checkpoint.clickReleased[4] = !isHeld(playLayer->m_player2, 2);
    checkpoint.clickReleased[5] = !isHeld(playLayer->m_player2, 3);

    // Deep Practice Fix: GJBaseGameLayer / PlayLayer physics and collision block state
    checkpoint.player1CollisionBlock = playLayer->m_player1CollisionBlock;
    checkpoint.player2CollisionBlock = playLayer->m_player2CollisionBlock;
    checkpoint.extraDelta = playLayer->m_extraDelta;
    checkpoint.unk32d0 = playLayer->m_unk32d0;
    checkpoint.unk32ec = playLayer->m_unk32ec;
    checkpoint.currentStep = playLayer->m_currentStep;
    checkpoint.unk3380 = playLayer->m_unk3380;
    checkpoint.gameObjectPhysics = playLayer->m_gameState.m_gameObjectPhysics;

    return checkpoint;
}

void CheckpointStateManager::restore(GJBaseGameLayer* playLayer, CheckpointStateBundle const& checkpoint) {
    if (!playLayer) {
        return;
    }

    // Deep Practice Fix: restore PlayLayer physics/collision states
    playLayer->m_player1CollisionBlock = checkpoint.player1CollisionBlock;
    playLayer->m_player2CollisionBlock = checkpoint.player2CollisionBlock;
    playLayer->m_extraDelta = checkpoint.extraDelta;
    playLayer->m_unk32d0 = checkpoint.unk32d0;
    playLayer->m_unk32ec = checkpoint.unk32ec;
    playLayer->m_currentStep = checkpoint.currentStep;
    playLayer->m_unk3380 = checkpoint.unk3380;
    if (!checkpoint.gameObjectPhysics.empty()) {
        playLayer->m_gameState.m_gameObjectPhysics = checkpoint.gameObjectPhysics;
    }

    PlayerStateRestorer::restoreState(playLayer->m_player1, checkpoint.player1);
    if (playLayer->m_player2) {
        PlayerStateRestorer::restoreState(playLayer->m_player2, checkpoint.player2);
    }
    InputStateRestorer::restoreInputState(playLayer, checkpoint);

    // Rewind the variance table to the checkpoint state: everything not in the
    // snapshot is 'unfilled' again, so lazy rerolls come from the (locked) RNG
    // stream at the same tick - same values as during capture.
    for (float& value : playLayer->m_varianceValues) {
        value = 0.0f;
    }
    for (auto const& entry : checkpoint.variance) {
        if (entry.first < playLayer->m_varianceValues.size()) {
            playLayer->m_varianceValues[entry.first] = entry.second;
        }
    }

    auto* autoclicker = Autoclicker::get();
    if (checkpoint.timedAutoclickerActive) {
        autoclicker->restoreTimedPlayerState(false, {
            checkpoint.timedAutoclicker[0].holding,
            checkpoint.timedAutoclicker[0].userHolding,
            checkpoint.timedAutoclicker[0].timeUntilNextEdge
        });
        autoclicker->restoreTimedPlayerState(true, {
            checkpoint.timedAutoclicker[1].holding,
            checkpoint.timedAutoclicker[1].userHolding,
            checkpoint.timedAutoclicker[1].timeUntilNextEdge
        });
    } else if (autoclicker->isTimedMode()) {
        autoclicker->reset();
    }

    // --- Practice-fix extended restore ---
    if (checkpoint.hasFullState) {
        // Restore full P1 state
        auto applyFullP1 = [](auto& fp, PlayerObject* p) {
            if (!p) return;
            p->m_position = cocos2d::CCPoint(fp.p_x, fp.p_y);
            p->setPosition(p->m_position);
            p->setRotation(fp.p_rotation);
            p->m_yVelocity = fp.yVelocity;
            p->m_fallSpeed = fp.fallSpeed;
            p->m_platformerXVelocity = fp.platformerXVelocity;
            p->m_groundYVelocity = fp.groundYVelocity;
            p->m_yVelocityBeforeSlope = fp.yVelocityBeforeSlope;
            p->m_yVelocityRelated = fp.yVelocityRelated;
            p->m_scaleXRelated = fp.scaleXRelated;
            p->m_scaleXRelated2 = fp.scaleXRelated2;
            p->m_scaleXRelated3 = fp.scaleXRelated3;
            p->m_scaleXRelated4 = fp.scaleXRelated4;
            p->m_scaleXRelated5 = fp.scaleXRelated5;
            p->m_scaleXRelatedTime = fp.scaleXRelatedTime;
            p->m_playerSpeed = fp.playerSpeed;
            p->m_vehicleSize = fp.vehicleSize;
            p->m_platformerVelocityRelated = fp.platformerVelocityRelated;
            p->m_xVelocityRelated = fp.xVelocityRelated;
            p->m_xVelocityRelated2 = fp.xVelocityRelated2;
            p->m_yVelocityRelated3 = fp.yVelocityRelated3;
            p->m_slopeVelocity = fp.slopeVelocity;
            p->m_gravity = fp.gravity;
            p->m_speedMultiplier = fp.speedMultiplier;
            p->m_snapDistance = fp.snapDistance;
            p->m_accelerationOrSpeed = fp.accelerationOrSpeed;
            p->m_yStart = fp.yStart;
            p->m_gravityMod = fp.gravityMod;
            p->m_slopeAngle = fp.slopeAngle;
            p->m_slopeAngleRadians = fp.slopeAngleRadians;
            p->m_slopeRotation = fp.slopeRotation;
            p->m_currentSlopeYVelocity = fp.currentSlopeYVelocity;
            p->m_slopeStartTime = fp.slopeStartTime;
            p->m_dashX = fp.dashX;
            p->m_dashY = fp.dashY;
            p->m_dashAngle = fp.dashAngle;
            p->m_dashStartTime = fp.dashStartTime;
            p->m_totalTime = fp.totalTime;
            p->m_lastCheckpointTime = fp.lastCheckpointTime;
            p->m_lastJumpTime = fp.lastJumpTime;
            p->m_lastFlipTime = fp.lastFlipTime;
            p->m_lastSpiderFlipTime = fp.lastSpiderFlipTime;
            p->m_gameModeChangedTime = fp.gameModeChangedTime;
            p->m_lastLandTime = fp.lastLandTime;
            p->m_lastCollisionBottom = fp.lastCollisionBottom;
            p->m_lastCollisionTop = fp.lastCollisionTop;
            p->m_lastCollisionLeft = fp.lastCollisionLeft;
            p->m_lastCollisionRight = fp.lastCollisionRight;
            p->m_stateOnGround = fp.stateOnGround;
            p->m_stateForce = fp.stateForce;
            p->m_stateFlipGravity = fp.stateFlipGravity;
            p->m_reverseRelated = fp.reverseRelated;
            p->m_holdingRight = fp.holdingRight;
            p->m_holdingLeft = fp.holdingLeft;

            // Unpack flags
            uint64_t f1 = fp.flags1;
            p->m_isShip = (f1 & (1ull << 0)) != 0;
            p->m_isBird = (f1 & (1ull << 1)) != 0;
            p->m_isBall = (f1 & (1ull << 2)) != 0;
            p->m_isDart = (f1 & (1ull << 3)) != 0;
            p->m_isRobot = (f1 & (1ull << 4)) != 0;
            p->m_isSpider = (f1 & (1ull << 5)) != 0;
            p->m_isUpsideDown = (f1 & (1ull << 6)) != 0;
            p->m_isSwing = (f1 & (1ull << 7)) != 0;
            p->m_isDead = (f1 & (1ull << 8)) != 0;
            p->m_isOnGround = (f1 & (1ull << 9)) != 0;
            p->m_isGoingLeft = (f1 & (1ull << 10)) != 0;
            p->m_isSideways = (f1 & (1ull << 11)) != 0;
            p->m_isDashing = (f1 & (1ull << 12)) != 0;
            p->m_isSlidingRight = (f1 & (1ull << 13)) != 0;
            p->m_isOnSlope = (f1 & (1ull << 14)) != 0;
            p->m_wasOnSlope = (f1 & (1ull << 15)) != 0;
            p->m_isAccelerating = (f1 & (1ull << 16)) != 0;
            p->m_affectedByForces = (f1 & (1ull << 17)) != 0;
            p->m_jumpBuffered = (f1 & (1ull << 18)) != 0;
            p->m_isMoving = (f1 & (1ull << 19)) != 0;
            p->m_platformerMovingRight = (f1 & (1ull << 20)) != 0;
            p->m_platformerMovingLeft = (f1 & (1ull << 21)) != 0;
            p->m_isSliding = (f1 & (1ull << 22)) != 0;
            p->m_isOnIce = (f1 & (1ull << 23)) != 0;
            p->m_isOnGround4 = (f1 & (1ull << 24)) != 0;
            p->m_isPlatformer = (f1 & (1ull << 25)) != 0;
            p->m_controlsDisabled = (f1 & (1ull << 26)) != 0;
            p->m_isLocked = (f1 & (1ull << 27)) != 0;
            p->m_hasEverJumped = (f1 & (1ull << 28)) != 0;
            p->m_isBeingSpawnedByDualPortal = (f1 & (1ull << 29)) != 0;
            p->m_isSecondPlayer = (f1 & (1ull << 30)) != 0;
            p->m_maybeIsBoosted = (f1 & (1ull << 31)) != 0;

            uint64_t f2 = fp.flags2;
            p->m_wasRobotJump = (f2 & 1ull) != 0;
            p->m_wasJumpBuffered = (f2 & (1ull << 1)) != 0;
            p->m_touchedRing = (f2 & (1ull << 2)) != 0;
            p->m_touchedCustomRing = (f2 & (1ull << 3)) != 0;
            p->m_touchedGravityPortal = (f2 & (1ull << 4)) != 0;
            p->m_touchedPad = (f2 & (1ull << 5)) != 0;
            p->m_maybeTouchedBreakableBlock = (f2 & (1ull << 6)) != 0;
            p->m_isCollidingWithSlope = (f2 & (1ull << 7)) != 0;
            p->m_slopeFlipGravityRelated = (f2 & (1ull << 8)) != 0;
            p->m_quickCheckpointMode = (f2 & (1ull << 9)) != 0;
            p->m_hasGlow = (f2 & (1ull << 10)) != 0;
            p->m_isHidden = (f2 & (1ull << 11)) != 0;
            p->m_useLandParticles0 = (f2 & (1ull << 12)) != 0;
            p->m_canPlaceCheckpoint = (f2 & (1ull << 13)) != 0;
            p->m_fadeOutStreak = (f2 & (1ull << 14)) != 0;
            p->m_maybeIsColliding = (f2 & (1ull << 15)) != 0;
            p->m_isRotating = (f2 & (1ull << 16)) != 0;
            p->m_isBallRotating = (f2 & (1ull << 17)) != 0;
            p->m_isBallRotating2 = (f2 & (1ull << 18)) != 0;
        };

        applyFullP1(checkpoint.fullP1, playLayer->m_player1);
        // BUG-18 follow-up: fullP2 is only CAPTURED inside a dual/2p context
        // (see store). Applying it whenever m_player2 merely exists wrote a
        // default-constructed bundle — position (0,0), every mode flag false,
        // zero velocity/scale — onto a live player. hasFullPlayer2 is the gate
        // BUG-18 set up; the reduced/full P1 calls below were simply missed.
        if (playLayer->m_player2 && checkpoint.hasFullPlayer2) {
            applyFullP1(checkpoint.fullP2, playLayer->m_player2);
        }

        // NOTE: there is deliberately NO forceCurrentModeVisual() here. It was a
        // local addition no reference bot has (GDH checkpointData.hpp and
        // silicate checkpoint/checkpoint.cpp write flags raw and never
        // deactivate+activate the mode; ToastyReplay-main relies on
        // reconcileVehicleVisual, which runs inside restoreState at the top of
        // this function). Its unconditional toggleRobotMode(false) hid the robot
        // sprite, and nothing ever showed it again — the robot stayed invisible
        // for the rest of the session after a single restore.

        // The complete snapshot re-pins every remaining field (ship angle,
        // rotation speed, fall start, colours, wave trail) after the mode has
        // been reconciled, so the respawn continues the recorded flight exactly.
        if (checkpoint.hasFullPlayer) {
            checkpoint.fullPlayer1.apply(playLayer->m_player1);
            if (playLayer->m_player2 && checkpoint.hasFullPlayer2) {
                checkpoint.fullPlayer2.apply(playLayer->m_player2);
            }
        }

        // Restore broken objects — re-disable them so they stay destroyed
        for (auto* obj : checkpoint.brokenObjects) {
            if (obj) {
                obj->m_isDisabled = true;
                obj->m_isDisabled2 = true;
                obj->setOpacity(0.0f);
            }
        }

        // Restore EMS persistent items
        if (playLayer->m_effectManager) {
            playLayer->m_effectManager->m_persistentItemCountMap.clear();
            for (auto& [id, count] : checkpoint.persistentItems) {
                playLayer->m_effectManager->m_persistentItemCountMap[id] = count;
            }
        }
    }
}
