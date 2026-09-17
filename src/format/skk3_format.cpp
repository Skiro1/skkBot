#include "skk3_format.hpp"
#include "skk_format.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

#include "codec.hpp"

namespace skk::skk3 {
namespace {

static constexpr size_t kHeaderSize = 16;
static constexpr size_t kMaxStringSize = 4096;
static constexpr uint64_t kMaxPayloadSize = 512ull * 1024ull * 1024ull;
static constexpr uint32_t kKnownFlags =
    FlagLosslessVerified | FlagFromStartPos | FlagPlatformer | FlagTwoPlayer |
    FlagRngLocked | FlagHasAnchors | FlagHasCheckpoints | FlagHasPersistence |
    FlagHasTpsEvents | FlagMacroConverted | FlagCompressed | FlagHasPerFrame | FlagHasVariance |
    FlagStepClock | FlagHasPracticeFix | FlagHasRngBoundaries;

struct ReadContext;

template <typename T>
static T readLE(ReadContext& ctx);

static bool checkCountFits(ReadContext& ctx, uint64_t count, size_t minRecordSize, std::string* error, char const* label);
static void writeInputsSection(std::vector<uint8_t>& out, std::vector<Input> const& inputs, double baseTps);
static std::vector<Input> readInputsSection(ReadContext& ctx, double framerateHint, std::string* error);
static std::vector<TpsEvent> readTpsEventsSection(ReadContext& ctx, double baseTps, std::string* error);
static std::vector<Checkpoint> readCheckpointsSection(ReadContext& ctx, double baseTps, std::string* error);
static std::vector<PracticeFixCheckpoint> readPracticeFixSection(ReadContext& ctx, std::string* error);
static std::vector<Anchor> readAnchorsSection(ReadContext& ctx, double baseTps, std::string* error);
static std::vector<Attempt> readPersistenceSection(ReadContext& ctx, double framerateHint, std::string* error);
static std::vector<Macro::RngBoundary> readRngBoundariesSection(ReadContext& ctx, std::string* error);
static PlayerStateBundle readPlayerState(ReadContext& ctx);

struct ReadContext {
    std::vector<uint8_t> const& data;
    size_t position = 0;
    size_t end = 0;
    bool failed = false;

    explicit ReadContext(
        std::vector<uint8_t> const& bytes,
        size_t start = 0,
        size_t limit = std::numeric_limits<size_t>::max()
    )
        : data(bytes), position(start), end(std::min(limit, bytes.size())) {
        if (start > end) {
            failed = true;
        }
    }

    size_t remaining() const {
        return !failed && position <= end ? end - position : 0;
    }
};

static bool validTime(double value) {
    return std::isfinite(value) && value >= 0.0;
}

static bool validTps(double value) {
    return std::isfinite(value) && value > 0.0 && value <= 1000000.0;
}

// ─── Wire v5 density helpers (CML V7-style) ──────────────────────────────
// Byte LEB128 varints, zigzag signed integers, XOR-masked strings and a
// tick-anchored microsecond/picosecond delta timestamp stream. All are
// lossless for the values the format actually carries.

static void writeVarint(std::vector<uint8_t>& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

static uint64_t readVarint(ReadContext& ctx) {
    uint64_t value = 0;
    for (int shift = 0; shift < 64 && !ctx.failed; shift += 7) {
        if (ctx.remaining() < 1) {
            ctx.failed = true;
            break;
        }
        uint8_t byte = ctx.data[ctx.position++];
        if (shift == 63 && byte > 1) {
            ctx.failed = true;
            break;
        }
        value |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            return value;
        }
    }
    return value;
}

static void writeZigzag(std::vector<uint8_t>& out, int64_t value) {
    uint64_t zig = (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
    writeVarint(out, zig);
}

static int64_t readZigzag(ReadContext& ctx) {
    uint64_t value = readVarint(ctx);
    return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1u);
}

// CML V7 string mask: byte[i] ^ (61*i + 17*(i>>1) − 89) mod 256.
static uint8_t xorMask(size_t i) {
    int64_t raw = 61ll * static_cast<int64_t>(i) + 17ll * static_cast<int64_t>(i >> 1) - 89;
    return static_cast<uint8_t>(raw & 0xFF);
}

static void writeXorString(std::vector<uint8_t>& out, std::string const& value) {
    writeVarint(out, static_cast<uint64_t>(value.size()));
    for (size_t i = 0; i < value.size(); ++i) {
        out.push_back(static_cast<uint8_t>(value[i]) ^ xorMask(i));
    }
}

static std::string readXorString(ReadContext& ctx) {
    uint64_t length = readVarint(ctx);
    if (ctx.failed || length > kMaxStringSize || ctx.remaining() < length) {
        ctx.failed = true;
        return {};
    }
    std::string value;
    value.resize(static_cast<size_t>(length));
    for (size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<char>(
            static_cast<uint8_t>(ctx.data[ctx.position + i]) ^ xorMask(i));
    }
    ctx.position += static_cast<size_t>(length);
    return value;
}

// Delta timestamp stream: per record writes a zigzag frame-delta anchored to
// the macro TPS grid plus a picosecond residue for sub-frame offsets. Grid
// timestamps round-trip bit-exactly; off-grid (CBS) values keep picosecond
// resolution, far below the float stepOffset quantisation of the recorder.
struct TimeStream {
    double tps = 240.0;
    double unit = 1.0 / 240.0;
    int64_t prevTick = 0;
    int64_t lastTickDelta = 0;
    int64_t lastResiduePs = 0;

    void reset(double baseTps) {
        tps = baseTps > 0.0 ? baseTps : 240.0;
        unit = 1.0 / tps;
        prevTick = 0;
        lastTickDelta = 0;
        lastResiduePs = 0;
    }

    void write(std::vector<uint8_t>& out, double time) {
        int64_t tick = static_cast<int64_t>(std::llround(time * tps));
        lastTickDelta = tick - prevTick;
        writeZigzag(out, lastTickDelta);
        double residue = time - static_cast<double>(tick) * unit;
        lastResiduePs = static_cast<int64_t>(std::llround(residue * 1e12));
        writeZigzag(out, lastResiduePs);
        prevTick = tick;
    }

    // Try to encode `time` as an RLE repeat of the previous record's cycle
    // (autoclicker: constant frame delta, constant residue). Advances prevTick
    // so a run of repeats chains correctly. Caller decides button/flags.
    bool tryRepeat(double time) {
        int64_t tick = static_cast<int64_t>(std::llround(time * tps));
        int64_t delta = tick - prevTick;
        double residue = time - static_cast<double>(tick) * unit;
        int64_t residuePs = static_cast<int64_t>(std::llround(residue * 1e12));
        if (delta != lastTickDelta || residuePs != lastResiduePs) {
            return false;
        }
        prevTick = tick;
        return true;
    }

    double read(ReadContext& ctx) const {
        int64_t frameDelta = readZigzag(ctx);
        int64_t residuePs = readZigzag(ctx);
        if (ctx.failed) {
            return 0.0;
        }
        auto* self = const_cast<TimeStream*>(this);
        self->lastTickDelta = frameDelta;
        self->lastResiduePs = residuePs;
        int64_t tick = prevTick + frameDelta;
        self->prevTick = tick;
        return static_cast<double>(tick) * unit + static_cast<double>(residuePs) / 1e12;
    }

