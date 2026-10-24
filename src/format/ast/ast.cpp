#include "ast.hpp"

#include "ast_wasm.hpp"
#include "format/slc/slc.hpp"

// The reader refuses anything it cannot decode exactly. A macro that plays one
// frame off, or that presses the wrong button because an unproven prefix code
// was guessed at, is worse than a macro that fails to load - the first wastes
// an attempt, the second wastes the run and looks like the user's fault.

#include <zstd.h>

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace skk::ast {

// The default is a guess about the LEVEL, not a fact about the format, and the
// two levels we have disagree:
//
//   GRIEF          two-player macro, plays through with 2 - the left button;
//   a user's level plays through with 1 - the jump button.
//
// 1 is the default because it is also what player one is hardcoded to, so a
// level that gives both players the jump button works out of the box. GRIEF is
// the exception, and the setting is how a level like that gets corrected.
//
namespace {

// Only the fallback path uses this. The converter re-emits the macro as
// Silicate whenever it can, and that record carries the button, so in practice
// the button is read from the file and this never decides anything.
constexpr std::uint8_t kFallbackPlayer2Button = 1;

// Names only exist for the log line. GD's PlayerButton is 1..3 and the GUI
// offers 1..4, so the fourth is spelled out rather than guessed at.
const char* buttonName(std::uint8_t button) {
    switch (button) {
        case 1: return "Jump";
        case 2: return "Left";
        case 3: return "Right";
        case 4: return "Down";
        default: return "?";
    }
}

constexpr uint64_t kLoopGuard = 1ull << 20;

constexpr std::size_t kMagicSize = 4;
constexpr std::size_t kTpsGuard = 1u << 20;   // refuse absurd tps rather than divide by it

bool readFile(const std::filesystem::path& path, std::vector<uint8_t>& out,
              std::string& reason) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        reason = "cannot open file";
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0) {
        reason = "file is empty";
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), size);
    if (!in) {
        reason = "short read";
        return false;
    }
    return true;
}

// sub_1800439B0. The bit index descends from 7 to 0 inside a byte and wraps to
// 7 on the next byte, which is MSB-first per byte. A varint takes one
// continuation bit FIRST, then seven data bits, and the first data bit read
// lands on bit 6 of the value.
class BitReader {
public:
    BitReader(const uint8_t* data, std::size_t size)
        : mData(data), mSize(size) {}

    bool bit(uint32_t& out) {
        if (mByte >= mSize) return false;
        out = (mData[mByte] >> mBit) & 1u;
        if (mBit == 0) {
            mBit = 7;
            ++mByte;
        } else {
            --mBit;
        }
        return true;
    }

    bool varint(uint64_t& out) {
        uint64_t value = 0;
        uint32_t shift = 0;
        for (;;) {
            uint32_t cont = 0;
            if (!bit(cont)) return false;
            uint32_t chunk = 0;
            for (int i = 0; i < 7; ++i) {
                uint32_t b = 0;
                if (!bit(b)) return false;
                chunk = (chunk << 1) | b;
            }
            value |= static_cast<uint64_t>(chunk) << shift;
            if (cont == 0) break;
            shift += 7;
            if (shift > 63) return false;
        }
        out = value;
        return true;
    }

    // True when everything from the cursor on is zero padding. Strictly weaker
    // than exact consumption on purpose: see parse() for why zeroes are allowed
    // and anything else is not.
    [[nodiscard]] bool allRemainingAreZero() const {
        for (std::size_t i = mByte; i < mSize; ++i) {
            if (mData[i] != 0) return false;
        }
        return true;
    }

private:
    const uint8_t* mData;
    std::size_t mSize;
    std::size_t mByte = 0;
    uint32_t mBit = 7;
};

