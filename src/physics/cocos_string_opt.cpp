#include <Geode/Geode.hpp>

#include "version_offsets.hpp"
#include "physics/cocos_string_opt.hpp"

#ifdef GEODE_IS_WINDOWS
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace geode::prelude;

// RobTop's CCString::initWithFormatAndValist allocates 100 KB on the heap every format call.
// Under high TPS or frequent string formatting, this fragments heap memory and slows down the CPU.
// Replacing it with a stack buffer of 1024 bytes avoids all heap allocation.
//
// Сайт живёт в реестре version_offsets.hpp (kCocosStringInitWithFormat) и
// резолвится по МОДУЛЕ COCOS (libcocos2d.dll): RVA fast-path с побайтовой
// сверкой + сигнатурный фолбэк. Хук не вешается на непроверенный байт —
// если сайт не резолвится, оптимизация просто отключается (fail-closed).

using InitWithFormat = bool (*)(cocos2d::CCString*, const char*, va_list);
static InitWithFormat original_init = nullptr;

static bool init_formatted(cocos2d::CCString* self, const char* format, va_list ap) {
    char buffer[1024];
    va_list copy;
    va_copy(copy, ap);
    int len = std::vsnprintf(buffer, sizeof(buffer), format, copy);
    va_end(copy);
    if (len < 0 || len >= static_cast<int>(sizeof(buffer))) {
        return original_init(self, format, ap);
    }
    self->m_sString.assign(buffer, static_cast<std::size_t>(len));
    return true;
}

namespace skk {

bool installCocosStringOpt() {
    auto* cocosBase = reinterpret_cast<uint8_t*>(geode::base::getCocos());
    if (!cocosBase) {
        skkbot2::log::warn("cocos string opt: cocos module unavailable, optimization off");
        return false;
    }
    const size_t cocosSize = skk::offsets::moduleSizeOf(cocosBase);
    auto addr = skk::offsets::resolve(skk::offsets::kCocosStringInitWithFormat,
                                      cocosBase, cocosSize);
    if (!addr) {
        skkbot2::log::warn("cocos string opt: CCString::initWithFormatAndValist "
                           "не резолвится (RVA 0x{:X}, сигнатура не совпала) - "
                           "оптимизация отключена",
                           skk::offsets::kCocosStringInitWithFormat.rva);
        return false;
    }
    original_init = reinterpret_cast<InitWithFormat>(*addr);
    (void)Mod::get()->hook(
        reinterpret_cast<void*>(original_init),
        &init_formatted,
        "CCString::initWithFormatAndValist"
    );
    return true;
}

}
#endif
