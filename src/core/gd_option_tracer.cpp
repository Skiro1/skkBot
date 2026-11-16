#include "core/playback_fps.hpp"

#include "logger/logger.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJOptionsLayer.hpp>
#include <Geode/modify/GJOptionsLayer.hpp>

#include <safetyhook.hpp>

#include "version_offsets.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace {


safetyhook::MidHook g_toggleVarHook;

bool g_pacingSaved = false;
std::map<std::string, bool> g_pacingBefore;

void toggleGameVariableMidhook(safetyhook::Context& ctx) {
    auto const* key = reinterpret_cast<char const*>(ctx.rdx);
    if (!key) return;
    skkbot2::log::info("[GDOPT] toggleGameVariable(\"{}\")", key);
}

}

namespace skkbot2 {

namespace {

constexpr const char* kDisableVsync = "disable-vsync";
constexpr const char* kUncapFramerate = "uncap-framerate";

geode::Mod* sdlMod() {
    return geode::Loader::get()->getLoadedMod("zmx.sdl");
}

}

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
#ifdef GEODE_IS_WINDOWS
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const size_t moduleSize = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
    auto addr = skk::offsets::resolve(skk::offsets::kToggleGameVariable, base, moduleSize);
    if (!addr) {
        skkbot2::log::warn("[GDOPT] toggleGameVariable не резолвится (RVA 0x{:X}, "
                           "сигнатура не совпала), tracer off",
                           skk::offsets::kToggleGameVariable.rva);
        return false;
    }
    auto hook = safetyhook::create_mid(reinterpret_cast<void*>(*addr),
                                       toggleGameVariableMidhook);
#else
    auto hook = safetyhook::MidHook{};
#endif
    if (!hook) {
        skkbot2::log::warn("[GDOPT] could not hook toggleGameVariable, tracer off");
        return false;
    }
    g_toggleVarHook = std::move(hook);
    skkbot2::log::info("[GDOPT] option tracer armed - flip a GD setting and its "
                       "key will be printed here");
    return true;
}

}

}