float readLeFloat(const uint8_t* p) {
    float value = 0.0f;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

bool decompressPayload(std::span<const uint8_t> data, std::vector<uint8_t>& out,
                       std::string& reason) {
    // Locate the frame by its magic rather than at a fixed offset: the header
    // carries fields whose presence has changed between builds, and a fixed
    // offset would break on the next one.
    std::size_t start = data.size();
    for (std::size_t i = 0; i + 4 <= data.size(); ++i) {
        if (data[i] == 0x28 && data[i + 1] == 0xB5 && data[i + 2] == 0x2F &&
            data[i + 3] == 0xFD) {
            start = i;
            break;
        }
    }
    if (start == data.size()) {
        reason = "no zstd frame in container";
        return false;
    }

    const auto* src = data.data() + start;
    const auto srcSize = data.size() - start;

    const unsigned long long declared = ZSTD_getFrameContentSize(src, srcSize);
    if (declared == ZSTD_CONTENTSIZE_ERROR || declared == ZSTD_CONTENTSIZE_UNKNOWN) {
        reason = "zstd frame does not declare its size";
        return false;
    }
    if (declared > (64ull << 20)) {
        reason = "zstd frame declares an implausible size";
        return false;
    }

    out.resize(static_cast<std::size_t>(declared));
    const auto got = ZSTD_decompress(out.data(), out.size(), src, srcSize);
    if (ZSTD_isError(got) || got != declared) {
        reason = std::string("zstd inflate failed: ") + ZSTD_getErrorName(got);
        return false;
    }
    out.resize(got);
    return true;
}

}  // namespace

bool looksLikeAst(std::span<const uint8_t> data) {
    return data.size() >= kMagicSize && data[0] == 'A' && data[1] == 'S' &&
           data[2] == 'T' && data[3] == '9';
}

