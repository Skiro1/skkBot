#pragma once

#include <Geode/Geode.hpp>

namespace skk::phys {
    [[nodiscard]] bool nearMoversEnabled();
    void setNearMoversEnabled(bool enabled);

    std::vector<GameObject*>* filterNearMovers(PlayerObject* player, std::vector<GameObject*>* objects, int count, float dt);
}