    // RLE hold-repeat: reuse the previous record's frame/residue delta (the
    // autoclicker cycle). prevTick advances; deltas stay identical for the run.
    double readRepeat(ReadContext& ctx) const {
        int64_t tick = prevTick + lastTickDelta;
        const_cast<TimeStream*>(this)->prevTick = tick;
        return static_cast<double>(tick) * unit + static_cast<double>(lastResiduePs) / 1e12;
    }
};

// Bit-delta encoder for float/double columns: stores XOR of consecutive raw
// bit patterns as a zigzag varint. Fully lossless (XOR is its own inverse).
struct BitDeltaStream_u32 {
    uint32_t prev = 0;
    void write(std::vector<uint8_t>& out, float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        writeZigzag(out, static_cast<int64_t>(static_cast<int32_t>(bits ^ prev)));
        prev = bits;
    }
    float read(ReadContext& ctx) const {
        int64_t delta = readZigzag(ctx);
        uint32_t bits = static_cast<uint32_t>(static_cast<int32_t>(delta)) ^ prev;
        const_cast<BitDeltaStream_u32*>(this)->prev = bits;
        float value = 0.f;
        std::memcpy(&value, &bits, sizeof(bits));
        return value;
    }
};

struct BitDeltaStream_u64 {
    uint64_t prev = 0;
    void write(std::vector<uint8_t>& out, double value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        writeZigzag(out, static_cast<int64_t>(bits ^ prev));
        prev = bits;
    }
    double read(ReadContext& ctx) const {
        int64_t delta = readZigzag(ctx);
        uint64_t bits = static_cast<uint64_t>(delta) ^ prev;
        const_cast<BitDeltaStream_u64*>(this)->prev = bits;
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(bits));
        return value;
    }
};

template <typename T>
static void writeLE(std::vector<uint8_t>& buffer, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    size_t position = buffer.size();
    buffer.resize(position + sizeof(T));
    std::memcpy(buffer.data() + position, &value, sizeof(T));
}

static void patchU32(std::vector<uint8_t>& buffer, size_t position, uint32_t value) {
    std::memcpy(buffer.data() + position, &value, sizeof(value));
}

static uint8_t packHoldMask(std::array<bool, 4> const& holds) {
    uint8_t mask = 0;
    for (size_t index = 0; index < holds.size(); ++index) {
        if (holds[index]) {
            mask |= static_cast<uint8_t>(1u << index);
        }
    }
    return mask;
}

static std::array<bool, 4> unpackHoldMask(uint8_t mask) {
    std::array<bool, 4> holds = { false, false, false, false };
    for (size_t index = 0; index < holds.size(); ++index) {
        holds[index] = (mask & static_cast<uint8_t>(1u << index)) != 0;
    }
    return holds;
}

static uint32_t packExtendedPlayerFlags(PlayerStateBundle const& state) {
    uint32_t flags = 0;
    if (state.flags.ship) flags |= 1 << 0;
    if (state.flags.bird) flags |= 1 << 1;
    if (state.flags.ball) flags |= 1 << 2;
    if (state.flags.wave) flags |= 1 << 3;
    if (state.flags.robot) flags |= 1 << 4;
    if (state.flags.spider) flags |= 1 << 5;
    if (state.flags.swing) flags |= 1 << 6;
    if (state.flags.sideways) flags |= 1 << 7;
    if (state.flags.dashing) flags |= 1 << 8;
    if (state.flags.onSlope) flags |= 1 << 9;
    if (state.flags.wasOnSlope) flags |= 1 << 10;
    if (state.flags.onGround) flags |= 1 << 11;
    if (state.flags.goingLeft) flags |= 1 << 12;
    if (state.flags.platformerMovingRight) flags |= 1 << 13;
    if (state.flags.slidingRight) flags |= 1 << 14;
    if (state.flags.accelerating) flags |= 1 << 15;
    if (state.flags.affectedByForces) flags |= 1 << 16;
    if (state.flags.jumpBuffered) flags |= 1 << 17;
    return flags;
}

static void unpackExtendedPlayerFlags(PlayerStateBundle& state, uint32_t flags) {
    state.flags.ship = (flags & (1 << 0)) != 0;
    state.flags.bird = (flags & (1 << 1)) != 0;
    state.flags.ball = (flags & (1 << 2)) != 0;
    state.flags.wave = (flags & (1 << 3)) != 0;
    state.flags.robot = (flags & (1 << 4)) != 0;
    state.flags.spider = (flags & (1 << 5)) != 0;
    state.flags.swing = (flags & (1 << 6)) != 0;
    state.flags.sideways = (flags & (1 << 7)) != 0;
    state.flags.dashing = (flags & (1 << 8)) != 0;
    state.flags.onSlope = (flags & (1 << 9)) != 0;
    state.flags.wasOnSlope = (flags & (1 << 10)) != 0;
    state.flags.onGround = (flags & (1 << 11)) != 0;
    state.flags.goingLeft = (flags & (1 << 12)) != 0;
    state.flags.platformerMovingRight = (flags & (1 << 13)) != 0;
    state.flags.slidingRight = (flags & (1 << 14)) != 0;
    state.flags.accelerating = (flags & (1 << 15)) != 0;
    state.flags.affectedByForces = (flags & (1 << 16)) != 0;
    state.flags.jumpBuffered = (flags & (1 << 17)) != 0;
}

static void writePlayerState(std::vector<uint8_t>& payload, PlayerStateBundle const& state) {
    writeLE<float>(payload, state.motion.position.x);
    writeLE<float>(payload, state.motion.position.y);
    writeLE<double>(payload, state.motion.verticalVelocity);
    writeLE<double>(payload, state.motion.preSlopeVerticalVelocity);
    writeLE<double>(payload, state.motion.horizontalVelocity);
    writeLE<float>(payload, state.motion.rotation);
    writeLE<float>(payload, static_cast<float>(state.environment.gravity));

    uint8_t stateFlags = 0;
    if (state.flags.upsideDown) stateFlags |= 1 << 0;
    if (state.flags.holdingLeft) stateFlags |= 1 << 1;
    if (state.flags.holdingRight) stateFlags |= 1 << 2;
    if (state.flags.platformer) stateFlags |= 1 << 3;
    if (state.flags.dead) stateFlags |= 1 << 4;
    if (state.environment.dualContext) stateFlags |= 1 << 5;
    if (state.environment.twoPlayerContext) stateFlags |= 1 << 6;
    if (state.environment.extendedState) stateFlags |= 1 << 7;
    writeLE<uint8_t>(payload, stateFlags);
    writeLE<uint8_t>(payload, packHoldMask(state.flags.buttonHolds));
    writeLE<uint32_t>(payload, packExtendedPlayerFlags(state));
    writeLE<double>(payload, state.motion.dashX);
    writeLE<double>(payload, state.motion.dashY);
    writeLE<double>(payload, state.motion.dashAngle);
    writeLE<double>(payload, state.motion.dashStartTime);
    writeLE<double>(payload, state.motion.slopeStartTime);
    writeLE<float>(payload, state.motion.fallSpeed);
    writeLE<float>(payload, state.motion.slopeVelocity);
    writeLE<float>(payload, state.motion.shipRotation.x);
    writeLE<float>(payload, state.motion.shipRotation.y);
    writeLE<float>(payload, state.motion.lastPortalPosition.x);
    writeLE<float>(payload, state.motion.lastPortalPosition.y);
    writeLE<float>(payload, state.motion.stateForceVector.x);
    writeLE<float>(payload, state.motion.stateForceVector.y);
    writeLE<float>(payload, state.environment.gravityMod);
    writeLE<float>(payload, state.environment.playerSpeed);
    writeLE<float>(payload, state.environment.playerSpeedAC);
    writeLE<double>(payload, state.environment.speedMultiplier);
    writeLE<float>(payload, state.environment.vehicleSize);
    writeLE<int32_t>(payload, state.environment.reverseRelated);
    writeLE<int32_t>(payload, state.environment.stateDartSlide);
    writeLE<int32_t>(payload, state.environment.stateFlipGravity);
    writeLE<int32_t>(payload, state.environment.stateForce);
    // Wire v3+: flip-tween time bases appended (see PlayerEnvironmentState).
    writeLE<double>(payload, state.environment.lastFlipTime);
    writeLE<double>(payload, state.environment.lastSpiderFlipTime);
}

static PlayerStateBundle readPlayerState(ReadContext& ctx) {
    PlayerStateBundle state;
    state.motion.position.x = readLE<float>(ctx);
    state.motion.position.y = readLE<float>(ctx);
    state.motion.verticalVelocity = readLE<double>(ctx);
    state.motion.preSlopeVerticalVelocity = readLE<double>(ctx);
    state.motion.horizontalVelocity = readLE<double>(ctx);
    state.motion.rotation = readLE<float>(ctx);
    state.environment.gravity = static_cast<double>(readLE<float>(ctx));

    uint8_t stateFlags = readLE<uint8_t>(ctx);
    state.flags.upsideDown = (stateFlags & (1 << 0)) != 0;
    state.flags.holdingLeft = (stateFlags & (1 << 1)) != 0;
    state.flags.holdingRight = (stateFlags & (1 << 2)) != 0;
    state.flags.platformer = (stateFlags & (1 << 3)) != 0;
    state.flags.dead = (stateFlags & (1 << 4)) != 0;
    state.environment.dualContext = (stateFlags & (1 << 5)) != 0;
    state.environment.twoPlayerContext = (stateFlags & (1 << 6)) != 0;
    bool extendedFlagSet = (stateFlags & (1 << 7)) != 0;
    uint8_t holdMask = readLE<uint8_t>(ctx);
    if ((holdMask & 0xF0u) != 0) {
        ctx.failed = true;
        return state;
    }
    state.flags.buttonHolds = unpackHoldMask(holdMask);

    uint32_t extendedFlags = readLE<uint32_t>(ctx);
    if (extendedFlags & 0xFFF80000u) {
        ctx.failed = true;
        return state;
    }
    unpackExtendedPlayerFlags(state, extendedFlags);
    state.motion.dashX = readLE<double>(ctx);
    state.motion.dashY = readLE<double>(ctx);
    state.motion.dashAngle = readLE<double>(ctx);
    state.motion.dashStartTime = readLE<double>(ctx);
    state.motion.slopeStartTime = readLE<double>(ctx);
    state.motion.fallSpeed = readLE<float>(ctx);
    state.motion.slopeVelocity = readLE<float>(ctx);
    state.motion.shipRotation.x = readLE<float>(ctx);
    state.motion.shipRotation.y = readLE<float>(ctx);
    state.motion.lastPortalPosition.x = readLE<float>(ctx);
    state.motion.lastPortalPosition.y = readLE<float>(ctx);
    state.motion.stateForceVector.x = readLE<float>(ctx);
    state.motion.stateForceVector.y = readLE<float>(ctx);
    state.environment.gravityMod = readLE<float>(ctx);
    state.environment.playerSpeed = readLE<float>(ctx);
    state.environment.playerSpeedAC = readLE<float>(ctx);
    state.environment.speedMultiplier = readLE<double>(ctx);
    state.environment.vehicleSize = readLE<float>(ctx);
    state.environment.reverseRelated = readLE<int32_t>(ctx);
    state.environment.stateDartSlide = readLE<int32_t>(ctx);
    state.environment.stateFlipGravity = readLE<int32_t>(ctx);
    state.environment.stateForce = readLE<int32_t>(ctx);
    state.environment.lastFlipTime = readLE<double>(ctx);
    state.environment.lastSpiderFlipTime = readLE<double>(ctx);
    auto finite = [](auto value) {
        return std::isfinite(static_cast<double>(value));
    };
    if (ctx.failed ||
        !finite(state.motion.position.x) ||
        !finite(state.motion.position.y) ||
        !finite(state.motion.verticalVelocity) ||
        !finite(state.motion.preSlopeVerticalVelocity) ||
        !finite(state.motion.horizontalVelocity) ||
        !finite(state.motion.rotation) ||
        !finite(state.environment.gravity) ||
        !finite(state.motion.dashX) ||
        !finite(state.motion.dashY) ||
        !finite(state.motion.dashAngle) ||
        !finite(state.motion.dashStartTime) ||
        !finite(state.motion.slopeStartTime) ||
        !finite(state.motion.fallSpeed) ||
        !finite(state.motion.slopeVelocity) ||
        !finite(state.motion.shipRotation.x) ||
        !finite(state.motion.shipRotation.y) ||
        !finite(state.motion.lastPortalPosition.x) ||
        !finite(state.motion.lastPortalPosition.y) ||
        !finite(state.motion.stateForceVector.x) ||
        !finite(state.motion.stateForceVector.y) ||
        !finite(state.environment.gravityMod) ||
        !finite(state.environment.playerSpeed) ||
        !finite(state.environment.playerSpeedAC) ||
        !finite(state.environment.speedMultiplier) ||
        !finite(state.environment.vehicleSize)) {
        ctx.failed = true;
        return state;
    }

    state.environment.extendedState = extendedFlagSet;
    return state;
}

static void writeAnchor(std::vector<uint8_t>& payload, Anchor const& anchor, TimeStream& time) {
    time.write(payload, anchor.timeSeconds);
    uint8_t flags = 0;
    if (anchor.state.hasPlayer2) flags |= 1 << 0;
    if (anchor.state.rng.fastRandState != 0) flags |= 1 << 1;
    writeLE<uint8_t>(payload, flags);
    writeLE<uint8_t>(payload, anchor.state.player1LatchMask);
    writeLE<uint8_t>(payload, anchor.state.player2LatchMask);
    writeLE<uint8_t>(payload, 0);
    writePlayerState(payload, anchor.state.player1);
    if ((flags & (1 << 0)) != 0) {
        writePlayerState(payload, anchor.state.player2);
    }
    if ((flags & (1 << 1)) != 0) {
        writeLE<uint64_t>(payload, static_cast<uint64_t>(anchor.state.rng.fastRandState));
    }
    writeLE<uint8_t>(payload, anchor.state.rng.locked ? 1 : 0);
    // r0: bit 0 = hasTripleRng (shake/teleport data follows after seed)
    uint8_t r0 = 0;
    if (anchor.state.rng.shakeState != 0 || anchor.state.rng.teleportState != 0) {
        r0 |= 1 << 0;
    }
    writeLE<uint8_t>(payload, r0);
    writeLE<uint8_t>(payload, 0);
    writeLE<uint8_t>(payload, 0);
    writeLE<uint32_t>(payload, anchor.state.rng.seed);
    // Triple RNG data (optional, indicated by r0 bit 0).
    if ((r0 & (1 << 0)) != 0) {
        writeLE<uint64_t>(payload, anchor.state.rng.shakeState);
        writeLE<uint64_t>(payload, anchor.state.rng.teleportState);
    }
}

static Anchor readAnchor(ReadContext& ctx, TimeStream& time, std::string* error) {
    Anchor anchor;
    anchor.timeSeconds = time.read(ctx);
    uint8_t flags = readLE<uint8_t>(ctx);
    anchor.state.player1LatchMask = readLE<uint8_t>(ctx);
    anchor.state.player2LatchMask = readLE<uint8_t>(ctx);
    uint8_t reserved = readLE<uint8_t>(ctx);
    if (!validTime(anchor.timeSeconds) || reserved != 0 || (flags & ~0x03u) != 0) {
        ctx.failed = true;
        if (error) *error = "invalid SKK3 anchor";
        return anchor;
    }
    anchor.state.hasPlayer2 = (flags & (1 << 0)) != 0;
    anchor.state.player1 = readPlayerState(ctx);
    if (ctx.failed) {
        if (error && error->empty()) *error = "invalid SKK3 anchor player1";
        return anchor;
    }
    if (anchor.state.hasPlayer2) {
        anchor.state.player2 = readPlayerState(ctx);
        if (ctx.failed) {
            if (error && error->empty()) *error = "invalid SKK3 anchor player2";
            return anchor;
        }
    }
    if ((flags & (1 << 1)) != 0) {
        anchor.state.rng.fastRandState = static_cast<uintptr_t>(readLE<uint64_t>(ctx));
    }
    anchor.state.rng.locked = readLE<uint8_t>(ctx) != 0;
    uint8_t r0 = readLE<uint8_t>(ctx);
    uint8_t r1 = readLE<uint8_t>(ctx);
    uint8_t r2 = readLE<uint8_t>(ctx);
    anchor.state.rng.seed = readLE<uint32_t>(ctx);
    if (ctx.failed) {
        if (error && error->empty()) *error = "invalid SKK3 anchor";
        return anchor;
    }
    // Triple RNG data (optional, indicated by r0 bit 0). Old files have r0=0.
    if ((r0 & (1 << 0)) != 0) {
        anchor.state.rng.shakeState = readLE<uint64_t>(ctx);
        anchor.state.rng.teleportState = readLE<uint64_t>(ctx);
    }
    // Validate reserved bytes: r1/r2 must be 0, r0 flags must be recognized.
    if (r1 != 0 || r2 != 0 || (r0 & ~0x01u) != 0) {
        ctx.failed = true;
        if (error && error->empty()) *error = "invalid SKK3 anchor";
    }
    return anchor;
}

static void writeAnchorsSection(std::vector<uint8_t>& out, std::vector<Anchor> const& anchors, double baseTps) {
    writeVarint(out, static_cast<uint64_t>(anchors.size()));
    TimeStream time;
    time.reset(baseTps);
    for (auto const& anchor : anchors) {
        writeAnchor(out, anchor, time);
    }
}

static std::vector<Anchor> readAnchorsSection(ReadContext& ctx, double baseTps, std::string* error) {
    std::vector<Anchor> anchors;
    uint64_t count = readVarint(ctx);
    if (ctx.failed || !checkCountFits(ctx, count, 2, error, "anchor")) return anchors;
    TimeStream time;
    time.reset(validTps(baseTps) ? baseTps : 240.0);
    anchors.reserve(static_cast<size_t>(count));
    for (uint64_t index = 0; index < count && !ctx.failed; ++index) {
        Anchor anchor = readAnchor(ctx, time, error);
        if (!ctx.failed) {
            anchors.push_back(anchor);
        }
    }
    return anchors;
}

static void writePerFrame(std::vector<uint8_t>& payload, PerFrame const& frame, TimeStream& time,
    BitDeltaStream_u32& p1xy, BitDeltaStream_u32& p1rot, BitDeltaStream_u64& p1vy,
    BitDeltaStream_u32& p2xy, BitDeltaStream_u32& p2rot, BitDeltaStream_u64& p2vy) {
    time.write(payload, frame.timeSeconds);
    uint8_t flags = 0;
    if (frame.hasPlayer2) flags |= 1 << 0;
    writeLE<uint8_t>(payload, flags);
    p1xy.write(payload, frame.p1x);
    p1xy.write(payload, frame.p1y);
    p1vy.write(payload, frame.p1vy);
    p1vy.write(payload, frame.p1vyb);
    p1rot.write(payload, frame.p1rot);
    writeLE<uint8_t>(payload, frame.p1vis);
    writeZigzag(payload, frame.p1RingId);
    if (frame.hasPlayer2) {
        p2xy.write(payload, frame.p2x);
        p2xy.write(payload, frame.p2y);
        p2vy.write(payload, frame.p2vy);
        p2vy.write(payload, frame.p2vyb);
        p2rot.write(payload, frame.p2rot);
        writeLE<uint8_t>(payload, frame.p2vis);
        writeZigzag(payload, frame.p2RingId);
    }
}

static PerFrame readPerFrame(ReadContext& ctx, TimeStream& time,
    BitDeltaStream_u32& p1xy, BitDeltaStream_u32& p1rot, BitDeltaStream_u64& p1vy,
    BitDeltaStream_u32& p2xy, BitDeltaStream_u32& p2rot, BitDeltaStream_u64& p2vy) {
    PerFrame frame;
    frame.timeSeconds = time.read(ctx);
    uint8_t flags = readLE<uint8_t>(ctx);
    frame.p1x = p1xy.read(ctx);
    frame.p1y = p1xy.read(ctx);
    frame.p1vy = p1vy.read(ctx);
    frame.p1vyb = p1vy.read(ctx);
    // p1 visual tail sits between p1 physics and p2 block in the binary layout
    frame.p1rot = p1rot.read(ctx);
    frame.p1vis = readLE<uint8_t>(ctx);
    frame.p1RingId = static_cast<int32_t>(readZigzag(ctx));
    if (ctx.failed) return frame;
    if (!validTime(frame.timeSeconds) || (flags & ~0x01u) != 0) {
        ctx.failed = true;
        return frame;
    }
    frame.hasPlayer2 = (flags & (1 << 0)) != 0;
    if (frame.hasPlayer2) {
        frame.p2x = p2xy.read(ctx);
        frame.p2y = p2xy.read(ctx);
        frame.p2vy = p2vy.read(ctx);
        frame.p2vyb = p2vy.read(ctx);
        frame.p2rot = p2rot.read(ctx);
        frame.p2vis = readLE<uint8_t>(ctx);
        frame.p2RingId = static_cast<int32_t>(readZigzag(ctx));
    }
    if (ctx.failed) return frame;
    if (!std::isfinite(frame.p1x) || !std::isfinite(frame.p1y) || !std::isfinite(frame.p1vy)
        || !std::isfinite(frame.p1vyb)
        || (frame.hasPlayer2 && (!std::isfinite(frame.p2x) || !std::isfinite(frame.p2y)
                                 || !std::isfinite(frame.p2vy) || !std::isfinite(frame.p2vyb)))) {
        ctx.failed = true;
        return frame;
    }
    if (!std::isfinite(frame.p1rot)
        || (frame.hasPlayer2 && !std::isfinite(frame.p2rot))) {
        ctx.failed = true;
    }
    return frame;
}

// ─── Per-frame section v4 (wire 7, layout 4) ───────────────────────────────
// Density model: every column becomes a *first difference* of a fixed-point
// integer (positions x1/256 px, velocities /16384, rotation /1024, exact ints
// for vis/ring-id; time keeps the tick+picosecond deltas), then whole records
// whose 17-element difference vector repeats are coalesced into a run-length
// block. Holds/gravity/autoclickers have constant per-frame deltas -> one tiny
// head + a run counter covers thousands of frames. Fixed-point math is integer
// exact, so round-trip is deterministic and drift-free; reconstructed pins lie
// within 0.5/scale of the captured floats (< 0.002 px, invisible).
static constexpr double kPerFrameScaleXy = 256.0;
static constexpr double kPerFrameScaleVy = 16384.0;
static constexpr double kPerFrameScaleRot = 1024.0;

// Deltas per column: 0 tick, 1 residue-ps, 2 flags, 3..9 p1 (x,y,vy,vyb,rot,vis,ring),
// 10..16 p2 (same 7).
static constexpr int kPerFrameDeltaCount = 17;

static int64_t perFrameQuant(double value, double scale) {
    return static_cast<int64_t>(std::llround(value * scale));
}

static void writePerFrameSectionV3(std::vector<uint8_t>& out, std::vector<PerFrame> const& frames, double baseTps) {
    writeLE<uint8_t>(out, 3); // v3: rotation + vis flags + dash-ring IDs
    writeVarint(out, static_cast<uint64_t>(frames.size()));
    TimeStream time;
    time.reset(baseTps);
    BitDeltaStream_u32 p1xy, p1rot, p2xy, p2rot;
    BitDeltaStream_u64 p1vy, p2vy;
    for (auto const& frame : frames) {
        writePerFrame(out, frame, time, p1xy, p1rot, p1vy, p2xy, p2rot, p2vy);
    }
}

static void writePerFrameSection(std::vector<uint8_t>& out, std::vector<PerFrame> const& frames, double baseTps) {
    double tps = validTps(baseTps) ? baseTps : 240.0;
    double unit = 1.0 / tps;
    writeLE<uint8_t>(out, 4); // v4: fixed-point first-diff + record-run RLE
    writeVarint(out, static_cast<uint64_t>(frames.size()));

    int64_t prevTick = 0, prevResPs = 0, prevFlags = 0;
    int64_t prevQ[14] = { 0 };
    int64_t lastD[kPerFrameDeltaCount] = { 0 };
    bool haveRun = false;
    uint64_t runLen = 0;
    std::vector<uint8_t> head;
    head.reserve(48);

    auto flush = [&]() {
        if (!haveRun) return;
        writeVarint(out, runLen);
        out.insert(out.end(), head.begin(), head.end());
        head.clear();
        haveRun = false;
        runLen = 0;
    };

    for (auto const& frame : frames) {
        int64_t tick = static_cast<int64_t>(std::llround(frame.timeSeconds * tps));
        int64_t resPs = static_cast<int64_t>(std::llround(
            (frame.timeSeconds - static_cast<double>(tick) * unit) * 1e12));
        int64_t flags = frame.hasPlayer2 ? 1 : 0;
        int64_t q[14];
        q[0] = perFrameQuant(static_cast<double>(frame.p1x), kPerFrameScaleXy);
        q[1] = perFrameQuant(static_cast<double>(frame.p1y), kPerFrameScaleXy);
        q[2] = perFrameQuant(frame.p1vy, kPerFrameScaleVy);
        q[3] = perFrameQuant(frame.p1vyb, kPerFrameScaleVy);
        q[4] = perFrameQuant(static_cast<double>(frame.p1rot), kPerFrameScaleRot);
        q[5] = frame.p1vis;
        q[6] = frame.p1RingId;
        if (flags) {
            q[7] = perFrameQuant(static_cast<double>(frame.p2x), kPerFrameScaleXy);
            q[8] = perFrameQuant(static_cast<double>(frame.p2y), kPerFrameScaleXy);
            q[9] = perFrameQuant(frame.p2vy, kPerFrameScaleVy);
            q[10] = perFrameQuant(frame.p2vyb, kPerFrameScaleVy);
            q[11] = perFrameQuant(static_cast<double>(frame.p2rot), kPerFrameScaleRot);
            q[12] = frame.p2vis;
            q[13] = frame.p2RingId;
        } else {
            for (int j = 7; j < 14; ++j) q[j] = 0;
        }

        int64_t d[kPerFrameDeltaCount];
        d[0] = tick - prevTick;
        d[1] = resPs - prevResPs;
        d[2] = flags - prevFlags;
        for (int j = 0; j < 7; ++j) d[3 + j] = q[j] - prevQ[j];
        if (flags) {
            for (int j = 7; j < 14; ++j) d[10 + (j - 7)] = q[j] - prevQ[j];
        } else {
            for (int j = 10; j < kPerFrameDeltaCount; ++j) d[j] = 0;
        }

        prevTick = tick;
        prevResPs = resPs;
        prevFlags = flags;
        for (int j = 0; j < 14; ++j) prevQ[j] = q[j];

        bool same = haveRun;
        for (int j = 0; same && j < kPerFrameDeltaCount; ++j) {
            if (d[j] != lastD[j]) same = false;
        }
        if (same) {
            ++runLen;
            continue;
        }
        flush();
        haveRun = true;
        runLen = 1;
        for (int j = 0; j < kPerFrameDeltaCount; ++j) lastD[j] = d[j];

        head.clear();
        writeZigzag(head, d[0]);
        writeZigzag(head, d[1]);
        head.push_back(static_cast<uint8_t>(flags));
        for (int j = 3; j < 10; ++j) writeZigzag(head, d[j]);
        if (flags) {
            for (int j = 10; j < kPerFrameDeltaCount; ++j) writeZigzag(head, d[j]);
        }
    }
    flush();
}

static std::vector<PerFrame> readPerFrameSectionV4(ReadContext& ctx, double baseTps, std::string* error) {
    double tps = validTps(baseTps) ? baseTps : 240.0;
    double unit = 1.0 / tps;
    uint64_t count = readVarint(ctx);
    if (ctx.failed || !checkCountFits(ctx, count, 1, error, "per-frame")) return {};

    std::vector<PerFrame> frames;
    frames.reserve(static_cast<size_t>(count));
    uint64_t nextIndex = 0;
    int64_t prevTick = 0, prevResPs = 0;
    int64_t prevQ[14] = { 0 };
    int64_t lastD[kPerFrameDeltaCount] = { 0 };

    auto applyDelta = [&](int hasP2, int64_t const* d, PerFrame& frame) {
        prevTick += d[0];
        prevResPs += d[1];
        frame.timeSeconds = static_cast<double>(prevTick) * unit + static_cast<double>(prevResPs) / 1e12;
        frame.hasPlayer2 = hasP2 != 0;
        prevQ[0] += d[3];
        prevQ[1] += d[4];
        prevQ[2] += d[5];
        prevQ[3] += d[6];
        prevQ[4] += d[7];
        prevQ[5] += d[8];
        prevQ[6] += d[9];
        frame.p1x = static_cast<float>(static_cast<double>(prevQ[0]) / kPerFrameScaleXy);
        frame.p1y = static_cast<float>(static_cast<double>(prevQ[1]) / kPerFrameScaleXy);
        frame.p1vy = static_cast<double>(prevQ[2]) / kPerFrameScaleVy;
        frame.p1vyb = static_cast<double>(prevQ[3]) / kPerFrameScaleVy;
        frame.p1rot = static_cast<float>(static_cast<double>(prevQ[4]) / kPerFrameScaleRot);
        frame.p1vis = static_cast<uint8_t>(prevQ[5]);
        frame.p1RingId = static_cast<int32_t>(prevQ[6]);
        if (hasP2) {
            prevQ[7] += d[10];
            prevQ[8] += d[11];
            prevQ[9] += d[12];
            prevQ[10] += d[13];
            prevQ[11] += d[14];
            prevQ[12] += d[15];
            prevQ[13] += d[16];
            frame.p2x = static_cast<float>(static_cast<double>(prevQ[7]) / kPerFrameScaleXy);
            frame.p2y = static_cast<float>(static_cast<double>(prevQ[8]) / kPerFrameScaleXy);
            frame.p2vy = static_cast<double>(prevQ[9]) / kPerFrameScaleVy;
            frame.p2vyb = static_cast<double>(prevQ[10]) / kPerFrameScaleVy;
            frame.p2rot = static_cast<float>(static_cast<double>(prevQ[11]) / kPerFrameScaleRot);
            frame.p2vis = static_cast<uint8_t>(prevQ[12]);
            frame.p2RingId = static_cast<int32_t>(prevQ[13]);
        } else {
            for (int j = 7; j < 14; ++j) prevQ[j] = 0;
        }
    };

    auto emit = [&](PerFrame const& frame) {
        if (!frames.empty() && frame.timeSeconds < frames.back().timeSeconds) {
            if (error) *error = "unsorted per-frames";
            ctx.failed = true;
            return;
        }
        frames.push_back(frame);
        ++nextIndex;
    };

    while (nextIndex < count && !ctx.failed) {
        uint64_t runLen = readVarint(ctx);
        if (ctx.failed || runLen == 0 || runLen > count - nextIndex) {
            if (error) *error = "per-frame run exceeds section count";
            ctx.failed = true;
            break;
        }
        lastD[0] = readZigzag(ctx);
        lastD[1] = readZigzag(ctx);
        if (ctx.failed) break;
        uint8_t flags = readLE<uint8_t>(ctx);
        if (ctx.failed) break;
        if ((flags & ~0x01u) != 0) {
            if (error) *error = "invalid per-frame flags";
            ctx.failed = true;
            break;
        }
        int hasP2 = flags & 1;
        for (int j = 3; j < 10; ++j) lastD[j] = readZigzag(ctx);
        if (hasP2) {
            for (int j = 10; j < kPerFrameDeltaCount; ++j) lastD[j] = readZigzag(ctx);
        }
        if (ctx.failed) break;

        for (uint64_t k = 0; k < runLen && !ctx.failed; ++k) {
            PerFrame frame;
            applyDelta(hasP2, lastD, frame);
            if (ctx.failed) break;
            if (!std::isfinite(frame.timeSeconds)
                || !std::isfinite(frame.p1x) || !std::isfinite(frame.p1y)
                || !std::isfinite(frame.p1vy) || !std::isfinite(frame.p1vyb)
                || (hasP2 && (!std::isfinite(frame.p2x) || !std::isfinite(frame.p2y)
                              || !std::isfinite(frame.p2vy) || !std::isfinite(frame.p2vyb)))) {
                ctx.failed = true;
                break;
            }
            emit(frame);
        }
    }
    if (nextIndex != count) {
        ctx.failed = true;
    }
    return frames;
}

static std::vector<PerFrame> readPerFrameSection(ReadContext& ctx, double baseTps, std::string* error) {
    std::vector<PerFrame> frames;
    uint8_t layout = readLE<uint8_t>(ctx);
    if (ctx.failed) return frames;
    if (layout == 3) {
        uint64_t count = readVarint(ctx);
        if (ctx.failed || !checkCountFits(ctx, count, 3, error, "per-frame")) return frames;
        TimeStream time;
        time.reset(validTps(baseTps) ? baseTps : 240.0);
        BitDeltaStream_u32 p1xy, p1rot, p2xy, p2rot;
        BitDeltaStream_u64 p1vy, p2vy;
        frames.reserve(static_cast<size_t>(count));
        for (uint64_t index = 0; index < count && !ctx.failed; ++index) {
            PerFrame frame = readPerFrame(ctx, time, p1xy, p1rot, p1vy, p2xy, p2rot, p2vy);
            if (!ctx.failed) {
                if (!frames.empty() && frame.timeSeconds < frames.back().timeSeconds) {
                    if (error) *error = "unsorted per-frames";
                    ctx.failed = true;
                    break;
                }
                frames.push_back(frame);
            }
        }
        return frames;
    }
    if (layout == 4) {
        return readPerFrameSectionV4(ctx, baseTps, error);
    }
    if (error) *error = "unsupported old SKK3 per-frame layout";
    ctx.failed = true;
    return frames;
}

static void writeInputsSectionRelative(std::vector<uint8_t>& out, std::vector<Input> const& inputs, double baseTps) {
    writeInputsSection(out, inputs, baseTps);
}

static void writeVarianceSection(std::vector<uint8_t>& out, std::vector<VarianceEntry> const& entries, uint64_t setupSeed,
    std::vector<std::pair<int32_t, int32_t>> const& items) {
    writeVarint(out, static_cast<uint64_t>(entries.size()));
    BitDeltaStream_u32 value;
    uint64_t lastIndex = 0;
    for (auto const& entry : entries) {
        writeZigzag(out, static_cast<int64_t>(entry.index) - static_cast<int64_t>(lastIndex));
        lastIndex = entry.index;
        value.write(out, entry.value);
    }
    writeVarint(out, setupSeed);
    writeVarint(out, static_cast<uint64_t>(items.size()));
    for (auto const& item : items) {
        writeZigzag(out, item.first);
        writeZigzag(out, item.second);
    }
}

static std::vector<VarianceEntry> readVarianceSection(ReadContext& ctx, std::string* error, uint64_t& setupSeed,
    std::vector<std::pair<int32_t, int32_t>>& items) {
    std::vector<VarianceEntry> entries;
    setupSeed = 0;
    items.clear();
    uint64_t count = readVarint(ctx);
    if (ctx.failed || !checkCountFits(ctx, count, 2, error, "variance")) return entries;
    BitDeltaStream_u32 value;
    entries.reserve(static_cast<size_t>(count));
    uint16_t lastIndex = 0;
    for (uint64_t index = 0; index < count && !ctx.failed; ++index) {
        VarianceEntry entry;
        int64_t indexDelta = readZigzag(ctx);
        entry.value = value.read(ctx);
        if (ctx.failed) break;
        int64_t cumulative = static_cast<int64_t>(lastIndex) + indexDelta;
        // Indices must be strictly ascending, in range of the 2000-slot array,
        // and values finite (0 is meaningless but harmless; reject for hygiene).
        if (cumulative <= static_cast<int64_t>(lastIndex) && index > 0) {
            ctx.failed = true;
            if (error) *error = "invalid SKK3 variance entry";
            break;
        }
        if (cumulative >= 2000 || cumulative < 0 ||
            !std::isfinite(entry.value) || entry.value == 0.0f) {
            ctx.failed = true;
            if (error) *error = "invalid SKK3 variance entry";
            break;
        }
        entry.index = static_cast<uint16_t>(cumulative);
        lastIndex = entry.index;
        entries.push_back(entry);
    }
    // Trailing setup-seed + persistent-items block (mandatory in wire v4).
    setupSeed = readVarint(ctx);
    uint64_t itemCount = readVarint(ctx);
    if (!ctx.failed && !checkCountFits(ctx, itemCount, 2, error, "persistent items")) return entries;
    for (uint64_t i = 0; i < itemCount && !ctx.failed; ++i) {
        int32_t key = static_cast<int32_t>(readZigzag(ctx));
        int32_t value = static_cast<int32_t>(readZigzag(ctx));
        if (ctx.failed) break;
        items.emplace_back(key, value);
    }
    return entries;
}

static std::vector<Input> readInputsSectionRelative(ReadContext& ctx, double framerateHint, std::string* error) {
    return readInputsSection(ctx, framerateHint, error);
}

static void writePersistenceSection(std::vector<uint8_t>& out, std::vector<Attempt> const& attempts, double baseTps) {
    writeVarint(out, static_cast<uint64_t>(attempts.size()));
    TimeStream deathTime;
    deathTime.reset(baseTps);
    for (auto const& attempt : attempts) {
        deathTime.write(out, attempt.deathTimeSeconds);
        writeLE<uint8_t>(out, attempt.deathPlayer2 ? 1 : 0);
        // Reserved slot 1 doubles as "attempt carries a per-frame sub-block"
        // (written after the anchors). 0 keeps old readers fully compatible.
        writeLE<uint8_t>(out, attempt.perFrames.empty() ? 0 : 1);
        writeLE<uint8_t>(out, 0);
        writeLE<uint8_t>(out, 0);
        writeLE<uint8_t>(out, 0);
        writeLE<uint8_t>(out, 0);
        writeLE<uint8_t>(out, 0);
        writeLE<uint8_t>(out, 0);
        writeInputsSectionRelative(out, attempt.inputs, baseTps);
        writeAnchorsSection(out, attempt.anchors, baseTps);
        if (!attempt.perFrames.empty()) {
            writePerFrameSection(out, attempt.perFrames, baseTps);
        }
    }
}

static std::vector<Attempt> readPersistenceSection(ReadContext& ctx, double framerateHint, std::string* error) {
    std::vector<Attempt> attempts;
    uint64_t count = readVarint(ctx);
    if (ctx.failed || !checkCountFits(ctx, count, 2, error, "persistence attempt")) return attempts;
    double baseTps = validTps(framerateHint) ? framerateHint : 240.0;
    TimeStream deathTime;
    deathTime.reset(baseTps);
    attempts.reserve(static_cast<size_t>(count));

    for (uint64_t index = 0; index < count && !ctx.failed; ++index) {
        Attempt attempt;
        attempt.deathTimeSeconds = deathTime.read(ctx);
        attempt.deathPlayer2 = readLE<uint8_t>(ctx) != 0;
        uint8_t attemptPerFrames = readLE<uint8_t>(ctx);
        if (attemptPerFrames > 1) {
            ctx.failed = true;
            if (error) *error = "invalid SKK3 persistence attempt flags";
            break;
        }
        for (int i = 0; i < 6; ++i) {
            if (readLE<uint8_t>(ctx) != 0) {
                ctx.failed = true;
                if (error) *error = "invalid SKK3 persistence header";
                break;
            }
        }
        if (ctx.failed) break;
        if (!validTime(attempt.deathTimeSeconds)) {
            ctx.failed = true;
            if (error) *error = "invalid SKK3 persistence death time";
            break;
        }
        attempt.inputs = readInputsSectionRelative(ctx, framerateHint, error);
        if (ctx.failed) break;
        attempt.anchors = readAnchorsSection(ctx, baseTps, error);
        if (ctx.failed) break;
        if (attemptPerFrames != 0) {
            attempt.perFrames = readPerFrameSection(ctx, baseTps, error);
            if (ctx.failed) break;
        }
        if (attempt.deathTimeSeconds == 0.0 && attempt.inputs.empty() && attempt.anchors.empty() &&
            attempt.perFrames.empty()) {
            ctx.failed = true;
            if (error) *error = "empty SKK3 persistence attempt";
            break;
        }
        attempts.push_back(std::move(attempt));
    }

    return attempts;
}

template <typename T>
static T readLE(ReadContext& ctx) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value {};
    if (ctx.failed || ctx.remaining() < sizeof(T)) {
        ctx.failed = true;
        return value;
    }

