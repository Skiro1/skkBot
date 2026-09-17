#pragma once

#ifdef GEODE_IS_WINDOWS

#include <cstdint>

namespace skk {

// Step-grid enforcement midhooks (Windows, GD 2.208x) - the silicate/grape
// physStepCount/restorePhysDt pair, located by byte pattern (fixed RVA as
// fast path, full-module scan as fallback; refused entirely if neither
// matches). They pin GJBaseGameLayer::update's internal physics step LOOP to
// exactly the step count the tick planner chose, so the engine can never
// round the count differently between the recording session and the playback
// session - the residual +-1-substep collision-phase offset that position
// forcing alone cannot fix.

bool installStepGridHooks();
bool stepGridHooksAvailable();
// True while an update() call is inside a StepGridEnforcement scope (the
// dt handed to the engine is already planner-quantized: getModifiedDelta
// must pass it through untouched to avoid a second rounding).
bool stepGridEnforcementActive();

// RAII: while alive, every engine step-loop store is forced to `steps`
// iterations of `timestep` seconds. Game-thread only, must wrap a single
// GJBaseGameLayer::update() call; never nest.
class StepGridEnforcement {
public:
    StepGridEnforcement(int steps, double timestep);
    ~StepGridEnforcement();
    StepGridEnforcement(const StepGridEnforcement&) = delete;
    StepGridEnforcement& operator=(const StepGridEnforcement&) = delete;

private:
    bool m_armed = false;
};

}

#endif
