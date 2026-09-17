#include "core/practice_fix_bridge.hpp"
#include "core/broken_object_tracker.hpp"
#include "logger/logger.hpp"
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace skk::bridge {

static void copyFullP1ToWire(
    CheckpointStateBundle::FullP1 const& fp,
    SkPracticeFixCheckpoint& w
) {
    w.p1_x = fp.p_x; w.p1_y = fp.p_y; w.p1_rotation = fp.p_rotation;
    w.p1_yVelocity = fp.yVelocity; w.p1_fallSpeed = fp.fallSpeed;
    w.p1_platformerXVelocity = fp.platformerXVelocity; w.p1_groundYVelocity = fp.groundYVelocity;
    w.p1_yVelocityBeforeSlope = fp.yVelocityBeforeSlope; w.p1_yVelocityRelated = fp.yVelocityRelated;
    w.p1_scaleXRelated = fp.scaleXRelated; w.p1_scaleXRelated2 = fp.scaleXRelated2; w.p1_scaleXRelated3 = fp.scaleXRelated3;
    w.p1_scaleXRelated4 = fp.scaleXRelated4; w.p1_scaleXRelated5 = fp.scaleXRelated5;
    w.p1_scaleXRelatedTime = fp.scaleXRelatedTime;
    w.p1_playerSpeed = fp.playerSpeed; w.p1_vehicleSize = fp.vehicleSize;
    w.p1_platformerVelocityRelated = fp.platformerVelocityRelated;
    w.p1_xVelocityRelated = fp.xVelocityRelated; w.p1_xVelocityRelated2 = fp.xVelocityRelated2;
    w.p1_yVelocityRelated3 = fp.yVelocityRelated3; w.p1_slopeVelocity = fp.slopeVelocity;
    w.p1_gravity = fp.gravity; w.p1_speedMultiplier = fp.speedMultiplier;
    w.p1_snapDistance = fp.snapDistance; w.p1_accelerationOrSpeed = fp.accelerationOrSpeed;
    w.p1_yStart = fp.yStart;
    w.p1_gravityMod = fp.gravityMod; w.p1_slopeAngle = fp.slopeAngle; w.p1_slopeAngleRadians = fp.slopeAngleRadians;
    w.p1_slopeRotation = fp.slopeRotation; w.p1_currentSlopeYVelocity = fp.currentSlopeYVelocity;
    w.p1_slopeStartTime = fp.slopeStartTime;
    w.p1_dashX = fp.dashX; w.p1_dashY = fp.dashY; w.p1_dashAngle = fp.dashAngle; w.p1_dashStartTime = fp.dashStartTime;
    w.p1_totalTime = fp.totalTime; w.p1_lastCheckpointTime = fp.lastCheckpointTime;
    w.p1_lastJumpTime = fp.lastJumpTime; w.p1_lastFlipTime = fp.lastFlipTime; w.p1_lastSpiderFlipTime = fp.lastSpiderFlipTime;
    w.p1_gameModeChangedTime = fp.gameModeChangedTime; w.p1_lastLandTime = fp.lastLandTime;
    w.p1_lastCollisionBottom = fp.lastCollisionBottom; w.p1_lastCollisionTop = fp.lastCollisionTop;
    w.p1_lastCollisionLeft = fp.lastCollisionLeft; w.p1_lastCollisionRight = fp.lastCollisionRight;
    w.p1_stateOnGround = fp.stateOnGround; w.p1_stateForce = fp.stateForce; w.p1_reverseRelated = fp.reverseRelated;
    w.p1_flags1 = fp.flags1; w.p1_flags2 = fp.flags2;
    w.p1_holdingRight = fp.holdingRight; w.p1_holdingLeft = fp.holdingLeft;
}

static void copyFullP2ToWire(
    CheckpointStateBundle::FullP2 const& fp,
    SkPracticeFixCheckpoint& w
) {
    w.p2_x = fp.p_x; w.p2_y = fp.p_y; w.p2_rotation = fp.p_rotation;
    w.p2_yVelocity = fp.yVelocity; w.p2_fallSpeed = fp.fallSpeed;
    w.p2_platformerXVelocity = fp.platformerXVelocity; w.p2_groundYVelocity = fp.groundYVelocity;
    w.p2_yVelocityBeforeSlope = fp.yVelocityBeforeSlope; w.p2_yVelocityRelated = fp.yVelocityRelated;
    w.p2_scaleXRelated = fp.scaleXRelated; w.p2_scaleXRelated2 = fp.scaleXRelated2; w.p2_scaleXRelated3 = fp.scaleXRelated3;
    w.p2_scaleXRelated4 = fp.scaleXRelated4; w.p2_scaleXRelated5 = fp.scaleXRelated5;
    w.p2_scaleXRelatedTime = fp.scaleXRelatedTime;
    w.p2_playerSpeed = fp.playerSpeed; w.p2_vehicleSize = fp.vehicleSize;
    w.p2_platformerVelocityRelated = fp.platformerVelocityRelated;
    w.p2_xVelocityRelated = fp.xVelocityRelated; w.p2_xVelocityRelated2 = fp.xVelocityRelated2;
    w.p2_yVelocityRelated3 = fp.yVelocityRelated3; w.p2_slopeVelocity = fp.slopeVelocity;
    w.p2_gravity = fp.gravity; w.p2_speedMultiplier = fp.speedMultiplier;
    w.p2_snapDistance = fp.snapDistance; w.p2_accelerationOrSpeed = fp.accelerationOrSpeed;
    w.p2_yStart = fp.yStart;
    w.p2_gravityMod = fp.gravityMod; w.p2_slopeAngle = fp.slopeAngle; w.p2_slopeAngleRadians = fp.slopeAngleRadians;
    w.p2_slopeRotation = fp.slopeRotation; w.p2_currentSlopeYVelocity = fp.currentSlopeYVelocity;
    w.p2_slopeStartTime = fp.slopeStartTime;
    w.p2_dashX = fp.dashX; w.p2_dashY = fp.dashY; w.p2_dashAngle = fp.dashAngle; w.p2_dashStartTime = fp.dashStartTime;
    w.p2_totalTime = fp.totalTime; w.p2_lastCheckpointTime = fp.lastCheckpointTime;
    w.p2_lastJumpTime = fp.lastJumpTime; w.p2_lastFlipTime = fp.lastFlipTime; w.p2_lastSpiderFlipTime = fp.lastSpiderFlipTime;
    w.p2_gameModeChangedTime = fp.gameModeChangedTime; w.p2_lastLandTime = fp.lastLandTime;
    w.p2_lastCollisionBottom = fp.lastCollisionBottom; w.p2_lastCollisionTop = fp.lastCollisionTop;
    w.p2_lastCollisionLeft = fp.lastCollisionLeft; w.p2_lastCollisionRight = fp.lastCollisionRight;
    w.p2_stateOnGround = fp.stateOnGround; w.p2_stateForce = fp.stateForce; w.p2_reverseRelated = fp.reverseRelated;
    w.p2_flags1 = fp.flags1; w.p2_flags2 = fp.flags2;
    w.p2_holdingRight = fp.holdingRight; w.p2_holdingLeft = fp.holdingLeft;
}

static void copyWireToFullP1(
    SkPracticeFixCheckpoint const& w,
    CheckpointStateBundle::FullP1& fp
) {
    fp.p_x = w.p1_x; fp.p_y = w.p1_y; fp.p_rotation = w.p1_rotation;
    fp.yVelocity = w.p1_yVelocity; fp.fallSpeed = w.p1_fallSpeed;
    fp.platformerXVelocity = w.p1_platformerXVelocity; fp.groundYVelocity = w.p1_groundYVelocity;
    fp.yVelocityBeforeSlope = w.p1_yVelocityBeforeSlope; fp.yVelocityRelated = w.p1_yVelocityRelated;
    fp.scaleXRelated = w.p1_scaleXRelated; fp.scaleXRelated2 = w.p1_scaleXRelated2; fp.scaleXRelated3 = w.p1_scaleXRelated3;
    fp.scaleXRelated4 = w.p1_scaleXRelated4; fp.scaleXRelated5 = w.p1_scaleXRelated5;
    fp.scaleXRelatedTime = w.p1_scaleXRelatedTime;
    fp.playerSpeed = w.p1_playerSpeed; fp.vehicleSize = w.p1_vehicleSize;
    fp.platformerVelocityRelated = w.p1_platformerVelocityRelated;
    fp.xVelocityRelated = w.p1_xVelocityRelated; fp.xVelocityRelated2 = w.p1_xVelocityRelated2;
    fp.yVelocityRelated3 = w.p1_yVelocityRelated3; fp.slopeVelocity = w.p1_slopeVelocity;
    fp.gravity = w.p1_gravity; fp.speedMultiplier = w.p1_speedMultiplier;
    fp.snapDistance = w.p1_snapDistance; fp.accelerationOrSpeed = w.p1_accelerationOrSpeed;
    fp.yStart = w.p1_yStart;
    fp.gravityMod = w.p1_gravityMod; fp.slopeAngle = w.p1_slopeAngle; fp.slopeAngleRadians = w.p1_slopeAngleRadians;
    fp.slopeRotation = w.p1_slopeRotation; fp.currentSlopeYVelocity = w.p1_currentSlopeYVelocity;
    fp.slopeStartTime = w.p1_slopeStartTime;
    fp.dashX = w.p1_dashX; fp.dashY = w.p1_dashY; fp.dashAngle = w.p1_dashAngle; fp.dashStartTime = w.p1_dashStartTime;
    fp.totalTime = w.p1_totalTime; fp.lastCheckpointTime = w.p1_lastCheckpointTime;
    fp.lastJumpTime = w.p1_lastJumpTime; fp.lastFlipTime = w.p1_lastFlipTime; fp.lastSpiderFlipTime = w.p1_lastSpiderFlipTime;
    fp.gameModeChangedTime = w.p1_gameModeChangedTime; fp.lastLandTime = w.p1_lastLandTime;
    fp.lastCollisionBottom = w.p1_lastCollisionBottom; fp.lastCollisionTop = w.p1_lastCollisionTop;
    fp.lastCollisionLeft = w.p1_lastCollisionLeft; fp.lastCollisionRight = w.p1_lastCollisionRight;
    fp.stateOnGround = w.p1_stateOnGround; fp.stateForce = w.p1_stateForce; fp.reverseRelated = w.p1_reverseRelated;
    fp.flags1 = w.p1_flags1; fp.flags2 = w.p1_flags2;
    fp.holdingRight = w.p1_holdingRight; fp.holdingLeft = w.p1_holdingLeft;
}

static void copyWireToFullP2(
    SkPracticeFixCheckpoint const& w,
    CheckpointStateBundle::FullP2& fp
) {
    fp.p_x = w.p2_x; fp.p_y = w.p2_y; fp.p_rotation = w.p2_rotation;
    fp.yVelocity = w.p2_yVelocity; fp.fallSpeed = w.p2_fallSpeed;
    fp.platformerXVelocity = w.p2_platformerXVelocity; fp.groundYVelocity = w.p2_groundYVelocity;
    fp.yVelocityBeforeSlope = w.p2_yVelocityBeforeSlope; fp.yVelocityRelated = w.p2_yVelocityRelated;
    fp.scaleXRelated = w.p2_scaleXRelated; fp.scaleXRelated2 = w.p2_scaleXRelated2; fp.scaleXRelated3 = w.p2_scaleXRelated3;
    fp.scaleXRelated4 = w.p2_scaleXRelated4; fp.scaleXRelated5 = w.p2_scaleXRelated5;
    fp.scaleXRelatedTime = w.p2_scaleXRelatedTime;
    fp.playerSpeed = w.p2_playerSpeed; fp.vehicleSize = w.p2_vehicleSize;
    fp.platformerVelocityRelated = w.p2_platformerVelocityRelated;
    fp.xVelocityRelated = w.p2_xVelocityRelated; fp.xVelocityRelated2 = w.p2_xVelocityRelated2;
    fp.yVelocityRelated3 = w.p2_yVelocityRelated3; fp.slopeVelocity = w.p2_slopeVelocity;
    fp.gravity = w.p2_gravity; fp.speedMultiplier = w.p2_speedMultiplier;
    fp.snapDistance = w.p2_snapDistance; fp.accelerationOrSpeed = w.p2_accelerationOrSpeed;
    fp.yStart = w.p2_yStart;
    fp.gravityMod = w.p2_gravityMod; fp.slopeAngle = w.p2_slopeAngle; fp.slopeAngleRadians = w.p2_slopeAngleRadians;
    fp.slopeRotation = w.p2_slopeRotation; fp.currentSlopeYVelocity = w.p2_currentSlopeYVelocity;
    fp.slopeStartTime = w.p2_slopeStartTime;
    fp.dashX = w.p2_dashX; fp.dashY = w.p2_dashY; fp.dashAngle = w.p2_dashAngle; fp.dashStartTime = w.p2_dashStartTime;
    fp.totalTime = w.p2_totalTime; fp.lastCheckpointTime = w.p2_lastCheckpointTime;
    fp.lastJumpTime = w.p2_lastJumpTime; fp.lastFlipTime = w.p2_lastFlipTime; fp.lastSpiderFlipTime = w.p2_lastSpiderFlipTime;
    fp.gameModeChangedTime = w.p2_gameModeChangedTime; fp.lastLandTime = w.p2_lastLandTime;
    fp.lastCollisionBottom = w.p2_lastCollisionBottom; fp.lastCollisionTop = w.p2_lastCollisionTop;
    fp.lastCollisionLeft = w.p2_lastCollisionLeft; fp.lastCollisionRight = w.p2_lastCollisionRight;
    fp.stateOnGround = w.p2_stateOnGround; fp.stateForce = w.p2_stateForce; fp.reverseRelated = w.p2_reverseRelated;
    fp.flags1 = w.p2_flags1; fp.flags2 = w.p2_flags2;
    fp.holdingRight = w.p2_holdingRight; fp.holdingLeft = w.p2_holdingLeft;
}

SkPracticeFixCheckpoint bundleToWire(
    CheckpointStateBundle const& bundle,
    double tps
) {
    skkbot2::log::debug("P1 bridge: bundleToWire tick={} priorTick={} broken={} variance={} hasFull={}",
        bundle.tick, bundle.priorTick,
        bundle.brokenObjects.size(), bundle.variance.size(), bundle.hasFullState);
    SkPracticeFixCheckpoint w;
    w.tick = bundle.tick;
    w.priorTick = bundle.priorTick;
    w.timeSeconds = (tps > 0.0) ? static_cast<double>(bundle.tick) / tps : 0.0;
    w.priorTimeSeconds = (tps > 0.0) ? static_cast<double>(bundle.priorTick) / tps : 0.0;
    w.player1LatchMask = bundle.player1LatchMask;
    w.player2LatchMask = bundle.player2LatchMask;
    w.seedState = bundle.seedState;
    w.shakeRandomState = bundle.shakeRandomState;
    w.teleportRandomState = bundle.teleportRandomState;
    w.persistentItems = bundle.persistentItems;
    w.variance = bundle.variance;
    for (int i = 0; i < 6; ++i) w.clickReleased[i] = bundle.clickReleased[i];

    if (bundle.hasFullState) {
        copyFullP1ToWire(bundle.fullP1, w);
        copyFullP2ToWire(bundle.fullP2, w);
    }

    for (auto* obj : bundle.brokenObjects) {
        if (obj) {
            w.brokenObjectUIDs.push_back(static_cast<int32_t>(obj->m_objectID));
        }
    }

    return w;
}

CheckpointStateBundle wireToBundle(
    SkPracticeFixCheckpoint const& wire,
    double tps
) {
    CheckpointStateBundle bundle;
    bundle.tick = wire.tick;
    bundle.priorTick = wire.priorTick;
    bundle.player1LatchMask = wire.player1LatchMask;
    bundle.player2LatchMask = wire.player2LatchMask;
    bundle.seedState = wire.seedState;
    bundle.shakeRandomState = wire.shakeRandomState;
    bundle.teleportRandomState = wire.teleportRandomState;
    bundle.persistentItems = wire.persistentItems;
    bundle.variance = wire.variance;
    for (int i = 0; i < 6; ++i) bundle.clickReleased[i] = wire.clickReleased[i];

    bundle.hasFullState = true;
    copyWireToFullP1(wire, bundle.fullP1);
    copyWireToFullP2(wire, bundle.fullP2);

    if (auto* pl = PlayLayer::get()) {
        int resolved = 0;
        int unresolved = 0;
        for (int32_t uid : wire.brokenObjectUIDs) {
            bool found = false;
            if (pl->m_objects) {
                auto* arr = pl->m_objects;
                for (unsigned i = 0; i < arr->count(); ++i) {
                    auto* obj = static_cast<GameObject*>(arr->objectAtIndex(i));
                    if (obj && obj->m_objectID == uid) {
                        bundle.brokenObjects.push_back(obj);
                        found = true;
                        break;
                    }
                }
            }
            if (found) ++resolved; else ++unresolved;
        }
        skkbot2::log::debug("P1 bridge: wireToBundle tick={} brokenUIDs={} resolved={} unresolved={}",
            wire.tick, wire.brokenObjectUIDs.size(), resolved, unresolved);
    }

    return bundle;
}

std::vector<SkPracticeFixCheckpoint> captureRestorePointsToWire(
    std::unordered_map<CheckpointObject*, CheckpointStateBundle> const& restorePoints,
    double tps
) {
    std::vector<SkPracticeFixCheckpoint> result;
    result.reserve(restorePoints.size());
    int fullStateCount = 0;
    for (auto const& [cp, bundle] : restorePoints) {
        if (bundle.hasFullState) {
            result.push_back(bundleToWire(bundle, tps));
            ++fullStateCount;
        }
    }
    std::sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
        return a.tick < b.tick;
    });
    skkbot2::log::info("P1 bridge: captureRestorePointsToWire total={} fullState={} sorted",
        restorePoints.size(), fullStateCount);
    return result;
}

std::vector<CheckpointStateBundle> wireToBundles(
    std::vector<SkPracticeFixCheckpoint> const& wires,
    double tps
) {
    std::vector<CheckpointStateBundle> result;
    result.reserve(wires.size());
    for (auto const& wire : wires) {
        result.push_back(wireToBundle(wire, tps));
    }
    skkbot2::log::info("P1 bridge: wireToBundles count={} tps={:.1f}", wires.size(), tps);
    return result;
}

}
