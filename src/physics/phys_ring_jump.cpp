#include "physics/phys_ring_jump.hpp"

namespace skkbot2::phys {

static bool g_enabled = false;

bool ringJumpEnabled() { return g_enabled; }
void setRingJumpEnabled(bool v) { g_enabled = v; }

// Full reimplementation is not feasible via Geode bindings: the fields
// (m_canRingJump, m_yVelocity, m_yStart, m_playerScale, ...) are not exposed
// in the binding headers; they live at game-specific offsets. Implementing
// phys::ringJump requires raw-offset access and per-field verification,
// which is a multi-session reverse-engineering task. For now this always
// falls back to the original PlayerObject::ringJump (no behaviour change).
bool tryRingJump(void* /*player*/, void* /*ring*/, bool /*skipCheck*/) {
    return false;
}

} // namespace skkbot2::phys
