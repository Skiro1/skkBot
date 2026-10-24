#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace skk::skc {

// Counts are kept in units of 1/countScale of a whole symbol.
//
// The reason is measured, not theoretical. With integer counts starting at 1
// and halving to a floor of 1, a symbol that occurs once in a row of 33 can
// never cost less than log2(33) bits even if the coder knows it is rare - the
// floor is a sixteenth of a symbol's worth of probability mass, which is what
// LZMA-style probability models buy and plain counts cannot. On GRIEF the
// measured gap between what we spend and the true order-0 entropy of the same
// sequences is 0.79 bits per gap and 0.43 per hold, and all 82 bytes of the
// remaining difference from GRIEF.echo live in that gap.
inline constexpr uint32_t kCountScale = 16u;

// A row's probability mass is renormalised back to roughly this many whole
// symbols' worth of weight once it exceeds countScale * rowBudget.
//
// The threshold matters as much as the scale, and for a surprising reason: with
// the old setting a row never renormalised at all on a real macro - it would
// have to accumulate 65536 - so every slot the macro never used kept its
// starting weight forever. In a 33-slot row where only 16 are ever used, those
// 17 dead slots hold a third of the row's mass and are paid for on every single
// symbol. Halving is what lets a dead slot decay toward the fractional floor.
inline constexpr uint32_t kRowBudget = 320u;

// ---------------------------------------------------------------------------
// Range coder + adaptive order-1 symbol models.
//
// The SKC v2 codec. Replaces the v1 "varint + zstd" pipeline with a single
// arithmetic back end that also does the entropy modelling, which is what
// makes the format small: on a 40-second level the v1 stream is a few hundred
// loosely-correlated varints, and zstd cannot find structure in something that
// short. A range coder with a per-magnitude context bucket sees it.
//
// The encoder is a standard 32-bit range coder with carryless E1/E2/E3
// renormalisation (bit output, pending-bit stack). The models are plain
// count-based frequency tables, one row per context, with an escape symbol for
// out-of-range values. Everything is deterministic: encoding the same macro
// twice yields byte-identical output.
// ---------------------------------------------------------------------------

// 32-bit range coder. Emits bits; caller packs them MSB-first into bytes.
class RangeEncoder {
public:
    void encode(uint32_t cumLow, uint32_t cumHigh, uint32_t total);

    // Flushes the coder. Pads with zero bits to a byte boundary.
    [[nodiscard]] std::vector<uint8_t> finish();

    // Bits of output produced so far, including bits still sitting in the carry
    // stack. This is what a component of the stream actually cost, and it is the
    // only honest way to decide which model is worth replacing - guessing has
    // cost ten failed attempts in a row.
    //
    // m_bits holds one byte per bit (finish() packs eight at a time), so the
    // element count IS the bit count.
    [[nodiscard]] uint64_t bitsProduced() const {
        return static_cast<uint64_t>(m_bits.size()) + m_pending;
    }

private:
    void emitBit(uint8_t bit);

    uint32_t m_low = 0;
    uint32_t m_high = 0xFFFFFFFFu;
    unsigned m_pending = 0;
    std::vector<uint8_t> m_bits;
};

class RangeDecoder {
public:
    RangeDecoder(const uint8_t* data, size_t size);

    [[nodiscard]] uint32_t decodeTarget(uint32_t total) const;
    void remove(uint32_t cumLow, uint32_t cumHigh, uint32_t total);

private:
    [[nodiscard]] uint32_t readBit();

    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_bitPos = 0;
    uint32_t m_low = 0;
    uint32_t m_high = 0xFFFFFFFFu;
    uint32_t m_code = 0;
};

// ---------------------------------------------------------------------------
// Adaptive symbol model: `symbolCount` symbols, `contextCount` independent
// frequency tables. Symbols are 0..symbolCount-1; every count starts at 1
// (Laplace) so a never-seen symbol still has nonzero probability.
//
// Counts halve (minimum 1) when a row's total crosses kModelRescaleThreshold,
// which keeps long macros from saturating a single row and freezing the
// adaptation.
// ---------------------------------------------------------------------------
class SymbolModel {
public:
    SymbolModel(uint32_t symbolCount, uint32_t contextCount,
                uint32_t countScale = kCountScale, uint32_t rowBudget = kRowBudget);

    void encode(RangeEncoder& enc, uint32_t symbol, uint32_t ctx);
    [[nodiscard]] uint32_t decode(RangeDecoder& dec, uint32_t ctx);

    [[nodiscard]] uint32_t symbolCount() const { return m_symbolCount; }
    [[nodiscard]] uint32_t contextCount() const { return m_contextCount; }

    [[nodiscard]] uint32_t rescaleThreshold() const { return m_countScale * m_rowBudget; }

private:
    void update(uint32_t ctx, uint32_t symbol);

