#include "memtrack.hpp"
#include "memtrack_alloc.hpp"

#include <new>
#include <cstdlib>

extern "C++" {

void* operator new(std::size_t size) {
    void* p = skkbot2::memtrack_alloc::rawMalloc(size);
    if (!p) throw std::bad_alloc();
    skkbot2::memtrack::noteAlloc(p, skkbot2::memtrack_alloc::retAddr());
    return p;
}

void operator delete(void* p) noexcept {
    skkbot2::memtrack::noteFree(p, skkbot2::memtrack_alloc::retAddr());
    skkbot2::memtrack_alloc::rawFree(p);
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept {
    skkbot2::memtrack::noteFree(p, skkbot2::memtrack_alloc::retAddr());
    skkbot2::memtrack_alloc::rawFree(p);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* p = skkbot2::memtrack_alloc::rawMalloc(size);
    if (p) skkbot2::memtrack::noteAlloc(p, skkbot2::memtrack_alloc::retAddr());
    return p;
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    skkbot2::memtrack::noteFree(p, skkbot2::memtrack_alloc::retAddr());
    skkbot2::memtrack_alloc::rawFree(p);
}
void operator delete(void* p, std::size_t) noexcept {
    skkbot2::memtrack::noteFree(p, skkbot2::memtrack_alloc::retAddr());
    skkbot2::memtrack_alloc::rawFree(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    skkbot2::memtrack::noteFree(p, skkbot2::memtrack_alloc::retAddr());
    skkbot2::memtrack_alloc::rawFree(p);
}

}
