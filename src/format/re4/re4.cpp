#include "re4.hpp"
#include "utils.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace {

constexpr uint8_t kMagic[3] = {'R', 'E', '4'};

// Measured, not guessed - and none of the values work, which is the point.
//
//   shift  0 -> dies at frame 3899 after 78 clicks
//   shift -1 -> dies at frame 3899 after 78 clicks
//
// The same point, not a nearby one. A real off-by-one moves the death; this did
// not move at all, so the frame scale is not RE4's problem and no shift fixes
// it. That is why this stays a measured constant with no formula behind it -
// see the six-format table in the header for why a formula cannot exist.
//
// The cause is GDH's own playback (replayEngine.cpp:107-118): it pins the
// player's position every frame, unconditionally, and velocity only when
// velocity_fix is on, which it is not by default. A GDH macro is therefore
// self-certifying - it completes whatever its inputs are, because the
// trajectory is handed to it. Its inputs were never required to stand alone.
//
// The recording was clean: 9803 physics frames covering 0..9807 with no x jump
// over 30px, so no deaths or respawns are hiding in there. It was simply
// recorded under correction.
static constexpr int64_t kFrameShift = -1;

std::size_t channelIndex(uint8_t button, bool player2) {
    const int base = (button >= 1 && button <= 3) ? button - 1 : 0;
    return static_cast<std::size_t>(base) + (player2 ? 3u : 0u);
}

}  // namespace

bool skk::re4::looksLikeRe4(std::span<const uint8_t> data) {
    return data.size() >= kMagicSize &&
           std::memcmp(data.data(), kMagic, kMagicSize) == 0;
}

bool skk::re4::parse(std::span<const uint8_t> data, skk::re4::Replay& out,
                     std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    // "RE4" is three bytes. A four-byte magic check would consume the low byte
    // of the fps float and shift every subsequent field by one.
    if (!looksLikeRe4(data)) {
        return fail("re4: not an RE4 file (magic must be \"RE4\")");
    }
    if (data.size() < kHeaderSize) {
        return fail("re4: file is too small to hold a header");
    }

    out = Replay{};
    float fps = 0.f;
    std::memcpy(&fps, data.data() + kMagicSize, sizeof(fps));
    out.tps = fps;

    uint64_t physicCount = 0;
    std::memcpy(&physicCount, data.data() + kMagicSize + 4, sizeof(physicCount));
    out.physicFrames = physicCount;

    if (physicCount > kMaxPhysicFrames) {
        return fail("re4: implausible physics frame count " +
                    std::to_string(physicCount));
    }

    // Imported anyway, with a warning, rather than refused. GDH pins the player
    // to the recorded position on every frame of playback
    // (replayEngine.cpp:107-118), so a macro with physics is self-certifying:
    // it completes whatever its inputs are, because the trajectory is handed
    // to the player. Measured on the real file, 0 and -1 both die at frame 3899
    // after 78 clicks - the same point, not a neighbouring one, which is what a
    // genuine off-by-one never does. That is expected, and it does not stop
    // anyone who wants the clicks anyway; it only means "it played" proves
    // nothing about them.
    if (physicCount > 0) {
        // Short, because Geode's notification bubble is small and long text
        // simply gets cut off at the edge of the screen. The full explanation
        // goes to the log right below; the toast only has to stop the user
        // wondering why the macro dies at frame 3899 and blaming the shift.
        out.caution = "Recorded with physics pinning - may not replay correctly.";
        skkbot2::log::warn(
            "[RE4] this macro carries {} physics frame(s): GDH pinned the player to a recorded "
            "trajectory on every frame while playing it, so its clicks were never required to "
            "reproduce the run. They are imported anyway, but the macro may well die partway.",
            physicCount);
    }

    const size_t physicBytes = static_cast<size_t>(physicCount) * kPhysicRecordSize;
    if (kHeaderSize + physicBytes > data.size()) {
        return fail("re4: physics block needs " + std::to_string(physicBytes) +
                    " bytes but only " +
                    std::to_string(data.size() - kHeaderSize) + " remain");
    }
    if (kHeaderSize + physicBytes + 8 > data.size()) {
        return fail("re4: truncated - the physics block runs past the end");
    }

    // Physics is deliberately skipped, not parsed. GDH pins the player to these
    // coordinates every frame regardless of divergence, so replaying with them
    // would make any input set "work" and prove nothing. SKC plays the inputs.
    size_t pos = kHeaderSize + physicBytes;

    uint64_t inputCount = 0;
    std::memcpy(&inputCount, data.data() + pos, sizeof(inputCount));
    pos += 8;

    if (inputCount > kMaxActions) {
        return fail("re4: implausible input count " + std::to_string(inputCount));
    }
    const size_t inputBytes = static_cast<size_t>(inputCount) * kInputRecordSize;
    if (pos + inputBytes > data.size()) {
        return fail("re4: input block needs " + std::to_string(inputBytes) +
                    " bytes but only " + std::to_string(data.size() - pos) +
                    " remain");
    }

    out.actions.reserve(static_cast<size_t>(inputCount));
    for (uint64_t i = 0; i < inputCount; ++i) {
        uint64_t frame = 0;
        std::memcpy(&frame, data.data() + pos, sizeof(frame));
        pos += 8;
        const uint8_t down = data[pos++];
        int32_t button = 0;
        std::memcpy(&button, data.data() + pos, sizeof(button));
        pos += 4;
        const uint8_t player2 = data[pos++];

        // GD's PlayerButton here, unlike Neverhook's and Xenon's: RE4 stores an
        // explicit int32 button rather than assuming Jump.
        uint8_t btn = (button >= 1 && button <= 3) ? static_cast<uint8_t>(button) : 1;
        out.actions.push_back(Action{frame, btn, down != 0, player2 != 0});
    }

    if (out.actions.empty()) return fail("re4: no inputs in the file");

    skkbot2::log::info(
        "[RE4] tps {:.0f}: discarded {} physics frame(s) ({} KB, {:.0f}% of the "
        "file) and took {} input(s)",
        fps, physicCount, physicBytes / 1024,
        100.0 * physicBytes / static_cast<double>(data.size()), inputCount);

    std::stable_sort(out.actions.begin(), out.actions.end(),
                     [](const Action& a, const Action& b) { return a.frame < b.frame; });
    return true;
}