    uint32_t m_symbolCount = 0;
    uint32_t m_contextCount = 0;
    uint32_t m_countScale = kCountScale;
    uint32_t m_rowBudget = kRowBudget;
    // counts_[ctx * symbolCount + symbol]
    std::vector<uint32_t> m_counts_;
};

// ---------------------------------------------------------------------------
// Value model for small non-negative integers — magnitude-split.
//
// The first cut of this used ONE 257-symbol model with a magnitude *context*
// (the top bits of the previous value). Measured on a real 128-click macro
// that bought nothing: a context row does not shrink the alphabet, so a symbol
// worth 3 in bucket 0 still carried the probability mass of all 257 symbols.
//
// Splitting the VALUE instead of bucketing the context fixes that. A value v
// is coded as (bucket = v >> 5, offset = v & 31): the bucket picks one of nine
// independent models, and each model only ever holds its own 32 offsets plus
// one escape. A model that never sees offset 20 therefore never pays for it.
// Same macro, same coder: 263 -> 243 bytes.
//
// The bucket itself is an order-0 choice - coding it as order-1 on the
// previous bucket measured worse (248 B), because consecutive clicks almost
// always share a magnitude.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Bucket mixer: an order-0 choice and an order-1 guess, blended by a weight.
//
// Every candidate so far either split the alphabet (a context per previous
// value) or re-parameterised the stream. Neither is a prediction: a context row
// is chosen before the value is known and then has to pay for its own alphabet.
// Mixing is different - both distributions are always available and the coder
// takes their weighted sum, so the weight itself moves toward whichever model
// has been right.
//
// The order-1 half is a transition table over buckets: "the last gap was in
// bucket 2, this one usually lands in 2 or 3". That is the part the order-0
// model structurally cannot see.
//
// All arithmetic is integer and both sides run the identical sequence of
// operations, so the mixture is reproducible. The shared denominator is a
// power of two, which keeps the divisions exact enough that a bucket can never
// round to zero width.
// ---------------------------------------------------------------------------
class BucketMixer {
public:
    static constexpr uint32_t kTotal = 1u << 16;
    static constexpr uint32_t kWeightStart = kTotal / 2u;
    static constexpr uint32_t kWeightStep = kTotal / 64u;
    static constexpr uint32_t kNoPrev = 0xFFFFFFFFu;

    explicit BucketMixer(uint32_t bucketCount, uint32_t countScale = kCountScale,
                         uint32_t rowBudget = kRowBudget)
        : m_buckets(bucketCount),
          m_countScale(countScale),
          m_rescale(countScale * rowBudget * 8u),
          m_p0(static_cast<size_t>(bucketCount), countScale * 8u),
          // One extra row for "no previous bucket yet", which is what the first
          // click uses.
          m_p1(static_cast<size_t>(bucketCount + 1u) * bucketCount, countScale * 8u),
          m_weight(kWeightStart) {}

    // The mixture must be bit-identical on both sides, so neither side is
    // allowed to divide a running sum. Each bucket's mass is computed once,
    // from truncated terms, and the CDF is the sum of those masses - encode and
    // decode then walk exactly the same integers. Dividing the cumulative count
    // instead looks equivalent and is not: truncation is not additive, the two
    // CDFs drift apart, and eventually the coded interval collapses to zero.
    [[nodiscard]] uint32_t massOf(size_t row, uint32_t bucket, uint32_t t0, uint32_t t1) const {
        return (m_weight * m_p0[bucket]) / t0 +
               ((kTotal - m_weight) * m_p1[row + bucket]) / t1;
    }

    [[nodiscard]] uint32_t cdfOf(size_t row, uint32_t upto, uint32_t t0, uint32_t t1) const {
        uint32_t sum = 0;
        for (uint32_t b = 0; b <= upto; ++b) sum += massOf(row, b, t0, t1);
        return sum;
    }

    void encode(RangeEncoder& enc, uint32_t bucket, uint32_t prevBucket) {
        const size_t row = rowOf(prevBucket);
        const uint32_t t0 = total0();
        const uint32_t t1 = total1(row);

        const uint32_t high = cdfOf(row, bucket, t0, t1);
        const uint32_t low = bucket == 0 ? 0u : cdfOf(row, bucket - 1u, t0, t1);
        const uint32_t total = cdfOf(row, m_buckets - 1u, t0, t1);
        enc.encode(low, high, total);
        update(bucket, row, t0, t1);
    }

