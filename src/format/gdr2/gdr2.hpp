#pragma once


#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::gdr2 {

struct Input {
    uint64_t frame = 0;
    uint8_t button = 1;
    bool player2 = false;
    bool down = false;
};

struct Replay {
    int version = 0;
    std::string inputTag;
    std::string author;
    std::string description;
    float duration = 0.0f;
    int gameVersion = 0;
    double framerate = 240.0;
    int seed = 0;
    int coins = 0;
    bool ldm = false;
    bool platformer = false;
    std::string botName;
    int botVersion = 0;
    uint32_t levelId = 0;
    std::string levelName;
    std::vector<uint64_t> deaths;
    std::vector<Input> inputs;

    bool hasInputExtension = false;
};

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}