    std::memcpy(&value, ctx.data.data() + ctx.position, sizeof(T));
    ctx.position += sizeof(T);
    return value;
}

static uint32_t buildFlags(Macro const& macro, WriteOptions const& options) {
    uint32_t flags = 0;
    if (macro.losslessVerified) flags |= FlagLosslessVerified;
    if (macro.stepClock) flags |= FlagStepClock;
    if (macro.recordedFromStartPos) flags |= FlagFromStartPos;
    if (macro.platformerMode) flags |= FlagPlatformer;
    if (macro.twoPlayerMode) flags |= FlagTwoPlayer;
    if (macro.rngLocked) flags |= FlagRngLocked;
    if (!macro.anchors.empty()) flags |= FlagHasAnchors;
    if (!macro.checkpoints.empty()) flags |= FlagHasCheckpoints;
    if (!macro.practiceFixCheckpoints.empty()) flags |= FlagHasPracticeFix;
    if (!macro.persistenceAttempts.empty()) flags |= FlagHasPersistence;
    if (!macro.perFrames.empty()) flags |= FlagHasPerFrame;
    if (!macro.variance.empty() || macro.setupRngSeed != 0 || !macro.persistentItems.empty()) flags |= FlagHasVariance;
    if (!macro.rngBoundaries.empty()) flags |= FlagHasRngBoundaries;
    if (!macro.tpsEvents.empty()) flags |= FlagHasTpsEvents;
    if (macro.macroConverted) flags |= FlagMacroConverted;
    if (options.compressPayload) flags |= FlagCompressed;
    return flags;
}

static uint8_t inputFlags(Input const& input) {
    uint8_t flags = 0;
    if (input.player2) flags |= 1u << 0;
    if (input.pressed) flags |= 1u << 1;
    if (input.swiftPairAnchor) flags |= 1u << 2;
    return flags;
}

static void writeInputsSection(std::vector<uint8_t>& out, std::vector<Input> const& inputs, double baseTps) {
    writeVarint(out, static_cast<uint64_t>(inputs.size()));
    TimeStream time;
    time.reset(baseTps);
    for (size_t index = 0; index < inputs.size(); ++index) {
        Input const& input = inputs[index];
        bool swiftFree = !input.swiftPairAnchor &&
            (index == 0 || !inputs[index - 1].swiftPairAnchor);
        Input const* prev = index > 0 ? &inputs[index - 1] : nullptr;
        bool sameButton = prev && input.button == prev->button &&
            input.player2 == prev->player2;
        if (swiftFree && sameButton && time.tryRepeat(input.timeSeconds)) {
            // RLE repeat: same button/player2, same cycle delta, pressed in flags.
            writeLE<uint8_t>(out, 0);
            writeLE<uint8_t>(out, input.pressed ? 2 : 0);
            continue;
        }
        writeLE<uint8_t>(out, input.button);
        writeLE<uint8_t>(out, inputFlags(input));
        time.write(out, input.timeSeconds);
    }
}

static void writeTpsEventsSection(std::vector<uint8_t>& out, std::vector<TpsEvent> const& events, double baseTps) {
    writeVarint(out, static_cast<uint64_t>(events.size()));
    TimeStream time;
    time.reset(baseTps);
    BitDeltaStream_u64 tps;
    for (auto const& event : events) {
        time.write(out, event.timeSeconds);
        tps.write(out, event.tps);
    }
}

static void writeCheckpointsSection(std::vector<uint8_t>& out, std::vector<Checkpoint> const& checkpoints, double baseTps) {
    writeVarint(out, static_cast<uint64_t>(checkpoints.size()));
    TimeStream time;
    time.reset(baseTps);
    for (auto const& checkpoint : checkpoints) {
        time.write(out, checkpoint.timeSeconds);
        writeLE<uint64_t>(out, checkpoint.rngState);
        time.write(out, checkpoint.priorTimeSeconds);
    }
}

