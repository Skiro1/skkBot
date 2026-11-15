#pragma once


#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::nhm {

inline constexpr uint32_t kVersion = 1;
inline constexpr size_t kRecordSize = 7;
inline constexpr size_t kMaxActions = 5'000'000;

struct Action {
    uint64_t frame = 0;
    uint8_t button = 1;
    bool pressed = false;
    bool player2 = false;
};

struct Replay {
    std::vector<Action> actions;
};

[[nodiscard]] bool looksLikeNhm(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}