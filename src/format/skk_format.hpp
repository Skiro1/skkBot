#ifndef _skk_format_hpp
#define _skk_format_hpp

#include "replay.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace geode::prelude;

enum class SkFileFormat : uint8_t {
    SKK3 = 0,
};

enum SkFlags : uint32_t {
    SK_FLAG_ACCURACY_CBS = 1 << 0,
    SK_FLAG_FROM_START_POS = 1 << 1,
    SK_FLAG_PLATFORMER = 1 << 2,
    SK_FLAG_TWO_PLAYER = 1 << 3,
    SK_FLAG_RNG_LOCKED = 1 << 4,
    SK_FLAG_ACCURACY_CBF = 1 << 5,
    SK_FLAG_ACCURACY_SUBSTEP = 1 << 6,
    SK_FLAG_EXACT_CBS_TIMING = 1 << 7,
    SK_FLAG_PERSISTENCE = 1 << 8,
};

// v2 per-frame visual orientation flag bits (shared between recording and
// playback sides — used in SkPerFrame.p1vis/p2vis).
inline constexpr uint8_t kVisualGravityNegative = 1 << 0;
inline constexpr uint8_t kVisualUpsideDown       = 1 << 1;
inline constexpr uint8_t kVisualModeMask         = 0x1C;  // bits 2-4
inline constexpr uint8_t kVisualModeShift        = 2;
inline constexpr uint8_t kVisualSideways         = 1 << 5;
inline constexpr uint8_t kVisualControlsDisabled = 1 << 6;
inline constexpr uint8_t kVisualDashing          = 1 << 7;

inline uint8_t vehicleModeBits(PlayerObject* player) {
    if (!player) return 0;
    if (player->m_isShip)   return 1;
    if (player->m_isBall)   return 2;
    if (player->m_isBird)   return 3;
    if (player->m_isDart)   return 4;
    if (player->m_isRobot)  return 5;
    if (player->m_isSpider) return 6;
    if (player->m_isSwing)  return 7;
    return 0;
}

inline uint8_t visualFlagsOf(PlayerObject* player) {
    if (!player) return 0;
    uint8_t flags = 0;
    if (player->m_gravity < 0.0) flags |= kVisualGravityNegative;
    if (player->m_isUpsideDown) flags |= kVisualUpsideDown;
    flags |= (vehicleModeBits(player) << kVisualModeShift) & kVisualModeMask;
    if (player->m_isSideways) flags |= kVisualSideways;
    if (player->m_controlsDisabled) flags |= kVisualControlsDisabled;
    if (player->m_isDashing) flags |= kVisualDashing;
    return flags;
}

struct SkInput {
    int32_t tick = 0;
    uint8_t actionType = 0;
    uint8_t flags = 0;
    float stepOffset = 0.0f;
    double cbsTimeOffset = -1.0;
    double timeSeconds = -1.0;
    bool swiftPairAnchor = false;

    bool isPlayer2() const { return (flags & 0x01) != 0; }
    bool isPressed() const { return (flags & 0x02) != 0; }
    bool hasAbsoluteTime() const { return std::isfinite(timeSeconds) && timeSeconds >= 0.0; }

    void setPlayer2(bool value) { flags = (flags & ~0x01) | (value ? 0x01 : 0x00); }
    void setPressed(bool value) { flags = (flags & ~0x02) | (value ? 0x02 : 0x00); }
};

struct SkCheckpoint {
    int32_t tick = 0;
    uint64_t rngState = 0;
    int32_t priorTick = 0;
    double timeSeconds = -1.0;
    double priorTimeSeconds = -1.0;

    bool hasAbsoluteTime() const { return std::isfinite(timeSeconds) && timeSeconds >= 0.0; }
    bool hasAbsolutePriorTime() const { return std::isfinite(priorTimeSeconds) && priorTimeSeconds >= 0.0; }
};

