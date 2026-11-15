#pragma once


#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::ast::wasm {

inline constexpr uint32_t kFormatAstV9 = 26;
inline constexpr uint32_t kFormatSlcV3 = 47;

enum class Kind : uint8_t {
    Click = 0,
    Release = 1,
    Skip = 2,
};

struct Click {
    uint32_t frame = 0;
    Kind p1 = Kind::Skip;
    Kind p2 = Kind::Skip;
};

struct Output {
    double tps = 240.0;
    std::vector<Click> clicks;
};

[[nodiscard]] bool convert(std::span<const uint8_t> data, Output& out,
                         std::string& outReason);

[[nodiscard]] bool saveAs(std::span<const uint8_t> data, uint32_t formatId,
                          std::vector<uint8_t>& out, std::string& outReason);

}