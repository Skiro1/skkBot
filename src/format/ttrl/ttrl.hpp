#pragma once

// TTRL reader - imports macros written by ToastyReplay (ToastyReplay-Lite).
//
// Layout from ToastyReplay-Lite-main src/replay/TtrlCodec.cpp:412-586:
//
//   [payload, terminated by a CRC32 of everything before it]
//   "TTRL"                      4 bytes
//   uint8                       version, must be 1
//   uint8                       flags (see kFlag*)
//   varint                      tps numerator, varint tps denominator
//   varint                      gameVersion
//   varint                      levelId
//   varint                      levelRevision
//   uint64                      levelFingerprint
//   varint                      tickCount
//   [seed]        varint       if kFlagSeed
//   [startPos]    float32      if kFlagStartPos
//   [controlFlip] uint8        if kFlagControlFlip
//   varint                      inputSize
//   inputSize bytes             delta-encoded input events
//   [frameFixes]   uint8 schema, varint size, that many bytes - if kFlagFrameFix
//   uint32                      CRC32 of the payload (little-endian)
//
// Inputs are the reason this format is interesting: each event is one varint
// holding BOTH a tick delta and the button state in its low bits, so a run
// costs about one byte per click. The real file is 345 336 bytes, of which 345
// 026 are frame fixes and 275 are the inputs - a 99.9% physics / 0.1% input
// split, even more lopsided than RE4.
//
// THE FRAME FIXES ARE DELIBERATELY DISCARDED. ToastyReplay applies them on
// playback (EngineHooks.cpp:299 realignPlayback, and the schema-3 record
// carries position, rotation, vertical velocity, the full player-state bitmask,
// vehicle size, speed and gravity), so its macros are corrected trajectories
// rather than honest click recordings - the same situation as GDH's RE4. We
// keep the 234 inputs and let SKC play them.
//
// The header is a varint LEB128, not a fixed-width integer, so every field
// shifts together if one is misread. The CRC is the check that catches that:
// it is verified over the whole payload before anything is parsed.

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
    uint8_t button = 1;  // 1 = Jump, 2 = Left, 3 = Right
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
    uint64_t frameFixBytes = 0;  // read, then discarded
    std::vector<Action> actions;
};

[[nodiscard]] bool looksLikeTtrl(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

// ToastyReplay dispatches inputs before the base processCommands and advances
// its tick afterwards (EngineHooks.cpp:291-304), which is the same shape as our
// own clock and as Silicate's - the opposite of GDH's drain-then-queue. So 0 is
// the shape-based prediction, not a fitted value.
[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::ttrl