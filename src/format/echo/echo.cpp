#include "echo.hpp"
#include "utils.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

constexpr size_t kModelSize = 0x486D28;
constexpr size_t kHashSlots = 1u << 18;  // 2^18 u32, 1 MiB - sub_1800F6170:451-461
constexpr size_t kBaseSize = 4096;

// The decoder is allowed to read past the payload: the writer trims trailing
// zero bytes and the reference only raises its error once the position passes
// size + 64 (sub_1800F3530:68).
constexpr size_t kGrace = 64;

constexpr uint32_t kCounterInit = 0x80000000u;

// dword_180661510, the 33 meaningful entries before the surrounding data stops
// being the table. Monotonic, saturating at 4094; index 33 onwards is unrelated.
// Truncating this to the first 24 is what capped a macro at 22 correct records.
constexpr std::array<int32_t, 33> kFibo = {
    1,    2,    3,    6,    10,   16,   27,   45,   73,   120,  194,
    310,  488,  747,  1101, 1546, 2047, 2549, 2994, 3348, 3607, 3785,
    3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094};

constexpr std::array<int32_t, 4> kWeightInit = {21845, 21845, 21845, 0};

struct InitRegion {
    size_t start, end, stride, slots;
};

// From the model constructor, sub_1800F6170. Every eight-byte slot is
// {u32 counter, u16 move counter}.
constexpr std::array<InitRegion, 7> kRegions = {{
    {0, 1920, 48, 6},
    {1920, 7680, 80, 10},
    {7680, 8160, 8, 1},
    {8160, 47040, 80, 10},
    {47120, 47792, 48, 6},
    {47792, 48432, 40, 5},
    {48464, 101200, 32, 4},
}};

// The two 13184-stride arrays the delta coder walks, 101200 through 4320080.
constexpr size_t kStrideFrom = 101200;
constexpr size_t kStrideTo = 4320080;
constexpr size_t kStrideBlock = 13184;

constexpr std::array<std::pair<size_t, size_t>, 3> kWeightVectors = {{
    {47040, 5},
    {48432, 2},
    {4320080, 12},
}};

// sub_1800F74A0:127 - piecewise interpolation over the 128-wide breakpoints.
int32_t curve(int32_t v) {
    const int32_t seg = v >> 7;
    const int32_t frac = v & 0x7F;
    const int32_t a = kFibo[static_cast<size_t>(seg) + 16];
    const int32_t b = kFibo[static_cast<size_t>(seg) + 17];
    return (a * (128 - frac) + b * frac + 64) >> 7;
}

std::array<int16_t, kBaseSize> buildBase() {
    std::array<int16_t, kBaseSize> base{};
    size_t cursor = 0;
    for (int32_t i = -2047; i <= 2047 && cursor < kBaseSize; ++i) {
        size_t top = static_cast<size_t>(curve(i) + 1);
        if (cursor > top) top = cursor + 1;
        for (size_t j = cursor; j < top && j < kBaseSize; ++j) {
            base[j] = static_cast<int16_t>(i);
        }
        cursor = top;
    }
    // sub_1800F7EC0:85-86 patches the end of the ramp to the final index.
    if (cursor < kBaseSize) base[kBaseSize - 1] = 2047;
    return base;
}

const std::array<int16_t, kBaseSize>& baseTable() {
    static const std::array<int16_t, kBaseSize> table = buildBase();
    return table;
}

class Model {
public:
    Model() : m_buf(kModelSize, 0) {
        for (const auto& r : kRegions) {
            for (size_t cell = r.start; cell < r.end; cell += r.stride) {
                for (size_t s = 0; s < r.slots; ++s) {
                    setCounter(cell + s * 8, kCounterInit);
                }
            }
        }
        for (size_t region = kStrideFrom; region < kStrideTo; region += kStrideBlock) {
            for (size_t off = region; off < region + kStrideBlock; off += 8) {
                setCounter(off, kCounterInit);
            }
        }
        for (const auto& [base, count] : kWeightVectors) {
            for (size_t k = 0; k < count; ++k) {
                const size_t off = base + k * 16;
                for (size_t lane = 0; lane < 4; ++lane) {
                    writeI32(off + lane * 4, kWeightInit[lane]);
                }
            }
        }
    }

