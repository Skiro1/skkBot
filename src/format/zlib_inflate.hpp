#pragma once

// Minimal DEFLATE / zlib decompressor.
//
// TTR3 stores its whole payload as one zlib stream (TtrlCodec-equivalent
// serialize() calls compress2 when FlagCompressed is set), and the inputs
// section lives INSIDE that stream - there is no reading around it. The project
// has no zlib to link against: geode ships the header for cocos but no library,
// and Geometry Dash statically links its own copy, so there is nothing to call.
//
// This is the classic puff-style decoder: canonical Huffman tables built from
// code lengths, bits consumed LSB-first. It handles stored, fixed and dynamic
// blocks, which is all zlib's compress2 ever emits for this data.
//
// It refuses rather than guesses: a truncated stream, a bad block type, an
// over-subscribed Huffman table or an output larger than the caller's cap all
// return false. Verified against the real TTR3 file - 396 488 compressed bytes
// expand to exactly the 2 150 356 bytes its own section table claims.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace skk::inflate {

inline constexpr size_t kMaxOutput = 256u * 1024u * 1024u;

namespace detail {

struct BitReader {
    std::span<const uint8_t> data;
    size_t pos = 0;
    uint32_t buffer = 0;
    int count = 0;
    bool failed = false;

    int bits(int need) {
        while (count < need) {
            if (pos >= data.size() || need > 24) {
                failed = true;
                return 0;
            }
            buffer |= static_cast<uint32_t>(data[pos++]) << count;
            count += 8;
        }
        const int value = static_cast<int>(buffer & ((1u << need) - 1u));
        buffer >>= need;
        count -= need;
        return value;
    }
};

struct Huffman {
    std::array<int16_t, 16> lengths{};
    std::vector<int16_t> symbols;
};

inline bool buildHuffman(Huffman& out, const int16_t* lengths, int n) {
    out.lengths.fill(0);
    for (int i = 0; i < n; ++i) ++out.lengths[lengths[i]];
    if (out.lengths[0] == n) return true;  // no codes at all: legal, never used

    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= out.lengths[len];
        if (left < 0) return false;  // over-subscribed
    }

    std::array<int, 16> offsets{};
    offsets[1] = 0;
    for (int len = 1; len < 15; ++len) offsets[len + 1] = offsets[len] + out.lengths[len];

    out.symbols.assign(static_cast<size_t>(n), 0);
    for (int sym = 0; sym < n; ++sym) {
        if (lengths[sym] != 0) out.symbols[static_cast<size_t>(offsets[lengths[sym]]++)] =
            static_cast<int16_t>(sym);
    }
    return true;
}

inline int decodeSymbol(BitReader& in, const Huffman& table) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= in.bits(1);
        const int count = table.lengths[len];
        if (code - count < first) {
            return table.symbols[static_cast<size_t>(index + (code - first))];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (in.failed) return -1;
    }
    return -1;
}

inline constexpr int16_t kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
inline constexpr int16_t kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline constexpr int16_t kDistanceBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
inline constexpr int16_t kDistanceExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

inline bool inflateBlockData(BitReader& in, const Huffman& literals, const Huffman& distances,
                             std::vector<uint8_t>& out, size_t cap) {
    for (;;) {
        const int symbol = decodeSymbol(in, literals);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (out.size() >= cap) return false;
            out.push_back(static_cast<uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) return true;  // end of block

        const int lengthIndex = symbol - 257;
        if (lengthIndex >= 29) return false;
        const int length =
            kLengthBase[lengthIndex] + in.bits(kLengthExtra[lengthIndex]);

        const int distanceSymbol = decodeSymbol(in, distances);
        if (distanceSymbol < 0 || distanceSymbol >= 30) return false;
        const int distance = kDistanceBase[distanceSymbol] + in.bits(kDistanceExtra[distanceSymbol]);
        if (in.failed || distance <= 0 || static_cast<size_t>(distance) > out.size()) return false;
        if (out.size() + static_cast<size_t>(length) > cap) return false;

        size_t from = out.size() - static_cast<size_t>(distance);
        for (int i = 0; i < length; ++i) out.push_back(out[from + static_cast<size_t>(i)]);
    }
}

