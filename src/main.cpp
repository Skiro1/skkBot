#include <imgui.h>

#include "skkBot.hpp"
#include "core/hotpath.hpp"
#include "core/playback_fps.hpp"
#include "format/skk3_format.hpp"
#include "skc/skc_bot.hpp"
#include "skc/skc_format.hpp"
#include "gui/blur.hpp"
#include "core/paths.hpp"
#include "gui/skkbot_gui.hpp"
#include "hacks/practice_music_sync.hpp"
#include "logger/logger.hpp"
#include "render/renderer.hpp"
#ifdef GEODE_IS_WINDOWS
#include "physics/step_grid_hooks.hpp"
#include "core/rng_lock.hpp"
#endif

#include <Geode/Geode.hpp>
#include <Geode/utils/Keyboard.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/utils/string.hpp>
#include <imgui-cocos.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

using namespace geode::prelude;
using namespace skkbot2::gui;

$on_mod(Loaded) {
    auto& logger = skkbot2::Logger::get();
    // INFO, not Debug: the diag push/release lines fire up to ~14k times a
    // second in a dual playback (Schafs), and every line above the level still
    // pays fmt::format. At Debug that measured as a share of the frame budget
    // hiding inside the [HOT] "GD base scheduler" number (BUG-70 session,
    // 21:45). The instrument stays: raise the level in the GUI (Log Level)
    // when a button-seam or drift hunt needs the flood back.
    logger.setLevel(skkbot2::Logger::Info);
    logger.setFileLogging(true);

    logger.info("========================================");
    logger.info("skkBot2 v{} initializing (M1: SKK3 Record/Play + TPS)", MOD_VERSION);
    logger.info("========================================");

    namespace fs = std::filesystem;
    // Ensure all standard mod data folders exist from startup
    auto failedDirs = skkbot2::paths::ensureDataFolders();
    for (auto const& f : failedDirs) {
        logger.error("Failed to create mod data folder: {}", skk::pathToUtf8(f));
    }
    skkbot2::render::Renderer::prepareFfmpegDropIn();

    for (auto const& [label, p] : skkbot2::paths::summary()) {
        logger.info("[PATHS] {:<10} -> {}", label, skk::pathToUtf8(p));
    }

    // Per-frame v4 codec self-check: synthetic round-trip + v3/v4 size ratio.
    auto pf = skk::skk3::selfTestPerFrameCodec();
    logger.info(
        "skk3 per-frame codec v4 self-test: ok={} frames={} v3={}B v4={}B ratio={:.1f}x",
        pf.ok, pf.frames, pf.v3Bytes, pf.v4Bytes,
        pf.v4Bytes > 0 ? static_cast<double>(pf.v3Bytes) / static_cast<double>(pf.v4Bytes) : 0.0);
    if (!pf.ok) {
        using Stage = skk::skk3::PerFrameSelfTestResult::Stage;
        auto stageName = [&]() -> const char* {
            switch (pf.stage) {
                case Stage::ReadFailed: return "read-failed";
                case Stage::CountMismatch: return "count-mismatch";
                case Stage::FieldMismatch: return "field-mismatch";
                case Stage::RewriteMismatch: return "rewrite-mismatch";
                default: return "unknown";
            }
        };
        logger.error("skk3 per-frame codec v4 self-test FAILED - do not trust new .skk files "
            "(stage={} failIdx={} expected={} decoded={} detail={})",
            stageName(), pf.failIdx, pf.expected, pf.decoded, pf.failDetail);
    }
    // Header self-test. Separate from the codec one on purpose: that one
    // checks that new files are written and read consistently, and this one
    // checks the promise the other half of the change rests on - that .skk
    // files written before the display rate existed keep loading. A codec
    // round-trip cannot catch that, because it only ever sees current files.
    auto hd = skk::skk3::selfTestHeader();
    logger.info(
        "skk3 header self-test: ok={} oldShape={}B", hd.ok, hd.oldShapeHeaderBytes);
    if (!hd.ok) {
        logger.error("skk3 header self-test FAILED - {}",
            hd.detail.empty() ? "no detail" : hd.detail);
    }

    skk::skc::Bot::get().setIgnoreInputs(
        Mod::get()->getSavedValue<bool>("gui_skc_ignore_inputs", true)
    );

    // .skc codec self-check, same idea as the skk3 one above: a broken
    // recorder would silently write unreadable macros.
    if (!skk::skc::selfTest()) {
        logger.error("skc codec self-test FAILED - do not trust new .skc files");
    } else {
        logger.info("skc codec self-test ok");
    }

#ifdef GEODE_IS_WINDOWS
    // Install the step-grid midhooks NOW (module load), never lazily from
    // inside GJBaseGameLayer::update - patching trampoline bytes on the same
    // thread currently executing the target function is a freeze.
    skk::installStepGridHooks();
    // Read-only. Prints the key behind every option GD's own settings flip, so
    // the vertical-sync switch can be identified by observation. See
    // src/core/gd_option_tracer.cpp for why it is a tracer and not a guess.
    skkbot2::fps::installOptionKeyTracer();

    // Install RNG midhooks (teleport LCG interception) at module load.
    skk::installRngMidhooks();
#endif

    skkbot2::gui::get().init();

    // If the user's FPS toggle is OFF, any previous session that left VSync
    // disabled (game crashed while a pin was active, or the user quit mid-pin)
    // needs to be cleaned up. This runs after gui.init() which loads
    // gui_fps_enabled and pushes it into the fps module, so the module's own
    // g_enabled is already the user's intended state.
    if (!skkbot2::fps::enabled()) {
        skkbot2::fps::restoreVsyncOnStartup();
    }

    ImGuiCocos::get()
        .setup([] {
            // Rasterize a real TTF at a DPI-aware size so text is crisp (the
            // default imgui font is tiny and gets upscaled -> blurry), and so
            // Cyrillic renders instead of boxes.
            // Custom font: any TTF in the `fonts` folder can be picked via COSMETICS;
            // falls back to the bundled Roboto (`font_main.ttf`).
            auto& io = ImGui::GetIO();
            float factor = std::max(1.0f, geode::utils::getDisplayFactor());
            int fontSize = static_cast<int>(std::round(18.0f * factor));

            const auto resDir = skkbot2::paths::resources();
            const auto fontsFolder = skkbot2::paths::fonts();
            std::error_code ec;
            std::filesystem::create_directories(fontsFolder, ec);

            std::string customFont = Mod::get()->getSavedValue<std::string>("gui_custom_font", "");
            ImFont* loaded = nullptr;

            if (!customFont.empty() && customFont != "Default") {
                const auto customPath = fontsFolder / customFont;
                if (std::filesystem::exists(customPath, ec)) {
                    ImFontConfig customCfg;
                    customCfg.OversampleH = 3;
                    customCfg.OversampleV = 3;
                    std::string fontPathStr = geode::utils::string::pathToString(customPath);
                    loaded = io.Fonts->AddFontFromFileTTF(
                        fontPathStr.c_str(), static_cast<float>(fontSize), &customCfg,
                        io.Fonts->GetGlyphRangesCyrillic());
                    if (loaded) {
                        skkbot2::Logger::get().info(
                            "skkBot2 custom font '{}' loaded (size {})", customFont, fontSize);
                    } else {
                        skkbot2::Logger::get().warn(
                            "skkBot2 could not load custom font '{}'; falling back to default", customFont);
                    }
                } else {
                    skkbot2::Logger::get().warn(
                        "skkBot2 custom font '{}' not found on disk; falling back to default", customFont);
                }
            }

            if (!loaded) {
                ImFontConfig cfg;
                cfg.OversampleH = 3;
                cfg.OversampleV = 3;
                cfg.GlyphExtraAdvanceX = -0.45f;
                auto defaultFontPath = geode::utils::string::pathToString(resDir / "font_main.ttf");
                loaded = io.Fonts->AddFontFromFileTTF(
                    defaultFontPath.c_str(), static_cast<float>(fontSize), &cfg,
                    io.Fonts->GetGlyphRangesCyrillic());
                if (loaded) {
                    skkbot2::Logger::get().info(
                        "skkBot2 default font loaded (size {})", fontSize);
                } else {
                    skkbot2::Logger::get().warn(
                        "skkBot2 could not load default font; using default");
                }
            }

            if (loaded) {
                io.FontDefault = loaded;
                io.FontGlobalScale = 1.0f;
            }
            // ONE font for the whole bot: every label, readout, number and the
            // wordmark render in the font loaded above.
        })
.draw([] {
              // Times the WHOLE imgui draw hook body. ImGui's draw hook is where
              // the render issues its capture, so this slot contains the capture
              // plus ImGui itself, and Stage::Grab inside it is already counted
              // separately. The difference is what ImGui costs on its own -
              // which nobody could read before, because the hook runs INSIDE
              // swapBuffers and was therefore buried in the swap stage, where it
              // was mistaken for vsync wait. RAII so the early return below for
              // a hidden menu still lands the elapsed time.
              skkbot2::hot::Timed skkbot2Timing(skkbot2::hot::ImguiHook);
              auto& renderer = skkbot2::render::Renderer::get();

            // The hot-path table used to be printed only at the end of a render
            // (renderer.cpp:2141), which made it useless for the question it was
            // written for: what does the bot add to the game OUTSIDE recording?
            // This prints it periodically during ordinary play, so the numbers
            // can be read from the log after a level without starting a render.
            // Throttled to keep it out of the hot path's own noise.
            static double lastDump = 0.0;
            if (!renderer.isRecording()) {
                const double now = skkbot2::hot::now();
                if (now - lastDump > 5.0) {
                    lastDump = now;
                    auto& hs = skkbot2::hot::stats();
                    std::string table = "[HOT] hot path, last 5s of play:";
                    for (size_t i = 0; i < skkbot2::hot::N; i++) {
                        if (hs.calls[i] == 0 && hs.ms[i] == 0.0) continue;
                        const double perSec = hs.calls[i] / 5.0;
                        const char* slotName =
                            skkbot2::hot::name(static_cast<skkbot2::hot::Slot>(i));
                        table += std::string("\n  ") + slotName + " = " +
                                 std::to_string(hs.calls[i]) + " calls (" +
                                 std::to_string(static_cast<long long>(perSec)) + "/s)";
                        if (hs.ms[i] > 0.0) {
                            table += ", " + std::to_string(hs.ms[i] / 5.0) +
                                     "ms per wall second";
                        }
                    }
                    skkbot2::log::info("{}", table);
                    // The one number the whole counter set exists for: what the
                    // MOD costs, with GD's own physics subtracted.
                    //
                    // sched.update(hook) alone is misleading. The hook runs once
                    // per drive step and calls CCScheduler::update up to
                    // kMaxBatchesPerFrame times, so the outer timer mostly
                    // measures the game's simulation - measured 287.9ms per
                    // wall second on 2026-10-18, which reads as "the bot eats
                    // 29% of the frame" and is not what that number means.
                    // SchedUpdate - SchedVanilla is the bot's own share.
                    {
                        auto& s = skkbot2::hot::stats();
                        const double hookMs = s.ms[static_cast<size_t>(skkbot2::hot::SchedUpdate)];
                        const double gdMs = s.ms[static_cast<size_t>(skkbot2::hot::SchedVanilla)];
                        const double perHook = s.calls[static_cast<size_t>(skkbot2::hot::SchedUpdate)];
                        std::string verdict =
                            std::string("\n  >>> mod's own share = ") +
                            std::to_string(std::max(0.0, hookMs - gdMs) / 5.0) +
                            "ms per wall second, " +
                            std::to_string(perHook > 0 ? std::max(0.0, hookMs - gdMs) / perHook : 0.0) +
                            "us per hook call (" +
                            std::to_string(hookMs / 5.0) +
                            "ms hook total, " + std::to_string(gdMs / 5.0) +
                            "ms of it GD's base scheduler)";
                        verdict += "\n  >>> always-on per-player hooks: " +
                                   std::to_string(
                                       (s.ms[static_cast<size_t>(skkbot2::hot::PlayerUpdate)] +
                                        s.ms[static_cast<size_t>(skkbot2::hot::WtsPlayerUpdate)] +
                                        s.ms[static_cast<size_t>(skkbot2::hot::MacroPlayerUpdate)]) /
                                        5.0) +
                                   "ms per wall second";
                        skkbot2::log::info("{}", verdict);
                    }
                    for (size_t i = 0; i < skkbot2::hot::N; i++) {
                        hs.calls[i] = 0;
                        hs.ms[i] = 0.0;
                    }
                }
            }

            // Issue any capture pending from the renderer's last pts boundary
            // BEFORE the GUI overlay is drawn, so the bot menu never leaks
            // into the video (the capture reads a fresh backbuffer).
            renderer.onFrameDraw();

            // While a render is running and the level is actually being captured, the bot
            // draws NOTHING. The capture above already copied the level into its own
            // FBO, so every pixel of ImGui output is discarded - but it still costs
            // a full per-frame pass: gradient text, capsules, animations, blur
            // regions, all laid out and vertex-converted at 4K. Measured at ~10 s
            // over a 45 s level, so nothing is drawn here.
            //
            // The pause is the exception, and it is what makes this usable: a
            // paused level produces no frames, so a menu costs nothing and the
            // menu (with its STOP RENDER button) has to be reachable. Pause, stop
            // the render, resume - back in the level the menu vanishes again.
            if (renderer.isRecording() && !renderer.levelPaused() &&
                skkbot2::gui::get().renderHideMenu) {
                return;
            }

            // Reset blur regions for new ImGui frame
            skkbot2::gui::blur::newFrame();

            // The capture above swaps the cocos view to the OUTPUT size for its
            // visit()-based grab; switch back so the imgui menu that follows is
            // laid out (and vertex-converted) at the real window size — the same
            // height it has outside a render.
            renderer.useWindowViewForUi();

            skkbot2::gui::get().draw();
        });

    // Configurable keybind toggles the menu from anywhere (default F8).
    KeyboardInputEvent().listen([](KeyboardInputData& data) {
        auto& gui = skkbot2::gui::get();
        const bool down = data.action != KeyboardInputData::Action::Release;
        const bool repeat = data.action == KeyboardInputData::Action::Repeat;
        // Raw input can report right-Alt (AltGr) as Ctrl and vice versa; the
        // cocos enum then loses which modifier it really was. The physical
        // scan code is the ground truth, so disambiguate Alt/Ctrl/Shift by it.
        int key = static_cast<int>(data.key);
        switch (data.native.extra & 0xFF) {
            case 0x38: key = 0xA5; break; // Alt (left scan 0x38, right scan E0 0x38)
            case 0x1D: key = 0xA3; break; // Ctrl (left scan 0x1D, right scan E0 0x1D)
            case 0x2A: key = 0xA0; break; // Left Shift
            case 0x36: key = 0xA1; break; // Right Shift
            default: break;
        }
        // While capturing a new keybind, consume the key regardless of ImGui focus.
        if (gui.kbWaiting >= 0) {
            gui.handleKey(key, down, repeat);
            return ListenerResult::Propagate;
        }
        // Don't trigger hotkeys while typing into an ImGui text input field when menu is open
        if (gui.open && down && ImGui::GetIO().WantTextInput) {
            return ListenerResult::Propagate;
        }
        gui.handleKey(key, down, repeat);
        return ListenerResult::Propagate;
    }).leak();

    // Dispatch right and middle mouse clicks to ImGui (gd-imgui-cocos only touches left click).
    MouseInputEvent().listen([](MouseInputData& data) {
        if (ImGuiCocos::get().isInitialized()) {
            auto& io = ImGui::GetIO();
            int btn = -1;
            if (data.button == MouseInputData::Button::Right) btn = 1;
            else if (data.button == MouseInputData::Button::Middle) btn = 2;
            if (btn >= 0) {
                io.AddMouseButtonEvent(btn, data.action == MouseInputData::Action::Press);
            }
        }
        return ListenerResult::Propagate;
    }).leak();

    // Practice Music Sync (always on, no toggle): unlock GD's hidden item 17 so
    // GD's own "Practice Music Sync" option is live and the layer's
    // m_practiceMusicSync gets set on every level load. Applied at module load
    // as well as from PlayLayer::init so GD's Game Options screen reads it as
    // enabled even before any level is opened. See hacks/practice_music_sync.cpp.
    skk::practice_music::applyUnlock();

    logger.info("skkBot2 ready");
}
