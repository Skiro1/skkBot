#pragma once

// RE4 reader - imports macros written by GDH (Replay Engine v4).
//
// Layout from GDH-main replayEngine.cpp:209-245 and replayEngine.hpp:6-20:
//
//   "RE4"        3 bytes  (THREE, not four - the fourth byte is the first
//                         byte of the fps float, and reading a 4-byte magic
//                         shifts the whole file by one)
//   float        fps
//   uint64       physicSize, then physicSize records of 25 bytes:
//                   uint64 frame, float x, float y, double y_accel, uint8 p2
//   uint64       inputSize, then inputSize records of 14 bytes:
//                   uint64 frame, uint8 down, int32 button, uint8 p2
//
// Verified on the real file: 15 + 9803*25 + 8 + 237*14 = 248416, the exact
// file size.
//
// THE PHYSICS BLOCK IS SKIPPED, AND ITS PRESENCE IS REPORTED LOUDLY. GDH pins
// the player's position every frame during playback (replayEngine.cpp:107-118),
// so its macros are self-certifying: they complete whatever their inputs are,
// because the trajectory is handed to the player. Their inputs were never
// required to stand on their own. Measured on the real file: 0 dies at frame 3899
// after 78 clicks, -1 dies at frame 3899 after 78 clicks - the same point, not a
// neighbouring one, which is what a real off-by-one never does.
//
// So it is imported anyway, on -1, with a warning that says why a run may not
// finish. Refusing would remove the format; a warning keeps it usable and makes
// clear that "it played" would prove nothing about the clicks.
//
// The recording itself was clean: 9803 physics frames covering 0..9807 with no x
// jump over 30px, so there are no deaths or respawns hiding in there. It was
// simply recorded under correction.
//
// Contrast TTRL, which is also mostly physics by volume and DOES work: Lite
// applies its fixes only while seeking (EngineHooks.cpp:219-231), never during
// ordinary playback. So the question is not "does the file hold physics" but
// "does the writer pin the player during ordinary playback".
//
// The trajectory is still worth keeping in mind: it is the only ground-truth
// path in the corpus, x and y for every one of 9803 frames, and it is what a
// frame-by-frame comparison tool would check against - including to verify that
// the -1s fitted by trial on GDR2, GBR6 and NHM are actually right.
//
// The table is still worth keeping in mind: it is the only ground-truth
// trajectory in the corpus, x and y for every one of 9803 frames.

#include "skc/skc_format.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::re4 {

inline constexpr size_t kMagicSize = 3;
inline constexpr size_t kPhysicRecordSize = 25;
inline constexpr size_t kInputRecordSize = 14;
inline constexpr size_t kHeaderSize = kMagicSize + 4 + 8;  // 15
inline constexpr size_t kMaxPhysicFrames = 8'000'000;
inline constexpr size_t kMaxActions = 5'000'000;

struct Action {
    uint64_t frame = 0;
    uint8_t button = 1;
    bool pressed = false;
    bool player2 = false;
};

struct Replay {
    float tps = 240.0f;
    uint64_t physicFrames = 0;  // read, then discarded
    // Set when the macro carries physics the writer pinned the player to during
    // playback. The import still succeeds - the clicks may just not reproduce
    // the run - and this is what the UI shows the user so they are not left
    // guessing why it died at frame 3899. Deliberately short: Geode's
    // notification bubble is small and longer text is cut off at the screen
    // edge. The full explanation goes to the log. Empty when there is nothing
    // to say, so honest formats stay silent.
    std::string caution;
    std::vector<Action> actions;
};

[[nodiscard]] bool looksLikeRe4(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

// No frame shift: RE4's first click is at 137, the same frame XE's is at, and
// XE replays on 0.
[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::re4