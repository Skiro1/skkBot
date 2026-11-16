// BUG-120 (B): allocation/free tracking for our TU.
#pragma once

namespace skkbot2::memtrack {

// BUG-131: трекинг по умолчанию выключен (см. memtrack.cpp).
bool trackingEnabled();

// Record an allocation (called from operator new overrides).
void noteAlloc(void* p, void* site);
// Record a free (called from operator delete overrides). A second free of the
// same pointer logs [MEMTRACK] DOUBLE FREE with both site ids (module-resolved).
void noteFree(void* p, void* site);
// Snapshot the loaded module table for describeSite (call once at mod load).
void initModules();
// Human-readable "module+RVA" for a code address.
std::string describeSite(void* site);
// Totals to the log.
void report();

} // namespace skkbot2::memtrack

// Replaceable allocation operators for our TU only (the game's allocations go
// through GeometryDash.exe/libcocos2d and are untouched).
void* operator new(std::size_t size);
void operator delete(void* p) noexcept;
// Arrays / sized / nothrow variants must route through the same tracker so our
// classes' interleaved vectors/strings cannot confuse the map.
void* operator new[](std::size_t size);
void operator delete[](void* p) noexcept;
void* operator new(std::size_t size, const std::nothrow_t&) noexcept;
void operator delete(void* p, const std::nothrow_t&) noexcept;
void operator delete(void* p, std::size_t) noexcept;
void operator delete[](void* p, std::size_t) noexcept;
