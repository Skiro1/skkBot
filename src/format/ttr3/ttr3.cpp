#include "ttr3.hpp"
#include "utils.hpp"

#include "format/zlib_inflate.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace {

constexpr uint8_t kMagic[4] = {'T', 'T', 'R', '3'};

// Predicted from the hook, see the header.
static constexpr int64_t kFrameShift = -1;

std::size_t channelIndex(uint8_t button, bool player2) {
    const int base = (button >= 1 && button <= 3) ? button - 1 : 0;
    return static_cast<std::size_t>(base) + (player2 ? 3u : 0u);
}

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes, size_t limit = SIZE_MAX)
        : m_bytes(bytes), m_end(std::min(limit, bytes.size())) {}

    bool readU8(uint8_t& out) { return raw(&out, 1); }
    bool readU16(uint16_t& out) { return raw(&out, 2); }
    bool readU32(uint32_t& out) { return raw(&out, 4); }
    bool readI32(int32_t& out) { return raw(&out, 4); }
    bool readU64(uint64_t& out) { return raw(&out, 8); }
    bool readI64(int64_t& out) { return raw(&out, 8); }
    bool readF32(float& out) { return raw(&out, 4); }
    bool readF64(double& out) { return raw(&out, 8); }

    bool readString(std::string& out, size_t maxSize = 4096) {
        uint16_t length = 0;
        if (!readU16(length) || length > maxSize || remaining() < length) return false;
        out.assign(reinterpret_cast<const char*>(m_bytes.data() + m_pos), length);
        m_pos += length;
        return true;
    }

    bool skip(size_t count) {
        if (remaining() < count) return false;
        m_pos += count;
        return true;
    }

    [[nodiscard]] size_t position() const { return m_pos; }
    [[nodiscard]] size_t remaining() const { return m_end > m_pos ? m_end - m_pos : 0; }
    [[nodiscard]] std::span<const uint8_t> subspan(size_t offset, size_t size) const {
        return m_bytes.subspan(offset, size);
    }

private:
    template <typename T>
    bool raw(T* out, size_t size) {
        if (remaining() < size) return false;
        std::memcpy(out, m_bytes.data() + m_pos, size);
        m_pos += size;
        return true;
    }

    std::span<const uint8_t> m_bytes;
    size_t m_pos = 0;
    size_t m_end = 0;
};

struct SectionEntry {
    uint8_t kind = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

}  // namespace

bool skk::ttr3::looksLikeTtr3(std::span<const uint8_t> data) {
    return data.size() >= kMagicSize &&
           std::memcmp(data.data(), kMagic, kMagicSize) == 0;
}

