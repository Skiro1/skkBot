#include "core/rng_lock.hpp"

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

// ── GD data globals ───────────────────────────────────────────────

uint64_t* gdPrimaryRng() {
    return reinterpret_cast<uint64_t*>(geode::base::get() + 0x6C2E90);
}

uint64_t* gdTeleportRng() {
    return reinterpret_cast<uint64_t*>(geode::base::get() + 0x6C2EF8);
}

// ── Capture / Restore ─────────────────────────────────────────────

TripleRngState captureTripleRng() {
    TripleRngState s;
    s.primary   = *gdPrimaryRng();
    s.teleport  = *gdTeleportRng();
    return s;
}

void restoreTripleRng(TripleRngState const& state) {
    *gdPrimaryRng()   = state.primary;
    *gdTeleportRng()  = state.teleport;
    // Keep the midhook bot-owned copy in lockstep so the *next* teleport
    // read advances the same chain the global was left at.
    pinTripleSeedAtReset(state.primary, state.teleport);
}

// ── Per-object advanced random ────────────────────────────────────

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

// ── Variance regeneration ─────────────────────────────────────────
// GD formula: v = (s & 0xFFFF) / 32768.0f - 1.0f; s = LCG(s)
void regenerateVarianceFromSeed(uint64_t seed, float* table, size_t count) {
    uint64_t s = seed;
    for (size_t i = 0; i < count; ++i) {
        table[i] = static_cast<float>(s & 0xFFFF) / 32768.0f - 1.0f;
        s = lcgAdvance(s);
    }
}

// ── Midhook installation (Windows) ────────────────────────────────

#ifdef GEODE_IS_WINDOWS

namespace {
    SafetyHookMid g_teleportHook;
    SafetyHookMid g_shakeHooks[4];
    bool g_rngHooksInstalled = false;

    // Bot-owned copy for midhook callback.
    uint64_t g_teleportState = 0;

    // Bot-owned shake state. GD draws shake randomness through C rand(), whose
    // seed is not part of the streams we pin at reset, so two attempts at the
    // same level consume different draws. Silicate overrides all four call
    // sites for exactly this reason
    // (silicate-main/src/hooks/GJBaseGameLayer.cpp:736-744).
    //
    // Shared by SKK and SKC on purpose. It was previously SKC-only behind a
    // gate, with SKK deliberately leaving shake unlocked
    // ("shake (camera) is not locked"). A half-locked stream is worse than
    // either: a checkpoint captured mid-attempt recorded a shake value that
    // could not be restored, so practice replays drifted visually anyway.
    uint64_t g_shakeState = 0;

    // Teleport midhook: intercept the inline LCG call.
    // GD calls rand() inline at these sites. We intercept rax (the return
    // value) by advancing our own state and setting rax to the output.
    // The hook site is RIGHT AFTER the LCG step, so rax already holds the
    // truncated 15-bit value. We skip the original call entirely.
    void teleportMidhook(safetyhook::Context& ctx) {
        g_teleportState = lcgAdvance(g_teleportState);
        ctx.rax = static_cast<uintptr_t>(lcgOutput(g_teleportState));
    }

    // Shake rand() override, shared by SKK and SKC. Always active once
    // installed: the chain is pinned deterministically at every attempt reset
    // and snapshotted into every practice checkpoint, so there is no state
    // left un-owned.
    void shakeRandomMidhook(safetyhook::Context& ctx) {
        g_shakeState = lcgAdvance(g_shakeState);
        ctx.rax = static_cast<uintptr_t>(lcgOutput(g_shakeState));
        ctx.rip += 6;   // skip the original call to rand()
    }

    // Sync bot-owned copy from GD global (called after restore).
    void syncMidhookFromGlobals() {
        g_teleportState = *gdTeleportRng();
    }

    // Sync GD global from bot-owned copy (called each tick to keep in sync).
    void syncGlobalsFromMidhook() {
        // The midhook advances bot-owned state. Write back to the GD global
        // so other GD code reading it sees consistent values.
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

    // Teleport site (inline LCG call in portal/teleport update).
    // Pattern: imul rax, [rip+disp], 0x343FD; add rax, 0x269EC3; ...
    // We hook at the point where rax holds the truncated output.
    auto tryHook = [&](SafetyHookMid& hook, uintptr_t rva, const char* name,
                       void (*callback)(safetyhook::Context&)) -> bool {
        uintptr_t addr = reinterpret_cast<uintptr_t>(base + rva);
        try {
            hook = safetyhook::create_mid(addr, callback);
        } catch (...) {
            skkbot2::log::warn("rng midhooks: {} at +0x{:X} threw", name, rva);
            return false;
        }
        if (!hook) {
            skkbot2::log::warn("rng midhooks: {} at +0x{:X} failed", name, rva);
            return false;
        }
        return true;
    };

    bool ok = true;
    ok &= tryHook(g_teleportHook, 0x20FED3, "teleport", teleportMidhook);

    // The four C-rand() call sites GD uses for shake randomness.
    static constexpr uintptr_t kShakeSites[4] = {
        0x23E173, 0x23E1A1, 0x23E1CB, 0x23E1E9,
    };
    for (size_t i = 0; i < 4; ++i) {
        ok &= tryHook(g_shakeHooks[i], kShakeSites[i], "shake",
                      shakeRandomMidhook);
    }

    if (!ok) {
        skkbot2::log::warn("rng midhooks: some hooks failed - teleport lock degraded");
        // Don't disable completely; the hooks that succeeded still work.
    }

    g_rngHooksInstalled = true;
    skkbot2::log::info("rng midhooks installed: teleport@0x20FED3 + 4 shake rand() sites (SKK and SKC)");
    return ok;
}

void pinTripleSeedAtReset(uint64_t primary, uint64_t teleport) {
    // Write to GD globals.
    *gdPrimaryRng()   = primary;
    *gdTeleportRng()  = teleport;

    // Sync bot-owned midhook copy.
    g_teleportState = teleport;
    // Shake draws come from C rand(), which we override while an SKC session is
    // live. Derive it from the same primary seed the attempt starts from so the
    // chain is identical on every run of that attempt. Masked to 15 bits: the
    // four call sites consume the truncated value.
    g_shakeState = primary & 0x7FFFULL;
}

void applyRngBoundaryReseed(uint64_t primary, uint64_t teleport,
                            uint32_t seedSalt, int progress) {
    // Restore the GD globals + bot-owned midhook copy to the captured
    // boundary state. Unlike pinTripleSeedAtReset this only repairs the RNG
    // globals - the arm family is not re-applied.
    pinTripleSeedAtReset(primary, teleport);

    // Re-seed per-object advanced randoms (2068/1912/3607) to the SAME base
    // refreshRngState derived at this practice progress during capture. The
    // LCG states themselves are not serialized into the macro, so playback
    // must reconstruct them from the deterministic seedSalt+progress formula
    // exactly as capture did on the respawn frame - otherwise the segment's
    // per-object rand chain diverges and the random section looks different.
    if (progress >= 0) {
        advRandomReseedAt(seedSalt, progress);
    }
}

// Export sync functions for use by refreshRngState / checkpoint restore.
void syncMidhookFromGlobals();
void syncGlobalsFromMidhook();

uint64_t shakeStateGet() { return g_shakeState; }
void shakeStateSet(uint64_t state) { g_shakeState = state; }

#else
// Non-Windows: stubs.
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