    uint32_t counter(size_t off) const { return readU32(off); }

    void setCounter(size_t off, uint32_t v) {
        if (off + 4 <= m_buf.size()) std::memcpy(m_buf.data() + off, &v, 4);
    }

    uint16_t prob(size_t off) const {
        uint16_t v = 0;
        if (off + 6 <= m_buf.size()) std::memcpy(&v, m_buf.data() + off + 4, 2);
        return v;
    }

    void setProb(size_t off, uint16_t v) {
        if (off + 6 <= m_buf.size()) std::memcpy(m_buf.data() + off + 4, &v, 2);
    }

    int32_t weight(size_t off, size_t lane) const { return readI32(off + lane * 4); }

    void setWeights(size_t off, const std::array<int32_t, 4>& w) {
        for (size_t lane = 0; lane < 4; ++lane) writeI32(off + lane * 4, w[lane]);
    }

    // sub_1800F74A0:120
    int32_t raw(const std::array<int16_t, 3>& b, const std::array<int32_t, 4>& w) const {
        const int64_t sum = static_cast<int64_t>(b[0]) * w[0] +
                            static_cast<int64_t>(b[1]) * w[1] +
                            static_cast<int64_t>(b[2]) * w[2] +
                            (static_cast<int64_t>(w[3]) << 8);
        return static_cast<int32_t>(static_cast<uint64_t>(sum) >> 16);
    }

private:
    uint32_t readU32(size_t off) const {
        uint32_t v = 0;
        if (off + 4 <= m_buf.size()) std::memcpy(&v, m_buf.data() + off, 4);
        return v;
    }
    int32_t readI32(size_t off) const {
        int32_t v = 0;
        if (off + 4 <= m_buf.size()) std::memcpy(&v, m_buf.data() + off, 4);
        return v;
    }
    void writeI32(size_t off, int32_t v) {
        if (off + 4 <= m_buf.size()) std::memcpy(m_buf.data() + off, &v, 4);
    }

    std::vector<uint8_t> m_buf;
};

// sub_1800F74A0:121-136. Note the floor is 1, not 2: the decompilation is
// `v18 = 1; if (v17 >= 2) v18 = v17;`, a clamp to [1, 4095].
int32_t probability(int32_t raw) {
    if (raw > 2047) return 4095;
    if (raw <= -2047) return 1;
    int32_t v = curve(raw);
    if (v < 1) v = 1;
    return v >= 4095 ? 4095 : v;
}

class RangeDec {
public:
    explicit RangeDec(std::span<const uint8_t> bytes) : m_bytes(bytes) {
        m_size = bytes.size();
        m_range = 0xFFFFFFFFu;
        m_pos = 4;
        for (size_t i = 0; i < 4 && i < m_size; ++i) {
            m_code = (m_code << 8) | bytes[i];
        }
    }

    bool decodeBit(uint32_t bound, uint32_t& outBit) {
        if (bound > m_range) bound = m_range;
        if (m_code >= bound) {
            m_code -= bound;
            m_range -= bound;
            outBit = 1;
        } else {
            m_range = bound;
            outBit = 0;
        }
        // sub_1800F3530:54-75 - the repeat test is on the range BEFORE the
        // shift, and it REPEATS while that is still below 0x10000.
        while (m_range <= 0xFFFFFF) {
            const uint32_t before = m_range;
            m_range <<= 8;
            m_code = (m_code << 8) & 0xFFFFFFFFu;
            if (m_pos < m_size) {
                m_code |= m_bytes[m_pos];
            } else if (m_pos > m_size + kGrace) {
                m_failed = true;
            }
            ++m_pos;
            if (before >= 0x10000) break;
        }
        return !m_failed;
    }

    // The gamma and integer paths split the range exactly in half, quantised.
    bool bit(uint32_t& out) { return decodeBit((m_range >> 1) & 0x7FFF8000u, out); }

    bool bits(int count, uint32_t& out) {
        out = 0;
        for (int i = count; i > 0; --i) {
            uint32_t b = 0;
            if (!bit(b)) return false;
            out |= b << (i - 1);
        }
        return true;
    }

    [[nodiscard]] bool failed() const { return m_failed; }