bool skk::ttr3::parse(std::span<const uint8_t> data, skk::ttr3::Replay& out,
                      std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!looksLikeTtr3(data)) return fail("ttr3: not a TTR3 file (magic \"TTR3\")");
    if (data.size() < kBaseHeaderSize) return fail("ttr3: file is too small for a header");

    Reader header(data);
    header.skip(kMagicSize);
    uint16_t version = 0, reserved = 0;
    uint32_t flags = 0, headerLen = 0;
    if (!header.readU16(version) || !header.readU16(reserved) || !header.readU32(flags) ||
        !header.readU32(headerLen)) {
        return fail("ttr3: truncated header");
    }
    if (version == 0 || version > kWireVersion) {
        return fail("ttr3: unsupported wire version " + std::to_string(version) +
                    ", this reader handles version 1");
    }
    if (reserved != 0) return fail("ttr3: reserved header field is not zero");
    if ((flags & ~kKnownFlags) != 0) {
        return fail("ttr3: unknown flag bits 0x" + std::to_string(flags & ~kKnownFlags) +
                    " - written by a newer ToastyReplay");
    }
    if (headerLen < kBaseHeaderSize || headerLen > data.size()) {
        return fail("ttr3: headerLen " + std::to_string(headerLen) +
                    " does not fit the " + std::to_string(data.size()) + " byte file");
    }

    // Refused, not imported. ToastyReplay only takes its input-only playback
    // path when the macro has no anchors at all (replay_engine.cpp:397-407);
    // with anchors it calls reconcileAnchors() and pins the player every tick.
    // This macro has anchors, so its clicks were recorded against a trajectory
    // that was being supplied to the player - they cannot reproduce the run on
    // their own, and no frame shift changes that.
    // SKC has no death chain: a macro that dies replays from frame 0 and dies in
    // the same place. Silently dropping the deaths would hand back a macro that
    // cannot finish, so this is refused instead - same as SLC does.
    if (flags & kFlagHasCheckpoints) {
        return fail("ttr3: this macro carries checkpoints, so it contains deaths - "
                    "SKC has no death chain and cannot replay that");
    }
    if (flags & kFlagHasPersistence) {
        return fail("ttr3: this macro carries persistence attempts, so it contains "
                    "deaths - SKC has no death chain and cannot replay that");
    }

    out = Replay{};
    out.flags = flags;

    // Set after the Replay is constructed, or the assignment above wipes it.
    if (flags & kFlagHasAnchors) {
        // Short, because Geode's notification bubble is small and long text
        // simply gets cut off at the edge of the screen. The full explanation
        // goes to the log right below; the toast only has to stop the user
        // wondering why the macro dies at frame 698 and blaming the shift.
        out.caution = "Recorded with anchor correction - may not replay correctly.";
        skkbot2::log::warn(
            "[TTR3] this macro carries anchors: ToastyReplay pinned the player to a recorded "
            "trajectory on every frame while playing it, so its clicks were never required to "
            "reproduce the run. They are imported anyway, but the macro may well die partway.");
    }

    Reader meta(data, headerLen);
    meta.skip(kBaseHeaderSize);
    uint64_t sourceFormatId = 0, gameVersion = 0;
    int32_t levelId = 0;
    if (!meta.readU64(sourceFormatId) || !meta.readU64(gameVersion) || !meta.readI32(levelId)) {
        return fail("ttr3: truncated metadata");
    }
    if (!meta.readString(out.levelName) || !meta.readString(out.author)) {
        return fail("ttr3: truncated levelName/author");
    }
    double framerateHint = 0.0;
    if (!meta.readF64(framerateHint)) return fail("ttr3: truncated framerateHint");
    if (!std::isfinite(framerateHint) || framerateHint <= 0.0 || framerateHint > 1000000.0) {
        return fail("ttr3: unusable framerateHint " + std::to_string(framerateHint));
    }
    out.tps = static_cast<float>(framerateHint);

    float startPosX = 0.f, startPosY = 0.f;
    int64_t timestamp = 0;
    uint32_t rngSeed = 0;
    if (!meta.readF32(startPosX) || !meta.readF32(startPosY) || !meta.readI64(timestamp) ||
        !meta.readU32(rngSeed)) {
        return fail("ttr3: truncated metadata");
    }
    if (!std::isfinite(startPosX) || !std::isfinite(startPosY)) {
        return fail("ttr3: start position is not finite");
    }
    if (meta.remaining() >= 1) {
        uint8_t accuracyMode = 0;
        if (!meta.readU8(accuracyMode)) return fail("ttr3: truncated accuracyMode");
    }
    if (meta.remaining() >= sizeof(double)) {
        if (!meta.readF64(out.durationSeconds)) return fail("ttr3: truncated duration");
        if (!std::isfinite(out.durationSeconds) || out.durationSeconds < 0.0) {
            return fail("ttr3: duration is not a usable time");
        }
    } else if (meta.remaining() != 0) {
        return fail("ttr3: metadata has a partial trailing field");
    }

    Reader table(data);
    table.skip(headerLen);
    uint16_t sectionCount = 0;
    if (!table.readU16(sectionCount)) return fail("ttr3: truncated section table");

    std::vector<SectionEntry> entries;
    entries.reserve(sectionCount);
    std::array<bool, 6> seen{};
    uint64_t expectedPayload = 0;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        SectionEntry entry;
        uint8_t r0 = 0, r1 = 0, r2 = 0;
        if (!table.readU8(entry.kind) || !table.readU8(r0) || !table.readU8(r1) ||
            !table.readU8(r2) || !table.readU64(entry.offset) || !table.readU64(entry.size)) {
            return fail("ttr3: truncated section table");
        }
        if (r0 != 0 || r1 != 0 || r2 != 0) {
            return fail("ttr3: nonzero reserved bytes in a section entry");
        }
        if (entry.kind >= seen.size()) {
            return fail("ttr3: unknown section kind " + std::to_string(entry.kind));
        }
        if (seen[entry.kind]) return fail("ttr3: duplicate section kind " + std::to_string(entry.kind));
        seen[entry.kind] = true;
        if (entry.size > UINT64_MAX - entry.offset) {
            return fail("ttr3: section range overflows");
        }
        expectedPayload = std::max(expectedPayload, entry.offset + entry.size);
        entries.push_back(entry);
    }
    if (!seen[kSectionInputs]) return fail("ttr3: no inputs section");

    const auto compressed = data.subspan(table.position());
    std::vector<uint8_t> decompressed;
    std::span<const uint8_t> payload;
    if (flags & kFlagCompressed) {
        if (expectedPayload == 0 || expectedPayload > kMaxPayloadSize) {
            return fail("ttr3: implausible compressed payload size " +
                        std::to_string(expectedPayload));
        }
        if (!inflate::inflateZlib(compressed, decompressed, static_cast<size_t>(expectedPayload))) {
            return fail("ttr3: could not inflate the payload, or its Adler-32 does not match - "
                        "the file is corrupt");
        }
        payload = decompressed;
    } else {
        if (expectedPayload > compressed.size()) {
            return fail("ttr3: sections extend past the end of the file");
        }
        payload = compressed.subspan(0, static_cast<size_t>(expectedPayload));
    }

    uint64_t previousEnd = 0;
    for (const auto& entry : entries) {
        if (entry.offset < previousEnd) return fail("ttr3: overlapping sections");
        previousEnd = entry.offset + entry.size;
        if (previousEnd > payload.size()) return fail("ttr3: a section runs past the payload");
    }

    const SectionEntry& inputSection = entries.front();
    for (const auto& entry : entries) {
        if (entry.kind == kSectionAnchors) out.anchorBytes = entry.size;
    }

    Reader inputs(payload);
    inputs.skip(static_cast<size_t>(inputSection.offset));
    uint64_t inputCount = 0;
    if (!inputs.readU64(inputCount)) return fail("ttr3: truncated input count");
    if (inputCount > kMaxInputs) {
        return fail("ttr3: implausible input count " + std::to_string(inputCount));
    }
    if (inputCount > (inputSection.size - 8) / kInputRecordSize) {
        return fail("ttr3: " + std::to_string(inputCount) + " inputs do not fit in a " +
                    std::to_string(inputSection.size) + " byte section");
    }

    struct RawInput {
        double time = 0.0;
        uint8_t button = 1;
        bool player2 = false;
        bool pressed = false;
        bool swift = false;
    };
    std::vector<RawInput> raw;
    raw.reserve(static_cast<size_t>(inputCount));
    for (uint64_t i = 0; i < inputCount; ++i) {
        RawInput entry;
        uint8_t flagsByte = 0;
        uint16_t reservedInput = 0;
        if (!inputs.readF64(entry.time) || !inputs.readU8(entry.button) ||
            !inputs.readU8(flagsByte) || !inputs.readU16(reservedInput)) {
            return fail("ttr3: truncated input record " + std::to_string(i));
        }
        if (!std::isfinite(entry.time) || entry.time < 0.0) {
            return fail("ttr3: input " + std::to_string(i) + " has an unusable time");
        }
        if (entry.button < 1 || entry.button > 3) {
            return fail("ttr3: input " + std::to_string(i) + " has button " +
                        std::to_string(entry.button) + ", expected 1..3");
        }
        if ((flagsByte & ~0x07u) != 0) {
            return fail("ttr3: input " + std::to_string(i) + " has unknown flag bits");
        }
        if (reservedInput != 0) {
            return fail("ttr3: input " + std::to_string(i) + " has nonzero reserved bytes");
        }
        if (!raw.empty() && entry.time < raw.back().time) {
            return fail("ttr3: input " + std::to_string(i) + " goes back in time");
        }
        entry.player2 = (flagsByte & 1u) != 0;
        entry.pressed = (flagsByte & 2u) != 0;
        entry.swift = (flagsByte & 4u) != 0;
        raw.push_back(entry);
    }

    // A swift pair is a hold and its release stamped on the same frame. The
    // partner carries its own later time, which the writer only keeps for
    // readers that want to know they differ; collapse it onto the anchor's
    // frame, which is what the reference reader does.
    std::size_t swiftPairs = 0, malformedSwift = 0;
    const double swiftTolerance = 1.0 / out.tps;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (!raw[i].swift) continue;
        if (i + 1 >= raw.size()) {
            ++malformedSwift;
            break;
        }
        RawInput& partner = raw[i + 1];
        const bool matching = partner.button == raw[i].button && partner.player2 == raw[i].player2 &&
                             partner.pressed != raw[i].pressed &&
                             std::fabs(partner.time - raw[i].time) < swiftTolerance;
        if (!matching) {
            ++malformedSwift;
            continue;
        }
        partner.time = raw[i].time;
        ++swiftPairs;
    }

    const double fps = static_cast<double>(out.tps);
    out.actions.reserve(raw.size());
    for (const auto& entry : raw) {
        const double exact = entry.time * fps;
        const double rounded = std::nearbyint(exact);
        if (std::fabs(exact - rounded) > 1e-6) ++out.fractionalFrames;
        out.actions.push_back(Action{static_cast<uint64_t>(std::max(0.0, rounded)), entry.button,
                                     entry.pressed, entry.player2});
    }

    if (out.actions.empty()) return fail("ttr3: no inputs in the file");

    if (out.fractionalFrames > 0) {
        skkbot2::log::warn(
            "[TTR3] {} of {} inputs land on a fractional frame at {:.6g}tps; they were rounded "
            "to whole frames, and SKC cannot store the sub-frame part",
            out.fractionalFrames, out.actions.size(), out.tps);
    }
    if (malformedSwift > 0) {
        skkbot2::log::warn("[TTR3] {} swift pair anchor(s) had no matching partner", malformedSwift);
    }
    skkbot2::log::info(
        "[TTR3] \"{}\" by {} at {}tps: discarded {} anchor byte(s) ({} KB, {:.1f}% of the "
        "payload) and took {} input(s)",
        out.levelName, out.author, out.tps, out.anchorBytes, out.anchorBytes / 1024,
        100.0 * static_cast<double>(out.anchorBytes) / static_cast<double>(payload.size()),
        out.actions.size());
    return true;
}

