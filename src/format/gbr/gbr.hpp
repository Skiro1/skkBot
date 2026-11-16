#pragma once


#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::gbr {

inline constexpr const char* kExtensions[] = {
    ".gbr", ".brrr", ".maybach", ".romo",    ".grizzley", ".redkingdom",
    ".lemonade", ".icebrrr", ".waka", ".youngsta", ".knockerz",
};

inline constexpr uint32_t kVersion = 1;

struct Action {
    uint64_t frame = 0;
    uint8_t button = 1;
    bool pressed = false;
    bool player2 = false;
    bool fromAutoclicker = false;
};

enum class DeathKind : uint8_t { Unknown = 0 };

struct Death {
    uint64_t frame = 0;
    uint8_t type = 0;
};

struct Subtick {
    uint64_t frame = 0;
    uint8_t button = 1;
    bool pressed = true;
    bool player2 = false;
    double offset = 0.0;
};

struct Replay {
    uint32_t version = 1;
    uint8_t flags = 0;
    float tps = 240.0f;
    int64_t timestamp = 0;
    uint64_t rngSeed = 0;
    std::string name;
    std::string levelName;
    std::vector<Action> actions;
    std::vector<Death> deaths;
    std::vector<Subtick> subticks;

    bool twoPlayer() const { return (flags & 0x01) != 0; }
    bool fromStartPos() const { return (flags & 0x02) != 0; }
    bool rngLocked() const { return (flags & 0x04) != 0; }
    bool platformer() const { return (flags & 0x08) != 0; }
};

[[nodiscard]] bool looksLikeGbr(std::span<const uint8_t> data);

[[nodiscard]] bool looksLikeGbrByExtension(std::string_view ext);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}