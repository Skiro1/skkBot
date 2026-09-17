#include "skkBot.hpp"

// Stub for the removed skkBot audio module. The original engine called
// triggerAudio() to play click sounds for macro input; skkBot2 ships no click
// sounds, so this is a no-op that keeps the engine linkable/compilable.

void ReplayEngine::triggerAudio(bool secondPlayer, int actionType, bool pressed) {
    static_cast<void>(secondPlayer);
    static_cast<void>(actionType);
    static_cast<void>(pressed);
}
