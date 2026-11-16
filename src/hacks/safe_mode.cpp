
#include <Geode/Geode.hpp>
#include "../core/hotpath.hpp"
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "logger/logger.hpp"
#include "skkBot.hpp"

using namespace geode::prelude;

namespace skk::safe_mode {

bool on() {
    auto* engine = ReplayEngine::get();
    return engine && engine->safeMode;
}

bool freezeAttempts() {
    return on();
}

bool freezeJumps() {
    return on();
}

}

class $modify(SafeModePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        const bool ret = PlayLayer::init(level, dontCreateObjects);

        m_isTestMode = skk::safe_mode::on();
        if (m_isTestMode) {
            skkbot2::Logger::get().info(
                "skkBot2: safe mode on — progress suppressed for this level");
        }

        return ret;
    }

    void resetLevel() {
        if (skk::safe_mode::on() && skk::safe_mode::freezeAttempts() && m_level) {
            m_level->m_attempts = m_level->m_attempts - 1;
        }
        PlayLayer::resetLevel();
    }
};

class $modify(SafeModePlayerObject, PlayerObject) {
void incrementJumps() {
          skkbot2::hot::Count skkbot2Count(skkbot2::hot::SafeModeJumps);
          if (skk::safe_mode::on() && skk::safe_mode::freezeJumps()) {
            return;
        }
        PlayerObject::incrementJumps();
    }
};