    // The current window, needed by both model updates to scale their bound.
    [[nodiscard]] uint32_t rangeValue() const { return m_range; }

private:
    std::span<const uint8_t> m_bytes;
    size_t m_size = 0;
    size_t m_pos = 0;
    uint32_t m_range = 0;
    uint32_t m_code = 0;
    bool m_failed = false;
};

bool readGamma(RangeDec& st, uint64_t& out) {
    uint32_t n = 0;
    if (!st.bits(7, n)) return false;
    if (n >= 65) return false;
    if (n <= 1) {
        out = 1;
        return true;
    }
    const uint32_t e = n - 1;
    uint32_t rest = 0;
    if (!st.bits(static_cast<int>(e), rest)) return false;
    out = rest | (1ull << e);
    return true;
}

bool readInteger(RangeDec& st, int width, uint64_t& out) {
    const int lead = (width == 64) ? 7 : 6;
    uint32_t pfx = 0;
    if (!st.bits(lead, pfx)) return false;
    if (width <= static_cast<int>(pfx)) return false;
    const int rest = width - static_cast<int>(pfx) - 1;
    if (rest <= 0) {
        out = 1;
        return true;
    }
    uint32_t body = 0;
    if (!st.bits(rest, body)) return false;
    out = (static_cast<uint64_t>(body) * 2 + 1) << pfx;
    return true;
}

// sub_1800F74A0. The cap is `<= 0x1d` on all three contexts: the original spells
// it `< 0x1e` on the first and `<= 0x1d` on the other two, and those are the
// same bound. Reading the second as `<` caps at 29 and shifts every probability.
bool svmBit(RangeDec& st, Model& model, const std::array<size_t, 3>& ctxs,
            size_t weightOff, int scale) {
    std::array<int32_t, 4> w{};
    for (size_t lane = 0; lane < 4; ++lane) w[lane] = model.weight(weightOff, lane);

    const auto& base = baseTable();
    std::array<int16_t, 3> b{};
    for (size_t k = 0; k < 3; ++k) {
        b[k] = base[model.counter(ctxs[k]) >> 20];
    }

    const int32_t p = probability(model.raw(b, w));
    const uint32_t bound =
        static_cast<uint32_t>(0x10000 - 16 * static_cast<int32_t>(p)) *
        (st.rangeValue() >> 16);

    uint32_t bit = 0;
    st.decodeBit(bound, bit);

    for (size_t k = 0; k < 3; ++k) {
        const size_t off = ctxs[k];
        const uint32_t cur = model.counter(off);
        const uint16_t mv = model.prob(off);
        // v29 is 0xffffffff on a one and zero on a zero, and the divide is
        // chosen by the high half of the 64-bit difference - so a one divides
        // the UNSIGNED 4294967295 - cur, not -1.
        uint32_t inc = 0;
        if (bit) {
            inc = static_cast<uint32_t>(0xFFFFFFFFu - cur) / (mv + 2);
        } else if (cur != 0) {
            inc = static_cast<uint32_t>(-(static_cast<int64_t>(cur) / (mv + 2)));
        }
        model.setCounter(off, cur + inc);
        if (mv <= 0x1D) model.setProb(off, static_cast<uint16_t>(mv + 1));
    }

    const int32_t d = scale * ((bit ? 4096 : 0) - p);
    std::array<int32_t, 4> nw{};
    for (size_t k = 0; k < 3; ++k) {
        // The SSE form is _mm_cvtsi32_si128(d * (unsigned)base) then
        // _mm_srai_epi32(..., 10): the product wraps to 32 bits BEFORE the
        // shift, which is not the same as shifting the 64-bit product.
        const uint32_t prod = static_cast<uint32_t>(d) * static_cast<uint32_t>(b[k]);
        const int32_t shifted = static_cast<int32_t>(prod) >> 10;
        nw[k] = static_cast<int32_t>(static_cast<uint32_t>(w[k]) +
                                      static_cast<uint32_t>(shifted));
    }
    nw[3] = static_cast<int32_t>(static_cast<uint32_t>(w[3]) +
                                 static_cast<uint32_t>(d >> 2));
    model.setWeights(weightOff, nw);
    return bit != 0;
}

