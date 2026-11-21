#ifndef _replay_hpp
#define _replay_hpp

#include "core/accuracy_mode.hpp"
#include "core/full_player_checkpoint.hpp"
#include "core/paths.hpp"
#include "utils.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>
#include <gdr/gdr.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

#define MACRO_FORMAT_VER 1.2f

inline AccuracyMode writableAccuracyMode(AccuracyMode mode) {
    return usesTimedAccuracy(mode) ? AccuracyMode::CBS : AccuracyMode::Vanilla;
}

namespace ReplayStorage {
    inline constexpr uintmax_t kMaxReplayFileSize = 64 * 1024 * 1024;

    inline std::filesystem::path getReplayDirectoryPath() {
        return skkbot2::paths::replays();
    }

    inline std::string sanitizeReplayName(std::string name) {
        auto isTrimChar = [](unsigned char ch) {
            return std::isspace(ch) || ch == '.';
        };

        while (!name.empty() && isTrimChar(static_cast<unsigned char>(name.front()))) {
            name.erase(name.begin());
        }
        while (!name.empty() && isTrimChar(static_cast<unsigned char>(name.back()))) {
            name.pop_back();
        }

        size_t write = 0;
        for (size_t read = 0; read < name.size(); ++read) {
            unsigned char ch = static_cast<unsigned char>(name[read]);
            if (ch < 0x20 || ch == 0x7F) {
                continue;
            }
            if (ch == ':' || ch == '*' || ch == '?' || ch == '/' || ch == '\\' ||
                ch == '|' || ch == '<' || ch == '>' || ch == '"') {
                ch = '_';
            }
            name[write++] = static_cast<char>(ch);
        }
        name.resize(write);

        constexpr size_t kMaxNameBytes = 120;
        if (name.size() > kMaxNameBytes) {
            size_t cut = kMaxNameBytes;
            while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0u) == 0x80u) {
                --cut;
            }
            name.resize(cut);
        }

        return name;
    }

    inline bool replayFileExists(
        std::filesystem::path const& directory,
        std::string_view stem,
        std::string_view extension
    ) {
        std::error_code ec;
        if (!std::filesystem::exists(directory, ec) || ec) {
            return false;
        }

        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) {
                continue;
            }

            if (skk::pathToUtf8(it->path().stem()) != stem) {
                continue;
            }
            if (skk::pathToUtf8(it->path().extension()) == extension) {
                return true;
            }
        }

        return false;
    }

    inline std::string makeUniqueReplayNameInDirectory(
        std::filesystem::path const& directory,
        std::string requestedName,
        std::string_view extension
    ) {
        std::error_code ec;
        if (!std::filesystem::exists(directory, ec)) {
            std::filesystem::create_directories(directory, ec);
        }

        std::string baseName = sanitizeReplayName(std::move(requestedName));
        if (baseName.empty()) {
            baseName = "macro";
        }

        if (!replayFileExists(directory, baseName, extension)) {
            return baseName;
        }

        for (int suffix = 1;; ++suffix) {
            std::string candidate = baseName + "_" + std::to_string(suffix);
            if (!replayFileExists(directory, candidate, extension)) {
                return candidate;
            }
        }
    }

    inline std::string makeUniqueReplayName(
        std::string requestedName,
        std::string_view extension
    ) {
        return makeUniqueReplayNameInDirectory(
            getReplayDirectoryPath(), std::move(requestedName), extension);
    }

    inline std::optional<std::vector<uint8_t>> readReplayBytes(std::filesystem::path const& path) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) {
            return std::nullopt;
        }

        auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            skkbot2::log::warn("Could not inspect replay file '{}': {}", skk::pathToUtf8(path), ec.message());
            return std::nullopt;
        }
        if (size > kMaxReplayFileSize) {
            skkbot2::log::warn("Replay file '{}' exceeds the {} byte size limit", skk::pathToUtf8(path), kMaxReplayFileSize);
            return std::nullopt;
        }

        auto maxSize = static_cast<uintmax_t>(std::numeric_limits<size_t>::max());
        auto maxStreamSize = static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max());
        if (size > maxSize || size > maxStreamSize) {
            skkbot2::log::warn("Replay file '{}' is too large for this platform", skk::pathToUtf8(path));
            return std::nullopt;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            skkbot2::log::error("Could not open replay file '{}'", skk::pathToUtf8(path));
            return std::nullopt;
        }

        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!bytes.empty()) {
            auto streamSize = static_cast<std::streamsize>(size);
            input.read(reinterpret_cast<char*>(bytes.data()), streamSize);
            if (!input || input.gcount() != streamSize) {
                skkbot2::log::error("Could not read replay file '{}': read {} of {} bytes",
                    skk::pathToUtf8(path), input.gcount(), streamSize);
                return std::nullopt;
            }
        }

        return bytes;
    }
}

