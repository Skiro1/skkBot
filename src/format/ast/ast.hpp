#pragma once


#include "skc/skc_format.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace skk::ast {

struct Metadata {
    double tps = 240.0;
    std::string build;
    uint32_t loopBlocks = 0;
};

struct Replay {
    Metadata meta;
    std::vector<skc::Action> actions;
    std::string caution;
};

[[nodiscard]] bool looksLikeAst(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}