#ifdef GEODE_IS_WINDOWS

#include "skkBot.hpp"
#include "../core/hotpath.hpp"
#include "core/rng_lock.hpp"

#include <Geode/modify/RandTriggerGameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/Geode.hpp>
#include <logger/logger.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

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
        if (m_objectID != 2068 || !layer) {
            RandTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);
            return;
        }

        if (m_fields->m_advRandomState == std::numeric_limits<uint64_t>::max()) {
            const uint64_t seedBase = skk::currentAttemptSeed();
            m_fields->m_advRandomState = seedBase ^ (static_cast<uint64_t>(m_uniqueID) * 2137ULL);
        }

        const uint64_t roll = m_fields->m_advRandomState;
        m_fields->m_advRandomState = 214013ULL * roll + 2531011ULL;

        int sum = 0;
        for (const auto& chance : m_chanceObjects) {
            sum += chance.m_chance;
        }

        const int threshold = static_cast<int>(roundf(static_cast<float>(
            static_cast<double>(static_cast<float>((roll >> 16) & 32767)) / 32767.0 *
            static_cast<double>(static_cast<float>(sum)))));

        int running = 0;
        int group = 0;
        for (const auto& chance : m_chanceObjects) {
            running += chance.m_chance;
            group = chance.m_groupID;
            if (running >= threshold) break;
        }

        skkbot2::log::info("[RNG] RandTrigger (2068) uid={} roll={:#x} threshold={}/{} group={}",
                           m_uniqueID, roll, threshold, sum, group);

        std::vector<int> keys;
        if (remapKeys) keys = *remapKeys;
        layer->spawnGroup(group, false, 0.0, keys, m_uniqueID, m_controlID);
    }
};


class $modify(SkEffectGameObject, EffectGameObject) {
    struct Fields {
        uint64_t m_advRandomState = std::numeric_limits<uint64_t>::max();
    };

    void customObjectSetup(std::vector<std::string>& values, std::vector<void*>& exists) {
        EffectGameObject::customObjectSetup(values, exists);
        if (m_objectID == 1912) {
            skk::advRandomRegister(&m_fields->m_advRandomState, m_uniqueID);
        }
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, std::vector<int> const* remapKeys) {
        if (m_objectID != 1912 || !layer) {
            EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
            return;
        }

        if (m_fields->m_advRandomState == std::numeric_limits<uint64_t>::max()) {
            const uint64_t seedBase = skk::currentAttemptSeed();
            m_fields->m_advRandomState = seedBase ^ (static_cast<uint64_t>(m_uniqueID) * 2137ULL);
        }

        const uint64_t roll = m_fields->m_advRandomState;
        m_fields->m_advRandomState = 214013ULL * roll + 2531011ULL;

        const float rollPct = static_cast<float>(
            static_cast<double>(static_cast<float>((roll >> 16) & 32767)) / 32767.0 * 100.0);

        const float chance = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(this) + 1468);
        const int targetGroup = *reinterpret_cast<int*>(
            reinterpret_cast<uintptr_t>(this) + (chance < rollPct ? 1484 : 1480));

        skkbot2::log::info("[RNG] EffectGameObject (1912) uid={} roll={:#x} chance={:.1f} rollPct={:.2f} targetGroup={}",
                           m_uniqueID, roll, static_cast<double>(chance), static_cast<double>(rollPct), targetGroup);

        std::vector<int> keys;
        if (remapKeys) keys = *remapKeys;
        layer->spawnGroup(targetGroup, false, 0.0, keys, m_uniqueID, m_controlID);
    }
};

#endif
