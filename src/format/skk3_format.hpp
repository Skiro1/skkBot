#pragma once

#include "core/accuracy_mode.hpp"
#include "replay.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class SkMacro;

namespace skk::skk3 {

// v6: dense wire — CML V7-style encoding with RLE hold repeats. XOR-masked
// header strings, byte-LEB128 varints, zigzag integers, delta timestamps
// (TimeStream with tick anchor + picosecond residue) and bit-delta coordinate
// streams. Input records lead with the button byte; button==0 marks an RLE
// repeat of the previous record's cycle. v5 payload layout is no longer
// readable.
// 7: per-frame section layout v4 - fixed-point first-difference columns with
//     record-level run-length blocks (positions x1/256px, velocities /16384,
//     rotation /1024; integer-exact, deterministic round-trip, 5-15x smaller
//     before the outer zstd pass). Old v6 files (layout 3) still load.
inline constexpr uint16_t kWireVersion = 7;
inline constexpr uint64_t kNativeSourceFormatId = 0x00000000FFFF0003ull;

enum Flags : uint32_t {
    FlagLosslessVerified = 1u << 0,
    FlagFromStartPos = 1u << 1,
    FlagPlatformer = 1u << 2,
    FlagTwoPlayer = 1u << 3,
    FlagRngLocked = 1u << 4,
    FlagHasAnchors = 1u << 5,
    FlagHasCheckpoints = 1u << 6,
    FlagHasPersistence = 1u << 7,
    FlagHasTpsEvents = 1u << 8,
    FlagMacroConverted = 1u << 9,
    FlagCompressed = 1u << 10,
    FlagHasPerFrame = 1u << 11,
    FlagHasVariance = 1u << 12,
    // v4: tick labels are GD physics-step pairs (m_currentStep cadence), not
    // floor(levelTime*TPS). Files without this bit keep the legacy time grid.
    FlagStepClock = 1u << 13,
    FlagHasPracticeFix = 1u << 14,
    FlagHasRngBoundaries = 1u << 15,
};

enum class SectionKind : uint8_t {
    Inputs = 1,
    TpsEvents = 2,
    Anchors = 3,
    Checkpoints = 4,
    Persistence = 5,
    PerFrame = 6,
    Variance = 7,
    PracticeFix = 8,
    RngBoundaries = 9,
};

struct Input {
    double timeSeconds = 0.0;
    uint8_t button = 1;
    bool player2 = false;
    bool pressed = false;
    bool swiftPairAnchor = false;
};

struct TpsEvent {
    double timeSeconds = 0.0;
    double tps = 240.0;
};

struct Checkpoint {
    double timeSeconds = 0.0;
    uint64_t rngState = 0;
    double priorTimeSeconds = 0.0;
};

struct PracticeFixCheckpoint {
    double timeSeconds = 0.0;
    double priorTimeSeconds = 0.0;
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
    int32_t p1_stateOnGround = 0, p1_stateForce = 0, p1_reverseRelated = 0;
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
    int32_t p2_stateOnGround = 0, p2_stateForce = 0, p2_reverseRelated = 0;
    uint64_t p2_flags1 = 0, p2_flags2 = 0;
    bool p2_holdingRight = false, p2_holdingLeft = false;
};

struct Anchor {
    double timeSeconds = 0.0;
    PlaybackAnchor state;
};

// v2 — per-frame reliability layer (position + velocity every frame).
// Records x/y + vertical velocity (and pre-slope velocity) each frame as a
// safety net on top of the sparse semantic anchors. Restores position AND
// velocity together on playback — the velocity fix is always applied, no
// toggle (avoids the GDH pitfall of forcing position without velocity -> jumps).
struct PerFrame {
    double timeSeconds = 0.0;
    float p1x = 0.f;
    float p1y = 0.f;
    double p1vy = 0.0;
    double p1vyb = 0.0;
    float p1rot = 0.f;
    uint8_t p1vis = 0;
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

// GD variance-slot snapshot entry (GJBaseGameLayer::m_varianceValues).
struct VarianceEntry {
    uint16_t index = 0;
    float value = 0.f;
};

struct Attempt {
    double deathTimeSeconds = 0.0;
    bool deathPlayer2 = false;
    std::vector<Input> inputs;
    std::vector<Anchor> anchors;
    std::vector<PerFrame> perFrames;
};

struct Macro {
    uint64_t sourceFormatId = kNativeSourceFormatId;
    uint64_t gameVersion = 0;
    int32_t levelId = 0;
    std::string name;
    std::string levelName;
    std::string author;
    double framerateHint = 240.0;
    // Display rate the macro was recorded under; 0 = follow framerateHint. The
    // only rate that reaches the wire file, and only when it differs.
    double displayFps = 0.0;
    bool lockDelta = true;
    bool realTime = false;
    double duration = 0.0;
    float startPosX = 0.0f;
    float startPosY = 0.0f;
    int64_t recordTimestamp = 0;
    uint32_t rngSeed = 0;
    bool losslessVerified = false;
    bool recordedFromStartPos = false;
    bool platformerMode = false;
    bool twoPlayerMode = false;
    bool rngLocked = false;
    bool macroConverted = false;
    bool stepClock = false;
    AccuracyMode accuracyMode = AccuracyMode::Vanilla;
    std::vector<Input> inputs;
    std::vector<TpsEvent> tpsEvents;
    std::vector<Anchor> anchors;
    std::vector<PerFrame> perFrames;
    std::vector<Checkpoint> checkpoints;
    std::vector<PracticeFixCheckpoint> practiceFixCheckpoints;
    std::vector<Attempt> persistenceAttempts;
    std::vector<VarianceEntry> variance;
    uint64_t setupRngSeed = 0;
    std::vector<std::pair<int32_t, int32_t>> persistentItems;
    // Practice-restart boundary reseeds (tick, progress, live triple) - same as
    // the legacy SkRngBoundary, mirrored for every splice point.
    struct RngBoundary {
        int32_t tick = 0;
        int32_t progress = 0;
        uint64_t primary = 0;   // live GD+0x6C2E90 at the boundary frame
        uint64_t shake = 0;     // live GD+0x6C2EE0 at the boundary frame
        uint64_t teleport = 0;  // live GD+0x6C2EF8 at the boundary frame
    };
    std::vector<RngBoundary> rngBoundaries;
};

struct WriteOptions {
    bool compressPayload = true;
};

std::vector<uint8_t> serialize(Macro const& macro, WriteOptions options = {});
std::optional<Macro> deserialize(std::vector<uint8_t> const& data, std::string* error = nullptr);
double maxSourceTps(Macro const& macro);
Macro fromSkMacro(SkMacro const& macro);
SkMacro toSkMacro(Macro const& macro);

// Runtime self-check for the per-frame v4 codec: round-trips a synthetic
// macro (holds/gravity/autoclicker/p2/taps) through the section writer+reader
// and reports byte-stability + the v3-vs-v4 size ratio.
struct PerFrameSelfTestResult {
    bool ok = false;
    uint64_t frames = 0;
    size_t v3Bytes = 0;
    size_t v4Bytes = 0;
    // BUG-06/SELFTEST diagnostics: which stage failed and the first
    // diverging detail, so a failing round-trip pinpoints the root cause
    // instead of a bare 'ok=false'.
    enum class Stage {
        None,
        ReadFailed,
        CountMismatch,
        FieldMismatch,
        RewriteMismatch,
    };
    Stage stage = Stage::None;
    int64_t failIdx = -1;
    uint32_t expected = 0;
    uint32_t decoded = 0;
    std::string failDetail; // which field diverged, or rewrite byte count
};
PerFrameSelfTestResult selfTestPerFrameCodec();

// The header self-test: proves that a file written WITHOUT the display rate -
// which is byte for byte what the previous build wrote, since the field is
// omitted rather than zero-filled - still decodes, and that a file WITH one
// grows its header by exactly that field and nothing else.
struct HeaderSelfTestResult {
    bool ok = false;
    size_t oldShapeHeaderBytes = 0;
    std::string detail;
};
HeaderSelfTestResult selfTestHeader();

}
