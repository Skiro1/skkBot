#pragma once

#include <Geode/cocos/include/ccTypes.h>

namespace skk::trajectory {

struct Settings {
    bool enabled = false;
    bool performance = true;
    bool noCollisions = false;
    int iterations = 150;
    int stepMultiplier = 1;
    float tps = 240.0f;
    cocos2d::ccColor4F jumpColor = {0.91f, 0.00f, 1.00f, 1.00f};
    cocos2d::ccColor4F normalColor = {0.00f, 1.00f, 1.00f, 1.00f};
    cocos2d::ccColor4F hitboxColor = {1.00f, 0.00f, 0.00f, 1.00f};
    cocos2d::ccColor4F hitboxInnerColor = {0.00f, 0.25f, 1.00f, 1.00f};
};

Settings& settings();
bool isSimulating();

}
