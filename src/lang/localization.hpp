#pragma once

#include <string>
#include <string_view>

namespace skk::lang {

inline std::string tr(std::string_view key) {
    return std::string(key);
}

}
