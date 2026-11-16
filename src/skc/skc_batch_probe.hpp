#pragma once

namespace skkbot2 {
namespace skcprobe {

    inline int g_batchStep = 0;
    inline double g_frameDt = 0.0;

    inline void beginFrame(double dt) {
        g_batchStep = 0;
        g_frameDt = dt;
    }

    inline void noteStep() {
        ++g_batchStep;
    }

    inline void* g_probePortal = nullptr;
    inline int g_probePortalId = 0;

}
}
