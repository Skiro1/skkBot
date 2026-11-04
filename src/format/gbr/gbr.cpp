#include "gbr.hpp"
#include "utils.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace {

constexpr uint8_t kMagic[4] = {'G', 'B', 'R', '6'};
constexpr uint8_t kHasLevelName = 1 << 4;
constexpr uint8_t kHasDeaths = 1 << 5;
constexpr uint8_t kHasSubtick = 1 << 6;

constexpr size_t kMaxName = 0xFFFF;
constexpr size_t kMaxStream = 64ull * 1024 * 1024;
constexpr size_t kMaxActions = 8'000'000;
constexpr size_t kMaxSubticks = 2'000'000;

// Every imported frame is shifted by this much. One constant, one character.
//
// -1 is being TRIED, not derived. The evidence points the other way: this
// macro's first click sits at 141, exactly where our own working recording of
// the same level starts, and a -1 shift would move it to 140 and break the one
// thing that currently matches. So there is a falsifiable prediction attached:
// if the opening is what matters, the run dies EARLIER than frame 4887. If it
// dies later, the shift is doing something real despite the start disagreeing.
// If it dies at 4887 again, the shift changed nothing.
static constexpr int64_t kFrameShift = -1;

class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : m_data(data) {}

    [[nodiscard]] bool ok() const { return m_ok; }
    [[nodiscard]] size_t pos() const { return m_pos; }
    [[nodiscard]] size_t remaining() const { return m_data.size() - m_pos; }

    uint8_t u8() {
        if (m_pos >= m_data.size()) {
            m_ok = false;
            return 0;
        }
        return m_data[m_pos++];
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (i * 8);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(u8()) << (i * 8);
        return v;
    }
    float f32() {
        const uint32_t b = u32();
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    double f64() {
        const uint64_t b = u64();
        double d;
        std::memcpy(&d, &b, 8);
        return d;
    }
    // GBR6 strings are uint16-length prefixed, NOT uint32. GBR6_HEADER_BASE is
    // 36 and the fixed fields before the name are 4+1+1+4+8+8+4+4 = 34: the
    // constant is what says the prefix is two bytes.
    std::string str() {
        const uint16_t len =
            static_cast<uint16_t>(u8() | (static_cast<uint16_t>(u8()) << 8));
        if (len > remaining()) {
            m_ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_data.data() + m_pos), len);
        m_pos += len;
        return s;
    }
    std::span<const uint8_t> take(size_t n) {
        if (n > remaining()) {
            m_ok = false;
            return {};
        }
        const auto view = m_data.subspan(m_pos, n);
        m_pos += n;
        return view;
    }
    void skip(size_t n) {
        if (n > remaining()) {
            m_ok = false;
            m_pos = m_data.size();
            return;
        }
        m_pos += n;
    }

private:
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
    bool m_ok = true;
};

// A stream is (b0, b1) records. Three shapes, told apart by the top bit:
//   b0 & 0x80  regular input; b0 = 0x80|(p2<<4)|((button&7)<<1)|pressed,
//              b1 = frame delta, 0xFF meaning "+255 and keep going"
//   b0 == 0x00  autoclicker: b1 = (hold<<4)|release, then u16 cycles; the
//              pattern is expanded into press/release pairs here
//   otherwise   tap: b0 is the hold duration, b1 the gap to the next press
bool decodeStream(std::span<const uint8_t> data, bool streamIsP2,
                  std::vector<skk::gbr::Action>& out, std::string& outReason) {
    if (data.size() < 2) return true;
    uint64_t frame = 0;
    uint32_t pending = 0;
    size_t i = 0;

    auto push = [&](uint64_t f, bool pressed, uint8_t button, bool ac) {
        if (out.size() >= kMaxActions) return false;
        out.push_back(skk::gbr::Action{f, button, pressed, streamIsP2, ac});
        return true;
    };

    while (i + 1 < data.size()) {
        const uint8_t b0 = data[i];
        const uint8_t b1 = data[i + 1];
        i += 2;

        if (b0 == 0x00) {
            if (i + 1 >= data.size()) break;
            const uint16_t cycles =
                static_cast<uint16_t>(data[i] | (static_cast<uint16_t>(data[i + 1]) << 8));
            i += 2;
            const uint8_t hold = (b1 >> 4) & 0x0F;
            const uint8_t rel = b1 & 0x0F;
            if (!hold || !rel || !cycles) continue;
            frame += pending;
            pending = 0;
            for (uint32_t c = 0; c < cycles; ++c) {
                if (!push(frame, true, 1, true)) return false;
                frame += hold;
                if (!push(frame, false, 1, true)) return false;
                frame += rel;
            }
            continue;
        }

        if (b1 == 0xFF) {
            pending += 255;
            if (b0 == 0x80) continue;  // bare delta run
            if ((b0 & 0x80) == 0) {
                const uint8_t holdDur = b0 & 0x7F;
                frame += pending;
                pending = 0;
                if (!push(frame, true, 1, false)) return false;
                frame += holdDur;
                if (!push(frame, false, 1, false)) return false;
            }
            continue;
        }

        frame += pending;
        pending = 0;
        frame += b1;

        if ((b0 & 0x80) == 0) {
            const uint8_t holdDur = b0 & 0x7F;
            if (!push(frame, true, 1, false)) return false;
            frame += holdDur;
            if (!push(frame, false, 1, false)) return false;
        } else {
            const bool p2 = (b0 & 0x10) != 0;
            const uint8_t btn = static_cast<uint8_t>((b0 >> 1) & 0x07);
            const bool pressed = (b0 & 0x01) != 0;
            if (!push(frame, pressed, btn ? btn : 1, false)) return false;
        }
    }

    if (out.size() >= kMaxActions) {
        outReason = "gbr: input stream exceeds the action ceiling";
        return false;
    }
    return true;
}

}  // namespace

