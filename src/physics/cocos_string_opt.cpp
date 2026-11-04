#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace geode::prelude;

// RobTop's CCString::initWithFormatAndValist allocates 100 KB on the heap every format call.
// Under high TPS or frequent string formatting, this fragments heap memory and slows down the CPU.
// Replacing it with a stack buffer of 1024 bytes avoids all heap allocation.
#define CCSTRING_INIT_WITH_FORMAT 0x105e0

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

$execute {
    auto cocos = geode::base::getCocos();
    original_init = reinterpret_cast<InitWithFormat>(cocos + CCSTRING_INIT_WITH_FORMAT);
    (void)Mod::get()->hook(
        reinterpret_cast<void*>(original_init),
        &init_formatted,
        "CCString::initWithFormatAndValist"
    );
}
#endif
