#pragma once

#include "skkBot.hpp"
#include "logger/logger.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

// Shared actions for the beta UI.
//
// The UI is intentionally thin: this header only calls functionality that
// already exists in skkBot/ReplayEngine/Renderer/Logger. It does not create
// new bot features or duplicate engine logic.

namespace skkbot2::gui {

inline bool startRecording(std::string const& macroName) {
    auto* playLayer = PlayLayer::get();
    auto* engine = ReplayEngine::get();

    if (!playLayer || !playLayer->m_level) {
        return false;
    }

    if (engine->engineMode != MODE_DISABLED) {
        engine->haltExecution();
    }

    skkbot2::Logger::get().setSessionMode(
        skkbot2::Logger::SessionMode::Record
    );

    if (!engine->beginCapture(playLayer->m_level)) {
        skkbot2::Logger::get().setSessionMode(
            skkbot2::Logger::SessionMode::None
        );
        return false;
    }

    if (!macroName.empty()) {
        if (engine->activeSk) {
            engine->activeSk->name = macroName;
        }
        if (engine->activeMacro) {
            engine->activeMacro->name = macroName;
        }
    }

    return true;
}

inline void stopRecording(std::string const& macroName) {
    auto* engine = ReplayEngine::get();

    if (!macroName.empty()) {
        if (engine->activeSk) {
            engine->activeSk->name = macroName;
        }
        if (engine->activeMacro) {
            engine->activeMacro->name = macroName;
        }
    }

    // Keep the engine shutdown order used by the existing menu:
    // stop the engine first, then close the logger session.
    engine->haltExecution();
    skkbot2::Logger::get().setSessionMode(
        skkbot2::Logger::SessionMode::None
    );
}

inline bool loadMacroIntoMemory(std::string const& name) {
    auto* engine = ReplayEngine::get();

    if (name.empty()) {
        return false;
    }

    // Never swap the macro while the engine is hot: activeSk/macro are read in
    // the play/record loop every tick, so replacing them mid-session is a
    // use-after-free (row click in the beta dropdown, Load in the classic GUI
    // during execute). Only idle loads are allowed.
    if (engine->engineMode != MODE_DISABLED) {
        return false;
    }

    namespace fs = std::filesystem;
    auto dir = Mod::get()->getSaveDir() / "replays";
    auto file = dir / (name + ".skk");

    if (!fs::exists(file)) {
        return false;
    }

    std::unique_ptr<SkMacro> macro(SkMacro::loadFromPath(file));
    if (!macro) {
        return false;
    }

    delete engine->activeSk;
    engine->activeSk = macro.release();

    engine->skMode = true;
    engine->selectedAccuracyMode = engine->activeSk->accuracyMode;
    engine->enableCBSForLoadedMacroIfNeeded();

    double safeTps =
        engine->activeSk->framerate > 0.0
            ? engine->activeSk->framerate
            : engine->tickRate;

    engine->practiceFixQueue =
        skk::bridge::wireToBundles(
            engine->activeSk->practiceFixCheckpoints,
            safeTps
        );

    skkbot2::log::info(
        "P1 load(GUI): macro loaded practiceFixCheckpoints={} queueSize={}",
        engine->activeSk->practiceFixCheckpoints.size(),
        engine->practiceFixQueue.size()
    );

    return true;
}

inline void armPlayback(std::string const& name) {
    auto* engine = ReplayEngine::get();

    // A typed name must win over whatever is already loaded: if the user asks
    // for "B" while "A" sits in memory, load B (else B would never play because
    // hasMacroInputs() already returns true for A).
    if (!name.empty() && engine->getMacroName() != name) {
        if (!engine->loadMacroFromName(name)) {
            return;
        }
    }
    if (!engine->hasMacroInputs()) {
        return;
    }

    skkbot2::Logger::get().setSessionMode(
        skkbot2::Logger::SessionMode::Playback
    );

    if (!engine->requestExecutionStart()) {
        skkbot2::Logger::get().setSessionMode(
            skkbot2::Logger::SessionMode::None
        );
    }
}

inline bool startRender() {
    auto& renderer = skkbot2::render::Renderer::get();
    auto* engine = ReplayEngine::get();

    // Same gate as the classic GUI's Start Render button: a render needs a
    // level with a loaded macro, and the render engine itself only spins up
    // while it is not already recording. begin() does the deeper validation
    // (resolution/fps sanity, encoder start) and reports its error string via
    // lastError().
    // The level does NOT have to be playing: begin() arms the render and the
    // capture loop waits until the user starts/resumes the level manually
    // (manual start — begin() never restarts the level itself). If the macro
    // is armed but the level is still paused, the render simply holds until
    // the user presses play.
    if (renderer.isRecording()) {
        return false;
    }
    if (!PlayLayer::get() || !PlayLayer::get()->m_level) {
        Notification::create("Enter a level first", NotificationIcon::Error)->show();
        return false;
    }
    if (engine->engineMode != MODE_EXECUTE || !engine->activeSk) {
        Notification::create(
            "Load a macro and press Play in the bot menu first",
            NotificationIcon::Error
        )->show();
        return false;
    }

    // The GUI mirror (State::render) is applied to the live renderer on every
    // edit, so begin() sees the current settings.
    if (!renderer.begin(PlayLayer::get())) {
        Notification::create(
            fmt::format("Render start failed: {}", renderer.lastError()),
            NotificationIcon::Error
        )->show();
        return false;
    }
    return true;
}

inline void stopRender() {
    skkbot2::render::Renderer::get().stop();
}

inline ImVec2 centerPosFor(const ImVec2& size) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    return ImVec2(
        (display.x - size.x) * 0.5f,
        (display.y - size.y) * 0.5f
    );
}

inline double wallSecs() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

#ifdef GEODE_IS_WINDOWS

inline void openFolderFor(const char* path) {
    const int wlen =
        MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);

    // Size the buffer for the FULL wide length (incl. the null terminator),
    // then drop the terminator: writing `wlen` wide chars into a `wlen - 1`
    // buffer wrote one wchar past the end.
    std::wstring wide(wlen > 0 ? wlen : 0, L'\0');

    if (wlen > 0) {
        MultiByteToWideChar(
            CP_UTF8,
            0,
            path,
            -1,
            wide.data(),
            wlen
        );
        wide.pop_back();
    }

    std::wstring args = L"/select,\"" + wide + L"\"";

    ShellExecuteW(
        nullptr,
        L"open",
        L"explorer.exe",
        args.c_str(),
        nullptr,
        SW_SHOW
    );
}

#else

inline void openFolderFor(const char*) {}

#endif

} // namespace skkbot2::gui
