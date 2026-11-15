#pragma once

#include <cstddef>

namespace skkbot2::memtrack_alloc {

inline void* retAddr() {
#if defined(_MSC_VER) && defined(_M_X64)
    return _ReturnAddress();
#else
    return nullptr;
#endif
}

inline void* rawMalloc(std::size_t size) {
    return size ? std::malloc(size) : nullptr;
}

inline void rawFree(void* p) {
    std::free(p);
}

}
