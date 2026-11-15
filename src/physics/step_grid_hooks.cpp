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

    int g_forcedSteps = 0;
    double g_forcedTimestep = 0.0;

void physStepCountMidhook(safetyhook::Context& ctx) {
      ++skkbot2::hot::stats().calls[static_cast<size_t>(skkbot2::hot::PhysStep)];
      if (g_forcedSteps > 0) {
          ctx.r13 = 2 - g_forcedSteps;
      }
  }

    void restorePhysDtMidhook(safetyhook::Context& ctx) {
        if (g_forcedSteps > 0) {
            ctx.xmm9.f64[0] = g_forcedTimestep * static_cast<double>(g_forcedSteps);
        }
    }


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
