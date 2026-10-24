#pragma once

// SLC v3 reader - imports replays written by Silicate (peony.silicate) into
// SKC's in-memory representation.
//
// v3 only, on purpose. v2 exists in the same repository (formats/v2.hpp) and a
// future v4 would need its own reader; accepting "anything that starts with
// SLC" would turn a wrong-version file into a silent misparse.
//
// Import only. Our output stays .skc - lossless input-only, and measurably
// smaller: on the same clicks SKC is ~1.6x tighter than GDR2 (0.89 B/click
// against 1.41 B/event), which is why .slc is read and not written.
//
// Layout confirmed against a real Rauchkammer 2 recording: 676 bytes consumed
// to the last one, footer 0xCC, 226 actions across 79 sections.

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

// A decoded player event. SLC stores frames as deltas against the previous
// event, so these come out absolute.
struct Action {
    uint64_t frame = 0;
    Button button = Button::Jump;
    bool holding = false;
    bool player2 = false;
};

// Non-player events SLC can carry. SKC cannot express any of them: a restart
// needs a death chain it does not have, and a TPS change needs an event stream
// it does not store. They are surfaced rather than dropped, because dropping a
// Death would silently produce a macro that dies where the original did not.
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

// Converts to SKC. No frame shift: SLC's first click on the reference level
// lands at 142 against our own macro's 141, close enough that an offset would
// be a guess rather than a measurement. (GDR2 needed -1 empirically; that was
// measured, this was not.)
[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::slc