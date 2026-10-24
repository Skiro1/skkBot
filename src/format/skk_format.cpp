#include "skk_format.hpp"
#include "skk3_format.hpp"
#include "core/replay_timing.hpp"
#include "core/paths.hpp"
#include "core/rng_lock.hpp"
#include "utils.hpp"

#include <Geode/utils/file.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <type_traits>

static uint8_t packHoldMask(std::array<bool, 4> const& holds) {
    uint8_t mask = 0;
    for (size_t index = 0; index < holds.size(); ++index) {
        if (holds[index]) {
            mask |= static_cast<uint8_t>(1u << index);
        }
    }
    return mask;
}

static PlayerStateBundle capturePlayerState(PlayerObject* player, bool isPlatformer, bool isDual, bool isTwoPlayer) {
    PlayerStateBundle state;
    auto position = player->getPosition();
    state.motion.position = position;
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
    state.flags.buttonHolds[0] = player->m_holdingButtons[1];
    state.flags.buttonHolds[1] = player->m_holdingButtons[2];
    state.flags.buttonHolds[2] = player->m_holdingButtons[3];
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
    state.environment.dualContext = isDual;
    state.environment.twoPlayerContext = isTwoPlayer;
    state.environment.extendedState = true;
    return state;
}

void SkMacro::recordAction(std::vector<SkInput>& target, int tick, int button, bool player2, bool pressed, float offset, double cbsTimeOffset) {
    SkInput input;
    input.tick = std::max(0, tick);
    // Rewind self-heal (see recordPerFrame): checkpoint respawn restarts the
    // tick clock; drop the future tail to keep on-disk order valid.
    if (!target.empty() && input.tick < target.back().tick) {
        size_t dropped = 0;
        while (!target.empty() && target.back().tick >= input.tick) {
            target.pop_back();
            ++dropped;
        }
        skkbot2::log::debug("CAPTURE rewind: inputs stream dropped {} stale records (tick {} -> {})",
            dropped, input.tick, std::max(0, tick));
    }
    input.actionType = static_cast<uint8_t>(button);
    input.setPlayer2(player2);
    input.setPressed(pressed);
    input.stepOffset = offset;

    double safeFramerate = std::isfinite(framerate) && framerate > 0.0 ? framerate : 240.0;
    double tickDuration = 1.0 / safeFramerate;
    input.timeSeconds = static_cast<double>(std::max(0, tick)) / safeFramerate;
    // Defensive guard: cbsTimeOffset is a within-tick phase and must be < 1
    // tick duration.  A garbage baseline (tickStartTimestamp still zeroed by
    // resetTimingTracking) inflates it to the entire accumulated game clock,
    // breaking timeSeconds monotonicity and producing an on-disk "unsorted
    // inputs" rejection.  Reject the offset when it exceeds a full tick.
    if (std::isfinite(cbsTimeOffset) && cbsTimeOffset >= 0.0 && cbsTimeOffset < tickDuration) {
        input.timeSeconds += cbsTimeOffset;
    } else if (std::isfinite(offset) && offset > 0.0f) {
        input.timeSeconds += static_cast<double>(offset) / safeFramerate;
    }

    if (std::isfinite(cbsTimeOffset) && cbsTimeOffset >= 0.0f && cbsTimeOffset < tickDuration) {
        input.cbsTimeOffset = cbsTimeOffset;
        exactCbsTiming = true;
    }
    target.push_back(input);
}

void SkMacro::recordAction(int tick, int button, bool player2, bool pressed, float offset, double cbsTimeOffset) {
    recordAction(inputs, tick, button, player2, pressed, offset, cbsTimeOffset);
}