    [[nodiscard]] uint32_t decode(RangeDecoder& dec, uint32_t prevBucket) {
        const size_t row = rowOf(prevBucket);
        const uint32_t t0 = total0();
        const uint32_t t1 = total1(row);

        const uint32_t target = dec.decodeTarget(cdfOf(row, m_buckets - 1u, t0, t1));
        uint32_t low = 0;
        uint32_t bucket = 0;
        while (bucket + 1u < m_buckets) {
            const uint32_t high = low + massOf(row, bucket, t0, t1);
            if (target < high) break;
            low = high;
            ++bucket;
        }
        dec.remove(low, low + massOf(row, bucket, t0, t1),
                   cdfOf(row, m_buckets - 1u, t0, t1));
        update(bucket, row, t0, t1);
        return bucket;
    }

private:
    [[nodiscard]] uint32_t total0() const {
        uint32_t t = 0;
        for (uint32_t b = 0; b < m_buckets; ++b) t += m_p0[b];
        return t;
    }

    [[nodiscard]] uint32_t total1(size_t row) const {
        uint32_t t = 0;
        for (uint32_t b = 0; b < m_buckets; ++b) t += m_p1[row + b];
        return t;
    }

    [[nodiscard]] size_t rowOf(uint32_t prevBucket) const {
        return static_cast<size_t>(prevBucket >= m_buckets ? m_buckets : prevBucket) * m_buckets;
    }

    void update(uint32_t bucket, size_t row, uint32_t t0, uint32_t t1) {
        // Move the weight toward whichever model was relatively more confident
        // about this bucket. Comparing the two normalised probabilities is what
        // makes this a fair contest - their totals differ wildly.
        const bool order1Better =
            static_cast<uint64_t>(m_p1[row + bucket]) * t0 >
            static_cast<uint64_t>(m_p0[bucket]) * t1;
        if (order1Better) m_weight = (m_weight + kWeightStep < kTotal) ? m_weight + kWeightStep : kTotal;
        else m_weight = (m_weight > kWeightStep) ? m_weight - kWeightStep : 0u;

        m_p0[bucket] += m_countScale;
        m_p1[row + bucket] += m_countScale;
        if (t0 > m_rescale) {
            for (uint32_t b = 0; b < m_buckets; ++b) {
                m_p0[b] = std::max<uint32_t>(m_countScale, m_p0[b] / 2u);
                for (uint32_t r = 0; r <= m_buckets; ++r)
                    m_p1[r * m_buckets + b] =
                        std::max<uint32_t>(m_countScale, m_p1[r * m_buckets + b] / 2u);
            }
        }
    }

    uint32_t m_buckets = 1;
    uint32_t m_countScale = kCountScale;
    uint32_t m_rescale = 0;
    std::vector<uint32_t> m_p0;
    std::vector<uint32_t> m_p1;
    uint32_t m_weight = kWeightStart;
};
class ValueModel {
public:
    // Bucket width, as a shift. 5 (32 offsets per bucket) is the long-standing
    // default; 4 is measurably better on a repetitive macro and measurably
    // worse on a hand-played one, so the encoder tries both and keeps the
    // smaller - see skc_format.cpp. The value lives in the file.
    static constexpr uint32_t kDefaultShift = 5;
    static constexpr uint32_t kShiftMin = 3;
    static constexpr uint32_t kShiftMax = 6;

    explicit ValueModel(uint32_t shift = kDefaultShift,
                        uint32_t countScale = kCountScale, uint32_t rowBudget = kRowBudget,
                        bool mix = false);

    void encode(RangeEncoder& enc, uint32_t value, uint64_t* bucketBits = nullptr,
                 uint64_t* offsetBits = nullptr, uint32_t prevValue = 0u);
    [[nodiscard]] uint32_t decode(RangeDecoder& dec, uint32_t prevValue = 0u);

private:
    [[nodiscard]] uint32_t bucketOf(uint32_t value) const {
        return value < 256u ? (value >> m_shift) : m_buckets;
    }

    uint32_t m_shift = kDefaultShift;
    uint32_t m_width = 1u << kDefaultShift;
    uint32_t m_buckets = 256u / (1u << kDefaultShift);
    uint32_t m_escape = 1u << kDefaultShift;
    uint32_t m_symbolsPerBucket = (1u << kDefaultShift) + 1u;

    // counts_[bucket * m_symbolsPerBucket + offset]
    std::vector<uint32_t> m_offsets_;
    std::vector<uint32_t> m_buckets_;
    uint32_t m_countScale = kCountScale;
    uint32_t m_rowBudget = kRowBudget;

