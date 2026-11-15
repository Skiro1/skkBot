#pragma once


#include "skc/skc_format.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace skk::echo {

inline constexpr size_t kMagicSize = 6;
inline constexpr uint8_t kMinVersion = 3;
inline constexpr uint8_t kMaxVersion = 4;

inline constexpr uint8_t kFlagSamples = 1;
inline constexpr uint8_t kFlagWholeFps = 2;
inline constexpr uint8_t kFlagSeed = 4;
inline constexpr uint8_t kFlagUnrounded = 8;
inline constexpr uint8_t kFlagSilicateVariance = 16;
inline constexpr uint8_t kFlagLdmKnown = 32;
inline constexpr uint8_t kFlagLdm = 64;

inline constexpr size_t kMaxActions = 1u * 1024u * 1024u;
inline constexpr uint64_t kMaxRateChanges = 1ull << 16;

struct Rate {
    uint32_t frame = 0;
    float fps = 0.0f;
};

struct Action {
    uint32_t frame = 0;
    uint8_t button = 1;
    bool pressed = false;
    bool player2 = false;
};

struct Replay {
    uint8_t version = 0;
    uint8_t flags = 0;
    float tps = 240.0f;
    float endX = 0.0f;
    double recordingSeconds = 0.0;
    uint64_t attempts = 0;
    bool hasSeed = false;
    uint64_t seed = 0;
    std::vector<Rate> rates;
    std::vector<Action> actions;
    std::string caution;
};

[[nodiscard]] bool looksLikeEcho(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}