struct SectionEntry {
    uint8_t kind = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

static void appendSection(
    std::vector<SectionEntry>& entries,
    std::vector<uint8_t>& payload,
    SectionKind kind,
    auto writer
) {
    SectionEntry entry;
    entry.kind = static_cast<uint8_t>(kind);
    entry.offset = static_cast<uint64_t>(payload.size());
    writer(payload);
    entry.size = static_cast<uint64_t>(payload.size() - static_cast<size_t>(entry.offset));
    entries.push_back(entry);
}

static bool checkCountFits(ReadContext& ctx, uint64_t count, size_t minRecordSize, std::string* error, char const* label) {
    if (minRecordSize == 0) {
        return true;
    }
    if (count > static_cast<uint64_t>(ctx.remaining() / minRecordSize)) {
        if (error) *error = std::string(label) + " count exceeds section size";
        ctx.failed = true;
        return false;
    }
    return true;
}

static std::vector<Input> readInputsSection(ReadContext& ctx, double framerateHint, std::string* error) {
    std::vector<Input> inputs;
    uint64_t count = readVarint(ctx);
    if (ctx.failed) return inputs;
    if (count > 100000000ull) {
        if (error) *error = "input count exceeds cap";
        ctx.failed = true;
        return inputs;
    }
    if (!checkCountFits(ctx, count, 2, error, "input")) return inputs;

    TimeStream time;
    time.reset(validTps(framerateHint) ? framerateHint : 240.0);
    inputs.reserve(static_cast<size_t>(count));
    for (uint64_t index = 0; index < count && !ctx.failed; ++index) {
        Input input;
        uint8_t button = readLE<uint8_t>(ctx);
        uint8_t flags = readLE<uint8_t>(ctx);
        if (button == 0) {
            // RLE repeat: same button/player2/cycle as previous record.
            if ((flags & ~0x02u) != 0 || inputs.empty()) {
                if (error) *error = "unknown input repeat flag bits";
                ctx.failed = true;
                break;
            }
            Input const& prev = inputs.back();
            input.timeSeconds = time.readRepeat(ctx);
            input.button = prev.button;
            input.player2 = prev.player2;
            input.pressed = (flags & (1u << 1)) != 0;
            input.swiftPairAnchor = false;
        } else {
            if (button < 1 || button > 3) {
                if (error) *error = "invalid input button";
                ctx.failed = true;
                break;
            }
            if ((flags & ~0x07u) != 0) {
                if (error) *error = "unknown input flag bits";
                ctx.failed = true;
                break;
            }
            input.timeSeconds = time.read(ctx);
            input.button = button;
            input.player2 = (flags & (1u << 0)) != 0;
            input.pressed = (flags & (1u << 1)) != 0;
            input.swiftPairAnchor = (flags & (1u << 2)) != 0;
        }
        if (!validTime(input.timeSeconds)) {
            if (error) *error = "invalid input time";
            ctx.failed = true;
            break;
        }
        if (!inputs.empty() && input.timeSeconds < inputs.back().timeSeconds) {
            if (error) *error = "unsorted inputs";
            ctx.failed = true;
            break;
        }
        inputs.push_back(input);
    }

    double swiftPairTolerance = 1.0 / (validTps(framerateHint) ? framerateHint : 240.0);
    for (size_t i = 0; i < inputs.size() && !ctx.failed; ++i) {
        auto const& input = inputs[i];
        if (!input.swiftPairAnchor) {
            continue;
        }
        if (i + 1 >= inputs.size()) {
            if (error) *error = "malformed swift pair";
            ctx.failed = true;
            break;
        }
        auto& next = inputs[i + 1];
        bool matchingPair = input.button == next.button &&
            input.player2 == next.player2 &&
            input.pressed != next.pressed &&
            std::abs(input.timeSeconds - next.timeSeconds) < swiftPairTolerance;
        if (!matchingPair) {
            if (error) *error = "malformed swift pair";
            ctx.failed = true;
            break;
        }
        next.timeSeconds = input.timeSeconds;
    }

    return inputs;
}

static std::vector<TpsEvent> readTpsEventsSection(ReadContext& ctx, double baseTps, std::string* error) {
    std::vector<TpsEvent> events;
    uint64_t count = readVarint(ctx);
    if (ctx.failed || !checkCountFits(ctx, count, 2, error, "TPS event")) return events;
    TimeStream time;
    time.reset(validTps(baseTps) ? baseTps : 240.0);
    BitDeltaStream_u64 tps;
    events.reserve(static_cast<size_t>(count));

    for (uint64_t index = 0; index < count && !ctx.failed; ++index) {
        TpsEvent event;
        event.timeSeconds = time.read(ctx);
        event.tps = tps.read(ctx);
        if (!validTime(event.timeSeconds) || !validTps(event.tps)) {
            if (error) *error = "invalid TPS event";
            ctx.failed = true;
            break;
        }
        if (!events.empty() && event.timeSeconds < events.back().timeSeconds) {
            if (error) *error = "unsorted TPS events";
            ctx.failed = true;
            break;
        }
        events.push_back(event);
    }
    return events;
}

static std::vector<Checkpoint> readCheckpointsSection(ReadContext& ctx, double baseTps, std::string* error) {
    std::vector<Checkpoint> checkpoints;
    uint64_t count = readVarint(ctx);
    if (ctx.failed || !checkCountFits(ctx, count, 2, error, "checkpoint")) return checkpoints;
    TimeStream time;
    time.reset(validTps(baseTps) ? baseTps : 240.0);
    checkpoints.reserve(static_cast<size_t>(count));

    for (uint64_t index = 0; index < count && !ctx.failed; ++index) {
        Checkpoint checkpoint;
        checkpoint.timeSeconds = time.read(ctx);
        checkpoint.rngState = readLE<uint64_t>(ctx);
        checkpoint.priorTimeSeconds = time.read(ctx);
        if (!validTime(checkpoint.timeSeconds) || !validTime(checkpoint.priorTimeSeconds)) {
            if (error) *error = "invalid checkpoint time";
            ctx.failed = true;
            break;
        }
        if (!checkpoints.empty() && checkpoint.timeSeconds < checkpoints.back().timeSeconds) {
            if (error) *error = "unsorted checkpoints";
            ctx.failed = true;
            break;
        }
        checkpoints.push_back(checkpoint);
    }
    return checkpoints;
}

static void writePracticeFixSection(std::vector<uint8_t>& out, std::vector<PracticeFixCheckpoint> const& checkpoints) {
    writeVarint(out, static_cast<uint64_t>(checkpoints.size()));
    for (auto const& cp : checkpoints) {
        writeLE<double>(out, cp.timeSeconds);
        writeLE<double>(out, cp.priorTimeSeconds);
        writeLE<uint8_t>(out, cp.player1LatchMask);
        writeLE<uint8_t>(out, cp.player2LatchMask);
        writeLE<uint64_t>(out, cp.seedState);
        writeLE<uint64_t>(out, cp.shakeRandomState);
        writeLE<uint64_t>(out, cp.teleportRandomState);
        writeVarint(out, static_cast<uint64_t>(cp.brokenObjectUIDs.size()));
        for (auto uid : cp.brokenObjectUIDs) writeZigzag(out, uid);
        writeVarint(out, static_cast<uint64_t>(cp.persistentItems.size()));
        for (auto const& [group, id] : cp.persistentItems) {
            writeZigzag(out, group); writeZigzag(out, id);
        }
        writeVarint(out, static_cast<uint64_t>(cp.variance.size()));
        for (auto const& [idx, val] : cp.variance) {
            writeZigzag(out, idx); writeLE<float>(out, val);
        }
        uint8_t clickMask = 0;
        for (int i = 0; i < 6; ++i) {
            if (cp.clickReleased[i]) clickMask |= static_cast<uint8_t>(1u << i);
        }
        writeLE<uint8_t>(out, clickMask);
        // Full P1 state
        writeLE<float>(out, cp.p1_x); writeLE<float>(out, cp.p1_y); writeLE<float>(out, cp.p1_rotation);
        writeLE<double>(out, cp.p1_yVelocity); writeLE<double>(out, cp.p1_fallSpeed);
        writeLE<double>(out, cp.p1_platformerXVelocity); writeLE<double>(out, cp.p1_groundYVelocity);
        writeLE<double>(out, cp.p1_yVelocityBeforeSlope); writeLE<double>(out, cp.p1_yVelocityRelated);
        writeLE<double>(out, cp.p1_scaleXRelated); writeLE<double>(out, cp.p1_scaleXRelated2); writeLE<double>(out, cp.p1_scaleXRelated3);
        writeLE<double>(out, cp.p1_scaleXRelated4); writeLE<double>(out, cp.p1_scaleXRelated5);
        writeLE<double>(out, cp.p1_scaleXRelatedTime);
        writeLE<float>(out, cp.p1_playerSpeed); writeLE<float>(out, cp.p1_vehicleSize);
        writeLE<float>(out, cp.p1_platformerVelocityRelated);
        writeLE<float>(out, cp.p1_xVelocityRelated); writeLE<float>(out, cp.p1_xVelocityRelated2);
        writeLE<float>(out, cp.p1_yVelocityRelated3); writeLE<float>(out, cp.p1_slopeVelocity);
        writeLE<double>(out, cp.p1_gravity); writeLE<double>(out, cp.p1_speedMultiplier);
        writeLE<double>(out, cp.p1_snapDistance); writeLE<double>(out, cp.p1_accelerationOrSpeed);
        writeLE<double>(out, cp.p1_yStart);
        writeLE<float>(out, cp.p1_gravityMod); writeLE<float>(out, cp.p1_slopeAngle); writeLE<float>(out, cp.p1_slopeAngleRadians);
        writeLE<double>(out, cp.p1_slopeRotation); writeLE<double>(out, cp.p1_currentSlopeYVelocity);
        writeLE<double>(out, cp.p1_slopeStartTime);
        writeLE<double>(out, cp.p1_dashX); writeLE<double>(out, cp.p1_dashY); writeLE<double>(out, cp.p1_dashAngle); writeLE<double>(out, cp.p1_dashStartTime);
        writeLE<double>(out, cp.p1_totalTime); writeLE<double>(out, cp.p1_lastCheckpointTime);
        writeLE<double>(out, cp.p1_lastJumpTime); writeLE<double>(out, cp.p1_lastFlipTime); writeLE<double>(out, cp.p1_lastSpiderFlipTime);
        writeLE<double>(out, cp.p1_gameModeChangedTime); writeLE<double>(out, cp.p1_lastLandTime);
        writeLE<int32_t>(out, cp.p1_lastCollisionBottom); writeLE<int32_t>(out, cp.p1_lastCollisionTop);
        writeLE<int32_t>(out, cp.p1_lastCollisionLeft); writeLE<int32_t>(out, cp.p1_lastCollisionRight);
        writeLE<int32_t>(out, cp.p1_stateOnGround); writeLE<int32_t>(out, cp.p1_stateForce); writeLE<int32_t>(out, cp.p1_reverseRelated);
        writeLE<uint64_t>(out, cp.p1_flags1); writeLE<uint64_t>(out, cp.p1_flags2);
        uint8_t p1Hold = 0;
        if (cp.p1_holdingRight) p1Hold |= 1u;
        if (cp.p1_holdingLeft) p1Hold |= 2u;
        writeLE<uint8_t>(out, p1Hold);
        // Full P2 state
        writeLE<float>(out, cp.p2_x); writeLE<float>(out, cp.p2_y); writeLE<float>(out, cp.p2_rotation);
        writeLE<double>(out, cp.p2_yVelocity); writeLE<double>(out, cp.p2_fallSpeed);
        writeLE<double>(out, cp.p2_platformerXVelocity); writeLE<double>(out, cp.p2_groundYVelocity);
        writeLE<double>(out, cp.p2_yVelocityBeforeSlope); writeLE<double>(out, cp.p2_yVelocityRelated);
        writeLE<double>(out, cp.p2_scaleXRelated); writeLE<double>(out, cp.p2_scaleXRelated2); writeLE<double>(out, cp.p2_scaleXRelated3);
        writeLE<double>(out, cp.p2_scaleXRelated4); writeLE<double>(out, cp.p2_scaleXRelated5);
        writeLE<double>(out, cp.p2_scaleXRelatedTime);
        writeLE<float>(out, cp.p2_playerSpeed); writeLE<float>(out, cp.p2_vehicleSize);
        writeLE<float>(out, cp.p2_platformerVelocityRelated);
        writeLE<float>(out, cp.p2_xVelocityRelated); writeLE<float>(out, cp.p2_xVelocityRelated2);
        writeLE<float>(out, cp.p2_yVelocityRelated3); writeLE<float>(out, cp.p2_slopeVelocity);
        writeLE<double>(out, cp.p2_gravity); writeLE<double>(out, cp.p2_speedMultiplier);
        writeLE<double>(out, cp.p2_snapDistance); writeLE<double>(out, cp.p2_accelerationOrSpeed);
        writeLE<double>(out, cp.p2_yStart);
        writeLE<float>(out, cp.p2_gravityMod); writeLE<float>(out, cp.p2_slopeAngle); writeLE<float>(out, cp.p2_slopeAngleRadians);
        writeLE<double>(out, cp.p2_slopeRotation); writeLE<double>(out, cp.p2_currentSlopeYVelocity);
        writeLE<double>(out, cp.p2_slopeStartTime);
        writeLE<double>(out, cp.p2_dashX); writeLE<double>(out, cp.p2_dashY); writeLE<double>(out, cp.p2_dashAngle); writeLE<double>(out, cp.p2_dashStartTime);
        writeLE<double>(out, cp.p2_totalTime); writeLE<double>(out, cp.p2_lastCheckpointTime);
        writeLE<double>(out, cp.p2_lastJumpTime); writeLE<double>(out, cp.p2_lastFlipTime); writeLE<double>(out, cp.p2_lastSpiderFlipTime);
        writeLE<double>(out, cp.p2_gameModeChangedTime); writeLE<double>(out, cp.p2_lastLandTime);
        writeLE<int32_t>(out, cp.p2_lastCollisionBottom); writeLE<int32_t>(out, cp.p2_lastCollisionTop);
        writeLE<int32_t>(out, cp.p2_lastCollisionLeft); writeLE<int32_t>(out, cp.p2_lastCollisionRight);
        writeLE<int32_t>(out, cp.p2_stateOnGround); writeLE<int32_t>(out, cp.p2_stateForce); writeLE<int32_t>(out, cp.p2_reverseRelated);
        writeLE<uint64_t>(out, cp.p2_flags1); writeLE<uint64_t>(out, cp.p2_flags2);
        uint8_t p2Hold = 0;
        if (cp.p2_holdingRight) p2Hold |= 1u;
        if (cp.p2_holdingLeft) p2Hold |= 2u;
        writeLE<uint8_t>(out, p2Hold);
    }
}

static std::vector<PracticeFixCheckpoint> readPracticeFixSection(ReadContext& ctx, std::string* error) {
    std::vector<PracticeFixCheckpoint> checkpoints;
    uint64_t count = readVarint(ctx);
    if (ctx.failed || !checkCountFits(ctx, count, 2, error, "practice-fix checkpoint")) return checkpoints;
    checkpoints.reserve(static_cast<size_t>(count));

    for (uint64_t index = 0; index < count && !ctx.failed; ++index) {
        PracticeFixCheckpoint cp;
        cp.timeSeconds = readLE<double>(ctx);
        cp.priorTimeSeconds = readLE<double>(ctx);
        cp.player1LatchMask = readLE<uint8_t>(ctx);
        cp.player2LatchMask = readLE<uint8_t>(ctx);
        cp.seedState = readLE<uint64_t>(ctx);
        cp.shakeRandomState = readLE<uint64_t>(ctx);
        cp.teleportRandomState = readLE<uint64_t>(ctx);
        uint64_t brokenCount = readVarint(ctx);
        if (brokenCount > 100000) { if (error) *error = "broken object count too high"; ctx.failed = true; break; }
        cp.brokenObjectUIDs.resize(static_cast<size_t>(brokenCount));
        for (size_t i = 0; i < static_cast<size_t>(brokenCount) && !ctx.failed; ++i) {
            cp.brokenObjectUIDs[i] = static_cast<int32_t>(readZigzag(ctx));
        }
        uint64_t itemPairCount = readVarint(ctx);
        if (itemPairCount > 200000) { if (error) *error = "persistent item count too high"; ctx.failed = true; break; }
        cp.persistentItems.resize(static_cast<size_t>(itemPairCount));
        for (size_t i = 0; i < static_cast<size_t>(itemPairCount) && !ctx.failed; ++i) {
            cp.persistentItems[i].first = static_cast<int32_t>(readZigzag(ctx));
            cp.persistentItems[i].second = static_cast<int32_t>(readZigzag(ctx));
        }
        uint64_t varianceCount = readVarint(ctx);
        if (varianceCount > 2000) { if (error) *error = "variance count too high"; ctx.failed = true; break; }
        cp.variance.resize(static_cast<size_t>(varianceCount));
        for (size_t i = 0; i < static_cast<size_t>(varianceCount) && !ctx.failed; ++i) {
            cp.variance[i].first = static_cast<uint16_t>(readZigzag(ctx));
            cp.variance[i].second = readLE<float>(ctx);
        }
        uint8_t clickMask = readLE<uint8_t>(ctx);
        for (int i = 0; i < 6; ++i) {
            cp.clickReleased[i] = (clickMask & static_cast<uint8_t>(1u << i)) != 0;
        }
        // Full P1 state
        cp.p1_x = readLE<float>(ctx); cp.p1_y = readLE<float>(ctx); cp.p1_rotation = readLE<float>(ctx);
        cp.p1_yVelocity = readLE<double>(ctx); cp.p1_fallSpeed = readLE<double>(ctx);
        cp.p1_platformerXVelocity = readLE<double>(ctx); cp.p1_groundYVelocity = readLE<double>(ctx);
        cp.p1_yVelocityBeforeSlope = readLE<double>(ctx); cp.p1_yVelocityRelated = readLE<double>(ctx);
        cp.p1_scaleXRelated = readLE<double>(ctx); cp.p1_scaleXRelated2 = readLE<double>(ctx); cp.p1_scaleXRelated3 = readLE<double>(ctx);
        cp.p1_scaleXRelated4 = readLE<double>(ctx); cp.p1_scaleXRelated5 = readLE<double>(ctx);
        cp.p1_scaleXRelatedTime = readLE<double>(ctx);
        cp.p1_playerSpeed = readLE<float>(ctx); cp.p1_vehicleSize = readLE<float>(ctx);
        cp.p1_platformerVelocityRelated = readLE<float>(ctx);
        cp.p1_xVelocityRelated = readLE<float>(ctx); cp.p1_xVelocityRelated2 = readLE<float>(ctx);
        cp.p1_yVelocityRelated3 = readLE<float>(ctx); cp.p1_slopeVelocity = readLE<float>(ctx);
        cp.p1_gravity = readLE<double>(ctx); cp.p1_speedMultiplier = readLE<double>(ctx);
        cp.p1_snapDistance = readLE<double>(ctx); cp.p1_accelerationOrSpeed = readLE<double>(ctx);
        cp.p1_yStart = readLE<double>(ctx);
        cp.p1_gravityMod = readLE<float>(ctx); cp.p1_slopeAngle = readLE<float>(ctx); cp.p1_slopeAngleRadians = readLE<float>(ctx);
        cp.p1_slopeRotation = readLE<double>(ctx); cp.p1_currentSlopeYVelocity = readLE<double>(ctx);
        cp.p1_slopeStartTime = readLE<double>(ctx);
        cp.p1_dashX = readLE<double>(ctx); cp.p1_dashY = readLE<double>(ctx); cp.p1_dashAngle = readLE<double>(ctx); cp.p1_dashStartTime = readLE<double>(ctx);
        cp.p1_totalTime = readLE<double>(ctx); cp.p1_lastCheckpointTime = readLE<double>(ctx);
        cp.p1_lastJumpTime = readLE<double>(ctx); cp.p1_lastFlipTime = readLE<double>(ctx); cp.p1_lastSpiderFlipTime = readLE<double>(ctx);
        cp.p1_gameModeChangedTime = readLE<double>(ctx); cp.p1_lastLandTime = readLE<double>(ctx);
        cp.p1_lastCollisionBottom = readLE<int32_t>(ctx); cp.p1_lastCollisionTop = readLE<int32_t>(ctx);
        cp.p1_lastCollisionLeft = readLE<int32_t>(ctx); cp.p1_lastCollisionRight = readLE<int32_t>(ctx);
        cp.p1_stateOnGround = readLE<int32_t>(ctx); cp.p1_stateForce = readLE<int32_t>(ctx); cp.p1_reverseRelated = readLE<int32_t>(ctx);
        cp.p1_flags1 = readLE<uint64_t>(ctx); cp.p1_flags2 = readLE<uint64_t>(ctx);
        uint8_t p1Hold = readLE<uint8_t>(ctx);
        cp.p1_holdingRight = (p1Hold & 1u) != 0;
        cp.p1_holdingLeft = (p1Hold & 2u) != 0;
        // Full P2 state
        cp.p2_x = readLE<float>(ctx); cp.p2_y = readLE<float>(ctx); cp.p2_rotation = readLE<float>(ctx);
        cp.p2_yVelocity = readLE<double>(ctx); cp.p2_fallSpeed = readLE<double>(ctx);
        cp.p2_platformerXVelocity = readLE<double>(ctx); cp.p2_groundYVelocity = readLE<double>(ctx);
        cp.p2_yVelocityBeforeSlope = readLE<double>(ctx); cp.p2_yVelocityRelated = readLE<double>(ctx);
        cp.p2_scaleXRelated = readLE<double>(ctx); cp.p2_scaleXRelated2 = readLE<double>(ctx); cp.p2_scaleXRelated3 = readLE<double>(ctx);
        cp.p2_scaleXRelated4 = readLE<double>(ctx); cp.p2_scaleXRelated5 = readLE<double>(ctx);
        cp.p2_scaleXRelatedTime = readLE<double>(ctx);
        cp.p2_playerSpeed = readLE<float>(ctx); cp.p2_vehicleSize = readLE<float>(ctx);
        cp.p2_platformerVelocityRelated = readLE<float>(ctx);
        cp.p2_xVelocityRelated = readLE<float>(ctx); cp.p2_xVelocityRelated2 = readLE<float>(ctx);
        cp.p2_yVelocityRelated3 = readLE<float>(ctx); cp.p2_slopeVelocity = readLE<float>(ctx);
        cp.p2_gravity = readLE<double>(ctx); cp.p2_speedMultiplier = readLE<double>(ctx);
        cp.p2_snapDistance = readLE<double>(ctx); cp.p2_accelerationOrSpeed = readLE<double>(ctx);
        cp.p2_yStart = readLE<double>(ctx);
        cp.p2_gravityMod = readLE<float>(ctx); cp.p2_slopeAngle = readLE<float>(ctx); cp.p2_slopeAngleRadians = readLE<float>(ctx);
        cp.p2_slopeRotation = readLE<double>(ctx); cp.p2_currentSlopeYVelocity = readLE<double>(ctx);
        cp.p2_slopeStartTime = readLE<double>(ctx);
        cp.p2_dashX = readLE<double>(ctx); cp.p2_dashY = readLE<double>(ctx); cp.p2_dashAngle = readLE<double>(ctx); cp.p2_dashStartTime = readLE<double>(ctx);
        cp.p2_totalTime = readLE<double>(ctx); cp.p2_lastCheckpointTime = readLE<double>(ctx);
        cp.p2_lastJumpTime = readLE<double>(ctx); cp.p2_lastFlipTime = readLE<double>(ctx); cp.p2_lastSpiderFlipTime = readLE<double>(ctx);
        cp.p2_gameModeChangedTime = readLE<double>(ctx); cp.p2_lastLandTime = readLE<double>(ctx);
        cp.p2_lastCollisionBottom = readLE<int32_t>(ctx); cp.p2_lastCollisionTop = readLE<int32_t>(ctx);
        cp.p2_lastCollisionLeft = readLE<int32_t>(ctx); cp.p2_lastCollisionRight = readLE<int32_t>(ctx);
        cp.p2_stateOnGround = readLE<int32_t>(ctx); cp.p2_stateForce = readLE<int32_t>(ctx); cp.p2_reverseRelated = readLE<int32_t>(ctx);
        cp.p2_flags1 = readLE<uint64_t>(ctx); cp.p2_flags2 = readLE<uint64_t>(ctx);
        uint8_t p2Hold = readLE<uint8_t>(ctx);
        cp.p2_holdingRight = (p2Hold & 1u) != 0;
        cp.p2_holdingLeft = (p2Hold & 2u) != 0;

        if (!validTime(cp.timeSeconds)) { if (error) *error = "invalid practice-fix checkpoint time"; ctx.failed = true; break; }
        checkpoints.push_back(cp);
    }
    return checkpoints;
}

static std::vector<Macro::RngBoundary> readRngBoundariesSection(ReadContext& ctx, std::string* error) {
    std::vector<Macro::RngBoundary> boundaries;
    uint64_t count = readVarint(ctx);
    if (count > 1'000'000) { if (error) *error = "rng-boundary count too high"; ctx.failed = true; return {}; }
    boundaries.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count && !ctx.failed; ++i) {
        Macro::RngBoundary boundary;
        boundary.tick = static_cast<int32_t>(readZigzag(ctx));
        boundary.progress = static_cast<int32_t>(readZigzag(ctx));
        boundary.primary = readLE<uint64_t>(ctx);
        boundary.shake = readLE<uint64_t>(ctx);
        boundary.teleport = readLE<uint64_t>(ctx);
        boundaries.push_back(boundary);
    }
    return boundaries;
}

