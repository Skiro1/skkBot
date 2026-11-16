#pragma once

#include <Geode/cocos/include/ccTypes.h>

namespace skk::trajectory {

struct Settings {
    bool enabled = false;          // GDH: level.show_trajectory
    bool performance = true;       // GDH: level.show_trajectory::performance
    bool noCollisions = false;     // GDH: level.show_trajectory::no_collisions
    int iterations = 150;          // GDH: level.show_trajectory::iterations
    int stepMultiplier = 1;        // GDH: level.show_trajectory::step_multiplier
    float tps = 240.0f;            // GDH: invisible.tps::value
    cocos2d::ccColor4F jumpColor = {0.91f, 0.00f, 1.00f, 1.00f};    // #E900FFFF
    cocos2d::ccColor4F normalColor = {0.00f, 1.00f, 1.00f, 1.00f};  // #00FFFFFF
    cocos2d::ccColor4F hitboxColor = {1.00f, 0.00f, 0.00f, 1.00f};  // #FF0000FF
    cocos2d::ccColor4F hitboxInnerColor = {0.00f, 0.25f, 1.00f, 1.00f}; // #003FFFFF
};

Settings& settings();
bool isSimulating();

} // namespace skk::trajectory