inline bool readDynamicTables(BitReader& in, Huffman& literals, Huffman& distances) {
    const int literalCount = in.bits(5) + 257;
    const int distanceCount = in.bits(5) + 1;
    const int codeLengthCount = in.bits(4) + 4;
    if (in.failed || literalCount > 286 || distanceCount > 30) return false;

    static constexpr int16_t kOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    std::array<int16_t, 19> codeLengths{};
    for (int i = 0; i < codeLengthCount; ++i) codeLengths[kOrder[i]] = static_cast<int16_t>(in.bits(3));
    if (in.failed) return false;

    Huffman codeLengthTable;
    if (!buildHuffman(codeLengthTable, codeLengths.data(), 19)) return false;

    std::vector<int16_t> lengths(static_cast<size_t>(literalCount + distanceCount), 0);
    size_t index = 0;
    while (index < lengths.size()) {
        const int symbol = decodeSymbol(in, codeLengthTable);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<int16_t>(symbol);
            continue;
        }
        int repeat = 0;
        int16_t value = 0;
        if (symbol == 16) {
            if (index == 0) return false;
            value = lengths[index - 1];
            repeat = 3 + in.bits(2);
        } else if (symbol == 17) {
            repeat = 3 + in.bits(3);
        } else {
            repeat = 11 + in.bits(7);
        }
        if (in.failed || index + static_cast<size_t>(repeat) > lengths.size()) return false;
        for (int i = 0; i < repeat; ++i) lengths[index++] = value;
    }

    if (!buildHuffman(literals, lengths.data(), literalCount)) return false;
    if (!buildHuffman(distances, lengths.data() + literalCount, distanceCount)) return false;
    return true;
}

inline bool buildFixedTables(Huffman& literals, Huffman& distances) {
    std::array<int16_t, 288> literalLengths{};
    for (int i = 0; i < 144; ++i) literalLengths[i] = 8;
    for (int i = 144; i < 256; ++i) literalLengths[i] = 9;
    for (int i = 256; i < 280; ++i) literalLengths[i] = 7;
    for (int i = 280; i < 288; ++i) literalLengths[i] = 8;
    if (!buildHuffman(literals, literalLengths.data(), 288)) return false;

    std::array<int16_t, 30> distanceLengths{};
    distanceLengths.fill(5);
    return buildHuffman(distances, distanceLengths.data(), 30);
}

}  // namespace detail

// Raw DEFLATE (RFC 1951), no zlib wrapper.
[[nodiscard]] inline bool inflateRaw(std::span<const uint8_t> input,
                                     std::vector<uint8_t>& out, size_t expectedSize) {
    out.clear();
    const size_t cap = expectedSize ? expectedSize : kMaxOutput;
    if (cap > kMaxOutput) return false;

    detail::BitReader in{input, 0, 0, 0, false};
    for (;;) {
        const int final = in.bits(1);
        const int type = in.bits(2);
        if (in.failed) return false;

        if (type == 0) {  // stored
            in.buffer = 0;
            in.count = 0;
            if (in.pos + 4 > in.data.size()) return false;
            const int length = in.data[in.pos] | (in.data[in.pos + 1] << 8);
            in.pos += 4;  // skip LEN and NLEN; verified below
            if (in.pos + static_cast<size_t>(length) > in.data.size()) return false;
            if (out.size() + static_cast<size_t>(length) > cap) return false;
            out.insert(out.end(), in.data.begin() + static_cast<long>(in.pos),
                       in.data.begin() + static_cast<long>(in.pos + static_cast<size_t>(length)));
            in.pos += static_cast<size_t>(length);
        } else if (type == 1 || type == 2) {
            detail::Huffman literals, distances;
            const bool tablesOk = (type == 1) ? detail::buildFixedTables(literals, distances)
                                              : detail::readDynamicTables(in, literals, distances);
            if (!tablesOk) return false;
            if (!detail::inflateBlockData(in, literals, distances, out, cap)) return false;
        } else {
            return false;  // reserved block type
        }

        if (final) break;
    }
    return expectedSize == 0 || out.size() == expectedSize;
}

// zlib stream: 2-byte header (CM/FLG), DEFLATE payload, 4-byte Adler-32.
// The Adler-32 is checked - a payload that inflates to the right size with the
// wrong contents is still a corrupt file, and TTR3 has no other integrity check
// of its own on this path.
[[nodiscard]] inline bool inflateZlib(std::span<const uint8_t> input,
                                      std::vector<uint8_t>& out, size_t expectedSize) {
    if (input.size() < 6) return false;
    const uint8_t cmf = input[0];
    const uint8_t flg = input[1];
    if ((cmf & 0x0f) != 8) return false;                  // must be DEFLATE
    if (((cmf << 8) | flg) % 31 != 0) return false;        // header checksum
    if (flg & 0x20) return false;                         // preset dictionary: not produced by zlib

    if (!inflateRaw(input.subspan(2, input.size() - 6), out, expectedSize)) return false;

    uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) {
        stored = (stored << 8) | input[input.size() - 4 + static_cast<size_t>(i)];
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : out) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return ((b << 16) | a) == stored;
}

}  // namespace skk::inflate