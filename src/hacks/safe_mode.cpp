// Safe Mode — suppresses all GD progress accounting so runs in the level cannot
// bank a score, a jump count or an attempt count.
//
// Reference implementations (identical core in both, EclipseMenu is a superset):
//   GDH          src/hacks/core/safe_mode.cpp
//                GUI_HACK_CREATE("Core", "Safe Mode", "Disables progress on levels", false)
//                PlayLayer::destroyPlayer  :19-27  m_isTestMode = true around the base call
//                PlayLayer::levelComplete :29-37  same, around the base call
//                PlayLayer::resetLevel    :14-17  m_level->m_attempts = m_level->m_attempts - 1
//                PlayerObject::incrementJumps     returns early (no jump stat)
//                Hook priorities Early on all three PlayLayer hooks.
//   EclipseMenu  src/hacks/Global/SafeMode.cpp:144-218, same four effects, plus
//                per-effect toggles:
//                  global.safemode.freeze_attempts  (default true)
//                  global.safemode.freeze_jumps    (default true)
//                  global.safemode.freeze_best_run (default false)
//                  global.safemode.warn-popup      (default true)
//                and an AutoSafeMode variant that arms itself when any cheat is on.
//
// Deviations from both, all deliberate:
//
// 1. NO m_isTestMode munging around destroyPlayer / levelComplete. Both
//    references save the field, set it true, call through, then restore. The
//    restore is what makes them fragile: anything GD reads asynchronously after
//    the call (deferred stat writes, the end-layer population) sees the restored
//    value instead of test mode. Here the field is simply FORCED true for the
//    whole level lifetime while Safe Mode is on, which is the same observable
//    result without the save/restore window. Toggled off at level entry, so
//    turning the option off in the menu cannot leave a level stuck in test mode.
//
// 2. NO warn popup. EclipseMenu shows a FLAlertLayer listing active cheats when
//    you press play. Not ported: our option list already states what is frozen,
//    and a popup on every level launch is the single most annoying thing a
//    "quiet" feature can do.
//
// 3. NO AutoSafeMode (Eclipse's automatic arming on detected cheats). It depends
//    on a per-hack isCheating() registry that this codebase has no equivalent of.
//    Guessing which of our toggles count as a "cheat" would silently enable
//    Safe Mode for people who never asked for it.
//
// 4. Attempts are frozen by decrementing m_level->m_attempts in resetLevel
//    (both references do exactly this) rather than by suppressing
//    PlayLayer::updateAttempts. GDH and EclipseMenu hook updateAttempts for
//    Freeze Attempts in other features but NOT for Safe Mode itself, and our
//    checkpoint_system already has an updateAttempts hook that other features
//    depend on — routing Safe Mode through it would couple the two.
//
// Effects, matching the references:
//   - m_isTestMode forced true  -> no progress / coins / score banked
//   - m_attempts not incremented across resets
//   - incrementJumps suppressed  -> the jump stat stays put
//
// Visual-only otherwise: physics, input, replay and checkpoint state are
// untouched, so a macro recorded or played with Safe Mode on behaves exactly
// the same as with it off.

#include <Geode/Geode.hpp>
#include "../core/hotpath.hpp"
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "logger/logger.hpp"
#include "skkBot.hpp"

using namespace geode::prelude;

namespace skk::safe_mode {

// Master switch. Mirrored into ReplayEngine (gui_safe_mode) so the GUI and the
// hooks read the same value instead of each touching Mod saved values.
bool on() {
    auto* engine = ReplayEngine::get();
    return engine && engine->safeMode;
}

// Per-effect switches were removed: Safe Mode is a single master toggle and now
// always freezes both attempts and jumps. The user asked for the two extra
// settings to be dropped and their behaviour kept on by default, which also
// matches both references (GDH "Safe Mode" hacks/core/safe_mode.cpp,
// EclipseMenu SafeMode.cpp) — they hardcode both effects with no submenu.
// Kept as functions rather than inlined so the hook call sites stay unchanged.
bool freezeAttempts() {
    return on();
}

bool freezeJumps() {
    return on();
}

}  // namespace skk::safe_mode

class $modify(SafeModePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        const bool ret = PlayLayer::init(level, useReplay, dontCreateObjects);

        // Applied on EVERY level entry, not just the first. Turning Safe Mode off
        // in the menu has to clear a level that was entered with it on, and the
        // forced-true field would otherwise survive for the whole session.
        m_isTestMode = skk::safe_mode::on();
        if (m_isTestMode) {
            skkbot2::Logger::get().info(
                "skkBot2: safe mode on — progress suppressed for this level");
        }

        return ret;
    }

    void resetLevel() {
        if (skk::safe_mode::on() && skk::safe_mode::freezeAttempts() && m_level) {
            // Decrement BEFORE the base call so the counter GD writes during the
            // reset lands one lower and the net effect across the reset is zero.
            // GDH layout_mode.cpp:14-17 and EclipseMenu SafeMode.cpp:186-187 both
            // decrement after; doing it before keeps the value correct even if
            // the base resetLevel early-returns.
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