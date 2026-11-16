#pragma once


#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::xe {

inline constexpr size_t kMaxActions = 5'000'000;

struct Action {
    uint64_t frame = 0;
    bool pressed = false;
    bool player2 = false;
    double subTickFrame = 0.0;
    bool fractional = false;
};

struct Replay {
    double tps = 240.0;
    bool cbf = false;
    bool hasHeader = false;
    std::vector<Action> actions;
};

[[nodiscard]] bool parse(std::string_view text, Replay& out, std::string& outReason);

[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}