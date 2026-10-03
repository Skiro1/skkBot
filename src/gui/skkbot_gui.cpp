#include "gui/skkbot_gui.hpp"
#include "gui/beta_actions.hpp"
#include "gui/beta_gui.hpp"

#include "skkBot.hpp"
#include "logger/logger.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>
#include <imgui-cocos.hpp>

#include <filesystem>
#include <string>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstdint>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

using namespace geode::prelude;
using namespace skkbot2::gui;

namespace skkbot2::gui {

Gui& get() {
    static Gui instance;
    return instance;
}

void Gui::init() {
    auto* mod = Mod::get();
    auto* engine = ReplayEngine::get();

    open = true;
    showSettings = false;
    showFeatures = true;
    showMacroList = false;
    menuKeybind = mod->getSavedValue<int>("menu_keybind", 0xA5);
    useBetaGui = mod->getSavedValue<bool>("gui_use_beta", true);

    if (mod->getSavedValue<bool>("gui_show_features", true)) {
        showFeatures = true;
    } else {
        showFeatures = false;
    }

    // Features window settings.
    engine->tickRate = 240.0;
    tpsEnabled = mod->getSavedValue<bool>("gui_tps_enabled", false);
    tpsValue = std::clamp(mod->getSavedValue<double>("gui_tps_value", 240.0), 1.0, 1000000.0);
    engine->tickRate = tpsEnabled ? tpsValue : 240.0;
    engine->gameSpeed = 1.0;
    speedhackEnabled = mod->getSavedValue<bool>("gui_speedhack_enabled", false);
    speedhackValue =
        std::max(0.01, mod->getSavedValue<double>("gui_speedhack_value", 1.0));
    engine->gameSpeed = speedhackEnabled ? speedhackValue : 1.0;
    // RNG seed is fixed (100) in the engine now - no saved value, no widget.
    engine->collisionBypass = mod->getSavedValue<bool>("gui_noclip", false);
    engine->noclipPlayer1 = mod->getSavedValue<bool>("gui_noclip_p1", true);
    engine->noclipPlayer2 = mod->getSavedValue<bool>("gui_noclip_p2", true);
    engine->noclipDeathFlash = mod->getSavedValue<bool>("gui_noclip_flash", false);

    // HITBOX OVERLAY — visual-only collision-box debug draw (see
    // src/hacks/hitboxes.cpp). Off by default; toggled from the RECORD tab.
    engine->showHitboxes = mod->getSavedValue<bool>("gui_hitboxes", false);
    engine->hitboxFill = mod->getSavedValue<bool>("gui_hitbox_fill", false);

    // TRAJECTORY overlay (RECORD tab) — off by default, it simulates physics
    // every frame while on. See src/hacks/trajectory.cpp. The length is a
    // compile-time constant (ReplayEngine::kTrajectoryLengthDefault); there is
    // no setting to restore.
    engine->showTrajectory = mod->getSavedValue<bool>("gui_trajectory", false);

    // WAVE TRAIL FIX — always-on visual-only trail rework (drag-fix +
    // draw-fix). Defaults to on, like the reference mods.
    engine->waveTrailFix = mod->getSavedValue<bool>("gui_wavetrail", true);
    engine->waveTrailDragFix = mod->getSavedValue<bool>("gui_wavetrail_drag", true);
    engine->waveTrailDrawFix = mod->getSavedValue<bool>("gui_wavetrail_draw", true);
    engine->waveTrailColorOverride = mod->getSavedValue<bool>("gui_wavetrail_color_override", false);
    engine->waveTrailColorR = std::clamp(mod->getSavedValue<float>("gui_wavetrail_color_r", 1.0f), 0.0f, 1.0f);
    engine->waveTrailColorG = std::clamp(mod->getSavedValue<float>("gui_wavetrail_color_g", 1.0f), 0.0f, 1.0f);
    engine->waveTrailColorB = std::clamp(mod->getSavedValue<float>("gui_wavetrail_color_b", 1.0f), 0.0f, 1.0f);
    engine->waveTrailOpacity = std::clamp(mod->getSavedValue<float>("gui_wavetrail_opacity", 1.0f), 0.0f, 1.0f);
    engine->waveTrailOutlineOpacity = std::clamp(mod->getSavedValue<float>("gui_wavetrail_outline_opacity", 0.65f), 0.0f, 1.0f);
    engine->waveTrailEditor = mod->getSavedValue<bool>("gui_wavetrail_editor", true);
    int acc = mod->getSavedValue<int>("gui_accuracy", 0);
    AccuracyMode accMode = sanitizeAccuracyMode(acc);
    engine->selectedAccuracyMode = accMode;
    engine->cosEnabled = mod->getSavedValue<bool>("gui_cos", false);
    // Mirror the game's actual live CBS / COS state if available
    engine->syncAccuracyFromGame();

    // Settings window.
    auto& logger = skkbot2::Logger::get();
    logger.setFileLogging(mod->getSavedValue<bool>("gui_file_logging", false));
    logger.setSplitFiles(mod->getSavedValue<bool>("gui_split_logs", false));
    logger.setLevel(static_cast<skkbot2::Logger::Level>(
        std::clamp(mod->getSavedValue<int>("gui_log_level", 4), 0, 4)));
    // Persistence mode is temporarily locked OFF (feature incomplete);
    // saved value intentionally ignored until it's re-enabled. Existing
    // chain files still play back fine - only new capture stays linear.
    engine->setPersistenceMode(false);

    // Window positions.
    mainPos = ImVec2(mod->getSavedValue<float>("gui_main_x", 20.0f),
                     mod->getSavedValue<float>("gui_main_y", 20.0f));
    featuresPos = ImVec2(mod->getSavedValue<float>("gui_features_x", 352.0f),
                         mod->getSavedValue<float>("gui_features_y", 20.0f));

    if (mod->getSavedValue<std::string>("gui_macro_name", "").length() < sizeof(macroName)) {
        std::string nm = mod->getSavedValue<std::string>("gui_macro_name", "");
        if (!nm.empty()) {
            std::snprintf(macroName, sizeof(macroName), "%s", nm.c_str());
        }
    }

    loadRenderSettings();

    refreshMacroList();
}

void Gui::saveSettings() {
    auto* mod = Mod::get();
    auto* engine = ReplayEngine::get();

    mod->setSavedValue("gui_show_features", showFeatures);
    mod->setSavedValue("gui_use_beta", useBetaGui);
    mod->setSavedValue("gui_tps_enabled", tpsEnabled);
    mod->setSavedValue("gui_tps_value", tpsValue);
    mod->setSavedValue("gui_speedhack_enabled", speedhackEnabled);
    mod->setSavedValue("gui_speedhack_value", speedhackValue);
    mod->setSavedValue("gui_noclip", engine->collisionBypass);
    mod->setSavedValue("gui_noclip_p1", engine->noclipPlayer1);
    mod->setSavedValue("gui_noclip_p2", engine->noclipPlayer2);
    mod->setSavedValue("gui_noclip_flash", engine->noclipDeathFlash);
    mod->setSavedValue("gui_hitboxes", engine->showHitboxes);
    mod->setSavedValue("gui_hitbox_fill", engine->hitboxFill);
    mod->setSavedValue("gui_wavetrail", engine->waveTrailFix);
    mod->setSavedValue("gui_wavetrail_drag", engine->waveTrailDragFix);
    mod->setSavedValue("gui_wavetrail_draw", engine->waveTrailDrawFix);
    mod->setSavedValue("gui_wavetrail_color_override", engine->waveTrailColorOverride);
    mod->setSavedValue("gui_wavetrail_color_r", engine->waveTrailColorR);
    mod->setSavedValue("gui_wavetrail_color_g", engine->waveTrailColorG);
    mod->setSavedValue("gui_wavetrail_color_b", engine->waveTrailColorB);
    mod->setSavedValue("gui_wavetrail_opacity", engine->waveTrailOpacity);
    mod->setSavedValue("gui_wavetrail_outline_opacity", engine->waveTrailOutlineOpacity);
    mod->setSavedValue("gui_wavetrail_editor", engine->waveTrailEditor);
    mod->setSavedValue("gui_accuracy",
                       static_cast<int>(sanitizeAccuracyMode(
                           static_cast<int>(engine->selectedAccuracyMode))));
    mod->setSavedValue("gui_cos", engine->cosEnabled);

    auto& logger = skkbot2::Logger::get();
    mod->setSavedValue("gui_file_logging", logger.isFileLogging());
    mod->setSavedValue("gui_split_logs", logger.isSplitFiles());
    mod->setSavedValue("gui_log_level", static_cast<int>(logger.getLevel()));
    mod->setSavedValue("gui_persistence", engine->persistenceMode);

    mod->setSavedValue("gui_main_x", mainPos.x);
    mod->setSavedValue("gui_main_y", mainPos.y);
    mod->setSavedValue("gui_features_x", featuresPos.x);
    mod->setSavedValue("gui_features_y", featuresPos.y);

    mod->setSavedValue("gui_render_x", renderPos.x);
    mod->setSavedValue("gui_render_y", renderPos.y);
    mod->setSavedValue("gui_show_render", showRender);
    saveRenderSettings();

    mod->setSavedValue("gui_macro_name", std::string(macroName));
}

void Gui::refreshMacroList() {
    macros.clear();
    selectedMacro = -1;

    auto* engine = ReplayEngine::get();
    engine->reloadMacroList();
    macros = engine->storedMacros;
}

void Gui::syncMacros() {
    selectedMacro = -1;
    auto* engine = ReplayEngine::get();
    macros = engine->storedMacros;
}

void Gui::selectMacroInList(std::string const& name) {
    refreshMacroList();
    selectedMacro = -1;
    for (int i = 0; i < static_cast<int>(macros.size()); ++i) {
        if (macros[i] == name) {
            selectedMacro = i;
            break;
        }
    }
}

void Gui::loadRenderSettings() {
    auto* mod = Mod::get();
    render.width = std::clamp(mod->getSavedValue<int>("gui_render_width", 1920), 320, 7680);
    render.height = std::clamp(mod->getSavedValue<int>("gui_render_height", 1080), 240, 4320);
    render.fps = std::clamp(mod->getSavedValue<int>("gui_render_fps", 60), 24, 240);
    render.bitrateMbps = std::clamp(mod->getSavedValue<int>("gui_render_bitrate", 50), 1, 300);
    render.audioEnabled = mod->getSavedValue<bool>("gui_render_audio", false);
    std::string ac = mod->getSavedValue<std::string>("gui_render_audiocodec", "aac");
    std::snprintf(render.audioCodec, sizeof(render.audioCodec), "%s", ac.c_str());
    render.audioBitrate = std::clamp(mod->getSavedValue<int>("gui_render_audiobitrate", 320), 32, 512);
    render.musicVolume = std::clamp(mod->getSavedValue<float>("gui_render_musicvol", 1.0f), 0.0f, 1.0f);
    render.sfxVolume = std::clamp(mod->getSavedValue<float>("gui_render_sfxvol", 1.0f), 0.0f, 1.0f);
    render.fadeIn = std::clamp(mod->getSavedValue<double>("gui_render_fadein", 0.0), 0.0, 10.0);
    render.fadeOut = std::clamp(mod->getSavedValue<double>("gui_render_fadeout", 0.0), 0.0, 10.0);
    render.audioFadeIn = std::clamp(mod->getSavedValue<double>("gui_render_afadein", 0.0), 0.0, 10.0);
    render.audioFadeOut = std::clamp(mod->getSavedValue<double>("gui_render_afadeout", 0.0), 0.0, 10.0);
    renderEncoder = mod->getSavedValue<std::string>("gui_render_encoder", "");
}

void Gui::saveRenderSettings() {
    auto* mod = Mod::get();
    mod->setSavedValue("gui_render_width", render.width);
    mod->setSavedValue("gui_render_height", render.height);
    mod->setSavedValue("gui_render_fps", render.fps);
    mod->setSavedValue("gui_render_bitrate", render.bitrateMbps);
    mod->setSavedValue("gui_render_audio", render.audioEnabled);
    mod->setSavedValue("gui_render_audiocodec", std::string(render.audioCodec));
    mod->setSavedValue("gui_render_audiobitrate", render.audioBitrate);
    mod->setSavedValue("gui_render_musicvol", render.musicVolume);
    mod->setSavedValue("gui_render_sfxvol", render.sfxVolume);
    mod->setSavedValue("gui_render_fadein", render.fadeIn);
    mod->setSavedValue("gui_render_fadeout", render.fadeOut);
    mod->setSavedValue("gui_render_afadein", render.audioFadeIn);
    mod->setSavedValue("gui_render_afadeout", render.audioFadeOut);
    mod->setSavedValue("gui_render_encoder", renderEncoder);
}

void Gui::startRender() {
    auto& renderer = skkbot2::render::Renderer::get();
    if (!renderer.isFFmpegLoaded()) {
        Notification::create("FFmpeg not loaded", NotificationIcon::Error)->show();
        return;
    }
    auto* engine = ReplayEngine::get();
    if (engine->engineMode != MODE_EXECUTE) {
        Notification::create("Load a macro and start playback first",
                             NotificationIcon::Error)->show();
        return;
    }
    auto* pl = PlayLayer::get();
    if (!pl) {
        Notification::create("Enter a level first", NotificationIcon::Error)->show();
        return;
    }

    skkbot2::render::Renderer::RenderSettings rs;
    rs.width = static_cast<unsigned>(std::max(0, render.width));
    rs.height = static_cast<unsigned>(std::max(0, render.height));
    rs.fps = std::max(1, render.fps);
    rs.bitrateMbps = std::max(1, render.bitrateMbps);
    rs.audioEnabled = render.audioEnabled;
    rs.audioCodec = render.audioCodec;
    rs.audioBitrate = std::max(32, render.audioBitrate);
    rs.musicVolume = render.musicVolume;
    rs.sfxVolume = render.sfxVolume;
    rs.fadeInSeconds = render.fadeIn;
    rs.fadeOutSeconds = render.fadeOut;
    rs.audioFadeInSec = render.audioFadeIn;
    rs.audioFadeOutSec = render.audioFadeOut;
    renderer.applySettings(rs);

    if (!renderer.begin(pl)) {
        Notification::create(
            fmt::format("Render start failed: {}", renderer.lastError()),
            NotificationIcon::Error)->show();
        return;
    }
    skkbot2::Logger::get().info("Render started -> {} ({}x{}@{}fps)",
        renderer.lastOutputPath(), render.width, render.height, render.fps);
}

void Gui::drawRenderWindow() {
    auto& renderer = skkbot2::render::Renderer::get();
    auto* engine = ReplayEngine::get();

    // Load FFmpeg the first time the window opens (7 DLLs, ~160 MB total —
    // do not block mod load with it).
    if (!renderer.isFFmpegLoaded()) {
        if (renderer.loadFFmpeg()) {
            // If the user has no saved encoder, auto-pick the most basic GPU
            // H.264 encoder available (nvenc -> amf -> qsv), falling back to
            // software libx264. No bogus "default" pseudo-entry anymore.
            if (renderEncoder.empty()) {
                const auto& encoders = renderer.availableEncoders();
                const char* kPref[] = {
                    "h264_nvenc", "h264_amf", "h264_qsv", "libx264",
                };
                for (const char* want : kPref) {
                    auto it = std::find(encoders.begin(), encoders.end(), want);
                    if (it != encoders.end()) {
                        renderEncoder = *it;
                        break;
                    }
                }
            }
            renderer.setEncoder(renderEncoder);
            skkbot2::Logger::get().info(
                "Render: FFmpeg loaded, {} encoder(s) available",
                renderer.availableEncoders().size());
        }
    }

    const bool ffLoaded = renderer.isFFmpegLoaded();
    const bool recording = renderer.isRecording();
    // A render has run this session and is no longer active — "done" state.
    const bool done = !recording && renderer.renderStartWall() > 0.0 &&
                      renderer.mEncodedFrames.load() > 0;
    const uint64_t encoded = renderer.mEncodedFrames.load();
    const uint64_t captured = renderer.mCapturedFrames.load();

    const double now = wallSecs();
    const double sinceStart =
        renderer.renderStartWall() > 0.0 ? now - renderer.renderStartWall() : 0.0;

    // Size/position are set by draw() on open (flush left of the main menu,
    // same height); the user may still resize the window freely afterwards.
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 300), ImVec2(1400, 1100));
    if (ImGui::Begin("Video Render", &showRender, ImGuiWindowFlags_NoCollapse)) {
        renderPos = ImGui::GetWindowPos();
        renderDragged = ImGui::IsWindowFocused() &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left);
        // One-shot notification on the transition recording -> done.
        if (renderWasRecording && !recording &&
            renderer.renderStartWall() > 0.0) {
            Notification::create("Render finished", NotificationIcon::Success)
                ->show();
        }
        renderWasRecording = recording;

        if (!ffLoaded) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("FFmpeg load failed: %s",
                               renderer.lastError().c_str());
            ImGui::PopStyleColor();
        }
        if (recording) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 1.0f, 0.2f, 1.0f));
            ImGui::TextUnformatted("Rendering...");
            ImGui::PopStyleColor();
            auto* rpl = PlayLayer::get();
            if (rpl && rpl->m_isPaused) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(1.0f, 0.8f, 0.1f, 1.0f));
                ImGui::TextUnformatted("Paused - recording held");
                ImGui::PopStyleColor();
            }
        } else if (done) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 1.0f, 0.2f, 1.0f));
            ImGui::TextUnformatted("Render finished");
            ImGui::PopStyleColor();
        }

        float paneW = ImGui::GetWindowContentRegionMax().x -
                      ImGui::GetWindowContentRegionMin().x;

        // --- Left pane: live preview + progress ---------------------------
        ImGui::BeginChild("##render_left", ImVec2(paneW * 0.55f, 0), true);
        {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 prevSize = ImVec2(avail.x, avail.y * 0.76f);
            ImGui::InvisibleButton("##render_preview", prevSize);
            ImVec2 p0 = ImGui::GetItemRectMin();
            ImVec2 p1 = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, IM_COL32(10, 10, 12, 255));

            unsigned tex = renderer.previewTexture();
            if (tex) {
                float asp =
                    static_cast<float>(renderer.previewWidth()) /
                    static_cast<float>(std::max(1u, renderer.previewHeight()));
                float imgW = prevSize.x;
                float imgH = imgW / asp;
                if (imgH > prevSize.y) {
                    imgH = prevSize.y;
                    imgW = imgH * asp;
                }
                ImVec2 ip0 = ImVec2(p0.x + (prevSize.x - imgW) * 0.5f,
                                    p0.y + (prevSize.y - imgH) * 0.5f);
                dl->AddImage((ImTextureID)(uintptr_t)tex,
                             ip0, ImVec2(ip0.x + imgW, ip0.y + imgH),
                             ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
            } else {
                const char* msg = "No preview yet — start a render";
                ImVec2 ts = ImGui::CalcTextSize(msg);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.5f, 1.0f));
                dl->AddText(ImVec2(p0.x + (prevSize.x - ts.x) * 0.5f,
                                   p0.y + (prevSize.y - ts.y) * 0.5f),
                            IM_COL32(120, 120, 130, 255), msg);
                ImGui::PopStyleColor();
            }

            // Progress against the estimated macro length.
            uint64_t total = 0;
            if (engine->activeSk && !engine->activeSk->inputs.empty()) {
                double macroSecs =
                    static_cast<double>(engine->activeSk->inputs.back().tick) /
                    engine->runtimeTickRate();
                total = static_cast<uint64_t>(macroSecs * renderer.fps()) + 1;
            }
            float progress = 0.0f;
            if (done) {
                progress = 1.0f;
            } else if (recording && total > 0) {
                progress = std::min(1.0f, static_cast<float>(encoded) /
                                              static_cast<float>(total));
            }
            char pbuf[32] = "-";
            if (recording || done) {
                std::snprintf(pbuf, sizeof(pbuf), "%.1f%%", progress * 100.0f);
            }
            ImGui::ProgressBar(progress, ImVec2(-1, 18), pbuf);

            std::snprintf(pbuf, sizeof(pbuf), "Captured %llu  \xc2\xb7  Encoded %llu",
                          static_cast<unsigned long long>(captured),
                          static_cast<unsigned long long>(encoded));
            ImGui::TextUnformatted(pbuf);

            // Elapsed / ETA line.
            if (recording) {
                double eta = progress > 0.01f
                    ? sinceStart / progress * (1.0 - progress)
                    : 0.0;
                char tim[64];
                std::snprintf(tim, sizeof(tim), "Elapsed %02d:%02d  \xc2\xb7  ETA %02d:%02d",
                              static_cast<int>(sinceStart / 60),
                              static_cast<int>(sinceStart) % 60,
                              static_cast<int>(eta / 60),
                              static_cast<int>(eta) % 60);
                ImGui::TextUnformatted(tim);
            }

            // Parameter line.
            const char* encName = renderer.encoderUsed().empty()
                ? (renderEncoder.empty() ? "libx264" : renderEncoder.c_str())
                : renderer.encoderUsed().c_str();
            const char* encHdr = render.audioEnabled && ffLoaded
                ? render.audioCodec
                : "no audio";
            std::snprintf(pbuf, sizeof(pbuf), "%ux%u @ %d fps \xc2\xb7 %s \xc2\xb7 %s",
                          render.width, render.height, render.fps, encName,
                          encHdr);
            ImGui::TextUnformatted(pbuf);

            ImGui::Separator();
            if (!renderer.lastOutputPath().empty()) {
                ImGui::TextWrapped("Output: %s",
                                   renderer.lastOutputPath().c_str());
                if (ImGui::Button("Open folder", ImVec2(-1, 0))) {
                    openFolderFor(renderer.lastOutputPath().c_str());
                }
            } else {
                ImGui::TextDisabled("No output yet");
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // --- Right pane: output settings + Start/Stop ---------------------
        const float wheelDelta = ImGui::GetIO().MouseWheel;
        // Consume the wheel so the native widgets don't double-step. Safe to
        // zero globally here because child-window scroll already happened in
        // NewFrame.
        ImGui::GetIO().MouseWheel = 0.0f;
        bool anyValueHovered = false;
        ImGui::BeginChild("##render_right", ImVec2(0, 0), true,
                          renderScrollLocked ? ImGuiWindowFlags_NoScrollWithMouse
                                             : 0);
        {
            bool changed = false;

            ImGui::BeginDisabled(recording);
            ImGui::Separator();

            const char* resNames[] = {"1280x720",  "1600x900",  "1920x1080",
                                      "2560x1440", "3840x2160", "7680x4320 (8K)"};
            const int resArr[][2] = {{1280, 720},  {1600, 900},  {1920, 1080},
                                     {2560, 1440}, {3840, 2160}, {7680, 4320}};
            int curRes = 0;
            for (int i = 0; i < 6; ++i) {
                if (resArr[i][0] == render.width && resArr[i][1] == render.height) {
                    curRes = i;
                }
            }
            if (ImGui::Combo("Resolution", &curRes, resNames, 6)) {
                render.width = resArr[curRes][0];
                render.height = resArr[curRes][1];
                changed = true;
            }

            if (ImGui::InputInt("FPS", &render.fps, 1, 10)) changed = true;
            if (ImGui::IsItemHovered()) {
                anyValueHovered = true;
                if (wheelDelta != 0.0f) {
                    render.fps += wheelDelta > 0.0f ? 1 : -1;
                    changed = true;
                }
            }
            render.fps = std::clamp(render.fps, 24, 240);

            if (ImGui::SliderInt("Bitrate V", &render.bitrateMbps, 5, 300))
                changed = true;
            if (ImGui::IsItemHovered()) {
                anyValueHovered = true;
                if (wheelDelta != 0.0f) {
                    render.bitrateMbps += wheelDelta > 0.0f ? 1 : -1;
                    changed = true;
                }
            }
            render.bitrateMbps = std::clamp(render.bitrateMbps, 1, 300);

            const auto& encoders = renderer.availableEncoders();
            if (!encoders.empty()) {
                int cur = -1;
                for (size_t i = 0; i < encoders.size(); ++i) {
                    if (encoders[i] == renderEncoder) {
                        cur = static_cast<int>(i);
                        break;
                    }
                }
                if (ImGui::BeginCombo("Encoder",
                                      cur >= 0 ? encoders[cur].c_str()
                                               : renderEncoder.c_str())) {
                    for (size_t i = 0; i < encoders.size(); ++i) {
                        if (ImGui::Selectable(encoders[i].c_str(),
                                              cur == static_cast<int>(i))) {
                            renderEncoder = encoders[i];
                            renderer.setEncoder(renderEncoder);
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::Separator();
            ImGui::Text("Audio");
            if (ImGui::Checkbox("Capture audio", &render.audioEnabled))
                changed = true;
            if (render.audioEnabled) {
                const char* kAudioCodecs[] = {
                    "aac", "libmp3lame", "libopus",
                    "ac3", "libvorbis", "flac",
                };
                {
                    // Combo shows only codecs present in the bundled FFmpeg
                    // (hasAudioCodec queries avcodec_find_encoder_by_name).
                    std::string preview = render.audioCodec;
                    if (preview.empty()) preview = "aac";
                    bool open = ImGui::BeginCombo("Codec", preview.c_str());
                    if (open) {
                        for (const char* name : kAudioCodecs) {
                            if (!renderer.hasAudioCodec(name)) continue;
                            bool sel = preview == name;
                            if (ImGui::Selectable(name, sel)) {
                                std::snprintf(render.audioCodec,
                                              sizeof(render.audioCodec),
                                              "%s", name);
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                if (ImGui::SliderInt("Bitrate A", &render.audioBitrate, 32, 512))
                    changed = true;
                if (ImGui::IsItemHovered()) {
                    anyValueHovered = true;
                    if (wheelDelta != 0.0f) {
                        render.audioBitrate += wheelDelta > 0.0f ? 1 : -1;
                        changed = true;
                    }
                }
                render.audioBitrate = std::clamp(render.audioBitrate, 32, 512);
                if (ImGui::SliderFloat("Music Volume", &render.musicVolume,
                                       0.0f, 1.0f, "%.2f"))
                    changed = true;
                if (ImGui::IsItemHovered()) {
                    anyValueHovered = true;
                    if (wheelDelta != 0.0f) {
                        render.musicVolume = std::clamp(
                            render.musicVolume + (wheelDelta > 0.0f ? 0.01f : -0.01f),
                            0.0f, 1.0f);
                        changed = true;
                    }
                }
                if (ImGui::SliderFloat("SFX Volume", &render.sfxVolume,
                                       0.0f, 1.0f, "%.2f"))
                    changed = true;
                if (ImGui::IsItemHovered()) {
                    anyValueHovered = true;
                    if (wheelDelta != 0.0f) {
                        render.sfxVolume = std::clamp(
                            render.sfxVolume + (wheelDelta > 0.0f ? 0.01f : -0.01f),
                            0.0f, 1.0f);
                        changed = true;
                    }
                }
            }

            ImGui::Separator();
            ImGui::Text("Fades");
            float fadeIn = static_cast<float>(render.fadeIn);
            float fadeOut = static_cast<float>(render.fadeOut);
            if (ImGui::SliderFloat("Fade in", &fadeIn, 0.0f, 5.0f)) {
                render.fadeIn = static_cast<double>(fadeIn);
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                anyValueHovered = true;
                if (wheelDelta != 0.0f) {
                    // Step +-0.001 per wheel event (sign-based, like TPS):
                    // multi-detent wheels report 2-3 per event and would
                    // otherwise skip values.
                    render.fadeIn = std::clamp(
                        render.fadeIn + (wheelDelta > 0.0f ? 0.001 : -0.001),
                        0.0, 5.0);
                    changed = true;
                }
            }
            if (ImGui::SliderFloat("Fade out", &fadeOut, 0.0f, 5.0f)) {
                render.fadeOut = static_cast<double>(fadeOut);
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                anyValueHovered = true;
                if (wheelDelta != 0.0f) {
                    render.fadeOut = std::clamp(
                        render.fadeOut + (wheelDelta > 0.0f ? 0.001 : -0.001),
                        0.0, 5.0);
                    changed = true;
                }
            }
            ImGui::EndDisabled();

            // Persist hover state until the next window draw (controls the
            // NoScrollWithMouse lock applied on the BeginChild above).
            renderScrollLocked = anyValueHovered;

            if (changed) {
                saveRenderSettings();
            }

            ImGui::Separator();

            const bool canStart = !recording && ffLoaded && PlayLayer::get() &&
                                  engine->engineMode == MODE_EXECUTE &&
                                  engine->activeSk;
            ImGui::BeginDisabled(!canStart && !recording);
            if (recording) {
                if (ImGui::Button("Stop Render", ImVec2(-1, 34))) {
                    renderer.stop();
                }
            } else {
                if (ImGui::Button("Start Render", ImVec2(-1, 34))) {
                    startRender();
                }
            }
            ImGui::EndDisabled();

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Load a macro and press Play in the bot menu, then start "
                    "the render. Speed is adaptive: the sim runs as fast as "
                    "the encoder keeps up; the output video is always 1x.");
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// Applies skkBot2's simple, compact dark styling. Runs every frame at the
// start of draw() so the palette always reflects the current styling. We keep
// the default ImGui look with modestly rounded corners so the UI stays light,
// simple and compact.
static void applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(7, 6);
    style.ItemInnerSpacing = ImVec2(5, 5);
    style.IndentSpacing = 14.0f;
    style.FrameBorderSize = 1.0f;
    style.GrabMinSize = 10.0f;
    style.DisabledAlpha = 0.35f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
}

void Gui::applyCursorForOpenState() {
    // GD owns the OS cursor on desktop: it hides it during gameplay and shows
    // it on menu scenes. The bot menu must always be mouse-friendly, so force
    // the cursor visible while the menu is open and hand control back to the
    // state GD had before opening once the menu closes.
    auto* view = CCEGLView::sharedOpenGLView();
    if (!view) return;
    if (open) {
        if (!cursorForced) {
            cursorForced = true;
            cursorPrevShouldHide = view->getShouldHideCursor();
        }
        view->showCursor(true);
    } else if (cursorForced) {
        cursorForced = false;
        view->showCursor(!cursorPrevShouldHide);
    }
}

void Gui::draw() {
    // Keep the system mouse visible while the menu is open, restoring GD's
    // original cursor state (hidden during gameplay) when it closes.
    applyCursorForOpenState();

    // Beta GUI routing: the beta shell draws (or clears) itself and takes over
    // completely; the classic windows below only run when it is disabled.
    if (useBetaGui) {
        beta_gui::render(open);
        return;
    }
    beta_gui::render(false);
    if (!open) return;

    // On the very first time the menu appears in this session, show only the
    // main tab; sub-windows are opened via their buttons afterwards. Closing
    // and reopening the menu keeps whatever sub-windows were open.
    if (!wasMenuOpen) {
        showFeatures = false;
        showSettings = false;
        showMacroList = false;
        wasMenuOpen = true;
    }

    auto* engine = ReplayEngine::get();
    bool inLevel = PlayLayer::get() != nullptr;
    bool capturing = engine->engineMode == MODE_CAPTURE;
    bool executing = engine->engineMode == MODE_EXECUTE;

    applyTheme();

    if (featuresDragged) {
        ImGui::SetNextWindowPos(ImVec2(featuresPos.x - mainWindowWidth - 12.0f, featuresPos.y), ImGuiCond_Always);
    }
    ImGui::SetNextWindowSize(ImVec2(340, 440), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(centerPosFor(ImVec2(340, 440)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 300), ImVec2(540, 720));
    ImGui::Begin("skkBot2", &open, ImGuiWindowFlags_NoCollapse);

    // Record / Play toggle buttons, mirroring the old skkBot pair. The buttons
    // also reflect a queued Record/Play (pressed while outside a level) and an
    // armed playback that is waiting for the next attempt.
    bool recordQueued = engine->pendingAction == ReplayEngine::PendingAction::Record;
    bool playQueued = engine->pendingAction == ReplayEngine::PendingAction::Play;
    bool playArmed = playQueued || engine->pendingPlaybackStart;

    // Status line - colored like the old skkBot.
    const char* statusStr = "Disabled";
    ImVec4 statusColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    if (capturing) {
        statusStr = "RECORDING";
        statusColor = ImVec4(1.0f, 0.2f, 0.1f, 1.0f);
    } else if (executing) {
        statusStr = "PLAYING";
        statusColor = ImVec4(0.1f, 1.0f, 0.2f, 1.0f);
    } else if (playArmed) {
        statusStr = "ARMED";
        statusColor = ImVec4(1.0f, 0.8f, 0.1f, 1.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, statusColor);
    ImGui::TextUnformatted("Status:");
    ImGui::SameLine();
    ImGui::TextUnformatted(statusStr);
    ImGui::PopStyleColor();

    ImGui::Separator();

    float rowW = ImGui::GetContentRegionAvail().x;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float pairW = (rowW - spacing) / 2.0f;

    bool saveBusy = engine->macroSaveBusy();

    ImGui::BeginDisabled(executing || playQueued || saveBusy);
    if (ImGui::Button(capturing || recordQueued ? "Stop" : "Record", ImVec2(pairW, 26))) {
        if (capturing) {
            stopRecording(macroName);
            selectMacroInList(macroName);
        } else if (recordQueued) {
            engine->clearPendingAction();
        } else if (inLevel) {
            startRecording(macroName);
        } else {
            engine->queueRecord(macroName);
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    bool noMacroToPlay = !engine->hasMacroInputs() && macroName[0] == '\0';
    ImGui::BeginDisabled(capturing || recordQueued || saveBusy || (noMacroToPlay && !executing && !playArmed));
    if (ImGui::Button(executing || playArmed ? "Stop" : "Play", ImVec2(pairW, 26))) {
        if (executing) {
            engine->haltExecution();
            skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
        } else if (playArmed) {
            skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
            engine->haltExecution();
            engine->clearPendingAction();
        } else if (inLevel) {
            armPlayback(macroName);
        } else {
            engine->queuePlayback(macroName);
        }
    }
    ImGui::EndDisabled();

    const char* statusHint;
    if (capturing) {
        statusHint = inLevel ? "Recording - press Stop to finish." : "Recording queued - enter the level to start.";
    } else if (executing) {
        statusHint = inLevel ? "Playing - press Stop to halt." : "Playback queued - enter the level to start.";
    } else if (engine->pendingPlaybackStart) {
        statusHint = "Playback armed - takes over on the next attempt.";
    } else if (inLevel) {
        statusHint = "In level - press Record or Play to start.";
    } else if (engine->pendingAction != ReplayEngine::PendingAction::None) {
        statusHint = "Queued - will start when a level is entered.";
    } else {
        statusHint = "Enter a level to record or play.";
    }
    ImGui::TextDisabled("%s", statusHint);

    ImGui::Separator();

    // Counters + progress bar. Playback is main-first: when the macro owns a
    // clean main timeline the persistence death-chain is NOT played (it only
    // re-enacts recorded deaths), so the headline number must show what will
    // actually run, with the chain broken out separately. During capture the
    // pending attempt feeds the chain bucket until it is promoted on Stop.
    size_t mainInputs = 0;
    size_t chainInputs = 0;
    if (engine->activeSk) {
        mainInputs = engine->activeSk->inputs.size();
        for (auto const& attempt : engine->activeSk->persistenceAttempts) {
            chainInputs += attempt.inputs.size();
        }
        chainInputs += engine->persistenceCaptureAttempt.inputs.size();
    }
    const size_t playInputs = (mainInputs > 0) ? mainInputs : chainInputs;
    size_t segmentInputs = playInputs;
    size_t playedInputs = engine->executeIndex;
    if (executing) {
        if (auto* list = engine->activeSkInputs()) {
            segmentInputs = list->size();
        }
    }
    if (executing) {
        ImGui::Text("Inputs: %zu / %zu", playedInputs, segmentInputs);
    } else if (mainInputs > 0 && chainInputs > 0) {
        ImGui::Text("Inputs: %zu (+%zu chain)", mainInputs, chainInputs);
    } else if (chainInputs > 0) {
        ImGui::Text("Inputs: %zu [chain]", chainInputs);
    } else {
        ImGui::Text("Inputs: %zu", mainInputs);
    }
    if (capturing) {
        int p = static_cast<int>((mainInputs + chainInputs) % 240);
        ImGui::ProgressBar(static_cast<float>(p) / 240.0f, ImVec2(-1, 4), "");
    } else if (executing && segmentInputs > 0) {
        float progress = static_cast<float>(playedInputs) /
                         static_cast<float>(segmentInputs);
        ImGui::ProgressBar(progress, ImVec2(-1, 4), "");
    }

    ImGui::Separator();

    // Macro name + Save/Load/Clear/List. The name box is user-controlled
    // only - no automatic seeding (recordings keep the auto level name
    // inside the macro; typing a name here overrides it on Save).
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##macroname", "Macro name", macroName, sizeof(macroName));

    rowW = ImGui::GetContentRegionAvail().x;
    spacing = ImGui::GetStyle().ItemSpacing.x;
    float btnW = (rowW - spacing * 3.0f) / 4.0f;
    bool recOrPlay = capturing || recordQueued || executing || playArmed;
    ImGui::BeginDisabled(recOrPlay || saveBusy);
    if (ImGui::Button("Save", ImVec2(btnW, 0))) {
        if (macroName[0] == '\0') {
            Notification::create("Macro name is empty", NotificationIcon::Error)->show();
        } else if (!engine->activeSk && !engine->activeMacro) {
            Notification::create("Nothing to save - no macro loaded",
                                 NotificationIcon::Error)->show();
        } else {
            std::string nm(macroName);
            if (engine->activeSk) {
                engine->activeSk->name = nm;
            }
            if (engine->activeMacro) {
                engine->activeMacro->name = nm;
            }
            if (engine->saveActiveMacro()) {
                Notification::create("Saving macro...", NotificationIcon::Info)->show();
            } else {
                Notification::create("Failed to save macro",
                                     NotificationIcon::Error)->show();
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Loading replaces activeSk - lethal mid-recording (Toasty refuses all
    // macro-load operations while capturing for exactly this reason).
    ImGui::BeginDisabled(capturing || recordQueued || executing || playArmed || saveBusy);
    if (ImGui::Button("Load", ImVec2(btnW, 0))) {
        if (macroName[0] == '\0') {
            Notification::create("Macro name is empty",
                                 NotificationIcon::Error)->show();
        } else if (loadMacroIntoMemory(macroName)) {
            selectMacroInList(macroName);
            Notification::create(
                fmt::format("Macro loaded: {}", macroName),
                NotificationIcon::Success)->show();
        } else {
            Notification::create("Failed to load macro",
                                 NotificationIcon::Error)->show();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(recOrPlay || saveBusy);
    if (ImGui::Button("Clear", ImVec2(btnW, 0))) {
        skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
        engine->haltExecution();
        engine->discardActiveMacro();
        macroName[0] = '\0';
        Notification::create("Macro cleared", NotificationIcon::Info)->show();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("List", ImVec2(btnW, 0))) {
        refreshMacroList();
        showMacroList = !showMacroList;
    }

    ImGui::Separator();

    // Macro info line: always show the live level name + ID (ID can't be
    // negative; clamp to 0). Independent of record/play state so it doesn't
    // flicker or lose the ID mid-session. Warn when the active macro was
    // recorded elsewhere - GDH has no check at all and silently desyncs.
    if (auto* pl = PlayLayer::get(); pl && pl->m_level) {
        int lvlId = static_cast<int>(pl->m_level->m_levelID);
        ImGui::Text("Level: %s (ID: %d)",
            pl->m_level->m_levelName.c_str(), std::max(0, lvlId));
        if (engine->activeSk && engine->activeSk->levelId > 0 &&
            engine->activeSk->levelId != static_cast<int32_t>(pl->m_level->m_levelID)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.1f, 1.0f));
            ImGui::Text("Macro recorded in another level: %s (ID: %d)",
                engine->activeSk->levelName.c_str(), engine->activeSk->levelId);
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::TextDisabled("Not in a level");
    }

    ImGui::Separator();

    if (ImGui::Button("Features", ImVec2(-1, 0))) {
        showFeatures = !showFeatures;
        settingsDirty = true;
    }
    if (ImGui::Button("Settings", ImVec2(-1, 0))) {
        showSettings = !showSettings;
    }
    if (ImGui::Button("Render", ImVec2(-1, 0))) {
        showRender = !showRender;
        settingsDirty = true;
    }

    mainWindowWidth = ImGui::GetWindowWidth();
    mainWindowHeight = ImGui::GetWindowHeight();
    mainPos = ImGui::GetWindowPos();
    mainDragged = ImGui::IsWindowFocused() && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    ImGui::End();

    // When the main menu is dragged, carry every open sub-window along so the
    // whole set moves as a unit, while each window stays independently
    // draggable on its own. Delta-based so previously-moved windows keep their
    // relative offset. The Render window is intentionally NOT carried: it
    // remembers and keeps its own position (recorded in drawRenderWindow), so
    // moving the main menu doesn't magnet it again.
    ImVec2 mainDelta = ImVec2(mainPos.x - prevMainPos.x, mainPos.y - prevMainPos.y);
    prevMainPos = mainPos;
    if (mainDragged) {
        if (showFeatures) {
            featuresPos.x += mainDelta.x;
            featuresPos.y += mainDelta.y;
            ImGui::SetWindowPos("Features", featuresPos);
        }
        if (showSettings) {
            settingsPos.x += mainDelta.x;
            settingsPos.y += mainDelta.y;
            ImGui::SetWindowPos("Bot Settings", settingsPos);
        }
        if (showMacroList) {
            macroListPos.x += mainDelta.x;
            macroListPos.y += mainDelta.y;
            ImGui::SetWindowPos("Macro List", macroListPos);
        }
        settingsDirty = true;
    }

    if (showSettings) {
        // Flag is read inside the window (first-open positioning) and only set
        // to true AFTER drawing so the opening frame can still act on it.
        drawSettingsWindow();
        settingsWasOpenLast = true;
    } else {
        settingsWasOpenLast = false;
    }
    if (showMacroList) {
        if (!macroListWasOpenLast) {
            ImGui::SetNextWindowPos(
                ImVec2(mainPos.x, mainPos.y + mainWindowHeight + 12.0f), ImGuiCond_Always);
        }
        macroListWasOpenLast = true;
        drawMacroListWindow();
    } else {
        macroListWasOpenLast = false;
    }
    if (showRender) {
        if (!renderWasOpenLast && !renderPosUserSet) {
            // Position the render window flush beside the main menu: left
            // when there is room, otherwise right.  Width adapts to what
            // fits on the chosen side; height matches the main menu. This
            // only happens on the FIRST open; after that the window keeps
            // whatever position the user dragged it to.
            ImGuiViewport* vp = ImGui::GetMainViewport();
            float mainRight = mainPos.x + mainWindowWidth;
            float screenRight = vp->WorkPos.x + vp->WorkSize.x;
            float mainBottom = mainPos.y + mainWindowHeight;
            float screenBottom = vp->WorkPos.y + vp->WorkSize.y;

            float leftAvail = std::max(0.0f, mainPos.x - vp->WorkPos.x - 12.0f);
            float rightAvail = std::max(0.0f, screenRight - mainRight - 12.0f);

            bool openLeft = leftAvail >= 420.0f && leftAvail >= rightAvail;
            float availSide = openLeft ? leftAvail : rightAvail;
            float rw = std::clamp(availSide, 420.0f, 640.0f);
            float availBelow = std::max(0.0f, screenBottom - mainPos.y - 12.0f);
            float rh = std::clamp(mainWindowHeight + 10.0f, 320.0f,
                                  std::max(320.0f, availBelow));

            if (openLeft) {
                renderPos = ImVec2(mainPos.x - rw - 12.0f, mainPos.y);
            } else {
                renderPos = ImVec2(mainRight + 12.0f, mainPos.y);
            }
            ImGui::SetNextWindowPos(renderPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(rw, rh), ImGuiCond_Always);
            renderPosUserSet = true;
        } else if (!renderWasOpenLast) {
            // Not the first open: restore the window's own saved position
            // instead of magnetizing it back next to the main menu.
            ImGui::SetNextWindowPos(renderPos, ImGuiCond_Always);
        }
        renderWasOpenLast = true;
        drawRenderWindow();
    } else {
        renderWasOpenLast = false;
        renderScrollLocked = false;
    }

    if (mainDragged || (featuresWasOpenLast && featuresDragged) ||
        (renderWasOpenLast && renderDragged) || settingsDirty) {
        saveSettings();
        settingsDirty = false;
    }

    // Features is drawn dead-last so that the wheel-consume inside
    // drawFeaturesWindow (used to step TPS/speed/RNG without the window
    // scrolling) can't eat the scroll of any other window this frame.
    if (showFeatures) {
        if (!featuresWasOpenLast) {
            ImGui::SetNextWindowPos(ImVec2(mainPos.x + mainWindowWidth + 12.0f, mainPos.y), ImGuiCond_Always);
        }
        featuresWasOpenLast = true;
        drawFeaturesWindow();
    } else {
        featuresWasOpenLast = false;
        featuresScrollLocked = false;
    }
}

void Gui::drawFeaturesWindow() {
    auto* engine = ReplayEngine::get();
    ImGui::SetNextWindowSize(ImVec2(400, 440), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(370, 300), ImVec2(570, 820));
    // Scroll lock is applied from the previous frame's hover state (the wheel
    // scroll of a window happens during NewFrame, before this draw runs):
    // hovering a value widget locks the window scroll so the wheel steps the
    // value instead; moving the mouse off re-enables normal scrolling.
    ImGuiWindowFlags featuresFlags = ImGuiWindowFlags_NoCollapse;
    if (featuresScrollLocked) {
        featuresFlags |= ImGuiWindowFlags_NoScrollWithMouse;
    }
    if (!ImGui::Begin("Features", &showFeatures, featuresFlags)) {
        ImGui::End();
        return;
    }
    featuresPos = ImGui::GetWindowPos();
    featuresWindowWidth = ImGui::GetWindowWidth();
    featuresDragged = ImGui::IsWindowFocused() && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    if (featuresDragged) {
        settingsDirty = true;
    }

    const float wheelDelta = ImGui::GetIO().MouseWheel;
    // Consume the wheel so the native widgets don't double-step. Safe to zero
    // globally here because window scroll already happened in NewFrame.
    ImGui::GetIO().MouseWheel = 0.0f;

    bool anyValueHovered = false;

    if (ImGui::CollapsingHeader("Core", ImGuiTreeNodeFlags_DefaultOpen)) {
        // While a replay owns the clock (beginReplayAccuracyEnvironment applies
        // the macro's declared TPS on PLAY), the row shows that LIVE rate with
        // the toggle ON, and turning the toggle off hands the rate back to the
        // menu's own value (dropPlaybackTickRateOverride) without disturbing
        // the replay's accuracy. Outside that, only WRITE engine->tickRate on
        // an actual edit — writing it every frame (as the old
        // `engine->tickRate = tpsOn ? tpsValue : 240.0` did) stomps the
        // macro-declared TPS: a macro recorded at 360 TPS would silently fall
        // back to 240 the moment the GUI redraws, and so would every render
        // started while the TPS toggle was off.
        const bool playbackOverriding =
            engine->engineMode == MODE_EXECUTE && engine->playbackTickRateOverrideActive;
        if (playbackOverriding) {
            ImGui::BeginDisabled(true);
            float liveTps = static_cast<float>(engine->runtimeTickRate());
            ImGui::SliderFloat("TPS", &liveTps, 1.0f, 1000000.0f, "%.0f");
            ImGui::EndDisabled();
            bool on = true;
            if (ImGui::Checkbox("##tps_enable", &on)) {
                if (!on) engine->dropPlaybackTickRateOverride();
            }
        } else {
            engine->tickRate = tpsEnabled ? tpsValue : 240.0;
            ImGui::BeginDisabled(!tpsEnabled);
            float tps = static_cast<float>(tpsEnabled ? tpsValue : 240.0);
            if (ImGui::SliderFloat("TPS", &tps, 1.0f, 1000000.0f, "%.0f")) {
                tpsValue = static_cast<double>(tps);
                engine->tickRate = tpsValue;
                settingsDirty = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered() && tpsEnabled) {
                anyValueHovered = true;
                if (wheelDelta != 0.0f) {
                    // Step exactly +-1 per wheel event (not by |wheelDelta|):
                    // multi-detent wheels report 2-3 per event and would skip
                    // values like 360 (359 -> 361). Sign-based single-step keeps
                    // every integer reachable.
                    tpsValue = std::clamp(
                        tpsValue + (wheelDelta > 0.0f ? 1.0 : -1.0), 1.0, 1000000.0);
                    engine->tickRate = tpsValue;
                    settingsDirty = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("##tps_enable", &tpsEnabled)) {
                settingsDirty = true;
            }
        }
        ImGui::TextDisabled("Base: %.0f Hz", ReplayEngine::kBaseTickRate);

        ImGui::Separator();
        // Effective game speed: 1.0 while disabled, the slider value when on.
        if (speedhackEnabled) {
            engine->gameSpeed = speedhackValue;
        } else {
            engine->gameSpeed = 1.0;
        }
        ImGui::BeginDisabled(!speedhackEnabled);
        float speed = static_cast<float>(speedhackEnabled ? speedhackValue : 1.0);
        if (ImGui::SliderFloat("Speedhack", &speed, 0.01f, 100.0f, "x%.2f")) {
            speedhackValue = static_cast<double>(speed);
            engine->gameSpeed = speedhackValue;
            settingsDirty = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && speedhackEnabled) {
            anyValueHovered = true;
            if (wheelDelta != 0.0f) {
                // Round to 2 decimals so stepping from 0.99 lands exactly on
                // 1.00 (float 0.99 + 0.01 would otherwise read as 1.01).
                speedhackValue = std::max(0.01,
                    std::round((speedhackValue + 0.01 * static_cast<double>(wheelDelta)) * 100.0) / 100.0);
                engine->gameSpeed = speedhackValue;
                settingsDirty = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("##speedhack_enable", &speedhackEnabled)) {
            settingsDirty = true;
        }

        ImGui::Separator();
        engine->syncAccuracyFromGame();
        struct AccuracyLabel {
            AccuracyMode mode;
            const char* name;
        };
        static const AccuracyLabel kAccuracyOptions[] = {
            { AccuracyMode::Vanilla, "Vanilla" },
            { AccuracyMode::CBS, "CBS" },
        };
        AccuracyMode selected = engine->selectedAccuracyMode;
        const char* preview = "Vanilla";
        for (auto const& opt : kAccuracyOptions) {
            if (opt.mode == selected) {
                preview = opt.name;
                break;
            }
        }
        ImGui::Text("Accuracy:");
        ImGui::SameLine();
        if (ImGui::BeginCombo("##accuracymode", preview)) {
            for (auto const& opt : kAccuracyOptions) {
                bool isSelected = (opt.mode == selected);
                if (ImGui::Selectable(opt.name, isSelected)) {
                    engine->selectedAccuracyMode = opt.mode;
                    settingsDirty = true;
                    ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode, engine->cosEnabled);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Checkbox("COS (Click On Steps)", &engine->cosEnabled)) {
            settingsDirty = true;
            ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode, engine->cosEnabled);
        }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Noclip", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool anyNoclip = engine->collisionBypass;
        // Label must differ from the CollapsingHeader("Noclip") above to avoid
        // an ImGui ID conflict; ## keeps the visible text but makes the ID unique.
        if (ImGui::Checkbox("Noclip##master", &anyNoclip)) {
            engine->collisionBypass = anyNoclip;
            settingsDirty = true;
        }
        ImGui::BeginDisabled(!engine->collisionBypass);
        bool p1 = engine->noclipPlayer1;
        bool p2 = engine->noclipPlayer2;
        bool flash = engine->noclipDeathFlash;
        if (ImGui::Checkbox("Noclip Player 1", &p1)) {
            engine->noclipPlayer1 = p1;
            settingsDirty = true;
        }
        if (ImGui::Checkbox("Noclip Player 2", &p2)) {
            engine->noclipPlayer2 = p2;
            settingsDirty = true;
        }
        if (ImGui::Checkbox("Death flash", &flash)) {
            engine->noclipDeathFlash = flash;
            settingsDirty = true;
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextDisabled("Noclip accuracy");
        if (ImGui::Button("Reset stats", ImVec2(120, 0))) {
            engine->resetNoclipAccuracyStats();
        }
        ImGui::Text("Frames: %d", engine->noclipTotalFrames);
        ImGui::Text("Unsafe: %d", engine->noclipUnsafeFrames);
        ImGui::Text("Acc: %.2f%%", engine->noclipAccuracyPercent());
    }

    // Persist hover state until the next window draw (controls scroll locking).
    featuresScrollLocked = anyValueHovered;

    ImGui::End();
}

void Gui::drawSettingsWindow() {
    auto* engine = ReplayEngine::get();
    ImGui::SetNextWindowSize(ImVec2(340, 440), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(330, 240), ImVec2(570, 740));
    if (!ImGui::Begin("Bot Settings", &showSettings,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    settingsPos = ImGui::GetWindowPos();
    settingsWindowWidth = ImGui::GetWindowWidth();

    // Align the window's right edge 12px left of the main menu on its opening
    // frame. Done after Begin so the real window width is used (the stored
    // width is stale on the very first open, which used to offset it further).
    if (!settingsWasOpenLast) {
        ImGui::SetWindowPos(ImVec2(mainPos.x - settingsWindowWidth - 12.0f, mainPos.y));
    }

    if (ImGui::CollapsingHeader("Interface", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Beta GUI (new style)", &useBetaGui)) {
            Mod::get()->setSavedValue("gui_use_beta", useBetaGui);
        }
        ImGui::TextDisabled(useBetaGui ? "Beta GUI is active."
                                       : "Classic menu interface.");
    }

    if (ImGui::CollapsingHeader("Menu Keybind", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted("Key to open/close the menu:");
        if (keybindWaiting) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Press a key... (Esc = cancel)");
        } else {
            ImGui::TextUnformatted(keyName(menuKeybind));
            ImGui::SameLine();
            if (ImGui::Button("Change", ImVec2(0, 0))) {
                keybindWaiting = true;
            }
        }
    }

    if (ImGui::CollapsingHeader("Persistence", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Toggle hidden while the mode is incomplete (see forced-off in load).
        ImGui::TextDisabled("Persistence mode: temporarily unavailable.");
        ImGui::TextDisabled("Re-enabled once chain capture is verified.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Logging");
    auto& logger = skkbot2::Logger::get();
    bool fileLogging = logger.isFileLogging();
    if (ImGui::Checkbox("File Logging", &fileLogging)) {
        logger.setFileLogging(fileLogging);
        settingsDirty = true;
    }
    if (fileLogging) {
        bool splitFiles = logger.isSplitFiles();
        if (ImGui::Checkbox("Split Record/Play logs", &splitFiles)) {
            logger.setSplitFiles(splitFiles);
            settingsDirty = true;
        }
    }
    static int levelItem = static_cast<int>(logger.getLevel());
    const char* levels[] = { "Debug", "Info", "Warn", "Error", "None" };
    ImGui::Text("Level:");
    ImGui::SameLine();
    if (ImGui::Combo("##loglevel", &levelItem, levels, 5)) {
        logger.setLevel(static_cast<skkbot2::Logger::Level>(levelItem));
        settingsDirty = true;
    }

    ImGui::End();
}

void Gui::drawMacroListWindow() {
    auto* engine = ReplayEngine::get();
    bool capturing = engine->engineMode == MODE_CAPTURE;
    ImGui::SetNextWindowSize(ImVec2(340, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Macro List", &showMacroList,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    macroListPos = ImGui::GetWindowPos();

    if (ImGui::Button("Reload", ImVec2(-1, 0))) {
        refreshMacroList();
    }

    ImGui::Separator();

    if (macros.empty()) {
        ImGui::TextDisabled("No replays recorded yet.");
    } else {
        for (int i = 0; i < static_cast<int>(macros.size()); ++i) {
            ImGui::PushID(i);
            bool selected = (i == selectedMacro);
            const char* label = macros[i].empty() ? "<unnamed>" : macros[i].c_str();
            if (ImGui::Selectable(label, &selected)) {
                selectedMacro = i;
            }
            ImGui::PopID();
        }

        ImGui::Separator();

        // Loading/deleting during capture: loading would replace the
        // in-progress activeSk; blocked like Toasty does.
        ImGui::BeginDisabled(selectedMacro < 0 || capturing);
        if (ImGui::Button("Load Selected", ImVec2(145, 0))) {
            if (selectedMacro >= 0 &&
                selectedMacro < static_cast<int>(macros.size())) {
                std::string nm = macros[selectedMacro];
                if (loadMacroIntoMemory(nm)) {
                    selectMacroInList(nm);
                    Notification::create(
                        fmt::format("Macro loaded: {}", nm),
                        NotificationIcon::Success)->show();
                } else {
                    Notification::create(
                        fmt::format("Failed to load macro: {}", nm),
                        NotificationIcon::Error)->show();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete", ImVec2(145, 0))) {
            if (selectedMacro >= 0 &&
                selectedMacro < static_cast<int>(macros.size())) {
                namespace fs = std::filesystem;
                fs::path path = Mod::get()->getSaveDir() / "replays" /
                                (macros[selectedMacro] + ".skk");
                if (fs::remove(path)) {
                    Notification::create(
                        fmt::format("Deleted: {}", macros[selectedMacro]),
                        NotificationIcon::Info)->show();
                    refreshMacroList();
                } else {
                    Notification::create("Failed to delete macro",
                                         NotificationIcon::Error)->show();
                }
            }
        }
        ImGui::EndDisabled();
    }

    ImGui::End();
}

bool Gui::handleKey(int key, bool down, bool repeat) {
    // While capturing a new keybind: the next key (or Esc to cancel) sets it.
    if (keybindWaiting) {
        if (down && !repeat) {
            if (key == 0x1B) { // Escape = cancel
                keybindWaiting = false;
            } else {
                menuKeybind = key;
                keybindWaiting = false;
                // Right-Alt / AltGr can emit several Press events for a single
                // physical press; lock the toggle until this key is released so
                // capturing it doesn't immediately close the menu.
                toggleLockKey = key;
                Mod::get()->setSavedValue("menu_keybind", menuKeybind);
                skkbot2::Logger::get().info("Keybind captured: key=0x{:X}", key);
            }
        }
        return true; // consume the key so it doesn't reach the game
    }

    // Releasing the just-captured key clears the toggle lock.
    if (!down && key == toggleLockKey) {
        toggleLockKey = -1;
        return false;
    }

    // Toggle the bot menu on the configured keybind press.
    if (down && key == menuKeybind) {
        if (key == toggleLockKey) {
            return true; // still the same physical press that just captured
        }
        open = !open;
        return true;
    }

    return false;
}

const char* Gui::keyName(int key) {
    switch (key) {
        case 0x08: return "Backspace";
        case 0x09: return "Tab";
        case 0x0C: return "Clear";
        case 0x0D: return "Enter";
        case 0x10: return "Shift";
        case 0x11: return "Ctrl";
        case 0x12: return "Alt";
        case 0x13: return "Pause";
        case 0x14: return "Caps Lock";
        case 0x1B: return "Escape";
        case 0x20: return "Space";
        case 0x21: return "PageUp";
        case 0x22: return "PageDown";
        case 0x23: return "End";
        case 0x24: return "Home";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0x29: return "Select";
        case 0x2A: return "Print";
        case 0x2B: return "Execute";
        case 0x2C: return "PrintScreen";
        case 0x2D: return "Insert";
        case 0x2E: return "Delete";
        case 0x2F: return "Help";
        case 0x30: return "0";
        case 0x31: return "1";
        case 0x32: return "2";
        case 0x33: return "3";
        case 0x34: return "4";
        case 0x35: return "5";
        case 0x36: return "6";
        case 0x37: return "7";
        case 0x38: return "8";
        case 0x39: return "9";
        case 0x41: return "A";
        case 0x42: return "B";
        case 0x43: return "C";
        case 0x44: return "D";
        case 0x45: return "E";
        case 0x46: return "F";
        case 0x47: return "G";
        case 0x48: return "H";
        case 0x49: return "I";
        case 0x4A: return "J";
        case 0x4B: return "K";
        case 0x4C: return "L";
        case 0x4D: return "M";
        case 0x4E: return "N";
        case 0x4F: return "O";
        case 0x50: return "P";
        case 0x51: return "Q";
        case 0x52: return "R";
        case 0x53: return "S";
        case 0x54: return "T";
        case 0x55: return "U";
        case 0x56: return "V";
        case 0x57: return "W";
        case 0x58: return "X";
        case 0x59: return "Y";
        case 0x5A: return "Z";
        case 0x5B: return "Win";
        case 0x5C: return "Win";      // right windows key
        case 0x5D: return "Menu";
        case 0x60: return "Num 0";
        case 0x61: return "Num 1";
        case 0x62: return "Num 2";
        case 0x63: return "Num 3";
        case 0x64: return "Num 4";
        case 0x65: return "Num 5";
        case 0x66: return "Num 6";
        case 0x67: return "Num 7";
        case 0x68: return "Num 8";
        case 0x69: return "Num 9";
        case 0x6A: return "Num *";
        case 0x6B: return "Num +";
        case 0x6C: return "Num ,";
        case 0x6D: return "Num -";
        case 0x6E: return "Num .";
        case 0x6F: return "Num /";
        case 0x90: return "Num Lock";
        case 0x91: return "Scroll Lock";
        case 0xA0: return "Shift";    // Left Shift
        case 0xA1: return "Shift";    // Right Shift
        case 0xA2: return "Ctrl";     // Left Control
        case 0xA3: return "Ctrl";     // Right Control
        case 0xA4: return "Alt";      // Left Alt
        case 0xA5: return "Alt";      // Right Alt
        case 0xB8: return "+";
        case 0xBA: return ";";
        case 0xBC: return ",";
        case 0xBD: return "-";
        case 0xBE: return ".";
        case 0xBF: return "/";
        case 0xC0: return "`";
        case 0xDB: return "[";
        case 0xDC: return "\\";
        case 0xDD: return "]";
        case 0xDE: return "'";
        case 0x0B0: return "Next Track";
        case 0x0B3: return "Play/Pause";
        default: break;
    }
    if (key >= 0x70 && key <= 0x87) { // F1..F24
        static thread_local char fbuf[8];
        std::snprintf(fbuf, sizeof(fbuf), "F%d", key - 0x70 + 1);
        return fbuf;
    }
    static thread_local char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%X", key);
    return hex;
}

void registerKeybind(int key) {
    static_cast<void>(key);
}

}
