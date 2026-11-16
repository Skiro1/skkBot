#include "xe.hpp"
#include "utils.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

// Xenon has no button field, so every event is a Jump.
constexpr uint8_t kButton = 1;

static constexpr int64_t kFrameShift = 0;

std::vector<std::string> splitNumbers(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : line) {
        if (c == ' ' || c == '\t' || c == '\r' || c == ',') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else if (c != '\n') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool parseUnsigned(const std::string& s, unsigned long long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

bool parseDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

}  // namespace

bool skk::xe::parse(std::string_view text, skk::xe::Replay& out,
                    std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    out = Replay{};
    out.actions.reserve(256);

    std::istringstream stream{std::string(text)};
    std::string line;
    size_t lineNo = 0;
    size_t malformed = 0;
    std::size_t fractional = 0;
    while (std::getline(stream, line)) {
        ++lineNo;
        const auto nums = splitNumbers(line);
        if (nums.empty()) continue;

        if (nums.size() == 2) {
            if (out.hasHeader) {
                ++malformed;
                continue;
            }
            // A two-number line is the header, NOT an action. Reading it as an
            // action injects a bogus record at the tps value.
            unsigned long long t = 0;
            unsigned long long c = 0;
            if (!parseUnsigned(nums[0], t) || !parseUnsigned(nums[1], c)) {
                ++malformed;
                continue;
            }
            out.hasHeader = true;
            out.tps = static_cast<double>(t);
            out.cbf = c != 0;
            continue;
        }

        if (nums.size() != 3) {
            ++malformed;
            continue;
        }
        if (out.actions.size() >= kMaxActions) {
            return fail("nhm/xe: action count exceeded the ceiling");
        }

        if (out.cbf) {
            double frame = 0.0;
            double holding = 0.0;
            double p1 = 0.0;
            if (!parseDouble(nums[0], frame) || !parseDouble(nums[1], holding) ||
                !parseDouble(nums[2], p1)) {
                ++malformed;
                continue;
            }
            if (frame < 0.0) {
                ++malformed;
                continue;
            }
            const bool isFractional =
                frame != std::floor(frame);
            if (isFractional) ++fractional;
            const auto whole = static_cast<uint64_t>(std::floor(frame));
            out.actions.push_back(skk::xe::Action{whole, holding != 0.0,
                                                   p1 == 0.0, frame, isFractional});
            continue;
        }

        unsigned long long frame = 0;
        unsigned long long holding = 0;
        unsigned long long p1 = 0;
        if (!parseUnsigned(nums[0], frame) || !parseUnsigned(nums[1], holding) ||
            !parseUnsigned(nums[2], p1)) {
            ++malformed;
            continue;
        }
        if (frame > 0xFFFFFFFFull) {
            return fail("xe: frame " + std::to_string(frame) +
                        " exceeds SKC's 32-bit frame counter");
        }
        // isPlayer1, not isPlayer2: player2 is its negation.
        out.actions.push_back(skk::xe::Action{frame, holding != 0, p1 == 0, 0.0, false});
    }

    if (out.actions.empty()) {
        return fail("xe: no actions found - is this a .xe macro at all?");
    }
    if (malformed > out.actions.size()) {
        return fail("xe: " + std::to_string(malformed) +
                    " malformed line(s), more than the " +
                    std::to_string(out.actions.size()) + " parsed");
    }
    if (malformed) {
        skkbot2::log::warn("[XE] skipped {} malformed line(s)", malformed);
    }
    if (fractional) {
        skkbot2::log::warn(
            "[XE] file is CBF and carries {} sub-tick frame(s); SKC stores whole "
            "frames, so those inputs land on the tick edge",
            fractional);
    }

    std::stable_sort(out.actions.begin(), out.actions.end(),
                     [](const Action& a, const Action& b) { return a.frame < b.frame; });
    return true;
}

bool skk::xe::toSkcMacro(const skk::xe::Replay& replay, skc::Macro& out,
                         std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!std::isfinite(replay.tps) || replay.tps <= 1.0) {
        return fail("xe: unusable tps " + std::to_string(replay.tps));
    }

    out = skc::Macro{};
    out.header.tps = replay.tps;
    out.actions.reserve(replay.actions.size());

    std::size_t droppedRelease = 0, droppedDuplicate = 0;
    for (const auto& a : replay.actions) {
        const int64_t shifted = static_cast<int64_t>(a.frame) + kFrameShift;
        if (shifted < 0 || shifted > 0xFFFFFFFFll) {
            return fail("xe: frame " + std::to_string(a.frame) + " shifted by " +
                        std::to_string(kFrameShift) + " is out of range");
        }
        out.actions.push_back(skc::Action{static_cast<uint32_t>(shifted), kButton,
                                          a.pressed, a.player2});
    }

    // Stray releases are dropped here and a collapsed tap is rebuilt as a
    // same-frame up+down (never deleted - see normalizeActionStream()).
    skc::NormalizeReport normalized;
    skc::normalizeActionStream(out.actions, normalized);
    droppedRelease = normalized.droppedRelease;
    droppedDuplicate = normalized.rebuiltTaps;

    if (out.actions.empty()) return fail("xe: no usable inputs after normalisation");

    const uint64_t firstKeptRaw =
        static_cast<uint64_t>(static_cast<int64_t>(out.actions.front().frame) - kFrameShift);

    std::string reason;
    if (!skc::validateActions(out.actions, reason)) {
        return fail("xe: " + reason);
    }

    skkbot2::log::warn(
        "[XE] {} (tps {:.0f}, cbf={}): {} actions -> {} actions, dropped {} "
        "release(s) with no press and rebuilt {} collapsed tap(s), frame shift {:+d} "
        "(first click {} -> {})",
        replay.hasHeader ? "with header" : "no header", replay.tps, replay.cbf,
        replay.actions.size(), out.actions.size(), droppedRelease,
        droppedDuplicate, kFrameShift, firstKeptRaw,
        out.actions.empty() ? 0 : out.actions.front().frame);
    return true;
}

bool skk::xe::importFile(const std::filesystem::path& path, skc::Macro& out,
                         skk::xe::Replay* outInfo, std::string& outReason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outReason = "xe: cannot open " + skk::pathToUtf8(path);
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    if (text.empty()) {
        outReason = "xe: file is empty";
        return false;
    }

    Replay parsed;
    if (!parse(text, parsed, outReason)) return false;
    if (!toSkcMacro(parsed, out, outReason)) return false;
    if (outInfo) *outInfo = parsed;
    return true;
}
