#pragma once

#include <imgui.h>
#include <string>
#include <vector>

// skkBot2 native ImGui menu (M1). Replaces the entire cocos frontend of the
// skkBot base. Short menu mirroring the old skkBot layout: Record/Play
// toggle buttons, macro name + Save/Load/Clear/List, macro info, and a
// separate Settings window for TPS / RNG / logging.

namespace skkbot2::gui {

struct Gui {
    bool open = true;

    // Beta GUI (new ImGui shell): routed at the top of draw().
    bool useBetaGui = true;

    // Semibold UI font for titles/logo (loaded in main.cpp setup, may stay
    // null if no bold system font was found — callers must null-check).
    ImFont* boldFont = nullptr;

    // Fixed-width font for eyebrow labels / numerical readouts, loaded in
    // main.cpp setup (Consolas / Cascadia Mono / Courier New). May stay null
    // if none found — callers must null-check (spacedText falls back to the
    // default font).
    ImFont* monoFont = nullptr;

    char macroName[128] = {0};

    bool showSettings = false;
    bool showFeatures = false;
    bool showMacroList = false;
    bool showRender = false;

    ImVec2 featuresPos = ImVec2(352, 20);
    ImVec2 mainPos = ImVec2(20, 20);
    ImVec2 prevMainPos = ImVec2(20, 20);
    ImVec2 settingsPos = ImVec2(0, 0);
    ImVec2 macroListPos = ImVec2(0, 0);
    ImVec2 renderPos = ImVec2(0, 0);
    float mainWindowWidth = 320.0f;
    float mainWindowHeight = 420.0f;
    float featuresWindowWidth = 320.0f;
    float settingsWindowWidth = 360.0f;
    bool featuresDragged = false;
    bool mainDragged = false;
    bool settingsWasOpenLast = false;
    bool featuresWasOpenLast = false;
    bool macroListWasOpenLast = false;
    bool renderWasOpenLast = false;
    bool renderDragged = false;
    // True once the render window has been positioned at least once; on
    // subsequent opens its own saved position is honored instead of snapping
    // it flush beside the main menu again.
    bool renderPosUserSet = false;
    // Tracks the previous frame's recording state so the render window can
    // fire a one-shot "Render finished" notification on the recording->done
    // edge.
    bool renderWasRecording = false;

    // Speedhack toggle: when disabled the effective game speed is locked to
    // 1.0 regardless of the slider value.
    bool speedhackEnabled = false;
    double speedhackValue = 1.0;

    // TPS toggle: when disabled the effective tick rate is locked to 240.0
    // regardless of the slider value.
    bool tpsEnabled = false;
    double tpsValue = 240.0;

    int selectedMacro = -1;
    std::vector<std::string> macros;

    int menuKeybind = 0xA5;      // Left Alt (scan 0x38 -> 0xA5 in main.cpp)
    bool keybindWaiting = false;
    bool settingsDirty = false;

    // Video render settings (port of the old skkBot render tab).
    struct RenderSettingsStore {
        int width = 1920;
        int height = 1080;
        int fps = 60;
        int bitrateMbps = 50;
        bool audioEnabled = false;
        char audioCodec[32] = "aac";
        int audioBitrate = 320;  // kbps
        float musicVolume = 1.0f;  // in-game Music volume forced during render
        float sfxVolume = 1.0f;    // in-game SFX volume forced during render
        double fadeIn = 0.0;
        double fadeOut = 0.0;
        double audioFadeIn = 0.0;
        double audioFadeOut = 0.0;
    };
    RenderSettingsStore render;
    // Encoder selected in the dropdown ("" = auto-pick on first FFmpeg load:
    // basic GPU H.264 nvenc/amf/qsv, else libx264).
    std::string renderEncoder;
    // Whether the Features window should suppress mouse-wheel scrolling on the
    // next frame (true while the cursor hovers a value widget so the wheel
    // steps TPS/speed/RNG instead of scrolling). Re-evaluated each frame.
    bool featuresScrollLocked = false;
    // Same hover-based wheel-scroll lock for the render window's right pane
    // (so the wheel steps bitrate/fade values instead of scrolling it).
    bool renderScrollLocked = false;
    // True once the main window has been drawn open at least once; used to
    // reset the sub-windows (Features/Settings/Macro List) each time the menu
    // opens so only the main tab is shown on first appearance.
    bool wasMenuOpen = false;
    // The key captured on the current physical press. AltGr / right-Alt can
    // emit several Press events per physical press; this suppresses the toggle
    // for that key until it's released, so capturing it doesn't instantly
    // close the menu.
    int toggleLockKey = -1;

    // OS mouse handling: GD hides the system cursor during gameplay. While the
    // menu is open we force it visible every frame; the pre-open state is
    // remembered and restored exactly when the menu closes again.
    bool cursorForced = false;
    bool cursorPrevShouldHide = false;

    static const char* keyName(int key);
    void init();
    void saveSettings();
    void applyCursorForOpenState();
    void draw();
    void refreshMacroList();
    // Copy the engine's already-scanned storedMacros into this shell's list
    // WITHOUT touching the disk (no storedMacros scan). Used to pick up async
    // list changes (a save finishing on the worker thread bumps the engine's
    // macroListVersion, the shell notices and calls this).
    void syncMacros();
    void selectMacroInList(std::string const& name);
    void drawSettingsWindow();
    void drawFeaturesWindow();
    void drawMacroListWindow();
    void drawRenderWindow();
    void startRender();
    void loadRenderSettings();
    void saveRenderSettings();
    bool handleKey(int key, bool down, bool repeat);
};

Gui& get();
void registerKeybind(int key);

}