void SkMacro::recordAnchor(std::vector<PlaybackAnchor>& target, int tick, PlayerObject* p1, PlayerObject* p2, bool isPlatformer, bool isDual) {
    PlaybackAnchor anchor;
    anchor.tick = std::max(0, tick);
    // Rewind self-heal (see recordPerFrame).
    if (!target.empty() && anchor.tick < target.back().tick) {
        while (!target.empty() && target.back().tick >= anchor.tick) {
            target.pop_back();
        }
    }
    double safeFramerate = std::isfinite(framerate) && framerate > 0.0 ? framerate : 240.0;
    anchor.timeSeconds = static_cast<double>(std::max(0, tick)) / safeFramerate;
    anchor.hasPlayer2 = isDual;
    anchor.player1 = capturePlayerState(p1, isPlatformer, isDual, twoPlayerMode);
    anchor.player1LatchMask = packHoldMask(anchor.player1.flags.buttonHolds);

    if (isDual) {
        anchor.player2 = capturePlayerState(p2, isPlatformer, isDual, twoPlayerMode);
        anchor.player2LatchMask = packHoldMask(anchor.player2.flags.buttonHolds);
    }

    anchor.rng.locked = rngLocked;
    anchor.rng.seed = rngSeed;
    anchor.rng.fastRandState = GameToolbox::getfast_srand();
    anchor.rng.shakeState = skk::shakeStateGet();
    anchor.rng.teleportState = *skk::gdTeleportRng();

    if (!target.empty() && target.back().tick == tick) {
        target.back() = std::move(anchor);
    } else {
        target.push_back(std::move(anchor));
    }
}

void SkMacro::recordAnchor(int tick, PlayerObject* p1, PlayerObject* p2, bool isPlatformer, bool isDual) {
    recordAnchor(anchors, tick, p1, p2, isPlatformer, isDual);
}

// v2 per-frame visual orientation flag bits: now shared via skk_format.hpp
// (kVisual* constants, vehicleModeBits, visualFlagsOf).

void SkMacro::recordPerFrame(std::vector<SkPerFrame>& target, int tick, PlayerObject* p1, PlayerObject* p2) {
    SkPerFrame frame;
    frame.tick = std::max(0, tick);
    // Rewind self-heal: a tick going backwards means the run restarted
    // (checkpoint respawn / practice). Drop the stale tail so the stream
    // stays monotonic on disk (readers hard-reject unsorted per-frames).
    if (!target.empty() && frame.tick < target.back().tick) {
        size_t dropped = 0;
        while (!target.empty() && target.back().tick >= frame.tick) {
            target.pop_back();
            ++dropped;
        }
        skkbot2::log::debug("CAPTURE rewind: per-frame stream dropped {} stale records (tick {} -> {})",
            dropped, frame.tick, std::max(0, tick));
    }
    if (p1) {
        auto pos = p1->getPosition();
        frame.p1x = pos.x;
        frame.p1y = pos.y;
        frame.p1vy = p1->m_yVelocity;
        frame.p1vyb = p1->m_yVelocityBeforeSlope;
        frame.p1rot = p1->getRotation();
        frame.p1vis = visualFlagsOf(p1);
        if (p1->m_dashRing) {
            frame.p1RingId = static_cast<GameObject*>(p1->m_dashRing)->m_objectID;
        }
    }
    // P2 liveness gate (BUG-21): in a 2P/dual level GD keeps m_player2
    // allocated but DORMANT until it actually joins — spawnPlayer2() parents
    // it into the layer, removePlayer2() detaches it. A dormant P2 sits at
    // (0,0) and never moves. Recording it as hasPlayer2=true poisons the
    // stream: on playback, once a dual portal activates the LIVE P2, the
    // pre-phy pin/reconcile yank it back to the recorded (0,0) — a
    // 14000px TELEPORT-SPIKE that kills it. GD's own liveness signal is
    // the parenting, so mirror it here.
    if (p2 && p2->getParent() != nullptr) {
        frame.hasPlayer2 = true;
        auto pos = p2->getPosition();
        frame.p2x = pos.x;
        frame.p2y = pos.y;
        frame.p2vy = p2->m_yVelocity;
        frame.p2vyb = p2->m_yVelocityBeforeSlope;
        frame.p2rot = p2->getRotation();
        frame.p2vis = visualFlagsOf(p2);
        if (p2->m_dashRing) {
            frame.p2RingId = static_cast<GameObject*>(p2->m_dashRing)->m_objectID;
        }
    }
    // BUG-04 study: reporting lives on the playback side (live-vs-recorded
    // mismatches), not here - the naive ud!=gravNeg check was a false positive.
    if (!target.empty() && target.back().tick == frame.tick) {
        target.back() = std::move(frame);
    } else {
        target.push_back(std::move(frame));
    }
    if (!target.empty()) {
        const SkPerFrame& recorded = target.back();
        if (recorded.tick <= 24 && recorded.tick >= 2) {
            skkbot2::log::debug("phase capture: tick={} p1=({:.3f},{:.3f}) p2=({:.3f},{:.3f})",
                recorded.tick, recorded.p1x, recorded.p1y, recorded.p2x, recorded.p2y);
        }
    }
}

