#pragma once

// TTR3 reader - imports macros written by ToastyReplay (full version).
//
// Layout from ToastyReplay-main src/format/ttr3_format.cpp:740-958:
//
//   "TTR3"        4 bytes
//   uint16        wire version, must be 1
//   uint16        reserved, must be 0
//   uint32        flags
//   uint32        headerLen, measured from offset 0
//   metadata, [16, headerLen):
//     uint64      sourceFormatId
//     uint64      gameVersion
//     int32       levelId
//     uint16+bytes levelName
//     uint16+bytes author
//     double      framerateHint
//     float       startPosX
//     float       startPosY
//     int64       recordTimestamp
//     uint32      rngSeed
//     [uint8     accuracyMode]
//     [double    duration]
//   section table, [headerLen, ...):
//     uint16      sectionCount
//     per section: uint8 kind, 3 reserved bytes, uint64 offset, uint64 size
//   payload, the rest: one zlib stream when FlagCompressed is set
//
// Sections: 1 Inputs, 2 TpsEvents, 3 Anchors, 4 Checkpoints, 5 Persistence.
// The inputs section is uint64 count followed by 12-byte records: double
// timeSeconds, uint8 button, uint8 flags, uint16 reserved. The input flag bits
// are bit0 player2, bit1 pressed, bit2 swiftPairAnchor.
//
// TWO THINGS WORTH KNOWING BEFORE CHANGING ANYTHING HERE:
//
// 1. TIME IS IN SECONDS, NOT FRAMES. Every input carries a double, so fractional
//    frames are representable and the file does not have to pretend they are
//    not. The real file happens to be integral at 240tps, but a file recorded
//    at a tps that does not divide evenly will not be, and rounding those
//    silently would produce a macro that is off by a frame somewhere with
//    nothing to show for it. They are counted and reported.
//
// 2. THE PAYLOAD IS ONE zlib STREAM AND THE INPUTS LIVE INSIDE IT. There is no
//    reading around the compression - and no zlib to call: geode ships the
//    header for cocos but no library, and Geometry Dash links its own copy.
//    So src/format/zlib_inflate.hpp carries a decoder. It is verified against
//    the real file: 396 488 compressed bytes expand to exactly the 2 150 356
//    bytes the file's own section table claims, with the Adler-32 checked.
//
// The anchors section (2 147 564 of those bytes) is skipped, and the import
// says so loudly rather than quietly. replay_engine.cpp:397-407 gates
// input-only playback on activeTTR->anchors.empty(), so a macro that HAS
// anchors never takes the input-only path: it goes through reconcileAnchors()
// at replay_engine.cpp:567 and pins the player every tick. A macro like that had
// its clicks recorded against a trajectory being supplied to the player, so they
// may well not reproduce the run on their own - measured, they do not.
//
// It is imported anyway, with a warning that says exactly that. Refusing would
// remove the format outright; a warning keeps it usable and tells the truth
// about what a successful run would and would not prove.
//
// Worth contrasting with TTRL, the same bot family, which is also almost
// entirely physics by volume and does work: Lite applies its fixes only while
// seeking (EngineHooks.cpp:219-231), never during ordinary playback. So the
// question is never "does the file hold physics" but "does the writer pin the
// player during ordinary playback".

#include "skc/skc_format.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace skk::ttr3 {

inline constexpr size_t kMagicSize = 4;
inline constexpr size_t kBaseHeaderSize = 16;
inline constexpr uint16_t kWireVersion = 1;

enum HeaderFlags : uint32_t {
    kFlagLosslessVerified = 1u << 0,
    kFlagFromStartPos = 1u << 1,
    kFlagPlatformer = 1u << 2,
    kFlagTwoPlayer = 1u << 3,
    kFlagRngLocked = 1u << 4,
    kFlagHasAnchors = 1u << 5,
    kFlagHasCheckpoints = 1u << 6,
    kFlagHasPersistence = 1u << 7,
    kFlagHasTpsEvents = 1u << 8,
    kFlagMacroConverted = 1u << 9,
    kFlagCompressed = 1u << 10,
};
inline constexpr uint32_t kKnownFlags = kFlagLosslessVerified | kFlagFromStartPos |
                                       kFlagPlatformer | kFlagTwoPlayer | kFlagRngLocked |
                                       kFlagHasAnchors | kFlagHasCheckpoints |
                                       kFlagHasPersistence | kFlagHasTpsEvents |
                                       kFlagMacroConverted | kFlagCompressed;

enum SectionKind : uint8_t {
    kSectionInputs = 1,
    kSectionTpsEvents = 2,
    kSectionAnchors = 3,
    kSectionCheckpoints = 4,
    kSectionPersistence = 5,
};

inline constexpr size_t kInputRecordSize = 12;
inline constexpr size_t kMaxPayloadSize = 256u * 1024u * 1024u;
inline constexpr uint64_t kMaxInputs = 100000000ull;

struct Action {
    uint64_t frame = 0;
    uint8_t button = 1;
    bool pressed = false;
    bool player2 = false;
};

struct Replay {
    float tps = 240.0;
    uint32_t flags = 0;
    std::string levelName;
    std::string author;
    double durationSeconds = 0.0;
    uint64_t anchorBytes = 0;  // read, then discarded
    // Set when the macro carries anchors ToastyReplay pinned the player to
    // during playback. The import still succeeds - the clicks may just not
    // reproduce the run - and this is what the UI shows so the user is not left
    // guessing why it died at frame 698. Deliberately short: Geode's
    // notification bubble is small and longer text is cut off at the screen
    // edge. The full explanation goes to the log. Empty when there is nothing
    // to say, so honest formats stay silent.
    std::string caution;
    std::size_t fractionalFrames = 0;
    std::vector<Action> actions;
};

[[nodiscard]] bool looksLikeTtr3(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out, std::string& outReason);

// ToastyReplay computes its playback tick as levelTime * tps + 1
// (replay_engine.cpp:48-55) - the same expression our computePlaybackTick uses
// in playback_align.hpp - and dispatches when input.tick <= progress. So 0 is
// predicted from the hook rather than fitted, the same call that turned out
// right for TTRL.
[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out, std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::ttr3