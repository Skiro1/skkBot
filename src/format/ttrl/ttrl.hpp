#pragma once


#include "skc/skc_format.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace skk::ttrl {

inline constexpr size_t kMagicSize = 4;
inline constexpr size_t kChecksumSize = 4;
inline constexpr uint8_t kVersion = 1;

inline constexpr uint8_t kFlagFrameFix = 1;
inline constexpr uint8_t kFlagInputChannels = 2;
inline constexpr uint8_t kFlagPlatformer = 4;
inline constexpr uint8_t kFlagSeed = 8;
inline constexpr uint8_t kFlagStartPos = 16;
inline constexpr uint8_t kFlagControlFlip = 32;
inline constexpr uint8_t kKnownFlags = kFlagFrameFix | kFlagInputChannels |
                                       kFlagPlatformer | kFlagSeed |
                                       kFlagStartPos | kFlagControlFlip;

inline constexpr size_t kMaxInputSize = 8u * 1024u * 1024u;
inline constexpr size_t kMaxActions = 1u * 1024u * 1024u;

struct Action {
    uint64_t frame = 0;
    uint8_t button = 1;
    bool pressed = false;
    bool player2 = false;
};

struct Replay {
    float tps = 240.0f;
    uint32_t gameVersion = 0;
    uint64_t levelId = 0;
    uint64_t levelRevision = 0;
    uint64_t levelFingerprint = 0;
    uint64_t tickCount = 0;
    uint8_t flags = 0;
    uint64_t frameFixBytes = 0;
    std::vector<Action> actions;
};

[[nodiscard]] bool looksLikeTtrl(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}