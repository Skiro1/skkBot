#include "skc_checkpoint.hpp"

#include "skc_bot.hpp"
#include "skkBot.hpp"
#include "core/rng_lock.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <logger/logger.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace skk::skc {

namespace {

// BUG-88: the rotation-only bridge (BUG-87) was proof that GD's
// loadFromCheckpoint drops hidden player state, but the dump of the reference
// bots (docs/Референсы - Изучение.md) shows our field list was far below the
// SAMPLE everyone else restores at a seam. Silicate carries ~230 fields per
// player; Echo restores the same set on top of the vanilla restore; GucciBot
// documents the policy that makes this safe: restore what GD drops, NEVER
// rewrite what GD's own restore already repaired (m_calcNonEffectObjects),
// and never touch position/velocity/ground contact - that overlay is what
// previously bent dy into a y-only drift.
//
// So this helper applies, from the FULL snapshot the SKC capture already took
// every time (CheckpointStateBundle.fullPlayer, silicate-grade), exactly the
// families GD's PlayerCheckpoint is known to not carry:
//   - spin/rotation drivers (m_rotationSpeed/m_rotateSpeed/m_isRotating/
//     m_isBallRotating(bytes in flags1)/m_slopeRotation/m_shipRotation),
//   - the slope family (angle/radians, velocity, per-slope timers and slide
//     forces, currentSlopeYVelocity, identity containers),
//   - collision one-shot filters (m_lastCollision*, m_unk50C/510,
//     m_collided*MinY/MaxY, m_collidingWithSlopeId),
//   - force/stick state (m_stateForce(+Vector), m_stateBoostX/Y, ...),
//   - input-buffer/one-shot flags (jumpBuffered family, touched*Ring/Pad/
//     GravityPortal, stateRingJump*),
//   - the m_totalTime timer family and the mode-identity flags
//     (isGoingLeft/isSideways) plus the live values of m_gravity/
//     m_gravityMod/m_speedMultiplier/m_playerSpeed/m_vehicleSize.
// Every applied value is the value the live world itself carried at the
// capture frame, so a correct bridge is a no-op wherever GD's restore was
// exact, and it can never inject a value the recorded world did not have.
// Deliberately NOT applied: position (m_position/setPosition), velocity
// family (m_yVelocity, m_fallSpeed, m_platformerXVelocity,
// m_groundYVelocity, m_yVelocityBeforeSlope, m_yVelocityRelated,
// xVelocityRelated*, scaleXRelated*), ground contact (m_isOnGround*/isDead,
// mode toggles, colours, wave trail). Those are the fields whose manual
// rewrite corrupted vertical dynamics before.
void bridgePlayerSeamState(PlayerObject& player,
                           const FullPlayerCheckpoint& snap) {
    // --- spin / rotation drivers -------------------------------------------
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

    // --- slope family -------------------------------------------------------
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
    // Identity containers of the slope/rotation lattice: without them the
    // takeoff after a seam reads a rebuilt (empty) map and installs a
    // different rotation base (BUG-87 root cause family).
    if (!snap.potentialSlopeMap.empty()) {
        player.m_potentialSlopeMap = snap.potentialSlopeMap;
    }
    if (!snap.rotateObjectsRelated.empty()) {
        player.m_rotateObjectsRelated = snap.rotateObjectsRelated;
    }

    // --- collision one-shot filters ----------------------------------------
    player.m_lastCollisionBottom = snap.lastCollisionBottom;
    player.m_lastCollisionTop = snap.lastCollisionTop;
    player.m_lastCollisionLeft = snap.lastCollisionLeft;
    player.m_lastCollisionRight = snap.lastCollisionRight;
    player.m_unk50C = snap.unk50C;
    player.m_unk510 = snap.unk510;
    player.m_collidedTopMinY = snap.collidedTopMinY;
    player.m_collidedBottomMaxY = snap.collidedBottomMaxY;
    player.m_collidedLeftMaxX = snap.collidedLeftMaxX;
    player.m_collidedRightMinX = snap.collidedRightMinX;

    // --- force / stick state ------------------------------------------------
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

    // --- input buffer / one-shot activation flags ---------------------------
    // touchedGravityPortal + stateRingJump* are the player-side half of the
    // portal/ring one-shot contract; a dropped flag here activates a portal
    // a step early or late (BUG-87 picture).
    // BUG-105: the buffered-JUMP family is NOT restored from the snapshot. A
    // press buffered at the capture frame belongs to the dying attempt's
    // input tail, which the stitch cuts at the boundary - the restore then
    // "replays" it from the buffer with NO macro event. Seen live today:
    // take restored at f=13517 climbed at vy=+9.6 for ~40 frames while
    // m_holdingButtons stayed empty; closeSeamHolds had already inserted the
    // release at the boundary, so playback (event stream only) had hold
    // down..release and fell - died exactly at f=13527. Playback cannot
    // reproduce a press that is no event; the user contract of 03.11
    // («восстановление с зажатием — убрать», BUG-99 for SKK) covers this
    // same family in the SKC bridge. The user's own re-press after the seam
    // is a normal recorded event and replays fine.
    player.m_jumpBuffered = false;
    player.m_wasJumpBuffered = false;
    player.m_stateJumpBuffered = 0;
    player.m_holdingButtons.clear();
    player.m_stateRingJump = (snap.flags1 & (1ull << 30)) != 0;
    player.m_stateRingJump2 = (snap.flags1 & (1ull << 33)) != 0;
    player.m_ringJumpRelated = snap.ringJumpRelated;
    player.m_touchedRing = (snap.flags1 & (1ull << 34)) != 0;
    player.m_touchedCustomRing = (snap.flags1 & (1ull << 35)) != 0;
    player.m_touchedPad = (snap.flags1 & (1ull << 38)) != 0;
    player.m_touchedGravityPortal = (snap.flags1 & (1ull << 36)) != 0;
    player.m_maybeTouchedBreakableBlock =
        (snap.flags1 & (1ull << 37)) != 0;

    // --- mode identity + live config values ----------------------------------
    // isGoingLeft/isSideways steer the x-delta composition; the config values
    // below are no-ops whenever GD's restore was exact and named repairs when
    // it was not; all of them are the live captured values by construction.
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

    // --- timer family on the m_totalTime scale -------------------------------
    player.m_totalTime = snap.totalTime;
    player.m_gameModeChangedTime = snap.gameModeChangedTime;
    player.m_lastFlipTime = snap.lastFlipTime;
    player.m_lastLandTime = snap.lastLandTime;
    player.m_lastSpiderFlipTime = snap.lastSpiderFlipTime;
    // m_lastJumpTime is deliberately NOT restored: it is a WALL-CLOCK stamp
    // (std::chrono, PlayerObject::updateJump.cpp), not an m_totalTime timer.
    // GD re-derives it live after a restore; writing the recorded attempt's
    // stale wall-clock in resurrects dead windows/cooldowns and makes the
    // bot fail the level (user-verified regression). Same reasoning for
    // m_lastCheckpointTime.
    player.m_flashTime = snap.flashTime;
    player.m_flashDuration = snap.flashDuration;
    player.m_flashDelay = snap.flashDelay;
    player.m_fallStartY = snap.fallStartY;
}

} // namespace