struct PlayerKinematicState {
    cocos2d::CCPoint position = { 0.f, 0.f };
    float rotation = 0.f;
    double verticalVelocity = 0.0;
    double preSlopeVerticalVelocity = 0.0;
    double horizontalVelocity = 0.0;
    double dashX = 0.0;
    double dashY = 0.0;
    double dashAngle = 0.0;
    double dashStartTime = 0.0;
    double slopeStartTime = 0.0;
    float fallSpeed = 0.0f;
    float slopeVelocity = 0.0f;
    cocos2d::CCPoint shipRotation = { 0.f, 0.f };
    cocos2d::CCPoint lastPortalPosition = { 0.f, 0.f };
    cocos2d::CCPoint stateForceVector = { 0.f, 0.f };
};

struct PlayerFlagState {
    bool upsideDown = false;
    bool holdingLeft = false;
    bool holdingRight = false;
    bool platformer = false;
    bool dead = false;
    bool ship = false;
    bool bird = false;
    bool ball = false;
    bool wave = false;
    bool robot = false;
    bool spider = false;
    bool swing = false;
    bool sideways = false;
    bool dashing = false;
    bool onSlope = false;
    bool wasOnSlope = false;
    bool onGround = false;
    bool goingLeft = false;
    bool platformerMovingRight = false;
    bool slidingRight = false;
    bool accelerating = false;
    bool affectedByForces = false;
    bool jumpBuffered = false;
    bool slopeFlipGravityRelated = false;
    std::array<bool, 4> buttonHolds = { false, false, false, false };
};

struct PlayerEnvironmentState {
    double gravity = 0.0;
    float gravityMod = 0.0f;
    float playerSpeed = 0.0f;
    float playerSpeedAC = 0.0f;
    double speedMultiplier = 0.0;
    float vehicleSize = 0.0f;
    int reverseRelated = 0;
    int stateDartSlide = 0;
    int stateFlipGravity = 0;
    int stateForce = 0;
    double lastFlipTime = 0.0;
    double lastSpiderFlipTime = 0.0;
    bool dualContext = false;
    bool twoPlayerContext = false;
    bool extendedState = false;
};

struct PlayerStateBundle {
    PlayerKinematicState motion;
    PlayerFlagState flags;
    PlayerEnvironmentState environment;
};

struct AnchorRngState {
    uintptr_t fastRandState = 0;
    bool locked = false;
    uint32_t seed = 0;
    uint64_t shakeState = 0;
    uint64_t teleportState = 0;
};

struct PlaybackAnchor {
    int tick = 0;
    double timeSeconds = -1.0;
    bool hasPlayer2 = false;
    PlayerStateBundle player1;
    PlayerStateBundle player2;
    AnchorRngState rng;
    uint8_t player1LatchMask = 0;
    uint8_t player2LatchMask = 0;

    bool hasAbsoluteTime() const {
        return std::isfinite(timeSeconds) && timeSeconds >= 0.0;
    }
};

struct CheckpointStateBundle {
    int tick = 0;
    int priorTick = 0;
    bool hasPlayerState = false;
    PlayerStateBundle player1;
    PlayerStateBundle player2;
    AnchorRngState rng;
    uint8_t player1LatchMask = 0;
    uint8_t player2LatchMask = 0;
    std::vector<std::pair<uint16_t, float>> variance;
    struct TimedAutoclickerPlayerState {
        bool holding = false;
        bool userHolding = false;
        double timeUntilNextEdge = 0.0;
    };
    bool timedAutoclickerActive = false;
    TimedAutoclickerPlayerState timedAutoclicker[2];

