#pragma once

#ifdef GEODE_IS_WINDOWS

#include <cstdint>

namespace skk {


bool installStepGridHooks();
bool stepGridHooksAvailable();
bool stepGridEnforcementActive();

bool isPlayerArtDeferred();
void setPlayerArtDeferred(bool defer);

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