bool skk::gbr::looksLikeGbr(std::span<const uint8_t> data) {
    return data.size() >= 4 && std::memcmp(data.data(), kMagic, 4) == 0;
}

bool skk::gbr::looksLikeGbrByExtension(std::string_view ext) {
    for (const char* known : kExtensions) {
        if (ext == known) return true;
    }
    return false;
}

bool skk::gbr::parse(std::span<const uint8_t> data, skk::gbr::Replay& out,
                     std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!looksLikeGbr(data)) {
        return fail("gbr: not a GBR6 file (magic must be \"GBR6\")");
    }

    Reader r(data);
    r.skip(4);

    out = Replay{};
    out.version = r.u8();
    out.flags = r.u8();
    out.tps = r.f32();
    out.timestamp = static_cast<int64_t>(r.u64());
    out.rngSeed = r.u64();

    const uint32_t p1Len = r.u32();
    const uint32_t p2Len = r.u32();
    if (p1Len > kMaxStream || p2Len > kMaxStream) {
        return fail("gbr: implausible stream size");
    }
    out.name = r.str();
    if (!r.ok()) return fail("gbr: truncated header");

    const auto p1 = r.take(p1Len);
    const auto p2 = r.take(p2Len);
    if (!r.ok()) return fail("gbr: truncated input streams");

    if (out.flags & kHasLevelName) {
        out.levelName = r.str();
        if (!r.ok()) return fail("gbr: truncated level name");
    }
    if (out.flags & kHasDeaths) {
        const uint32_t n = r.u32();
        if (n > kMaxActions) return fail("gbr: implausible death count");
        out.deaths.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            Death d;
            d.frame = r.u32();
            d.type = r.u8();
            out.deaths.push_back(d);
        }
        if (!r.ok()) return fail("gbr: truncated death list");
    }
    if (out.flags & kHasSubtick) {
        // The deaths block is always written alongside, so a reader can find
        // where this one starts without guessing.
        if (!(out.flags & kHasDeaths)) r.u32();
        const uint32_t n = r.u32();
        if (n > kMaxSubticks) return fail("gbr: implausible subtick count");
        out.subticks.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            Subtick st;
            st.frame = r.u32();
            st.button = r.u8();
            const uint8_t f = r.u8();
            st.pressed = (f & 1) != 0;
            st.player2 = (f & 2) != 0;
            st.offset = r.f64();
            out.subticks.push_back(st);
        }
        if (!r.ok()) return fail("gbr: truncated subtick block");
    }

    if (!r.ok()) return fail("gbr: truncated stream");
    if (r.pos() != data.size()) {
        return fail("gbr: " + std::to_string(data.size() - r.pos()) +
                    " trailing byte(s) after the last section");
    }

    std::string reason;
    if (!decodeStream(p1, false, out.actions, reason) ||
        !decodeStream(p2, true, out.actions, reason)) {
        return fail(std::move(reason));
    }

    std::stable_sort(out.actions.begin(), out.actions.end(),
                     [](const Action& a, const Action& b) { return a.frame < b.frame; });
    return true;
}

