#include "ttrl.hpp"
#include "utils.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>

namespace {

constexpr uint8_t kMagic[4] = {'T', 'T', 'R', 'L'};

// Predicted from the hook order, not fitted - see the header.
static constexpr int64_t kFrameShift = 0;

// ToastyReplay's checksum (TtrlCodec.cpp:89-99): reflected CRC-32, poly
// 0xedb88320, init 0xffffffff, final complement. Identical to zlib's crc32,
// which is what confirmed it against the real file.
uint32_t crc32(std::span<const uint8_t> bytes) {
    uint32_t value = 0xffffffffu;
    for (uint8_t byte : bytes) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (value & 1u);
            value = (value >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~value;
}

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : m_bytes(bytes) {}

    bool readByte(uint8_t& out) {
        if (m_pos >= m_bytes.size()) return false;
        out = m_bytes[m_pos++];
        return true;
    }

    bool readVarint(uint64_t& out) {
        uint64_t value = 0;
        uint32_t shift = 0;
        while (shift <= 63) {
            uint8_t byte = 0;
            if (!readByte(byte)) return false;
            if (shift == 63 && (byte & 0xfe) != 0) return false;
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                if (shift != 0 && byte == 0) return false;  // non-canonical
                out = value;
                return true;
            }
            shift += 7;
        }
        return false;
    }

    bool readU64(uint64_t& out) {
        if (remaining() < 8) return false;
        std::memcpy(&out, m_bytes.data() + m_pos, 8);
        m_pos += 8;
        return true;
    }

    bool readFloat(float& out) {
        if (remaining() < 4) return false;
        std::memcpy(&out, m_bytes.data() + m_pos, 4);
        m_pos += 4;
        return true;
    }

    bool skip(size_t count) {
        if (remaining() < count) return false;
        m_pos += count;
        return true;
    }

    [[nodiscard]] size_t position() const { return m_pos; }
    [[nodiscard]] size_t remaining() const { return m_bytes.size() - m_pos; }
    [[nodiscard]] std::span<const uint8_t> rest() const { return m_bytes.subspan(m_pos); }

private:
    std::span<const uint8_t> m_bytes;
    size_t m_pos = 0;
};

std::size_t channelIndex(uint8_t button, bool player2) {
    const int base = (button >= 1 && button <= 3) ? button - 1 : 0;
    return static_cast<std::size_t>(base) + (player2 ? 3u : 0u);
}

}  // namespace

bool skk::ttrl::looksLikeTtrl(std::span<const uint8_t> data) {
    return data.size() >= kMagicSize &&
           std::memcmp(data.data(), kMagic, kMagicSize) == 0;
}