bool skk::ttr3::toSkcMacro(const skk::ttr3::Replay& replay, skc::Macro& out,
                           std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!std::isfinite(replay.tps) || replay.tps <= 1.0f) {
        return fail("ttr3: unusable tps " + std::to_string(replay.tps));
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
            return fail("ttr3: frame " + std::to_string(a.frame) +
                        " exceeds SKC's 32-bit frame counter");
        }
        const int64_t shifted = static_cast<int64_t>(a.frame) + kFrameShift;
        if (shifted < 0 || shifted > 0xFFFFFFFFll) {
            return fail("ttr3: frame " + std::to_string(a.frame) + " shifted by " +
                        std::to_string(kFrameShift) + " is out of range");
        }
        out.actions.push_back(
            skc::Action{static_cast<uint32_t>(shifted), a.button, a.pressed, a.player2});
    }

    if (out.actions.empty()) return fail("ttr3: no usable inputs after normalisation");

    std::string reason;
    if (!skc::validateActions(out.actions, reason)) return fail("ttr3: " + reason);

    skkbot2::log::warn(
        "[TTR3] {} inputs -> {} actions, dropped {} release(s) with no press and "
        "{} duplicate press(es), frame shift {:+d} (first click {} -> {})",
        replay.actions.size(), out.actions.size(), droppedRelease, droppedDuplicate,
        kFrameShift, firstKeptRaw, out.actions.front().frame);
    return true;
}

bool skk::ttr3::importFile(const std::filesystem::path& path, skc::Macro& out,
                           skk::ttr3::Replay* outInfo, std::string& outReason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outReason = "ttr3: cannot open " + skk::pathToUtf8(path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        outReason = "ttr3: file is empty";
        return false;
    }

    Replay parsed;
    if (!parse(bytes, parsed, outReason)) return false;
    if (!toSkcMacro(parsed, out, outReason)) return false;
    if (outInfo) *outInfo = parsed;
    return true;
}