// The dictionary repeat bit. There is no separate probability field: the
// adaptive update writes a full u32 at entry+0, so the u16 the coder reads at
// +2 is the high half of that same word, seeded to 0x80000000 => P(hit) = 0.5.
bool counterBit(RangeDec& st, Model& model, size_t off) {
    const uint32_t w = model.counter(off);
    const uint32_t p16 = (w >> 16) & 0xFFFFu;
    const uint16_t mv = model.prob(off);

    const uint32_t bound =
        static_cast<uint32_t>(0x10000 - ((p16 == 0 ? 1u : 0u) + p16)) *
        (st.rangeValue() >> 16);
    uint32_t bit = 0;
    st.decodeBit(bound, bit);

    uint32_t inc = 0;
    if (bit) {
        inc = static_cast<uint32_t>(0xFFFFFFFFu - w) / (mv + 2);
    } else if (w != 0) {
        inc = static_cast<uint32_t>(-(static_cast<int64_t>(w) / (mv + 2)));
    }
    model.setCounter(off, w + inc);
    if (mv <= 0xF9F) model.setProb(off, static_cast<uint16_t>(mv + 1));
    return bit != 0;
}

// sub_1800F7910. a6 is an encoder-side hint and is genuinely dead here.
uint32_t decodeDelta(RangeDec& st, Model& model, uint32_t a3, uint32_t a4, uint32_t a5) {
    const size_t b0 = 13184u * a3 + 48464;
    const size_t b1 = 13184u * (40u * a3 + a4) + 101200;
    const size_t b2 = 13184u * (a5 + 40u * a3) + 2210640;
    const size_t wMain = 4320080 + 48 * a3;

    uint32_t idx = 1;
    for (int i = 0; i < 6; ++i) {
        const std::array<size_t, 3> ctxs{b0 + 8 * idx, b1 + 8 * idx, b2 + 8 * idx};
        const bool b = svmBit(st, model, ctxs, wMain, 1);
        idx = (i == 0) ? ((b ? 1u : 0u) | 2u) : (2u * idx + (b ? 1u : 0u));
    }

    if (idx >= 97) return 0;
    const uint32_t length = idx - 64;
    if (length <= 1) return length;

    const size_t wA = 4320096 + 48 * a3;
    const size_t wB = 4320112 + 48 * a3;
    const size_t baseA = 48464 + 13184u * a3;
    const size_t baseB = 101200 + 13184u * (40u * a3 + a4);
    const size_t baseC = 2210640 + 13184u * (a5 + 40u * a3);

    const uint32_t v30 = (length - 2) + 32 * idx - 1472;
    size_t tailA = 48592 + 13184u * a3 + 8u * v30;
    size_t tailB = 101328 + 13184u * (40u * a3 + a4) + 8u * v30;
    size_t tailC = 2210768 + 13184u * (a5 + 40u * a3) + 8u * v30;
    const uint32_t span = 16 * length + 64;

    uint32_t value = 1;
    uint32_t prev = 1;
    for (uint32_t step = 0; step + 1 < length; ++step) {
        bool bit;
        uint32_t next;
        if (step > 3) {
            // v33 is an `unsigned int *`, so the source's `v33 -= 2` is eight
            // bytes; all three tails step by eight.
            bit = svmBit(st, model, {tailA, tailB, tailC}, wB, 1);
            next = prev;
        } else {
            const uint32_t k = prev + span;
            bit = svmBit(st, model,
                         {baseA + 8u * k, baseB + 8u * k, baseC + 8u * k}, wA, 1);
            next = (bit ? 1u : 0u) | (2u * prev);
        }
        value = 2 * value + (bit ? 1u : 0u);
        tailA -= 8;
        tailB -= 8;
        tailC -= 8;
        prev = next;
    }
    return value;
}

uint32_t bucketOf(uint64_t x) {
    if (x < 4) return static_cast<uint32_t>(x);
    if (x >= 0x100000u) return 39;
    // Index of the highest set bit. x is at least 4 here, and the 0x100000
    // early-out caps the walk at twenty iterations, so a plain loop beats
    // depending on a compiler intrinsic.
    int b = 0;
    while ((x >> (b + 1)) != 0) ++b;
    return static_cast<uint32_t>(((x >> (b - 1)) & 1) + 2u * static_cast<uint64_t>(b));
}

