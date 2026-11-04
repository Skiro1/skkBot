#include "nhm.hpp"
#include "utils.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace {

constexpr uint8_t kMagic[4] = {'N', 'H', 'M', '1'};

// Applied to every frame on conversion, not on parse, so parse() stays a
// faithful reading of the file.
//
// -1 is measured, not guessed. With 0 this macro died at frame 4844 after 98
// clicks; GBR6 died at 4887 after 99 and -1 made it pass. Two different
// formats dying within 43 frames and 1 click of each other is a systematic
// offset between how these bots stamp frames and how we do, not two coincidences.
// (SLC needed none and its first click sat at 142 like this one, which is the
// outlier - hence the constant rather than a decision.)
static constexpr int64_t kFrameShift = -1;

}  // namespace

bool skk::nhm::looksLikeNhm(std::span<const uint8_t> data) {
    return data.size() >= 4 && std::memcmp(data.data(), kMagic, 4) == 0;
}

bool skk::nhm::parse(std::span<const uint8_t> data, skk::nhm::Replay& out,
                     std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!looksLikeNhm(data)) {
        return fail("nhm: not an NHM1 file (magic must be \"NHM1\")");
    }
    if (data.size() < 8) return fail("nhm: file is too small to hold a count");

    uint32_t count = 0;
    std::memcpy(&count, data.data() + 4, 4);
    if (count > kMaxActions) {
        return fail("nhm: implausible action count " + std::to_string(count));
    }

    const size_t needed = 8 + static_cast<size_t>(count) * kRecordSize;
    if (needed > data.size()) {
        // Fixed-size records make this exact: a short file is a truncated or
        // corrupt one, never a stream that ended early.
        return fail("nhm: needs " + std::to_string(needed) + " bytes for " +
                    std::to_string(count) + " records but has " +
                    std::to_string(data.size()));
    }
    if (needed < data.size()) {
        return fail("nhm: " + std::to_string(data.size() - needed) +
                    " trailing byte(s) after the last record");
    }

    out = Replay{};
    out.actions.reserve(count);
    size_t pos = 8;
    size_t badButtons = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t frame = 0;
        std::memcpy(&frame, data.data() + pos, 4);
        const uint8_t button = data[pos + 4];
        const uint8_t down = data[pos + 5];
        const uint8_t player1 = data[pos + 6];
        pos += kRecordSize;

        if (button < 1 || button > 3) ++badButtons;

        // player1 is GD's own flag, so the second player is its negation.
        out.actions.push_back(
            skk::nhm::Action{frame, button, down != 0, player1 == 0});
    }

    if (badButtons) {
        return fail("nhm: " + std::to_string(badButtons) +
                    " record(s) carry a button outside 1..3, which GD would "
                    "never emit");
    }

    std::stable_sort(out.actions.begin(), out.actions.end(),
                     [](const Action& a, const Action& b) { return a.frame < b.frame; });
    return true;
}

bool skk::nhm::toSkcMacro(const skk::nhm::Replay& replay, skc::Macro& out,
                          std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    out = skc::Macro{};
    // NHM stores no tick rate. Neverhook records at the game's runtime tick
    // rate, and every macro in the reference corpus is 240.
    out.header.tps = 240.0f;
    out.actions.reserve(replay.actions.size());

    std::size_t droppedRelease = 0, droppedDuplicate = 0;
    for (const auto& a : replay.actions) {
        const int64_t shifted = static_cast<int64_t>(a.frame) + kFrameShift;
        if (shifted < 0 || shifted > 0xFFFFFFFFll) {
            return fail("nhm: frame " + std::to_string(a.frame) + " shifted by " +
                        std::to_string(kFrameShift) + " is out of range");
        }
        out.actions.push_back(
            skc::Action{static_cast<uint32_t>(shifted), a.button, a.pressed, a.player2});
    }

    // Stray releases are dropped here and a collapsed tap is rebuilt as a
    // same-frame up+down (never deleted - see normalizeActionStream()).
    skc::NormalizeReport normalized;
    skc::normalizeActionStream(out.actions, normalized);
    droppedRelease = normalized.droppedRelease;
    droppedDuplicate = normalized.rebuiltTaps;

    if (out.actions.empty()) return fail("nhm: no usable inputs after normalisation");

    // Report the SOURCE frame of the first action that actually survived
    // normalisation. Reporting replay.actions.front() instead would quote the
    // dangling release at frame 1 and compare it against the first kept click
    // at 142, which reads as a 141-frame shift that does not exist.
    const uint64_t firstKeptRaw =
        static_cast<uint64_t>(static_cast<int64_t>(out.actions.front().frame) - kFrameShift);

    std::string reason;
    if (!skc::validateActions(out.actions, reason)) {
        return fail("nhm: " + reason);
    }

    skkbot2::log::warn(
        "[NHM] {} records -> {} actions, dropped {} release(s) with no press and "
        "rebuilt {} collapsed tap(s), frame shift {:+d} (first click {} -> {})",
        replay.actions.size(), out.actions.size(), droppedRelease,
        droppedDuplicate, kFrameShift, firstKeptRaw,
        out.actions.empty() ? 0 : out.actions.front().frame);
    return true;
}

bool skk::nhm::importFile(const std::filesystem::path& path, skc::Macro& out,
                          skk::nhm::Replay* outInfo, std::string& outReason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outReason = "nhm: cannot open " + skk::pathToUtf8(path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        outReason = "nhm: file is empty";
        return false;
    }

    Replay parsed;
    if (!parse(bytes, parsed, outReason)) return false;
    if (!toSkcMacro(parsed, out, outReason)) return false;
    if (outInfo) *outInfo = parsed;
    return true;
}