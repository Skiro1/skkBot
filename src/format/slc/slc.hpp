#pragma once


#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::slc {

enum class Button : uint8_t {
    Swift = 0,
    Jump = 1,
    Left = 2,
    Right = 3,
};

struct Action {
    uint64_t frame = 0;
    Button button = Button::Jump;
    bool holding = false;
    bool player2 = false;
};

enum class SpecialKind { Tps, Restart, RestartFull, Death, Bugpoint };

struct Special {
    SpecialKind kind = SpecialKind::Bugpoint;
    uint64_t frame = 0;
    double tps = 240.0;
    uint64_t seed = 0;
};

struct Metadata {
    double tps = 240.0;
    uint64_t seed = 0;
    uint32_t version = 0;
    uint32_t build = 0;
    uint32_t randomnessAlgorithm = 0;
};

struct Replay {
    Metadata meta;
    std::vector<Action> actions;
    std::vector<Special> specials;
};

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}