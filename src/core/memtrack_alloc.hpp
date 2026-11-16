// Allocation backend helpers for memtrack: raw malloc/free and the caller
// return-address capture (via _ReturnAddress(), x64 MSVC intrinsic).
#pragma once

#include <cstddef>

namespace skkbot2::memtrack_alloc {

// The MSVC intrinsic is only valid at the immediate call site of the wrapping
// operator; each operator calls retAddrFromCaller() defined per-file inline.
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

} // namespace skkbot2::memtrack_alloc
