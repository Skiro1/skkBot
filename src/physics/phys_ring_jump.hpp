#pragma once

#include <Geode/Geode.hpp>

namespace skkbot2::phys {

bool ringJumpEnabled();
void setRingJumpEnabled(bool v);

bool tryRingJump(void* player, void* ring, bool skipCheck);

}
