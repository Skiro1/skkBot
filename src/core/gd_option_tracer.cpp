// Where the frame rate is actually gated, and how to find out.
//
// WHY THIS FILE EXISTS. For most of 19.10.2026 this mod assumed the thing that
// gates presentation was a field on GameManager. It tried a game variable
// ("0116", key never established), then a named field (m_vsyncEnabled, which the
// run log showed was ALREADY false before anything wrote to it), and both were
// guesses dressed as findings.
//
// The updated Echo reference settles it. mac_step.cpp:62:
//
//     auto* sdl = Loader::get()->getLoadedMod("zmx.sdl");
//     if (!sdl || window_max_refresh() <= 61.0) return;
//     if (!sdl->hasSetting("disable-vsync") || !sdl->hasSetting("uncap-framerate")) return;
//     if (sdl->getSettingValue<bool>("disable-vsync") && sdl->getSettingValue<bool>("uncap-framerate"))
//         return;
//     Logic::get().notify("Turn on the SDL Mod's \"Disable VSync\" and "
//                         "\"Disable Framerate Cap\" for better performance.", true);
//
// So the switch is the SDL MOD's, read out of geode.settings as `disable-vsync`
// and `uncap-framerate`. It is a driver/presenter-level setting the SDL mod
// consumes at startup - which is why no GD field, and no game variable, moves it,
// and why it was already false in the run that "did not turn vsync off".
//
// Two details of Echo's check that are worth not copying blindly:
//   - window_max_refresh() is defined only in mac_promotion.mm, so on Windows the
//     whole guard collapses and Echo never raises this on our platform. The user
//     here found it by hand. Checking the settings without the refresh-rate gate
//     is strictly more useful: the answer is the same either way.
//   - Echo only NOTIFIES. It does not toggle the setting, and cannot: it is
//     consumed before any mod runs. Same rule here - report, do not pretend.
//
// The tracer below is the leftover instrument from the guess. It stays because
// it is free and it prints keys the moment anyone flips a GD option, but it is
// no longer the thing the FPS feature depends on.
#include "core/playback_fps.hpp"

#include "logger/logger.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJOptionsLayer.hpp>
// Generated per class. Without it $modify has no declaration to attach to and
// fails its static_assert with "modified class not recognized".
#include <Geode/modify/GJOptionsLayer.hpp>

#include <safetyhook.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace {

// GameManager::toggleGameVariable - win 0x183290 (bindings 2.2081). Pinned
// offset with a range check, same style as the step-grid hooks.
constexpr uintptr_t kToggleGameVariableRva = 0x183290;

safetyhook::MidHook g_toggleVarHook;

bool g_pacingSaved = false;
std::map<std::string, bool> g_pacingBefore;

void toggleGameVariableMidhook(safetyhook::Context& ctx) {
    // RDX, NOT RCX. toggleGameVariable is a non-static member of GameManager, so
    // on x64 RCX is `this` and the first real argument - the key - is RDX. The
    // first version read RCX and printed the GameManager OBJECT as if it were a
    // string, which is what came out in the log as "?f???": four bytes of struct
    // that happened to be printable. Three clicks produced three identical
    // pieces of garbage, which is what finally made it obvious it was not a key.
    auto const* key = reinterpret_cast<char const*>(ctx.rdx);
    if (!key) return;
    skkbot2::log::info("[GDOPT] toggleGameVariable(\"{}\")", key);
}

}  // namespace