struct SkPracticeFixCheckpoint {
    double timeSeconds = 0.0;
    double priorTimeSeconds = 0.0;
    int32_t tick = 0;
    int32_t priorTick = 0;
    uint8_t player1LatchMask = 0;
    uint8_t player2LatchMask = 0;
    uint64_t seedState = 0;
    uint64_t shakeRandomState = 0;
    uint64_t teleportRandomState = 0;
    std::vector<int32_t> brokenObjectUIDs;
    std::vector<std::pair<int32_t, int32_t>> persistentItems;
    std::vector<std::pair<uint16_t, float>> variance;
    bool clickReleased[6] = { true, true, true, true, true, true };
    // Full P1 state
    float p1_x = 0.f, p1_y = 0.f, p1_rotation = 0.f;
    double p1_yVelocity = 0.0, p1_fallSpeed = 0.0;
    double p1_platformerXVelocity = 0.0, p1_groundYVelocity = 0.0;
    double p1_yVelocityBeforeSlope = 0.0, p1_yVelocityRelated = 0.0;
    double p1_scaleXRelated = 0.0, p1_scaleXRelated2 = 0.0, p1_scaleXRelated3 = 0.0;
    double p1_scaleXRelated4 = 0.0, p1_scaleXRelated5 = 0.0;
    double p1_scaleXRelatedTime = 0.0;
    float p1_playerSpeed = 0.f, p1_vehicleSize = 1.f;
    float p1_platformerVelocityRelated = 0.f;
    float p1_xVelocityRelated = 0.f, p1_xVelocityRelated2 = 0.f;
    float p1_yVelocityRelated3 = 0.f, p1_slopeVelocity = 0.f;
    double p1_gravity = 1.0, p1_speedMultiplier = 1.0;
    double p1_snapDistance = 0.0, p1_accelerationOrSpeed = 0.0;
    double p1_yStart = 0.0;
    float p1_gravityMod = 1.f, p1_slopeAngle = 0.f, p1_slopeAngleRadians = 0.f;
    double p1_slopeRotation = 0.0, p1_currentSlopeYVelocity = 0.0;
    double p1_slopeStartTime = 0.0;
    double p1_dashX = 0.0, p1_dashY = 0.0, p1_dashAngle = 0.0, p1_dashStartTime = 0.0;
    double p1_totalTime = 0.0, p1_lastCheckpointTime = 0.0;
    double p1_lastJumpTime = 0.0, p1_lastFlipTime = 0.0, p1_lastSpiderFlipTime = 0.0;
    double p1_gameModeChangedTime = 0.0, p1_lastLandTime = 0.0;
    int32_t p1_lastCollisionBottom = 0, p1_lastCollisionTop = 0;
    int32_t p1_lastCollisionLeft = 0, p1_lastCollisionRight = 0;
    int32_t p1_stateOnGround = 0, p1_stateForce = 0, p1_stateFlipGravity = 0, p1_reverseRelated = 0;
    uint64_t p1_flags1 = 0, p1_flags2 = 0;
    bool p1_holdingRight = false, p1_holdingLeft = false;
    // Full P2 state
    float p2_x = 0.f, p2_y = 0.f, p2_rotation = 0.f;
    double p2_yVelocity = 0.0, p2_fallSpeed = 0.0;
    double p2_platformerXVelocity = 0.0, p2_groundYVelocity = 0.0;
    double p2_yVelocityBeforeSlope = 0.0, p2_yVelocityRelated = 0.0;
    double p2_scaleXRelated = 0.0, p2_scaleXRelated2 = 0.0, p2_scaleXRelated3 = 0.0;
    double p2_scaleXRelated4 = 0.0, p2_scaleXRelated5 = 0.0;
    double p2_scaleXRelatedTime = 0.0;
    float p2_playerSpeed = 0.f, p2_vehicleSize = 1.f;
    float p2_platformerVelocityRelated = 0.f;
    float p2_xVelocityRelated = 0.f, p2_xVelocityRelated2 = 0.f;
    float p2_yVelocityRelated3 = 0.f, p2_slopeVelocity = 0.f;
    double p2_gravity = 1.0, p2_speedMultiplier = 1.0;
    double p2_snapDistance = 0.0, p2_accelerationOrSpeed = 0.0;
    double p2_yStart = 0.0;
    float p2_gravityMod = 1.f, p2_slopeAngle = 0.f, p2_slopeAngleRadians = 0.f;
    double p2_slopeRotation = 0.0, p2_currentSlopeYVelocity = 0.0;
    double p2_slopeStartTime = 0.0;
    double p2_dashX = 0.0, p2_dashY = 0.0, p2_dashAngle = 0.0, p2_dashStartTime = 0.0;
    double p2_totalTime = 0.0, p2_lastCheckpointTime = 0.0;
    double p2_lastJumpTime = 0.0, p2_lastFlipTime = 0.0, p2_lastSpiderFlipTime = 0.0;
    double p2_gameModeChangedTime = 0.0, p2_lastLandTime = 0.0;
    int32_t p2_lastCollisionBottom = 0, p2_lastCollisionTop = 0;
    int32_t p2_lastCollisionLeft = 0, p2_lastCollisionRight = 0;
    int32_t p2_stateOnGround = 0, p2_stateForce = 0, p2_stateFlipGravity = 0, p2_reverseRelated = 0;
    uint64_t p2_flags1 = 0, p2_flags2 = 0;
    bool p2_holdingRight = false, p2_holdingLeft = false;