    bool hasFullState = false;

    FullPlayerCheckpoint fullPlayer1;
    FullPlayerCheckpoint fullPlayer2;
    bool hasFullPlayer = false;
    bool hasFullPlayer2 = false;
    struct FullP1 {
        float p_x = 0.f, p_y = 0.f;
        float p_rotation = 0.f;
        double yVelocity = 0.0;
        double fallSpeed = 0.0;
        double platformerXVelocity = 0.0;
        double groundYVelocity = 0.0;
        double yVelocityBeforeSlope = 0.0;
        double yVelocityRelated = 0.0;
        double scaleXRelated = 0.0, scaleXRelated2 = 0.0, scaleXRelated3 = 0.0;
        double scaleXRelated4 = 0.0, scaleXRelated5 = 0.0;
        double scaleXRelatedTime = 0.0;
        float playerSpeed = 0.f;
        float vehicleSize = 1.f;
        float platformerVelocityRelated = 0.f;
        float xVelocityRelated = 0.f, xVelocityRelated2 = 0.f;
        float yVelocityRelated3 = 0.f;
        float slopeVelocity = 0.f;
        double gravity = 1.0;
        double speedMultiplier = 1.0;
        double snapDistance = 0.0;
        double accelerationOrSpeed = 0.0;
        double yStart = 0.0;
        float gravityMod = 1.f;
        float slopeAngle = 0.f;
        float slopeAngleRadians = 0.f;
        double slopeRotation = 0.0;
        double currentSlopeYVelocity = 0.0;
        double slopeStartTime = 0.0;
        double dashX = 0.0, dashY = 0.0, dashAngle = 0.0, dashStartTime = 0.0;
        double totalTime = 0.0;
        double lastCheckpointTime = 0.0;
        double lastJumpTime = 0.0;
        double lastFlipTime = 0.0;
        double lastSpiderFlipTime = 0.0;
        double gameModeChangedTime = 0.0;
        double lastLandTime = 0.0;
        int lastCollisionBottom = 0, lastCollisionTop = 0;
        int lastCollisionLeft = 0, lastCollisionRight = 0;
        int stateOnGround = 0;
        int stateForce = 0;
        int stateFlipGravity = 0;
        int reverseRelated = 0;
        uint64_t flags1 = 0;
        uint64_t flags2 = 0;
        bool holdingRight = false;
        bool holdingLeft = false;
    } fullP1;

    struct FullP2 {
        float p_x = 0.f, p_y = 0.f;
        float p_rotation = 0.f;
        double yVelocity = 0.0;
        double fallSpeed = 0.0;
        double platformerXVelocity = 0.0;
        double groundYVelocity = 0.0;
        double yVelocityBeforeSlope = 0.0;
        double yVelocityRelated = 0.0;
        double scaleXRelated = 0.0, scaleXRelated2 = 0.0, scaleXRelated3 = 0.0;
        double scaleXRelated4 = 0.0, scaleXRelated5 = 0.0;
        double scaleXRelatedTime = 0.0;
        float playerSpeed = 0.f;
        float vehicleSize = 1.f;
        float platformerVelocityRelated = 0.f;
        float xVelocityRelated = 0.f, xVelocityRelated2 = 0.f;
        float yVelocityRelated3 = 0.f;
        float slopeVelocity = 0.f;
        double gravity = 1.0;
        double speedMultiplier = 1.0;
        double snapDistance = 0.0;
        double accelerationOrSpeed = 0.0;
        double yStart = 0.0;
        float gravityMod = 1.f;
        float slopeAngle = 0.f;
        float slopeAngleRadians = 0.f;
        double slopeRotation = 0.0;
        double currentSlopeYVelocity = 0.0;
        double slopeStartTime = 0.0;
        double dashX = 0.0, dashY = 0.0, dashAngle = 0.0, dashStartTime = 0.0;
        double totalTime = 0.0;
        double lastCheckpointTime = 0.0;
        double lastJumpTime = 0.0;
        double lastFlipTime = 0.0;
        double lastSpiderFlipTime = 0.0;
        double gameModeChangedTime = 0.0;
        double lastLandTime = 0.0;
        int lastCollisionBottom = 0, lastCollisionTop = 0;
        int lastCollisionLeft = 0, lastCollisionRight = 0;
        int stateOnGround = 0;
        int stateForce = 0;
        int stateFlipGravity = 0;
        int reverseRelated = 0;
        uint64_t flags1 = 0;
        uint64_t flags2 = 0;
        bool holdingRight = false;
        bool holdingLeft = false;
    } fullP2;

