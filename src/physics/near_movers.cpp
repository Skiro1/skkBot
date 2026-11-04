#include "physics/near_movers.hpp"
#include <Geode/Geode.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

#define SWEEP_MARGIN 8.f

namespace skk::phys {
    // OPT-IN, default OFF. This filter narrows the live `collisionCheckObjects`
    // list to objects inside a swept box around the player. It is our own
    // heuristic, not something Echo or silicate do (both REPLACE the whole
    // collision routine instead), so a wrong margin silently drops a real
    // collision: the object is never checked, the player flies through it and
    // the attempt diverges from the recording. Until a drift=0 run proves the
    // margin safe on real levels it must not be on by default.
    // Enable with the saved value `perf_near_movers = true` (or the setter).
    static int g_nearMoversState = -1;  // -1 = not read from settings yet
    static gd::vector<GameObject*> g_near;

    bool nearMoversEnabled() {
        if (g_nearMoversState < 0) {
            g_nearMoversState =
                Mod::get()->getSavedValue<bool>("perf_near_movers", false) ? 1 : 0;
        }
        return g_nearMoversState == 1;
    }

    void setNearMoversEnabled(bool enabled) {
        g_nearMoversState = enabled ? 1 : 0;
        Mod::get()->setSavedValue("perf_near_movers", enabled);
    }

    gd::vector<GameObject*>* filterNearMovers(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        if (!player || !objects) {
            return objects;
        }

        const CCRect& box = player->getObjectRect();
        float margin = SWEEP_MARGIN + std::fabs(dt) * 5.f;
        float half_w = box.size.width * 0.5f + margin;
        float half_h = box.size.height * 0.5f + margin;
        float px = box.origin.x + box.size.width * 0.5f;
        float py = box.origin.y + box.size.height * 0.5f;

        g_near.clear();
        int used = std::min(count, static_cast<int>(objects->size()));
        for (int i = 0; i < used; ++i) {
            auto* object = (*objects)[static_cast<std::size_t>(i)];
            if (!object) continue;

            float scale = std::max(std::fabs(object->m_scaleX), std::fabs(object->m_scaleY));
            float reach = (object->m_width + object->m_height + object->m_objectRadius) * scale
                + std::fabs(object->m_customBoxOffset.x)
                + std::fabs(object->m_customBoxOffset.y)
                + std::fabs(object->m_boxOffset.x)
                + std::fabs(object->m_boxOffset.y);
            float reach_w = half_w + reach;
            float reach_h = half_h + reach;

            bool close = std::fabs(static_cast<float>(object->m_positionX) - px) <= reach_w
                      && std::fabs(static_cast<float>(object->m_positionY) - py) <= reach_h;
            if (!close) {
                close = std::fabs(object->m_lastPosition.x - px) <= reach_w
                     && std::fabs(object->m_lastPosition.y - py) <= reach_h;
            }
            if (close) {
                g_near.push_back(object);
            }
        }
        return &g_near;
    }
}
