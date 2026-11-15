#ifndef _skk_editor_playback_hpp
#define _skk_editor_playback_hpp

#include "skkBot.hpp"

class LevelEditorLayer;

namespace skk::editor {
    void attemptPlaytestArm(GJBaseGameLayer* layer, ReplayEngine* engine);
}

#endif