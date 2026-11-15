#pragma once


#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::re4 {

inline constexpr size_t kMagicSize = 3;
inline constexpr size_t kPhysicRecordSize = 25;
inline constexpr size_t kInputRecordSize = 14;
inline constexpr size_t kHeaderSize = kMagicSize + 4 + 8;
inline constexpr size_t kMaxPhysicFrames = 8'000'000;
inline constexpr size_t kMaxActions = 5'000'000;

struct Action {
    uint64_t frame = 0;
    uint8_t button = 1;
    bool pressed = false;
    bool player2 = false;
};

struct Replay {
    float tps = 240.0f;
    uint64_t physicFrames = 0;
    std::string caution;
    std::vector<Action> actions;
};

[[nodiscard]] bool looksLikeRe4(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}