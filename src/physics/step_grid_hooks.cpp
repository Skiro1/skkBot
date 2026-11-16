#ifdef GEODE_IS_WINDOWS

#include "physics/step_grid_hooks.hpp"

#include "core/hotpath.hpp"
#include "version_offsets.hpp"

#include <Geode/Geode.hpp>
#include <logger/logger.hpp>
#include <safetyhook.hpp>

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

using namespace geode::prelude;

namespace {

    SafetyHookMid g_stepCountHook;
    SafetyHookMid g_restoreDtHook;
    bool g_installAttempted = false;
    bool g_available = false;

    // Game-thread-only shared state (the hooks fire inside the engine update
    // call wrapped by StepGridEnforcement on that same thread).
    int g_forcedSteps = 0;
    double g_forcedTimestep = 0.0;

    // 0x340B18: "mov [rsp+0x50], r13d" - stores the physics step-loop's initial
    // counter; the loop runs the counter up to (excluding) 2, so seeding it
    // with 2-N yields exactly N iterations (negative starts included).
    // В 2.2091 счётчик переехал с edx на r13d (было 0x237DCE: mov [rbp+0xA0], edx).
void physStepCountMidhook(safetyhook::Context& ctx) {
      // Counts the forced physics steps. This is the number that has to be
      // compared against another renderer, because the two do not step physics
      // the same way: this hook pins the step loop to exactly g_forcedSteps
      // iterations of g_forcedTimestep, so one CCScheduler::update advances the
      // level by g_forcedSteps * g_forcedTimestep - 1/240s at steps=1, NOT by
      // the dt=1/60 that was handed to the scheduler.
      //
      // A render that needs 1/60s per video frame therefore has to call the
      // scheduler several times, and the loop around it re-runs the per-step
      // bookkeeping each time. Whether that is where the extra wall time goes
      // is exactly the open question, so it gets measured rather than argued.
      ++skkbot2::hot::stats().calls[static_cast<size_t>(skkbot2::hot::PhysStep)];
      if (g_forcedSteps > 0) {
          ctx.r13 = 2 - g_forcedSteps;
      }
  }

    // 0x341BC7: post-loop bookkeeping reads xmm9 as the time actually consumed
    // by the step loop; report exactly N * timestep so the engine's internal
    // accumulators agree with the enforced grid.
    void restorePhysDtMidhook(safetyhook::Context& ctx) {
        if (g_forcedSteps > 0) {
            ctx.xmm9.f64[0] = g_forcedTimestep * static_cast<double>(g_forcedSteps);
        }
    }

    // Сканирование сигнатур и резолв сайтов живут в реестре
    // src/version_offsets.hpp (parsePattern/matchesAt/resolve).

    size_t getModuleSize(uint8_t* base) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
    }

}

namespace skk {

bool installStepGridHooks() {
    if (g_installAttempted) {
        return g_available;
    }
    g_installAttempted = true;

    auto* base = reinterpret_cast<uint8_t*>(geode::base::get());
    const size_t size = base ? getModuleSize(base) : 0;
    if (!base || size == 0) {
        skkbot2::log::warn("step-grid hooks: module unreadable, disabled");
        return false;
    }

    auto stepCount = skk::offsets::resolve(skk::offsets::kStepGridStepCount, base, size);
    auto restoreDt = skk::offsets::resolve(skk::offsets::kStepGridRestoreDt, base, size);

    if (!stepCount || !restoreDt) {
        skkbot2::log::warn("step-grid hooks: sites not found (GD build differs?) - "
                           "falling back to per-step update batching");
        g_available = false;
        return false;
    }

    try {
        g_stepCountHook = safetyhook::create_mid(*stepCount, physStepCountMidhook);
        g_restoreDtHook = safetyhook::create_mid(*restoreDt, restorePhysDtMidhook);
    } catch (...) {
        skkbot2::log::warn("step-grid hooks: create_mid threw - falling back to per-step batching");
        g_stepCountHook = SafetyHookMid{};
        g_restoreDtHook = SafetyHookMid{};
        g_available = false;
        return false;
    }

    if (!g_stepCountHook || !g_restoreDtHook) {
        skkbot2::log::warn("step-grid hooks: install failed - falling back to per-step batching");
        g_stepCountHook = SafetyHookMid{};
        g_restoreDtHook = SafetyHookMid{};
        g_available = false;
        return false;
    }

    g_available = true;
    skkbot2::log::info("step-grid hooks installed: stepCount@0x{:X} restoreDt@0x{:X}",
        *stepCount - reinterpret_cast<uintptr_t>(base),
        *restoreDt - reinterpret_cast<uintptr_t>(base));
    return true;
}

bool stepGridHooksAvailable() {
    return g_available;
}

bool stepGridEnforcementActive() {
    return g_forcedSteps > 0;
}

static bool g_deferPlayerArt = false;

bool isPlayerArtDeferred() {
    return g_deferPlayerArt;
}

void setPlayerArtDeferred(bool defer) {
    g_deferPlayerArt = defer;
}

StepGridEnforcement::StepGridEnforcement(int steps, double timestep) {
    if (g_available && steps > 0 && timestep > 0.0 && g_forcedSteps == 0) {
        g_forcedSteps = steps;
        g_forcedTimestep = timestep;
        m_armed = true;
    }
}

StepGridEnforcement::~StepGridEnforcement() {
    if (m_armed) {
        g_forcedSteps = 0;
        g_forcedTimestep = 0.0;
    }
}

}

#endif