    bool hasAbsoluteTime() const { return std::isfinite(timeSeconds) && timeSeconds >= 0.0; }
};

struct SkTpsEvent {
    double timeSeconds = 0.0;
    double tps = 240.0;
};

struct SkVarianceEntry {
    uint16_t index = 0;
    float value = 0.f;
};

struct SkPerFrame {
    int32_t tick = 0;
    double timeSeconds = -1.0;
    float p1x = 0.f;
    float p1y = 0.f;
    double p1vy = 0.0;
    double p1vyb = 0.0;
    // v2 visual pin: rotation and orientation flags per player (bit0 gravity
    // sign, bit1 level upside-down). Rotational state is integral (ball/swing
    // spin accumulates), so GD cannot self-heal it from pinned position;
    // restoring it here is safe - rotation is an OUTPUT of GD physics, no
    // sub-step state machine reads it back (unlike latch masks / dashing,
    // which the anchor full-restore used to stomp).
    //
    // v4: bits 2-4 vehicle mode (0=cube,1=ship,2=ball,3=ufo,4=wave,5=robot,6=spider,7=swing).
    //     bit 5 isSideways, bit 6 controlsDisabled, bit 7 isDashing.
    //     Old V3 recordings have 0 in bits 2-7 (cube mode, safe default).
    float p1rot = 0.f;
    uint8_t p1vis = 0;
    // v3: m_dashRing object ID (0 = none) - reattaches the dash-ring hold on
    // playback (GD picks the ring by ID in the identical world) so the
    // press/hold animation and m_dashRing state survive.
    int32_t p1RingId = 0;
    bool hasPlayer2 = false;
    float p2x = 0.f;
    float p2y = 0.f;
    double p2vy = 0.0;
    double p2vyb = 0.0;
    float p2rot = 0.f;
    uint8_t p2vis = 0;
    int32_t p2RingId = 0;
};

struct SkAttemptSegment {
    int32_t deathTick = 0;
    double deathTimeSeconds = -1.0;
    bool deathPlayer2 = false;
    std::vector<SkInput> inputs;
    std::vector<PlaybackAnchor> anchors;
    std::vector<SkPerFrame> perFrames;

    bool hasAbsoluteDeathTime() const {
        return std::isfinite(deathTimeSeconds) && deathTimeSeconds >= 0.0;
    }

    bool hasData() const {
        return deathTick > 0 || hasAbsoluteDeathTime() || !inputs.empty() || !anchors.empty() ||
            !perFrames.empty();
    }
};

// Practice-restart RNG boundary. When a recording is spliced (the player dies
// in practice mode and respawns from a checkpoint), vanilla resetLevel re-arms
// the locked LCG (armSeed = computeLockedSeed(seed, progress) family) and the
// checkpoint restore then overrides the primary stream to the checkpoint's
// captured fastRandState. Each spliced segment therefore runs under that
// combined state. Capture latches exactly these two values and playback
// reproduces them at the same tick, otherwise the stitched macro drifts from
// the recorded random sections (segmented == divergent).
//
// The boundary captures the LIVE triple RNG state on the first captured frame
// after a practice respawn (this is what refreshRngState(arm) + the checkpoint
// restore actually left the engine in), NOT a formula-derived value. Playback
// restores those raw globals verbatim at the matching tick - the only way to be
// byte-identical to the capture as the arm family is not a function of progress.
struct SkRngBoundary {
    int32_t tick = 0;        // macro tick of the first captured frame after the respawn
    int32_t progress = 0;    // the checkpoint's m_currentProgress (informational)
    uint64_t primary = 0;    // live GD+0x6C2E90 at the boundary frame
    uint64_t shake = 0;      // live GD+0x6C2EE0 at the boundary frame
    uint64_t teleport = 0;   // live GD+0x6C2EF8 at the boundary frame
};

