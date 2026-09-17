#include "codec.hpp"
#include "zstd.h"

#include <cstring>

namespace skk::codec {

static constexpr uint8_t kCodecZstd = 0;
static constexpr size_t kHeaderSize = 8;

// ─── Compress ────────────────────────────────────────────────────────
std::vector<uint8_t> compress(
    std::vector<uint8_t> const& data,
    Level level
)
{
    if (data.empty()) return {};

    int zstdLevel = 1;
    if (level == Level::Balanced) zstdLevel = 12;
    if (level == Level::Deep) zstdLevel = 22;

    size_t cap = ZSTD_compressBound(data.size());
    std::vector<uint8_t> out(kHeaderSize + cap);
    out[0] = 'Z'; out[1] = 'D';
    out[2] = 3;
    out[3] = kCodecZstd;
    uint32_t sz = static_cast<uint32_t>(data.size());
    std::memcpy(out.data() + 4, &sz, 4);
    size_t cSize = ZSTD_compress(out.data() + kHeaderSize, cap, data.data(), data.size(), zstdLevel);
    if (ZSTD_isError(cSize)) return {};
    out.resize(kHeaderSize + cSize);
    return out;
}

// ─── Decompress ──────────────────────────────────────────────────────
std::vector<uint8_t> decompress(
    std::vector<uint8_t> const& data,
    size_t expectedSize
)
{
    if (data.size() < kHeaderSize) return {};
    if (data[0] != 'Z' || data[1] != 'D') return {};

    uint8_t codec = data[3];
    uint32_t origSize = 0;
    std::memcpy(&origSize, data.data() + 4, 4);

    if (codec != kCodecZstd) return {};
    if (origSize == 0 || origSize > 256 * 1024 * 1024) return {};

    uint8_t const* payload = data.data() + kHeaderSize;
    size_t payloadSize = data.size() - kHeaderSize;

    std::vector<uint8_t> out(origSize);
    size_t dSize = ZSTD_decompress(out.data(), out.size(), payload, payloadSize);
    if (ZSTD_isError(dSize) || dSize != origSize) return {};
    return out;
}

}  // namespace skk::codec