static bool isKnownSection(uint8_t kind) {
    return kind >= static_cast<uint8_t>(SectionKind::Inputs) &&
        kind <= static_cast<uint8_t>(SectionKind::RngBoundaries);
}

static bool sectionFlagPresent(uint32_t flags, uint8_t kind) {
    switch (static_cast<SectionKind>(kind)) {
        case SectionKind::Inputs: return true;
        case SectionKind::TpsEvents: return (flags & FlagHasTpsEvents) != 0;
        case SectionKind::Anchors: return (flags & FlagHasAnchors) != 0;
        case SectionKind::Checkpoints: return (flags & FlagHasCheckpoints) != 0;
        case SectionKind::Persistence: return (flags & FlagHasPersistence) != 0;
        case SectionKind::PerFrame: return (flags & FlagHasPerFrame) != 0;
        case SectionKind::Variance: return (flags & FlagHasVariance) != 0;
        case SectionKind::PracticeFix: return (flags & FlagHasPracticeFix) != 0;
        case SectionKind::RngBoundaries: return (flags & FlagHasRngBoundaries) != 0;
    }
    return false;
}

static std::optional<SectionEntry> findSection(std::vector<SectionEntry> const& entries, SectionKind kind) {
    auto wanted = static_cast<uint8_t>(kind);
    auto it = std::find_if(entries.begin(), entries.end(), [wanted](SectionEntry const& entry) {
        return entry.kind == wanted;
    });
    if (it == entries.end()) {
        return std::nullopt;
    }
    return *it;
}

}

std::vector<uint8_t> serialize(Macro const& macro, WriteOptions options) {
    std::vector<uint8_t> output;
    output.reserve(256 + macro.inputs.size() * 12);

    output.push_back('S');
    output.push_back('K');
    output.push_back('K');
    output.push_back('3');
    writeLE<uint16_t>(output, kWireVersion);
    writeLE<uint16_t>(output, 0);
    uint32_t flags = buildFlags(macro, options);
    writeLE<uint32_t>(output, flags);
    size_t headerLenPosition = output.size();
    writeLE<uint32_t>(output, 0);

    writeLE<uint64_t>(output, macro.sourceFormatId);
    writeLE<uint64_t>(output, macro.gameVersion);
    writeLE<int32_t>(output, macro.levelId);
    writeXorString(output, macro.levelName);
    writeXorString(output, macro.author);
    writeLE<double>(output, macro.framerateHint);
    writeLE<float>(output, macro.startPosX);
    writeLE<float>(output, macro.startPosY);
    writeLE<int64_t>(output, macro.recordTimestamp);
    writeLE<uint32_t>(output, macro.rngSeed);
    writeLE<uint8_t>(output, static_cast<uint8_t>(sanitizeAccuracyMode(static_cast<int>(macro.accuracyMode))));
    writeLE<double>(output, macro.duration);

    if (output.size() > std::numeric_limits<uint32_t>::max()) {
        return {};
    }
    patchU32(output, headerLenPosition, static_cast<uint32_t>(output.size()));

    double baseTps = validTps(macro.framerateHint) ? macro.framerateHint : 240.0;
    std::vector<SectionEntry> entries;
    std::vector<uint8_t> payload;
    appendSection(entries, payload, SectionKind::Inputs, [&](std::vector<uint8_t>& sectionPayload) {
        writeInputsSection(sectionPayload, macro.inputs, baseTps);
    });
    if (!macro.tpsEvents.empty()) {
        appendSection(entries, payload, SectionKind::TpsEvents, [&](std::vector<uint8_t>& sectionPayload) {
            writeTpsEventsSection(sectionPayload, macro.tpsEvents, baseTps);
        });
    }
    if (!macro.anchors.empty()) {
        appendSection(entries, payload, SectionKind::Anchors, [&](std::vector<uint8_t>& sectionPayload) {
            writeAnchorsSection(sectionPayload, macro.anchors, baseTps);
        });
    }
    if (!macro.checkpoints.empty()) {
        appendSection(entries, payload, SectionKind::Checkpoints, [&](std::vector<uint8_t>& sectionPayload) {
            writeCheckpointsSection(sectionPayload, macro.checkpoints, baseTps);
        });
    }
    if (!macro.practiceFixCheckpoints.empty()) {
        appendSection(entries, payload, SectionKind::PracticeFix, [&](std::vector<uint8_t>& sectionPayload) {
            writePracticeFixSection(sectionPayload, macro.practiceFixCheckpoints);
        });
    }
    if (!macro.persistenceAttempts.empty()) {
        appendSection(entries, payload, SectionKind::Persistence, [&](std::vector<uint8_t>& sectionPayload) {
            writePersistenceSection(sectionPayload, macro.persistenceAttempts, baseTps);
        });
    }
    if (!macro.rngBoundaries.empty()) {
        appendSection(entries, payload, SectionKind::RngBoundaries, [&](std::vector<uint8_t>& sectionPayload) {
            writeVarint(sectionPayload, static_cast<uint64_t>(macro.rngBoundaries.size()));
            for (auto const& boundary : macro.rngBoundaries) {
                writeZigzag(sectionPayload, boundary.tick);
                writeZigzag(sectionPayload, boundary.progress);
                writeLE<uint64_t>(sectionPayload, boundary.primary);
                writeLE<uint64_t>(sectionPayload, boundary.shake);
                writeLE<uint64_t>(sectionPayload, boundary.teleport);
            }
        });
    }
    if (!macro.perFrames.empty()) {
        appendSection(entries, payload, SectionKind::PerFrame, [&](std::vector<uint8_t>& sectionPayload) {
            writePerFrameSection(sectionPayload, macro.perFrames, baseTps);
        });
    }
    if (!macro.variance.empty() || macro.setupRngSeed != 0 || !macro.persistentItems.empty()) {
        appendSection(entries, payload, SectionKind::Variance, [&](std::vector<uint8_t>& sectionPayload) {
            writeVarianceSection(sectionPayload, macro.variance, macro.setupRngSeed, macro.persistentItems);
        });
    }

    writeLE<uint16_t>(output, static_cast<uint16_t>(entries.size()));
    for (auto const& entry : entries) {
        writeLE<uint8_t>(output, entry.kind);
        writeLE<uint8_t>(output, 0);
        writeLE<uint8_t>(output, 0);
        writeLE<uint8_t>(output, 0);
        writeLE<uint64_t>(output, entry.offset);
        writeLE<uint64_t>(output, entry.size);
    }
    if (options.compressPayload) {
        std::vector<uint8_t> compressed = skk::codec::compress(payload, skk::codec::Level::Deep);
        if (compressed.empty()) {
            return {};
        }
        output.insert(output.end(), compressed.begin(), compressed.end());
    } else {
        output.insert(output.end(), payload.begin(), payload.end());
    }
    return output;
}