class SkMacro {
public:
    std::string author;
    std::string name;
    std::string persistedName;
    std::string levelName;
    int32_t levelId = 0;
    double framerate = 240.0;
    // Display rate as a WIRE value, not a number: +0.0 = the macro carries no
    // display rate (FPS was off while recording), -0.0 = "FPS on, follow the
    // tick rate", n > 0 = pinned. Read it with
    // skkbot2::fps::MacroDisplayRate::fromWire - comparing against zero cannot
    // see the marker, because -0.0 == +0.0.
    double displayFps = 0.0;
    double duration = 0.0;
    uint32_t gameVersion = 0;
    float startPosX = 0.f;
    float startPosY = 0.f;
    bool recordedFromStartPos = false;
    // One-shot latch: beginCapture runs BEFORE PlayLayer::init creates the
    // start-pos object, so SP state must be (re)stamped on first capture tick.
    bool startPosStamped = false;
    AccuracyMode accuracyMode = AccuracyMode::Vanilla;
    bool platformerMode = false;
    bool twoPlayerMode = false;
    bool rngLocked = false;
    bool exactCbsTiming = false;
    SkFileFormat fileFormat = SkFileFormat::SKK3;
    uint64_t sourceFormatId = 0x00000000FFFF0003ull;
    uint32_t rngSeed = 0;
    int64_t recordTimestamp = 0;
    bool losslessVerified = false;
    bool macroConverted = false;
    bool bestEffort = false;
    // v4: ticks labeled by GD physics-step pairs instead of levelTime.
    // New captures set it; legacy files default false (time labels).
    bool usesStepClock = false;
    std::vector<SkInput> inputs;
    std::vector<SkTpsEvent> tpsEvents;
    std::vector<PlaybackAnchor> anchors;
    std::vector<SkPerFrame> perFrames;
    std::vector<SkCheckpoint> checkpoints;
    std::vector<SkPracticeFixCheckpoint> practiceFixCheckpoints;
    std::vector<SkAttemptSegment> persistenceAttempts;
    // GD variance slots (GJBaseGameLayer::m_varianceValues, 2000 floats):
    // snapshot of non-zero slots captured during recording, force-prefilled
    // at each playback attempt so lazy random-trigger variance matches the
    // recorded session regardless of RNG-stream luck. Session-wide (chain
    // attempts share it).
    std::vector<SkVarianceEntry> varianceOverrides;
    // Global fast_srand state captured at the LAST recorded attempt's setup
    // (before vanilla resetLevel derived the object rolls). Pinning playback's
    // RNG to it makes GD's own derivation chain reproduce the recorded world
    // (block variance offsets, trigger picks, shake) with no midhooks.
    uint64_t setupRngSeed = 0;
    // Persistent trigger counters (GJEffectManager::m_persistentItemCountMap)
    // as they stood when the survivor run was captured. These accumulate
    // ACROSS attempts within one session, so a macro whose run was attempt N
    // sees different item counts than the fresh replay session; recording and
    // re-applying them removes that whole class of single-point world deaths.
    std::vector<std::pair<int32_t, int32_t>> persistentItems;
    // Practice-restart boundary reseeds (see SkRngBoundary). Playback re-applies
    // each entry's LCG reseed when the run crosses its tick.
    std::vector<SkRngBoundary> rngBoundaries;

    // Non-standard-start support. The recording knows WHERE its timeline began:
    // the tick (and p1 world position) of the very first captured frame. A macro
    // whose origin is beyond this threshold was recorded MID-RUN (the bot was
    // armed while the player was already in the level), so its data does not
    // cover the prefix a tick-0 timeline assumes. Playback aligns such macros by
    // POSITION instead of by tick. Everything is derived from the saved streams
    // (perFrames are the densest ground truth), so no wire-format change.
    static constexpr int kMidRunStartTickThreshold = 120;

    int64_t recordFirstTick() const {
        int64_t first = -1;
        if (!perFrames.empty()) first = perFrames.front().tick;
        if (!anchors.empty()) {
            first = first < 0 ? anchors.front().tick
                              : std::min<int64_t>(first, anchors.front().tick);
        }
        if (!inputs.empty()) {
            first = first < 0 ? inputs.front().tick
                              : std::min<int64_t>(first, inputs.front().tick);
        }
        return first < 0 ? 0 : first;
    }

    bool startedMidRun() const { return recordFirstTick() > kMidRunStartTickThreshold; }

    float recordStartX() const {
        if (!perFrames.empty()) return perFrames.front().p1x;
        if (!anchors.empty()) return anchors.front().player1.motion.position.x;
        return startPosX;
    }

