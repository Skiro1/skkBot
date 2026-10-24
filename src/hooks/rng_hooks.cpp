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

// ── RandTriggerGameObject (objectID 2068) ─────────────────────────
// Each 2068 trigger gets its own LCG state, seeded from
// attemptSeed ^ (uniqueID * 2137). The global RNG is NOT touched;
// the trigger uses its own state directly.

class $modify(SkRandTriggerGameObject, RandTriggerGameObject) {
    struct Fields {
        uint64_t m_advRandomState = std::numeric_limits<uint64_t>::max(); // sentinel
    };

    void customObjectSetup(gd::vector<gd::string>& values, gd::vector<void*>& exists) {
        RandTriggerGameObject::customObjectSetup(values, exists);
        skk::advRandomRegister(&m_fields->m_advRandomState, m_uniqueID);
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (m_objectID != 2068) {
            RandTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);
            return;
        }

        // Lazy-init from attempt seed.
        if (m_fields->m_advRandomState == std::numeric_limits<uint64_t>::max()) {
            uint64_t seedBase = 100ULL;
            if (auto* engine = ReplayEngine::get()) {
                seedBase = engine->macroSetupSeed();
            }
            m_fields->m_advRandomState = seedBase ^ (static_cast<uint64_t>(m_uniqueID) * 2137ULL);
        }

        // Advance our own LCG.
        m_fields->m_advRandomState = skk::lcgAdvance(m_fields->m_advRandomState);
        uint32_t value = skk::lcgOutput(m_fields->m_advRandomState);

        // Call original: GD will read the global RNG, but we pre-load it
        // with our value so the first draw returns our deterministic result.
        // Then immediately restore the global to avoid corrupting the stream.
        uint64_t savedGlobal = *skk::gdPrimaryRng();
        *skk::gdPrimaryRng() = (static_cast<uint64_t>(value) << 16) | (savedGlobal & 0xFFFF);
        RandTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);
        *skk::gdPrimaryRng() = savedGlobal;
    }
};

// ── EffectGameObject (objectID 1912, 3607) ────────────────────────
// Same per-object RNG pattern as 2068, but for effect triggers.

class $modify(SkEffectGameObject, EffectGameObject) {
    struct Fields {
        uint64_t m_advRandomState = std::numeric_limits<uint64_t>::max();
    };

    void customObjectSetup(gd::vector<gd::string>& values, gd::vector<void*>& exists) {
        EffectGameObject::customObjectSetup(values, exists);
        if (m_objectID == 1912 || m_objectID == 3607) {
            skk::advRandomRegister(&m_fields->m_advRandomState, m_uniqueID);
        }
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
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
