#pragma once

// GBR6 reader - imports macros written by GucciBot and by the other bots that
// adopted its layout.
//
// One format, many names. GucciBot saves .brrr, and .maybach, .romo,
// .grizzley, .redkingdom, .lemonade, .icebrrr, .waka, .youngsta and .knockerz
// are the same container under a different extension - which is why this reader
// keys off the "GBR6" magic rather than off the name, the same way GucciBot's
// own loader does (engine_core.cpp:716). Matching on extension would need a
// branch per alias and would still break on the next one.
//
// Import only, like the other foreign formats: our .skc is lossless input-only
// and measurably tighter.
//
// Layout confirmed against a real Rauchkammer 2 macro: 297 bytes consumed to
// the last one, name decoded, 240 inputs.

#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::gbr {

// Extensions known to carry GBR6. Kept for the library scan only - detection
// itself is magic-based, so an unknown alias still loads.
inline constexpr const char* kExtensions[] = {
    ".brrr", ".maybach", ".romo",    ".grizzley", ".redkingdom",
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

// Sub-tick offsets (anticroom's SCBF). SKC stores whole frames only, so these
// are reported and dropped rather than silently rounded into the tick grid.
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

// The library scan knows only file names, so it needs the alias list as a
// predicate. Import itself never uses this - it reads the magic from the bytes,
// which is how an alias nobody listed still loads.
[[nodiscard]] bool looksLikeGbrByExtension(std::string_view ext);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

// Converts to SKC. No frame shift: the reference macro's first click lands at
// 141, the same frame our own recording of that level uses.
[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::gbr