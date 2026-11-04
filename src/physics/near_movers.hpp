#pragma once

#include <Geode/Geode.hpp>

namespace skk::phys {
    [[nodiscard]] bool nearMoversEnabled();
    void setNearMoversEnabled(bool enabled);

    gd::vector<GameObject*>* filterNearMovers(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt);
}
