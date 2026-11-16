#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace skk::skc {

inline constexpr uint32_t kCountScale = 16u;

inline constexpr uint32_t kRowBudget = 320u;


class RangeEncoder {
public:
    void encode(uint32_t cumLow, uint32_t cumHigh, uint32_t total);

    [[nodiscard]] std::vector<uint8_t> finish();

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

public:
    [[nodiscard]] size_t consumedBytes() const {
        return (m_bitPos + 7) / 8;
    }
};

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
    std::vector<uint32_t> m_counts_;
};

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
          m_p1(static_cast<size_t>(bucketCount + 1u) * bucketCount, countScale * 8u),
          m_weight(kWeightStart) {}

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

    std::vector<uint32_t> m_offsets_;
    std::vector<uint32_t> m_buckets_;
    uint32_t m_countScale = kCountScale;
    uint32_t m_rowBudget = kRowBudget;

    std::optional<BucketMixer> m_mixer;
    uint32_t m_prevBucket = BucketMixer::kNoPrev;
};
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

}
