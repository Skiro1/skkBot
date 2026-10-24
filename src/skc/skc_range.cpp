#include "skc_range.hpp"

#include <algorithm>

namespace skk::skc {

// ---------------------------------------------------------------------------
// RangeEncoder
// ---------------------------------------------------------------------------

void RangeEncoder::encode(uint32_t cumLow, uint32_t cumHigh, uint32_t total) {
    const uint64_t range = static_cast<uint64_t>(m_high) - m_low;
    m_high = m_low + static_cast<uint32_t>((range * cumHigh) / total - 1);
    m_low = m_low + static_cast<uint32_t>((range * cumLow) / total);

    while (true) {
        if (m_high < 0x80000000u) {
            // Both ends in the lower half: E1, output a zero.
            emitBit(0);
        } else if (m_low >= 0x80000000u) {
            // Both ends in the upper half: E2, output a one and subtract the
            // high bit so the range fits back into 32 bits.
            emitBit(1);
            m_low -= 0x80000000u;
            m_high -= 0x80000000u;
        } else if (m_low >= 0x40000000u && m_high < 0xC0000000u) {
            // Straddling the middle: E3. The bit is undecided, so remember it
            // and narrow around the centre.
            ++m_pending;
            m_low -= 0x40000000u;
            m_high -= 0x40000000u;
        } else {
            break;
        }

        m_low = (m_low << 1) & 0xFFFFFFFFu;
        m_high = ((m_high << 1) | 1u) & 0xFFFFFFFFu;
    }
}

void RangeEncoder::emitBit(uint8_t bit) {
    m_bits.push_back(bit);
    while (m_pending > 0) {
        // A pending E3 bit resolves to the opposite of the bit that finally
        // settled the range.
        m_bits.push_back(bit ^ 1u);
        --m_pending;
    }
}

std::vector<uint8_t> RangeEncoder::finish() {
    // One more bit identifies the current half, which flushes any pending E3
    // stack through emitBit.
    ++m_pending;
    if (m_low < 0x40000000u) emitBit(0);
    else emitBit(1);

    std::vector<uint8_t> out;
    out.reserve((m_bits.size() + 7) / 8);
    for (size_t i = 0; i < m_bits.size(); i += 8) {
        uint8_t byte = 0;
        for (size_t j = 0; j < 8; ++j) {
            byte = static_cast<uint8_t>(byte << 1);
            if (i + j < m_bits.size()) byte |= m_bits[i + j] & 1u;
        }
        out.push_back(byte);
    }
    return out;
}

// ---------------------------------------------------------------------------
// RangeDecoder
// ---------------------------------------------------------------------------

RangeDecoder::RangeDecoder(const uint8_t* data, size_t size)
    : m_data(data), m_size(size) {
    for (int i = 0; i < 32; ++i)
        m_code = (m_code << 1) | readBit();
}

uint32_t RangeDecoder::readBit() {
    if (m_bitPos >= m_size * 8u) {
        // Past the end the stream is implicitly zero-padded; the encoder pads the
        // same way, so the decoder can never need a bit that is not there.
        return 0;
    }
    // The encoder packs bits MSB-first, so the decoder must walk bits, not
    // bytes: bit i lives in byte (i >> 3) at position (7 - (i & 7)).
    const uint8_t byte = m_data[m_bitPos >> 3];
    const uint32_t bit = (byte >> (7u - (m_bitPos & 7u))) & 1u;
    ++m_bitPos;
    return bit;
}

uint32_t RangeDecoder::decodeTarget(uint32_t total) const {
    const uint64_t range = static_cast<uint64_t>(m_high) - m_low;
    return static_cast<uint32_t>(
        ((static_cast<uint64_t>(m_code) - m_low + 1) * total - 1) / range);
}

void RangeDecoder::remove(uint32_t cumLow, uint32_t cumHigh, uint32_t total) {
    const uint64_t range = static_cast<uint64_t>(m_high) - m_low;
    m_high = m_low + static_cast<uint32_t>((range * cumHigh) / total - 1);
    m_low = m_low + static_cast<uint32_t>((range * cumLow) / total);

    while (true) {
        if (m_high < 0x80000000u) {
            // E1: no adjustment.
        } else if (m_low >= 0x80000000u) {
            m_low -= 0x80000000u;
            m_high -= 0x80000000u;
            m_code -= 0x80000000u;
        } else if (m_low >= 0x40000000u && m_high < 0xC0000000u) {
            m_low -= 0x40000000u;
            m_high -= 0x40000000u;
            m_code -= 0x40000000u;
        } else {
            break;
        }

        m_low = (m_low << 1) & 0xFFFFFFFFu;
        m_high = ((m_high << 1) | 1u) & 0xFFFFFFFFu;
        m_code = ((m_code << 1) | readBit()) & 0xFFFFFFFFu;
    }
}

// ---------------------------------------------------------------------------
// SymbolModel
// ---------------------------------------------------------------------------

SymbolModel::SymbolModel(uint32_t symbolCount, uint32_t contextCount,
                         uint32_t countScale, uint32_t rowBudget)
    : m_symbolCount(symbolCount),
      m_contextCount(contextCount),
      m_countScale(countScale),
      m_rowBudget(rowBudget),
      m_counts_(static_cast<size_t>(symbolCount) * contextCount, countScale) {}

void SymbolModel::encode(RangeEncoder& enc, uint32_t symbol, uint32_t ctx) {
    const size_t base = static_cast<size_t>(ctx) * m_symbolCount;
    uint32_t cumLow = 0;
    for (uint32_t s = 0; s < symbol; ++s)
        cumLow += m_counts_[base + s];
    const uint32_t count = m_counts_[base + symbol];

    uint32_t total = 0;
    for (uint32_t s = 0; s < m_symbolCount; ++s)
        total += m_counts_[base + s];

    enc.encode(cumLow, cumLow + count, total);
    update(ctx, symbol);
}

uint32_t SymbolModel::decode(RangeDecoder& dec, uint32_t ctx) {
    const size_t base = static_cast<size_t>(ctx) * m_symbolCount;

    uint32_t total = 0;
    for (uint32_t s = 0; s < m_symbolCount; ++s)
        total += m_counts_[base + s];

    const uint32_t target = dec.decodeTarget(total);

    // Walk the cumulated counts to find the symbol whose interval contains the
    // target. A linear scan is right here: the alphabets are tiny (<=257).
    uint32_t cum = 0;
    uint32_t symbol = 0;
    for (; symbol < m_symbolCount; ++symbol) {
        cum += m_counts_[base + symbol];
        if (target < cum) break;
    }
    if (symbol >= m_symbolCount) symbol = m_symbolCount - 1;

    const uint32_t cumHigh = cum;
    const uint32_t cumLow = cum - m_counts_[base + symbol];
    dec.remove(cumLow, cumHigh, total);
    update(ctx, symbol);
    return symbol;
}

void SymbolModel::update(uint32_t ctx, uint32_t symbol) {
    const size_t base = static_cast<size_t>(ctx) * m_symbolCount;
    m_counts_[base + symbol] += m_countScale;

    uint32_t total = 0;
    for (uint32_t s = 0; s < m_symbolCount; ++s)
        total += m_counts_[base + s];
    if (total <= rescaleThreshold()) return;

    for (uint32_t s = 0; s < m_symbolCount; ++s)
        m_counts_[base + s] = std::max(1u, m_counts_[base + s] / 2u);
}

// ---------------------------------------------------------------------------
// ValueModel — magnitude-split
// ---------------------------------------------------------------------------

ValueModel::ValueModel(uint32_t shift, uint32_t countScale, uint32_t rowBudget, bool mix)
    : m_shift(shift < kShiftMin ? kShiftMin : (shift > kShiftMax ? kShiftMax : shift)),
      m_width(1u << m_shift),
      m_buckets(256u / m_width),
      m_escape(m_width),
      m_symbolsPerBucket(m_width + 1u),
      m_countScale(countScale),
      m_rowBudget(rowBudget),
      m_mixer(mix ? std::optional<BucketMixer>(
                    BucketMixer(m_buckets + 1u, countScale, rowBudget))
                  : std::nullopt),
      m_offsets_(static_cast<size_t>(m_buckets + 1u) * m_symbolsPerBucket, countScale),
      m_buckets_(m_buckets + 1u, countScale) {}

void ValueModel::encode(RangeEncoder& enc, uint32_t value, uint64_t* bucketBits,
                        uint64_t* offsetBits, uint32_t prevValue) {
    const uint32_t mark0 = (bucketBits || offsetBits) ? enc.bitsProduced() : 0;
    const uint32_t bucket = bucketOf(value);
    const uint32_t offset =
        value < 256u ? (value & (m_width - 1u)) : m_escape;

    // 1) which bucket. The mixer is a drop-in for the plain chooser: same one
    // symbol, same cost if it does not help, and it only needs to know which
    // bucket the previous value fell in.
    if (m_mixer) {
        m_mixer->encode(enc, bucket, m_prevBucket);
        m_prevBucket = bucket;
    } else {
        uint32_t cumLow = 0;
        for (uint32_t b = 0; b < bucket; ++b) cumLow += m_buckets_[b];
        uint32_t total = 0;
        for (uint32_t b = 0; b <= m_buckets; ++b) total += m_buckets_[b];
        enc.encode(cumLow, cumLow + m_buckets_[bucket], total);

        if (total >= m_countScale * m_rowBudget) {
            for (uint32_t b = 0; b <= m_buckets; ++b)
                m_buckets_[b] = std::max(1u, m_buckets_[b] / 2u);
        } else {
            m_buckets_[bucket] += m_countScale;
        }
    }
    if (bucketBits) *bucketBits += enc.bitsProduced() - mark0;
    const uint64_t mark1 = (bucketBits || offsetBits) ? enc.bitsProduced() : 0;

    // 2) which offset inside that bucket
    const size_t base = static_cast<size_t>(bucket) * m_symbolsPerBucket;
    uint32_t offLow = 0;
    for (uint32_t o = 0; o < offset; ++o) offLow += m_offsets_[base + o];
    uint32_t offTotal = 0;
    for (uint32_t o = 0; o < m_symbolsPerBucket; ++o) offTotal += m_offsets_[base + o];

    enc.encode(offLow, offLow + m_offsets_[base + offset], offTotal);

    if (offTotal >= m_countScale * m_rowBudget) {
        for (uint32_t o = 0; o < m_symbolsPerBucket; ++o)
            m_offsets_[base + o] = std::max(1u, m_offsets_[base + o] / 2u);
    } else {
        m_offsets_[base + offset] += m_countScale;
    }

    // 3) escape: raw bytes through a uniform alphabet
    if (offset == m_escape) {
        // A value that does not fit in 16 bits needs four bytes, not two.
        // Writing two used to truncate it SILENTLY: at 240 tps a 72000-frame
        // gap between two presses on one channel reloaded with that press 6464
        // frames in, and validateActions still passed because the frames stayed
        // monotonic and the button states stayed legal. The file simply differed
        // from the macro that produced it, which is the one thing this format
        // exists not to do.
        //
        // The wide flag costs one bit on the escape path only, and the escape is
        // the rare path by construction: it is chosen precisely when the value
        // does not fit the bucket/offset model. Per AGENTS the format version is
        // not preserved across codec changes, so files saved before this need to
        // be re-saved; there is no way to shave the bit out of the three knob
        // bytes, they are all spent.
        const bool wide = value >= 0x10000u;
        enc.encode(wide ? 1u : 0u, wide ? 2u : 1u, 2u);
        const uint8_t low = static_cast<uint8_t>(value & 0xFFu);
        const uint8_t high = static_cast<uint8_t>((value >> 8) & 0xFFu);
        enc.encode(low * 256u, low * 256u + 256u, 256u * 256u);
        enc.encode(high * 256u, high * 256u + 256u, 256u * 256u);
        if (wide) {
            const uint8_t low2 = static_cast<uint8_t>((value >> 16) & 0xFFu);
            const uint8_t high2 = static_cast<uint8_t>((value >> 24) & 0xFFu);
            enc.encode(low2 * 256u, low2 * 256u + 256u, 256u * 256u);
            enc.encode(high2 * 256u, high2 * 256u + 256u, 256u * 256u);
        }
    }
    if (offsetBits) *offsetBits += enc.bitsProduced() - mark1;
}

uint32_t ValueModel::decode(RangeDecoder& dec, uint32_t prevValue) {
    // 1) bucket
    uint32_t bucket;
    if (m_mixer) {
        bucket = m_mixer->decode(dec, m_prevBucket);
        m_prevBucket = bucket;
    } else {
        uint32_t total = 0;
        for (uint32_t b = 0; b <= m_buckets; ++b) total += m_buckets_[b];

        const uint32_t target1 = dec.decodeTarget(total);
        uint32_t walk = 0;
        bucket = 0;
        for (; bucket <= m_buckets; ++bucket) {
            walk += m_buckets_[bucket];
            if (target1 < walk) break;
        }
        if (bucket > m_buckets) bucket = m_buckets;

        const uint32_t high1 = walk;
        const uint32_t low1 = walk - m_buckets_[bucket];
        dec.remove(low1, high1, total);

        if (total >= m_countScale * m_rowBudget) {
            for (uint32_t b = 0; b <= m_buckets; ++b)
                m_buckets_[b] = std::max(1u, m_buckets_[b] / 2u);
        } else {
            m_buckets_[bucket] += m_countScale;
        }
    }

    // 2) offset inside the bucket
    const size_t base = static_cast<size_t>(bucket) * m_symbolsPerBucket;
    uint32_t offTotal = 0;
    for (uint32_t o = 0; o < m_symbolsPerBucket; ++o) offTotal += m_offsets_[base + o];

    const uint32_t target = dec.decodeTarget(offTotal);
    uint32_t offset = 0;
    uint32_t cum2 = 0;
    for (; offset < m_symbolsPerBucket; ++offset) {
        cum2 += m_offsets_[base + offset];
        if (target < cum2) break;
    }
    if (offset >= m_symbolsPerBucket) offset = m_symbolsPerBucket - 1;

    const uint32_t offHigh = cum2;
    const uint32_t offLow = cum2 - m_offsets_[base + offset];
    dec.remove(offLow, offHigh, offTotal);

    if (offTotal >= m_countScale * m_rowBudget) {
        for (uint32_t o = 0; o < m_symbolsPerBucket; ++o)
            m_offsets_[base + o] = std::max(1u, m_offsets_[base + o] / 2u);
    } else {
        m_offsets_[base + offset] += m_countScale;
    }

    if (offset != m_escape)
        return (bucket << m_shift) | offset;

    // 3) escape: reverse of the uniform byte encode
    const uint32_t wideTarget = dec.decodeTarget(2);
    dec.remove(wideTarget, wideTarget + 1, 2);
    const bool wide = wideTarget == 1;
    uint32_t value = 0;
    const int passes = wide ? 4 : 2;
    for (int pass = 0; pass < passes; ++pass) {
        const uint32_t t = dec.decodeTarget(256u * 256u);
        const uint32_t slot = t / 256u;
        dec.remove(slot * 256u, slot * 256u + 256u, 256u * 256u);
        value |= slot << (8 * pass);
    }
    return value;
}

} // namespace skk::skc