    uint64_t seedState = 0;
    uint64_t shakeRandomState = 0;
    uint64_t teleportRandomState = 0;

    std::vector<GameObject*> brokenObjects;
    bool hasWireBrokenObjects = false;
    std::vector<int32_t> wireBrokenObjectKeys;

    std::vector<std::pair<int, int>> persistentItems;

    std::array<bool, 6> clickReleased = { true, true, true, true, true, true };

    GameObject* player1CollisionBlock = nullptr;
    GameObject* player2CollisionBlock = nullptr;
    double extraDelta = 0.0;
    int unk32d0 = 0;
    int unk32ec = 0;
    int currentStep = 0;
    float unk3380 = 0.f;
    skk::GameObjectPhysicsSnapshotMap gameObjectPhysics;

    float timeWarp = 1.0f;
    float queuedTimeWarp = 0.0f;
    float timeWarpRelated = 0.0f;
    float timeModRelated = 0.0f;
    bool timeModRelated2 = false;
    bool gravityRelated = false;
    double attemptTime = 0.0;
    bool hasLayerState = false;
};

struct MacroAction : gdr::Input {
    float stepOffset = 0.0f;
    double timeSeconds = -1.0;
    bool swiftPairAnchor = false;

    MacroAction() = default;

    MacroAction(int tick, int actionType, bool secondPlayer, bool pressed, float offset = 0.0f)
        : Input(tick, actionType, secondPlayer, pressed),
          stepOffset(offset) {}

    bool hasAbsoluteTime() const {
        return std::isfinite(timeSeconds) && timeSeconds >= 0.0;
    }

    gdr::json::object_t saveExtension() const override {
        gdr::json::object_t ext;
        if (stepOffset != 0.0f) {
            ext["accuracy_offset"] = stepOffset;
        }
        if (hasAbsoluteTime()) {
            ext["time_seconds"] = timeSeconds;
        }
        if (swiftPairAnchor) {
            ext["swift_pair_anchor"] = true;
        }
        return ext;
    }
};

class MacroSequence : public gdr::Replay<MacroSequence, MacroAction> {
public:
    std::string name;
    std::string persistedName;
    std::vector<PlaybackAnchor> anchors;
    AccuracyMode accuracyMode = AccuracyMode::Vanilla;
    int savedAnchorInterval = 240;
    float startPosX = 0.f;
    float startPosY = 0.f;
    bool recordedFromStartPos = false;
    bool platformerMode = false;
    bool hasPlatformerModeMetadata = false;
    double displayFps = 0.0;
    bool lockDelta = true;
    bool realTime = false;
    bool physics21 = false;

    MacroSequence()
        : Replay("skkBot", MOD_VERSION) {}

    gdr::json::object_t saveExtension() const override {
        gdr::json::object_t ext;
        AccuracyMode storedAccuracyMode = writableAccuracyMode(accuracyMode);
        bool const writeAnchorData = storedAccuracyMode != AccuracyMode::Vanilla;
        if (storedAccuracyMode != AccuracyMode::Vanilla) {
            ext["accuracy_mode"] = static_cast<int>(storedAccuracyMode);
        }
        if (writeAnchorData) {
            ext["anchor_interval"] = getPersistedAnchorStride();
        }
        ext["platformer_mode"] = platformerMode;
        if (displayFps > 0.0 && std::isfinite(displayFps) &&
            std::abs(displayFps - framerate) > 1e-3) {
            ext["display_fps"] = displayFps;
        }
        ext["timing_lock_delta"] = lockDelta;
        ext["timing_real_time"] = realTime;
        ext["physics_21"] = physics21;

        if (recordedFromStartPos) {
            ext["from_start_pos"] = true;
            ext["start_pos_x"] = startPosX;
            ext["start_pos_y"] = startPosY;
        }

        if (writeAnchorData && !anchors.empty()) {
            auto arr = gdr::json::array();
            for (auto const& anchor : buildPersistedAnchorSlice()) {
                arr.push_back(exportAnchor(anchor));
            }
            ext["anchors"] = arr;
        }
        return ext;
    }