    float recordStartY() const {
        if (!perFrames.empty()) return perFrames.front().p1y;
        if (!anchors.empty()) return anchors.front().player1.motion.position.y;
        return startPosY;
    }

    bool loadedFromSKK3() const { return fileFormat == SkFileFormat::SKK3; }
    double maxSourceTps() const;
    void materializeSKK3RuntimeTicks(double runtimeTps);
    void recordAction(int tick, int button, bool player2, bool pressed, float offset, double cbsTimeOffset = -1.0);
    void recordAction(std::vector<SkInput>& target, int tick, int button, bool player2, bool pressed, float offset, double cbsTimeOffset = -1.0);
    void recordAnchor(int tick, PlayerObject* p1, PlayerObject* p2, bool isPlatformer, bool isDual = true);
    void recordAnchor(std::vector<PlaybackAnchor>& target, int tick, PlayerObject* p1, PlayerObject* p2, bool isPlatformer, bool isDual = true);
    void recordPerFrame(std::vector<SkPerFrame>& target, int tick, PlayerObject* p1, PlayerObject* p2);
    void recordPerFrame(int tick, PlayerObject* p1, PlayerObject* p2);
    void truncateAfter(int tick);
    std::vector<uint8_t> serialize() const;
    std::vector<uint8_t> serializeSKK3() const;
    static SkMacro* deserialize(std::vector<uint8_t> const& data);
    bool persist();
    bool persistToDirectory(std::filesystem::path const& directory);
    bool saveToPath(std::filesystem::path const& path);
    static SkMacro* loadFromDisk(std::string const& filename);
    static SkMacro* loadFromPath(std::filesystem::path const& path);
    std::vector<MacroAction> toMacroActions() const;
    std::vector<MacroAction> toPersistenceMacroActions() const;
};

inline void normalizeSkPersistenceTiming(SkMacro& macro) {
    macro.accuracyMode = writableAccuracyMode(macro.accuracyMode);

    auto stripTimedOffsets = [](std::vector<SkInput>& inputList) {
        for (auto& input : inputList) {
            input.stepOffset = 0.0f;
            input.cbsTimeOffset = -1.0;
            input.timeSeconds = -1.0;
            input.swiftPairAnchor = false;
        }
    };

    if (!usesTimedAccuracy(macro.accuracyMode)) {
        stripTimedOffsets(macro.inputs);
        for (auto& attempt : macro.persistenceAttempts) {
            stripTimedOffsets(attempt.inputs);
        }
        macro.exactCbsTiming = false;
        return;
    }

    macro.exactCbsTiming = false;
    for (auto const& input : macro.inputs) {
        if (std::isfinite(input.cbsTimeOffset) && input.cbsTimeOffset >= 0.0) {
            macro.exactCbsTiming = true;
            return;
        }
    }
    for (auto const& attempt : macro.persistenceAttempts) {
        for (auto const& input : attempt.inputs) {
            if (std::isfinite(input.cbsTimeOffset) && input.cbsTimeOffset >= 0.0) {
                macro.exactCbsTiming = true;
                return;
            }
        }
    }
}

// rngBoundaries are pushed in RECORD order (each practice respawn appends), so
// the vector is not guaranteed ascending by tick - a death that restores to an
// earlier checkpoint can append a smaller tick AFTER a larger one. Playback
// consumes them via an ascending-tick scan (boundary.tick <= tick), so an
// unsorted vector applies a later-tick boundary FIRST and re-seeds the live
// stream backward -> RNG desync from that checkpoint onward. Normalize once on
// load: stable-sort by tick and keep only the LAST entry per tick (the final
// attempt's captured triple is the one the surviving segment continues under).
inline void normalizeSkRngBoundaries(SkMacro& macro) {
    auto& boundaries = macro.rngBoundaries;
    if (boundaries.size() < 2) {
        return;
    }
    std::stable_sort(boundaries.begin(), boundaries.end(),
        [](SkRngBoundary const& a, SkRngBoundary const& b) {
            return a.tick < b.tick;
        });
    std::vector<SkRngBoundary> deduped;
    deduped.reserve(boundaries.size());
    for (size_t i = 0; i < boundaries.size(); ++i) {
        size_t j = i + 1;
        while (j < boundaries.size() && boundaries[j].tick == boundaries[i].tick) {
            ++j;
        }
        deduped.push_back(boundaries[j - 1]);
        i = j - 1;
    }
    if (deduped.size() != boundaries.size()) {
        boundaries = std::move(deduped);
    }
}

#endif