std::optional<Macro> deserialize(std::vector<uint8_t> const& data, std::string* error) {
    auto fail = [&](std::string message) -> std::optional<Macro> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };

    if (data.size() < kHeaderSize) {
        return fail("SKK3 magic/header is missing");
    }
    if (std::memcmp(data.data(), "SKK3", 4) != 0) {
        return fail("invalid SKK3 magic");
    }

    ReadContext ctx { data, 4 };
    uint16_t version = readLE<uint16_t>(ctx);
    uint16_t reserved = readLE<uint16_t>(ctx);
    uint32_t flags = readLE<uint32_t>(ctx);
    uint32_t headerLen = readLE<uint32_t>(ctx);
    if (ctx.failed) return fail("truncated SKK3 header");
    if (version != kWireVersion) return fail("unsupported SKK3 version");
    if (reserved != 0) return fail("nonzero SKK3 reserved header field");
    if ((flags & ~kKnownFlags) != 0) return fail("unknown SKK3 flag bits");
    if (headerLen < kHeaderSize || headerLen > data.size()) return fail("invalid SKK3 header length");
    Macro macro;
    macro.losslessVerified = (flags & FlagLosslessVerified) != 0;
    macro.stepClock = (flags & FlagStepClock) != 0;
    macro.recordedFromStartPos = (flags & FlagFromStartPos) != 0;
    macro.platformerMode = (flags & FlagPlatformer) != 0;
    macro.twoPlayerMode = (flags & FlagTwoPlayer) != 0;
    macro.rngLocked = (flags & FlagRngLocked) != 0;
    macro.macroConverted = (flags & FlagMacroConverted) != 0;

    ReadContext metadataCtx { data, kHeaderSize, headerLen };
    macro.sourceFormatId = readLE<uint64_t>(metadataCtx);
    macro.gameVersion = readLE<uint64_t>(metadataCtx);
    macro.levelId = readLE<int32_t>(metadataCtx);
    macro.levelName = readXorString(metadataCtx);
    macro.author = readXorString(metadataCtx);
    macro.framerateHint = readLE<double>(metadataCtx);
    macro.startPosX = readLE<float>(metadataCtx);
    macro.startPosY = readLE<float>(metadataCtx);
    macro.recordTimestamp = readLE<int64_t>(metadataCtx);
    macro.rngSeed = readLE<uint32_t>(metadataCtx);
    if (metadataCtx.failed) return fail("invalid SKK3 metadata");
    if (!validTps(macro.framerateHint)) return fail("invalid SKK3 framerate hint");
    if (!std::isfinite(macro.startPosX) || !std::isfinite(macro.startPosY)) {
        return fail("invalid SKK3 metadata");
    }
    if (metadataCtx.remaining() >= 1) {
        uint8_t modeByte = readLE<uint8_t>(metadataCtx);
        if (metadataCtx.failed) return fail("invalid SKK3 metadata");
        macro.accuracyMode = sanitizeAccuracyMode(static_cast<int>(modeByte));
    } else {
        macro.accuracyMode = AccuracyMode::Vanilla;
    }
    if (metadataCtx.remaining() >= sizeof(double)) {
        macro.duration = readLE<double>(metadataCtx);
        if (!validTime(macro.duration)) return fail("invalid SKK3 duration");
    } else if (metadataCtx.remaining() != 0) {
        return fail("invalid SKK3 metadata");
    }

    ReadContext tableCtx { data, headerLen };
    uint16_t sectionCount = readLE<uint16_t>(tableCtx);
    if (tableCtx.failed) return fail("truncated SKK3 section table");
    std::vector<SectionEntry> entries;
    entries.reserve(sectionCount);
    bool seenKnown[10] = {false};
    for (uint16_t index = 0; index < sectionCount && !tableCtx.failed; ++index) {
        SectionEntry entry;
        entry.kind = readLE<uint8_t>(tableCtx);
        uint8_t r0 = readLE<uint8_t>(tableCtx);
        uint8_t r1 = readLE<uint8_t>(tableCtx);
        uint8_t r2 = readLE<uint8_t>(tableCtx);
        entry.offset = readLE<uint64_t>(tableCtx);
        entry.size = readLE<uint64_t>(tableCtx);
        if (r0 != 0 || r1 != 0 || r2 != 0) return fail("nonzero SKK3 section reserved bytes");
        if (isKnownSection(entry.kind)) {
            if (seenKnown[entry.kind]) return fail("duplicate SKK3 section");
            seenKnown[entry.kind] = true;
            if (!sectionFlagPresent(flags, entry.kind)) return fail("SKK3 section present without matching flag");
        }
        entries.push_back(entry);
    }
    if (tableCtx.failed) return fail("truncated SKK3 section table");
    if (!seenKnown[static_cast<uint8_t>(SectionKind::Inputs)]) return fail("missing SKK3 inputs section");
    if ((flags & FlagHasTpsEvents) != 0 && !seenKnown[static_cast<uint8_t>(SectionKind::TpsEvents)]) {
        return fail("missing flagged SKK3 TPS events section");
    }
    if ((flags & FlagHasAnchors) != 0 && !seenKnown[static_cast<uint8_t>(SectionKind::Anchors)]) {
        return fail("missing flagged SKK3 anchors section");
    }
    if ((flags & FlagHasCheckpoints) != 0 && !seenKnown[static_cast<uint8_t>(SectionKind::Checkpoints)]) {
        return fail("missing flagged SKK3 checkpoints section");
    }
    if ((flags & FlagHasPersistence) != 0 && !seenKnown[static_cast<uint8_t>(SectionKind::Persistence)]) {
        return fail("missing flagged SKK3 persistence section");
    }
    if ((flags & FlagHasPerFrame) != 0 && !seenKnown[static_cast<uint8_t>(SectionKind::PerFrame)]) {
        return fail("missing flagged SKK3 per-frame section");
    }
    if ((flags & FlagHasVariance) != 0 && !seenKnown[static_cast<uint8_t>(SectionKind::Variance)]) {
        return fail("missing flagged SKK3 variance section");
    }
    if ((flags & FlagHasPracticeFix) != 0 && !seenKnown[static_cast<uint8_t>(SectionKind::PracticeFix)]) {
        return fail("missing flagged SKK3 practice-fix section");
    }
    if ((flags & FlagHasRngBoundaries) != 0 && !seenKnown[static_cast<uint8_t>(SectionKind::RngBoundaries)]) {
        return fail("missing flagged SKK3 rng-boundaries section");
    }

    size_t payloadStart = tableCtx.position;
    size_t payloadSize = data.size() - payloadStart;
    std::vector<uint8_t> decodedPayload;
    std::vector<uint8_t> const* payloadBytes = &data;
    size_t payloadBase = payloadStart;
    if ((flags & FlagCompressed) != 0) {
        uint64_t expectedSize = 0;
        for (auto const& entry : entries) {
            expectedSize = std::max(expectedSize, entry.offset + entry.size);
        }
        if (expectedSize == 0 || expectedSize > kMaxPayloadSize) {
            return fail("invalid compressed SKK3 payload size");
        }
        std::vector<uint8_t> compressed(data.begin() + payloadStart, data.end());
        decodedPayload = skk::codec::decompress(compressed, static_cast<size_t>(expectedSize));
        if (decodedPayload.empty()) {
            return fail("failed to decompress SKK3 payload");
        }
        payloadBytes = &decodedPayload;
        payloadBase = 0;
        payloadSize = decodedPayload.size();
    }
    uint64_t previousEnd = 0;
    for (auto const& entry : entries) {
        if (entry.offset < previousEnd) return fail("overlapping SKK3 section ranges");
        if (entry.size > std::numeric_limits<uint64_t>::max() - entry.offset) return fail("invalid SKK3 section range");
        uint64_t entryEnd = entry.offset + entry.size;
        if (entryEnd > static_cast<uint64_t>(payloadSize)) return fail("SKK3 section range outside payload");
        previousEnd = entryEnd;
    }

    double baseTps = validTps(macro.framerateHint) ? macro.framerateHint : 240.0;
    auto readSection = [&](SectionEntry const& entry) {
        auto start = payloadBase + static_cast<size_t>(entry.offset);
        auto end = start + static_cast<size_t>(entry.size);
        return ReadContext { *payloadBytes, start, end };
    };

    if (auto entry = findSection(entries, SectionKind::Inputs)) {
        auto sectionCtx = readSection(*entry);
        macro.inputs = readInputsSection(sectionCtx, macro.framerateHint, error);
        if (sectionCtx.failed || sectionCtx.position != sectionCtx.end) {
            return fail(error && !error->empty() ? *error : "invalid SKK3 inputs section");
        }
    }
    if (auto entry = findSection(entries, SectionKind::TpsEvents)) {
        auto sectionCtx = readSection(*entry);
        macro.tpsEvents = readTpsEventsSection(sectionCtx, baseTps, error);
        if (sectionCtx.failed || sectionCtx.position != sectionCtx.end) {
            return fail(error && !error->empty() ? *error : "invalid SKK3 TPS events section");
        }
    }
    if (auto entry = findSection(entries, SectionKind::Checkpoints)) {
        auto sectionCtx = readSection(*entry);
        macro.checkpoints = readCheckpointsSection(sectionCtx, baseTps, error);
        if (sectionCtx.failed || sectionCtx.position != sectionCtx.end) {
            return fail(error && !error->empty() ? *error : "invalid SKK3 checkpoints section");
        }
    }
    if (auto entry = findSection(entries, SectionKind::PracticeFix)) {
        auto sectionCtx = readSection(*entry);
        macro.practiceFixCheckpoints = readPracticeFixSection(sectionCtx, error);
        if (sectionCtx.failed || sectionCtx.position != sectionCtx.end) {
            return fail(error && !error->empty() ? *error : "invalid SKK3 practice-fix section");
        }
    }
    if (findSection(entries, SectionKind::Anchors)) {
        auto sectionCtx = readSection(*findSection(entries, SectionKind::Anchors));
        macro.anchors = readAnchorsSection(sectionCtx, baseTps, error);
        if (sectionCtx.failed || sectionCtx.position != sectionCtx.end) {
            return fail(error && !error->empty() ? *error : "invalid SKK3 anchors section");
        }
    }
    if (findSection(entries, SectionKind::Persistence)) {
        auto sectionCtx = readSection(*findSection(entries, SectionKind::Persistence));
        macro.persistenceAttempts = readPersistenceSection(sectionCtx, macro.framerateHint, error);
        if (sectionCtx.failed || sectionCtx.position != sectionCtx.end) {
            return fail(error && !error->empty() ? *error : "invalid SKK3 persistence section");
        }
    }
    if (findSection(entries, SectionKind::PerFrame)) {
        auto sectionCtx = readSection(*findSection(entries, SectionKind::PerFrame));
        macro.perFrames = readPerFrameSection(sectionCtx, baseTps, error);
        if (sectionCtx.failed || sectionCtx.position != sectionCtx.end) {
            return fail(error && !error->empty() ? *error : "invalid SKK3 per-frame section");
        }
    }
    if (findSection(entries, SectionKind::Variance)) {
        auto sectionCtx = readSection(*findSection(entries, SectionKind::Variance));
        macro.variance = readVarianceSection(sectionCtx, error, macro.setupRngSeed, macro.persistentItems);
        if (sectionCtx.failed || sectionCtx.position != sectionCtx.end) {
            return fail(error && !error->empty() ? *error : "invalid SKK3 variance section");
        }
    }
    if (findSection(entries, SectionKind::RngBoundaries)) {
        auto sectionCtx = readSection(*findSection(entries, SectionKind::RngBoundaries));
        macro.rngBoundaries = readRngBoundariesSection(sectionCtx, error);
        if (sectionCtx.failed || sectionCtx.position != sectionCtx.end) {
            return fail(error && !error->empty() ? *error : "invalid SKK3 rng-boundaries section");
        }
    }

    if (error) error->clear();
    return macro;
}

double maxSourceTps(Macro const& macro) {
    double result = validTps(macro.framerateHint) ? macro.framerateHint : 240.0;
    for (auto const& event : macro.tpsEvents) {
        if (validTps(event.tps)) {
            result = std::max(result, event.tps);
        }
    }
    return result;
}

namespace {
    static double bridgeTps(double value) {
        return validTps(value) ? value : 240.0;
    }

    static int32_t materializeTickFromTime(double timeSeconds, double tps) {
        if (!validTime(timeSeconds)) {
            return 0;
        }
        double rawTick = std::floor(timeSeconds * bridgeTps(tps) + 0.000001);
        return static_cast<int32_t>(std::clamp<double>(
            rawTick,
            0.0,
            static_cast<double>(std::numeric_limits<int32_t>::max())
        ));
    }

    static double timeFromTick(int32_t tick, double tps) {
        return static_cast<double>(std::max<int32_t>(0, tick)) / bridgeTps(tps);
    }

    static double timeForInput(SkInput const& input, double tps) {
        if (input.hasAbsoluteTime()) {
            return input.timeSeconds;
        }
        double result = timeFromTick(input.tick, tps);
        if (std::isfinite(input.cbsTimeOffset) && input.cbsTimeOffset >= 0.0) {
            result += input.cbsTimeOffset;
        }
        return result;
    }

    static double timeForAnchor(PlaybackAnchor const& anchor, double tps) {
        return anchor.hasAbsoluteTime() ? anchor.timeSeconds : timeFromTick(anchor.tick, tps);
    }

    static Input fromSkInput(SkInput const& input, double tps) {
        Input result;
        result.timeSeconds = timeForInput(input, tps);
        result.button = static_cast<uint8_t>(std::clamp<int>(input.actionType, 1, 3));
        result.player2 = input.isPlayer2();
        result.pressed = input.isPressed();
        result.swiftPairAnchor = input.swiftPairAnchor;
        return result;
    }

    static SkInput toSkInput(Input const& input, double tps) {
        SkInput result;
        result.tick = materializeTickFromTime(input.timeSeconds, tps);
        result.actionType = static_cast<uint8_t>(std::clamp<int>(input.button, 1, 3));
        result.setPlayer2(input.player2);
        result.setPressed(input.pressed);
        result.timeSeconds = input.timeSeconds;
        result.swiftPairAnchor = input.swiftPairAnchor;

        double safeTps = bridgeTps(tps);
        double rawTick = input.timeSeconds * safeTps;
        double rounded = std::nearbyint(rawTick);
        constexpr double kSnapEpsilon = 1e-6;
        double fraction;
        if (std::abs(rawTick - rounded) < kSnapEpsilon) {
            fraction = 0.0;
        } else {
            double tickFloor = std::floor(rawTick);
            fraction = rawTick - tickFloor;
        }
        if (std::isfinite(fraction) && fraction > 0.0) {
            result.stepOffset = static_cast<float>(std::clamp(fraction, 0.0, 0.999999));
            result.cbsTimeOffset = result.stepOffset / safeTps;
        }
        return result;
    }

    static Anchor fromSkAnchor(PlaybackAnchor const& anchor, double tps) {
        Anchor result;
        result.timeSeconds = timeForAnchor(anchor, tps);
        result.state = anchor;
        return result;
    }

    static PlaybackAnchor toSkAnchor(Anchor const& anchor, double tps) {
        PlaybackAnchor result = anchor.state;
        result.timeSeconds = anchor.timeSeconds;
        result.tick = materializeTickFromTime(anchor.timeSeconds, tps);
        return result;
    }

    static PerFrame fromSkPerFrame(SkPerFrame const& frame, double tps) {
        PerFrame result;
        result.timeSeconds = frame.timeSeconds >= 0.0
            ? frame.timeSeconds
            : static_cast<double>(std::max(0, frame.tick)) / bridgeTps(tps);
        result.p1x = frame.p1x;
        result.p1y = frame.p1y;
        result.p1vy = frame.p1vy;
        result.p1vyb = frame.p1vyb;
        result.p1rot = frame.p1rot;
        result.p1vis = frame.p1vis;
        result.p1RingId = frame.p1RingId;
        result.hasPlayer2 = frame.hasPlayer2;
        result.p2x = frame.p2x;
        result.p2y = frame.p2y;
        result.p2vy = frame.p2vy;
        result.p2vyb = frame.p2vyb;
        result.p2rot = frame.p2rot;
        result.p2vis = frame.p2vis;
        result.p2RingId = frame.p2RingId;
        return result;
    }

    static SkPerFrame toSkPerFrame(PerFrame const& frame, double tps) {
        SkPerFrame result;
        result.tick = materializeTickFromTime(frame.timeSeconds, bridgeTps(tps));
        result.timeSeconds = frame.timeSeconds;
        result.p1x = frame.p1x;
        result.p1y = frame.p1y;
        result.p1vy = frame.p1vy;
        result.p1vyb = frame.p1vyb;
        result.p1rot = frame.p1rot;
        result.p1vis = frame.p1vis;
        result.p1RingId = frame.p1RingId;
        result.hasPlayer2 = frame.hasPlayer2;
        result.p2x = frame.p2x;
        result.p2y = frame.p2y;
        result.p2vy = frame.p2vy;
        result.p2vyb = frame.p2vyb;
        result.p2rot = frame.p2rot;
        result.p2vis = frame.p2vis;
        result.p2RingId = frame.p2RingId;
        return result;
    }

    static Checkpoint fromSkCheckpoint(SkCheckpoint const& checkpoint, double tps) {
        Checkpoint result;
        result.timeSeconds = checkpoint.hasAbsoluteTime()
            ? checkpoint.timeSeconds
            : timeFromTick(checkpoint.tick, tps);
        result.rngState = checkpoint.rngState;
        result.priorTimeSeconds = checkpoint.hasAbsolutePriorTime()
            ? checkpoint.priorTimeSeconds
            : timeFromTick(checkpoint.priorTick, tps);
        return result;
    }

    static SkCheckpoint toSkCheckpoint(Checkpoint const& checkpoint, double tps) {
        SkCheckpoint result;
        result.tick = materializeTickFromTime(checkpoint.timeSeconds, tps);
        result.rngState = checkpoint.rngState;
        result.priorTick = materializeTickFromTime(checkpoint.priorTimeSeconds, tps);
        result.timeSeconds = checkpoint.timeSeconds;
        result.priorTimeSeconds = checkpoint.priorTimeSeconds;
        return result;
    }