// sub_1800F36E0:396-406. The golden-ratio multiplier appears only on the
// innermost round; the other three use 0x61C8864680B583EB, which is exactly
// minus that multiplier modulo 2^64.
uint32_t hash4(const std::vector<uint64_t>& dct) {
    constexpr uint64_t A = 0x632BE59BD9B4E019ull;
    constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
    constexpr uint64_t B = 0x61C8864680B583EBull;
    constexpr uint64_t D = 0x9517D2453DB37474ull;
    const size_t n = dct.size();
    uint64_t t = K * (dct[n - 4] ^ D) + A;
    t = A - B * (dct[n - 3] ^ t);
    t = A - B * (dct[n - 2] ^ t);
    t = A - B * (dct[n - 1] ^ t);
    return static_cast<uint32_t>(t >> 46) & (kHashSlots - 1);
}

// Measured on RAUCHKAMMER 2: runs at zero, dies at minus one. The hook ordering
// suggests the opposite and is wrong - the header explains why, and why the
// same reasoning was right for TTRL and RE4 but not here.
static constexpr int64_t kFrameShift = 0;

std::size_t channelIndex(uint8_t button, bool player2) {
    const int base = (button >= 1 && button <= 3) ? button - 1 : 0;
    return static_cast<std::size_t>(base) + (player2 ? 3u : 0u);
}

}  // namespace

bool skk::echo::looksLikeEcho(std::span<const uint8_t> data) {
    return data.size() >= kMagicSize && std::memcmp(data.data(), "ECH2", 4) == 0;
}