bool skk::gbr::toSkcMacro(const skk::gbr::Replay& replay, skc::Macro& out,
                          std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!std::isfinite(replay.tps) || replay.tps <= 1.0f) {
        return fail("gbr: unusable tps " + std::to_string(replay.tps));
    }
    // Platformer macros are importable now: SKC carries the mode flag and the
    // buttons (2/3) were already in the action stream. What SKC cannot carry is
    // a death chain: a macro carrying deaths cannot be played, and silently
    // dropping them would produce one that dies where the original did not -
    // with nothing in the log pointing at the cause.
    if (!replay.deaths.empty()) {
        return fail("gbr: replay has " + std::to_string(replay.deaths.size()) +
                    " death(s), first at frame " +
                    std::to_string(replay.deaths.front().frame) +
                    "; SKC cannot represent them (no death chain, input-only "
                    "format)");
    }

    out = skc::Macro{};
    out.header.tps = replay.tps;
    out.platformerMode = replay.platformer();
    out.platformerKnown = true;
    out.header.platformerMode = out.platformerMode;
    out.header.platformerKnown = true;
    out.actions.reserve(replay.actions.size());

    std::size_t droppedRelease = 0, droppedDuplicate = 0;
    std::size_t fromAc = 0;
    for (const auto& a : replay.actions) {
        if (a.fromAutoclicker) ++fromAc;
        if (a.frame > 0xFFFFFFFFull) {
            return fail("gbr: frame " + std::to_string(a.frame) +
                        " exceeds SKC's 32-bit frame counter");
        }
        const int64_t shifted = static_cast<int64_t>(a.frame) + kFrameShift;
        if (shifted < 0 || shifted > 0xFFFFFFFFll) {
            return fail("gbr: frame " + std::to_string(a.frame) + " shifted by " +
                        std::to_string(kFrameShift) + " is out of range");
        }
        out.actions.push_back(
            skc::Action{static_cast<uint32_t>(shifted), a.button, a.pressed, a.player2});
    }

    // Stray releases are dropped and a collapsed tap is rebuilt as a same-frame
    // up+down (never deleted - see normalizeActionStream()).
    skc::NormalizeReport normalized;
    skc::normalizeActionStream(out.actions, normalized);
    droppedRelease = normalized.droppedRelease;
    droppedDuplicate = normalized.rebuiltTaps;

    if (out.actions.empty()) return fail("gbr: no usable inputs after normalisation");

    std::string reason;
    if (!skc::validateActions(out.actions, reason)) {
        return fail("gbr: " + reason);
    }

    // Sub-ticks are precision SKC cannot hold. Saying so is the difference
    // between a user who knows their macro is slightly weaker and one who
    // wonders why it desyncs.
    if (!replay.subticks.empty()) {
        skkbot2::log::warn(
            "[GBR] '{}': dropped {} sub-tick offset(s) (SCBF precision) - SKC "
            "stores whole frames only, so those inputs land on the tick edge",
            replay.name.empty() ? std::string("<unnamed>") : replay.name,
            replay.subticks.size());
    }

    skkbot2::log::warn(
        "[GBR] v{} (tps {:.0f}, {}, rngSeed {}{}): {} actions -> {} actions, "
        "{} from autoclicker, dropped {} release(s) with no press and rebuilt {} "
        "collapsed tap(s), frame shift {:+d} (first click {} -> {})",
        replay.version, replay.tps,
        replay.platformer() ? "platformer" : "classic", replay.rngSeed,
        replay.rngLocked() ? ", rng locked" : "", replay.actions.size(),
        out.actions.size(), fromAc, droppedRelease, droppedDuplicate, kFrameShift,
        replay.actions.empty() ? 0 : replay.actions.front().frame,
        out.actions.empty() ? 0 : out.actions.front().frame);
    return true;
}

bool skk::gbr::importFile(const std::filesystem::path& path, skc::Macro& out,
                          skk::gbr::Replay* outInfo, std::string& outReason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outReason = "gbr: cannot open " + skk::pathToUtf8(path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        outReason = "gbr: file is empty";
        return false;
    }

    Replay parsed;
    if (!parse(bytes, parsed, outReason)) return false;
    if (!toSkcMacro(parsed, out, outReason)) return false;
    if (outInfo) *outInfo = parsed;
    return true;
}