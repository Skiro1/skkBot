#pragma once

namespace skk::music_speed {
// Defined in hacks/music_speed.cpp. The GUI calls this when it flips the
// toggle so the FMOD pitch is re-evaluated on the very next audio update,
// instead of waiting for the Speed value to change.
void forceReapply();
}  // namespace skk::music_speed