bool skk::echo::parse(std::span<const uint8_t> data, Replay& out,
                      std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!looksLikeEcho(data)) return fail("echo: not an ECH2 file (magic \"ECH2\")");

    out = Replay{};
    out.version = data[4];
    out.flags = data[5];

    if (out.version < kMinVersion || out.version > kMaxVersion) {
        return fail("echo: unsupported version " + std::to_string(out.version) +
                    ", this reader handles 3 and 4");
    }

    RangeDec st(data.subspan(kMagicSize));

    // NOTE: nothing is read before the three scalars. The rates table is NOT at
    // the head - Echo.cpp:936-960 (load_version3, the reader side) reads
    // fps / end_x / seconds / attempts / input count / clicks / [poses] /
    // [rates] / [seed] in exactly that order, and write_echo (Echo.cpp:990-1002)
    // mirrors it. An earlier version of this reader pulled the rates table out
    // of the head of the stream, which could only ever have worked on the v3
    // files we happened to test with: v3 is written exactly when rates is empty
    // (Echo.cpp:1009), so the branch never ran. It is decoded at the tail now,
    // after the input loop.

    // The three scalars are floats and a double, but they travel through the
    // integer coder as their raw bit patterns. Verified bit-exact against the
    // bot's JSON export on all three reference files.
    uint64_t tpsBits = 0, endXBits = 0, secondsBits = 0;
    if (!readInteger(st, 32, tpsBits) || !readInteger(st, 32, endXBits) ||
        !readInteger(st, 64, secondsBits)) {
        return fail("echo: truncated in the header");
    }
    std::memcpy(&out.tps, &tpsBits, 4);
    std::memcpy(&out.endX, &endXBits, 4);
    std::memcpy(&out.recordingSeconds, &secondsBits, 8);

    if (!std::isfinite(out.tps) || out.tps <= 1.0f) {
        return fail("echo: unusable fps " + std::to_string(out.tps));
    }

    uint64_t attempts = 0, count = 0;
    if (!readGamma(st, attempts)) return fail("echo: truncated at attempts");
    if (!readGamma(st, count)) return fail("echo: truncated at the input count");
    out.attempts = attempts;

    if (count == 0) return fail("echo: the file declares zero inputs");
    if (count > kMaxActions) {
        return fail("echo: input count " + std::to_string(count) + " exceeds " +
                    std::to_string(kMaxActions));
    }

    Model model;
    std::vector<uint32_t> table(kHashSlots, 0xFFFFFFFFu);
    std::vector<uint64_t> dct;
    dct.reserve(static_cast<size_t>(count));

    std::array<int32_t, 6> hist{};
    std::array<uint32_t, 32> prevDelta{};
    uint64_t frame = 0;
    uint32_t run = 0;
    size_t cursor = 0;
    uint32_t misses = 0;

    for (uint64_t i = 0; i < count; ++i) {
        uint64_t packed = 0;

        if (run != 0 && cursor < dct.size()) {
            packed = dct[cursor];
            const uint64_t top = packed >> 4;
            const uint32_t cls = (top == 0) ? 0u : (top == 1 ? 2u : 4u);
            const uint32_t v14 = (misses <= 0) ? misses : 1u;
            const uint32_t off = 48 * bucketOf(run) + 8 * (v14 + cls);
            if (counterBit(st, model, off)) {
                misses = 0;
            } else {
                ++misses;
                packed = 0;
            }
        }

        if (packed == 0) {
            uint32_t second = 0, last = 0;
            uint64_t lastDelta = 0;
            if (!dct.empty()) {
                // The >= 9 byte test gates ONLY `second`. With a single 8-byte
                // entry the reference still knows the last nibble and the last
                // delta, and those select the context arrays.
                if (dct.size() * 8 >= 9) second = static_cast<uint32_t>(dct[dct.size() - 2] & 0xF);
                last = static_cast<uint32_t>(dct.back() & 0xF);
                lastDelta = dct.back() >> 4;
            }

            const size_t p38 = 1920 + 40 * (second + 12 * last);
            const size_t p39 = 7680 + 40 * last;
            const size_t p40 = 8160 + 40 * (last + 12 * (hist[0] + 3 * hist[1] +
                                                          9 * hist[3] + 27 * hist[4]));

            const uint32_t v42 = svmBit(st, model, {p38, p39, p40}, 47040, 2) ? 1u : 0u;
            const uint32_t v46 = svmBit(st, model,
                                        {p38 + 8u * v42 + 8, p39 + 8u * v42 + 8,
                                         p40 + 8u * v42 + 8},
                                        47040 + 16 * (v42 + 1), 2)
                                     ? 1u
                                     : 0u;
            uint32_t v47 = 0;
            if (v46 == 0) {
                v47 = svmBit(st, model,
                             {p38 + 8u * v42 + 24, p39 + 8u * v42 + 24, p40 + 8u * v42 + 24},
                             47040 + 16 * (v42 + 3), 2)
                         ? 1u
                         : 0u;
            }
            const uint32_t v48 = 3 * v42 + (v47 | (2 * v46));
            const uint32_t v49 = (v48 == (last >> 1)) ? 1u : 0u;
            const uint32_t v50 = static_cast<uint32_t>(hist[v48]);
            const uint32_t v51 = v49 | (2 * (last & 1) + 4 * v50);

            const uint32_t v53 = lastDelta < 5 ? static_cast<uint32_t>(lastDelta) : 5u;
            const uint32_t v55 = (v50 == 1) ? 40u : 0u;
            const uint32_t v56 = bucketOf(lastDelta);

            const uint32_t v58 = svmBit(st, model,
                                        {47120 + 8 * (v53 + 6 * v51), 47696 + 8 * v51,
                                         47792 + 8 * v56 + 8 * v55},
                                        48432 + 16 * v49, 2)
                                     ? 1u
                                     : 0u;

            uint32_t a4 = prevDelta[2 * v48 + v58];
            if (a4 > 3) a4 = bucketOf(a4);
            uint32_t a5 = static_cast<uint32_t>(lastDelta);
            if (a5 > 3) a5 = bucketOf(a5);

            const uint32_t delta = decodeDelta(st, model, v49 + 2 * v58, a4, a5);
            packed = (16ull * delta) | (2ull * v48) | v58;

            if (st.failed()) return fail("echo: the input stream ran out at record " +
                                         std::to_string(i));
        }

        const uint32_t field = static_cast<uint32_t>((packed >> 1) & 7);
        if (field >= 6) {
            return fail("echo: invalid state " + std::to_string(field) + " at record " +
                        std::to_string(i) + " - the model has desynchronised");
        }
        const uint32_t delta = static_cast<uint32_t>(packed >> 4);
        frame += delta;
        if (frame > 0xFFFFFFFFull) {
            return fail("echo: frame counter overflowed past 2^32 at record " +
                        std::to_string(i));
        }

        const uint32_t btnIndex = field >= 3 ? field - 3 : field;
        uint8_t button = 1;
        if (btnIndex == 1) button = 2;
        else if (btnIndex == 2) button = 3;
        const bool player2 = field >= 3;
        const bool pressed = (packed & 1) != 0;
        out.actions.push_back(Action{static_cast<uint32_t>(frame), button, pressed, player2});

        if (run != 0 && cursor < dct.size() && dct[cursor] == packed) {
            ++run;
            ++cursor;
        } else {
            run = 0;
        }

        dct.push_back(packed);

        if (dct.size() >= 4) {
            const uint32_t h = hash4(dct);
            if (run == 0) {
                const uint32_t slot = table[h];
                if ((slot & 0x80000000u) == 0) {
                    const uint32_t limit = std::min<uint32_t>(slot, 64);
                    uint32_t len = 0;
                    for (uint32_t k = 0; k < limit; ++k) {
                        if (dct[slot - 1 - k] != dct[dct.size() - 1 - k]) break;
                        len = k + 1;
                    }
                    if (len < 4) {
                        run = 0;  // the cursor is deliberately left alone
                    } else {
                        run = len;
                        cursor = slot;
                    }
                }
            }
            // Unconditional: the slot is overwritten even when the search never
            // ran, because this is a flat direct-mapped table with no probing.
            table[h] = static_cast<uint32_t>(dct.size());
        }

        hist[field] = 2 - static_cast<int32_t>(packed & 1);
        prevDelta[packed & 0xF] = delta;
    }

    // ---- the tail, in the writer's order (Echo.cpp:996-1002, reader :947-959) ----
    //
    // clicks -> [poses] -> [rates, v4 only] -> [seed]. Everything the bot needs
    // from this file is already in out.actions by this point, so the tail is
    // enrichment, not a requirement.
    //
    // Poses are skipped, not decoded. They are written only when the author had
    // `save_debug` on (Echo.cpp:986) and they ride a SECOND adaptive model -
    // POSE_FIELDS x 2 independent guess sets, delta + zigzag + code_number
    // (code_poses, Echo.cpp:358-375). Reconstructing that model purely to throw
    // the values away is not worth it, and the position data is useless to an
    // input-only bot anyway. The cost is that rates and seed cannot be reached
    // on such a file, which is why that case returns a partial success instead
    // of pretending the tail was empty.
    bool tailComplete = true;
    const char* tailReason = nullptr;

    if (out.flags & kFlagSamples) {
        tailComplete = false;
        tailReason = "carries position samples; rates and seed were not read";
    } else if (out.version == 4) {
        // Rates are DELTA coded (code_rates, Echo.cpp:589-610): a gamma gap from
        // the previous entry (the first is from 0), then the 32-bit float
        // pattern XORed against the previous entry's pattern. Reading them as
        // absolute frame / absolute bits - as this reader used to - yields
        // garbage on the very first entry.
        uint64_t count = 0;
        if (!readGamma(st, count)) {
            tailComplete = false;
        } else if (count > kMaxRateChanges) {
            tailComplete = false;
        } else {
            uint64_t last = 0;
            uint64_t prevBits = 0;
            bool ok = true;
            for (uint64_t i = 0; i < count && ok; ++i) {
                uint64_t gap = 0, bits = 0;
                if (!readGamma(st, gap) || !readInteger(st, 32, bits)) { ok = false; break; }
                bits ^= prevBits;
                float fps = 0.f;
                std::memcpy(&fps, &bits, 4);
                if (!std::isfinite(fps) || fps < 1.0f) { ok = false; break; }
                last += gap;
                if (last > 0xFFFFFFFFull) { ok = false; break; }
                out.rates.push_back(Rate{static_cast<uint32_t>(last), fps});
                prevBits = bits;
            }
            if (!ok) out.rates.clear();
            tailComplete = ok;
            if (!ok && !tailReason) tailReason = "the fps-change table did not decode cleanly";
        }
    }

    if (tailComplete && (out.flags & kFlagSeed)) {
        uint64_t seedBits = 0;
        if (readInteger(st, 64, seedBits) && !st.failed()) {
            out.seed = seedBits;
            out.hasSeed = true;
        } else {
            tailComplete = false;
            if (!tailReason) tailReason = "the level seed did not decode cleanly";
        }
    }

    // Metadata flags we act on by not understanding them. Each one changes what
    // the macro expects from the run, so saying so is better than replaying it
    // as if it were vanilla.
    std::vector<std::string> notes;
    if (out.flags & kFlagUnrounded) notes.push_back("recorded with velocity unrounding");
    if (out.flags & kFlagSilicateVariance) notes.push_back("expects Silicate variance numbering");
    if (out.flags & kFlagLdmKnown) {
        notes.push_back((out.flags & kFlagLdm) ? "expects low detail mode"
                                               : "expects high detail mode");
    }
    // The table itself is read and kept in Replay::rates, but SKC has one tps
    // per macro, so toSkcMacro can only honour the starting rate. Say so here:
    // the loss is otherwise silent and the macro drifts from the first change.
    if (!out.rates.empty())
        notes.push_back(
            "carries an fps-change table; only the starting rate is kept");
    if (!tailComplete && tailReason) notes.push_back(tailReason);
    if (!notes.empty()) {
        std::string caution = "Echo: ";
        for (size_t i = 0; i < notes.size(); ++i) {
            if (i) caution += "; ";
            caution += notes[i];
        }
        caution += ". Imported as input-only.";
        out.caution = std::move(caution);
    } else {
        out.caution = "Echo: input-only, no position data";
    }

    if (out.actions.empty()) return fail("echo: no inputs decoded");

    skkbot2::log::info(
        "[ECHO] v{} fps {:.4g}, {} attempt(s), {} input(s), {} action(s){}", out.version,
        out.tps, out.attempts, count, out.actions.size(),
        out.rates.empty() ? "" : ", fps change table present");
    return true;
}

