// Practice Music Sync — always on, no toggle, no GUI row.
//
// What this is: Geometry Dash ships a "Practice Music Sync" option in its own
// Game Options screen, but it is gated behind a hidden unlock item
// (UnlockType::GJItem id 17), so on a normal account it is neither visible nor
// usable. With it enabled the game restarts the level's music at the matching
// position on every practice respawn instead of letting it drift out of sync.
//
// Neither reference bot reimplements the sync — both just unlock the vanilla
// item and let GD do the work:
//   GDH          src/hacks/core/practice_music.cpp:8,20
//                GUI_HACK_CREATE("Core", "Practice Music",
//                "Unlocks RobTop's \"Practice Music Sync\"")
//                gsm->toggleEnableItem(UnlockType::GJItem, 17, enabled)
//                + hack.addHookPtr(...isItemUnlocked...)
//   EclipseMenu  src/hacks/Bypass/MusicUnlocker.cpp:9,17,45
//                class $hack(PracticeMusic) ... "Practice Music Sync"
//                utils::get<GameStatsManager>()->toggleEnableItem(GJItem, 17, v)
//                + isItemUnlocked hook
// GDH additionally kicks GJBaseGameLayer::toggleMusicInPractice() so the game
// re-applies the flag immediately; EclipseMenu does not.
//
// Why this sets the field instead of calling toggleMusicInPractice():
// that method is emitted into the bindings as an empty inline stub
// (GJBaseGameLayer.cpp:56 -> `void toggleMusicInPractice() {}`), and even if it
// did resolve it is a blind flip. GDH can call it safely because it fires from a
// user action where the previous state is known; called on every level load it
// would flip a value GD had just set from the unlocked item. Assigning
// m_practiceMusicSync (GJBaseGameLayer.hpp:4227) is idempotent.
//
// What this deliberately does NOT do:
//   - no Channel::setPosition / seek / setPaused tricks — neither GDH nor
//     EclipseMenu touch the FMOD channel position for practice;
//   - no stopAllMusic / pauseAllMusic on death — GDH has none at all, and
//     EclipseMenu only does it in its separate MuteLevelAudioOnDeath hack and
//     deliberately skips classic levels because GD already restarts music there;
//   - no AudioManager::setNextSong / song switching — absent in both.

#include <Geode/Geode.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "logger/logger.hpp"
#include "practice_music_sync.hpp"

using namespace geode::prelude;

namespace skk::practice_music {

// GD's hidden unlock item behind the Practice Music Sync option.
constexpr int kSyncItemId = 17;

// Unlock the item once at module load. toggleEnableItem is set-semantics (it
// takes the target state), so this is safe; the once-guard keeps the write out
// of every level load anyway. NOTE: do not gate this on isItemUnlocked — the
// hook below always reports item 17 as unlocked, so that check would always
// pass and the item would never actually be written.
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

}  // namespace skk::practice_music

// Makes GD treat the hidden item as unlocked, so the option shows up as enabled
// in GD's own Game Options screen and any internal isItemUnlocked gate passes.
// Same hook both reference bots install.
class $modify(PracticeMusicSyncStats, GameStatsManager) {
    bool isItemUnlocked(UnlockType type, int id) {
        if (type == UnlockType::GJItem && id == skk::practice_music::kSyncItemId) {
            return true;
        }
        return GameStatsManager::isItemUnlocked(type, id);
    }
};

// Force the live flag every level load. Re-asserted rather than set once so that
// re-entering practice, reloading a checkpoint or restarting the level cannot
// leave the layer with the flag off.
class $modify(PracticeMusicSyncLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        skk::practice_music::applyUnlock();
        m_practiceMusicSync = true;
        return true;
    }
};