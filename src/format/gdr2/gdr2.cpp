#include "gdr2.hpp"
#include "utils.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

// Ceilings. The format stores the input and death counts as bare varints, so a
// corrupt or hostile file can claim ~2^64 events.
static constexpr uint64_t kMaxInputs = 4'000'000;
static constexpr uint64_t kMaxDeaths = 1'000'000;
static constexpr uint64_t kMaxString = 0xFFFF;  // the reference refuses longer

// Every imported frame is shifted by this much before it reaches SKC.
//
// Applied at conversion time, not at parse time, so parse() stays a faithful
// reading of the file and the shift is a single, visible decision.
//
// NOTE ON DIRECTION: this is negative on purpose. The observed symptom is that
// an imported first jump lands at 144 where our own working macro of the same
// level jumps at 141, and the player dies at 229 having used two inputs. -1
// moves 144 to 143, towards the working value; +1 was tried first and moves
// away from it.
//
// Still unverified against a playback, and a constant shift only ever fixes a
// CONSTANT divergence. The two macros compared earlier drift apart rather than
// sitting a fixed distance apart (-2, +3, +6, -1, +13), so this may well move
// nothing. It is a one-character change either way.
static constexpr int64_t kFrameShift = -1;

namespace {

class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : m_data(data) {}

    [[nodiscard]] bool ok() const { return m_ok; }
    [[nodiscard]] size_t remaining() const { return m_data.size() - m_pos; }

    bool varint(uint64_t& out) {
        out = 0;
        for (size_t i = 0; i < 10; ++i) {
            if (m_pos >= m_data.size()) {
                m_ok = false;
                return false;
            }
            out |= static_cast<uint64_t>(m_data[m_pos] & 0x7F) << (i * 7);
            const bool more = (m_data[m_pos] & 0x80) != 0;
            ++m_pos;
            if (!more) return true;
        }
        m_ok = false;
        return false;
    }

    std::string str() {
        uint64_t len = 0;
        if (!varint(len)) return {};
        if (len > kMaxString || len > remaining()) {
            m_ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_data.data() + m_pos),
                      static_cast<size_t>(len));
        m_pos += static_cast<size_t>(len);
        return s;
    }

    bool byte(uint8_t& out) {
        if (m_pos >= m_data.size()) {
            m_ok = false;
            return false;
        }
        out = m_data[m_pos++];
        return true;
    }

    void skip(size_t n) {
        if (n > remaining()) {
            m_ok = false;
            return;
        }
        m_pos += n;
    }

    // GDR's binarystream memcpy's the raw object and then reverses the bytes on
    // a little-endian host, so anything that is neither varint nor string is
    // stored BIG-endian. A plain reinterpret_cast would read 8.9e-305 where the
    // framerate 240.0 belongs.
    bool bigEndian(double& out) {
        if (remaining() < 8) {
            m_ok = false;
            return false;
        }
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) b[i] = m_data[m_pos + static_cast<size_t>(7 - i)];
        std::memcpy(&out, b, 8);
        m_pos += 8;
        return true;
    }

    bool bigEndian(float& out) {
        if (remaining() < 4) {
            m_ok = false;
            return false;
        }
        uint8_t b[4];
        for (int i = 0; i < 4; ++i) b[i] = m_data[m_pos + static_cast<size_t>(3 - i)];
        std::memcpy(&out, b, 4);
        m_pos += 4;
        return true;
    }

private:
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
    bool m_ok = true;
};

std::size_t channelIndex(uint8_t button, bool player2) {
    const int base = (button >= 1 && button <= 3) ? button - 1 : 0;
    return static_cast<std::size_t>(base) + (player2 ? 3u : 0u);
}

}  // namespace

