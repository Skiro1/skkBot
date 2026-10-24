#pragma once

namespace skk::practice_music {
// Defined in hacks/practice_music_sync.cpp. Declared here so main.cpp can kick
// the unlock at module load without pulling in the $modify classes.
void applyUnlock();
}  // namespace skk::practice_music