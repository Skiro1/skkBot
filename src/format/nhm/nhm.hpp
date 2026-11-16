#pragma once

// NHM reader - imports macros written by Neverhook (nh::MacroEngine).
//
// The simplest format in the set: a 4-byte magic, a count, and fixed 7-byte
// records. No compression, no delta coding, no sections.
//
// Two things here are the opposite of what the names suggest, and both were
// taken from the reference rather than guessed:
//
//   The flag is player1, not player2. It is GD's own handleButton argument
//   passed straight through (Macro.cpp:27-31), so player2 = !player1. The
//   same inversion bit the P2 button fix had to deal with.
//
//   button is GD's PlayerButton, 1 Jump / 2 Left / 3 Right - recordInput
//   rejects anything outside 1..3 (MacroEngine.cpp:54). It is NOT a 0-based
//   index, so it maps onto skc::Action::button unchanged.
//
// Import only, like every foreign format here: our .skc is lossless and
// measurably tighter.
//
// Layout confirmed against a real Rauchkammer 2 recording: 1737 bytes
// consumed to the last one, 247 records, 8 + 247*7 exactly.

#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::nhm {

inline constexpr uint32_t kVersion = 1;
inline constexpr size_t kRecordSize = 7;
inline constexpr size_t kMaxActions = 5'000'000;  // the reference's own ceiling

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

// No frame shift by default: the reference macro's first real click is at 142,
// which is where SLC's sits and where SLC needed none. GBR6 sat at 141 and
// needed -1, so this one is a guess until a run says otherwise - it is one
// constant.
[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::nhm