bool skk::gdr2::parse(std::span<const uint8_t> data, skk::gdr2::Replay& out,
                      std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (data.size() < 5) return fail("gdr2: file is too small to hold a header");
    if (data[0] != 'G' || data[1] != 'D' || data[2] != 'R') {
        return fail("gdr2: bad magic, expected \"GDR\"");
    }

    Reader r(data);
    r.skip(3);

    uint64_t version = 0;
    r.varint(version);
    if (version != 2) {
        return fail("gdr2: unsupported format version " + std::to_string(version));
    }
    out = Replay{};
    out.version = static_cast<int>(version);

    out.inputTag = r.str();
    // input_has_extension is exactly `!tag.empty()` in the reference (gdr.hpp:142).
    // An empty tag means NO per-input block at all.
    out.hasInputExtension = !out.inputTag.empty();

    out.author = r.str();
    out.description = r.str();
    r.bigEndian(out.duration);

    uint64_t gameVersion = 0;
    r.varint(gameVersion);
    out.gameVersion = static_cast<int>(static_cast<uint32_t>(gameVersion));

    r.bigEndian(out.framerate);

    // The seed is a SIGNED int and getBytes() does `uint64_t value = data`, so
    // a negative seed sign-extends into a full 10-byte LEB128. Truncating back to
    // int32 is what reproduces the original value - a real megahack export of
    // Rauchkammer 2 carries seed -327972610, which as a raw varint reads as
    // 18446744071724944991.
    uint64_t seed = 0;
    r.varint(seed);
    out.seed = static_cast<int>(static_cast<uint32_t>(seed));

    uint64_t coins = 0;
    r.varint(coins);
    out.coins = static_cast<int>(coins);

    uint8_t ldm = 0, platformer = 0;
    r.byte(ldm);
    r.byte(platformer);
    out.ldm = ldm != 0;
    out.platformer = platformer != 0;

    out.botName = r.str();
    uint64_t botVersion = 0;
    r.varint(botVersion);
    out.botVersion = static_cast<int>(botVersion);

    uint64_t levelId = 0;
    r.varint(levelId);
    out.levelId = static_cast<uint32_t>(levelId);
    out.levelName = r.str();

    uint64_t extSize = 0;
    r.varint(extSize);
    r.skip(static_cast<size_t>(extSize));

    uint64_t deathCount = 0;
    r.varint(deathCount);
    if (deathCount > kMaxDeaths) {
        return fail("gdr2: implausible death count " + std::to_string(deathCount));
    }
    out.deaths.reserve(static_cast<size_t>(deathCount));
    uint64_t prevDeath = 0;
    for (uint64_t i = 0; i < deathCount; ++i) {
        uint64_t delta = 0;
        r.varint(delta);
        prevDeath += delta;
        out.deaths.push_back(prevDeath);
    }

    uint64_t inputCount = 0, p1Count = 0;
    r.varint(inputCount);
    r.varint(p1Count);
    if (!r.ok()) return fail("gdr2: truncated while reading the input counts");
    if (inputCount > kMaxInputs) {
        return fail("gdr2: implausible input count " + std::to_string(inputCount));
    }
    if (p1Count > inputCount) {
        return fail("gdr2: P1 count " + std::to_string(p1Count) +
                    " exceeds the total input count " + std::to_string(inputCount));
    }

    out.inputs.reserve(static_cast<size_t>(inputCount));
    uint64_t frameP1 = 0, frameP2 = 0;
    for (uint64_t i = 0; i < inputCount; ++i) {
        const bool isP1 = i < p1Count;
        uint64_t packed = 0;
        r.varint(packed);
        if (!r.ok()) {
            return fail("gdr2: truncated at input " + std::to_string(i) + "/" +
                        std::to_string(inputCount));
        }

        uint8_t button = 1;
        uint64_t delta = 0;
        bool down = false;
        if (out.platformer) {
            // [.. delta | button(2b) | down]
            down = (packed & 1) != 0;
            button = static_cast<uint8_t>((packed >> 1) & 3);
            delta = packed >> 3;
        } else {
            // [.. delta | down] - the button is NOT stored, every event is Jump.
            down = (packed & 1) != 0;
            delta = packed >> 1;
        }
        if (button == 0) button = 1;

        uint64_t& frame = isP1 ? frameP1 : frameP2;
        frame += delta;

        if (out.hasInputExtension) {
            uint64_t inExt = 0;
            r.varint(inExt);
            r.skip(static_cast<size_t>(inExt));
            if (!r.ok()) {
                return fail("gdr2: truncated in the extension of input " +
                            std::to_string(i));
            }
        }

        out.inputs.push_back(Input{frame, button, !isP1, down});
    }

    if (!r.ok()) return fail("gdr2: truncated input stream");

    // The file stores all P1 inputs then all P2 inputs; playback wants a single
    // frame-ordered stream.
    std::stable_sort(out.inputs.begin(), out.inputs.end(),
                     [](const Input& a, const Input& b) { return a.frame < b.frame; });
    return true;
}

