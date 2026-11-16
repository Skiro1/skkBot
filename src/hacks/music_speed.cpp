// Music Speed — ties the FMOD master-channel-group pitch to the Speedhack value
// so the level song slows down with the game instead of drifting away from it.
//
// Reference bots, both of which pick the exact same mechanism:
//   GDH          src/hacks/core/speedhack_audio.cpp:30-42
//                GUI_HACK_CREATE("Invisible", "Speedhack Audio", "", false)
//                class $modify(SpeedhackAudioFMODAudioEngine, FMODAudioEngine)
//                void update(float dt) { FMODAudioEngine::update(dt);
//                    if (m_system->getMasterChannelGroup(&group) == FMOD_OK)
//                        group->setPitch(speedhackEnabled ? value : 1.f); }
//                Feature name: "Speedhack Audio", lives in the "Invisible" window.
//   EclipseMenu  src/hacks/Global/AudioSpeed.cpp:37-95
//                class $modify(AudioSpeedFMODHook, FMODAudioEngine)
//                getId() = "Audio Speed", hack id "global.audiospeed.toggle",
//                range 0.0001f .. 1000.f, plus a "global.audiospeed.sync" toggle
//                that follows the speedhack instead of its own value.
//                Priority -8.
//
// Both hook FMODAudioEngine::update and set pitch on the MASTER CHANNEL GROUP
// (not per-channel). Note the two differ on scope: GDH only ever mirrors the
// speedhack value, EclipseMenu additionally offers an independent float. We take
// the GDH behaviour because the speedhack value already lives in our GUI right
// next to this toggle — an independent second pitch slider would only ever
// disagree with the first one.
//
// Why the master group and not m_backgroundMusicChannel: the master group is
// the single choke point every sound routes through, which is exactly what
// "the song follows the game" needs. Per-channel frequency juggling (Eclipse's
// commented-out updateChannels() over channels 126..129) also misses channels
// created later and is what GD's own channel pool recycles.
//
// Why setPitch and not pause: pitch scales playback rate, which is what
// "slow the song down" means. A pause would freeze it and desync from the
// slowed physics.
//
// The feature is subordinate to speedhack: while speedhack is off the game
// speed is pinned to 1.0, so pitch stays 1.0 too and the song is untouched.
// Turning speedhack off therefore needs no explicit pitch reset — pitch follows
// engine->gameSpeed, which the Speed row already forces back to 1.0.

#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>

#include "logger/logger.hpp"
#include "music_speed.hpp"
#include "gui/skkbot_gui.hpp"
#include "skkBot.hpp"

using namespace geode::prelude;

namespace skk::music_speed {

// Last pitch we asked FMOD for. GD's own audio engine re-derives channel state
// on every update and can reset a group pitch behind our back (song change,
// level load, resumeAllAudio). Re-asserting only when our own target differs from
// the last value we SET keeps the normal path to one call per change instead of
// one per frame, and still repairs a silent external reset within one frame.
float lastApplied = 1.0f;
bool everApplied = false;

}  // namespace skk::music_speed

// Declared in music_speed.hpp so the GUI can nudge the audio hook the moment the
// toggle flips, instead of waiting for the next speed value change.
void skk::music_speed::forceReapply() {
    skk::music_speed::everApplied = false;
}

namespace skk::music_speed {

float targetPitch() {
    auto* engine = ReplayEngine::get();
    if (!engine) return 1.0f;
    // Read the GUI switch rather than inferring from gameSpeed: gameSpeed is
    // already forced to 1.0 when speedhack is off, so it cannot tell "switch off"
    // from "switch on at exactly 1.0x", and it also cannot tell "sync off at
    // 0.5x" from "sync on at 0.5x". Both cases must stay at 1.0 pitch.
    auto& gui = skkbot2::gui::get();
    if (!gui.musicSpeedSync) return 1.0f;

    float speed = static_cast<float>(engine->gameSpeed);
    if (!(speed > 0.0f)) return 1.0f;
    if (speed > 100.0f) speed = 100.0f;
    return speed;
}

}  // namespace skk::music_speed

class $modify(MusicSpeedFMODAudioEngine, FMODAudioEngine) {
    void update(float dt) {
        FMODAudioEngine::update(dt);

        const float want = skk::music_speed::targetPitch();

        // Cheap bail-out: nothing to do until our own target actually moves.
        // We compare against the last value we APPLIED rather than querying
        // FMOD, because a query per frame costs a virtual call for nothing.
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