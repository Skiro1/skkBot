

#include "memtrack.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include <Geode/Geode.hpp>

#include "../logger/logger.hpp"

namespace skkbot2::memtrack {

namespace {

constexpr size_t kMapSlots = 1u << 17;

struct Entry {
    std::atomic<uint64_t> ptr;
    std::atomic<uint64_t> site;
    std::atomic<uint64_t> tick;
    std::atomic<uint64_t> freeEpoch;
    void* bt[6];
};

struct State {
    std::atomic<uint64_t> allocs;
    std::atomic<uint64_t> frees;
    std::atomic<uint64_t> doubleFrees;
    std::atomic<uint64_t> misses;
    std::atomic<uint64_t> epoch;
    Entry slots[kMapSlots];
};

State g_state;

inline size_t slotOf(uint64_t p) {
    constexpr uint64_t kMask = kMapSlots - 1;
    return (size_t)((p >> 6) & kMask);
}

inline uint64_t siteBits(void* site, bool first) {
    return ((uint64_t)site & ~1ull) | (first ? 1 : 0);
}

Entry* probeReserve(uint64_t p) {
    const size_t start = slotOf(p);
    Entry* hole = nullptr;
    for (size_t i = 0; i < 64; ++i) {
        Entry& e = g_state.slots[(start + i) & (kMapSlots - 1)];
        uint64_t cur = e.ptr.load(std::memory_order_acquire);
        if (cur == p) return &e;
        if (cur == 0 && !hole) hole = &e;
    }
    if (!hole) {
        g_state.misses.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    uint64_t expect = 0;
    if (!hole->ptr.compare_exchange_strong(expect, p)) {
        if (expect == p) return hole;
        g_state.misses.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    return hole;
}

Entry* probeFind(uint64_t p) {
    const size_t start = slotOf(p);
    for (size_t i = 0; i < 64; ++i) {
        Entry& e = g_state.slots[(start + i) & (kMapSlots - 1)];
        uint64_t cur = e.ptr.load(std::memory_order_acquire);
        if (cur == p) return &e;
        if (cur == 0) return nullptr;
    }
    return nullptr;
}

}

bool g_tracking = false;
bool g_trackingInit = false;

bool trackingEnabled() {
    if (!g_trackingInit) {
        g_trackingInit = true;
        g_tracking = geode::Mod::get()->getSavedValue<bool>(
            "memtrack_enabled", false);
    }
    return g_tracking;
}

struct ModRange {
    uintptr_t base;
    char name[40];
};
std::vector<ModRange> g_mods;

void initModules() {
    HMODULE mods[512] = {};
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods),
            &needed)) {
        return;
    }
    const int count = (int)(needed / sizeof(HMODULE));
    if (count <= 0 || count > 512) return;
    wchar_t path[MAX_PATH] = {};
    for (int i = 0; i < count; ++i) {
        const DWORD len = K32GetModuleFileNameExW(GetCurrentProcess(),
            mods[i], path, MAX_PATH);
        if (!len) continue;
        const wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) slash += 1;
        ModRange r;
        r.base = (uintptr_t)mods[i];
        WideCharToMultiByte(CP_UTF8, 0, slash ? slash : path, -1, r.name,
            sizeof(r.name), nullptr, nullptr);
        g_mods.push_back(r);
        skkbot2::log::info("[MEMTRACK] module {} base 0x{:x}", r.name, r.base);
    }
}

bool trackingEnabled();

void noteAlloc(void* p, void* site) {
    if (!trackingEnabled() || !p) return;
    g_state.allocs.fetch_add(1, std::memory_order_relaxed);
    g_state.epoch.fetch_add(1, std::memory_order_relaxed);
    Entry* e = probeReserve((uint64_t)p);
    if (!e) return;
    const auto ns =
        std::chrono::steady_clock::now().time_since_epoch().count();
    e->site.store(siteBits(site, true), std::memory_order_release);
    e->tick.store((uint64_t)ns, std::memory_order_release);
    e->freeEpoch.exchange(0, std::memory_order_relaxed);
}

static void captureBt(void** dst) {
    USHORT n = RtlCaptureStackBackTrace(1, 6, dst, nullptr);
    for (int i = n; i < 6; ++i) dst[i] = nullptr;
}

