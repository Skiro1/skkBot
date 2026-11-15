#include "core/rng_lock.hpp"
#include "version_offsets.hpp"

#include <Geode/Geode.hpp>
#include <logger/logger.hpp>

#ifdef GEODE_IS_WINDOWS
#include <safetyhook.hpp>
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace skk {


uint64_t* gdPrimaryRng() {
    return reinterpret_cast<uint64_t*>(
        geode::base::get() + skk::offsets::kRngPrimaryGlobalRva);
}

uint64_t* gdTeleportRng() {
    return reinterpret_cast<uint64_t*>(
        geode::base::get() + skk::offsets::kRngTeleportGlobalRva);
}


TripleRngState captureTripleRng() {
    TripleRngState s;
    s.primary   = *gdPrimaryRng();
    s.teleport  = *gdTeleportRng();
    return s;
}

void restoreTripleRng(TripleRngState const& state) {
    *gdPrimaryRng()   = state.primary;
    *gdTeleportRng()  = state.teleport;
    pinTripleSeedAtReset(state.primary, state.teleport);
}


namespace {
    std::vector<AdvRandEntry> g_advRandom;
}

void advRandomRegister(uint64_t* statePtr, int uniqueID) {
    if (!statePtr) return;
    for (auto const& e : g_advRandom) {
        if (e.statePtr == statePtr) return;
    }
    g_advRandom.push_back({statePtr, uniqueID});
}

void advRandomReseed(uint64_t seedBase) {
    for (auto& e : g_advRandom) {
        if (e.statePtr) {
            *e.statePtr = seedBase ^ (static_cast<uint64_t>(e.uniqueID) * 2137ULL);
        }
    }
}

void advRandomRestore(std::vector<uint64_t> const& savedStates) {
    size_t count = std::min(g_advRandom.size(), savedStates.size());
    for (size_t i = 0; i < count; ++i) {
        if (g_advRandom[i].statePtr) {
            *g_advRandom[i].statePtr = savedStates[i];
        }
    }
}

std::vector<uint64_t> advRandomCapture() {
    std::vector<uint64_t> result;
    result.reserve(g_advRandom.size());
    for (auto const& e : g_advRandom) {
        result.push_back(e.statePtr ? *e.statePtr : 0);
    }
    return result;
}

void advRandomClear() {
    g_advRandom.clear();
}

uint64_t advRandomBase(uint32_t seedSalt, int currentProgress) {
    std::mt19937 mixer(static_cast<unsigned>(seedSalt + currentProgress));
    std::uniform_int_distribution<uint64_t> dist(10000, 999999999);
    return static_cast<uint32_t>(dist(mixer));
}

void advRandomReseedAt(uint32_t seedSalt, int currentProgress) {
    advRandomReseed(advRandomBase(seedSalt, currentProgress));
}

void regenerateVarianceFromSeed(uint64_t seed, float* table, size_t count) {
    uint64_t s = seed;
    for (size_t i = 0; i < count; ++i) {
        table[i] = static_cast<float>(s & 0xFFFF) / 32768.0f - 1.0f;
        s = lcgAdvance(s);
    }
}


#ifdef GEODE_IS_WINDOWS

namespace {
    SafetyHookMid g_teleportHook;
    SafetyHookMid g_shakeHooks[4];
    bool g_rngHooksInstalled = false;

    uint64_t g_teleportState = 0;

    uint64_t g_shakeState = 0;

    void teleportMidhook(safetyhook::Context& ctx) {
        g_teleportState = lcgAdvance(g_teleportState);
        ctx.rax = static_cast<uintptr_t>(lcgOutput(g_teleportState));
    }

    void shakeRandomMidhook(safetyhook::Context& ctx) {
        g_shakeState = lcgAdvance(g_shakeState);
        ctx.rax = static_cast<uintptr_t>(lcgOutput(g_shakeState));
        ctx.rip += 6;
    }

    void syncMidhookFromGlobals() {
        g_teleportState = *gdTeleportRng();
    }

    void syncGlobalsFromMidhook() {
        *gdTeleportRng() = g_teleportState;
    }
}

bool installRngMidhooks() {
    if (g_rngHooksInstalled) return true;

    auto* base = reinterpret_cast<uint8_t*>(geode::base::get());
    if (!base) {
        skkbot2::log::warn("rng midhooks: base address unavailable");
        return false;
    }
#ifdef GEODE_IS_WINDOWS
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const size_t moduleSize = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
#else
    const size_t moduleSize = 0;
#endif

    auto tryHook = [&](const skk::offsets::Site& site,
                       SafetyHookMid& hook,
                       void (*callback)(safetyhook::Context&)) -> bool {
#ifdef GEODE_IS_WINDOWS
        auto addr = skk::offsets::resolve(site, base, moduleSize);
        if (!addr) {
            skkbot2::log::warn("rng midhooks: {} не резолвится (RVA 0x{:X}, сигнатура не совпала) - пропущен",
                               site.name, site.rva);
            return false;
        }
        try {
            hook = safetyhook::create_mid(*addr, callback);
        } catch (...) {
            skkbot2::log::warn("rng midhooks: {} at +0x{:X} threw", site.name, site.rva);
            return false;
        }
        if (!hook) {
            skkbot2::log::warn("rng midhooks: {} at +0x{:X} failed", site.name, site.rva);
            return false;
        }
        return true;
#else
        (void)site; (void)hook; (void)callback;
        return false;
#endif
    };

    bool ok = true;
    ok &= tryHook(skk::offsets::kRngTeleport, g_teleportHook, teleportMidhook);

    ok &= tryHook(skk::offsets::kRngShake1, g_shakeHooks[0], shakeRandomMidhook);
    ok &= tryHook(skk::offsets::kRngShake2, g_shakeHooks[1], shakeRandomMidhook);
    ok &= tryHook(skk::offsets::kRngShake3, g_shakeHooks[2], shakeRandomMidhook);
    ok &= tryHook(skk::offsets::kRngShake4, g_shakeHooks[3], shakeRandomMidhook);

    if (!ok) {
        skkbot2::log::warn("rng midhooks: some hooks failed - teleport lock degraded");
    }

    g_rngHooksInstalled = true;
    skkbot2::log::info("rng midhooks installed: teleport + 4 shake rand() sites (SKK and SKC)");
    return ok;
}

void pinTripleSeedAtReset(uint64_t primary, uint64_t teleport) {
    *gdPrimaryRng()   = primary;
    *gdTeleportRng()  = teleport;

    g_teleportState = teleport;
    g_shakeState = primary & 0x7FFFULL;
}

void applyRngBoundaryReseed(uint64_t primary, uint64_t teleport,
                            uint32_t seedSalt, int progress) {
    pinTripleSeedAtReset(primary, teleport);

    if (progress >= 0) {
        advRandomReseedAt(seedSalt, progress);
    }
}

void syncMidhookFromGlobals();
void syncGlobalsFromMidhook();

uint64_t shakeStateGet() { return g_shakeState; }
void shakeStateSet(uint64_t state) { g_shakeState = state; }

#else
bool installRngMidhooks() { return false; }
void pinTripleSeedAtReset(uint64_t primary, uint64_t teleport) {
    *gdPrimaryRng()   = primary;
    *gdTeleportRng()  = teleport;
}
void applyRngBoundaryReseed(uint64_t primary, uint64_t teleport,
                            uint32_t seedSalt, int progress) {
    pinTripleSeedAtReset(primary, teleport);
    if (progress >= 0) {
        advRandomReseedAt(seedSalt, progress);
    }
}
uint64_t shakeStateGet() { return 0; }
void shakeStateSet(uint64_t) {}
#endif
}
