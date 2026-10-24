#pragma once

// Astral Lite (.ast) reader - imports replays written by the Astral bot into
// SKC's in-memory representation.
//
// Import only, like every other foreign format we read. Our output stays .skc.
//
// The container was reverse engineered from three published macros plus two
// supplied ones, and every claim below was checked against the bytes rather
// than assumed. See docs/Astral AST format.md for the evidence.
//
// Layout confirmed against five real files, all consuming their payload
// byte-exactly with a final loop-block count of zero:
//
//   0x00  4    magic "AST9"
//   0x04  1    0x00
//   0x05  varint + bytes        build identity, "skiro1" in every sample
//          1    0x00
//          4    float32 LE      tps - 240.0 in every sample
//          ...                  payload encoding id, then a zstd frame
//
// The zstd payload is a BIT stream, not a byte stream. Within each byte the bit
// index runs 7 down to 0 (MSB first). A varint is one continuation bit followed
// by seven data bits, continuation first, and the first data bit read becomes
// bit 6 of the value.
//
//   varint N1                     frame count - matches metadata on all five
//   N1 x {
//       1 bit                     1 => delta is 1 and no varint follows
//       varint                    frame delta from the previous event
//       1 bit                     player 2
//       1 bit                     1 => release, 0 => press
//       if press: 2 bits          button
//   }
//   varint N2                     loop-block count
//
// Release records carry no button, because the channel being released is
// implied by which press is still open - so the reader tracks that per player.

#include "skc/skc_format.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace skk::ast {

struct Metadata {
    double tps = 240.0;
    std::string build;      // the identity string, "skiro1" in every sample
    uint32_t loopBlocks = 0;
};

struct Replay {
    Metadata meta;
    std::vector<skc::Action> actions;
    // Always empty. Kept so the loader can treat every format the same way.
    std::string caution;
};

[[nodiscard]] bool looksLikeAst(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::ast