void noteFree(void* p, void* site) {
    if (!trackingEnabled() || !p) return;
    const uint64_t epoch =
        g_state.epoch.fetch_add(1, std::memory_order_relaxed) + 1;
    g_state.frees.fetch_add(1, std::memory_order_relaxed);
    Entry* e = probeFind((uint64_t)p);
    if (!e) {
        void* bt[6];
        captureBt(bt);
        e = probeReserve((uint64_t)p);
        if (!e) {
            static std::atomic<int64_t> s_lastUntracked{0};
            const auto now = std::chrono::steady_clock::
                now().time_since_epoch().count();
            int64_t last = s_lastUntracked.load(std::memory_order_relaxed);
            if (now - last > 2000000000LL ) {
                s_lastUntracked.store(now, std::memory_order_relaxed);
                skkbot2::log::error(
                    "[MEMTRACK] free-untracked (throttled) ptr={:#x} {}",
                    (uint64_t)p, describeSite(site));
            }
            return;
        }
        const auto ns = std::chrono::steady_clock::
            now().time_since_epoch().count();
        e->ptr.store((uint64_t)p, std::memory_order_release);
        e->site.store(siteBits(site, false), std::memory_order_release);
        e->tick.store((uint64_t)ns, std::memory_order_release);
        e->freeEpoch.store(epoch, std::memory_order_release);
        for (int i = 0; i < 6; ++i) e->bt[i] = bt[i];
        return;
    }
    uint64_t alive = e->site.load(std::memory_order_acquire);
    if (alive & 1) {
        uint64_t expect = alive;
        void* bt[6];
        captureBt(bt);
        if (e->site.compare_exchange_strong(
                expect, siteBits(site, false), std::memory_order_acq_rel)) {
            e->freeEpoch.store(epoch, std::memory_order_release);
            for (int i = 0; i < 6; ++i) e->bt[i] = bt[i];
            return;
        }
    }
    const uint64_t firstEpoch = e->freeEpoch.load(std::memory_order_acquire);
    if (firstEpoch != 0 && epoch - firstEpoch > 4096) {
        void* bt[6];
        captureBt(bt);
        e->freeEpoch.store(epoch, std::memory_order_release);
        for (int i = 0; i < 6; ++i) e->bt[i] = bt[i];
        return;
    }
    if (firstEpoch == 0) {
        void* bt[6];
        captureBt(bt);
        e->freeEpoch.store(epoch, std::memory_order_release);
        for (int i = 0; i < 6; ++i) e->bt[i] = bt[i];
        return;
    }
    g_state.doubleFrees.fetch_add(1, std::memory_order_relaxed);
    void* bt2[6];
    captureBt(bt2);
    const uint64_t t1 = e->tick.load(std::memory_order_acquire);
    skkbot2::log::error(
        "[MEMTRACK] DOUBLE FREE ptr={:#x} site2={} site1={} tick1={:.3f}\n"
        "  bt1: {} | {} | {} | {} | {} | {}\n"
        "  bt2: {} | {} | {} | {} | {} | {}",
        (uint64_t)p, describeSite(site), describeSite((void*)(alive & ~1ull)),
        (double)((int64_t)t1) / 1e9, describeSite(e->bt[0]),
        describeSite(e->bt[1]), describeSite(e->bt[2]), describeSite(e->bt[3]),
        describeSite(e->bt[4]), describeSite(e->bt[5]), describeSite(bt2[0]),
        describeSite(bt2[1]), describeSite(bt2[2]), describeSite(bt2[3]),
        describeSite(bt2[4]), describeSite(bt2[5]));
    skkbot2::Logger::get().flush();
    e->ptr.store(0, std::memory_order_release);
}

void report() {
    skkbot2::log::info(
        "[MEMTRACK] allocs={} frees={} doubleFrees={} misses={} epoch={}",
        g_state.allocs.load(), g_state.frees.load(),
        g_state.doubleFrees.load(), g_state.misses.load(), g_state.epoch.load());
}

std::string describeSite(void* site) {
    const uintptr_t a = (uintptr_t)site;
    const ModRange* best = nullptr;
    for (const auto& m : g_mods) {
        if (a >= m.base && (!best || m.base > best->base)) best = &m;
    }
    if (best) {
        char buf[64];
        snprintf(buf, sizeof(buf), "mod/%s+0x%llX", best->name,
            (unsigned long long)(a - best->base));
        return buf;
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)a);
    return buf;
}

}
