#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace skk::codec {

enum class Level : uint8_t {
    Fast     = 0,  // zstd-1 (fast)
    Balanced = 1,  // zstd-12 (strong)
    Deep     = 2,  // zstd-22 (best ratio)
};

// Compress raw data.
std::vector<uint8_t> compress(
    std::vector<uint8_t> const& data,
    Level level = Level::Balanced
);

// Decompress data. expectedSize must match exactly.
std::vector<uint8_t> decompress(
    std::vector<uint8_t> const& data,
    size_t expectedSize
);

}  // namespace skk::codec