#pragma once

class GJBaseGameLayer;

namespace skkbot2::physics21 {
    bool isEnabled();
    void syncState(GJBaseGameLayer* layer);
    void onLevelExit();
}