    static PracticeFixCheckpoint fromSkPracticeFix(SkPracticeFixCheckpoint const& sk) {
        PracticeFixCheckpoint result;
        result.timeSeconds = sk.timeSeconds;
        result.priorTimeSeconds = sk.priorTimeSeconds;
        result.player1LatchMask = sk.player1LatchMask;
        result.player2LatchMask = sk.player2LatchMask;
        result.seedState = sk.seedState;
        result.shakeRandomState = sk.shakeRandomState;
        result.teleportRandomState = sk.teleportRandomState;
        result.brokenObjectUIDs = sk.brokenObjectUIDs;
        result.persistentItems = sk.persistentItems;
        result.variance = sk.variance;
        for (int i = 0; i < 6; ++i) result.clickReleased[i] = sk.clickReleased[i];
        result.p1_x = sk.p1_x; result.p1_y = sk.p1_y; result.p1_rotation = sk.p1_rotation;
        result.p1_yVelocity = sk.p1_yVelocity; result.p1_fallSpeed = sk.p1_fallSpeed;
        result.p1_platformerXVelocity = sk.p1_platformerXVelocity; result.p1_groundYVelocity = sk.p1_groundYVelocity;
        result.p1_yVelocityBeforeSlope = sk.p1_yVelocityBeforeSlope; result.p1_yVelocityRelated = sk.p1_yVelocityRelated;
        result.p1_scaleXRelated = sk.p1_scaleXRelated; result.p1_scaleXRelated2 = sk.p1_scaleXRelated2; result.p1_scaleXRelated3 = sk.p1_scaleXRelated3;
        result.p1_scaleXRelated4 = sk.p1_scaleXRelated4; result.p1_scaleXRelated5 = sk.p1_scaleXRelated5;
        result.p1_scaleXRelatedTime = sk.p1_scaleXRelatedTime;
        result.p1_playerSpeed = sk.p1_playerSpeed; result.p1_vehicleSize = sk.p1_vehicleSize;
        result.p1_platformerVelocityRelated = sk.p1_platformerVelocityRelated;
        result.p1_xVelocityRelated = sk.p1_xVelocityRelated; result.p1_xVelocityRelated2 = sk.p1_xVelocityRelated2;
        result.p1_yVelocityRelated3 = sk.p1_yVelocityRelated3; result.p1_slopeVelocity = sk.p1_slopeVelocity;
        result.p1_gravity = sk.p1_gravity; result.p1_speedMultiplier = sk.p1_speedMultiplier;
        result.p1_snapDistance = sk.p1_snapDistance; result.p1_accelerationOrSpeed = sk.p1_accelerationOrSpeed;
        result.p1_yStart = sk.p1_yStart;
        result.p1_gravityMod = sk.p1_gravityMod; result.p1_slopeAngle = sk.p1_slopeAngle; result.p1_slopeAngleRadians = sk.p1_slopeAngleRadians;
        result.p1_slopeRotation = sk.p1_slopeRotation; result.p1_currentSlopeYVelocity = sk.p1_currentSlopeYVelocity;
        result.p1_slopeStartTime = sk.p1_slopeStartTime;
        result.p1_dashX = sk.p1_dashX; result.p1_dashY = sk.p1_dashY; result.p1_dashAngle = sk.p1_dashAngle; result.p1_dashStartTime = sk.p1_dashStartTime;
        result.p1_totalTime = sk.p1_totalTime; result.p1_lastCheckpointTime = sk.p1_lastCheckpointTime;
        result.p1_lastJumpTime = sk.p1_lastJumpTime; result.p1_lastFlipTime = sk.p1_lastFlipTime; result.p1_lastSpiderFlipTime = sk.p1_lastSpiderFlipTime;
        result.p1_gameModeChangedTime = sk.p1_gameModeChangedTime; result.p1_lastLandTime = sk.p1_lastLandTime;
        result.p1_lastCollisionBottom = sk.p1_lastCollisionBottom; result.p1_lastCollisionTop = sk.p1_lastCollisionTop;
        result.p1_lastCollisionLeft = sk.p1_lastCollisionLeft; result.p1_lastCollisionRight = sk.p1_lastCollisionRight;
        result.p1_stateOnGround = sk.p1_stateOnGround; result.p1_stateForce = sk.p1_stateForce; result.p1_reverseRelated = sk.p1_reverseRelated;
        result.p1_flags1 = sk.p1_flags1; result.p1_flags2 = sk.p1_flags2;
        result.p1_holdingRight = sk.p1_holdingRight; result.p1_holdingLeft = sk.p1_holdingLeft;
        result.p2_x = sk.p2_x; result.p2_y = sk.p2_y; result.p2_rotation = sk.p2_rotation;
        result.p2_yVelocity = sk.p2_yVelocity; result.p2_fallSpeed = sk.p2_fallSpeed;
        result.p2_platformerXVelocity = sk.p2_platformerXVelocity; result.p2_groundYVelocity = sk.p2_groundYVelocity;
        result.p2_yVelocityBeforeSlope = sk.p2_yVelocityBeforeSlope; result.p2_yVelocityRelated = sk.p2_yVelocityRelated;
        result.p2_scaleXRelated = sk.p2_scaleXRelated; result.p2_scaleXRelated2 = sk.p2_scaleXRelated2; result.p2_scaleXRelated3 = sk.p2_scaleXRelated3;
        result.p2_scaleXRelated4 = sk.p2_scaleXRelated4; result.p2_scaleXRelated5 = sk.p2_scaleXRelated5;
        result.p2_scaleXRelatedTime = sk.p2_scaleXRelatedTime;
        result.p2_playerSpeed = sk.p2_playerSpeed; result.p2_vehicleSize = sk.p2_vehicleSize;
        result.p2_platformerVelocityRelated = sk.p2_platformerVelocityRelated;
        result.p2_xVelocityRelated = sk.p2_xVelocityRelated; result.p2_xVelocityRelated2 = sk.p2_xVelocityRelated2;
        result.p2_yVelocityRelated3 = sk.p2_yVelocityRelated3; result.p2_slopeVelocity = sk.p2_slopeVelocity;
        result.p2_gravity = sk.p2_gravity; result.p2_speedMultiplier = sk.p2_speedMultiplier;
        result.p2_snapDistance = sk.p2_snapDistance; result.p2_accelerationOrSpeed = sk.p2_accelerationOrSpeed;
        result.p2_yStart = sk.p2_yStart;
        result.p2_gravityMod = sk.p2_gravityMod; result.p2_slopeAngle = sk.p2_slopeAngle; result.p2_slopeAngleRadians = sk.p2_slopeAngleRadians;
        result.p2_slopeRotation = sk.p2_slopeRotation; result.p2_currentSlopeYVelocity = sk.p2_currentSlopeYVelocity;
        result.p2_slopeStartTime = sk.p2_slopeStartTime;
        result.p2_dashX = sk.p2_dashX; result.p2_dashY = sk.p2_dashY; result.p2_dashAngle = sk.p2_dashAngle; result.p2_dashStartTime = sk.p2_dashStartTime;
        result.p2_totalTime = sk.p2_totalTime; result.p2_lastCheckpointTime = sk.p2_lastCheckpointTime;
        result.p2_lastJumpTime = sk.p2_lastJumpTime; result.p2_lastFlipTime = sk.p2_lastFlipTime; result.p2_lastSpiderFlipTime = sk.p2_lastSpiderFlipTime;
        result.p2_gameModeChangedTime = sk.p2_gameModeChangedTime; result.p2_lastLandTime = sk.p2_lastLandTime;
        result.p2_lastCollisionBottom = sk.p2_lastCollisionBottom; result.p2_lastCollisionTop = sk.p2_lastCollisionTop;
        result.p2_lastCollisionLeft = sk.p2_lastCollisionLeft; result.p2_lastCollisionRight = sk.p2_lastCollisionRight;
        result.p2_stateOnGround = sk.p2_stateOnGround; result.p2_stateForce = sk.p2_stateForce; result.p2_reverseRelated = sk.p2_reverseRelated;
        result.p2_flags1 = sk.p2_flags1; result.p2_flags2 = sk.p2_flags2;
        result.p2_holdingRight = sk.p2_holdingRight; result.p2_holdingLeft = sk.p2_holdingLeft;
        return result;
    }

    static SkPracticeFixCheckpoint toSkPracticeFix(PracticeFixCheckpoint const& cp, double tps) {
        SkPracticeFixCheckpoint result;
        result.timeSeconds = cp.timeSeconds;
        result.priorTimeSeconds = cp.priorTimeSeconds;
        result.tick = materializeTickFromTime(cp.timeSeconds, tps);
        result.priorTick = materializeTickFromTime(cp.priorTimeSeconds, tps);
        result.player1LatchMask = cp.player1LatchMask;
        result.player2LatchMask = cp.player2LatchMask;
        result.seedState = cp.seedState;
        result.shakeRandomState = cp.shakeRandomState;
        result.teleportRandomState = cp.teleportRandomState;
        result.brokenObjectUIDs = cp.brokenObjectUIDs;
        result.persistentItems = cp.persistentItems;
        result.variance = cp.variance;
        for (int i = 0; i < 6; ++i) result.clickReleased[i] = cp.clickReleased[i];
        result.p1_x = cp.p1_x; result.p1_y = cp.p1_y; result.p1_rotation = cp.p1_rotation;
        result.p1_yVelocity = cp.p1_yVelocity; result.p1_fallSpeed = cp.p1_fallSpeed;
        result.p1_platformerXVelocity = cp.p1_platformerXVelocity; result.p1_groundYVelocity = cp.p1_groundYVelocity;
        result.p1_yVelocityBeforeSlope = cp.p1_yVelocityBeforeSlope; result.p1_yVelocityRelated = cp.p1_yVelocityRelated;
        result.p1_scaleXRelated = cp.p1_scaleXRelated; result.p1_scaleXRelated2 = cp.p1_scaleXRelated2; result.p1_scaleXRelated3 = cp.p1_scaleXRelated3;
        result.p1_scaleXRelated4 = cp.p1_scaleXRelated4; result.p1_scaleXRelated5 = cp.p1_scaleXRelated5;
        result.p1_scaleXRelatedTime = cp.p1_scaleXRelatedTime;
        result.p1_playerSpeed = cp.p1_playerSpeed; result.p1_vehicleSize = cp.p1_vehicleSize;
        result.p1_platformerVelocityRelated = cp.p1_platformerVelocityRelated;
        result.p1_xVelocityRelated = cp.p1_xVelocityRelated; result.p1_xVelocityRelated2 = cp.p1_xVelocityRelated2;
        result.p1_yVelocityRelated3 = cp.p1_yVelocityRelated3; result.p1_slopeVelocity = cp.p1_slopeVelocity;
        result.p1_gravity = cp.p1_gravity; result.p1_speedMultiplier = cp.p1_speedMultiplier;
        result.p1_snapDistance = cp.p1_snapDistance; result.p1_accelerationOrSpeed = cp.p1_accelerationOrSpeed;
        result.p1_yStart = cp.p1_yStart;
        result.p1_gravityMod = cp.p1_gravityMod; result.p1_slopeAngle = cp.p1_slopeAngle; result.p1_slopeAngleRadians = cp.p1_slopeAngleRadians;
        result.p1_slopeRotation = cp.p1_slopeRotation; result.p1_currentSlopeYVelocity = cp.p1_currentSlopeYVelocity;
        result.p1_slopeStartTime = cp.p1_slopeStartTime;
        result.p1_dashX = cp.p1_dashX; result.p1_dashY = cp.p1_dashY; result.p1_dashAngle = cp.p1_dashAngle; result.p1_dashStartTime = cp.p1_dashStartTime;
        result.p1_totalTime = cp.p1_totalTime; result.p1_lastCheckpointTime = cp.p1_lastCheckpointTime;
        result.p1_lastJumpTime = cp.p1_lastJumpTime; result.p1_lastFlipTime = cp.p1_lastFlipTime; result.p1_lastSpiderFlipTime = cp.p1_lastSpiderFlipTime;
        result.p1_gameModeChangedTime = cp.p1_gameModeChangedTime; result.p1_lastLandTime = cp.p1_lastLandTime;
        result.p1_lastCollisionBottom = cp.p1_lastCollisionBottom; result.p1_lastCollisionTop = cp.p1_lastCollisionTop;
        result.p1_lastCollisionLeft = cp.p1_lastCollisionLeft; result.p1_lastCollisionRight = cp.p1_lastCollisionRight;
        result.p1_stateOnGround = cp.p1_stateOnGround; result.p1_stateForce = cp.p1_stateForce; result.p1_reverseRelated = cp.p1_reverseRelated;
        result.p1_flags1 = cp.p1_flags1; result.p1_flags2 = cp.p1_flags2;
        result.p1_holdingRight = cp.p1_holdingRight; result.p1_holdingLeft = cp.p1_holdingLeft;
        result.p2_x = cp.p2_x; result.p2_y = cp.p2_y; result.p2_rotation = cp.p2_rotation;
        result.p2_yVelocity = cp.p2_yVelocity; result.p2_fallSpeed = cp.p2_fallSpeed;
        result.p2_platformerXVelocity = cp.p2_platformerXVelocity; result.p2_groundYVelocity = cp.p2_groundYVelocity;
        result.p2_yVelocityBeforeSlope = cp.p2_yVelocityBeforeSlope; result.p2_yVelocityRelated = cp.p2_yVelocityRelated;
        result.p2_scaleXRelated = cp.p2_scaleXRelated; result.p2_scaleXRelated2 = cp.p2_scaleXRelated2; result.p2_scaleXRelated3 = cp.p2_scaleXRelated3;
        result.p2_scaleXRelated4 = cp.p2_scaleXRelated4; result.p2_scaleXRelated5 = cp.p2_scaleXRelated5;
        result.p2_scaleXRelatedTime = cp.p2_scaleXRelatedTime;
        result.p2_playerSpeed = cp.p2_playerSpeed; result.p2_vehicleSize = cp.p2_vehicleSize;
        result.p2_platformerVelocityRelated = cp.p2_platformerVelocityRelated;
        result.p2_xVelocityRelated = cp.p2_xVelocityRelated; result.p2_xVelocityRelated2 = cp.p2_xVelocityRelated2;
        result.p2_yVelocityRelated3 = cp.p2_yVelocityRelated3; result.p2_slopeVelocity = cp.p2_slopeVelocity;
        result.p2_gravity = cp.p2_gravity; result.p2_speedMultiplier = cp.p2_speedMultiplier;
        result.p2_snapDistance = cp.p2_snapDistance; result.p2_accelerationOrSpeed = cp.p2_accelerationOrSpeed;
        result.p2_yStart = cp.p2_yStart;
        result.p2_gravityMod = cp.p2_gravityMod; result.p2_slopeAngle = cp.p2_slopeAngle; result.p2_slopeAngleRadians = cp.p2_slopeAngleRadians;
        result.p2_slopeRotation = cp.p2_slopeRotation; result.p2_currentSlopeYVelocity = cp.p2_currentSlopeYVelocity;
        result.p2_slopeStartTime = cp.p2_slopeStartTime;
        result.p2_dashX = cp.p2_dashX; result.p2_dashY = cp.p2_dashY; result.p2_dashAngle = cp.p2_dashAngle; result.p2_dashStartTime = cp.p2_dashStartTime;
        result.p2_totalTime = cp.p2_totalTime; result.p2_lastCheckpointTime = cp.p2_lastCheckpointTime;
        result.p2_lastJumpTime = cp.p2_lastJumpTime; result.p2_lastFlipTime = cp.p2_lastFlipTime; result.p2_lastSpiderFlipTime = cp.p2_lastSpiderFlipTime;
        result.p2_gameModeChangedTime = cp.p2_gameModeChangedTime; result.p2_lastLandTime = cp.p2_lastLandTime;
        result.p2_lastCollisionBottom = cp.p2_lastCollisionBottom; result.p2_lastCollisionTop = cp.p2_lastCollisionTop;
        result.p2_lastCollisionLeft = cp.p2_lastCollisionLeft; result.p2_lastCollisionRight = cp.p2_lastCollisionRight;
        result.p2_stateOnGround = cp.p2_stateOnGround; result.p2_stateForce = cp.p2_stateForce; result.p2_reverseRelated = cp.p2_reverseRelated;
        result.p2_flags1 = cp.p2_flags1; result.p2_flags2 = cp.p2_flags2;
        result.p2_holdingRight = cp.p2_holdingRight; result.p2_holdingLeft = cp.p2_holdingLeft;
        return result;
    }

    static Attempt fromSkAttempt(SkAttemptSegment const& attempt, double tps) {
        Attempt result;
        result.deathTimeSeconds = attempt.hasAbsoluteDeathTime()
            ? attempt.deathTimeSeconds
            : timeFromTick(attempt.deathTick, tps);
        result.deathPlayer2 = attempt.deathPlayer2;
        result.inputs.reserve(attempt.inputs.size());
        for (auto const& input : attempt.inputs) {
            result.inputs.push_back(fromSkInput(input, tps));
        }
        result.anchors.reserve(attempt.anchors.size());
        for (auto const& anchor : attempt.anchors) {
            result.anchors.push_back(fromSkAnchor(anchor, tps));
        }
        result.perFrames.reserve(attempt.perFrames.size());
        for (auto const& frame : attempt.perFrames) {
            result.perFrames.push_back(fromSkPerFrame(frame, tps));
        }
        return result;
    }

    static SkAttemptSegment toSkAttempt(Attempt const& attempt, double tps) {
        SkAttemptSegment result;
        result.deathTimeSeconds = attempt.deathTimeSeconds;
        result.deathTick = materializeTickFromTime(attempt.deathTimeSeconds, tps);
        result.deathPlayer2 = attempt.deathPlayer2;
        result.inputs.reserve(attempt.inputs.size());
        for (auto const& input : attempt.inputs) {
            result.inputs.push_back(toSkInput(input, tps));
        }
        result.anchors.reserve(attempt.anchors.size());
        for (auto const& anchor : attempt.anchors) {
            result.anchors.push_back(toSkAnchor(anchor, tps));
        }
        result.perFrames.reserve(attempt.perFrames.size());
        for (auto const& frame : attempt.perFrames) {
            result.perFrames.push_back(toSkPerFrame(frame, tps));
        }
        return result;
    }

    static bool hasExactTimedOffsets(std::vector<SkInput> const& inputs) {
        return std::any_of(inputs.begin(), inputs.end(), [](SkInput const& input) {
            return std::isfinite(input.cbsTimeOffset) && input.cbsTimeOffset >= 0.0;
        });
    }
}