bool skk::gdr2::toSkcMacro(const skk::gdr2::Replay& replay, skc::Macro& out,
                           std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!std::isfinite(replay.framerate) || replay.framerate <= 1.0) {
        return fail("gdr2: unusable framerate " + std::to_string(replay.framerate));
    }
    if (replay.platformer) {
        return fail("gdr2: platformer replays are not supported yet");
    }

    out = skc::Macro{};
    out.header.tps = static_cast<float>(replay.framerate);
    out.actions.reserve(replay.inputs.size());

    // Normalise per (button, player2) channel. Other bots emit releases with no
    // press before them, and repeated releases on one channel - both present in
    // real exports: RAUCHKAMMER 2-EC.gdr2 opens with a release at frame 1, and
    // the megahack export ends with two releases in a row. SKC's validator
    // rejects exactly those, so drop them instead of failing the whole import.
    std::array<bool, 6> held{};
    std::size_t droppedRelease = 0, droppedDuplicate = 0;
    for (const auto& in : replay.inputs) {
        const std::size_t ch = channelIndex(in.button, in.player2);
        if (!in.down) {
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
        if (in.frame > 0xFFFFFFFFull) {
            return fail("gdr2: frame " + std::to_string(in.frame) +
                        " exceeds SKC's 32-bit frame counter");
        }
        // Signed shift: a negative value must be able to bring the first frame
        // back towards zero, and the result must not wrap around.
        const int64_t shifted = static_cast<int64_t>(in.frame) + kFrameShift;
        if (shifted < 0 || shifted > 0xFFFFFFFFll) {
            return fail("gdr2: frame " + std::to_string(in.frame) +
                        " shifted by " + std::to_string(kFrameShift) +
                        " is out of range");
        }
        out.actions.push_back(skc::Action{static_cast<uint32_t>(shifted), in.button,
                                          in.down, in.player2});
    }

    if (out.actions.empty()) {
        return fail("gdr2: no usable inputs after normalisation");
    }

    std::string reason;
    if (!skc::validateActions(out.actions, reason)) {
        return fail("gdr2: " + reason);
    }

    skkbot2::log::warn(
        "[GDR2] '{}' by {} / bot '{}' v{} / level '{}' (tps {:.0f}): {} inputs -> "
        "{} actions, dropped {} release(s) with no press and {} duplicate press(es), "
        "frame shift {:+d} (first click frame {} -> {})",
        replay.description.empty() ? std::string("<no description>") : replay.description,
        replay.author, replay.botName, replay.botVersion, replay.levelName,
        replay.framerate, replay.inputs.size(), out.actions.size(), droppedRelease,
        droppedDuplicate, kFrameShift, replay.inputs.front().frame,
        out.actions.front().frame);
    return true;
}

bool skk::gdr2::importFile(const std::filesystem::path& path, skc::Macro& out,
                           skk::gdr2::Replay* outInfo, std::string& outReason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outReason = "gdr2: cannot open " + skk::pathToUtf8(path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        outReason = "gdr2: file is empty";
        return false;
    }

    Replay parsed;
    if (!parse(bytes, parsed, outReason)) return false;
    if (!toSkcMacro(parsed, out, outReason)) return false;
    if (outInfo) *outInfo = parsed;
    return true;
}