void SkMacro::recordPerFrame(int tick, PlayerObject* p1, PlayerObject* p2) {
    recordPerFrame(perFrames, tick, p1, p2);
}

void SkMacro::truncateAfter(int tick) {
    // Inputs: remove >= tick (inclusive).  After truncation closeSeamHoldEdges
    // re-adds a synthetic release for any dangling hold, then syncPhysicalHolds
    // re-presses whatever the user is still holding — both at `tick`.  Using
    // > tick left those fresh seam inputs in the stream; every subsequent death
    // appended another release+press pair at the boundary tick, inflating the
    // input count by +2 per death loop (BUG: inputs never decrease on respawn).
    //
    // PerFrames / anchors: keep > tick (exclusive) so the boundary frame and
    // anchor survive for captureResealBoundaryFrame to rewrite to the respawn
    // position (BUG-06 seam seal).
    while (!inputs.empty() && inputs.back().tick >= tick) {
        inputs.pop_back();
    }
    while (!anchors.empty() && anchors.back().tick > tick) {
        anchors.pop_back();
    }
    while (!perFrames.empty() && perFrames.back().tick > tick) {
        perFrames.pop_back();
    }
}

std::vector<uint8_t> SkMacro::serialize() const {
    return serializeSKK3();
}

std::vector<uint8_t> SkMacro::serializeSKK3() const {
    return skk::skk3::serialize(skk::skk3::fromSkMacro(*this));
}

SkMacro* SkMacro::deserialize(std::vector<uint8_t> const& data) {
    if (data.size() >= 4 && std::memcmp(data.data(), "SKK3", 4) == 0) {
        std::string error;
        auto macro = skk::skk3::deserialize(data, &error);
        if (!macro) {
            skkbot2::log::debug("SKK3 parser rejected replay data: {}", error);
            return nullptr;
        }
        return new SkMacro(skk::skk3::toSkMacro(*macro));
    }

    skkbot2::log::debug("Unsupported replay format: only SKK3 files are supported");
    return nullptr;
}

bool SkMacro::persist() {
    return persistToDirectory(ReplayStorage::getReplayDirectoryPath());
}

double SkMacro::maxSourceTps() const {
    double result = std::isfinite(framerate) && framerate > 0.0 ? framerate : 240.0;
    for (auto const& event : tpsEvents) {
        if (std::isfinite(event.tps) && event.tps > result) {
            result = event.tps;
        }
    }
    return result;
}

void SkMacro::materializeSKK3RuntimeTicks(double runtimeTps) {
    if (!inputs.empty()) {
        auto const& fi = inputs[0];
        skkbot2::log::info("materialize PRE: tps={} firstInput[tick={} time={:.6f} hasAbs={} stepOffset={:.4f} flags=0x{:02X}]",
            runtimeTps, fi.tick, fi.timeSeconds, fi.hasAbsoluteTime(), fi.stepOffset, fi.flags);
    }
    auto applyToInputs = [&](std::vector<SkInput>& target) {
        for (auto& input : target) {
            if (input.hasAbsoluteTime()) {
                input.tick = skk::replay_timing::materializeTickFromTime(input.timeSeconds, runtimeTps);
            }
        }
    };
    auto applyToAnchors = [&](std::vector<PlaybackAnchor>& target) {
        for (auto& anchor : target) {
            if (anchor.hasAbsoluteTime()) {
                anchor.tick = skk::replay_timing::materializeTickFromTime(anchor.timeSeconds, runtimeTps);
            }
        }
    };
    auto applyToPerFrames = [&](std::vector<SkPerFrame>& target) {
        for (auto& frame : target) {
            if (frame.timeSeconds >= 0.0) {
                frame.tick = skk::replay_timing::materializeTickFromTime(frame.timeSeconds, runtimeTps);
            }
        }
    };

    applyToInputs(inputs);
    applyToAnchors(anchors);
    applyToPerFrames(perFrames);

    for (auto& attempt : persistenceAttempts) {
        applyToInputs(attempt.inputs);
        applyToAnchors(attempt.anchors);
        applyToPerFrames(attempt.perFrames);
        if (attempt.hasAbsoluteDeathTime()) {
            attempt.deathTick = skk::replay_timing::materializeTickFromTime(
                attempt.deathTimeSeconds, runtimeTps);
        }
    }
    if (!inputs.empty()) {
        skkbot2::log::info("materialized to runtime {} Hz: inputs={} anchors={} frames={} links={} firstInput[tick={} time={:.6f} stepOffset={:.4f} flags=0x{:02X}] lastInput[tick={}]",
            runtimeTps, inputs.size(), anchors.size(), perFrames.size(), persistenceAttempts.size(),
            inputs[0].tick, inputs[0].timeSeconds, inputs[0].stepOffset, inputs[0].flags,
            inputs.back().tick);
    } else {
        skkbot2::log::info("materialized to runtime {} Hz: inputs={} anchors={} frames={} links={}",
            runtimeTps, inputs.size(), anchors.size(), perFrames.size(), persistenceAttempts.size());
    }
}