Macro fromSkMacro(SkMacro const& macro) {
    Macro result;
    double tps = bridgeTps(macro.framerate);
    result.sourceFormatId = macro.sourceFormatId;
    result.gameVersion = macro.gameVersion;
    result.levelId = macro.levelId;
    result.name = macro.name;
    result.levelName = macro.levelName;
    result.author = macro.author;
    result.framerateHint = tps;
    result.duration = macro.duration;
    result.startPosX = macro.startPosX;
    result.startPosY = macro.startPosY;
    result.recordTimestamp = macro.recordTimestamp;
    result.rngSeed = macro.rngSeed;
    result.losslessVerified = macro.losslessVerified;
    result.recordedFromStartPos = macro.recordedFromStartPos;
    result.platformerMode = macro.platformerMode;
    result.twoPlayerMode = macro.twoPlayerMode;
    result.rngLocked = macro.rngLocked;
    result.macroConverted = macro.macroConverted;
    result.stepClock = macro.usesStepClock;
    result.accuracyMode = sanitizeAccuracyMode(static_cast<int>(writableAccuracyMode(macro.accuracyMode)));

    result.inputs.reserve(macro.inputs.size());
    for (auto const& input : macro.inputs) {
        result.inputs.push_back(fromSkInput(input, tps));
    }
    result.tpsEvents.reserve(macro.tpsEvents.size());
    for (auto const& event : macro.tpsEvents) {
        result.tpsEvents.push_back({event.timeSeconds, event.tps});
    }
    result.anchors.reserve(macro.anchors.size());
    for (auto const& anchor : macro.anchors) {
        result.anchors.push_back(fromSkAnchor(anchor, tps));
    }
    result.perFrames.reserve(macro.perFrames.size());
    for (auto const& frame : macro.perFrames) {
        result.perFrames.push_back(fromSkPerFrame(frame, tps));
    }
    result.variance.reserve(macro.varianceOverrides.size());
    for (auto const& entry : macro.varianceOverrides) {
        VarianceEntry wire;
        wire.index = entry.index;
        wire.value = entry.value;
        result.variance.push_back(wire);
    }
    result.setupRngSeed = macro.setupRngSeed;
    result.persistentItems = macro.persistentItems;
    result.rngBoundaries.reserve(macro.rngBoundaries.size());
    for (auto const& boundary : macro.rngBoundaries) {
        Macro::RngBoundary wire;
        wire.tick = boundary.tick;
        wire.progress = boundary.progress;
        wire.primary = boundary.primary;
        wire.shake = boundary.shake;
        wire.teleport = boundary.teleport;
        result.rngBoundaries.push_back(wire);
    }
    result.checkpoints.reserve(macro.checkpoints.size());
    for (auto const& checkpoint : macro.checkpoints) {
        result.checkpoints.push_back(fromSkCheckpoint(checkpoint, tps));
    }
    result.practiceFixCheckpoints.reserve(macro.practiceFixCheckpoints.size());
    for (auto const& cp : macro.practiceFixCheckpoints) {
        result.practiceFixCheckpoints.push_back(fromSkPracticeFix(cp));
    }
    result.persistenceAttempts.reserve(macro.persistenceAttempts.size());
    for (auto const& attempt : macro.persistenceAttempts) {
        result.persistenceAttempts.push_back(fromSkAttempt(attempt, tps));
    }
    return result;
}

SkMacro toSkMacro(Macro const& macro) {
    SkMacro result;
    double tps = bridgeTps(macro.framerateHint);
    result.fileFormat = SkFileFormat::SKK3;
    result.sourceFormatId = macro.sourceFormatId;
    result.author = macro.author;
    result.levelName = macro.levelName;
    result.levelId = macro.levelId;
    result.framerate = tps;
    result.duration = macro.duration;
    result.gameVersion = static_cast<uint32_t>(std::min<uint64_t>(
        macro.gameVersion,
        std::numeric_limits<uint32_t>::max()
    ));
    result.startPosX = macro.startPosX;
    result.startPosY = macro.startPosY;
    result.recordTimestamp = macro.recordTimestamp;
    result.rngSeed = macro.rngSeed;
    result.losslessVerified = macro.losslessVerified;
    result.recordedFromStartPos = macro.recordedFromStartPos;
    result.platformerMode = macro.platformerMode;
    result.twoPlayerMode = macro.twoPlayerMode;
    result.rngLocked = macro.rngLocked;
    result.macroConverted = macro.macroConverted;
    result.bestEffort = !macro.losslessVerified;
    result.usesStepClock = macro.stepClock;

    result.inputs.reserve(macro.inputs.size());
    for (auto const& input : macro.inputs) {
        result.inputs.push_back(toSkInput(input, tps));
    }
    result.tpsEvents.reserve(macro.tpsEvents.size());
    for (auto const& event : macro.tpsEvents) {
        result.tpsEvents.push_back({event.timeSeconds, event.tps});
    }
    result.anchors.reserve(macro.anchors.size());
    for (auto const& anchor : macro.anchors) {
        result.anchors.push_back(toSkAnchor(anchor, tps));
    }
    result.perFrames.reserve(macro.perFrames.size());
    for (auto const& frame : macro.perFrames) {
        result.perFrames.push_back(toSkPerFrame(frame, tps));
    }
    result.varianceOverrides.reserve(macro.variance.size());
    for (auto const& entry : macro.variance) {
        SkVarianceEntry runtime;
        runtime.index = entry.index;
        runtime.value = entry.value;
        result.varianceOverrides.push_back(runtime);
    }
    result.setupRngSeed = macro.setupRngSeed;
    result.persistentItems = macro.persistentItems;
    result.rngBoundaries.reserve(macro.rngBoundaries.size());
    for (auto const& boundary : macro.rngBoundaries) {
        SkRngBoundary runtime;
        runtime.tick = boundary.tick;
        runtime.progress = boundary.progress;
        runtime.primary = boundary.primary;
        runtime.shake = boundary.shake;
        runtime.teleport = boundary.teleport;
        result.rngBoundaries.push_back(runtime);
    }
    normalizeSkRngBoundaries(result);
    result.checkpoints.reserve(macro.checkpoints.size());
    for (auto const& checkpoint : macro.checkpoints) {
        result.checkpoints.push_back(toSkCheckpoint(checkpoint, tps));
    }
    result.practiceFixCheckpoints.reserve(macro.practiceFixCheckpoints.size());
    for (auto const& cp : macro.practiceFixCheckpoints) {
        result.practiceFixCheckpoints.push_back(toSkPracticeFix(cp, tps));
    }
    result.persistenceAttempts.reserve(macro.persistenceAttempts.size());
    for (auto const& attempt : macro.persistenceAttempts) {
        result.persistenceAttempts.push_back(toSkAttempt(attempt, tps));
    }

    bool exactTiming = hasExactTimedOffsets(result.inputs);
    if (!exactTiming) {
        exactTiming = std::any_of(result.persistenceAttempts.begin(), result.persistenceAttempts.end(), [](SkAttemptSegment const& attempt) {
            return hasExactTimedOffsets(attempt.inputs);
        });
    }
    result.exactCbsTiming = exactTiming;
    result.accuracyMode = sanitizeAccuracyMode(static_cast<int>(macro.accuracyMode));
    return result;
}

PerFrameSelfTestResult selfTestPerFrameCodec() {
    PerFrameSelfTestResult result;
    std::vector<PerFrame> frames;

    auto push = [&](double time, float x, float y, double vy, double vyb, float rot,
        uint8_t vis, int32_t ring, bool p2, float p2x, float p2y, double p2vy,
        double p2vyb, float p2rot, uint8_t p2vis, int32_t p2ring) {
        PerFrame frame;
        frame.timeSeconds = time;
        frame.p1x = x;
        frame.p1y = y;
        frame.p1vy = vy;
        frame.p1vyb = vyb;
        frame.p1rot = rot;
        frame.p1vis = vis;
        frame.p1RingId = ring;
        frame.hasPlayer2 = p2;
        frame.p2x = p2x;
        frame.p2y = p2y;
        frame.p2vy = p2vy;
        frame.p2vyb = p2vyb;
        frame.p2rot = p2rot;
        frame.p2vis = p2vis;
        frame.p2RingId = p2ring;
        frames.push_back(frame);
    };

    // Synthetic macro: long constant-velocity hold (ship), gravity segment with
    // constant per-frame vy delta, a tap/spike burst with noisy pins, an
    // autoclicker leg with repeating frame deltas, and a player-2 section.
    size_t index = 0;
    constexpr double fps = 240.0;
    auto emit = [&](double x, double y, double vy, double rot, bool legTap) {
        double vyb = vy * 1.1;
        bool p2v = (index % 97) < 20;
        push(static_cast<double>(index) / fps,
            x, y, vy, vyb, rot,
            static_cast<uint8_t>((index >> 2) & 1u), 0,
            p2v, x + 12.5f, y - 3.25f, vy - 0.4, vyb - 0.4, rot + 0.5f,
            static_cast<uint8_t>((index >> 1) & 1u), 0);
        ++index;
    };
    // 1) 2000-frame straight ship hold (constant deltas -> one giant run).
    double vx = 0.39, yHoriz = 100.0, rot = 0.0, vy = -0.55;
    for (int k = 0; k < 2000; ++k) {
        double x = 50.0 + static_cast<double>(k) * vx;
        double y = yHoriz + static_cast<double>(k) * 0.21;
        emit(x, y, vy, rot, false);
    }
    // 2) 500-frame gravity arc (vy per-frame delta constant -> run continues).
    for (int k = 0; k < 500; ++k) {
        vy += 0.009;
        double x = 50.0 + 2000.0 * vx + static_cast<double>(k) * vx * 1.35;
        double y = yHoriz + 2000.0 * 0.21 + static_cast<double>(k) * 2.1 + static_cast<double>(k) * (0.0);
        emit(x, y, vy, rot, false);
    }
    // 3) 300-frame spike burst (noisy, breaks runs every few frames).
    unsigned int seed = 0x5EED;
    for (int k = 0; k < 300; ++k) {
        seed = seed * 1664525u + 1013904223u;
        double jx = (static_cast<double>(seed & 0xFF) - 127.5) / 600.0;
        seed = seed * 1664525u + 1013904223u;
        double jy = (static_cast<double>(seed & 0xFF) - 127.5) / 400.0;
        seed = seed * 1664525u + 1013904223u;
        double jvy = (static_cast<double>(seed & 0xFF) - 127.5) / 90.0;
        vy += 0.009 + jvy * 0.001;
        double x = 50.0 + 2000.0 * vx + 500.0 * vx * 1.35 + static_cast<double>(k) * (vx * 1.5 + jx);
        double y = yHoriz + 2000.0 * 0.21 + 500.0 * 2.1 + static_cast<double>(k) * (2.3 + jy);
        emit(x, y, vy, rot + jvy * 0.002f, (k % 29) < 2);
    }
    // 4) 700-frame autoclicker leg: identical time+xy+vy deltas per cycle.
    double baseX = 50.0 + (2000.0 + 500.0) * vx + 300.0 * vx * 1.5 + 700.0 * vx * 1.2;
    double baseY = 100.0 + 2000.0 * 0.21 + 500.0 * 2.1 + 300.0 * 2.3 + 700.0 * 2.2;
    for (int k = 0; k < 700; ++k) {
        emit(baseX + static_cast<double>(k) * 0.234, baseY + static_cast<double>(k) * 0.117,
            -0.55 + static_cast<double>(k) * 0.004, rot, (k % 4) == 0);
    }
    result.frames = frames.size();

    std::vector<uint8_t> v3;
    writePerFrameSectionV3(v3, frames, 240.0);
    std::vector<uint8_t> v4;
    writePerFrameSection(v4, frames, 240.0);
    result.v3Bytes = v3.size();
    result.v4Bytes = v4.size();

    ReadContext ctx { v4, 0 };
    std::string verifyError;
    std::vector<PerFrame> decoded = readPerFrameSection(ctx, 240.0, &verifyError);
    if (ctx.failed) {
        result.stage = PerFrameSelfTestResult::Stage::ReadFailed;
        result.failDetail = verifyError.empty() ? "read ctx.failed (out of data?)" : verifyError;
        return result;
    }
    if (decoded.size() != frames.size()) {
        result.stage = PerFrameSelfTestResult::Stage::CountMismatch;
        result.expected = static_cast<uint32_t>(frames.size());
        result.decoded = static_cast<uint32_t>(decoded.size());
        return result;
    }

    // Field tolerance = half the fixed-point quantum (rounding bound).
    auto closeEnough = [](double a, double b, double tol) {
        return std::fabs(a - b) <= tol;
    };
    for (size_t i = 0; i < decoded.size(); ++i) {
        PerFrame const& a = frames[i];
        PerFrame const& b = decoded[i];
        if (!closeEnough(a.timeSeconds, b.timeSeconds, 1e-9)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "timeSeconds: a=%.15g b=%.15g diff=%.3e",
                a.timeSeconds, b.timeSeconds, a.timeSeconds - b.timeSeconds);
            result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
            result.failIdx = static_cast<int64_t>(i);
            result.failDetail = buf;
            return result;
        }
        if (a.hasPlayer2 != b.hasPlayer2) {
            result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
            result.failIdx = static_cast<int64_t>(i);
            result.failDetail = "hasPlayer2";
            return result;
        }
        if (a.p1vis != b.p1vis) {
            result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
            result.failIdx = static_cast<int64_t>(i);
            result.failDetail = "p1vis";
            return result;
        }
        if (a.p1RingId != b.p1RingId) {
            result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
            result.failIdx = static_cast<int64_t>(i);
            result.failDetail = "p1RingId";
            return result;
        }
        if (!closeEnough(a.p1x, b.p1x, 0.5 / kPerFrameScaleXy)) {
            result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
            result.failIdx = static_cast<int64_t>(i);
            result.failDetail = "p1x";
            return result;
        }
        if (!closeEnough(a.p1y, b.p1y, 0.5 / kPerFrameScaleXy)) {
            result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
            result.failIdx = static_cast<int64_t>(i);
            result.failDetail = "p1y";
            return result;
        }
        if (!closeEnough(a.p1vy, b.p1vy, 0.5 / kPerFrameScaleVy)) {
            result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
            result.failIdx = static_cast<int64_t>(i);
            result.failDetail = "p1vy";
            return result;
        }
        if (!closeEnough(a.p1vyb, b.p1vyb, 0.5 / kPerFrameScaleVy)) {
            result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
            result.failIdx = static_cast<int64_t>(i);
            result.failDetail = "p1vyb";
            return result;
        }
        if (!closeEnough(a.p1rot, b.p1rot, 0.5 / kPerFrameScaleRot)) {
            result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
            result.failIdx = static_cast<int64_t>(i);
            result.failDetail = "p1rot";
            return result;
        }
        if (a.hasPlayer2) {
            if (b.p2vis != a.p2vis) {
                result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
                result.failIdx = static_cast<int64_t>(i);
                result.failDetail = "p2vis";
                return result;
            }
            if (b.p2RingId != a.p2RingId) {
                result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
                result.failIdx = static_cast<int64_t>(i);
                result.failDetail = "p2RingId";
                return result;
            }
            if (!closeEnough(a.p2x, b.p2x, 0.5 / kPerFrameScaleXy)) {
                result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
                result.failIdx = static_cast<int64_t>(i);
                result.failDetail = "p2x";
                return result;
            }
            if (!closeEnough(a.p2y, b.p2y, 0.5 / kPerFrameScaleXy)) {
                result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
                result.failIdx = static_cast<int64_t>(i);
                result.failDetail = "p2y";
                return result;
            }
            if (!closeEnough(a.p2vy, b.p2vy, 0.5 / kPerFrameScaleVy)) {
                result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
                result.failIdx = static_cast<int64_t>(i);
                result.failDetail = "p2vy";
                return result;
            }
            if (!closeEnough(a.p2vyb, b.p2vyb, 0.5 / kPerFrameScaleVy)) {
                result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
                result.failIdx = static_cast<int64_t>(i);
                result.failDetail = "p2vyb";
                return result;
            }
            if (!closeEnough(a.p2rot, b.p2rot, 0.5 / kPerFrameScaleRot)) {
                result.stage = PerFrameSelfTestResult::Stage::FieldMismatch;
                result.failIdx = static_cast<int64_t>(i);
                result.failDetail = "p2rot";
                return result;
            }
        }
    }

    // Determinism: re-encoding the decoded section must reproduce v4 byte-exact.
    std::vector<uint8_t> v4Again;
    writePerFrameSection(v4Again, decoded, 240.0);
    if (v4Again.size() != v4.size()) {
        result.stage = PerFrameSelfTestResult::Stage::RewriteMismatch;
        result.expected = static_cast<uint32_t>(v4.size());
        result.decoded = static_cast<uint32_t>(v4Again.size());
        result.failDetail = "rewrite byte length";
        return result;
    }
    size_t firstDiff = 0;
    for (; firstDiff < v4.size() && v4[firstDiff] == v4Again[firstDiff]; ++firstDiff) {}
    if (firstDiff != v4.size()) {
        result.stage = PerFrameSelfTestResult::Stage::RewriteMismatch;
        result.failIdx = static_cast<int64_t>(firstDiff);
        result.failDetail = "rewrite byte content";
        return result;
    }

    result.ok = true;
    return result;
}

}
