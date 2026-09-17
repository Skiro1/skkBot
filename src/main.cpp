#include "skkBot.hpp"
#include "format/skk3_format.hpp"
#include "gui/skkbot_gui.hpp"
#include "logger/logger.hpp"
#ifdef GEODE_IS_WINDOWS
#include "physics/step_grid_hooks.hpp"
#include "core/rng_lock.hpp"
#endif

#include <Geode/Geode.hpp>
#include <Geode/utils/Keyboard.hpp>
#include <Geode/utils/general.hpp>
#include <imgui-cocos.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace geode::prelude;
using namespace skkbot2::gui;

$on_mod(Loaded) {
    auto& logger = skkbot2::Logger::get();
    logger.setLevel(skkbot2::Logger::None);
    logger.setFileLogging(false);

    logger.info("========================================");
    logger.info("skkBot2 v{} initializing (M1: SKK3 Record/Play + TPS)", MOD_VERSION);
    logger.info("========================================");

    namespace fs = std::filesystem;
    auto dir = Mod::get()->getSaveDir() / "replays";
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
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

#ifdef GEODE_IS_WINDOWS
    // Install the step-grid midhooks NOW (module load), never lazily from
    // inside GJBaseGameLayer::update - patching trampoline bytes on the same
    // thread currently executing the target function is a freeze.
    skk::installStepGridHooks();

    // Install RNG midhooks (teleport LCG interception) at module load.
    skk::installRngMidhooks();
#endif

    skkbot2::gui::get().init();

    ImGuiCocos::get()
        .setup([] {
            // Rasterize a real TTF at a DPI-aware size so text is crisp (the
            // default imgui font is tiny and gets upscaled -> blurry), and so
            // Cyrillic renders instead of boxes.
            auto& io = ImGui::GetIO();
            float factor = std::max(1.0f, geode::utils::getDisplayFactor());
            int fontSize = static_cast<int>(std::round(18.0f * factor));

            const char* candidates[] = {
                "C:\\Windows\\Fonts\\segoeui.ttf",
                "C:\\Windows\\Fonts\\arial.ttf",
                "C:\\Windows\\Fonts\\tahoma.ttf",
                "C:\\Windows\\Fonts\\verdanab.ttf",
            };
            ImFont* loaded = nullptr;
            for (const char* path : candidates) {
                ImFont* f = io.Fonts->AddFontFromFileTTF(path, static_cast<float>(fontSize));
                if (f) {
                    loaded = f;
                    break;
                }
            }
            if (loaded) {
                io.FontDefault = loaded;
                io.FontGlobalScale = 1.0f;
                skkbot2::Logger::get().info(
                    "skkBot2 ImGui custom font loaded (size {})", fontSize);
            } else {
                skkbot2::Logger::get().warn(
                    "skkBot2 could not load a system font; using default");
            }
        })
        .draw([] {
            // Issue any capture pending from the renderer's last pts boundary
            // BEFORE the GUI overlay is drawn, so the bot menu never leaks
            // into the video (the capture reads a fresh backbuffer).
            auto& renderer = skkbot2::render::Renderer::get();
            renderer.onFrameDraw();
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
        if (gui.keybindWaiting) {
            gui.handleKey(key, down, repeat);
            return ListenerResult::Propagate;
        }
        // Don't toggle while typing into an ImGui field (releases always pass
        // through so a captured key's lock gets cleared).
        if (down && ImGui::GetIO().WantCaptureKeyboard) {
            return ListenerResult::Propagate;
        }
        gui.handleKey(key, down, repeat);
        return ListenerResult::Propagate;
    }).leak();

    logger.info("skkBot2 ready");
}
