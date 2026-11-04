#include "slc.hpp"
#include "utils.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

// Guards against a corrupt countExp/repeatsExp turning into a huge allocation.
// A section's own byte size already bounds it, so this is a second line.
static constexpr uint64_t kMaxSectionInputs = 1u << 20;
static constexpr uint64_t kMaxActions = 8'000'000;

namespace {

constexpr uint8_t kHeader[8] = {'S', 'L', 'C', '3', 'R', 'P', 'L', 'Y'};
constexpr uint8_t kFooter = 0xCC;
constexpr uint16_t kMetadataSize = 64;

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
    uint16_t u16() {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | (static_cast<uint16_t>(u8()) << 8));
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
    double f64() {
        const uint64_t bits = u64();
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    uint64_t uintOfSize(unsigned size) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; ++i) v |= static_cast<uint64_t>(u8()) << (i * 8);
        return v;
    }
    void skip(size_t n) {
        if (n > remaining()) {
            m_ok = false;
            m_pos = m_data.size();
            return;
        }
        m_pos += n;
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

private:
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
    bool m_ok = true;
};

// state = (delta << 4) | (button << 2) | (player2 << 1) | holding
struct DecodedState {
    uint64_t delta;
    uint8_t button;
    bool player2;
    bool holding;
};

DecodedState decodeState(uint64_t s) {
    return DecodedState{s >> 4, static_cast<uint8_t>((s >> 2) & 3),
                        (s & 2) != 0, (s & 1) != 0};
}

}  // namespace

bool skk::slc::parse(std::span<const uint8_t> data, skk::slc::Replay& out,
                     std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (data.size() < 10 + kMetadataSize) {
        return fail("slc: file is too small to hold a header and metadata");
    }

    Reader r(data);
    uint8_t header[8];
    for (int i = 0; i < 8; ++i) header[i] = r.u8();
    if (std::memcmp(header, kHeader, sizeof(header)) != 0) {
        return fail("slc: not an SLC v3 file (header must be SLC3RPLY)");
    }

    const uint16_t metaSize = r.u16();
    if (metaSize != kMetadataSize) {
        // The reference refuses anything else outright as "outdated or
        // malformed". v2 lands here too, which is the intent.
        return fail("slc: metadata size " + std::to_string(metaSize) +
                    ", expected " + std::to_string(kMetadataSize) +
                    " (outdated or malformed replay?)");
    }

    out = Replay{};
    out.meta.tps = r.f64();
    out.meta.seed = r.u64();
    out.meta.version = r.u32();
    out.meta.build = r.u32();
    out.meta.randomnessAlgorithm = r.u32();
    r.skip(36);  // __padding

    // Atoms: uint32 id, uint64 size with the top byte holding flags.
    while (r.remaining() > 1 && r.ok()) {
        const uint32_t atomId = r.u32();
        const uint64_t rawSize = r.u64();
        const uint64_t atomSize = rawSize & ~(0xFFull << 56);
        if (atomSize == 0 || atomSize > r.remaining()) {
            return fail("slc: atom " + std::to_string(atomId) + " claims " +
                        std::to_string(atomSize) + " bytes but only " +
                        std::to_string(r.remaining()) + " remain");
        }
        const size_t atomEnd = r.pos() + static_cast<size_t>(atomSize);

        if (atomId != 1) {
            // 0 = Null (skip), 2 = Marker (unknown to us). The reference skips
            // the body for anything it does not recognise, and that is the
            // behaviour that keeps custom atoms from breaking a read.
            r.skip(static_cast<size_t>(atomSize));
            continue;
        }

        const uint64_t count = r.u64();
        if (count > kMaxActions) {
            return fail("slc: implausible action count " + std::to_string(count));
        }
        out.actions.reserve(static_cast<size_t>(count));

        while (out.actions.size() + out.specials.size() < count && r.ok()) {
            const uint16_t h = r.u16();
            const unsigned id = h >> 14;

            if (id == 0 || id == 1) {
                const unsigned deltaSize = (h >> 12) & 3;
                const unsigned countExp = (h >> 8) & 0xF;
                uint64_t repeats = 1;
                if (id == 1) repeats = 1ull << ((h >> 3) & 0x1F);

                const unsigned byteSize = 1u << deltaSize;
                const uint64_t length = 1ull << countExp;
                if (length > kMaxSectionInputs || repeats > kMaxSectionInputs) {
                    return fail("slc: section too large (length " +
                                std::to_string(length) + ", repeats " +
                                std::to_string(repeats) + ")");
                }

                std::vector<DecodedState> cluster;
                cluster.reserve(static_cast<size_t>(length));
                for (uint64_t i = 0; i < length; ++i) {
                    cluster.push_back(decodeState(r.uintOfSize(byteSize)));
                }

                for (uint64_t rep = 0; rep < repeats; ++rep) {
                    for (const auto& st : cluster) {
                        const uint64_t prev =
                            out.actions.empty() ? 0 : out.actions.back().frame;
                        const uint64_t frame = prev + st.delta;
                        if (st.button == static_cast<uint8_t>(Button::Swift)) {
                            // Swift is a hold and release on the same frame.
                            out.actions.push_back(
                                Action{frame, Button::Jump, true, st.player2});
                            out.actions.push_back(
                                Action{frame, Button::Jump, false, st.player2});
                        } else {
                            out.actions.push_back(Action{
                                frame, static_cast<Button>(st.button), st.holding,
                                st.player2});
                        }
                        if (out.actions.size() > kMaxActions) {
                            return fail("slc: action count exceeded the ceiling");
                        }
                    }
                }
                continue;
            }

            // Special: 10 TTTT DD XXXXXXXX  (type, deltaSize, frameDelta)
            const unsigned type = (h >> 10) & 0xF;
            const unsigned sz = 1u << ((h >> 8) & 3);
            const uint64_t frameDelta = r.uintOfSize(sz);
            const uint64_t currentFrame =
                out.actions.empty() ? 0 : out.actions.back().frame;

            Special sp;
            sp.frame = currentFrame + frameDelta;
            switch (type) {
                case 1:
                    sp.kind = SpecialKind::Tps;
                    sp.tps = r.f64();
                    break;
                case 0:
                case 2:
                case 3:
                    sp.kind = type == 0 ? SpecialKind::Restart
                                        : (type == 2 ? SpecialKind::RestartFull
                                                     : SpecialKind::Death);
                    sp.seed = r.u64();
                    break;
                default:
                    sp.kind = SpecialKind::Bugpoint;
                    break;
            }
            out.specials.push_back(sp);
        }

        if (r.pos() > atomEnd) {
            return fail("slc: a section read past the end of its atom");
        }
        r.skip(atomEnd - r.pos());
    }

    if (!r.ok()) return fail("slc: truncated stream");

    const uint8_t footer = r.u8();
    if (footer != kFooter) {
        return fail("slc: bad footer");
    }
    if (r.pos() != data.size()) {
        return fail("slc: " + std::to_string(data.size() - r.pos()) +
                    " trailing byte(s) after the footer");
    }

    std::stable_sort(out.actions.begin(), out.actions.end(),
                     [](const Action& a, const Action& b) { return a.frame < b.frame; });
    return true;
}

