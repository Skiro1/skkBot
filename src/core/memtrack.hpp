#pragma once

namespace skkbot2::memtrack {

bool trackingEnabled();

void noteAlloc(void* p, void* site);
void noteFree(void* p, void* site);
void initModules();
std::string describeSite(void* site);
void report();

}

void* operator new(std::size_t size);
void operator delete(void* p) noexcept;
void* operator new[](std::size_t size);
void operator delete[](void* p) noexcept;
void* operator new(std::size_t size, const std::nothrow_t&) noexcept;
void operator delete(void* p, const std::nothrow_t&) noexcept;
void operator delete(void* p, std::size_t) noexcept;
void operator delete[](void* p, std::size_t) noexcept;