bool parse(std::span<const uint8_t> data, Replay& out, std::string& outReason) {
    outReason.clear();
    out = Replay{};

    if (!looksLikeAst(data)) {
        outReason = "not an AST9 container";
        return false;
    }
    if (data.size() < 16) {
        outReason = "container is too short for a header";
        return false;
    }

    std::size_t pos = 4;
    if (data[pos++] != 0x00) {
        outReason = "unexpected byte after the magic";
        return false;
    }

    // Build identity: byte-aligned varint length then that many raw bytes. The
    // header is byte-aligned in every sample, unlike the payload.
    uint64_t nameLen = 0;
    {
        uint32_t shift = 0;
        for (;;) {
            if (pos >= data.size() || shift > 56) {
                outReason = "malformed build identity length";
                return false;
            }
            const uint8_t b = data[pos++];
            nameLen |= static_cast<uint64_t>(b & 0x7Fu) << shift;
            if ((b & 0x80u) == 0) break;
            shift += 7;
        }
    }
    if (nameLen > 0xFF || pos + nameLen + 5 > data.size()) {
        outReason = "container is too short for its header";
        return false;
    }
    out.meta.build.assign(reinterpret_cast<const char*>(data.data() + pos),
                          static_cast<std::size_t>(nameLen));
    pos += static_cast<std::size_t>(nameLen);

    // One 0x00, then the tps as a little-endian float.
    if (data[pos] != 0x00) {
        outReason = "unexpected field before the tps";
        return false;
    }
    const float tps = readLeFloat(data.data() + pos + 1);
    if (!(tps > 1.0f) || !(tps < static_cast<float>(kTpsGuard))) {
        outReason = "implausible tps in the header";
        return false;
    }
    out.meta.tps = tps;
    pos += 5;  // the 0x00 checked above plus the four float bytes

    // Past the tps come an LEB128 seed and a u16. The low six bits of that u16
    // mark which of Astral's six macro inputs this macro uses - it lives in the
    // file rather than in the payload, which is why a one-input macro spends
    // exactly two bits per state change instead of six.
    std::size_t zstdAt = data.size();
    for (std::size_t i = 0; i + 4 <= data.size(); ++i) {
        if (data[i] == 0x28 && data[i + 1] == 0xB5 && data[i + 2] == 0x2F &&
            data[i + 3] == 0xFD) {
            zstdAt = i;
            break;
        }
    }
    if (zstdAt == data.size()) {
        outReason = "no zstd frame in container";
        return false;
    }
    for (uint32_t shift = 0;; shift += 7) {
        if (pos >= zstdAt || shift > 56) {
            outReason = "malformed seed in the header";
            return false;
        }
        const uint8_t b = data[pos++];
        if ((b & 0x80u) == 0) break;
    }
    if (pos + 2 > zstdAt) {
        outReason = "container is too short for the input mask";
        return false;
    }
    const uint16_t slotMask = static_cast<uint16_t>(data[pos] |
                                                  (static_cast<uint16_t>(data[pos + 1]) << 8));

    // The payload itself is decoded by the official Astral converter, running
    // as WebAssembly. Our own reader only ever agreed with the macros that
    // carried no loop blocks, and then read one of those as full of loops -
    // that was our misalignment, not the file. The converter handles all seven
    // of our real macros, so guessing further would only produce macros that
    // look plausible and are wrong.
    wasm::Output decoded;
    if (!wasm::convert(data, decoded, outReason)) return false;

    // Ask the converter for the same macro in a format that keeps what its own
    // per-click getters throw away.
    //
    // The wasm surface is frame/p1/p2 per click and nothing else, and that is
    // lossy in ways that matter: no button, no hold, no per-frame fps. The
    // converter's own structs DO carry them (fps_index, holding, is_p2, button)
    // - it can emit Silicate, whose record is
    // (delta << 4) | (button << 2) | (p2 << 1) | holding. So re-emitting and
    // reading that back gives us the truth instead of a reconstruction of it.
    //
    // Measured on all seven real macros: tps identical including as_world's
    // 2305, last frame identical, and zero frame-by-frame differences on five.
    // On the other two the two paths differ exactly where the old path FABRICATED
    // a release - GRIEF put one at 17100 where the real one is at 17114, thinking
    // at 13724 where it is 13763. So this is not a second opinion, it is a
    // strictly more faithful one, and the tap-rebuild hack below exists only for
    // the fallback now.
    {
        std::vector<uint8_t> slcBytes;
        std::string slcReason;
        if (wasm::saveAs(data, wasm::kFormatSlcV3, slcBytes, slcReason)) {
            skk::slc::Replay replay;
            skc::Macro macro;
            if (slc::parse(slcBytes, replay, slcReason) &&
                slc::toSkcMacro(replay, macro, slcReason)) {
                out.actions = std::move(macro.actions);
                out.meta.tps = decoded.tps;
                skkbot2::log::info(
                    "[AST] read through the converter's own SLC output ({} bytes): {} actions, "
                    "buttons and holds taken from the file",
                    slcBytes.size(), out.actions.size());
                return true;
            }
        }
        skkbot2::log::warn(
            "[AST] SLC re-emission unavailable ({}); falling back to per-click getters, where the "
            "second player's button is a guess and holds are reconstructed",
            slcReason);
    }

    // Only reached when the converter would not re-emit as SLC. The per-click
    // export has the second channel but no button - not in the payload, not in
    // the header (the u16 is 0x0224 in every macro we have) - so there is
    // nothing left to do but guess, and it is guessed once, loudly, instead of
    // being a menu the user has to be right about.
    //
    // 1 is the jump button, which is what a two-player gamemode normally gives
    // player two. The old menu existed because this guess had been wrong on a
    // level; the SLC path is why it no longer has to be a guess at all.
    const uint8_t p2Button = kFallbackPlayer2Button;

    out.actions.clear();
    out.actions.reserve(decoded.clicks.size() * 2);

    // A Click means "this button went down on this frame", and the converter
    // stores one value per player per frame. A tap - let go and press again
    // within the same step - therefore collapses into a single Click while the
    // button is still down, and a stream carrying that is two presses with no
    // release between them, which the SKC validator rejects.
    //
    // Both of the macros that trip it do so exactly once, on the first frame
    // the validator names: GRIEF at 17100, thinking at 13724. artemis, which
    // imports, has none. So this is the converter's frame granularity talking,
    // not a broken file.
    //
    // Rebuild the tap as a zero-length hold: the release lands on the same
    // frame the press does. The press keeps its exact frame, and the only thing
    // added is the release that physically has to have happened for it.
    bool held1 = false;
    bool held2 = false;
    int rebuiltTaps = 0;
    int p2Clicks = 0;
    for (const auto& click : decoded.clicks) {
        // Skip means that player did nothing on this frame.
        if (click.p1 != wasm::Kind::Skip) {
            const bool down = click.p1 == wasm::Kind::Click;
            if (down && held1) {
                out.actions.push_back(skc::Action{click.frame, 1, false, false});
                ++rebuiltTaps;
            }
            // A release with nothing held would be rejected downstream, and
            // the converter has no way to produce one, so the guard is cheap.
            if (down || held1) {
                out.actions.push_back(skc::Action{click.frame, 1, down, false});
                held1 = down;
            }
        }
        if (click.p2 != wasm::Kind::Skip) {
            const bool down = click.p2 == wasm::Kind::Click;
            if (down) ++p2Clicks;
            if (down && held2) {
                out.actions.push_back(skc::Action{click.frame, p2Button, false, true});
                ++rebuiltTaps;
            }
            if (down || held2) {
                out.actions.push_back(skc::Action{click.frame, p2Button, down, true});
                held2 = down;
            }
        }
    }
    // Neither of those two is news to the person importing the macro: the
    // second channel's button is fixed above, and the tap rebuild happens on
    // every import of a macro that needs it. Both go to the log, where they
    // are available if a macro ever does play wrong, instead of into a
    // notification the user has to dismiss for every file.
    if (rebuiltTaps > 0) {
        skkbot2::log::info(
            "[AST] rebuilt {} tap(s) the converter collapsed into one press", rebuiltTaps);
    }
    // A second channel means the macro was recorded on a two-player level, and
    // the button driving it is a guess. Say so out loud: if player two does
    // nothing, this line is the first thing to look at, and the fix is the P2
    // button setting rather than anything about the file.
    if (p2Clicks > 0) {
        skkbot2::log::warn(
            "[AST] two-player macro: {} press(es) on the second channel, assuming P2 drives "
            "button {} ({}). The format does not record it - if player two does not move, "
            "change P2 Button in the Astral settings and re-import.",
            p2Clicks, p2Button, buttonName(p2Button));
    }

    // A record's frame is shared between the two players, so the action list
    // has to be ordered by it before the engine reads it.
    std::stable_sort(out.actions.begin(), out.actions.end(),
                     [](const skc::Action& a, const skc::Action& b) {
                         if (a.frame != b.frame) return a.frame < b.frame;
                         return a.player2 < b.player2;
                     });

    // tps lives in the header and the converter reports the same field. Take
    // it as it is: one of our real macros, as_world, is recorded at 2305 tps
    // and its own .txt export opens with that number, so a high rate is a
    // legitimate recording rate and not a broken header. Anything the engine
    // cannot run at is the engine's problem to report, not ours to rewrite
    // silently.
    //
    // No caution.clear() here: the second-channel note above is still true.
    out.meta.tps = decoded.tps;

    return true;
}

bool importFile(const std::filesystem::path& path, skc::Macro& out,
                Replay* outInfo, std::string& outReason) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes, outReason)) return false;

    Replay replay;
    if (!parse(bytes, replay, outReason)) return false;

    out = skc::Macro{};
    out.header.tps = static_cast<float>(replay.meta.tps);
    out.actions = std::move(replay.actions);
    out.sortActions();

    if (outInfo) *outInfo = std::move(replay);
    return true;
}

}  // namespace skk::ast