bool skk::echo::toSkcMacro(const Replay& replay, skc::Macro& out,
                           std::string& outReason) {
    auto fail = [&](std::string why) {
        outReason = std::move(why);
        return false;
    };

    if (!std::isfinite(replay.tps) || replay.tps <= 1.0f) {
        return fail("echo: unusable fps " + std::to_string(replay.tps));
    }

    out = skc::Macro{};
    out.header.tps = replay.tps;
    out.actions.reserve(replay.actions.size());

    std::array<bool, 6> held{};
    std::size_t droppedRelease = 0, droppedDuplicate = 0;

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
        // Zero, measured. The hook body has Echo apply inputs after the vanilla
        // processCommands, which read like GDH's RE4 and predicted -1; the game
        // says otherwise, so the measurement wins.
        const int64_t shifted = static_cast<int64_t>(a.frame) + kFrameShift;
        if (shifted < 0 || shifted > 0xFFFFFFFFll) {
            return fail("echo: frame " + std::to_string(a.frame) + " shifted by " +
                        std::to_string(kFrameShift) + " is out of range");
        }
        out.actions.push_back(
            skc::Action{static_cast<uint32_t>(shifted), a.button, a.pressed, a.player2});
    }

    if (out.actions.empty()) return fail("echo: no usable inputs after normalisation");

    std::string reason;
    if (!skc::validateActions(out.actions, reason)) return fail("echo: " + reason);

    skkbot2::log::warn(
        "[ECHO] {} inputs -> {} actions, dropped {} release(s) with no press and "
        "{} duplicate press(es), frame shift {:+d} (first click {} -> {})",
        replay.actions.size(), out.actions.size(), droppedRelease, droppedDuplicate,
        kFrameShift, replay.actions.front().frame, out.actions.front().frame);
    return true;
}

bool skk::echo::importFile(const std::filesystem::path& path, skc::Macro& out,
                           Replay* outInfo, std::string& outReason) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outReason = "echo: cannot open " + skk::pathToUtf8(path);
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        outReason = "echo: file is empty";
        return false;
    }

    Replay parsed;
    if (!parse(bytes, parsed, outReason)) return false;
    if (!toSkcMacro(parsed, out, outReason)) return false;
    if (outInfo) *outInfo = std::move(parsed);
    return true;
}