bool skk::slc::toSkcMacro(const skk::slc::Replay& replay, skc::Macro& out,
                          std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!std::isfinite(replay.meta.tps) || replay.meta.tps <= 1.0) {
        return fail("slc: unusable tps " + std::to_string(replay.meta.tps));
    }

    // A replay carrying deaths or restarts is not something SKC can play: it
    // has no death chain and no restart events, so the run would diverge at the
    // first death with nothing in the log pointing at why.
    if (!replay.specials.empty()) {
        const auto& sp = replay.specials.front();
        const char* kind = sp.kind == SpecialKind::Death     ? "Death"
                           : sp.kind == SpecialKind::Restart ? "Restart"
                           : sp.kind == SpecialKind::RestartFull ? "RestartFull"
                           : sp.kind == SpecialKind::Tps      ? "TPS change"
                                                              : "Bugpoint";
        return fail("slc: replay contains a " + std::string(kind) + " at frame " +
                    std::to_string(sp.frame) +
                    " and SKC cannot represent it (no death chain, input-only "
                    "format)");
    }

    out = skc::Macro{};
    out.header.tps = replay.meta.tps;
    out.actions.reserve(replay.actions.size());

    std::size_t droppedRelease = 0, droppedDuplicate = 0;
    for (const auto& a : replay.actions) {
        const auto button = static_cast<uint8_t>(a.button);
        if (a.frame > 0xFFFFFFFFull) {
            return fail("slc: frame " + std::to_string(a.frame) +
                        " exceeds SKC's 32-bit frame counter");
        }
        out.actions.push_back(skc::Action{static_cast<uint32_t>(a.frame), button,
                                          a.holding, a.player2});
    }

    // Every SLC edge goes in verbatim. A second press on a channel that is
    // still held is one of the encoder's collapsed TAPS (the release happened
    // but was not stored), and normalizeActionStream() rebuilds it as a
    // same-frame up+down instead of deleting it. Deleting it changed the run:
    // on Windy Road the surviving single press made GD apply the jump one
    // physics step earlier than the identical timeline replayed as up+down.
    skc::NormalizeReport normalized;
    skc::normalizeActionStream(out.actions, normalized);
    droppedRelease = normalized.droppedRelease;
    droppedDuplicate = normalized.rebuiltTaps;

    if (out.actions.empty()) return fail("slc: no usable inputs after normalisation");

    std::string reason;
    if (!skc::validateActions(out.actions, reason)) {
        return fail("slc: " + reason);
    }

    skkbot2::log::warn(
        "[SLC] v{} build {} (tps {:.0f}, seed {}): {} actions -> {} actions, "
        "dropped {} release(s) with no press and rebuilt {} collapsed tap(s), no "
        "frame shift",
        replay.meta.version, replay.meta.build, replay.meta.tps,
        replay.meta.seed, replay.actions.size(), out.actions.size(),
        droppedRelease, droppedDuplicate);
    if (droppedDuplicate > 0) {
        std::string frames;
        for (const uint32_t frame : normalized.rebuiltAt) {
            if (!frames.empty()) frames += ", ";
            frames += std::to_string(frame);
        }
        skkbot2::log::info("[SLC] collapsed taps rebuilt at frame(s) {}{}", frames,
                           droppedDuplicate > normalized.rebuiltAt.size() ? ", ..." : "");
    }
    return true;
}

bool skk::slc::importFile(const std::filesystem::path& path, skc::Macro& out,
                          skk::slc::Replay* outInfo, std::string& outReason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outReason = "slc: cannot open " + skk::pathToUtf8(path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        outReason = "slc: file is empty";
        return false;
    }

    Replay parsed;
    if (!parse(bytes, parsed, outReason)) return false;
    if (!toSkcMacro(parsed, out, outReason)) return false;
    if (outInfo) *outInfo = parsed;
    return true;
}
