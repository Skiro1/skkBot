#include "physics/phys_ring_jump.hpp"

namespace skkbot2::phys {

static bool g_enabled = false;

bool ringJumpEnabled() { return g_enabled; }
void setRingJumpEnabled(bool v) { g_enabled = v; }

bool tryRingJump(void* , void* , bool ) {
    return false;
}

}