    // Empty when mixing is off, which is why it cannot be a plain member with a
    // default constructor: a default-constructed mixer has no counts and would
    // read out of bounds the moment someone used it.
    std::optional<BucketMixer> m_mixer;
    uint32_t m_prevBucket = BucketMixer::kNoPrev;
};
// One ValueModel per context bucket.
//
// A context is a small integer the caller derives from what was encoded just
// before - the previous delta, the previous hold - so a stream that repeats
// itself (a macro built out of loop blocks) is learned as separate statistics
// per situation instead of one blurred average.
//
// This is a context split, not a different coder: with one bucket it must
// produce byte-identical output to a plain ValueModel, and the context count
// travels in the file header so encoder and decoder pick the same buckets.
// One model's worth of state per click: either the bucket/offset split or the
// direct row. Which one is a format choice recorded in the header, so the
// encoder builds one, measures it, and writes down the winner.
class ValueModelSet {
public:
    virtual ~ValueModelSet() = default;
    virtual void encode(RangeEncoder& enc, uint32_t value, uint32_t context,
                        uint64_t* bucketBits = nullptr, uint64_t* offsetBits = nullptr,
                        uint32_t prevValue = 0u) = 0;
    [[nodiscard]] virtual uint32_t decode(RangeDecoder& dec, uint32_t context,
                                    uint32_t prevValue = 0u) = 0;
};


class ContextValueModel : public ValueModelSet {
public:
    ContextValueModel(uint32_t contextCount, uint32_t shift = ValueModel::kDefaultShift,
                      uint32_t countScale = kCountScale, uint32_t rowBudget = kRowBudget,
                      bool mix = false)
        : m_models(contextCount ? contextCount : 1u,
                    ValueModel(shift, countScale, rowBudget, mix)) {}

    void encode(RangeEncoder& enc, uint32_t value, uint32_t context, uint64_t* bucketBits = nullptr,
                 uint64_t* offsetBits = nullptr, uint32_t prevValue = 0u) override {
        m_models[context % m_models.size()].encode(enc, value, bucketBits, offsetBits, prevValue);
    }

    [[nodiscard]] uint32_t decode(RangeDecoder& dec, uint32_t context,
                                 uint32_t prevValue = 0u) override {
        return m_models[context % m_models.size()].decode(dec, prevValue);
    }

private:
    std::vector<ValueModel> m_models;
};

// ---------------------------------------------------------------------------
// Direct value model - one adaptive row per actual value.
//
// ValueModel spends two symbols per value (bucket, offset), and each offset row
// starts with every one of its 32 slots at count 1. A hold mostly lives below
// 32 frames - 76% of them on the hand-played macros - so three quarters of the
// holds land in bucket 0 and pay for a 32-wide row in which roughly half the
// slots are dead. The measured cost is 6.2 bits per hold against 7.5 for a gap
// whose range is twenty times wider.
//
// So this codes 0..directCount-1 straight out of one row, where a value that
// recurs gets the count it has earned, and sends anything larger to a ValueModel
// tail. The tail is rare (under 2% of holds) and does not matter how it is coded.
// ---------------------------------------------------------------------------
class DirectValueModel {
public:
    DirectValueModel(uint32_t directCount, uint32_t shift = ValueModel::kDefaultShift,
                    uint32_t countScale = kCountScale, uint32_t rowBudget = kRowBudget)
        : m_direct(directCount + 1u, 1u, countScale, rowBudget),
          m_tail(shift, countScale, rowBudget),
          m_directCount(directCount) {}

    void encode(RangeEncoder& enc, uint32_t value) {
        if (value < m_directCount) {
            m_direct.encode(enc, value, 0u);
            return;
        }
        m_direct.encode(enc, m_directCount, 0u);
        m_tail.encode(enc, value - m_directCount);
    }

    [[nodiscard]] uint32_t decode(RangeDecoder& dec) {
        const uint32_t symbol = m_direct.decode(dec, 0u);
        return symbol < m_directCount ? symbol : m_directCount + m_tail.decode(dec);
    }

private:
    SymbolModel m_direct;
    ValueModel m_tail;
    uint32_t m_directCount;
};

// Context-split version, so the hold can have per-context rows the same way the
// gap does. Which model the hold uses is a format choice, recorded in the header.
class DirectContextValueModel : public ValueModelSet {
public:
    DirectContextValueModel(uint32_t contextCount, uint32_t directCount,
                            uint32_t shift = ValueModel::kDefaultShift,
                            uint32_t countScale = kCountScale, uint32_t rowBudget = kRowBudget)
        : m_models(contextCount ? contextCount : 1u,
                    DirectValueModel(directCount, shift, countScale, rowBudget)) {}

    void encode(RangeEncoder& enc, uint32_t value, uint32_t context, uint64_t* = nullptr,
                 uint64_t* = nullptr, uint32_t = 0u) override {
        m_models[context % m_models.size()].encode(enc, value);
    }

    [[nodiscard]] uint32_t decode(RangeDecoder& dec, uint32_t context,
                                 uint32_t = 0u) override {
        return m_models[context % m_models.size()].decode(dec);
    }

private:
    std::vector<DirectValueModel> m_models;
};

}  // namespace skk::skc