bool SkMacro::persistToDirectory(std::filesystem::path const& directory) {
    SkMacro pending = *this;
    pending.author = GJAccountManager::get()->m_username;
    pending.duration = 0.0;
    double safeFramerate = std::isfinite(pending.framerate) && pending.framerate > 0.0
        ? pending.framerate
        : 240.0;
    for (auto const& input : pending.inputs) {
        double inputTime = input.hasAbsoluteTime()
            ? input.timeSeconds
            : static_cast<double>(std::max(0, input.tick)) / safeFramerate;
        if (!input.hasAbsoluteTime() && std::isfinite(input.cbsTimeOffset) && input.cbsTimeOffset >= 0.0) {
            inputTime += input.cbsTimeOffset;
        }
        pending.duration = std::max(pending.duration, inputTime);
    }
    pending.recordTimestamp = static_cast<int64_t>(std::time(nullptr));
    normalizeSkPersistenceTiming(pending);

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        std::filesystem::create_directories(directory, ec);
    }
    if (ec) {
        skkbot2::log::error("Could not prepare replay directory '{}': {}",
            skk::pathToUtf8(directory), ec.message());
        return false;
    }

    // One rule for both engines: an exact name+extension match gets _1.
    pending.name = ReplayStorage::makeUniqueReplayNameInDirectory(
        directory,
        pending.name,
        ".skk"
    );
    pending.persistedName = pending.name;

    pending.fileFormat = SkFileFormat::SKK3;
    auto outputPath = directory / (pending.name + ".skk");
    if (!pending.saveToPath(outputPath)) {
        return false;
    }
    *this = std::move(pending);
    return true;
}

bool SkMacro::saveToPath(std::filesystem::path const& path) {
    std::error_code ec;
    if (auto parent = path.parent_path(); !parent.empty() && !std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
    }
    if (ec) {
        skkbot2::log::error("Could not prepare replay path '{}': {}",
            skk::pathToUtf8(path), ec.message());
        return false;
    }

    auto bytes = serialize();
    if (bytes.empty()) {
        skkbot2::log::error("Could not save replay '{}': serialization produced no data", skk::pathToUtf8(path));
        return false;
    }

    auto writeResult = utils::file::writeBinarySafe(path, bytes);
    if (!writeResult) {
        skkbot2::log::error("Could not write replay file '{}': {}",
            skk::pathToUtf8(path), writeResult.unwrapErr());
        return false;
    }

    skkbot2::log::info(
        "SAVED '{}' bytes={} | main: inputs={} anchors={} frames={} | chainLinks={} varianceSlots={} setupSeed=0x{:X}",
        skk::pathToUtf8(path),
        bytes.size(),
        inputs.size(),
        anchors.size(),
        perFrames.size(),
        persistenceAttempts.size(),
        varianceOverrides.size(),
        setupRngSeed
    );
    if (!perFrames.empty() && perFrames.front().tick > 120) {
        skkbot2::log::warn("NOTE: first recorded frame is tick {} (~{:.1f}s) at p1=({:.0f},{:.0f}) - capture started MID-RUN; "
                           "playback will POSITION-ALIGN to wherever the player is, not assume a tick-0 timeline "
                           "(start recording from the level start for full coverage)",
                           perFrames.front().tick, perFrames.front().tick / 240.0,
                           perFrames.front().p1x, perFrames.front().p1y);
    } else if (!anchors.empty() && anchors.front().tick > 120) {
        skkbot2::log::warn("NOTE: first recorded anchor is tick {} - capture started MID-RUN; "
                           "playback will make a best-effort position alignment",
                           anchors.front().tick);
    }
    return true;
}

