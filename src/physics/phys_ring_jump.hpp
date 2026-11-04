#pragma once

#include <Geode/Geode.hpp>

namespace skkbot2::phys {

// Experimental reimplemented ring-jump physics (no GD overhead).
// Currently a skeleton: handles only the common "boost" rings for the
// cube/ship/ball/bird/robot/spider game modes. Visual effects, portal
// transitions, dash rings, spider orbs, drop rings and custom rings are
// still delegated to the original PlayerObject::ringJump.
//
// Enable via gui.perfPhysRingJump. When enabled, ringJump is called
// through this path; otherwise the original is used (zero behaviour change).
bool ringJumpEnabled();
void setRingJumpEnabled(bool v);

// Returns true if this phys path handled the ring (caller should skip original).
// Returns false to fall back to PlayerObject::ringJump.
bool tryRingJump(void* player, void* ring, bool skipCheck);

} // namespace skkbot2::phys