bool skk::ttrl::parse(std::span<const uint8_t> data, skk::ttrl::Replay& out,
                      std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!looksLikeTtrl(data)) return fail("ttrl: not a TTRL file (magic \"TTRL\")");
    if (data.size() < 25) return fail("ttrl: file is too small to hold a header");

    // Verify the checksum over the whole payload before parsing anything. Every
    // field after the magic is a varint, so a single misread shifts the rest of
    // the file silently - this is the check that catches that.
    const size_t payloadSize = data.size() - kChecksumSize;
    uint32_t stored = 0;
    std::memcpy(&stored, data.data() + payloadSize, 4);
    const uint32_t computed = crc32(data.subspan(0, payloadSize));
    if (stored != computed) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "ttrl: checksum mismatch (stored 0x%08x, computed 0x%08x)",
                      stored, computed);
        return fail(buf);
    }

    Reader reader(data.subspan(0, payloadSize));
    reader.skip(kMagicSize);

    uint8_t version = 0, flags = 0;
    if (!reader.readByte(version)) return fail("ttrl: truncated at version");
    if (version != kVersion) {
        return fail("ttrl: unsupported version " + std::to_string(version) +
                    ", this reader handles version 1");
    }
    if (!reader.readByte(flags)) return fail("ttrl: truncated at flags");
    if ((flags & ~kKnownFlags) != 0) {
        return fail("ttrl: unknown flag bits 0x" + std::to_string(flags & ~kKnownFlags) +
                    " - this file was written by a newer ToastyReplay");
    }

    out = Replay{};
    out.flags = flags;

    uint64_t numerator = 0, denominator = 0;
    if (!reader.readVarint(numerator) || !reader.readVarint(denominator)) {
        return fail("ttrl: truncated at tps");
    }
    if (numerator == 0 || denominator == 0) {
        return fail("ttrl: unusable tps " + std::to_string(numerator) + "/" +
                    std::to_string(denominator));
    }
    // The writer refuses to emit an unreduced fraction, so a non-reduced one
    // means we are misreading the field rather than meeting a valid file.
    // Note the check has to be a gcd, not "is either divisible by the other":
    // 240/1 is perfectly reduced, yet 240 % 1 == 0 is always true.
    if (std::gcd(numerator, denominator) != 1) {
        return fail("ttrl: tps " + std::to_string(numerator) + "/" +
                    std::to_string(denominator) + " is not in lowest terms");
    }
    out.tps = static_cast<float>(numerator) / static_cast<float>(denominator);

    uint64_t gameVersion = 0;
    if (!reader.readVarint(gameVersion)) return fail("ttrl: truncated at gameVersion");
    if (gameVersion == 0 || gameVersion > 0xffffffffull) {
        return fail("ttrl: implausible gameVersion " + std::to_string(gameVersion));
    }
    out.gameVersion = static_cast<uint32_t>(gameVersion);

    if (!reader.readVarint(out.levelId) || !reader.readVarint(out.levelRevision)) {
        return fail("ttrl: truncated at levelId/levelRevision");
    }
    if (!reader.readU64(out.levelFingerprint)) return fail("ttrl: truncated at fingerprint");

    if (!reader.readVarint(out.tickCount)) return fail("ttrl: truncated at tickCount");
    if (out.tickCount == 0) return fail("ttrl: tickCount is zero");

    if (flags & kFlagSeed) {
        uint64_t seed = 0;
        if (!reader.readVarint(seed)) return fail("ttrl: truncated at seed");
    }
    if (flags & kFlagStartPos) {
        float startPos = 0.f;
        if (!reader.readFloat(startPos)) return fail("ttrl: truncated at startPos");
    }
    if (flags & kFlagControlFlip) {
        uint8_t controlFlip = 0;
        if (!reader.readByte(controlFlip)) return fail("ttrl: truncated at controlFlip");
    }

    uint64_t inputSize = 0;
    if (!reader.readVarint(inputSize)) return fail("ttrl: truncated at inputSize");
    if (inputSize > kMaxInputSize || inputSize > reader.remaining()) {
        return fail("ttrl: input block of " + std::to_string(inputSize) +
                    " bytes does not fit in the remaining " +
                    std::to_string(reader.remaining()));
    }

    const auto inputSpan = reader.rest().subspan(0, static_cast<size_t>(inputSize));
    if (!reader.skip(static_cast<size_t>(inputSize))) {
        return fail("ttrl: truncated inside the input block");
    }

    const bool channels = (flags & kFlagInputChannels) != 0;
    const uint64_t shift = channels ? 4 : 1;

    Reader inputReader(inputSpan);
    uint64_t previousTick = 0;
    while (inputReader.remaining() > 0) {
        if (out.actions.size() >= kMaxActions) {
            return fail("ttrl: input count exceeds " + std::to_string(kMaxActions));
        }
        uint64_t event = 0;
        if (!inputReader.readVarint(event)) return fail("ttrl: invalid input varint");

        const uint64_t state = channels ? (event & 0xf) : (event & 1);
        const uint64_t tick = previousTick + (event >> shift);
        if (tick >= out.tickCount) {
            return fail("ttrl: input tick " + std::to_string(tick) +
                        " is outside the replay's " + std::to_string(out.tickCount) +
                        " ticks - the header is being misread");
        }

        uint8_t button = 1;
        bool player2 = false;
        if (channels) {
            const uint64_t buttonValue = (state >> 2) + 1;
            if (buttonValue > 3) {
                return fail("ttrl: invalid button value " + std::to_string(buttonValue));
            }
            button = static_cast<uint8_t>(buttonValue);
            player2 = (state & 2) != 0;
        }

        out.actions.push_back(Action{tick, button, (state & 1) != 0, player2});
        previousTick = tick;
    }

    if ((flags & kFlagFrameFix) != 0) {
        uint8_t schema = 0;
        if (!reader.readByte(schema)) return fail("ttrl: truncated at frameFix schema");
        if (schema < 1 || schema > 3) {
            return fail("ttrl: unsupported frameFix schema " + std::to_string(schema));
        }
        uint64_t frameFixSize = 0;
        if (!reader.readVarint(frameFixSize)) return fail("ttrl: truncated at frameFix size");
        if (frameFixSize > reader.remaining()) {
            return fail("ttrl: frameFix block of " + std::to_string(frameFixSize) +
                        " bytes does not fit in the remaining " +
                        std::to_string(reader.remaining()));
        }
        // Skipped, not parsed - see the header. ToastyReplay replays these on
        // top of its inputs, so keeping them would make the import prove
        // nothing about the clicks.
        out.frameFixBytes = frameFixSize;
        if (!reader.skip(static_cast<size_t>(frameFixSize))) {
            return fail("ttrl: truncated inside the frameFix block");
        }
    }

    if (reader.remaining() != 0) {
        return fail("ttrl: " + std::to_string(reader.remaining()) +
                    " unexpected byte(s) after the last section");
    }
    if (out.actions.empty()) return fail("ttrl: no inputs in the file");

    skkbot2::log::info(
        "[TTRL] tps {:.4g}, {} tick(s), game v{}: discarded {} frame fix(es) ({} KB, "
        "{:.1f}% of the file) and took {} input(s)",
        out.tps, out.tickCount, out.gameVersion, out.frameFixBytes,
        out.frameFixBytes / 1024,
        100.0 * static_cast<double>(out.frameFixBytes) /
            static_cast<double>(payloadSize ? payloadSize : 1),
        out.actions.size());
    return true;
}