SkMacro* SkMacro::loadFromPath(std::filesystem::path const& path) {
    auto bytes = ReplayStorage::readReplayBytes(path);
    if (!bytes) {
        return nullptr;
    }

    auto* macro = deserialize(*bytes);
    if (!macro) {
        skkbot2::log::error("Could not load replay '{}': the file is corrupt or unsupported", skk::pathToUtf8(path));
        return nullptr;
    }

    auto stem = skk::pathToUtf8(path.stem());
    macro->name = stem;
    macro->persistedName = stem;
    macro->fileFormat = SkFileFormat::SKK3;
    skkbot2::log::info("LOADED '{}' | main: inputs={} anchors={} frames={} | chainLinks={} varianceSlots={} hintTps={}",
        stem, macro->inputs.size(), macro->anchors.size(), macro->perFrames.size(),
        macro->persistenceAttempts.size(), macro->varianceOverrides.size(), macro->framerate);
    if (macro->startedMidRun()) {
        skkbot2::log::info("LOADED '{}' origin: mid-run recording - timeline starts at tick {} p1=({:.0f},{:.0f}); "
                           "playback aligns by player position",
            stem, macro->recordFirstTick(), macro->recordStartX(), macro->recordStartY());
    }
    return macro;
}

SkMacro* SkMacro::loadFromDisk(std::string const& filename) {
    auto directory = skkbot2::paths::replays();
    if (!std::filesystem::exists(directory) && !std::filesystem::create_directory(directory)) {
        return nullptr;
    }

    std::vector<std::filesystem::path> candidates;
    std::filesystem::path requested(filename);
    auto requestedExtension = skk::pathToUtf8(requested.extension());
    std::transform(requestedExtension.begin(), requestedExtension.end(), requestedExtension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (requestedExtension == ".skk") {
        candidates.push_back(directory / requested);
    } else {
        candidates.push_back(directory / (filename + ".skk"));
        candidates.push_back(directory / filename);
    }

    std::filesystem::path path;
    for (auto const& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            path = candidate;
            break;
        }
    }
    if (path.empty()) {
        return nullptr;
    }

    return loadFromPath(path);
}

std::vector<MacroAction> SkMacro::toMacroActions() const {
    std::vector<MacroAction> actions;
    actions.reserve(inputs.size());
    for (auto const& input : inputs) {
        MacroAction action(
            input.tick,
            static_cast<int>(input.actionType),
            input.isPlayer2(),
            input.isPressed(),
            input.stepOffset
        );
        action.timeSeconds = input.timeSeconds;
        action.swiftPairAnchor = input.swiftPairAnchor;
        actions.push_back(action);
    }
    return actions;
}

std::vector<MacroAction> SkMacro::toPersistenceMacroActions() const {
    size_t totalInputs = inputs.size();
    for (auto const& attempt : persistenceAttempts) {
        totalInputs += attempt.inputs.size();
    }

    std::vector<MacroAction> actions;
    actions.reserve(totalInputs);

    int32_t baseTick = 0;
    double baseTimeSeconds = 0.0;
    double safeFramerate = std::isfinite(framerate) && framerate > 0.0 ? framerate : 240.0;
    for (auto const& attempt : persistenceAttempts) {
        for (auto const& input : attempt.inputs) {
            MacroAction action(
                baseTick + input.tick,
                static_cast<int>(input.actionType),
                input.isPlayer2(),
                input.isPressed(),
                input.stepOffset
            );
            if (input.hasAbsoluteTime()) {
                action.timeSeconds = baseTimeSeconds + input.timeSeconds;
            }
            action.swiftPairAnchor = input.swiftPairAnchor;
            actions.push_back(action);
        }
        baseTick += std::max<int32_t>(1, attempt.deathTick);
        baseTimeSeconds += attempt.hasAbsoluteDeathTime()
            ? std::max(0.0, attempt.deathTimeSeconds)
            : static_cast<double>(std::max<int32_t>(1, attempt.deathTick)) / safeFramerate;
    }

    for (auto const& input : inputs) {
        MacroAction action(
            baseTick + input.tick,
            static_cast<int>(input.actionType),
            input.isPlayer2(),
            input.isPressed(),
            input.stepOffset
        );
        if (input.hasAbsoluteTime()) {
            action.timeSeconds = baseTimeSeconds + input.timeSeconds;
        }
        action.swiftPairAnchor = input.swiftPairAnchor;
        actions.push_back(action);
    }

    return actions;
}
