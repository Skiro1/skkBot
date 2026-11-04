#pragma once

#include "skkBot.hpp"
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

// Shared actions for the beta UI.
//
// The UI is intentionally thin: this header only calls functionality that
// already exists in skkBot/ReplayEngine/Renderer/Logger. It does not create
// new bot features or duplicate engine logic.

namespace skkbot2::gui {

inline bool startRecording(std::string const& macroName) {
    auto& gui = skkbot2::gui::get();
    if (gui.macroBotType == Gui::MacroBotType::SKC) {
        // Same gate as the SKK branch: recording needs a live level.
        auto* playLayer = PlayLayer::get();
        if (!playLayer || !playLayer->m_level) {
            return false;
        }

        // Mutual exclusion, same as SKK's startRecording: stop the other bot
        // first so one transport can never fight the other.
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
        // Starting a fresh recording discards whatever was in memory - routed
        // through the single deletion owner so it gets logged like every other
        // wipe instead of silently clearing.
        bot.onResetEvent(skk::skc::Bot::ResetReason::UserClear, 0);
        bot.startRecording();

        // SKK's beginCapture does NOT restart the level - the macro records
        // from the current position. SKC stores only frame deltas, so a
        // mid-level recording just starts at a non-zero frame; same rule.
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

    // Final autosave snapshot BEFORE the engines stop: flushRecording() is
    // gated on "is anything recording" on purpose (autosave must never write
    // from playback/idle), so stopping first would make it a no-op. Called
    // while isRecording()/MODE_CAPTURE still report Record, this guarantees
    // the rescue file on disk matches the end of the recording, not whatever
    // the last periodic tick (up to 10 s earlier) caught.
    skkbot2::autosave::flushRecording();

    if (gui.macroBotType == Gui::MacroBotType::SKC) {
        auto& bot = skk::skc::Bot::get();
        if (!macroName.empty()) {
            bot.setMacroName(macroName);
        }
        // Same order as the SKK branch: stop the bot first, then close the
        // logger session. The macro stays in memory (unsaved) until SAVE.
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

    // Keep the engine shutdown order used by the existing menu:
    // stop the engine first, then close the logger session.
    engine->haltExecution();
    skkbot2::Logger::get().setSessionMode(
        skkbot2::Logger::SessionMode::None
    );
}

inline bool importForeignMacroIntoSkc(std::string const& ext,
                                      std::filesystem::path const& path,
                                      std::string const& name) {
    // Каждый чужой импортированный макрос автоматически ставит TIMING MODE
    // в Real Time (запрос 03.11.2026): иностранные форматы не несут нашей
    // метрики Lock Delta, так что честная воспроизводимость для них —
    // только по реальному времени. Пишется и в GUI-состояние, и в
    // saved-ключи (те же, что у пилюль Mode), плюс dropCarry() — тот же
    // контекст обновления, что у ручного переключения.
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

    // Both foreign formats are import-only and both land in SKC: GDR2's Input
    // and SLC's Action are the same four fields our Macro carries, and neither
    // format stores physics. From the moment they load they ARE SKC macros,
    // converted once and kept in memory until saved as .skc.
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
    // Any of the ten GBR6 aliases, not just .brrr - the container is identical
    // and matching on the extension list keeps the nine siblings working
    // without a branch each.
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
        // Astral Lite. The reader accounts for the whole payload apart from
        // zero padding, so a refusal means "not understood", not "try again" -
        // and a macro it does accept is frame-exact rather than plausible.
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
    // SLC v3 metadata carries no level identity to bind against, so the macro
    // stays unbound: onLevelEnter() treats that as "no level to compare", which
    // is the honest state - we genuinely do not know which level it is from.
    return foreignAdopt(std::move(macro), name);
}

inline bool loadMacroIntoMemory(std::string const& name,
                               std::string const& forcedExt = {}) {
    auto* engine = ReplayEngine::get();

    if (name.empty()) {
        return false;
    }

    // Never swap the macro while the engine is hot: activeSk/macro are read in
    // the play/record loop every tick, so replacing them mid-session is a
    // use-after-free (row click in the beta dropdown, Load in the classic GUI
    // during execute). Only idle loads are allowed.
    if (engine && engine->engineMode != MODE_DISABLED) {
        return false;
    }

    namespace fs = std::filesystem;
    auto dir = skkbot2::paths::replays();
    auto fileSkc = dir / (name + ".skc");
    auto fileSkk = dir / (name + ".skk");

    auto fileGdr2 = dir / (name + ".gdr2");
    auto fileSlc = dir / (name + ".slc");
    auto fileGbr = dir / (name + ".brrr");
    auto fileGbrPlain = dir / (name + ".gbr");
    auto fileNhm = dir / (name + ".nhm");
    auto fileXe = dir / (name + ".xe");
    auto fileRe4 = dir / (name + ".re4");
    auto fileTtrl = dir / (name + ".ttrl");
    auto fileEcho = dir / (name + ".echo");
    auto fileAst = dir / (name + ".ast");

    auto& gui = skkbot2::gui::get();
    bool preferSkc = (gui.macroBotType == Gui::MacroBotType::SKC);

    // A row in the library points at ONE concrete file. When the caller says
    // which extension that row represented (both DF2.skk and DF2.skc can exist),
    // load exactly that one instead of inferring it from the bot-engine pill.
if (!forcedExt.empty()) {
        // Every foreign extension must reach the importer. A GBR6 alias slipped
        // past this once: the row was recognised as SKC-side, but the branch
        // below only named .gdr2 and .slc, so a .maybach fell through to
        // load(name + ".skc"), found nothing, and the macro silently stayed
        // empty - the library said it was there and playback did nothing.
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
                return importForeignMacroIntoSkc(forcedExt, dir / (name + forcedExt), name);
            }
            return skk::skc::Bot::get().load(dir / (name + ".skc"));
        }
        std::unique_ptr<SkMacro> exact(SkMacro::loadFromPath(dir / (name + ".skk")));
        if (!exact) return false;
        gui.macroBotType = Gui::MacroBotType::SKK;
        Mod::get()->setSavedValue("gui_macro_bot_type", static_cast<int>(gui.macroBotType));
        delete engine->activeSk;
        engine->activeSk = exact.release();
        engine->skMode = true;
        engine->selectedAccuracyMode = engine->activeSk->accuracyMode;
        engine->enableCBSForLoadedMacroIfNeeded();
        const double exactTps = engine->activeSk->framerate > 0.0 ? engine->activeSk->framerate
                                                                 : engine->tickRate;
        engine->practiceFixQueue =
            skk::bridge::wireToBundles(engine->activeSk->practiceFixCheckpoints, exactTps);
        return true;
    }

    if (preferSkc && fs::exists(fileSkc)) {
        return skk::skc::Bot::get().load(fileSkc);
    }
    // No extension given (the classic GUI's Load button). A .gdr2 is imported
    // into SKC, so it belongs on the SKC side of the preference check rather
    // than after it - a macro that only exists as GDR2 must still load.
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
        engine->skMode = true;
        engine->selectedAccuracyMode = engine->activeSk->accuracyMode;
        engine->enableCBSForLoadedMacroIfNeeded();
        double safeTps = engine->activeSk->framerate > 0.0 ? engine->activeSk->framerate : engine->tickRate;
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
        engine->skMode = true;
        engine->selectedAccuracyMode = engine->activeSk->accuracyMode;
        engine->enableCBSForLoadedMacroIfNeeded();
        double safeTps = engine->activeSk->framerate > 0.0 ? engine->activeSk->framerate : engine->tickRate;
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

        // ReplayEngine::loadMacroFromName refuses to swap a macro while the
        // engine is hot; the SKC bot is a separate engine, but the same rule
        // applies for the same reason (poll() reads the list every step).
        if (bot.isEnabled() || bot.isPlaybackArmed()) {
            bot.stop();
        }

        // A typed name must win over whatever is already loaded, same as the
        // SKK branch.
        // A native .skc is only one of the library's sources: a foreign import
        // (.slc/.echo/...) exists as its RAW FILE only - adopt() keeps it in
        // memory and dies with the GD process. So when the .skc miss comes
        // from that, run the universal loader (the same one the library row
        // click uses) before giving up. Without this, pressing PLAY right
        // after a GD restart refused silently: the .slc sat in the library,
        // the bot was empty, and nothing - button, log, toast - said why (the
        // user-visible "PLAY не включается" report of 31.10, Windy Road.slc).
        if (!name.empty() && bot.getMacroName() != name) {
            auto dir = skkbot2::paths::replays();
            if (!bot.load(dir / (name + ".skc"))) {
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
            // BUG-98: with nothing loaded the engine must not be touched at
            // all. The 05:59 session died silently right after the warn
            // (crashlog empty) - the empty-name run of 05:36 survived the
            // same path because it still held inputs; an empty bot going on
            // to halt/clear/pause plumbing is the suspect. Refuse here.
            if (!bot.hasInputs()) {
                return;
            }
            skkbot2::log::info("[SKC] PLAY: empty name, replaying the macro "
                               "already in memory ({})",
                               bot.getMacroName());
        }
        skkbot2::log::info("[SKC] PLAY: checkpoint A (inputs={}, engine={})",
                           bot.getInputCount(),
                           [&] {
                               auto* e = ReplayEngine::get();
                               return e ? static_cast<int>(e->engineMode) : -1;
                           }());
        if (!bot.hasInputs()) {
            skkbot2::log::error(
                "[SKC] PLAY refused: the bot holds no actions for '{}' "
                "(load failed or the import was refused - see the "
                "earlier [SLC]/[SKC] lines)",
                name);
            return;
        }

        auto* engine = ReplayEngine::get();
        if (engine && engine->engineMode != MODE_DISABLED) {
            engine->haltExecution();
        }
        skkbot2::log::info("[SKC] PLAY: checkpoint B (engine halted/cleared)");
        // A queued SKK Record/Play left over from earlier must not fire on the
        // next level entry while SKC owns the transport.
        if (engine && engine->pendingAction != ReplayEngine::PendingAction::None) {
            engine->clearPendingAction();
        }

        skkbot2::Logger::get().setSessionMode(
            skkbot2::Logger::SessionMode::Playback
        );
        skkbot2::log::info("[SKC] PLAY: checkpoint C (session mode set, mode={})",
                           gui.lockDelta ? "Lock Delta" : "Real Time");

        // Does the macro have anywhere to play from RIGHT HERE? The old rule was
        // `runActive`, which is true for the most common case of all - a normal
        // level, attempt just started, player alive - so PLAY did nothing until
        // the player died. The SKC clock separates those cases; `runActive`
        // could not.
        auto* playLayer = PlayLayer::get();
        if (!playLayer) {
            // Editor build mode (or menu): arm now and let the playtest-start
            // / PlayLayer::init hook align the spawn when physics begin.
            skkbot2::log::info("[SKC] PLAY: checkpoint D (no PlayLayer -> arm)");
            bot.armPlayback();
            return;
        }

        skk::skc::Bot::PlaybackStart start{};
        if (bot.startPlaybackIfReady(&start)) {
            // Started on the spot. No arming, no reset, no waiting for a death.
            skkbot2::log::info("[SKC] PLAY: checkpoint E (startPlaybackIfReady "
                               "true, firstActionFrame={})", start.firstActionFrame);
            skkbot2::log::info("[SKC] PLAY started immediately: attempt is at "
                               "frame 0, macro begins at frame {}",
                               start.firstActionFrame);
            return;
        }

        if (start.decision == skk::skc::Bot::StartDecision::MidAttempt) {
            // Genuinely unplayable here - the clicks would land on a player the
            // macro never positioned. Arm anyway so it takes over on the next
            // attempt WITHOUT the user having to ask again, and say out loud
            // why it did not start now. Silently doing nothing was the bug.
            bot.armPlayback();
            // Reason to the log only - no GD toast (removed 19.10.2026 on
            // request). "Armed" is visible in the transport, and the log says
            // why it did not start and what frame it is at.
            const std::string why = bot.describeStartBlock(start);
            skkbot2::log::warn("{}", why);
            skkbot2::Logger::get().error("{}", why);
            return;
        }

        if (start.decision == skk::skc::Bot::StartDecision::WrongMode) {
            // Arming would be wrong too: the next attempt is on the same level,
            // so the mode would still be wrong. Say it and stop.
            const std::string why = bot.describeStartBlock(start);
            skkbot2::log::warn("{}", why);
            skkbot2::Logger::get().error("{}", why);
            return;
        }

        // Dead, finished, or practice respawn: the old immediate path, which
        // restarts the level and plays from frame 0. Unchanged.
        skkbot2::log::info("[SKC] PLAY: checkpoint F (immediate start path: "
                           "startPlayback + resetLevel)");
        bot.startPlayback();
        if (playLayer->m_levelSettings && playLayer->m_levelSettings->m_platformerMode) {
            playLayer->resetLevelFromStart();
        } else {
            playLayer->resetLevel();
        }
        return;
    }

    auto& bot = skk::skc::Bot::get();
    if (bot.isEnabled() || bot.isPlaybackArmed() || bot.getPendingAction() != skk::skc::PendingAction::None) {
        bot.stop();
        bot.clearPendingAction();
    }

    auto* engine = ReplayEngine::get();

    // A typed name must win over whatever is already loaded: if the user asks
    // for "B" while "A" sits in memory, load B (else B would never play because
    // hasMacroInputs() already returns true for A).
    if (!name.empty() && engine->getMacroName() != name) {
        if (!engine->loadMacroFromName(name)) {
            skkbot2::log::error(
                "[SKK] PLAY: cannot load '{}.skk' from the replays "
                "directory",
                name);
            return;
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

    // Same gate as the classic GUI's Start Render button: a render needs a
    // level with a loaded macro, and the render engine itself only spins up
    // while it is not already recording. begin() does the deeper validation
    // (resolution/fps sanity, encoder start) and reports its error string via
    // lastError().
    // The level does NOT have to be playing: begin() arms the render and the
    // capture loop waits until the user starts/resumes the level manually
    // (manual start Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р Р†РІР‚С›РЎС›Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р РЋРІвЂћСћР В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р Р†РІР‚С›РЎС›Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В Р Р‹Р Р†РІР‚С›РЎС›Р В Р’В Р вЂ™Р’В Р В Р’В Р В РІР‚в„–Р В Р’В Р В РІР‚В Р В Р вЂ Р В РІР‚С™Р РЋРІР‚С”Р В Р Р‹Р РЋРІР‚С”Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р РЋРІвЂћСћР В Р’В Р В Р вЂ№Р В Р вЂ Р Р†Р вЂљРЎвЂєР РЋРЎвЂєР В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћвЂ“Р В Р’В Р вЂ™Р’В Р В Р’В Р В РІР‚в„–Р В Р’В Р В Р вЂ№Р В Р вЂ Р Р†Р вЂљРЎвЂєР РЋРЎвЂє begin() never restarts the level itself). If the macro
    // is armed but the level is still paused, the render simply holds until
    // the user presses play.
    if (renderer.isRecording()) {
        return false;
    }
    auto& gui = skkbot2::gui::get();
    const bool skc = gui.macroBotType == Gui::MacroBotType::SKC;
    auto& skcBot = skk::skc::Bot::get();
    const bool skcRunning = skcBot.getState() == skk::skc::State::Playback ||
                            skcBot.isPlaybackArmed();

    // SKC is accepted here too. The renderer itself is engine-agnostic - it
    // grabs frames and hands them to FFmpeg - so the old gate (MODE_EXECUTE &&
    // activeSk, i.e. SKK-only) rejected a playback that renders perfectly well.
    // SKC does NOT keep a death chain, so a render longer than the macro will
    // restart from frame 0 rather than resume; say so up front instead of
    // letting it look like a capture bug.
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

    // The GUI mirror (State::render) is applied to the live renderer on every
    // edit, so begin() sees the current settings.
    if (!renderer.begin(PlayLayer::get())) {
        Notification::create(
            fmt::format("Render start failed: {}", renderer.lastError()),
            NotificationIcon::Error
        )->show();
        return false;
    }
    if (skc) {
        // Geode's Notification is a CCLabelBMFont: one line, no wrapping, so a
        // long sentence simply does not fit. Keep it short.
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

// Open a DIRECTORY in the system file manager.
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

} // namespace skkbot2::gui