bool skk::ttrl::toSkcMacro(const skk::ttrl::Replay& replay, skc::Macro& out,
                           std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!std::isfinite(replay.tps) || replay.tps <= 1.0f) {
        return fail("ttrl: unusable tps " + std::to_string(replay.tps));
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
            return fail("ttrl: frame " + std::to_string(a.frame) +
                        " exceeds SKC's 32-bit frame counter");
        }
        const int64_t shifted = static_cast<int64_t>(a.frame) + kFrameShift;
        if (shifted < 0 || shifted > 0xFFFFFFFFll) {
            return fail("ttrl: frame " + std::to_string(a.frame) + " shifted by " +
                        std::to_string(kFrameShift) + " is out of range");
        }
        out.actions.push_back(
            skc::Action{static_cast<uint32_t>(shifted), a.button, a.pressed, a.player2});
    }

    if (out.actions.empty()) return fail("ttrl: no usable inputs after normalisation");

    std::string reason;
    if (!skc::validateActions(out.actions, reason)) return fail("ttrl: " + reason);

    skkbot2::log::warn(
        "[TTRL] {} inputs -> {} actions, dropped {} release(s) with no press and "
        "{} duplicate press(es), frame shift {:+d} (first click {} -> {})",
        replay.actions.size(), out.actions.size(), droppedRelease, droppedDuplicate,
        kFrameShift, firstKeptRaw, out.actions.front().frame);
    return true;
}

bool skk::ttrl::importFile(const std::filesystem::path& path, skc::Macro& out,
                           skk::ttrl::Replay* outInfo, std::string& outReason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outReason = "ttrl: cannot open " + skk::pathToUtf8(path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        outReason = "ttrl: file is empty";
        return false;
    }

    Replay parsed;
    if (!parse(bytes, parsed, outReason)) return false;
    if (!toSkcMacro(parsed, out, outReason)) return false;
    if (outInfo) *outInfo = parsed;
    return true;
}