PracticeFix& PracticeFix::get() {
    static PracticeFix instance;
    return instance;
}

void PracticeFix::capture(uint32_t frame, PlayLayer* playLayer,
                          CheckpointObject* checkpoint) {
    if (!playLayer) return;

    // ReplayEngine owns the lock bookkeeping (fast_srand state, locked flag,
    // seed, teleport chain). SKK's own capture call reads it from there
    // (checkpoint_system.cpp:1127-1132) - do the same instead of touching the
    // GD globals directly, or the checkpoint would capture RNG that does not
    // match what playback will actually restore against.
    auto* engine = ReplayEngine::get();
    const AnchorRngState rng = engine ? engine->captureRngState()
                                      : AnchorRngState{};

    SkcCheckpoint cp;
    cp.object = checkpoint;
    // Do NOT add +1 here. SKK does (`checkpoint_system.cpp:1133-1134`) because
    // it derives its tick from `m_levelTime * tickRate`, which at the capture
    // point already reads one behind. SKC's FrameClock ticks after the physics
    // step, so at the same point it already reads the frame the snapshot
    // belongs to. Adding +1 on top double-counts and throws the retry two frames
    // ahead - measured: dx went from 0 to exactly two frames of movement.
    //
    // The snapshot is taken before that frame's physics step runs, so the state
    // it holds is the end of frame `frame - 1`, and the retry continues from
    // `frame - 1` (see onResetEvent / loadFromCheckpoint).
    cp.frame = frame;
    cp.progressFrame =
        checkpoint ? checkpoint->m_gameState.m_currentProgress / 2 : 0;
    // The checkpoint's own level time is the identity that survives platformer
    // backtracking: m_currentProgress is an x coordinate there and moves
    // backwards when the player walks left, so it can name two different
    // snapshots. m_levelTime is monotone within the attempt and is copied into
    // the CheckpointObject, so capture and restore see the same value.
    const double tickRate = engine ? engine->runtimeTickRate() : 240.0;
    const double levelTime =
        checkpoint ? static_cast<double>(checkpoint->m_gameState.m_levelTime)
                   : 0.0;
    cp.timeFrame = static_cast<uint32_t>(std::llround(levelTime * tickRate));
    cp.platformer = playLayer->m_levelSettings &&
                    playLayer->m_levelSettings->m_platformerMode;
    cp.state = CheckpointStateManager::capture(
        static_cast<int>(frame), -1, playLayer, rng);

    // Log before the move - afterwards the object is moved-from.
    // info for the same reason as the restore line above: the per-seam snapshot
    // record is what a multi-checkpoint verification reads back.
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

    // Match the snapshot to the checkpoint GD is restoring. Take the LAST match:
    // a checkpoint placed twice at the same progress must use the newest
    // capture, and the vector is append-ordered.
    //
    // Classic: m_currentProgress/2 is stable. Platformer: it is an x coordinate
    // and walking left moves it backwards, so a restore can name a snapshot the
    // player is not at - match on the checkpoint's own level time instead. The
    // snapshot's own mode picks the key, not the live level's, so a macro can
    // never be matched through a key it was not captured with.
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
        if (best) break;
        const bool match = cp.platformer ? (cp.timeFrame == targetTime)
                                         : (cp.progressFrame == targetProgress);
        if (match) {
            best = &cp;
        }
    }
    if (!best) return std::nullopt;

    // -----------------------------------------------------------------------
    // Vanilla-only restore (Xenon's recipe) measured EXACT on the safe path:
    // rec vs. play agreed bit-for-bit on x/y/vy/levelTime for 400+ frames after
    // a seam. But The Yandere 2 showed the failure the recipe cannot survive:
    // after a cube takeoff from a slope the AIR SPIN base rotated differently
    // in the recording world than in the continuous playback world (a constant
    // 3.1659 deg offset from the takeoff tick on), and ~76 frames later the
    // gravity portal at frame ~2177 rounded its oriented-box overlap one
    // physics step earlier in playback - the player clipped the portal, HALVED
    // his fall velocity (-15.000 -> -7.500, PlayerObject::flipGravity) and
    // died. Position was never different, so no frame correction can see it;
    // the world itself diverged in state the trace does not print.
    //
    // That can only be state GD's own loadFromCheckpoint does not carry. SKK's
    // bundle saves a small set of such fields (the Deep Practice Fix list).
    // Overlaying the WHOLE bundle was what previously bent vertical dynamics
    // into a y-only drift, because it rewrites ~200 player fields including
    // position, velocity family and ground contract - fields GD's restore
    // already restores exactly, and any one landing a hair off shows up in dy.
    // So this bridge is deliberately NARROW: it restores ONLY
    //   - the layer deep fields (never touched by GD's restore):
    //     m_currentStep, m_extraDelta, collision blocks, unk32d0/ec/3380 and
    //     the per-object physics map,
    //   - the session variance table (same zero-then-fill SKK does),
    //   - the locked RNG channels (SKK restores the same values at its seams),
    //   - the player rotation family: m_unkAngle1 is the known GD capsule gap,
    //     and rotation / m_rotationSpeed / m_slopeRotation are rewritten from
    //     the snapshot as a no-op under GD's capsule - or the net when it
    //     dropped one. BUG-88 widened this to the full silicate-grade family
    //     list (slope, collision one-shots, force state, buffer flags,
    //     timers) that the snapshot already carried - see bridgePlayerSeamState.
    // Nothing here writes position, velocity, x, y, or ground contact. Every
    // value applied is the value the live world itself had at the capture
    // frame, so a correct bridge is invisible; the per-seam log below exists
    // so a wrong one names itself instead of hiding behind rotated portals.
    // -----------------------------------------------------------------------
    CheckpointStateBundle& state = best->state;

    PlayerObject* const p1 = playLayer->m_player1;

    struct Tracked {
        // before = what GD's own restore just produced; stored = the live
        // value the world itself had at the capture frame.
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
        // BUG-102 v3 (seam vy-drift meter): the velocity family is
        // deliberately NOT applied by the bridge (rewrite previously bent
        // vertical dynamics). But the f=418 divergence signature of the
        // 04.11 session was a vy mismatch at IDENTICAL x/y/rot right at a
        // seam, so the meter names what GD's own restore produced vs what
        // the live world carried at capture. If these two agree, GD's
        // restore carried the velocity exactly; if they differ, the seam's
        // vy came from GD's own checkpooint restore and the next suspect
        // is the velocity restore chain (not the portal).
        double vyBefore, vyStored;
        double fallSpeedBefore, fallSpeedStored;
        double groundYVelBefore, groundYVelStored;
        double yVelBeforeSlopeBefore, yVelBeforeSlopeStored;
        double yVelRelatedBefore, yVelRelatedStored;
        double platXVelBefore, platXVelStored;
        bool onGround2Before, onGround2Stored;
        float playerVelXBefore, playerVelXStored;
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

    // --- layer deep fields: GD's restore never writes these ----------------
    playLayer->m_player1CollisionBlock = state.player1CollisionBlock;
    playLayer->m_player2CollisionBlock = state.player2CollisionBlock;
    playLayer->m_extraDelta = state.extraDelta;
    playLayer->m_unk32d0 = state.unk32d0;
    playLayer->m_unk32ec = state.unk32ec;
    playLayer->m_currentStep = state.currentStep;
    playLayer->m_unk3380 = state.unk3380;
    if (!state.gameObjectPhysics.empty()) {
        playLayer->m_gameState.m_gameObjectPhysics = state.gameObjectPhysics;
    }

    // --- session variance table: rewind exactly like SKK --------------------
    for (float& value : playLayer->m_varianceValues) {
        value = 0.0f;
    }
    for (auto const& entry : state.variance) {
        if (entry.first < playLayer->m_varianceValues.size()) {
            playLayer->m_varianceValues[entry.first] = entry.second;
        }
    }

    // --- locked RNG channels (checkpoint_system.cpp:1897-1908 pattern) ------
    if (engine) {
        engine->restoreLockedRngState(state.rng.fastRandState);
        engine->restoreLockedShakeState(state.rng.shakeState);
        if (engine->rngLocked && state.rng.teleportState != 0) {
            skk::restoreTripleRng({state.rng.fastRandState,
                                   state.rng.teleportState});
        }
    }

    // --- player rotation family + the one GD capsule drops ------------------
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

    // Per-seam falsification log: "before" is GD's restore result, "snap" is
    // what the world itself carried at capture. Entries where the two agree
    // mean GD's restore is exact; entries that differ are what this bridge
    // just repaired. If a retest still diverges, whichever field here reads
    // WRONG against the "snap" value is the next suspect.
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

    // BUG-88 family log: the widened bridge's representative fields, still
    // before -> after (snap) so a wrong restoration names itself the same way
    // the rotation-only bridge did.
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

    // BUG-102 v3 seam vy-meter: the velocity family is NOT applied by this
    // bridge on purpose (rewriting it previously bent vertical dynamics).
    // Instead this line falsifies GD's own restore: "before" is what
    // GD's loadFromCheckpoint produced in THIS world right now, "snap" is
    // what the live world itself carried at the capture frame. They must
    // agree exactly; any mismatch is the true seam divergence seed (the
    // 04.11 session: vy 3.12 vs 1.56 at identical x/y/rot), and the portal
    // flip-shift is only its downstream symptom.
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

    // info, not debug: seam crossings are the thing a multi-checkpoint run has
    // to be verified against, and debug output does not reach the log file at
    // all, so a level with three checkpoints produced a log with zero mention
    // of any of them. One line per seam is what makes "did they join cleanly"
    // answerable from the log afterwards.
    skkbot2::log::info(
        "[SKC] practice restore (vanilla state + seam bridge): "
        "frame={} progress={} timeFrame={} mode={} snapshots={}",
        best->frame, best->progressFrame, best->timeFrame,
        best->platformer ? "platformer" : "classic", m_checkpoints.size());

    return best->frame;
}

void PracticeFix::clear() {
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

} // namespace skk::skc