bool skk::re4::toSkcMacro(const skk::re4::Replay& replay, skc::Macro& out,
                          std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!std::isfinite(replay.tps) || replay.tps <= 1.0f) {
        return fail("re4: unusable tps " + std::to_string(replay.tps));
    }

    out = skc::Macro{};
    out.header.tps = replay.tps;
    out.actions.reserve(replay.actions.size());

    std::array<bool, 6> held{};
    std::size_t droppedRelease = 0, droppedDuplicate = 0;
    uint64_t firstKeptRaw = 0;
    bool haveFirstKept = false;
    for (const auto& a : replay.actions) {
        const std::size_t ch = channelIndex(a.button, a.player2);
        if (!a.pressed) {
            if (!held[ch]) {
                ++droppedRelease;
                continue;
            }
            held[ch] = false;
        } else {
            if (held[ch]) {
                ++droppedDuplicate;
                continue;
            }
            held[ch] = true;
        }
        if (!haveFirstKept) {
            firstKeptRaw = a.frame;
            haveFirstKept = true;
        }
        if (a.frame > 0xFFFFFFFFull) {
            return fail("re4: frame " + std::to_string(a.frame) +
                        " exceeds SKC's 32-bit frame counter");
        }
        const int64_t shifted = static_cast<int64_t>(a.frame) + kFrameShift;
        if (shifted < 0 || shifted > 0xFFFFFFFFll) {
            return fail("re4: frame " + std::to_string(a.frame) + " shifted by " +
                        std::to_string(kFrameShift) + " is out of range");
        }
        out.actions.push_back(
            skc::Action{static_cast<uint32_t>(shifted), a.button, a.pressed, a.player2});
    }

    if (out.actions.empty()) return fail("re4: no usable inputs after normalisation");

    std::string reason;
    if (!skc::validateActions(out.actions, reason)) {
        return fail("re4: " + reason);
    }

    skkbot2::log::warn(
        "[RE4] {} inputs -> {} actions, dropped {} release(s) with no press and "
        "{} duplicate press(es), frame shift {:+d} (first click {} -> {})",
        replay.actions.size(), out.actions.size(), droppedRelease,
        droppedDuplicate, kFrameShift, firstKeptRaw, out.actions.front().frame);
    return true;
}

bool skk::re4::importFile(const std::filesystem::path& path, skc::Macro& out,
                          skk::re4::Replay* outInfo, std::string& outReason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outReason = "re4: cannot open " + skk::pathToUtf8(path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        outReason = "re4: file is empty";
        return false;
    }

    Replay parsed;
    if (!parse(bytes, parsed, outReason)) return false;
    if (!toSkcMacro(parsed, out, outReason)) return false;
    if (outInfo) *outInfo = parsed;
    return true;
}