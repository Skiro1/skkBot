#pragma once

namespace skkbot2 {
// BUG-80 [ENTRY-FREEZE] watcher: disarm + rearm hooks. The watcher lives in
// physicsbypass.cpp; call entryFreezeReset() whenever a PlayLayer dies (level
// exit) and when a bot session is queued outside a level - a fresh PlayLayer
// can reuse the dead one's heap address, and without a reset the watcher
// would mistake it for the already-watched entry and go silent.
void entryFreezeReset();
}

void resetSimulationTimingState();
void disableExpectedTicksPatch();
void dropCarry();

