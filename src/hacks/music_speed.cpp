
#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>

#include "logger/logger.hpp"
#include "music_speed.hpp"
#include "gui/skkbot_gui.hpp"
#include "skkBot.hpp"

using namespace geode::prelude;

namespace skk::music_speed {

float lastApplied = 1.0f;
bool everApplied = false;

}

void skk::music_speed::forceReapply() {
    skk::music_speed::everApplied = false;
}

namespace skk::music_speed {

float targetPitch() {
    auto* engine = ReplayEngine::get();
    if (!engine) return 1.0f;
    auto& gui = skkbot2::gui::get();
    if (!gui.musicSpeedSync) return 1.0f;

    float speed = static_cast<float>(engine->gameSpeed);
    if (!(speed > 0.0f)) return 1.0f;
    if (speed > 100.0f) speed = 100.0f;
    return speed;
}

}

class $modify(MusicSpeedFMODAudioEngine, FMODAudioEngine) {
    void update(float dt) {
        FMODAudioEngine::update(dt);

        const float want = skk::music_speed::targetPitch();

        if (skk::music_speed::everApplied &&
            std::abs(want - skk::music_speed::lastApplied) < 1e-4f) {
            return;
        }

        if (!m_system) return;
        FMOD::ChannelGroup* group = nullptr;
        if (m_system->getMasterChannelGroup(&group) != FMOD_OK || !group) {
            return;
        }
        if (group->setPitch(want) != FMOD_OK) {
            skkbot2::Logger::get().warn(
                "skkBot2: music speed: setPitch({:.3f}) failed", want);
            return;
        }

        if (!skk::music_speed::everApplied ||
            std::abs(want - skk::music_speed::lastApplied) >= 1e-4f) {
            skkbot2::Logger::get().info("skkBot2: music speed -> {:.3f}x", want);
        }
        skk::music_speed::lastApplied = want;
        skk::music_speed::everApplied = true;
    }
};