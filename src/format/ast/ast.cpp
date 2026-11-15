#include "ast.hpp"

#include "ast_wasm.hpp"
#include "format/slc/slc.hpp"


#include <zstd.h>

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace skk::ast {

namespace {

constexpr std::uint8_t kFallbackPlayer2Button = 1;

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
constexpr std::size_t kTpsGuard = 1u << 20;

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

}

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
    pos += 5;

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

    wasm::Output decoded;
    if (!wasm::convert(data, decoded, outReason)) return false;

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

    const uint8_t p2Button = kFallbackPlayer2Button;

    out.actions.clear();
    out.actions.reserve(decoded.clicks.size() * 2);

    bool held1 = false;
    bool held2 = false;
    int rebuiltTaps = 0;
    int p2Clicks = 0;
    for (const auto& click : decoded.clicks) {
        if (click.p1 != wasm::Kind::Skip) {
            const bool down = click.p1 == wasm::Kind::Click;
            if (down && held1) {
                out.actions.push_back(skc::Action{click.frame, 1, false, false});
                ++rebuiltTaps;
            }
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
    if (rebuiltTaps > 0) {
        skkbot2::log::info(
            "[AST] rebuilt {} tap(s) the converter collapsed into one press", rebuiltTaps);
    }
    if (p2Clicks > 0) {
        skkbot2::log::warn(
            "[AST] two-player macro: {} press(es) on the second channel, assuming P2 drives "
            "button {} ({}). The format does not record it - if player two does not move, "
            "change P2 Button in the Astral settings and re-import.",
            p2Clicks, p2Button, buttonName(p2Button));
    }

    std::stable_sort(out.actions.begin(), out.actions.end(),
                     [](const skc::Action& a, const skc::Action& b) {
                         if (a.frame != b.frame) return a.frame < b.frame;
                         return a.player2 < b.player2;
                     });

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
    out.header.tps = replay.meta.tps;
    out.actions = std::move(replay.actions);
    out.sortActions();

    if (outInfo) *outInfo = std::move(replay);
    return true;
}

}
