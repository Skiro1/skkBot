#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace skk::codec {

enum class Level : uint8_t {
    Fast     = 0,
    Balanced = 1,
    Deep     = 2,
};

std::vector<uint8_t> compress(
    std::vector<uint8_t> const& data,
    Level level = Level::Balanced
);

std::vector<uint8_t> decompress(
    std::vector<uint8_t> const& data,
    size_t expectedSize
);

}