    bool persist(AccuracyMode mode = AccuracyMode::Vanilla, int anchorInterval = 240, bool useJson = false) {
        MacroSequence pending = *this;
        pending.author = GJAccountManager::get()->m_username;
        if (!std::isfinite(pending.framerate) || pending.framerate <= 0.0f) {
            pending.framerate = 240.0f;
        }
        pending.duration = pending.inputs.empty()
            ? 0.0
            : static_cast<double>(pending.inputs.back().frame) / pending.framerate;
        pending.accuracyMode = writableAccuracyMode(mode);
        pending.hasPlatformerModeMetadata = true;
        pending.savedAnchorInterval = std::max(1, anchorInterval);
        if (!usesTimedAccuracy(pending.accuracyMode)) {
            for (auto& input : pending.inputs) {
                input.stepOffset = 0.0f;
                input.timeSeconds = -1.0;
                input.swiftPairAnchor = false;
            }
            pending.anchors.clear();
        }

        auto dir = ReplayStorage::getReplayDirectoryPath();
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) {
            std::filesystem::create_directories(dir, ec);
        }
        if (ec) {
            skkbot2::log::error("Could not prepare replay directory '{}': {}",
                skk::pathToUtf8(dir), ec.message());
            return false;
        }

        std::string const extension = useJson ? ".gdr.json" : ".gdr";
        pending.name = ReplayStorage::makeUniqueReplayName(pending.name, extension);
        pending.persistedName = pending.name;
        auto outputPath = dir / skk::stringToPath(pending.name + extension);
        auto bytes = pending.exportData(useJson);
        if (bytes.empty()) {
            skkbot2::log::error("Could not serialize replay '{}'", skk::pathToUtf8(outputPath));
            return false;
        }
        auto writeResult = utils::file::writeBinarySafe(skk::pathToUtf8(outputPath), bytes);
        if (!writeResult) {
            skkbot2::log::error("Could not write replay file '{}': {}",
                skk::pathToUtf8(outputPath), writeResult.unwrapErr());
            return false;
        }
        *this = std::move(pending);
        skkbot2::log::debug("Saved legacy GDR replay '{}'", skk::pathToUtf8(outputPath));
        return true;
    }

    void truncateAfter(int tick) {
        auto inputIt = std::lower_bound(inputs.begin(), inputs.end(), tick,
            [](MacroAction const& action, int t) { return static_cast<int>(action.frame) < t; });
        inputs.erase(inputIt, inputs.end());

        auto anchorIt = std::lower_bound(anchors.begin(), anchors.end(), tick + 1,
            [](PlaybackAnchor const& anchor, int t) { return anchor.tick < t; });
        anchors.erase(anchorIt, anchors.end());
    }

    void recordAction(int tick, int actionType, bool secondPlayer, bool pressed, float offset = 0.0f) {
        inputs.emplace_back(tick, actionType, secondPlayer, pressed, offset);
    }

    void recordAnchor(PlaybackAnchor anchor) {
        if (!anchors.empty() && anchors.back().tick == anchor.tick) {
            anchors.back() = std::move(anchor);
            return;
        }
        anchors.push_back(std::move(anchor));
    }

