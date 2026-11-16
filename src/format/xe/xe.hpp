#pragma once

// XE reader - imports macros written by Xenon (peony.xenon).
//
// The only text format in the set. One number-triplet per line, with an
// optional two-number header:
//
//   "240 0"                 tps, cbf flag (both optional in practice)
//   "137 1 1"               frame, isHolding, isPlayer1
//
// Three things that are not visible in the file itself, all taken from the
// reference (src/*/xe.cpp:60-83, replay.hpp:5-9):
//
//   The third field is isPlayer1, NOT isPlayer2 - GD's own handleButton
//   argument, so player2 = !isPlayer1. The same inversion .nhm has.
//
//   There is no button field at all. struct input has none, so every event is
//   a Jump; Xenon is a jump-only bot. kButton is fixed for that reason.
//
//   A two-number line is a HEADER, not a malformed action. Reading it as an
//   action would inject a bogus record at frame 240.
//
// In CBF mode the same three fields are parsed as doubles, giving a sub-tick
// frame. SKC stores whole frames, so those are reported and dropped.
//
// Import only, like every foreign format here.

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
    double subTickFrame = 0.0;  // only meaningful when the file is CBF
    bool fractional = false;
};

struct Replay {
    double tps = 240.0;
    bool cbf = false;
    bool hasHeader = false;
    std::vector<Action> actions;
};

[[nodiscard]] bool parse(std::string_view text, Replay& out, std::string& outReason);

// No frame shift by default. Xenon's first click is at 137 where our own
// working recording of the same level starts at 141 - four frames EARLIER,
// where GDR2 was later and needed -1 and NHM was later still and also needed
// -1. There is no rule here that predicts the right value, so this is a
// measurement, not a guess dressed up as one.
[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::xe