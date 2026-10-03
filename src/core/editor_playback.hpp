#ifndef _skk_editor_playback_hpp
#define _skk_editor_playback_hpp

#include "skkBot.hpp"

// LevelEditorLayer playtest integration for PLAY (see editor_playback.cpp).
class LevelEditorLayer;

namespace skk::editor {
    // Per-playtest spawn alignment, invoked once by the MacroEngineBaseLayer
    // drive-loop on the first editor-playtest tick (see replay_engine.cpp).
    void attemptPlaytestArm(GJBaseGameLayer* layer, ReplayEngine* engine);
}

#endif