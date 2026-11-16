#pragma once

#include "skkBot.hpp"
#include "core/autosave_recorder.hpp"
#include "format/gdr2/gdr2.hpp"
#include "format/slc/slc.hpp"
#include "format/GBR/gbr.hpp"
#include "format/nhm/nhm.hpp"
#include "format/xe/xe.hpp"
#include "format/re4/re4.hpp"
#include "format/ttrl/ttrl.hpp"
#include "format/echo/echo.hpp"
#include "format/ast/ast.hpp"
#include "logger/logger.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>
#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

#include "gui/skkbot_gui.hpp"
#include "core/autosave_recorder.hpp"
#include "skc/skc_bot.hpp"


namespace skkbot2::gui {

inline bool startRecording(std::string const& macroName) {
    auto& gui = skkbot2::gui::get();
    if (gui.macroBotType == Gui::MacroBotType::SKC) {
        auto* playLayer = PlayLayer::get();
        if (!playLayer || !playLayer->m_level) {
            return false;
        }

        auto* engine = ReplayEngine::get();
        if (engine && engine->engineMode != MODE_DISABLED) {
            engine->haltExecution();
        }
        if (engine && engine->pendingAction != ReplayEngine::PendingAction::None) {
            engine->clearPendingAction();
        }

        auto& bot = skk::skc::Bot::get();
        if (!macroName.empty()) {
            bot.setMacroName(macroName);
        }
        bot.setTps(gui.tpsEnabled ? gui.tpsValue : 240.0);
        bot.bindLevel(playLayer->m_level);
        bot.startRecording();

        skkbot2::Logger::get().setSessionMode(
            skkbot2::Logger::SessionMode::Record
        );
        return true;
    }

    auto* playLayer = PlayLayer::get();
    auto* engine = ReplayEngine::get();

    if (!playLayer || !playLayer->m_level) {
        return false;
    }

    auto& bot = skk::skc::Bot::get();
    if (bot.isEnabled() || bot.isPlaybackArmed() || bot.getPendingAction() != skk::skc::PendingAction::None) {
        bot.stop();
        bot.clearPendingAction();
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
    auto& gui = skkbot2::gui::get();


    if (gui.macroBotType == Gui::MacroBotType::SKC) {
        auto& bot = skk::skc::Bot::get();
        if (!macroName.empty()) {
            bot.setMacroName(macroName);
        }
        bot.stop();
        skkbot2::Logger::get().setSessionMode(
            skkbot2::Logger::SessionMode::None
        );
        return;
    }

    auto* engine = ReplayEngine::get();

    if (!macroName.empty()) {
        if (engine->activeSk) {
            engine->activeSk->name = macroName;
        }
        if (engine->activeMacro) {
            engine->activeMacro->name = macroName;
        }
    }

    engine->haltExecution();
    skkbot2::Logger::get().setSessionMode(
        skkbot2::Logger::SessionMode::None
    );
}

inline bool importForeignMacroIntoSkc(std::string const& ext,
                                      std::filesystem::path const& path,
                                      std::string const& name) {
    auto foreignAdopt = [](skk::skc::Macro macro, std::string const& n,
                           uint32_t levelId = 0, std::string levelName = {}) {
        auto& gui = skkbot2::gui::get();
        if (gui.lockDelta || !gui.realTime) {
            gui.lockDelta = false;
            gui.realTime = true;
            Mod::get()->setSavedValue("gui_timing_lock_delta", false);
            Mod::get()->setSavedValue("gui_timing_real_time", true);
            dropCarry();
            skkbot2::log::info("skkBot2: foreign import -> Timing Mode REAL TIME (auto)");
        }
        return skk::skc::Bot::get().adopt(std::move(macro), n, levelId, levelName);
    };

    if (ext == ".gdr2") {
        skk::skc::Macro macro;
        skk::gdr2::Replay info;
        std::string reason;
        if (!skk::gdr2::importFile(path, macro, &info, reason)) {
            skkbot2::log::error("[GDR2] import failed: {}", reason);
            return false;
        }
        return foreignAdopt(std::move(macro), name, info.levelId, info.levelName);
    }
    if (ext == ".nhm") {
        skk::skc::Macro macro;
        std::string reason;
        if (!skk::nhm::importFile(path, macro, nullptr, reason)) {
            skkbot2::log::error("[NHM] import failed: {}", reason);
            return false;
        }
        return foreignAdopt(std::move(macro), name);
    }
    if (ext == ".xe") {
        skk::skc::Macro macro;
        std::string reason;
        if (!skk::xe::importFile(path, macro, nullptr, reason)) {
            skkbot2::log::error("[XE] import failed: {}", reason);
            return false;
        }
        return foreignAdopt(std::move(macro), name);
    }
    if (ext == ".re4") {
        skk::skc::Macro macro;
        skk::re4::Replay info;
        std::string reason;
        if (!skk::re4::importFile(path, macro, &info, reason)) {
            skkbot2::log::error("[RE4] import failed: {}", reason);
            return false;
        }
        if (!info.caution.empty()) {
            Notification::create(info.caution, NotificationIcon::Warning)->show();
        }
        return foreignAdopt(std::move(macro), name);
    }
    if (ext == ".ttrl") {
        skk::skc::Macro macro;
        std::string reason;
        if (!skk::ttrl::importFile(path, macro, nullptr, reason)) {
            skkbot2::log::error("[TTRL] import failed: {}", reason);
            return false;
        }
        return foreignAdopt(std::move(macro), name);
    }
    if (ext == ".echo") {
        skk::skc::Macro macro;
        skk::echo::Replay info;
        std::string reason;
        if (!skk::echo::importFile(path, macro, &info, reason)) {
            skkbot2::log::error("[ECHO] import failed: {}", reason);
            return false;
        }
        if (!info.caution.empty()) {
            Notification::create(
                info.caution, NotificationIcon::Warning, NOTIFICATION_LONG_TIME
            )->show();
        }
        return foreignAdopt(std::move(macro), name);
    }
if (skk::gbr::looksLikeGbrByExtension(ext)) {
        skk::skc::Macro macro;
        skk::gbr::Replay info;
        std::string reason;
        if (!skk::gbr::importFile(path, macro, &info, reason)) {
            skkbot2::log::error("[GBR] import failed: {}", reason);
            return false;
        }
        return foreignAdopt(std::move(macro), name);
    }
    if (ext == ".ast") {
        skk::skc::Macro macro;
        skk::ast::Replay info;
        std::string reason;
        if (!skk::ast::importFile(path, macro, &info, reason)) {
            skkbot2::log::error("[AST] import failed: {}", reason);
            return false;
        }
        if (!info.caution.empty()) {
            Notification::create(
                info.caution, NotificationIcon::Warning, NOTIFICATION_LONG_TIME
            )->show();
        }
        return foreignAdopt(std::move(macro), name);
    }
    skk::skc::Macro macro;
    std::string reason;
    if (!skk::slc::importFile(path, macro, nullptr, reason)) {
        skkbot2::log::error("[SLC] import failed: {}", reason);
        return false;
    }
    return foreignAdopt(std::move(macro), name);
}

inline bool loadMacroIntoMemory(std::string const& name,
                               std::string const& forcedExt = {}) {
    auto* engine = ReplayEngine::get();

    if (name.empty()) {
        return false;
    }

    if (engine && engine->engineMode != MODE_DISABLED) {
        return false;
    }

    namespace fs = std::filesystem;
    auto dir = skkbot2::paths::replays();
    auto fileSkc = dir / skk::stringToPath(name + ".skc");
    auto fileSkk = dir / skk::stringToPath(name + ".skk");

    auto fileGdr2 = dir / skk::stringToPath(name + ".gdr2");
    auto fileSlc = dir / skk::stringToPath(name + ".slc");
    auto fileGbr = dir / skk::stringToPath(name + ".brrr");
    auto fileGbrPlain = dir / skk::stringToPath(name + ".gbr");
    auto fileNhm = dir / skk::stringToPath(name + ".nhm");
    auto fileXe = dir / skk::stringToPath(name + ".xe");
    auto fileRe4 = dir / skk::stringToPath(name + ".re4");
    auto fileTtrl = dir / skk::stringToPath(name + ".ttrl");
    auto fileEcho = dir / skk::stringToPath(name + ".echo");
    auto fileAst = dir / skk::stringToPath(name + ".ast");

    auto& gui = skkbot2::gui::get();
    bool preferSkc = (gui.macroBotType == Gui::MacroBotType::SKC);

if (!forcedExt.empty()) {
        const bool isForeign = forcedExt == ".gdr2" || forcedExt == ".slc" ||
                               forcedExt == ".nhm" || forcedExt == ".xe" ||
                               forcedExt == ".re4" || forcedExt == ".ttrl" ||
                                forcedExt == ".echo" ||
                               forcedExt == ".ast" ||
                               skk::gbr::looksLikeGbrByExtension(forcedExt);
        if (forcedExt == ".skc" || isForeign) {
            gui.macroBotType = Gui::MacroBotType::SKC;
            Mod::get()->setSavedValue("gui_macro_bot_type", static_cast<int>(gui.macroBotType));
            if (isForeign) {
                return importForeignMacroIntoSkc(forcedExt, dir / skk::stringToPath(name + forcedExt), name);
            }
            return skk::skc::Bot::get().load(dir / skk::stringToPath(name + ".skc"));
        }
        std::unique_ptr<SkMacro> exact(SkMacro::loadFromPath(dir / skk::stringToPath(name + ".skk")));
        if (!exact) return false;
        gui.macroBotType = Gui::MacroBotType::SKK;
        Mod::get()->setSavedValue("gui_macro_bot_type", static_cast<int>(gui.macroBotType));
        delete engine->activeSk;
        engine->activeSk = exact.release();
        ++engine->macroGeneration;
        engine->skMode = true;
        engine->selectedAccuracyMode = engine->activeSk->accuracyMode;
        engine->enableCBSForLoadedMacroIfNeeded();
        const double exactTps = engine->activeSk->framerate > 0.0 ? engine->activeSk->framerate
                                                                 : engine->tickRate;
        engine->storedRestorePoints.clear();
        engine->practiceFixQueue =
            skk::bridge::wireToBundles(engine->activeSk->practiceFixCheckpoints, exactTps);
        return true;
    }

    if (preferSkc && fs::exists(fileSkc)) {
        return skk::skc::Bot::get().load(fileSkc);
    }
    for (const auto& pair : {std::pair{fileGdr2, ".gdr2"}, std::pair{fileSlc, ".slc"}, std::pair{fileGbrPlain, ".gbr"}, std::pair{fileGbr, ".brrr"}, std::pair{fileNhm, ".nhm"}, std::pair{fileXe, ".xe"}, std::pair{fileRe4, ".re4"}, std::pair{fileTtrl, ".ttrl"},
                       std::pair{fileEcho, ".echo"},
                       std::pair{fileAst, ".ast"}}) {
        if (fs::exists(pair.first)) {
            gui.macroBotType = Gui::MacroBotType::SKC;
            Mod::get()->setSavedValue("gui_macro_bot_type",
                                      static_cast<int>(gui.macroBotType));
            return importForeignMacroIntoSkc(pair.second, pair.first, name);
        }
    }
    if (!preferSkc && fs::exists(fileSkk)) {
        std::unique_ptr<SkMacro> macro(SkMacro::loadFromPath(fileSkk));
        if (!macro) return false;
        delete engine->activeSk;
        engine->activeSk = macro.release();
        ++engine->macroGeneration;
        engine->skMode = true;
        engine->selectedAccuracyMode = engine->activeSk->accuracyMode;
        engine->enableCBSForLoadedMacroIfNeeded();
        double safeTps = engine->activeSk->framerate > 0.0 ? engine->activeSk->framerate : engine->tickRate;
        engine->storedRestorePoints.clear();
        engine->practiceFixQueue = skk::bridge::wireToBundles(engine->activeSk->practiceFixCheckpoints, safeTps);
        skkbot2::log::info("P1 load(GUI): macro loaded practiceFixCheckpoints={} queueSize={}",
                           engine->activeSk->practiceFixCheckpoints.size(),
                           engine->practiceFixQueue.size());
        return true;
    }

    if (fs::exists(fileSkc)) {
        gui.macroBotType = Gui::MacroBotType::SKC;
        Mod::get()->setSavedValue("gui_macro_bot_type", static_cast<int>(gui.macroBotType));
        return skk::skc::Bot::get().load(fileSkc);
    }
    if (fs::exists(fileSkk)) {
        gui.macroBotType = Gui::MacroBotType::SKK;
        Mod::get()->setSavedValue("gui_macro_bot_type", static_cast<int>(gui.macroBotType));
        std::unique_ptr<SkMacro> macro(SkMacro::loadFromPath(fileSkk));
        if (!macro) return false;
        delete engine->activeSk;
        engine->activeSk = macro.release();
        ++engine->macroGeneration;
        engine->skMode = true;
        engine->selectedAccuracyMode = engine->activeSk->accuracyMode;
        engine->enableCBSForLoadedMacroIfNeeded();
        double safeTps = engine->activeSk->framerate > 0.0 ? engine->activeSk->framerate : engine->tickRate;
        engine->storedRestorePoints.clear();
        engine->practiceFixQueue = skk::bridge::wireToBundles(engine->activeSk->practiceFixCheckpoints, safeTps);
        skkbot2::log::info("P1 load(GUI): macro loaded practiceFixCheckpoints={} queueSize={}",
                           engine->activeSk->practiceFixCheckpoints.size(),
                           engine->practiceFixQueue.size());
        return true;
    }

    return false;
}

inline void armPlayback(std::string const& name) {
    auto& gui = skkbot2::gui::get();
    if (gui.macroBotType == Gui::MacroBotType::SKC) {
        auto& bot = skk::skc::Bot::get();

        skkbot2::log::info("[SKC] PLAY: checkpoint 0 (armPlayback entry, "
                           "name='{}', bot state={})",
                           name, static_cast<int>(bot.getState()));
        skkbot2::Logger::get().flush();

        if (bot.isEnabled() || bot.isPlaybackArmed()) {
            bot.stop();
        }

        if (!name.empty() && bot.getMacroName() != name) {
            auto dir = skkbot2::paths::replays();
            if (!bot.load(dir / skk::stringToPath(name + ".skc"))) {
                if (!loadMacroIntoMemory(name)) {
                    skkbot2::log::error(
                        "[SKC] PLAY: cannot load any '{}.skc/.skk/.slc/... "
                        "from {}' - no such library entry",
                        name, skk::pathToUtf8(dir));
                }
            }
        } else if (name.empty()) {
            skkbot2::log::warn(
                "[SKC] PLAY: macro name is empty - pick a row in the "
                "library (or type a name) before playback");
            if (!bot.hasInputs()) {
                return;
            }
            skkbot2::log::info("[SKC] PLAY: empty name, replaying the macro "
                               "already in memory ({})",
                               bot.getMacroName());
        }
        skkbot2::log::info("[SKC] PLAY: checkpoint A2 (load done: {})",
                           bot.getMacroName());
        skkbot2::Logger::get().flush();
        skkbot2::log::info("[SKC] PLAY: checkpoint A (inputs={}, engine={})",
                           bot.getInputCount(),
                           [&] {
                               auto* e = ReplayEngine::get();
                               return e ? static_cast<int>(e->engineMode) : -1;
                           }());
        skkbot2::Logger::get().flush();
        if (!bot.hasInputs()) {
            skkbot2::log::error(
                "[SKC] PLAY refused: the bot holds no actions for '{}' "
                "(load failed or the import was refused - see the "
                "earlier [SLC]/[SKC] lines)",
                name);
            skkbot2::Logger::get().flush();
            return;
        }

        auto* engine = ReplayEngine::get();
        if (engine && engine->engineMode != MODE_DISABLED) {
            skkbot2::log::info("[SKC] PLAY: calling haltExecution() (mode={})",
                               static_cast<int>(engine->engineMode));
            skkbot2::Logger::get().flush();
            engine->haltExecution();
            skkbot2::log::info("[SKC] PLAY: haltExecution() returned");
            skkbot2::Logger::get().flush();
        } else {
            skkbot2::log::info("[SKC] PLAY: engineMode==MODE_DISABLED, skipping haltExecution()");
            skkbot2::Logger::get().flush();
        }
        skkbot2::log::info("[SKC] PLAY: checkpoint B (engine halted/cleared)");
        skkbot2::Logger::get().flush();
        skkbot2::log::info("[SKC] PLAY: checkpoint B2 (before setSessionMode)");
        skkbot2::Logger::get().flush();
        if (engine && engine->pendingAction != ReplayEngine::PendingAction::None) {
            skkbot2::log::info("[SKC] PLAY: clearing pendingAction (was={})",
                               static_cast<int>(engine->pendingAction));
            skkbot2::Logger::get().flush();
            engine->clearPendingAction();
        }

        skkbot2::Logger::get().setSessionMode(
            skkbot2::Logger::SessionMode::Playback
        );
        skkbot2::log::info("[SKC] PLAY: checkpoint C0 (after setSessionMode)");
        skkbot2::Logger::get().flush();
        skkbot2::log::info("[SKC] PLAY: checkpoint C (session mode set, mode={})",
                           gui.lockDelta ? "Lock Delta" : "Real Time");
        skkbot2::Logger::get().flush();

        auto* playLayer = PlayLayer::get();
        if (!playLayer) {
            skkbot2::log::info("[SKC] PLAY: checkpoint D (no PlayLayer -> arm)");
            skkbot2::Logger::get().flush();
            bot.armPlayback();
            return;
        }

        skk::skc::Bot::PlaybackStart start{};
        if (bot.startPlaybackIfReady(&start)) {
            skkbot2::log::info("[SKC] PLAY: checkpoint E (startPlaybackIfReady "
                               "true, firstActionFrame={})", start.firstActionFrame);
            skkbot2::Logger::get().flush();
            skkbot2::log::info("[SKC] PLAY started immediately: attempt is at "
                               "frame 0, macro begins at frame {}",
                               start.firstActionFrame);
            skkbot2::Logger::get().flush();
            return;
        }

        if (start.decision == skk::skc::Bot::StartDecision::MidAttempt) {
            bot.armPlayback();
            const std::string why = bot.describeStartBlock(start);
            skkbot2::log::warn("{}", why);
            skkbot2::Logger::get().error("{}", why);
            return;
        }

        if (start.decision == skk::skc::Bot::StartDecision::WrongMode) {
            const std::string why = bot.describeStartBlock(start);
            skkbot2::log::warn("{}", why);
            skkbot2::Logger::get().error("{}", why);
            return;
        }

        skkbot2::log::info("[SKC] PLAY: checkpoint F (immediate start path: "
                           "startPlayback + resetLevel)");
            skkbot2::Logger::get().flush();
        bot.startPlayback();
            skkbot2::log::info("[SKC] PLAY: after startPlayback()");
            skkbot2::Logger::get().flush();
        if (playLayer->m_levelSettings && playLayer->m_levelSettings->m_platformerMode) {
            playLayer->resetLevelFromStart();
        } else {
            playLayer->resetLevel();
        }
            skkbot2::log::info("[SKC] PLAY: after resetLevel()");
            skkbot2::Logger::get().flush();
        return;
    }

    auto& bot = skk::skc::Bot::get();
    if (bot.isEnabled() || bot.isPlaybackArmed() || bot.getPendingAction() != skk::skc::PendingAction::None) {
        bot.stop();
        bot.clearPendingAction();
    }

    auto* engine = ReplayEngine::get();

    if (!name.empty() && engine->getMacroName() != name) {
        if (!engine->loadMacroFromName(name)) {
            const std::string memName = engine->getMacroName();
            if (engine->hasMacroInputs() &&
                (engine->hasUnsavedActiveMacro() || memName.empty() ||
                 memName == name)) {
                skkbot2::log::info(
                    "[SKK] PLAY: '{}.skk' is not on disk yet - playing the "
                    "in-memory macro (name='{}')",
                    name, memName);
            } else {
                skkbot2::log::error(
                    "[SKK] PLAY: cannot load '{}.skk' from the replays "
                    "directory",
                    name);
                return;
            }
        }
    } else if (name.empty()) {
        skkbot2::log::warn(
            "[SKK] PLAY: macro name is empty - pick a row in the "
            "library (or type a name) before playback");
    }
    if (!engine->hasMacroInputs()) {
        skkbot2::log::error(
            "[SKK] PLAY refused: '{}' holds no actions (load failed or "
            "the macro carries no clicks)",
            name);
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

    if (renderer.isRecording()) {
        return false;
    }
    auto& gui = skkbot2::gui::get();
    const bool skc = gui.macroBotType == Gui::MacroBotType::SKC;
    auto& skcBot = skk::skc::Bot::get();
    const bool skcRunning = skcBot.getState() == skk::skc::State::Playback ||
                            skcBot.isPlaybackArmed();

    if (!PlayLayer::get() || !PlayLayer::get()->m_level) {
        Notification::create("Enter a level first", NotificationIcon::Error)->show();
        return false;
    }
    const bool skkRunning = engine->engineMode == MODE_EXECUTE && engine->activeSk;
    if (!skkRunning && !skcRunning) {
        Notification::create(
            "Load a macro and press Play in the bot menu first",
            NotificationIcon::Error
        )->show();
        return false;
    }

    if (!renderer.begin(PlayLayer::get())) {
        Notification::create(
            fmt::format("Render start failed: {}", renderer.lastError()),
            NotificationIcon::Error
        )->show();
        return false;
    }
    if (skc) {
        Notification::create(
            "SKC: no death chain - restart from frame 0",
            NotificationIcon::Info, NOTIFICATION_LONG_TIME
        )->show();
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

inline void openFolderFor(std::filesystem::path const& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) return;

    std::wstring args;
    const DWORD attrs = GetFileAttributesW(wide.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        args = L"/select,\"" + wide + L"\"";
    } else {
        const std::size_t slash = wide.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return;
        }
        std::wstring parent = wide.substr(0, slash);
        args = L"\"" + parent + L"\"";
    }

    ShellExecuteW(
        nullptr,
        L"open",
        L"explorer.exe",
        args.c_str(),
        nullptr,
        SW_SHOW
    );
}

inline void openFolderFor(const char* path) {
    if (!path || !*path) return;
    openFolderFor(skk::stringToPath(path));
}

#else

inline void openFolderFor(std::filesystem::path const&) {}
inline void openFolderFor(const char*) {}

#endif

inline void openFolderInExplorer(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

#ifdef GEODE_IS_WINDOWS
    const std::wstring wide = dir.wstring();
    if (wide.empty()) return;
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + wide + L"\"").c_str(), nullptr, SW_SHOW);
#else
    (void)dir;
#endif
}

}
