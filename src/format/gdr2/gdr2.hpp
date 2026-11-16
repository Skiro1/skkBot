#pragma once

// GDR2 reader - imports replays produced by other bots using the shared
// GDReplayFormat (maxnut/GDReplayFormat, branch gdr2) into SKC's in-memory
// representation.
//
// Import only. Our own output stays .skc: it is lossless input-only and keeps
// exactly what the run was recorded at. GDR2 is the lingua franca for READING
// other bots' macros, which is exactly the gap.
//
// Field order and encoding were taken from the reference implementation and
// then confirmed against six real files (EclipseMenu and megahack exports of
// Stereo Madness and Rauchkammer 2) with tools/gdr2_probe.cpp. The header
// layout, the big-endian float/double, the signed seed and the input packing
// below are all read off real bytes, not off the README - which is wrong about
// strings (they are varint-length prefixed, not NUL terminated).

#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::gdr2 {

// Mirrors gdr::Input. GDR2 keeps a separate delta chain per player (P1 deltas
// accumulate against the previous P1 input, P2 against the previous P2 one),
// so frames produced here are already absolute.
struct Input {
    uint64_t frame = 0;
    uint8_t button = 1;  // 1 = Jump, 2 = Left, 3 = Right
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

    // True when the producer wrote a per-input extension block. Depends purely
    // on the input tag being non-empty; getting this wrong desynchronises the
    // whole input stream rather than failing one field.
    bool hasInputExtension = false;
};

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

// Converts to SKC. Input and Action carry the same four fields, so this is a
// copy plus normalisation: other bots emit releases with no matching press and
// repeated releases on one channel, which SKC's validator rejects outright.
[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

// Reads and converts in one step.
[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::gdr2