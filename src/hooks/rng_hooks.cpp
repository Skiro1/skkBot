#ifdef GEODE_IS_WINDOWS

#include "skkBot.hpp"
#include "../core/hotpath.hpp"
#include "core/rng_lock.hpp"

#include <Geode/modify/RandTriggerGameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/Geode.hpp>
#include <logger/logger.hpp>

#include <cstdint>
#include <limits>

using namespace geode::prelude;


class $modify(SkRandTriggerGameObject, RandTriggerGameObject) {
    struct Fields {
        uint64_t m_advRandomState = std::numeric_limits<uint64_t>::max();
    };

    void customObjectSetup(std::vector<std::string>& values, std::vector<void*>& exists) {
        RandTriggerGameObject::customObjectSetup(values, exists);
        skk::advRandomRegister(&m_fields->m_advRandomState, m_uniqueID);
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, std::vector<int> const* remapKeys) {
        if (m_objectID != 2068) {
            RandTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);
            return;
        }

        if (m_fields->m_advRandomState == std::numeric_limits<uint64_t>::max()) {
            uint64_t seedBase = 100ULL;
            if (auto* engine = ReplayEngine::get()) {
                seedBase = engine->macroSetupSeed();
            }
            m_fields->m_advRandomState = seedBase ^ (static_cast<uint64_t>(m_uniqueID) * 2137ULL);
        }

        m_fields->m_advRandomState = skk::lcgAdvance(m_fields->m_advRandomState);
        uint32_t value = skk::lcgOutput(m_fields->m_advRandomState);

        uint64_t savedGlobal = *skk::gdPrimaryRng();
        *skk::gdPrimaryRng() = (static_cast<uint64_t>(value) << 16) | (savedGlobal & 0xFFFF);
        RandTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);
        *skk::gdPrimaryRng() = savedGlobal;
    }
};


class $modify(SkEffectGameObject, EffectGameObject) {
    struct Fields {
        uint64_t m_advRandomState = std::numeric_limits<uint64_t>::max();
    };

    void customObjectSetup(std::vector<std::string>& values, std::vector<void*>& exists) {
        EffectGameObject::customObjectSetup(values, exists);
        if (m_objectID == 1912 || m_objectID == 3607) {
            skk::advRandomRegister(&m_fields->m_advRandomState, m_uniqueID);
        }
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, std::vector<int> const* remapKeys) {
        if (m_objectID != 1912 && m_objectID != 3607) {
            EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
            return;
        }

        if (m_fields->m_advRandomState == std::numeric_limits<uint64_t>::max()) {
            uint64_t seedBase = 100ULL;
            if (auto* engine = ReplayEngine::get()) {
                seedBase = engine->macroSetupSeed();
            }
            m_fields->m_advRandomState = seedBase ^ (static_cast<uint64_t>(m_uniqueID) * 2137ULL);
        }

        m_fields->m_advRandomState = skk::lcgAdvance(m_fields->m_advRandomState);
        uint32_t value = skk::lcgOutput(m_fields->m_advRandomState);

        uint64_t savedGlobal = *skk::gdPrimaryRng();
        *skk::gdPrimaryRng() = (static_cast<uint64_t>(value) << 16) | (savedGlobal & 0xFFFF);
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
        *skk::gdPrimaryRng() = savedGlobal;
    }
};

#endif
