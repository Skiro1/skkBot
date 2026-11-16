#pragma once

#include <imgui.h>
#include <string>
#include <vector>


namespace skkbot2::render { class Renderer; }

namespace skkbot2::gui {

struct Gui {
    bool open = true;

    bool useBetaGui = true;

    ImFont* boldFont = nullptr;

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

    bool renderHideMenu = true;
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
    bool renderPosUserSet = false;
    bool renderWasRecording = false;

    bool speedhackEnabled = false;
    double speedhackValue = 1.0;
    bool musicSpeedSync = false;

    bool tpsEnabled = false;
    double tpsValue = 240.0;

    bool fpsEnabled = false;
    double fpsValue = 0.0;
    bool lockDelta = true;
    bool realTime = false;

    int selectedMacro = -1;
    std::vector<std::string> macros;

    enum class MacroBotType : int {
        SKK = 0,
        SKC = 1,
    };
    MacroBotType macroBotType = MacroBotType::SKK;

    int menuKeybind = 0xA5;
    int stepForwardKeybind = 0x43;
    bool settingsDirty = false;

    enum class Bind : int {
        Menu = 0,
        ResetLevel,
        Record,
        Play,
        StepForward,
        StepToggle,
        Hitboxes,
        Speedhack,
        Noclip,
        Trajectory,
        LayoutMode,
        Count,
    };
    static constexpr int kBindCount = static_cast<int>(Bind::Count);

    struct BindInfo {
        Bind bind;
        const char* label;
        int k;
        int group;
    };

    static constexpr int kGroupCount = 3;
    static constexpr const char* kBindGroupNames[kGroupCount] = {
        "MENU", "RECORD & PLAY", "ASSISTS",
    };

    int bindKeys[kBindCount] = {0};
    int kbWaiting = -1;
    int heldBind = -1;

    void persistBind(Bind b);
    const BindInfo* bindInfo(int i) const;
    bool transportAllowedFor(Bind b) const;
    bool runBind(Bind b);

    struct RenderSettingsStore {
        int width = 1920;
        int height = 1080;
        int fps = 60;
        int bitrateMbps = 50;
        bool audioEnabled = false;
        char audioCodec[32] = "aac";
        int audioBitrate = 320;
        float musicVolume = 1.0f;
        float sfxVolume = 1.0f;
        double fadeIn = 0.0;
        double fadeOut = 0.0;
        double audioFadeIn = 0.0;
        double audioFadeOut = 0.0;
    };
    RenderSettingsStore render;
    std::string renderEncoder;
    bool featuresScrollLocked = false;
    bool renderScrollLocked = false;
    bool wasMenuOpen = false;
    int toggleLockKey = -1;

    bool cursorForced = false;
    bool cursorPrevShouldHide = false;

    static const char* keyName(int key);
    void init();
    void saveSettings();
    void applyCursorForOpenState();
    void draw();
    void refreshMacroList();
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
