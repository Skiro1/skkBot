
#include <Geode/Geode.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "logger/logger.hpp"
#include "practice_music_sync.hpp"

using namespace geode::prelude;

namespace skk::practice_music {

constexpr int kSyncItemId = 17;

void applyUnlock() {
    static bool done = false;
    if (done) return;
    done = true;

    auto* gsm = GameStatsManager::get();
    if (!gsm) {
        done = false;
        skkbot2::Logger::get().warn("skkBot2: GameStatsManager unavailable, "
                                    "practice music sync unlock deferred");
        return;
    }

    gsm->toggleEnableItem(UnlockType::GJItem, kSyncItemId, true);
    skkbot2::Logger::get().info(
        "skkBot2: unlocked GD item {} (Practice Music Sync)", kSyncItemId);
}

}

class $modify(PracticeMusicSyncStats, GameStatsManager) {
    bool isItemUnlocked(UnlockType type, int id) {
        if (type == UnlockType::GJItem && id == skk::practice_music::kSyncItemId) {
            return true;
        }
        return GameStatsManager::isItemUnlocked(type, id);
    }
};

class $modify(PracticeMusicSyncLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) {
            return false;
        }

        skk::practice_music::applyUnlock();
        m_practiceMusicSync = true;
        return true;
    }
};