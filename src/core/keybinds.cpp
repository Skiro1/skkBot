#include "skkBot.hpp"

// skkBot2 uses its native-imGui keybind handling instead of Geode keybind
// settings; all keybinding lives in the new GUI (src/gui/skkbot_gui.cpp). The
// engine still declares and is called with processHotkeys(), so keep a no-op
// definition so the engine and physicsbypass compile/link unchanged.

void ReplayEngine::processHotkeys() {
}
