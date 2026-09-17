#pragma once

#include <Geode/Geode.hpp>
#include <vector>
#include "logger/logger.hpp"

using namespace geode::prelude;

class BrokenObjectTracker {
public:
    static BrokenObjectTracker& get() {
        static BrokenObjectTracker instance;
        return instance;
    }

    void registerBroken(GameObject* obj) {
        if (!obj) return;
        for (auto* existing : m_brokenObjects) {
            if (existing == obj) return;
        }
        m_brokenObjects.push_back(obj);
        skkbot2::log::debug("P1 brokenObj: register id={} total={}", obj->m_objectID, m_brokenObjects.size());
    }

    void reDisableBroken() {
        skkbot2::log::debug("P1 brokenObj: reDisable count={}", m_brokenObjects.size());
        for (auto* obj : m_brokenObjects) {
            if (!obj) continue;
            obj->m_isDisabled = true;
            obj->m_isDisabled2 = true;
            obj->setOpacity(0.0f);
        }
    }

    const std::vector<GameObject*>& getBrokenObjects() const {
        return m_brokenObjects;
    }

    void setBrokenObjects(const std::vector<GameObject*>& objs) {
        m_brokenObjects = objs;
    }

    void clear() {
        skkbot2::log::debug("P1 brokenObj: clear (was {})", m_brokenObjects.size());
        m_brokenObjects.clear();
    }

    bool empty() const {
        return m_brokenObjects.empty();
    }

private:
    BrokenObjectTracker() = default;
    std::vector<GameObject*> m_brokenObjects;
};