namespace skkbot2 {

namespace {

// The two SDL-mod settings that gate the display rate. Names taken verbatim from
// the Echo reference (mac_step.cpp:62) - this is the one place in the whole
// investigation where a key is known by observation rather than by inference.
constexpr const char* kDisableVsync = "disable-vsync";
constexpr const char* kUncapFramerate = "uncap-framerate";

geode::Mod* sdlMod() {
    return geode::Loader::get()->getLoadedMod("zmx.sdl");
}

}  // namespace

std::string pacingState() {
    auto* sdl = sdlMod();
    if (!sdl) {
        return "zmx.sdl not loaded";
    }
    std::string out;
    for (auto const* key : {kDisableVsync, kUncapFramerate}) {
        if (!sdl->hasSetting(key)) {
            if (!out.empty()) out += ", ";
            out += std::string(key) + "=absent";
            continue;
        }
        if (!out.empty()) out += ", ";
        out += std::string(key) + "=" +
               (sdl->getSettingValue<bool>(key) ? "true" : "false");
    }
    return out.empty() ? "zmx.sdl exposes neither setting" : out;
}

// Turns the two settings on and remembers what they were.
//
// The user's requirement is explicit: the checkbox must END UP unticked, not just
// be reported as unticked. geode.settings is a plain JSON file the loader owns
// and Mod::setSettingValue writes through its own API, so the box does change.
//
// It does NOT change the running session. The presenter consumed the old value
// when the process started, and nothing a mod does later can hand it a new one.
// So the write is real and the effect is deferred, and both halves of that are
// logged rather than only the half that looks like success.
std::string applyPacingForPin() {
    auto* sdl = sdlMod();
    if (!sdl) {
        return {};
    }
    std::string changed;
    for (auto const* key : {kDisableVsync, kUncapFramerate}) {
        if (!sdl->hasSetting(key)) continue;
        if (sdl->getSettingValue<bool>(key)) continue;
        if (!g_pacingSaved) {
            g_pacingBefore[key] = false;
        }
        sdl->setSettingValue<bool>(key, true);
        if (!changed.empty()) changed += ", ";
        changed += key;
    }
    if (!g_pacingBefore.empty()) {
        g_pacingSaved = true;
    }
    if (changed.empty()) {
        return {};
    }
    return "enabled " + changed +
           " in the SDL mod's settings - takes effect after a restart, the "
           "presenter reads it at startup";
}

void restorePacing() {
    if (!g_pacingSaved) {
        return;
    }
    g_pacingSaved = false;
    if (auto* sdl = sdlMod()) {
        for (auto const& [key, was] : g_pacingBefore) {
            if (sdl->hasSetting(key)) {
                sdl->setSettingValue<bool>(key, was);
            }
        }
    }
    g_pacingBefore.clear();
    skkbot2::log::info("[TIMING] SDL mod pacing settings put back");
}

namespace fps {

bool installOptionKeyTracer() {
    auto* base = reinterpret_cast<uint8_t*>(geode::base::get());
    if (!base) {
        skkbot2::log::warn("[GDOPT] game module unavailable, option tracer off");
        return false;
    }
    if (kToggleGameVariableRva > 0x400000u) {
        skkbot2::log::warn("[GDOPT] offset 0x{:X} out of range, tracer off",
                           kToggleGameVariableRva);
        return false;
    }
    auto hook = safetyhook::create_mid(
        reinterpret_cast<void*>(base + kToggleGameVariableRva),
        toggleGameVariableMidhook);
    if (!hook) {
        skkbot2::log::warn("[GDOPT] could not hook toggleGameVariable, tracer off");
        return false;
    }
    g_toggleVarHook = std::move(hook);
    skkbot2::log::info("[GDOPT] option tracer armed - flip a GD setting and its "
                       "key will be printed here");
    return true;
}

}  // namespace fps
// The Windows toggle path. MoreOptionsLayer and MoreVideoOptionsLayer - the two
// classes that own GD's video options - are both marked [[link(android)]] and do
// not exist here. GJOptionsLayer does exist, and its addGVToggle(title,
// variable, description) is how GD's own options put a checkbox on a game
// variable; didToggleGV is what runs when the user ticks one.
//
//  rather than another raw offset: didToggleGV is virtual, so this cannot
// attach to the wrong byte on a different build - which is exactly how the
// RVA-based tracer produced garbage ("?f???") on its first real catch.
//
// One tick of any GD checkbox and the variable name is printed. That is the whole
// instrument: a name read off the game's own call, not reconstructed from a
// four-digit guess.
class $modify(SkkOptionTracer, GJOptionsLayer) {
    void didToggleGV(gd::string variable) {
        skkbot2::log::info("[GDOPT] GJOptionsLayer::didToggleGV(\"{}\")", variable);
        GJOptionsLayer::didToggleGV(variable);
    }
};

}  // namespace skkbot2
