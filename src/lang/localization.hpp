#pragma once

#include <string>
#include <string_view>

// Stub replacement for the removed skkBot localization module.
// The replay engine references skk::lang::tr(key) for UI strings; the
// skkBot2 native-imGui port returns the key itself (English fallback).
namespace skk::lang {

inline std::string tr(std::string_view key) {
    return std::string(key);
}

}