private:
    static uint8_t encodeHoldMask(std::array<bool, 4> const& holds) {
        uint8_t mask = 0;
        for (size_t i = 0; i < holds.size(); ++i) {
            if (holds[i]) {
                mask |= static_cast<uint8_t>(1u << i);
            }
        }
        return mask;
    }

    static gdr::json::object_t exportPlayerState(PlayerStateBundle const& state) {
        gdr::json::object_t player;
        player["x"] = state.motion.position.x;
        player["y"] = state.motion.position.y;
        player["r"] = state.motion.rotation;
        player["vy"] = state.motion.verticalVelocity;
        player["svy"] = state.motion.preSlopeVerticalVelocity;
        player["hv"] = state.motion.horizontalVelocity;
        player["g"] = state.environment.gravity;
        player["u"] = state.flags.upsideDown;
        player["hl"] = state.flags.holdingLeft;
        player["hr"] = state.flags.holdingRight;
        player["pf"] = state.flags.platformer;
        player["dead"] = state.flags.dead;
        player["dual"] = state.environment.dualContext;
        player["tp"] = state.environment.twoPlayerContext;
        player["hold"] = encodeHoldMask(state.flags.buttonHolds);
        if (state.environment.extendedState) {
            player["ext"] = true;
            player["dx"] = state.motion.dashX;
            player["dy"] = state.motion.dashY;
            player["da"] = state.motion.dashAngle;
            player["dst"] = state.motion.dashStartTime;
            player["sst"] = state.motion.slopeStartTime;
            player["fs"] = state.motion.fallSpeed;
            player["sv"] = state.motion.slopeVelocity;
            player["srx"] = state.motion.shipRotation.x;
            player["sry"] = state.motion.shipRotation.y;
            player["lpx"] = state.motion.lastPortalPosition.x;
            player["lpy"] = state.motion.lastPortalPosition.y;
            player["fvx"] = state.motion.stateForceVector.x;
            player["fvy"] = state.motion.stateForceVector.y;
            player["gm"] = state.environment.gravityMod;
            player["ps"] = state.environment.playerSpeed;
            player["psac"] = state.environment.playerSpeedAC;
            player["sm"] = state.environment.speedMultiplier;
            player["vs"] = state.environment.vehicleSize;
            player["rev"] = state.environment.reverseRelated;
            player["dart"] = state.environment.stateDartSlide;
            player["flip"] = state.environment.stateFlipGravity;
            player["force"] = state.environment.stateForce;

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
            player["ef"] = flags;
        }
        return player;
    }

    static gdr::json::object_t exportAnchor(PlaybackAnchor const& anchor) {
        gdr::json::object_t obj;
        obj["t"] = anchor.tick;
        obj["has_p2"] = anchor.hasPlayer2;
        obj["p1"] = exportPlayerState(anchor.player1);
        if (anchor.hasPlayer2) {
            obj["p2"] = exportPlayerState(anchor.player2);
        }
        if (anchor.rng.fastRandState != 0) {
            obj["rng"] = static_cast<uint64_t>(anchor.rng.fastRandState);
        }
        if (anchor.rng.locked) {
            obj["rng_locked"] = true;
            obj["rng_seed"] = anchor.rng.seed;
        }
        if (anchor.rng.shakeState != 0) {
            obj["rng_shake"] = anchor.rng.shakeState;
        }
        if (anchor.rng.teleportState != 0) {
            obj["rng_teleport"] = anchor.rng.teleportState;
        }
        if (anchor.player1LatchMask != 0) {
            obj["l1"] = anchor.player1LatchMask;
        }
        if (anchor.player2LatchMask != 0) {
            obj["l2"] = anchor.player2LatchMask;
        }
        return obj;
    }

    int getPersistedAnchorStride() const {
        int stride = savedAnchorInterval > 0
            ? std::max(1, savedAnchorInterval / 10)
            : std::max(1, static_cast<int>(std::round(framerate / 10.0)));
        return stride;
    }

    std::vector<PlaybackAnchor> buildPersistedAnchorSlice() const {
        if (anchors.empty()) {
            return {};
        }

        std::vector<PlaybackAnchor> persisted;
        persisted.reserve(anchors.size() / 8 + 2);

        int stride = getPersistedAnchorStride();
        int lastTick = std::numeric_limits<int>::min();
        for (auto const& anchor : anchors) {
            bool boundary = persisted.empty() || anchor.tick == anchors.back().tick;
            if (!boundary && anchor.tick - lastTick < stride) {
                continue;
            }

            persisted.push_back(anchor);
            lastTick = anchor.tick;
        }

        if (persisted.back().tick != anchors.back().tick) {
            persisted.push_back(anchors.back());
        }

        return persisted;
    }
};

#endif
