#include "gui/beta_gui.hpp"
#include "gui/beta_actions.hpp"
#include "gui/beta_widgets.hpp"
#include "gui/blur.hpp"
#include "gui/skkbot_gui.hpp"

#include "skkBot.hpp"
#include "skc/skc_bot.hpp"
#include "core/autosave_recorder.hpp"
#include "core/frame_stepper.hpp"
#include "core/paths.hpp"
#include "hacks/music_speed.hpp"
#include "logger/logger.hpp"
#include "render/renderer.hpp"
#include "render/ffdownloader.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>
#include <imgui-cocos.hpp>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace geode::prelude;

// Beta GUI — "abstract-modern-ui-redesign" shell. Layout mirrors the React
// mock (docs/abstract-modern-ui-redesign): 88px header (brand wordmark,
// centered mono top-tabs with 2px indicator, window actions), then
// a body region, then a 66px footer (engine status + shortcuts + INPUTS
// readout). Pure Dear ImGui (GD fork, 1.93.0).

namespace skkbot2::gui::beta_gui {

// Page-wide vertical rhythm. Every accordion adds this after its content so the
// whitespace from a section's LAST row to the NEXT accordion's heading is the
// same everywhere — previously the stacked Settings/Render sections butted
// directly against the next heading (0k) while the right-column accordions left
// 11k, so the same visual break had two different sizes. Matches the 10k gap
// between rows (TPS/Speed, the codec sections, NOCLIP), so the page reads as one
// continuous rhythm with no divisions.
constexpr float kSectionGap = 10.0f;
namespace {

using namespace skkbot2::gui::beta_widgets;

enum class Tab { Record, Assist, Render, Settings };
constexpr const char* kTabNames[] = {"RECORD", "ASSIST", "RENDER", "SETTINGS"};
constexpr int kTabCount = 4;

constexpr float kHeaderH = 56.0f;
constexpr float kFooterH = 40.0f;
constexpr float kBodyPadX = 32.0f;
constexpr float kBodyPadTop = 12.0f;
constexpr float kBodyPadBottom = 10.0f;

// Master switch for the shell's in-frame diagnostic probes (F9 = blur-rect
// outline, F10 = the 7-mode corner diag cycle). The probes were the
// instruments that closed BUG-132 (14.11: the corner notch was the STROKE
// shadow, not the blur paste — see drawShellShadow). They stay in the code
// for the next shell hunt, but ship OFF: a user pressing F9/F10 in a level
// must not be able to strip the shadow or tint the glass.
#ifdef NDEBUG
constexpr bool kDebugShell = false;
#else
constexpr bool kDebugShell = true;
#endif

struct State {
    Tab tab = Tab::Record;
    Tab lastTab = Tab::Record;
    ImVec2 pos = ImVec2(0, 0);
    bool initialized = false;
    bool expanded = false;
    bool recordOpen = true;
    bool renderOpen = true;          // VIDEO section (the RENDER tab heading)
    bool audioOpen = true;           // AUDIO section under it
    bool macroBotOpen = true;        // MACRO BOT accordion in the Settings tab
    bool settingsOpen = true;
    bool speedOpen = true;         // SPEED accordion in the Record column
    bool noclipOpen = true;        // NOCLIP accordion in the Record column
    bool frameStepperOpen = true;  // FRAME STEPPER accordion in the Record column
    // KEYBIND rows used to be an accordion in the Settings tab; they now live on
    // their OWN screen, reached from the pinned KEYBINDS button. Only the rows
    // ease in/out here — the "KEYBINDS" title is a pinned page heading.
    bool keybindOpen = true;       // KEYBINDS rows on the dedicated screen
    // Sub-screen flag. False = Settings proper (ACCURACY / COSMETICS / LOGGING),
    // true = the keybind screen pushed over it, closed with BACK.
    bool keybindScreen = false;
    bool cosmeticsOpen = true;     // COSMETICS accordion (under KEYBIND)
    bool loggingOpen = true;       // LOGGING accordion in the Settings tab
    bool cosmeticsInit = false;    // gui_solid_background loaded once
    std::vector<std::string> fontItems;
    bool fontsDirty = true;
    bool fontsFolderDownPrev = false;

    bool waveTrailOpen = true;     // WAVE TRAIL FIX accordion in the Assist tab

    // v4 TOASTS (14.11): in-shell action feedback. The GD system notification
    // (Notification::create) drops a foreign-styled bubble over the level and
    // ignores the shell's theme entirely. Shell toasts are small pills stacked
    // under the header, easing in and out — same visual language as everything
    // else in the shell.
    struct Toast {
        std::string text;
        ImU32 col;
        double born;
    };
    std::deque<Toast> toasts;

    // v4 WINDOW ANIMATION (14.11): open/close animation of the whole shell,
    // user-configurable in COSMETICS (toggle + duration). shellWasOpen/
    // shellCloseT drive the exit: when `open` flips false the shell keeps
    // rendering for shellAnimSec, sliding down 14k, with widgets inert
    // (SkipItems), then performs the normal close cleanup.
    bool shellAnim = true;
    float shellAnimSec = 0.05f;
    bool shellWasOpen = false;
    float shellCloseT = 0.0f;

    // RENDER tab: the settings mirror. Loaded once from the mod's saved values
    // (loadRenderSettings) the first time the RENDER tab is shown, then kept in
    // sync — every edit persists back AND is applied to the live renderer, so a
    // render started later picks them up. renderEncoder is the picked video
    // encoder name (kept separately: the renderer stores it by name too, but
    // the GUI owns the auto-pick + fallback logic).
    skkbot2::render::Renderer::RenderSettings render{};
    std::string renderEncoder;
    bool renderInit = false;  // saved values loaded + FFmpeg probed once

    // Macro search dropdown geometry (Record page).
    ImVec2 searchMin = ImVec2(0, 0);
    ImVec2 searchBoxMax = ImVec2(0, 0);  // lower-right corner of the open list (absolute)
    float searchW = 0.0f;
    float searchScroll = 0.0f;  // row-aligned scroll offset inside the macro list
    bool searchForced = false;
    bool searchVisible = false;

    // Dropdown entrance: runs 0.f -> 1.f while the library list is open and is
    // reset to 0.f on the rising edge (closed -> open). Rows reveal staggered
    // off it (slide-up + fade) exactly like the hover wash glides in.
    float searchEntrT = 130.0f;  // start settled so a MACRO is never re-staggered
    bool searchEntrPrevOpen = false;
    float searchEntrCursor = 0.0f;

    // Animated container reveal (height & fade)
    float searchOpenT = 0.0f;

    // Refresh icon spin animation (radians)
    float refreshAngle = 0.0f;

    // Scrollbar interactive drag
    bool scrollDragging = false;
    float scrollDragStartY = 0.0f;
    float scrollDragStartScroll = 0.0f;

    // Body scroll (SETTINGS / RENDER tabs): the stack of collapsible sections
    // can outgrow the body viewport when everything is open, so the region
    // below the pinned tab heading scrolls inside a clip. The offset persists
    // across frames (content height is only known AFTER the sections are
    // drawn, so the clamp runs post-draw); reset to 0 whenever the tab changes.
    float bodyScroll = 0.0f;
    bool bodyScrollDragging = false;
    float bodyScrollDragY = 0.0f;
    float bodyScrollDragStart = 0.0f;
    // True on the frame the body scrollbar consumed an LMB press (thumb or
    // track click). The window-drag gate in render() reads it: the scrollbar
    // never registers an ImGui item, so IsAnyItemActive() stays false and the
    // drag gate would otherwise grab the window the same moment the thumb
    // drag starts (the whole menu slid while scrolling).
    bool bodyScrollClick = false;

    // Left column of RECORD below the timing divider: the TPS / Speed /
    // HITBOXES / LAYOUT MODE run scrolls in its own clip.
    // Only engages when the run is taller than the viewport; the scrollbar
    // reuses applyRegionScroll unchanged.
    float timingScroll = 0.0f;
    bool timingScrollDragging = false;
    float timingScrollDragY = 0.0f;
    float timingScrollDragStart = 0.0f;
    bool timingScrollClick = false;

    // Right column of RECORD (the NOCLIP stack): independent scroll, same
    // contract as timingScroll. Separate state so dragging one column's thumb
    // cannot move the other.
    float noclipScroll = 0.0f;
    bool noclipScrollDragging = false;
    float noclipScrollDragY = 0.0f;
    float noclipScrollDragStart = 0.0f;
    bool noclipScrollClick = false;

    // True on the frame the pinned RENDER action button (START/STOP RENDER)
    // consumed an LMB press. The button is drawn with an InvisibleButton, which
    // normally registers as the active item and blocks the drag gate via
    // IsAnyItemActive() — but this GD-fork imgui build can miss the hover/press
    // registration on the FIRST click after the window appears or after it was
    // dragged (IsWindowHovered is unreliable while LMB is held, see the drag
    // gate comment in render()). When that happens the press falls through to
    // empty space, IsAnyItemActive() stays false and the drag gate steals the
    // click to start a window grab instead of starting the render. The manual
    // hit-test in drawRenderAction sets this latch every time the press lands
    // inside the button rect, regardless of ImGui's item state, and the drag
    // gate consumes it the same way it consumes bodyScrollClick (BUG-20).
    bool actionBtnClick = false;

    // Last engine macroListVersion the shell synced (see render(): after an
    // async save reloads the engine list, the shell re-copies storedMacros).
    uint32_t seenMacroVersion = 0;

    // Cached dropdown rows: the (filtered) macro names and their per-row size
    // captions are rebuilt ONLY when the search query changes or the library is
    // refreshed — never per frame. Before the cache, drawing the list called
    // fs::file_size per visible row every frame and copied the whole
    // storedMacros vector each frame, which stuttered the menu on slow drives.
    std::vector<std::string> filtered;     // names currently shown
    std::vector<std::string> filteredInfo; // matching "12.3 KB В· .skk" captions
    std::string filterQuery;               // query the caches were built for
    bool filterDirty = true;               // rebuild on next frame

    // Folder button press latch (press-toggle is edge-triggered on IsItemActive).
    bool folderDownPrev = false;

    // Same edge latch for the "open macros folder" button next to the refresh icon.
    bool openFolderPrevDown = false;

    // Macro delete confirmation state (name of macro currently awaiting deletion confirmation)
    std::string deleteConfirmMacro;
    bool rmbDownPrev = false;

    // Window drag (Silicat-style): grab starts only from empty space, then the
    // shell follows the mouse even over widget areas.
    bool dragGrab = false;
    bool dragMoved = false;

    // v4: shell entrance. The shell slides up ~14px on every open (the beta
    // shell renders every frame even while closed, so the rising edge of
    // `open` is tracked here). Settled at 1.0 — never animates while open.
    float shellEntrT = 1.0f;
    bool shellEntrPrev = false;
};

State& state() {
    static State value;
    return value;
}

// v4 WINDOW ANIMATION: true while the shell is in its closing ghost frames.
// Manual (non-widget) input paths — the wheel/drag handlers, the search
// dropdown — check this and stay inert; widget input is disabled centrally.
bool gShellClosing = false;

// v4 PERSISTENCE (14.11): the shell used to forget which tab the user was on
// and which accordions were collapsed — every GD start reset everything to
// all-open Record, so a user who keeps NOCLIP folded and lives on SETTINGS
// re-folded it every single launch. The flags ride on the mod's saved values:
// restored once on the first rendered frame, written back on the first frame
// where the fingerprint differs from the last written one (so flipping a
// section mid-session persists without any per-frame disk traffic).
constexpr const char* kUiStateKey = "beta_gui_state";

// v4 TOASTS: push a pill message. A new toast REPLACES the current one —
// repeated actions (e.g. deleting several macros) refresh the same pill
// instead of stacking a pile of fading bubbles.
void pushToast(State& st, std::string text, ImU32 col) {
    st.toasts.clear();
    st.toasts.push_back(State::Toast{std::move(text), col, ImGui::GetTime()});
}

void loadUiState(State& st) {
    auto* mod = Mod::get();
    if (!mod) return;
    const std::string s = mod->getSavedValue<std::string>(kUiStateKey, "");
    if (s.size() < 12) return;
    const auto flag = [&](size_t i) { return s[i] == '1'; };
    st.tab = s[0] == '1' ? Tab::Record
             : s[0] == '2' ? Tab::Assist
             : s[0] == '3' ? Tab::Render
                           : Tab::Settings;
    st.lastTab = st.tab;
    st.recordOpen = flag(1);
    st.speedOpen = flag(2);
    st.noclipOpen = flag(3);
    st.frameStepperOpen = flag(4);
    st.renderOpen = flag(5);
    st.audioOpen = flag(6);
    st.macroBotOpen = flag(7);
    st.settingsOpen = flag(8);
    st.cosmeticsOpen = flag(9);
    st.loggingOpen = flag(10);
    st.waveTrailOpen = flag(11);
}

void saveUiState(State& st) {
    auto* mod = Mod::get();
    if (!mod) return;
    char buf[16];
    const int t = st.tab == Tab::Record    ? 1
                  : st.tab == Tab::Assist  ? 2
                  : st.tab == Tab::Render  ? 3
                                           : 4;
    std::snprintf(buf, sizeof(buf), "%d%d%d%d%d%d%d%d%d%d%d%d", t,
                  st.recordOpen ? 1 : 0, st.speedOpen ? 1 : 0,
                  st.noclipOpen ? 1 : 0, st.frameStepperOpen ? 1 : 0,
                  st.renderOpen ? 1 : 0, st.audioOpen ? 1 : 0,
                  st.macroBotOpen ? 1 : 0, st.settingsOpen ? 1 : 0,
                  st.cosmeticsOpen ? 1 : 0, st.loggingOpen ? 1 : 0,
                  st.waveTrailOpen ? 1 : 0);
    mod->setSavedValue(kUiStateKey, std::string(buf));
}

void syncUiStateSave(State& st) {
    static std::string lastWritten;
    char buf[16];
    const int t = st.tab == Tab::Record    ? 1
                  : st.tab == Tab::Assist  ? 2
                  : st.tab == Tab::Render  ? 3
                                           : 4;
    std::snprintf(buf, sizeof(buf), "%d%d%d%d%d%d%d%d%d%d%d%d", t,
                  st.recordOpen ? 1 : 0, st.speedOpen ? 1 : 0,
                  st.noclipOpen ? 1 : 0, st.frameStepperOpen ? 1 : 0,
                  st.renderOpen ? 1 : 0, st.audioOpen ? 1 : 0,
                  st.macroBotOpen ? 1 : 0, st.settingsOpen ? 1 : 0,
                  st.cosmeticsOpen ? 1 : 0, st.loggingOpen ? 1 : 0,
                  st.waveTrailOpen ? 1 : 0);
    if (buf != lastWritten) {
        lastWritten = buf;
        saveUiState(st);
    }
}

// v4 PERSISTENCE: the shell's position rides on the same saved-values store.
// Coordinates are stored as integers so a resolution change cannot resurrect
// subpixel junk; load clamps into the current display anyway.
constexpr const char* kUiPosKey = "beta_gui_pos";

void saveWindowPos(ImVec2 pos) {
    auto* mod = Mod::get();
    if (!mod) return;
    mod->setSavedValue(kUiPosKey,
                       std::to_string(static_cast<int>(pos.x)) + " " +
                           std::to_string(static_cast<int>(pos.y)));
}

void loadWindowPos(ImVec2& pos, float dispW, float dispH) {
    auto* mod = Mod::get();
    if (!mod) return;
    const std::string s = mod->getSavedValue<std::string>(kUiPosKey, "");
    if (s.empty()) return;
    const size_t sp = s.find(' ');
    if (sp == std::string::npos) return;
    try {
        pos.x = static_cast<float>(std::stoi(s.substr(0, sp)));
        pos.y = static_cast<float>(std::stoi(s.substr(sp + 1)));
    } catch (...) {
        return;  // a corrupt value must never trap the shell off-screen
    }
    (void)dispW;
    (void)dispH;
}

struct Status {
    const char* text;
    ImU32 color;
};

Status engineStatus() {
    auto& gui = skkbot2::gui::get();
    if (gui.macroBotType == Gui::MacroBotType::SKC) {
        auto& bot = skk::skc::Bot::get();
        if (bot.getState() == skk::skc::State::Record) return {"RECORDING", theme::Record};
        if (bot.getState() == skk::skc::State::Playback) return {"PLAYING", theme::Accent};
        if (bot.isPlaybackArmed() ||
            bot.getPendingAction() == skk::skc::PendingAction::Play) return {"ARMED", theme::Amber};
        if (bot.getPendingAction() == skk::skc::PendingAction::Record) return {"QUEUED", theme::Amber};
        return {"STANDBY", theme::Muted};
    }
    auto* engine = ReplayEngine::get();
    auto& renderer = render::Renderer::get();
    if (renderer.isRecording()) return {"RENDERING", theme::Mint};
    if (engine->engineMode == MODE_CAPTURE) return {"RECORDING", theme::Record};
    if (engine->engineMode == MODE_EXECUTE) return {"PLAYING", theme::Accent};
    if (engine->pendingPlaybackStart ||
        engine->pendingAction == ReplayEngine::PendingAction::Play) return {"ARMED", theme::Amber};
    if (engine->pendingAction == ReplayEngine::PendingAction::Record) return {"QUEUED", theme::Amber};
    return {"STANDBY", theme::Muted};
}

ImFont* mono() {
    // Everything draws with the one default font (Roboto regular / font_main).
    // The old mono/semibold faces are gone: readouts, numbers and tags render
    // with the exact same typeface as captions and headings.
    return ImGui::GetFont();
}

// ---------------------------------------------------------------------------
// Shell chrome
// ---------------------------------------------------------------------------

// BUG-132 diagnostics: shared between drawShell (which owns the mode cycling)
// and the header/wash fills further down, so DIAG 3 can recolour the chrome
// layers. 0 = normal, 1 = glass tint, 2 = no glass + no shadow,
// 3 = chrome colours (base wash BLUE, header ORANGE, no glass/shadow).
static int g_cornerDiagMode = 0;

void drawShell(ImDrawList* draw, ImVec2 min, ImVec2 max, float k) {
    const float r = 28.0f * k;
    // BUG-132 final geometry: the mask arc is back to the chrome arc (28k).
    // The +bias experiments (+2/+5/+8/+28) chased a wrong theory — the F10
    // probes eventually isolated the SHADOW as the notch's source, and the
    // shadow is now a filled-layer render (drawShellShadow). No bias.
    constexpr float kMaskRadiusBias = 0.0f;

    // 0. Hardware Dual-Kawase Background Blur under the glass. The capture
    // runs before this window's own draw data is emitted each frame, so the
    // region under the glass is pristine game content and blurs live while the
    // window is being dragged (the drag logic snaps the shell to whole pixels).
    // The blur region's alpha tracks the menu-opacity multiplier: the blur
    // paints opaque by default, so without this a lowered opacity would leave a
    // fully opaque blur slab under a clear shell — the menu would look flat and
    // the blur would read as a flickering solid square instead of fading with
    // the glass.
    // COSMETICS: both modes are glassmorphic now (BUG-118). "Solid" mimics the
    // δbot/Silicate look: the backdrop must dissolve into pure colour fields —
    // no readable shapes. That needs the FULL Kawase chain: radius 10.0 ->
    // levels 6 (chain clamp raised 3->6), down to ~1/32 resolution - "a hair
    // stronger" over the 5-level pass. The dark translucent tint (alpha 185)
    // keeps the colours saturated and lighting the panel. The default light
    // glass keeps radius 1.0.
    // Corner diagnosis (13.11, BUG-132): F10 cycles FOUR modes:
    //   0 — normal render;
    //   1 — glass mask tint: blur still drawn, the shader shows GREEN where
    //       the glass is opaque, MAGENTA in the AA/feather band and the RAW
    //       PASTE CONTENT in the rounded-away zone;
    //   2 — everything external OFF: no blur AND no shell shadow. Only the
    //       ImGui wash layers remain. If the notch survives here too, it is
    //       drawn by the ImGui chrome, not by the glass or the shadow;
    //   3 — chrome colours: glass and shadow off, base wash BLUE, header
    //       ORANGE. Whichever colour the notch shows names the guilty layer.
    //   4 — blur mask radius +8px: the SDF arc moves OUTSIDE the chrome arc;
    //   5 — blur mask radius -8px: the SDF arc cuts INSIDE the chrome arc.
    //       If the notch grows/shrinks with the bias, the mask/chrome arc
    //       mismatch is confirmed and its sign is measured directly.
    //   6 — shadow ON, blur OFF: the missing combo (2 is BOTH off, 0 is BOTH
    //       on). If the notch shows here, the soft halo is the culprit.
    int diagMode = 0;
    {
        static int mode = 0;
        static bool f10Prev = false;
        const bool f10Now = kDebugShell && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10Now && !f10Prev) mode = (mode + 1) % 7;
        f10Prev = f10Now;
        diagMode = mode;
        g_cornerDiagMode = mode;
    }
    blur::setDebugTint(diagMode == 1);
    blur::setDebugRoundBias(diagMode == 4 ? 8.0f : diagMode == 5 ? -8.0f : 0.0f);
    // Blur is drawn in 0/1 (normal + tint) and in 4/5 (radius bias probes).
    // NOTE: modes 4/5 MUST submit — otherwise the bias has nothing to act on
    // (first probe build drew no blur there and the notch vanished with it,
    // which itself proved the notch needs the blur paste).
    // v4 BLUR TOGGLE: the user can switch the Kawase chain off entirely — the
    // shell then paints a flat opaque colour instead. Independent of Solid
    // (user 14.11): the toggle applies in BOTH shell modes; the light glass
    // simply falls back to its opaque base tone.
    const bool useBlur = theme::blurEnabled;
    if ((diagMode == 0 || diagMode == 1 || diagMode == 4 || diagMode == 5) &&
        useBlur && blur::available()) {
        blur::submit(draw, min, max, r + kMaskRadiusBias,
                     theme::solidBackground ? 10.0f : 1.0f,
                     theme::menuOpacity);
    }
    // v4: soft drop shadow around the whole shell. ORDER MATTERS (BUG-132,
    // 13.11): the blur capture takes its snapshot at the first region
    // callback, i.e. AFTER every ImGui command queued before it. With the
    // shadow drawn BEFORE blur::submit its arc-hugging stroke bands landed in
    // the snapshot and were pasted back into the rounded-away corner chunks —
    // soft dark arc ghosts that read as a punched-out notch at all four
    // corners (DIAG 3, chrome only, was clean — glass+shadow was not). The
    // shadow must therefore come AFTER the blur submit. The strokes lie
    // outside the shell arc and only feather ~1px over the glass rim, so the
    // visual difference is a slightly darker edge fringe.
    if (diagMode <= 1 || diagMode == 6) {
        drawShellShadow(draw, min, max, r, theme::rgb(6, 10, 18), 0.55f, k);
    }
    if (diagMode != 0) {
        draw->AddRect(min, max, IM_COL32(80, 255, 120, 255), r, 0, 1.2f);
        const char* lbl = diagMode == 1
                              ? "DIAG 1: green=glass magenta=feather raw=paste"
                              : diagMode == 2
                                    ? "DIAG 2: no glass + no shadow"
                                    : diagMode == 3
                                          ? "DIAG 3: wash=BLUE header=ORANGE"
                                          : diagMode == 4
                                                ? "DIAG 4: mask radius +8px"
                                                : diagMode == 5
                                                      ? "DIAG 5: mask radius -8px"
                                                      : "DIAG 6: shadow ON blur OFF";
        // Top-left, so corner crops unambiguously identify the mode.
        draw->AddText(ImVec2(min.x + 10.0f, min.y + 4.0f),
                      IM_COL32(120, 255, 150, 255), lbl);
        draw->AddText(ImVec2(min.x + 10.0f, max.y - 22.0f * k),
                      IM_COL32(120, 255, 150, 255), lbl);
    }

    // BUG-126 debug: F9 toggles a magenta outline drawn EXACTLY at the blur
    // region's rect (min/max, rounded r). If the user's protruding edge
    // coincides with this line, the blur rect itself is the problem; if the
    // protrusion sits outside this line, the artefact belongs to the shell
    // chrome (base wash / border halo), not the blur. Edge-detected so a held
    // key only toggles once.
    {
        static bool blurDebugOn = false;
        static bool f9Prev = false;
        const bool f9Now = kDebugShell && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9Now && !f9Prev) blurDebugOn = !blurDebugOn;
        f9Prev = f9Now;
        if (blurDebugOn) {
            draw->AddRect(min, max, IM_COL32(255, 0, 200, 235), r, 0, 3.0f);
            draw->AddRect(ImVec2(min.x - 1, min.y - 1), ImVec2(max.x + 1, max.y + 1),
                          IM_COL32(255, 0, 200, 90), r + 1.0f, 0, 2.0f);
        }
    }

    // 1. Glass base background — v4 (final, 13.11): static wash. Two previous
    // iterations of a two-tone vertical gradient both showed a visible seam
    // band mid-shell (the feather strips read as a lighter strip over the
    // blur), so the gradient idea is dropped: one flat translucent wash, same
    // alpha range as the old breathing version (88-108), just no motion.
    ImU32 glassBg = theme::rgb(35, 51, 83, 98);
    if (theme::solidBackground) {
        // v4 BLUR TOGGLE: with the blur off the wash must carry the whole
        // background alone — same tone, fully opaque, no translucency.
        glassBg = useBlur ? theme::rgb(12, 16, 26, 185)
                          : theme::rgb(12, 16, 26, 255);
    } else if (!useBlur) {
        // Light glass with the blur off: same fallback — opaque flat tone.
        glassBg = theme::rgb(35, 51, 83, 255);
    }
    // DIAG 3: paint the base wash a loud flat blue so the corner notch, if it
    // belongs to this layer, lights up unmistakably.
    if (g_cornerDiagMode == 3) glassBg = IM_COL32(40, 120, 255, 255);
    draw->AddRectFilled(min, max, glassBg, r);

    // 2. Border. Both modes: clean edge (rule 10.11). The solid mode used to
    // draw a two-tone azure/sapphire rim along the shell bounds — the user
    // (13.11) flagged exactly that as a stray dim-blue strip hugging the left
    // edge (Solid-only, which is why the light glass never showed it); the
    // shell keeps its weight from the v4 drop shadow instead of a rim line.
    if (!theme::solidBackground) {
        (void)r;
    }
}

void drawBrand(ImDrawList* draw, ImVec2 topLeft, float cy, ImFont* bold, float k) {
    float x = topLeft.x;

    // Brand symbol — three input-wave bars, like the mock's mark.
    const float barH[] = {12.0f * k, 21.0f * k, 16.0f * k};
    const float bw = 5.0f * k;
    for (int i = 0; i < 3; ++i) {
        draw->AddRectFilled(ImVec2(x + i * 9.5f * k, cy + 10.0f * k - barH[i]),
                            ImVec2(x + i * 9.5f * k + bw, cy + 10.0f * k),
                            theme::Accent, 2.2f * k);
    }
    x += 33.0f * k;

    // Wordmark "skkBot" + superscript 2.
    ImFont* titleF = bold ? bold : ImGui::GetFont();
    const float titleSize = pxSize(ImGui::GetFontSize() * 1.55f);
    ImVec2 ws = titleF->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "skkBot");
    const float wy = cy - ws.y * 0.5f;
    draw->AddText(titleF, titleSize, ImVec2(x, wy), theme::Text, "skkBot");
    const float supSize = pxSize(titleSize * 0.52f);
    ImVec2 sup = titleF->CalcTextSizeA(supSize, FLT_MAX, 0.0f, "2");
    draw->AddText(titleF, supSize, ImVec2(x + ws.x + 1.0f * k, wy + 1.0f * k),
                  theme::Accent, "2");
    x += ws.x + 14.0f * k;
}

void drawTopTabs(ImDrawList* draw, ImVec2 min, float width, float headerH, float k) {
    ImFont* mf = mono();
    const float kt = uiTextScale();
    const float size = pxSize(11.0f * kt);
    const float spacing = 1.1f * k;
    const float gap = 32.0f * k;
    const float pad = 3.0f * k;

    // Layout is deterministic every frame: compute each tab's x/width/center.
    float textW[kTabCount], tabW[kTabCount], tabX[kTabCount], tabCX[kTabCount];
    float total = 0.0f;
    for (int i = 0; i < kTabCount; ++i) {
        textW[i] = spacedWidth(mf, size, kTabNames[i], spacing);
        tabW[i] = textW[i] + pad * 2.0f;
        total += tabW[i];
    }
    const float cy = min.y + headerH * 0.5f;
    const float centerX = min.x + width * 0.5f;
    float x = centerX - (total + gap * (kTabCount - 1)) * 0.5f;
    for (int i = 0; i < kTabCount; ++i) {
        tabX[i] = x;
        tabCX[i] = x + tabW[i] * 0.5f;
        x += tabW[i] + gap;
    }

    int hoveredTab = -1;
    // Hitboxes are CONTIGUOUS even though captions are spaced: each tab's zone
    // spans from the mid-gap before it to the mid-gap after it. A fast sweep
    // across the bar then never lands in a dead zone between captions — that
    // used to clear the hover and yank the indicator back onto the active tab.
    const float barMin = centerX - (total + gap * (kTabCount - 1)) * 0.5f;
    const float barMax = centerX + (total + gap * (kTabCount - 1)) * 0.5f;
    for (int i = 0; i < kTabCount; ++i) {
        const float hitL = i == 0 ? barMin : (tabCX[i - 1] + tabCX[i]) * 0.5f;
        const float hitR = i == kTabCount - 1 ? barMax : (tabCX[i] + tabCX[i + 1]) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(hitL, min.y));
        ImGui::InvisibleButton((std::string("##beta_tab") + std::to_string(i)).c_str(),
                               ImVec2(hitR - hitL, headerH));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) state().tab = static_cast<Tab>(i);
        if (ImGui::IsItemHovered()) {
            hoveredTab = i;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }
    const int act = static_cast<int>(state().tab);
    const bool previewing = hoveredTab >= 0 && hoveredTab != act;

    const ImGuiID tbase = ImGui::GetID("##beta_tabs");

    // Tab caption: Muted -> AccentLight (hover) -> Text (active), all eased.
    for (int i = 0; i < kTabCount; ++i) {
        const float tgt = i == act ? 2.0f : i == hoveredTab ? 1.0f : 0.0f;
        const float tv = animF(tbase + static_cast<ImGuiID>(i) + 0x1000, tgt, 14.0f);
        const ImU32 col = tv < 1.0f
            ? mixColor(theme::Muted, theme::AccentLight, tv)
            : mixColor(theme::AccentLight, theme::Text, tv - 1.0f);
        const float textH = mf ? mf->CalcTextSizeA(size, FLT_MAX, 0.0f, kTabNames[i]).y
                               : ImGui::GetFontSize();
        spacedText(draw, mf, size, ImVec2(tabX[i] + pad, cy - textH * 0.5f), kTabNames[i],
                   col, spacing);
    }

    // ONE indicator line slides across the bar: it lives on the active tab and
    // glides toward whichever inactive tab is hovered, tinting accent while it
    // previews the switch. Settles back to white when the new tab lands.
    const float indH = 2.0f * k;
    const float indY = min.y + headerH - indH;
    const int target = hoveredTab >= 0 ? hoveredTab : act;
    const float slideX = animF(tbase + 0x2000, tabCX[target], 32.0f);
    const float slideW = animF(tbase + 0x2001, tabW[target], 32.0f);
    const float switchAmt = anim01(tbase + 0x2002, previewing ? 1.0f : 0.0f, 14.0f);
    const ImU32 indCol = previewing
        ? mixColor(theme::rgbText(240, 242, 247), theme::AccentLight, switchAmt)
        : theme::rgbText(240, 242, 247);
    const float len = (slideW - pad * 2.0f) * 0.5f;
    // v4: pill indicator with a soft accent halo — the plain 2px line read as
    // a bare rule; the halo ties it to the brand's luminous accent language.
    drawSoftHalo(draw, ImVec2(slideX - len, indY), ImVec2(slideX + len, indY + indH),
                 indH * 0.5f, theme::Accent, 0.45f, k);
    draw->AddRectFilled(ImVec2(slideX - len, indY), ImVec2(slideX + len, indY + indH), indCol,
                        indH * 0.5f);
}

// ---------------------------------------------------------------------------
// Section heading (the mock's accordion header): rule | title | rule | chevron
// ---------------------------------------------------------------------------

bool sectionHeading(ImDrawList* draw, ImVec2 origin, float width, float y,
                    const char* title, bool open, const char* endLabel,
                    float k, float scale = 1.0f, const char* idStr = "main") {
    ImFont* mf = mono();
    ImFont* df = ImGui::GetFont();
    const float kt = uiTextScale();
    // Main headings 13px; mini headings (scale 0.8, e.g. the right-column
    // NOCLIP header) keep 11px — computing from the base would shrink them too.
    const float size = pxSize((scale >= 1.0f ? 13.0f : 11.0f) * kt);
    ImVec2 tv = df->CalcTextSizeA(size, FLT_MAX, 0, title);
    const float titleW = tv.x;
    // No dot/accent square before the title anymore. Both rules are symmetric:
    // each is inset from its content edge by the same amount (the chevron zone
    // on the right is mirrored by an equal empty margin on the left), and they
    // sit on the title's optical center line, so the rules frame the caption
    // exactly in the middle and never touch the chevron arrows.
    const float centerX = origin.x + width * 0.5f;
    const float titleX = centerX - titleW * 0.5f;

    const float rowY = y;
    // The hitbox is centered on the caption row and must never dip into the
    // content below: scale 1 gives [rowY - k, rowY + 19k], clearing gridTop
    // (= rowY + 20k) where NOCLIP's own heading starts. The old 26k box ran
    // to rowY + 25.5k and swallowed the top ~6k strip of NOCLIP's click
    // target, stealing hovers/clicks from it.
    const float hitH = 20.0f * k * scale;
    const float hitY = rowY - 1.0f * k * scale;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, hitY));
    const std::string btnId = std::string("##beta_section_head_") + idStr;
    ImGui::InvisibleButton(btnId.c_str(), ImVec2(width, hitH));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const ImGuiID hkey = ImGui::GetID(btnId.c_str());
    const float hov = anim01(hkey ^ 0x4844, hovered ? 1.0f : 0.0f, 13.0f);

    const ImU32 ruleCol = mixColor(theme::Line, theme::rgb(85, 97, 122), hov);
    const ImU32 titleCol = mixColor(theme::rgbText(215, 223, 238), theme::Text, hov);

    // The rule line sits on the optical center of the ACTUAL caption, not on
    // its top edge. Measuring the real string matters: glyphs like '&' carry
    // their own ink box (descenders / overshoot), so centering on "0" alone
    // lands the line a hair off the true middle of "RECORD & PLAY".
    InkMeasure ink;
    const bool haveInk = measureGlyphInk(df, size, title, ink);
    const float lineY = haveInk ? rowY + (ink.y0 + ink.y1) * 0.5f : rowY + size * 0.5f;

    // Hover cue: the WHOLE heading row lights up (a full-width band), not just the
    // caption. The band is centered on the caption's ink so the wash sits even
    // above and below the text, and its ends are rounded — no sharp corners and
    // no bottom-heavy strip.
    if (hov > 0.004f) {
        const float bandH = hitH;
        const float bandY = lineY - bandH * 0.5f;
        draw->AddRectFilled(ImVec2(origin.x, bandY),
                            ImVec2(origin.x + width, bandY + bandH),
                            withAlpha(theme::Accent, 0.05f * hov), bandH * 0.5f);
    }

    // The title is the TPS/Speed caption style: default (non-mono) font at
    // heading size, no letter-spacing, rendered at the exact geometric center
    // of the row (titleW comes from the same font so text and box agree).
    draw->AddText(df, size, ImVec2(titleX, rowY), titleCol, title);
    // The chevron zone stays FULL 26k regardless of `scale` (0.8 mini headings
    // like NOCLIP); because the right column ends flush with the body's right
    // edge, the mini heading's arrow then lands exactly under the full-width
    // "RECORD & PLAY" chevron instead of drifting inward.
    const float chevW = 26.0f * k;  // chevron zone, mirrored on the left margin
    const float gap = 18.0f * k * scale;
    // Gradient lines: brighter closer to the title text, fading towards outer edges.
    const ImU32 ruleOuter = withAlpha(ruleCol, 0.15f);
    const ImU32 ruleInner = mixColor(ruleCol, theme::AccentLight, 0.55f);
    // Left rule: outer edge (chevW) -> inner edge near title (titleX - gap)
    drawHGradientLine(draw, ImVec2(origin.x + chevW, lineY), ImVec2(titleX - gap, lineY),
                      ruleOuter, ruleInner, 1.0f);
    // Right rule: inner edge near title (titleX + titleW + gap) -> outer edge (origin.x + width - chevW)
    drawHGradientLine(draw, ImVec2(titleX + titleW + gap, lineY),
                      ImVec2(origin.x + width - chevW, lineY), ruleInner, ruleOuter, 1.0f);

    // Chevron lives in its own right zone, so the rules never cross the arrows.
    // It rotates open<->closed smoothly instead of swapping between the two
    // `else` branches' orientations.
    const ImVec2 c(origin.x + width - chevW * 0.5f, lineY);
    const float r = 3.0f * k;
    const ImU32 chevCol = mixColor(theme::rgb(150, 158, 178), theme::Accent, hov);
    const float rot = anim01(hkey ^ 0x4348, open ? 1.0f : 0.0f, 13.0f);
    // Open:  (-r,-1) -> (0,+2) -> (+r,-1).  Closed: (-1,-r) -> (+2,0) -> (-1,+r).
    const ImVec2 a(lerp(-1.0f, -r, rot), lerp(-r, -1.0f, rot));
    const ImVec2 b(lerp(2.0f, 0.0f, rot), lerp(0.0f, 2.0f, rot));
    const ImVec2 e(lerp(-1.0f, r, rot), lerp(r, -1.0f, rot));
    draw->AddLine(ImVec2(c.x + a.x, c.y + a.y), ImVec2(c.x + b.x, c.y + b.y), chevCol, 1.5f);
    draw->AddLine(ImVec2(c.x + b.x, c.y + b.y), ImVec2(c.x + e.x, c.y + e.y), chevCol, 1.5f);

    if (endLabel) {
        const float eSize = pxSize(9.0f * kt * scale);
        ImVec2 es = mf ? mf->CalcTextSizeA(eSize, FLT_MAX, 0.0f, endLabel)
                       : ImGui::CalcTextSize(endLabel);
        draw->AddText(mf, eSize,
                      ImVec2(origin.x + width - 45.0f * k * scale - es.x, rowY - 0.5f * k),
                      theme::rgb(146, 156, 176), endLabel);
    }
    return clicked;
}

// ---------------------------------------------------------------------------
// Footer: engine status | cur|total readout
// ---------------------------------------------------------------------------

void drawFooter(ImDrawList* draw, ImVec2 min, ImVec2 max, float footerH, float k) {
    // Bottom corners keep the shell's rounding (the footer is the shell's last
    // row; a square fill would cover the rounded corner radius).
    draw->AddRectFilled(min, max, theme::FooterBg, 28.0f * k,
                        ImDrawFlags_RoundCornersBottom);
    draw->AddLine(ImVec2(min.x, min.y), ImVec2(max.x, min.y), theme::LineFooter, 1.0f);

    const float cy = min.y + footerH * 0.5f;
    ImFont* mf = mono();
    const float kt = uiTextScale();
    const Status status = engineStatus();

    // Engine status (left). v4: when the engine is live (record/play/render)
    // the dot gets a slow breathing halo so a long-running capture is visible
    // from across the room; standby keeps a calm static ring.
    auto& gui = skkbot2::gui::get();
    auto* engine = ReplayEngine::get();
    const bool live = (gui.macroBotType == Gui::MacroBotType::SKC)
        ? (skk::skc::Bot::get().getState() == skk::skc::State::Record || skk::skc::Bot::get().getState() == skk::skc::State::Playback)
        : (engine->engineMode == MODE_EXECUTE || engine->engineMode == MODE_CAPTURE);
    const float lx = min.x + 33.0f * k;
    const float pulse = live ? 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.4f)
                             : 0.0f;
    draw->AddCircleFilled(ImVec2(lx + 2.5f * k, cy), 2.5f * k, status.color, 16);
    draw->AddCircle(ImVec2(lx + 2.5f * k, cy), 4.2f * k + pulse * 1.6f * k,
                    withAlpha(status.color, 0.25f + 0.35f * pulse), 16);
    const float tSize = pxSize(9.0f * kt);
    const float tSpacing = 1.2f * kt;
    const float statusTextY = cy - (mf ? mf->CalcTextSizeA(tSize, FLT_MAX, 0, "STANDBY").y
                                       : ImGui::GetFontSize()) * 0.5f;
    spacedText(draw, mf, tSize, ImVec2(lx + 12.0f * k, statusTextY),
               status.text, status.color, tSpacing);

    // Input readout (right): current | total, unpadded so the idle state reads
    // "0 | 0" instead of a wide zero-padded "0000 / 0000" slab.
    size_t cur = 0, total = 0;
    if (gui.macroBotType == Gui::MacroBotType::SKC) {
        auto& bot = skk::skc::Bot::get();
        total = bot.getInputCount();
        if (bot.getState() == skk::skc::State::Playback) cur = bot.getInputIndex();
        else if (bot.getState() == skk::skc::State::Record) cur = total;
    } else {
        if (engine->activeSk) total = engine->activeSk->inputs.size();
        if (engine->activeMacro) total = std::max(total, engine->activeMacro->inputs.size());
        if (engine->engineMode == MODE_EXECUTE) cur = engine->executeIndex;
        else if (engine->engineMode == MODE_CAPTURE) cur = total;
    }
    char curS[16], totalS[16];
    std::snprintf(curS, sizeof(curS), "%zu", cur);
    std::snprintf(totalS, sizeof(totalS), "%zu", total);
    const float numSize = pxSize(18.0f * kt);
    const float digitH = mf ? mf->CalcTextSizeA(numSize, FLT_MAX, 0, "0").y
                            : ImGui::GetFontSize();
    ImVec2 cs = mf ? mf->CalcTextSizeA(numSize, FLT_MAX, 0, curS)
                   : ImGui::CalcTextSize(curS);
    ImVec2 ts = mf ? mf->CalcTextSizeA(numSize, FLT_MAX, 0, totalS)
                   : ImGui::CalcTextSize(totalS);
    const float barW = mf ? mf->CalcTextSizeA(numSize, FLT_MAX, 0, "|").x
                          : ImGui::CalcTextSize("|").x;
    const float gap = 9.0f * k;
    const float readoutRight = max.x - 30.0f * k;

    // Readout drawn right-to-left: total | cur. All digits use the menu's own
    // glass text color so the counter never stands out against the shell.
    // v4: while the engine is live the CURRENT count tints accent — progress
    // becomes glanceable and matches the breathing status dot.
    const ImU32 barCol = theme::Muted;
    const ImU32 numCol = theme::Text;
    const ImU32 curCol = live ? theme::Accent : numCol;
    float rx = readoutRight - ts.x;
    draw->AddText(mf, numSize, ImVec2(rx, cy - digitH * 0.5f), numCol, totalS);
    rx -= gap + barW;
    draw->AddText(mf, numSize, ImVec2(rx, cy - digitH * 0.5f), barCol, "|");
    rx -= gap + cs.x;
    draw->AddText(mf, numSize, ImVec2(rx, cy - digitH * 0.5f), curCol, curS);

    // v4 FOOTER HAIRLINE (14.11): while the engine is live a thin accent line
    // rides along the footer's top border, its width = input progress. The
    // count readout ("412 | 900") is precise but requires reading; the hairline
    // is glanceable from across the room. Record fills left→right like play.
    // At rest (standby) nothing is drawn — the plain border stays untouched.
    if (live && total > 0) {
        const float frac = std::clamp(static_cast<float>(cur) / static_cast<float>(total), 0.0f, 1.0f);
        const float w = (max.x - min.x) * frac;
        if (w > 0.5f) {
            // Dim base: the full-width ghost of the remaining span, so the
            // filled part reads as progress and not as a random accent dash.
            draw->AddLine(ImVec2(min.x, min.y), ImVec2(max.x, min.y),
                          withAlpha(theme::Accent, 0.10f), 2.0f * k);
            draw->AddLine(ImVec2(min.x, min.y), ImVec2(min.x + w, min.y),
                          withAlpha(theme::Accent, 0.85f), 2.0f * k);
        }
    }
}

// ---------------------------------------------------------------------------
// Record page — the redesign's transport / timing / orbit-visual workspace
// ---------------------------------------------------------------------------

void drawSearchGlyph(ImDrawList* draw, ImVec2 c, float k, ImU32 col) {
    draw->AddCircle(ImVec2(c.x, c.y), 4.5f * k, col, 24, 1.5f);
    draw->AddLine(ImVec2(c.x + 3.5f * k, c.y + 3.5f * k),
                  ImVec2(c.x + 8.5f * k, c.y + 8.5f * k), col, 1.8f);
}

void drawSaveGlyph(ImDrawList* draw, ImVec2 c, float k, ImU32 col) {
    draw->AddLine(ImVec2(c.x, c.y - 6.0f * k), ImVec2(c.x, c.y + 4.0f * k), col, 1.6f);
    draw->AddLine(ImVec2(c.x - 4.0f * k, c.y + 0.5f * k), ImVec2(c.x, c.y + 4.5f * k), col, 1.6f);
    draw->AddLine(ImVec2(c.x + 4.0f * k, c.y + 0.5f * k), ImVec2(c.x, c.y + 4.5f * k), col, 1.6f);
    draw->AddLine(ImVec2(c.x - 6.0f * k, c.y + 8.5f * k), ImVec2(c.x + 6.0f * k, c.y + 8.5f * k),
                  col, 1.6f);
}

void drawFolderGlyph(ImDrawList* draw, ImVec2 c, float k, ImU32 col) {
    draw->AddLine(ImVec2(c.x - 6.0f * k, c.y - 1.0f * k), ImVec2(c.x - 3.0f * k, c.y - 4.5f * k),
                  col, 1.6f);
    draw->AddLine(ImVec2(c.x - 3.0f * k, c.y - 4.5f * k), ImVec2(c.x + 6.0f * k, c.y - 4.5f * k),
                  col, 1.6f);
    draw->AddRect(ImVec2(c.x - 6.0f * k, c.y - 1.0f * k), ImVec2(c.x + 6.0f * k, c.y + 6.0f * k),
                  col, 1.8f * k);
    draw->AddLine(ImVec2(c.x - 6.0f * k, c.y - 1.0f * k), ImVec2(c.x + 6.0f * k, c.y - 1.0f * k),
                  col, 1.3f);
}

void drawClearGlyph(ImDrawList* draw, ImVec2 c, float k, ImU32 col) {
    const float r = 5.0f * k;
    draw->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 1.6f);
    draw->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), col, 1.6f);
}

void drawEraseGlyph(ImDrawList* draw, ImVec2 c, float k, ImU32 col) {
    // Backspace tag shape: pentagon pointing to the left with a small inner cross
    const float h = 5.0f * k;
    const float xTip = c.x - 7.0f * k;
    const float xCorner = c.x - 2.5f * k;
    const float xRight = c.x + 6.5f * k;
    const float yTop = c.y - h;
    const float yBot = c.y + h;

    draw->PathLineTo(ImVec2(xTip, c.y));
    draw->PathLineTo(ImVec2(xCorner, yTop));
    draw->PathLineTo(ImVec2(xRight, yTop));
    draw->PathLineTo(ImVec2(xRight, yBot));
    draw->PathLineTo(ImVec2(xCorner, yBot));
    draw->PathStroke(col, ImDrawFlags_Closed, 1.4f);

    // Small cross inside tag
    const float cx = c.x + 2.0f * k;
    const float cr = 2.2f * k;
    draw->AddLine(ImVec2(cx - cr, c.y - cr), ImVec2(cx + cr, c.y + cr), col, 1.4f);
    draw->AddLine(ImVec2(cx + cr, c.y - cr), ImVec2(cx - cr, c.y + cr), col, 1.4f);
}

// Circular reload arrow for the library list.
void drawRefreshGlyph(ImDrawList* draw, ImVec2 c, float k, ImU32 col, float angleOffset = 0.0f) {
    constexpr float kPi = 3.14159265f;
    const float r = 5.0f * k;
    const float a0 = -0.35f * kPi + angleOffset;
    const float a1 = 1.15f * kPi + angleOffset;
    draw->PathArcTo(c, r, a0, a1, 24);
    draw->PathStroke(col, 0, 1.5f);

    const ImVec2 tip(c.x + r * std::cos(a0), c.y + r * std::sin(a0));
    // Base triangle offsets before rotation (around 0,0)
    const float cosA = std::cos(angleOffset);
    const float sinA = std::sin(angleOffset);
    auto rotateVec = [&](float dx, float dy) -> ImVec2 {
        return ImVec2(tip.x + (dx * cosA - dy * sinA), tip.y + (dx * sinA + dy * cosA));
    };
    draw->AddTriangleFilled(rotateVec(2.4f * k, -3.4f * k),
                            rotateVec(4.0f * k, 1.4f * k),
                            rotateVec(-1.6f * k, 0.8f * k), col);
}

void drawPlayGlyph(ImDrawList* draw, ImVec2 c, float k, ImU32 col) {
    draw->AddTriangleFilled(ImVec2(c.x - 4.5f * k, c.y - 6.0f * k),
                            ImVec2(c.x - 4.5f * k, c.y + 6.0f * k),
                            ImVec2(c.x + 6.5f * k, c.y), col);
}

void drawStopGlyph(ImDrawList* draw, ImVec2 c, float k, ImU32 col) {
    draw->AddRectFilled(ImVec2(c.x - 5.0f * k, c.y - 5.0f * k),
                        ImVec2(c.x + 5.0f * k, c.y + 5.0f * k), col, 1.5f * k);
}

void drawRecordGlyph(ImDrawList* draw, ImVec2 c, float k, ImU32 col) {
    draw->AddCircleFilled(ImVec2(c.x, c.y), 5.5f * k, col, 24);
}

enum class TransportStyle { Lime, Dark };

bool transportButton(ImDrawList* draw, const char* id, ImVec2 pos, ImVec2 size,
                     const char* label, const char* kbd, bool enabled,
                     TransportStyle style, bool active, int symbol,
                     float k) {
    // BUG-diagnostics (14.11, "PLAY не нажимается"): on a click over the
    // button rect, log whether the press was accepted or swallowed by the
    // disabled gate, so the log names the blocker.
    if (ImGui::IsMouseHoveringRect(pos, add(pos, size)) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        skkbot2::log::info("[GUI] transport '{}' {} (disabled={})",
                           id, enabled ? "press accepted" : "PRESS BLOCKED",
                           enabled ? 0 : 1);
        skkbot2::Logger::get().flush();
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();

    const float hov = anim01(ImGui::GetID(id) ^ 0x5452,
                             (enabled && hovered) ? 1.0f : 0.0f, 14.0f);
    ImU32 bg, fg, borderC, kbdB, kbdT;
    if (style == TransportStyle::Lime) {
        if (active) {
            bg = theme::Record; fg = theme::RecordText; borderC = theme::Record;
        } else {
            bg = mixColor(theme::Accent, theme::AccentLight, hov);
            fg = theme::OnAccent; borderC = bg;
        }
        if (!enabled) { bg = theme::rgb(44, 54, 74, 115); fg = theme::Muted; borderC = theme::rgb(122, 138, 165, 70); }
        kbdB = theme::rgb(31, 42, 66, 56); kbdT = theme::rgb(51, 68, 94);
    } else {
        if (active) {
            bg = withAlpha(theme::Accent, 0.20f); fg = theme::Accent;
            borderC = theme::rgbText(178, 199, 235);
        } else {
            // "Available" reads as a blue-tinted dark fill + blue border, so
            // an enabled PLAY is unmistakably clickable next to a flat
            // disabled one; hover lifts fill/border/text a smooth step.
            bg = mixColor(theme::rgb(42, 56, 82, 205), theme::rgb(60, 78, 108, 220), hov);
            fg = mixColor(theme::rgbText(215, 224, 240), theme::rgbText(231, 238, 248), hov);
            borderC = mixColor(theme::rgb(96, 128, 190), theme::rgb(146, 178, 240), hov);
        }
        if (!enabled) {
            bg = theme::rgb(44, 54, 74, 115); fg = theme::Muted;
            borderC = theme::rgb(122, 138, 165, 70);
        }
        kbdB = theme::rgb(74, 96, 140); kbdT = theme::rgbText(170, 185, 210);
    }

    const float effR = std::min(28.0f * k, std::min(size.x, size.y) * 0.5f);
    // Hover glow: a SOFT halo behind the button
    if (hov > 0.004f) {
        drawSoftHalo(draw, pos, add(pos, size), effR, theme::Accent, hov * 0.8f, k);
    }
    // While the button is ACTIVE (recording/playing) it slowly breathes: a
    // pulsing soft halo so the live state reads as alive — subtle, not strong.
    if (active && enabled) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.2f);
        drawSoftHalo(draw, pos, add(pos, size), effR, theme::Accent,
                     (0.16f + 0.14f * pulse) / 0.32f, k);
    }

    draw->AddRectFilled(pos, add(pos, size), bg, effR);
    // No specular glint along the rim - see fileButton(). The one-pixel white
    // line inset from the top edge read as a stray rule, not as a highlight.
    draw->AddRect(pos, add(pos, size), borderC, effR, 0, 1.0f);

    const float cy = pos.y + size.y * 0.5f;
    // Left content group: symbol + 12px gap + letter-spaced label.
    const ImVec2 iconC(pos.x + 29.0f * k, cy);
    if (symbol == 0) drawRecordGlyph(draw, iconC, k, active ? theme::rgb(70, 39, 25, 120) : fg);
    else if (symbol == 1) drawStopGlyph(draw, iconC, k, fg);
    else drawPlayGlyph(draw, iconC, k, fg);

    ImFont* mf = mono();
    const float kt = uiTextScale();
    const float lSize = pxSize(11.0f * kt);
    const float lSpacing = 1.25f * kt;
    const float labelX = pos.x + 48.0f * k;
    const float lh = mf ? mf->CalcTextSizeA(lSize, FLT_MAX, 0, label).y
                        : ImGui::GetFontSize();
    spacedText(draw, mf, lSize, ImVec2(labelX, cy - lh * 0.5f), label, fg, lSpacing);

// Right kbd chip (justify-content: space-between, 22px padding).
      // Hidden entirely when the action has no key bound — Record/Play ship
      // unbound, and a chip reading "R" while R is Reset Level is worse than no
      // chip at all.
      if (kbd && kbd[0]) {
          const float kbdW = 24.0f * k;
          const float kbdH = 25.0f * k;
          const float kx = pos.x + size.x - 22.0f * k - kbdW;
          draw->AddRect(ImVec2(kx, cy - kbdH * 0.5f), ImVec2(kx + kbdW, cy + kbdH * 0.5f),
                        enabled ? kbdB : withAlpha(kbdB, 0.4f), 28.0f * k);
          ImVec2 ks = mf ? mf->CalcTextSizeA(pxSize(10.0f * kt), FLT_MAX, 0, kbd)
                         : ImGui::CalcTextSize(kbd);
          draw->AddText(mf, pxSize(10.0f * kt), ImVec2(kx + (kbdW - ks.x) * 0.5f, cy - ks.y * 0.5f),
                        enabled ? kbdT : withAlpha(kbdT, 0.4f), kbd);
      }

    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}

bool fileButton(ImDrawList* draw, const char* id, ImVec2 pos, ImVec2 size,
                const char* label, int icon, bool enabled, float k) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();

    const float hov = anim01(ImGui::GetID(id) ^ 0x4642,
                             (enabled && hovered) ? 1.0f : 0.0f, 14.0f);
    // Enabled buttons stay QUIET (neutral fill + thin outline) so they don't
    // shout next to RECORD/PLAY; the accent only appears on hover. Disabled is
    // flat gray with a dimmer outline. Hover lifts both smoothly.
    draw->AddRectFilled(pos, add(pos, size),
                        !enabled ? theme::rgb(44, 54, 74, 115)
                                 : mixColor(theme::rgb(42, 56, 82, 210), theme::rgb(62, 80, 110, 225), hov),
                        28.0f * k);
    // No specular glint along the rim. It was a one-pixel white line inset 1*k
    // from the top edge, drawn straight while the border around it is rounded -
    // and it read as a stray rule hanging off the button rather than as a
    // highlight. It was on every glass widget, which is why it turned up under
    // RECORD, PLAY, SAVE, CLEAR and the search field alike.
    draw->AddRect(pos, add(pos, size),
                  !enabled ? theme::rgb(122, 138, 165, 70)
                           : mixColor(theme::rgb(96, 128, 190), theme::rgb(146, 178, 240), hov),
                  28.0f * k, 0, 1.0f);

    const float cy = pos.y + size.y * 0.5f;
    const ImU32 col = !enabled ? theme::Muted
                               : mixColor(theme::rgb(185, 195, 215),
                                          theme::rgbText(226, 232, 244), hov);
    ImFont* mf = mono();
    const float kt = uiTextScale();
    const float lSize = pxSize(10.0f * kt);
    const float spacing = 0.8f * kt;
    const float lw = spacedWidth(mf, lSize, label, spacing);
    const float iconW = 12.0f * k;
    const float groupW = iconW + 10.0f * k + lw;
    float x = pos.x + (size.x - groupW) * 0.5f;
    if (icon == 0) drawSaveGlyph(draw, ImVec2(x + iconW * 0.5f, cy), k, col);
    else if (icon == 2) drawClearGlyph(draw, ImVec2(x + iconW * 0.5f, cy), k, col);
    else drawFolderGlyph(draw, ImVec2(x + iconW * 0.5f, cy), k, col);
    const float lh = mf ? mf->CalcTextSizeA(lSize, FLT_MAX, 0, label).y
                        : ImGui::GetFontSize();
    spacedText(draw, mf, lSize, ImVec2(x + iconW + 10.0f * k, cy - lh * 0.5f),
               label, col, spacing);

    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}

// One compact timing-control row (TPS / Speed): the title on the left, and on
// the right FIRST the value square (type the value, or step it with the mouse
// wheel anywhere over the row) and THEN the enable toggle. No range track — the
// value is entered directly, so the block is a single short row. Returns the
// row height so callers can stack rows.
// `Real` is float for Speed and double for TPS / FPS: the value has to reach
// the engine with the digits the user typed, and a float intermediate would
// round 144.144 to 144.143997 before it ever became a tick rate.
// Row highlight strip (user model): INVISIBLE at rest. It appears when the
// cursor is over the row OR the row's toggle is active, and STAYS while the
// toggle is active. Strips are equal 50k tiles (40k row + half of the 10k gap
// on each side); a strip rounds only the edges that face the stack edge or an
// absent neighbour, so present neighbours merge into one continuous shape.
// Two channels (user rule 06.11): HOVER is brighter (fill 0.95x / rim 1x) and
// instant; the ACTIVE toggle pin is DIMMER (fill 0.55x / rim 0.6x) and eases.
// Overdraw safety: a row's fill is drawn ONCE with the stronger channel, the
// run rim is a single rect at the strongest effective alpha — nothing stacks.
constexpr float kHoverFill = 0.95f;
constexpr float kHoverRim = 1.0f;
// Active channel sits close to hover (user tweak 07.11) but stays dimmer.
constexpr float kActiveFill = 0.75f;
constexpr float kActiveRim = 0.80f;

// Returns the hover channel; `activeOut` receives the eased pin channel.
// `easeHover` eases the hover channel too (hitbox row, user request: its
// highlight should open smoothly instead of snapping).
// `easeSpeed` slows the APPEAR side (user 07.11: the hitbox pin still read
// as a snap — the default anim01 ease is too quick); 0 keeps the default.
//
// Disappear is NOT slowed (user replay 07.11: even at the default speed the
// fade-out still read slower than every other row, whose highlight simply
// ceases the frame the cursor leaves). Only the appear side keeps the slow
// ramp; the disappear side matches the other rows exactly - instant for
// hover, and the row-default eased speed for the pinned channel's own
// fade. The snap uses a huge speed, not hard assignment, so the stored
// tween state actually lands on the final value.
float rowChannels(const char* id, bool active,
                  float minX, float maxX, float rowTop, float k,
                  float& activeOut, bool easeHover = false,
                  float easeSpeed = 0.0f) {
    const float padSide = 7.0f * k;
    const ImVec2 hpMin(minX - padSide, rowTop - 5.0f * k);
    const ImVec2 hpMax(maxX + padSide, rowTop + 45.0f * k);
    activeOut = 0.0f;
    char activeId[64];
    std::snprintf(activeId, sizeof(activeId), "%s_on", id);
    const float sp = easeSpeed > 0.0f ? easeSpeed : 12.0f;
    // Pin channel: slow ramp in, row-default ramp out.
    activeOut = anim01(ImGui::GetID(activeId), active ? 1.0f : 0.0f,
                       active ? sp : 12.0f);
    const bool over = ImGui::IsMouseHoveringRect(hpMin, hpMax);
    if (easeHover) {
        // Hover channel: slow ramp in, instant out (the speed the other
        // rows' hover channels set their vanish at).
        return anim01(ImGui::GetID(id), over ? 1.0f : 0.0f,
                      over ? sp : 1000.0f);
    }
    return over ? 1.0f : 0.0f;
}

float rowPresence(const char* id, bool active,
                  float minX, float maxX, float rowTop, float k) {
    float a = 0.0f;
    const float h = rowChannels(id, active, minX, maxX, rowTop, k, a);
    return h > a ? h : a;
}

// Per-row channel brightness helpers shared by the strip renderers. The base
// token follows the shell mode (user 07.11): Solid keeps the azure Secondary
// pair on the dark tint; the LIGHT-glass shell draws the grey pair so the
// highlight does not glare on the bright backdrop.
ImU32 stripBaseFill() {
    return theme::solidBackground ? theme::Secondary : theme::SecondaryGrey;
}
ImU32 stripBaseRim() {
    return theme::solidBackground ? theme::SecondaryBorder
                                  : theme::SecondaryGreyBorder;
}
ImU32 stripFill(int channelHover, float t) {
    return withAlpha(stripBaseFill(),
                     (channelHover ? kHoverFill : kActiveFill) * t);
}
ImU32 stripRim(int channelHover, float t) {
    return withAlpha(stripBaseRim(),
                     (channelHover ? kHoverRim : kActiveRim) * t);
}

// Underlay (user 07.11): on the LIGHT-glass shell the grey strip still read
// too washed-out against the bright blur, so the highlight carries the SAME
// dark tint slab the shell paints when Solid Background is on
// (rgb(12,16,26,185)) UNDER the strip. The slab's alpha follows the lit
// strength, so it appears only while something is lit; on Solid mode the
// shell is already dark and the tint is skipped.
// User 10.11 (lighter highlight, iteration 2): the fill is now a near-white
// slab, so the dark underlay is dialed way down - otherwise it dims the
// light fill from underneath.
ImU32 stripUnderlay(float strength) {
    if (theme::solidBackground) return 0;
    return withAlpha(theme::rgb(12, 16, 26, 70), strength);
}

void drawRowStrip(ImDrawList* draw, const char* id, bool active,
                  float minX, float maxX, float rowTop, float k,
                  bool roundTop, bool roundBottom) {
    float activeT = 0.0f;
    const float hoverT = rowChannels(id, active, minX, maxX, rowTop, k, activeT);
    if (hoverT <= 0.001f && activeT <= 0.001f) return;
    const bool hover = hoverT >= activeT;
    const float t = hover ? hoverT : activeT;
    const float padSide = 7.0f * k;
    const ImVec2 hpMin(minX - padSide, rowTop - 5.0f * k);
    const ImVec2 hpMax(maxX + padSide, rowTop + 45.0f * k);
    ImDrawFlags flags = 0;
    if (roundTop) flags |= ImDrawFlags_RoundCornersTop;
    if (roundBottom) flags |= ImDrawFlags_RoundCornersBottom;
    const float capR = flags ? 21.0f * k : 0.0f;
    const ImU32 under = stripUnderlay(t);
    if (under) draw->AddRectFilled(hpMin, hpMax, under, capR, flags);
    draw->AddRectFilled(hpMin, hpMax, stripFill(hover, t), capR, flags);
    // Rim only on Solid (user 10.11): on the light-glass shell the rows are
    // NOT outlined at all - the grey border read as a hard box line on the
    // bright blur. Glass rows = underlay + fill, no border.
    if (theme::solidBackground)
        draw->AddRect(hpMin, hpMax, stripRim(hover, t), capR, flags, 1.0f);
}

// One MERGED run of adjacent 50k strips (user fix): per-row fills keep their
// own channel strength, but the rim is drawn ONCE around the whole run — a
// shared square seam between two lit rows must not grow a border line down
// the middle. Rounding is caller-decided: roundTopFirst / roundBottomLast
// round the run's outer ends; inner seams are always square.
void drawStripRun(ImDrawList* draw, float minX, float maxX, float k,
                  const float* tops, const float* hover, const float* active,
                  int count, bool roundTopFirst, bool roundBottomLast) {
    float maxRim = 0.0f;
    for (int i = 0; i < count; ++i) {
        // Winning channel per row: when both fire, the brighter one wins —
        // the fill is drawn once, so nothing double-blends.
        const float t = hover[i] >= active[i] ? hover[i] : active[i];
        if (t <= 0.001f) continue;
        const float rimAlpha =
            (hover[i] >= active[i] ? kHoverRim : kActiveRim) * t;
        maxRim = rimAlpha > maxRim ? rimAlpha : maxRim;
    }
    if (maxRim <= 0.001f) return;
    const float padSide = 7.0f * k;
    ImDrawFlags rimFlags = 0;
    if (roundTopFirst) rimFlags |= ImDrawFlags_RoundCornersTop;
    if (roundBottomLast) rimFlags |= ImDrawFlags_RoundCornersBottom;
    const float unionCapR = rimFlags ? 21.0f * k : 0.0f;
    // Dark tint slab under the whole run on the LIGHT-glass shell (user
    // 07.11): one rect before any fill, so the per-row fills sit on the same
    // dark ground the Solid-mode shell provides.
    const ImU32 under = stripUnderlay(maxRim);
    if (under) {
        draw->AddRectFilled(ImVec2(minX - padSide, tops[0] - 5.0f * k),
                            ImVec2(maxX + padSide, tops[count - 1] + 45.0f * k),
                            under, unionCapR, rimFlags);
    }
    for (int i = 0; i < count; ++i) {
        const bool hov = hover[i] >= active[i];
        const float t = hov ? hover[i] : active[i];
        if (t <= 0.001f) continue;
        ImDrawFlags flags = 0;
        if (i == 0 && roundTopFirst) flags |= ImDrawFlags_RoundCornersTop;
        if (i == count - 1 && roundBottomLast)
            flags |= ImDrawFlags_RoundCornersBottom;
        const float capR = flags ? 21.0f * k : 0.0f;
        draw->AddRectFilled(ImVec2(minX - padSide, tops[i] - 5.0f * k),
                            ImVec2(maxX + padSide, tops[i] + 45.0f * k),
                            stripFill(hov, t), capR, flags);
    }
    // Rim only on Solid (user 10.11) - see drawRowStrip; glass runs are
    // borderless (underlay + fills only).
    if (!theme::solidBackground) return;
    draw->AddRect(ImVec2(minX - padSide, tops[0] - 5.0f * k),
                  ImVec2(maxX + padSide, tops[count - 1] + 45.0f * k),
                  withAlpha(stripBaseRim(), maxRim),
                  unionCapR, rimFlags, 1.0f);
}

template <typename Real>
float timingControl(ImDrawList* draw, ImFont* mf, ImVec2 pos, float width,
                    const char* uid, const char* title,
                    Real& value, Real vmin, Real vmax, const char* fmt,
                    const char* suffix, bool& enabled,
                    float k, Real wheelStep = Real(0), bool allowToggle = true,
                    const char* titleSuffix = nullptr, bool revealed = true,
                    Real fineWheelStep = Real(0)) {
    static_assert(std::is_floating_point_v<Real>,
                  "timingControl only handles floating-point values");
    // Unique ID scope so two stacked rows (TPS + Speed) don't collide.
    ImGui::PushID(uid);
    (void)mf;  // title/values render in the default font; mono goes to callers
    // The macro dropdown paints an opaque panel over the whole Record page,
    // so while it is open the per-row inputs must not take clicks.
    const bool blocked = state().searchVisible;
    // `revealed` is the accordion gate: while the owning section is still
    // easing open (or is collapsed) the row is visible but must not register
    // clicks — a clipped-out control would otherwise steal them from whatever
    // sits underneath. Defaults to true so TPS / FPS call sites are unchanged.
    const bool canToggle = allowToggle && !blocked && revealed;
    const bool canInput = enabled && !blocked && revealed;
    const float rowH = 40.0f * k;
    ImFont* df = ImGui::GetFont();
    const float kt = uiTextScale();
    ImVec2 ts = df->CalcTextSizeA(pxSize(14.0f * kt), FLT_MAX, 0, title);
    const ImVec2 tPos(pos.x, pos.y + (rowH - ts.y) * 0.5f);
    const ImU32 c0 = enabled ? theme::rgbText(222, 230, 245) : theme::rgbText(179, 186, 204);
    const ImU32 c1 = enabled ? theme::rgbText(240, 245, 255) : theme::rgbText(179, 186, 204);
    drawGradientText(draw, df, pxSize(14.0f * kt), tPos, title, c0, c1);
    // A dimmed note after the title, for the row's dependent figure. It goes
    // HERE rather than in the value square because the square is 100px wide and
    // already holds the number plus its unit; putting "4 st/f" in there put it
    // in the field the user types into, which made both look cramped and made
    // it ambiguous whether the text was part of the editable value.
    if (titleSuffix && titleSuffix[0] != '\0') {
        spacedText(draw, df, pxSize(13.0f * kt),
                   ImVec2(tPos.x + ts.x + 8.0f * k, tPos.y),
                   titleSuffix, theme::rgb(150, 158, 176), 1.0f);
    }

    // Value square first, then the enable toggle — both vertically centered in
    // the row.
    //
    // The mouse wheel belongs to the COLUMN, not to this row. This used to step
    // the value whenever the pointer was anywhere over the row's rectangle —
    // including the caption text — and it zeroed the wheel, so hovering "TPS"
    // both scrubbed the rate by one and stopped the RECORD column from
    // scrolling: the page looked frozen under the labels. Only the value box
    // steps now, and numberField does that itself over its own box (consuming
    // the event there, so the page cannot scroll under a value that just
    // changed). Everywhere else in the row the wheel reaches the column scroll.
    const float nfW = 100.0f * k;
    const float nfH = 34.0f * k;
    const float tgGap = 17.0f * k;
    const float tgW = 44.0f * k;
    const float nfX = pos.x + width - nfW - tgGap - tgW;
    const float tgX = pos.x + width - tgW;

    Real v1 = value;
    const bool nfChanged = numberField(mf, "##nfield",
                                       ImVec2(nfX, pos.y + (rowH - nfH) * 0.5f),
                                       nfW, v1, vmin, vmax, suffix, fmt, canInput,
                                       wheelStep, fineWheelStep);
    const bool tgChanged = toggleAt("##ntoggle", ImVec2(tgX, pos.y + (rowH - 24.0f * k) * 0.5f),
                                    enabled, canToggle, canToggle);
    if (tgChanged && canToggle) enabled = !enabled;
    if (nfChanged && canInput) value = v1;

    ImGui::PopID();
    return rowH;
}

// The two RECORD column scrollbars (left column from the timing divider down,
// and the right NOCLIP column). Defined next to applyRecordScroll far below, so
// forward-declared here for the same reason.
void applyTimingScroll(ImDrawList* draw, ImVec2 winMin, ImVec2 winMax,
                       float viewTop, float viewBot, float contentH, float k);
void applyNoClipScroll(ImDrawList* draw, ImVec2 winMin, ImVec2 winMax,
                       float viewTop, float viewBot, float contentH, float k);

// Right column of the Record tab: a compact NOCLIP accordion (a scaled-down
// clone of the page headings) that drives the engine's collision-bypass
// settings — master switch, P1/P2 targets, death flash. The values live on
// ReplayEngine and are persisted via the same gui_noclip* keys the classic
// GUI uses, so both shells stay in sync.
// Hands the stepper a live "left button is physically down" probe. Registered
// once; the stepper's scheduler consults it every tick so a mouse-hold release
// is seen on the SAME tick that would otherwise have emitted a trailing step.
// GetAsyncKeyState is used instead of ImGui's IO because ImGui's mouse state is
// only refreshed during its own NewFrame, whose ordering relative to the game's
// scheduler we do not control.
//
// Either mouse button drives the STEP button, so the probe accepts both. It only
// answers "is a button physically down": the core still requires m_holdActive,
// which the GUI sets solely for the STEP button, so a right click elsewhere in
// the game cannot start hold-stepping.
static void registerStepperHoldProbe() {
    static bool installed = false;
    if (installed) return;
    installed = true;
#ifdef GEODE_IS_WINDOWS
    FrameStepper::get().setHoldProbe([] {
        return (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
               (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    });
#else
    FrameStepper::get().setHoldProbe([] {
        return ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
               ImGui::IsMouseDown(ImGuiMouseButton_Right);
    });
#endif
}

// Live "a mouse button is physically down" reading, matching the probe above.
// Exposed separately so the GUI can OR the right button into its own held state.
static bool stepMouseButtonDown() {
#ifdef GEODE_IS_WINDOWS
    return (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
           (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
#else
    return ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
           ImGui::IsMouseDown(ImGuiMouseButton_Right);
#endif
}

// Status capsule drawn next to a feature title, e.g. "BETA" / "MENU".
//
// Measures the glyph INK rather than the advance width: with a proportional or
// letter-spaced font the advance box includes side bearings that read as a
// lopsided capsule, while the ink box keeps the text optically centred.
// `left` is the badge's LEFT edge (anchoring by the left edge, not by a centre,
// is what keeps the gap from collapsing onto the title: a centred badge eats
// half its own width out of the gap) and `midY` is the vertical centre.
void statusBadge(ImDrawList* draw, ImFont* font, float fontSize, float k,
                 float left, float midY, const char* label) {
    ImFont* bf = font ? font : ImGui::GetFont();
    const ImVec2 textSz = bf ? bf->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label)
                             : ImVec2(24.0f * k, fontSize);
    InkMeasure bm;
    const bool haveInk = measureGlyphInk(bf, fontSize, label, bm);
    const float inkW = haveInk ? (bm.x1 - bm.x0) : textSz.x;
    const float inkH = haveInk ? (bm.y1 - bm.y0) : (fontSize * 0.70f);

    // Generous proportional padding so the text sits cleanly inside the capsule.
    const float padY = 4.0f * k;
    const float badgeH = std::round(inkH + padY * 2.0f);
    const float padX = std::round(badgeH * 0.5f + 3.0f * k);
    const float badgeW = std::round(inkW + padX * 2.0f);

    const ImVec2 bMin(left, midY - badgeH * 0.5f);
    const ImVec2 bMax(bMin.x + badgeW, bMin.y + badgeH);
    const float radius = badgeH * 0.5f;

    draw->AddRectFilled(bMin, bMax, theme::rgb(55, 95, 175, 90), radius);
    draw->AddRect(bMin, bMax, theme::rgb(110, 165, 255, 180), radius, 0, 1.0f);

    float tx, ty;
    if (haveInk) {
        tx = bMin.x + badgeW * 0.5f - (bm.x0 + bm.x1) * 0.5f;
        ty = midY - (bm.y0 + bm.y1) * 0.5f;
    } else {
        tx = bMin.x + badgeW * 0.5f - textSz.x * 0.5f;
        ty = midY - 0.43f * fontSize;
    }
    draw->AddText(bf, fontSize, ImVec2(std::round(tx), std::round(ty)),
                  theme::rgb(170, 215, 255), label);
}

// Gap between a title and the badge that follows it. The badge is anchored by
// its LEFT edge at titleEnd + this, so the visible gap is exactly this and does
// not shrink when the label is wider.
constexpr float kBadgeGap = 14.0f;

void drawNoClipSection(ImDrawList* draw, ImFont* mf, ImVec2 from, ImVec2 to, float k) {
    auto* engine = ReplayEngine::get();
    const float kt = uiTextScale();

    // Border between the columns.
    draw->AddLine(ImVec2(from.x - 22.0f * k, from.y), ImVec2(from.x - 22.0f * k, to.y),
                  theme::Line, 1.0f);

    const float width = to.x - from.x;
    float y = from.y;

    // The whole right column (SPEED + NOCLIP + FRAME STEPPER) scrolls as one
    // run: with every row open it outgrows the body and the lower controls
    // would be unreachable. Drawn shifted up by the persistent offset and
    // clipped to the viewport; the scrollbar reuses applyNoClipScroll. The
    // column border above is drawn BEFORE the shift so it keeps spanning the
    // full viewport, not the content. Every section is a mini accordion with
    // the same metrics: heading -> first row 27k, last row -> next heading
    // kSectionGap (10k), and a collapsed section keeps only the 27k slot.
    const float ncTop = from.y;
    // Furthest drawn bottom of the column. Each section advances its own
    // cursor, so the content height has to be the max of all three - using one
    // `y` would under-report it whenever FRAME STEPPER is the taller of the
    // pair and the scrollbar would clip short of the real bottom.
    float ncContentBot = ncTop;
    State& st = state();
    const bool listOpen = state().searchVisible;
    y -= state().noclipScroll;
    draw->PushClipRect(ImVec2(from.x - 12.0f * k, ncTop),
                       ImVec2(to.x + 12.0f * k, to.y), true);

    // -----------------------------------------------------------------------
    // SPEED - mini accordion, same heading/metrics as NOCLIP and FRAME STEPPER.
    // Moved here from the left timing column: it sits ABOVE NOCLIP as its own
    // collapsible section. The heading owns the caps ("SPEED"); the control
    // row keeps the ordinary caption "Speed".
    //
    // Speed is NOT frozen during playback, unlike TPS. TPS decides the size of
    // one physics step, so changing it would slide every click off its frame.
    // Speed only scales the wall-clock delta handed to the scheduler
    // (speedDelta = rawDt * gameSpeed) while the step itself stays
    // 1/runtimeTickRate, and macro frames are counted in physics steps rather
    // than seconds. Turning the speedhack on halfway through a run, or dragging
    // it, therefore changes how fast the level is consumed and nothing about
    // the physics each frame sees - which is exactly what a speedhack is for.
    auto& gui = skkbot2::gui::get();
    if (sectionHeading(draw, from, width, y, "SPEED", st.speedOpen, nullptr, k,
                       0.8f, "speed_section")) {
        if (!listOpen) st.speedOpen = !st.speedOpen;
    }
    y += 27.0f * k;  // heading -> first row: the same slot NOCLIP uses
    const float speedContentY = y;

    const float speedRowH = 40.0f * k;
    const float syncGap = 10.0f * k;
    const float speedOpenT = anim01(ImGui::GetID("##speed_accordion"),
                                    st.speedOpen ? 1.0f : 0.0f, 11.0f);
    const float syncT = anim01(ImGui::GetID("##ms_rows"),
                               gui.speedhackEnabled ? 1.0f : 0.0f, 11.0f);
    // The block is the Speed row, plus the Sync Music row while the speedhack
    // is engaged (its 10k gap is part of the eased height, so a collapsed row
    // leaves no dangling space).
    const float speedSeen = speedOpenT * (speedRowH + syncT * (syncGap + speedRowH));
    if (speedOpenT > 0.001f && speedSeen > 0.001f) {
        draw->PushClipRect(ImVec2(from.x - 12.0f * k, speedContentY - 12.0f * k),
                           // +6k below the content end: the Speed row's
                           // highlight band extends 5k past the row (half-gap
                           // tiling) — a clip exactly at the content end used
                           // to slice its bottom border off.
                           ImVec2(to.x + 16.0f * k,
                                  speedContentY + speedSeen + 6.0f * k), true);
        const bool speedRowL = speedSeen >= speedRowH * 0.85f;
        const bool syncRowL = speedSeen >= (speedRowH + syncGap + speedRowH) * 0.85f;
        const bool ctrl = listOpen ? false : true;

        // Row 1 - Speed (the accordion's first control).
        const double prevSpeedV = gui.speedhackValue;
        const bool prevSpeedOn = gui.speedhackEnabled;
        float speedV = static_cast<float>(gui.speedhackValue);
        bool speedOn = gui.speedhackEnabled;
        drawRowStrip(draw, "##speed_hl", speedOn, from.x, from.x + width,
                     speedContentY, k, /*roundTop=*/true, /*roundBottom=*/true);
        timingControl(draw, mf, ImVec2(from.x, speedContentY), width,
                      "speed", "Speed",
                      speedV, 0.01f, 100.0f, "%.2f", nullptr,
                      speedOn, k, 0.01f, /*allowToggle=*/true,
                      /*titleSuffix=*/nullptr, /*revealed=*/speedRowL);
        gui.speedhackValue = static_cast<double>(speedV);
        engine->gameSpeed = speedOn ? gui.speedhackValue : 1.0;
        gui.speedhackEnabled = speedOn;
        if (gui.speedhackValue != prevSpeedV || gui.speedhackEnabled != prevSpeedOn) {
            Mod::get()->setSavedValue("gui_speedhack_enabled", gui.speedhackEnabled);
            Mod::get()->setSavedValue("gui_speedhack_value", gui.speedhackValue);
        }

        // Row 2 - Sync Music. Only while the speedhack is engaged, directly
        // under the Speed row so the two read as one control: it is the same
        // value applied to the FMOD master channel group's pitch instead of the
        // scheduler dt, which is how both reference bots do it (GDH "Speedhack
        // Audio", EclipseMenu "Audio Speed"). A plain toggle, not a second
        // float: the pitch follows the Speed value above, so a separate pitch
        // slider could only ever contradict it.
        if (syncT > 0.001f) {
            ImFont* mdf = ImGui::GetFont();
            const float msSize = pxSize(14.0f * kt);
            const float msTextH = mdf ? mdf->CalcTextSizeA(msSize, FLT_MAX, 0.0f,
                                                           "Sync Music").y
                                      : ImGui::GetFontSize();
            const float syncY = speedContentY + speedRowH + syncGap * syncT;
            const bool syncOn = speedOn && gui.musicSpeedSync;
            const ImVec2 msTPos(from.x, syncY + (speedRowH - msTextH) * 0.5f);
            drawGradientText(draw, mdf, msSize, msTPos, "Sync Music",
                             syncOn ? theme::rgbText(222, 230, 245)
                                    : theme::rgbText(179, 186, 204),
                             syncOn ? theme::rgbText(240, 245, 255)
                                    : theme::rgbText(179, 186, 204));
            const ImVec2 msTog(from.x + width - 44.0f * k,
                               syncY + (speedRowH - 24.0f * k) * 0.5f);
            if (toggleAt("##beta_music_speed", msTog, syncOn, ctrl && syncRowL)) {
                gui.musicSpeedSync = !gui.musicSpeedSync;
                Mod::get()->setSavedValue("gui_music_speed_sync", gui.musicSpeedSync);
                // Nudge the audio hook immediately instead of waiting for the
                // next speed change: switching the toggle off must restore 1.0x
                // pitch on the very next audio update.
                skk::music_speed::forceReapply();
                skkbot2::Logger::get().info("skkBot2: music speed sync -> {}",
                                            gui.musicSpeedSync ? "on" : "off");
            }
        }

        draw->PopClipRect();
    }
    // Content bottom of the section, then the gap to the next accordion
    // heading. `speedOpenT` keeps the gap at 0 while collapsed, so a closed
    // section is just heading + 27k - exactly like NOCLIP -> FRAME STEPPER.
    y = speedContentY + speedSeen;
    y += speedOpenT * kSectionGap * k;

    // -----------------------------------------------------------------------
    // NOCLIP
    // -----------------------------------------------------------------------
    if (sectionHeading(draw, from, width, y, "NOCLIP", st.noclipOpen, nullptr, k,
                       0.8f, "noclip")) {
        if (!listOpen) st.noclipOpen = !st.noclipOpen;
    }
    y += 27.0f * k;

    const bool noclipOn = engine->collisionBypass;
    ImFont* df = ImGui::GetFont();
    // Row captions use the TPS/Speed caption style: default (non-mono) font at
    // the SAME 14px size as the TPS/Speed labels, same on/off colors, flush
    // with the column edge (no inset — TPS/Speed titles start at pos.x).
    const float titleSize = pxSize(14.0f * kt);
    const ImU32 capOn = theme::rgbText(222, 228, 242);
    const ImU32 capOff = theme::rgbText(179, 186, 204);
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    // Controls mirror the TPS/Speed shapes exactly: the P1/P2 pills use the
    // same box as the timing number field (100x34k), the toggles match the TPS
    // toggle (44x24k), and the rows are the same 40k height.
    const float pillW = 100.0f * k;
    const float pillH = 34.0f * k;
    // Control rows mirror the TPS/Speed topline: title text on the left and
    // the control on the right, both optically centered in one 40k row — so
    // the toggle sits on the SAME vertical level as its caption.
    const float rowH = 40.0f * k;
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "NOCLIP").y
                           : ImGui::GetFontSize();

    // Accordion: the rows grow/shrink smoothly instead of popping. The
    // content is drawn inside a clip that follows the eased open height, and
    // each row only registers its interactive control once it is (almost)
    // fully revealed — a still-hidden control must not steal clicks.
    //
    // Two nested reveals, both eased exactly like the block itself (rate 11):
    //  - openT  — the whole NOCLIP block opens/closes from the mini heading.
    //  - rowsT  — rows 2-3 (Players + Death Flash) appear ONLY while the master
    //    "Noclip" switch is on. With the master off they collapse away, so the
    //    disabled state shows just the single master row.
    const float baseH = rowH;                        // row 1: master switch
    const float extraH = rowH * 2.0f + 20.0f * k;    // 10k + rows 2-3 + 10k
    const float slotH = baseH + extraH;
    const float openT = anim01(ImGui::GetID("##nc_accordion"),
                               st.noclipOpen ? 1.0f : 0.0f, 11.0f);
    const float rowsT = anim01(ImGui::GetID("##nc_rows"),
                               noclipOn ? 1.0f : 0.0f, 11.0f);
    const float seen = openT * (baseH + rowsT * extraH);
    const float noclipContentY = y;
    if (openT > 0.001f && seen > 0.001f) {
        draw->PushClipRect(ImVec2(from.x - 12.0f * k, y - 12.0f * k),
                           // +6k below the content end: the master row's
                           // highlight band extends 5k past the row (half-gap
                           // tiling) — a clip exactly at the content end used
                           // to slice its bottom border off.
                           ImVec2(to.x + 16.0f * k, y + seen + 6.0f * k), true);
        const float rowTop2 = rowH + 10.0f * k;
        const float rowTop3 = rowH * 2.0f + 20.0f * k;
        const bool row1L = seen >= rowH * 0.85f;
        const bool row2L = seen >= (rowTop2 + rowH) * 0.85f;
        const bool row3L = seen >= (rowTop3 + rowH) * 0.85f;
        // The list is window-modal: its rows live in the left column, so the right
        // column would stay clickable unless we gate every control on it.
        const bool ctrl = listOpen ? false : true;

        // Row 1 — master switch.
        {
            drawRowStrip(draw, "##nc_hl", noclipOn, from.x, from.x + width,
                         y, k, /*roundTop=*/true, /*roundBottom=*/true);
            const ImVec2 tPos(from.x, y + (rowH - textH) * 0.5f);
            // Both states render through the SAME drawGradientText path: the
            // plain AddText branch lands glyphs at the fractional tPos.x while
            // drawGradientText snaps it to whole pixels, so mixing them made
            // the caption jump sideways on every toggle.
            drawGradientText(draw, df, titleSize, tPos, "Noclip",
                             noclipOn ? theme::rgbText(222, 230, 245) : capOff,
                             noclipOn ? theme::rgbText(240, 245, 255) : capOff);
            ImVec2 tog(from.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
            if (toggleAt("##nc_master", tog, noclipOn, ctrl && row1L)) {
                engine->collisionBypass = !noclipOn;
                Mod::get()->setSavedValue("gui_noclip", engine->collisionBypass);
            }
            y += rowH + 10.0f * k;
        }

        // Row 2 — player targets: two pills the exact size of the TPS number field.
        {
            const float labelSize = pxSize(9.0f * kt);
            const float labelSpacing = 1.2f * kt;
            const float btnW = pillW;  // 100k — same box as the TPS/Speed value field
            const float btnH = pillH;  // 34k — ditto
            const float gap = 10.0f * k;
            const float groupW = btnW * 2.0f + gap;
            const float gx = from.x + width - groupW;
            const float by = y + (rowH - btnH) * 0.5f;

            const ImVec2 tPos(from.x, y + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Players",
                             noclipOn ? theme::rgbText(222, 230, 245) : capOff,
                             noclipOn ? theme::rgbText(240, 245, 255) : capOff);
            if (selectionToggle("##nc_p1", ImVec2(gx, by), ImVec2(btnW, btnH), mf,
                                labelSize, labelSpacing, "PLAYER 1",
                                engine->noclipPlayer1, noclipOn, ctrl && row2L)) {
                engine->noclipPlayer1 = !engine->noclipPlayer1;
                Mod::get()->setSavedValue("gui_noclip_p1", engine->noclipPlayer1);
            }
            if (selectionToggle("##nc_p2", ImVec2(gx + btnW + gap, by), ImVec2(btnW, btnH), mf,
                                labelSize, labelSpacing, "PLAYER 2",
                                engine->noclipPlayer2, noclipOn, ctrl && row2L)) {
                engine->noclipPlayer2 = !engine->noclipPlayer2;
                Mod::get()->setSavedValue("gui_noclip_p2", engine->noclipPlayer2);
            }
            y += rowH + 10.0f * k;
        }

        // Row 3 — death flash. Hidden entirely while the master is off (rowsT
        // collapses rows 2-3 away; it only renders, like Players, while on).
        {
            const bool flashOn = noclipOn && engine->noclipDeathFlash;
            const ImVec2 tPos(from.x, y + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Death Flash",
                             flashOn ? theme::rgbText(222, 230, 245) : capOff,
                             flashOn ? theme::rgbText(240, 245, 255) : capOff);
            ImVec2 tog(from.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
            if (toggleAt("##nc_flash", tog, engine->noclipDeathFlash, noclipOn, ctrl && row3L)) {
                engine->noclipDeathFlash = !engine->noclipDeathFlash;
                Mod::get()->setSavedValue("gui_noclip_flash", engine->noclipDeathFlash);
            }
        y += rowH;
    }

        draw->PopClipRect();
    }
    ncContentBot = std::max(ncContentBot, y);

    // -----------------------------------------------------------------------
    // Mini accordion heading for FRAME STEPPER (placeholder under NOCLIP)
    // -----------------------------------------------------------------------
    // The user requested:
    // "Расстояние между фрейм степпером и деат флеш должно быть таким же как и
    //  расстояние между ноуклипом (который типа "RECORD & PLAY") и ноуклипом (который кнопка)!
    //  И в закрытом виде тоже!"
    //
    // The gap ABOVE the NOCLIP block (heading -> block top) is the 27k added by
    // `y += 27.0f * k`; the gap BELOW it uses the shared kSectionGap so the
    // whitespace from a section's last row to the next accordion heading is the
    // same here as in the left column. `openT` keeps it at 0 while the NOCLIP
    // accordion is closed, so the collapsed heading-to-heading distance is
    // unchanged.
    const float fsHeadingY = noclipContentY + seen + openT * kSectionGap * k;
    if (sectionHeading(draw, from, width, fsHeadingY, "FRAME STEPPER", st.frameStepperOpen,
                       nullptr, k, 0.8f, "frame_stepper")) {
        if (!listOpen) st.frameStepperOpen = !st.frameStepperOpen;
    }

    const float fsOpenT = anim01(ImGui::GetID("##fs_accordion"),
                                 st.frameStepperOpen ? 1.0f : 0.0f, 11.0f);
    const bool fsMasterOn = FrameStepper::get().isEnabled();
    // Two-level reveal, same shape as NOCLIP:
    //  - the master row + the Auto Pause On Death OPTION are always visible, so
    //    the option can be armed once and left alone (arming it must not require
    //    turning the stepper itself on);
    //  - the STEP row and Hold toggle appear ONLY while the master switch is on,
    //    since they are useless without the freeze.
    const float fsBaseH = rowH * 2.0f + 10.0f * k;   // master + 10k + option row
    const float fsExtraH = rowH * 2.0f + 20.0f * k;   // 10k + STEP + 10k + Hold
    const float fsRowsT = anim01(ImGui::GetID("##fs_rows"),
                                 fsMasterOn ? 1.0f : 0.0f, 11.0f);
    const float fsSeen = fsOpenT * (fsBaseH + fsRowsT * fsExtraH);
    // Content bottom of this section: heading + the 27k slot + the eased rows.
    // Declared OUTSIDE the reveal guard so it can feed the column scroll height
    // even while the section is collapsed (then it is just the heading slot).
    const float fsContentBot = fsHeadingY + 27.0f * k + fsSeen;
    if (fsOpenT > 0.001f && fsSeen > 0.001f) {
        const float fsContentY = fsHeadingY + 27.0f * k;
        // Bottom edge must land EXACTLY on the content end (fsSeen already
        // includes the inter-row gaps). An extra bottom pad here would re-expose
        // a sliver of a collapsed row — rows sit only 10k apart, so any pad
        // >= 10k leaves part of a cap visible.
        draw->PushClipRect(ImVec2(from.x - 12.0f * k, fsContentY - 12.0f * k),
                           // +6k below the content end: the FS+Auto Pause
                           // group band extends 5k past the last row (half-gap
                           // tiling) — a clip exactly at the content end used
                           // to slice its bottom border off.
                           ImVec2(to.x + 16.0f * k,
                                  fsContentY + fsSeen + 6.0f * k), true);

        const float rowTop2 = rowH + 10.0f * k;
        const float rowTop3 = rowTop2 + rowH + 10.0f * k;
        const float rowTop4 = rowTop3 + rowH + 10.0f * k;
        const bool row1L = fsSeen >= rowH * 0.85f;
        const bool row2L = fsSeen >= (rowTop2 + rowH) * 0.85f;
        const bool row3L = fsSeen >= (rowTop3 + rowH) * 0.85f;
        const bool row4L = fsSeen >= (rowTop4 + rowH) * 0.85f;
        const bool ctrl = listOpen ? false : true;

        float fy = fsContentY;

        // Row 1 — Master switch (Frame Stepper toggle)
        {
            // Pair with Auto Pause On Death: per-row strips, invisible at
            // rest, lit by hover or an active toggle. Both lit merge into one
            // run — per-row fills, a SINGLE rim around the pair, square seam
            // without a border line; a lone strip is a full capsule.
            const bool fsActive = FrameStepper::get().isEnabled();
            const bool apActive = FrameStepper::get().isAutoPauseOnDeathEnabled();
            float gFsA = 0.0f, gApA = 0.0f;
            const float fsH = rowChannels("##fs_hl", fsActive,
                                          from.x, from.x + width, fy, k,
                                          gFsA);
            const float apH = rowChannels("##ap_hl", apActive,
                                          from.x, from.x + width,
                                          fy + 50.0f * k, k, gApA);
            const bool fsLit = fsH > 0.001f || gFsA > 0.001f;
            const bool apLit = apH > 0.001f || gApA > 0.001f;
            const float hov[2] = { fsH, apH };
            const float act[2] = { gFsA, gApA };
            const float tops[2] = { fy, fy + 50.0f * k };
            if (fsLit && apLit) {
                drawStripRun(draw, from.x, from.x + width, k, tops, hov, act, 2,
                             /*roundTopFirst=*/true, /*roundBottomLast=*/true);
            } else if (fsLit) {
                drawStripRun(draw, from.x, from.x + width, k, tops, hov, act, 1,
                             /*roundTopFirst=*/true, /*roundBottomLast=*/false);
            } else if (apLit) {
                drawStripRun(draw, from.x, from.x + width, k, tops + 1,
                             hov + 1, act + 1, 1,
                             /*roundTopFirst=*/false, /*roundBottomLast=*/true);
            }
            const bool stepperOn = fsActive;
            // Mutually exclusive with playback: the stepper advances the scheduler
            // manually, playback advances it for real, so only one may own it.
            // Deliberately NOT gated on engine->pendingPlaybackStart: that flag is
            // an internal arming latch, and anything that tests it here leaves
            // the toggle stuck greyed out once a PLAY has been cancelled.
            auto* engine = ReplayEngine::get();
            auto& bot = skk::skc::Bot::get();
            const bool playbackOn = (gui.macroBotType == Gui::MacroBotType::SKC)
                ? (bot.getState() == skk::skc::State::Playback || bot.isPlaybackArmed() || bot.getPendingAction() == skk::skc::PendingAction::Play)
                : (engine && (engine->engineMode == MODE_EXECUTE ||
                              engine->pendingAction == ReplayEngine::PendingAction::Play));
            const ImVec2 tPos(from.x, fy + (rowH - textH) * 0.5f);
            const float fsTitleW = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Frame Stepper").x : 0.0f;
            drawGradientText(draw, df, titleSize, tPos, "Frame Stepper",
                             stepperOn ? theme::rgbText(222, 230, 245) : capOff,
                             stepperOn ? theme::rgbText(240, 245, 255) : capOff);
            // Same BETA capsule the other beta rows wear.
            statusBadge(draw, df, pxSize(9.5f * kt), k,
                        from.x + fsTitleW + kBadgeGap * k,
                        fy + rowH * 0.5f, "BETA");
            ImVec2 tog(from.x + width - toggleW, fy + (rowH - toggleH) * 0.5f);
            if (toggleAt("##fs_master", tog, stepperOn, ctrl && row1L && !playbackOn)) {
                FrameStepper::get().toggle();
            }
            fy += rowH + 10.0f * k;
        }

        // Row 2 — Auto Pause On Death. An OPTION rather than a step control, so it
        // lives in the always-visible band: it can be armed without turning the
        // stepper on, and stays armed for the rest of the session.
        {
            const bool apOn = FrameStepper::get().isAutoPauseOnDeathEnabled();
            const ImVec2 tPos(from.x, fy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Auto Pause On Death",
                             apOn ? theme::rgbText(222, 230, 245) : capOff,
                             apOn ? theme::rgbText(240, 245, 255) : capOff);
            ImVec2 tog(from.x + width - toggleW, fy + (rowH - toggleH) * 0.5f);
            if (toggleAt("##fs_autopause", tog, apOn, ctrl && row2L)) {
                FrameStepper::get().setAutoPauseOnDeathEnabled(!apOn);
            }
            fy += rowH + 10.0f * k;
        }

        // Rows 3-4 — step controls, revealed only while the master is on.
        {
            // The count field is ALWAYS shown: it defines how many frames one
            // press is worth, for the button as well as for the keybind, and it
            // was previously hidden whenever Hold To Step was on.
            const float gap = 8.0f * k;
            const float inputW = 68.0f * k;
            const float btnW = width - inputW - gap;
            const float btnH = pillH;
            const float by = fy + (rowH - btnH) * 0.5f;

            bool stepHeld = false;
            const bool stepClicked =
                buttonPrimary("##fs_step", ImVec2(from.x, by), ImVec2(btnW, btnH),
                              "STEP>", nullptr, ctrl && row3L, theme::Accent,
                              theme::AccentLight, &stepHeld);

            // The RIGHT mouse button drives STEP exactly like the left one.
            // ImGui only reports left-button activation, so the right press is
            // recognised here — but ONLY while the cursor is over the button, or
            // a right click anywhere else in the game would step (the same class
            // of bug as the earlier "any left click steps" regression).
            const ImVec2 stepMin(from.x, by);
            const ImVec2 stepMax(from.x + btnW, by + btnH);
            const bool cursorOverStep = ImGui::IsMouseHoveringRect(stepMin, stepMax, false);
            const bool anyMouseDown = stepMouseButtonDown();
            static bool prevStepMouseDown = false;
            const bool rightOrLeftEdge = anyMouseDown && !prevStepMouseDown;
            prevStepMouseDown = anyMouseDown;

            // fromMenuButton=true: this press obeys the Hold To Step switch below
            // (off = tap only, on = hold only), unlike the keybind.
            if (stepClicked || (rightOrLeftEdge && cursorOverStep && ctrl && row3L)) {
                FrameStepper::get().stepForward(0, /*fromMenuButton=*/true);
            }

            // The hold itself is driven by the stepper, not from here: the menu
            // is drawn AFTER the scheduler, so any repeat requested from this
            // side lands one tick after the button came up. Instead we hand the
            // core a probe that reads the OS mouse state, which it can consult in
            // the same tick it decides to step. Either button counts, and the
            // hover test keeps an unrelated click from latching the hold.
            if (stepHeld || (anyMouseDown && cursorOverStep && ctrl && row3L)) {
                FrameStepper::get().setHoldState(true, /*forward=*/true);
            } else if (!FrameStepper::get().isKeyHeld()) {
                FrameStepper::get().setHoldState(false, true);
            }
            registerStepperHoldProbe();

            float stepCountVal = static_cast<float>(FrameStepper::get().getStepCount());
            if (numberField(mf, "##fs_step_count", ImVec2(from.x + btnW + gap, by), inputW,
                            stepCountVal, 1.0f, 1000.0f, "", "%.0f", ctrl && row3L, 1.0f)) {
                FrameStepper::get().setStepCount(static_cast<int>(std::round(stepCountVal)));
            }

            fy += rowH + 10.0f * k;
        }

        // Row 4 — Hold To Step. Governs the STEP BUTTON only (the keybind always
        // supports both press and hold). ON = hold-only: a press steps nothing.
        // OFF = tap-only: a press is worth the count, holding adds nothing.
        {
            const bool holdOn = FrameStepper::get().isHoldToStepEnabled();
            const ImVec2 tPos(from.x, fy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Hold To Step",
                             holdOn ? theme::rgbText(222, 230, 245) : capOff,
                             holdOn ? theme::rgbText(240, 245, 255) : capOff);
            // MENU badge: this switch only governs the menu STEP button, the keybind
            // always supports press and hold — hence MENU, not BETA.
            {
                const float holdTextW = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Hold To Step").x
                                          : 80.0f * k;
                const float badgeFontSize = pxSize(9.5f * kt);
                statusBadge(draw, df, badgeFontSize, k,
                            from.x + holdTextW + kBadgeGap * k, fy + rowH * 0.5f, "MENU");
            }
            ImVec2 tog(from.x + width - toggleW, fy + (rowH - toggleH) * 0.5f);
            if (toggleAt("##fs_hold", tog, holdOn, ctrl && row4L)) {
                FrameStepper::get().setHoldToStepEnabled(!holdOn);
            }
            fy += rowH;
        }

        draw->PopClipRect();
    }
    ncContentBot = std::max(ncContentBot, fsContentBot);

    // Close the column clip and run its scrollbar. The content height is only
    // known now, after the eased rows settled, so the clamp runs post-draw and
    // the offset persists to the next frame.
    const float ncContentH = (ncContentBot + state().noclipScroll) - ncTop;
    draw->PopClipRect();
    applyNoClipScroll(draw, ImVec2(from.x - 12.0f * k, ncTop),
                      ImVec2(to.x + 20.0f * k, to.y), ncTop, to.y, ncContentH, k);
}

void drawRecordPage(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                    float bottom, float k) {
    auto& gui = skkbot2::gui::get();
    auto* engine = ReplayEngine::get();
    ImFont* mf = mono();
    const float kt = uiTextScale();
    const float colGap = 36.0f * k;
    const float total = width - colGap;
    const float leftW = total * 1.15f / 2.15f;
    const float rightW = total - leftW;
    const float lx = gridTL.x;
    const float rx = lx + leftW + colGap;

    float y = top;

    // Текстовая шапка («YOUR MACRO», «{n} saved») убрана по запросу
    // 03.11.2026. Поле ввода имени/поиска помещаем чуть ниже верха —
    // небольшой воздух вместо убранной шапки (запрос 03.11.2026.
    // «прям чуть ниже»); правый столбец не привязан к этому отступу.

    // Macro search box.
    const float srchH = 52.0f * k;
    y += 12.0f * k;
    ImVec2 sb(lx, y);
    ImVec2 eb(lx + leftW, y + srchH);
    draw->AddRectFilled(sb, eb, theme::FieldDark, 28.0f * k);
    draw->AddRect(sb, eb, theme::FieldBorder, 28.0f * k, 0, 1.0f);

    const float searchIconCX = sb.x + 25.0f * k;
    drawSearchGlyph(draw, ImVec2(searchIconCX, sb.y + srchH * 0.5f), k,
                    theme::rgb(136, 148, 170));
    const float searchICX = sb.x + 46.0f * k;
    // The folder button owns the right edge of the search box; the name field
    // is clamped so its hit rect never reaches the folder zone (a full-width
    // InputText used to stretch over it and steal the folder's clicks).
    const float btnSize = 32.0f * k;
    const float folderMarg = 4.0f * k;
    const float folderPosX = lx + leftW - folderMarg - btnSize;
    const float sepGap = 7.0f * k;
    const float sepX = folderPosX - sepGap;
    const float clearPosX = sepX - sepGap - btnSize;
    const float inputW = clearPosX - 6.0f * k - searchICX;

    const bool inputEnabled = engine->engineMode == MODE_DISABLED &&
                              !render::Renderer::get().isRecording() &&
                              !engine->macroSaveBusy();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, vec4(theme::rgbText(228, 233, 246)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 28.0f * k);
    // Center the input line in the 52k search box: the widget's own frame
    // height is the current font height + FramePadding*2, so keep the padding
    // derived from the real font size instead of a magic constant.
    const float fs = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(2.0f * k, std::max(0.0f, (srchH - fs) * 0.5f)));
    ImGui::SetCursorScreenPos(ImVec2(searchICX, sb.y));
    // Clamp the field to `inputW` so its hit rect stops before the action buttons
    ImGui::SetNextItemWidth(inputW);
    ImGui::BeginDisabled(!inputEnabled);
    // NOTE: IS THIS LIST TOGGLABLE BY TYPING? No - typing only fills the name/
    // search field; the library list is opened/closed exclusively by the folder
    // button (see below). The field's input state is simply not tracked here.
    if (ImGui::InputTextWithHint("##beta_macro_name", "Search or name your macro...",
                                 gui.macroName, sizeof(gui.macroName))) {
        state().filterDirty = true;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);

    state().searchMin = sb;
    state().searchW = leftW;

    const float chipCY = sb.y + srchH * 0.5f;

    // Clear button (deletes all typed text in the macro name field).
    const bool hasText = (gui.macroName[0] != '\0');
    const ImVec2 clearPos(clearPosX, chipCY - btnSize * 0.5f);
    const ImVec2 clearBtnSize(btnSize, btnSize);
    ImGui::SetCursorScreenPos(clearPos);
    ImGui::BeginDisabled(!hasText || !inputEnabled);
    if (ImGui::InvisibleButton("##beta_clear_name", clearBtnSize)) {
        gui.macroName[0] = '\0';
        state().filterDirty = true;
    }
    ImGui::EndDisabled();
    const bool clearHover = hasText && inputEnabled && ImGui::IsItemHovered();
    const float chov = anim01(ImGui::GetID("##beta_clear_name") ^ 0x434C,
                              clearHover ? 1.0f : 0.0f, 14.0f);
    if (chov > 0.004f) {
        draw->AddRectFilled(clearPos, add(clearPos, clearBtnSize),
                            withAlpha(theme::AccentSoft, chov), 28.0f * k);
        if (clearHover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const ImU32 clearCol = !hasText ? theme::rgb(70, 78, 96)
                                    : clearHover ? theme::Accent
                                                 : theme::rgb(136, 148, 170);
    drawEraseGlyph(draw, ImVec2(clearPosX + btnSize * 0.5f, chipCY), k, clearCol);

    // Vertical divider line between clear button and folder button
    draw->AddLine(ImVec2(sepX, chipCY - 10.0f * k), ImVec2(sepX, chipCY + 10.0f * k),
                  theme::rgb(56, 66, 86), 1.0f);

    // Folder button (right side) opens/closes the library list.
    const ImVec2 folderPos(folderPosX, chipCY - btnSize * 0.5f);
    const ImVec2 folderSize(btnSize, btnSize);
    ImGui::SetCursorScreenPos(folderPos);
    ImGui::InvisibleButton("##beta_folder", folderSize);
    const bool folderHover = ImGui::IsItemHovered();
    // Folder hover wash eases in instead of popping.
    const float fhov = anim01(ImGui::GetID("##beta_folder") ^ 0x464F,
                              folderHover ? 1.0f : 0.0f, 14.0f);
    if (fhov > 0.004f) {
        draw->AddRectFilled(folderPos, add(folderPos, folderSize),
                            withAlpha(theme::AccentSoft, fhov), 28.0f * k);
        if (folderHover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const bool folderDown = ImGui::IsItemActive();
    if (folderDown && !state().folderDownPrev) {
        state().searchForced = !state().searchVisible;
        if (state().searchForced) {
            gui.refreshMacroList();
            state().filterDirty = true;
        }
    }
    state().folderDownPrev = folderDown;
    // The icon doubles as a close button: a folder when the list is closed and
    // a cross while it is open, so the toggle affordance reads at a glance.
    const ImU32 iconCol = folderHover ? theme::Accent : theme::rgb(136, 148, 170);
    const ImVec2 iconC(folderPosX + btnSize * 0.5f, chipCY);
    if (state().searchForced) drawClearGlyph(draw, iconC, k, iconCol);
    else drawFolderGlyph(draw, iconC, k, iconCol);

    // Search results dropdown (drawn above the footer inside the window).
    // Only an explicit toggle (folder button) opens it — typing in the name
    // field must NOT dump the whole library.
    state().searchVisible = state().searchForced;

    y += srchH + 10.0f * k;

    // Transport buttons.
    const bool isSkc = (gui.macroBotType == Gui::MacroBotType::SKC);
    auto& bot = skk::skc::Bot::get();

    const bool recActive = isSkc
        ? (bot.getState() == skk::skc::State::Record || bot.getPendingAction() == skk::skc::PendingAction::Record)
        : (engine->engineMode == MODE_CAPTURE || engine->pendingAction == ReplayEngine::PendingAction::Record);
    const bool playActive = isSkc
        ? (bot.getState() == skk::skc::State::Playback || bot.isPlaybackArmed() || bot.getPendingAction() == skk::skc::PendingAction::Play)
        : (engine->engineMode == MODE_EXECUTE || engine->pendingPlaybackStart || engine->pendingAction == ReplayEngine::PendingAction::Play);
    const bool renderBusy = render::Renderer::get().isRecording();
    const bool hasInputs = isSkc
        ? bot.hasInputs()
        : engine->hasMacroInputs();
    // While the macro list is open it is modal: everything under it is
    // registered disabled (with AllowOverlap) so the list swallows the clicks.
    const bool listOpen = state().searchVisible;
    const float tBtnW = (leftW - 12.0f * k) * 0.5f;
    const float tBtnH = 60.0f * k;

    const bool saveBusy = !isSkc && engine->macroSaveBusy();
    const bool idle = isSkc
        ? (!recActive && !playActive && !renderBusy)
        : (engine->engineMode == MODE_DISABLED && !renderBusy && !saveBusy && !recActive && !playActive);
    const bool canClear = idle && !saveBusy && (hasInputs || gui.macroName[0] != '\0') && !listOpen;

    // The frame stepper freezes the scheduler by hand and drives it one tick at a
    // time, so playback cannot run alongside it — both would own
    // CCScheduler::update. Whichever is armed first wins and the other stays
    // disabled in the menu.
    const bool stepperOn = FrameStepper::get().isEnabled();

    // Keyboard chips on the transport buttons come from the live bind table, so
    // they stay truthful after a rebind (and disappear while unbound).
    const int recKey = gui.bindKeys[static_cast<int>(Gui::Bind::Record)];
    const int playKey = gui.bindKeys[static_cast<int>(Gui::Bind::Play)];
    const char* recHint = recKey ? Gui::keyName(recKey) : "";
    const char* playHint = playKey ? Gui::keyName(playKey) : "";

    if (transportButton(draw, "##beta_record", ImVec2(lx, y), ImVec2(tBtnW, tBtnH),
                        recActive ? "STOP" : "RECORD", recHint,
                        !playActive && !renderBusy && !listOpen && !saveBusy && !stepperOn,
                        TransportStyle::Lime, recActive,
                        recActive ? 1 : 0, k)) {
        if (isSkc) {
            if (recActive) {
                if (bot.getPendingAction() == skk::skc::PendingAction::Record) {
                    bot.clearPendingAction();
                } else {
                    skkbot2::gui::stopRecording(gui.macroName);
                }
            } else if (PlayLayer::get()) {
                skkbot2::gui::startRecording(gui.macroName);
            } else if (skk::gameplay::asEditor()) {
                skkbot2::log::info("RECORD (SKC) is only available in gameplay levels, not in the editor");
            } else {
                bot.setPendingAction(skk::skc::PendingAction::Record);
            }
            gui.refreshMacroList();
        } else {
            if (recActive) {
                if (engine->pendingAction == ReplayEngine::PendingAction::Record) {
                    engine->clearPendingAction();
                } else {
                    skkbot2::gui::stopRecording(gui.macroName);
                }
            } else if (PlayLayer::get()) {
                skkbot2::gui::startRecording(gui.macroName);
            } else if (skk::gameplay::asEditor()) {
                // Recording inside the editor is unsupported: captureTick is gated
                // on PlayLayer, so a queued record would just sit here and fire in
                // some unrelated level later. Refuse instead of queueing.
                skkbot2::log::info("RECORD is only available in gameplay levels, not in the editor");
            } else {
                engine->queueRecord(gui.macroName);
            }
            gui.refreshMacroList();
        }
    }
    // BUG-diagnostics (14.11, "PLAY не нажимается"): log every gate that
    // feeds the enabled flag, so a blocked press names its exact cause.
    if (ImGui::IsMouseHoveringRect(ImVec2(lx + tBtnW + 12.0f * k, y),
                                   ImVec2(lx + 2 * tBtnW + 12.0f * k, y + tBtnH)) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        skkbot2::log::info(
            "[GUI] PLAY gates: isSkc={} recActive={} renderBusy={} listOpen={} "
            "saveBusy={} stepperOn={} hasInputs={} playActive={} inLevel={}",
            isSkc ? 1 : 0, recActive ? 1 : 0, renderBusy ? 1 : 0,
            listOpen ? 1 : 0, saveBusy ? 1 : 0, stepperOn ? 1 : 0,
            hasInputs ? 1 : 0, playActive ? 1 : 0,
            PlayLayer::get() ? 1 : 0);
        skkbot2::Logger::get().flush();
    }
    if (transportButton(draw, "##beta_play", ImVec2(lx + tBtnW + 12.0f * k, y),
                        ImVec2(tBtnW, tBtnH),
                        playActive ? "STOP" : "PLAY", playHint,
                        !recActive && !renderBusy && !listOpen && !saveBusy && !stepperOn &&
                        (hasInputs || playActive),
                        TransportStyle::Dark,
                        playActive, playActive ? 1 : 2, k)) {
        if (isSkc) {
            if (playActive) {
                if (bot.getPendingAction() == skk::skc::PendingAction::Play) {
                    bot.clearPendingAction();
                } else {
                    bot.stop();
                }
                skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
            } else if (PlayLayer::get() || skk::gameplay::asEditor()) {
                skkbot2::gui::armPlayback(gui.macroName);
            } else {
                bot.setPendingAction(skk::skc::PendingAction::Play);
            }
        } else {
            if (playActive) {
                if (engine->pendingAction == ReplayEngine::PendingAction::Play ||
                    engine->pendingPlaybackStart) {
                    skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
                    engine->haltExecution();
                    engine->clearPendingAction();
                } else {
                    engine->haltExecution();
                    skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
                }
            } else if (PlayLayer::get()) {
                skkbot2::gui::armPlayback(gui.macroName);
            } else if (skk::gameplay::asEditor()) {
                // Same PLAY button in the editor: the engine decides where it is.
                // No PlayLayer yet -> requestExecutionStart arms (build mode) and
                // the playtest-start hook aligns; already in playtest -> in-place
                // armed-wait takeover. Never queue for a future level.
                skkbot2::gui::armPlayback(gui.macroName);
            } else {
                engine->queuePlayback(gui.macroName);
            }
        }
    }

    y += tBtnH + 6.0f * k;

    // File buttons (SAVE / LOAD).
    const float fBtnW = (leftW - 12.0f * k) * 0.5f;
    const float fBtnH = 40.0f * k;
    const char* saveLabel = saveBusy ? "SAVING..." : "SAVE";
    if (fileButton(draw, "##beta_save", ImVec2(lx, y), ImVec2(fBtnW, fBtnH), saveLabel, 0,
                   idle && hasInputs && gui.macroName[0] && !listOpen && !saveBusy, k)) {
        if (isSkc) {
            auto dir = skkbot2::paths::replays();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            // Exact name+.skc match gets _n+1, always - same rule as the SKK path. The
            // SKC branch had no name resolution at all, which is why saving here
            // behaved differently from every other save in the app.
            std::string const requested = gui.macroName;
            const std::string stem = ReplayStorage::makeUniqueReplayNameInDirectory(
                dir, requested, ".skc");
            auto file = dir / skk::stringToPath(stem + ".skc");
            if (bot.save(file)) {
                gui.refreshMacroList();
                // The name actually USED, not the one typed: after a collision
                // resolution they differ, and "saved awd" when the file is
                // awd_1.skc is how a user ends up hunting for a file that is not
                // there under the name they remember. The list refresh right
                // above is the visible feedback - no toast on top of it.
                const std::string shown =
                    (stem == requested) ? requested : (requested + " -> " + stem);
                skkbot2::Logger::get().info("skkBot2: saved SKC macro '{}'", shown);
            } else {
                // No toast here either (removed 19.10.2026 on request). The list
                // not gaining a row IS the signal, and the reason goes to the
                // log - which is where every other diagnostic in this mod lives
                // and where the user chooses to look.
                skkbot2::Logger::get().error(
                    "skkBot2: SKC save FAILED for '{}' (target {}) - the most "
                    "likely cause is a recording that validateActions() refuses "
                    "(practice checkpoint respawn mid-hold); see [SKC] SAVE "
                    "REFUSED above.", requested, skk::pathToUtf8(file));
            }
        } else {
            if (engine->activeSk) engine->activeSk->name = gui.macroName;
            if (engine->activeMacro) engine->activeMacro->name = gui.macroName;
            if (engine->saveActiveMacro()) {
                gui.refreshMacroList();
                skkbot2::Logger::get().info("skkBot2: saved macro '{}'", gui.macroName);
            }
        }
    }
    if (fileButton(draw, "##beta_clear", ImVec2(lx + fBtnW + 12.0f * k, y),
                   ImVec2(fBtnW, fBtnH), "CLEAR", 2,
                   canClear, k)) {
        if (isSkc) {
            bot.onResetEvent(skk::skc::Bot::ResetReason::UserClear, 0);
            gui.macroName[0] = '\0';
            gui.refreshMacroList();
        } else {
            skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
            engine->haltExecution();
            engine->discardActiveMacro();
            gui.macroName[0] = '\0';
            gui.refreshMacroList();
        }
    }

    y += fBtnH + 10.0f * k;

    // Timing controls divider (pinned separator under transport buttons).
    draw->AddLine(ImVec2(lx, y), ImVec2(lx + leftW, y), theme::Line, 1.0f);
    // 3k below the divider (was 5k): the freed 2k plus the previous 4k make a
    // 6k headroom INSIDE the scroll region right below, so the TPS group band
    // (which now starts 5k above the row, half-gap tiling) clears the clip
    // with a 1k margin.
    y += 1.0f + 3.0f * k;

    // Everything below the divider (TPS / Speed / Sync Music / HITBOXES /
    // LAYOUT MODE) becomes a single scrolling region.
    const float timingTop = y;
    y -= state().timingScroll;
    draw->PushClipRect(ImVec2(lx - 12.0f * k, timingTop),
                       ImVec2(lx + leftW + 16.0f * k, bottom), true);
    // Headroom for the TPS group band's top overhang (see the divider comment):
    // 6k of scrollable space before the first row; the band starts 5k above
    // the row, leaving a 1k margin to the clip.
    y += 6.0f * k;


    // TPS row. The last value stays visible/kept even while the toggle is off
    // (the value square and toggle just render disabled).
    // During playback (engineMode == MODE_EXECUTE), TPS is mandatory: the toggle
    // is locked ON and cannot be switched off by the user.
    const double prevTpsV = gui.tpsValue;
    const bool prevTpsOn = gui.tpsEnabled;
    double tpsV = gui.tpsValue;
    bool tpsOn = gui.tpsEnabled;
    const bool isPlaying = isSkc
        ? (bot.getState() == skk::skc::State::Playback || bot.isPlaybackArmed())
        : (engine->engineMode == MODE_EXECUTE);
    const bool playbackOverriding =
        isPlaying && !isSkc && engine->playbackTickRateOverrideActive;
    // An SKC session (record OR play) owns engine.tickRate: it pushed the
    // macro's tps there and expects it to stay. Without this the menu's
    // per-frame write below stomped the rate mid-recording, so the macro was
    // recorded at the menu TPS and played back at another one.
    const bool skcOwnsTiming = isSkc && bot.ownsTiming();
    // Timing is frozen for BOTH engines while a macro is executing. SKC already
    // took ownership of engine.tickRate at record time too, hence the extra
    // term; SKK's rate is already left alone by the branch structure below, but
    // the control stayed interactive, so the user could drag a number that
    // silently did nothing.
    const bool timingLocked = skcOwnsTiming || isPlaying;
    if (isPlaying) {
        tpsOn = true;
        tpsV = isSkc ? bot.getTps() : engine->runtimeTickRate();
    }
    // Fractional TPS is a first-class value: the pattern keeps 240 as "240",
    // 240.5 as "240.5" and 144.144 as "144.144" instead of rounding all three
    // to the same integer label. Wheel: 0.25 per notch, 0.001 with Shift.
    char tpsFmt[16];
    formatNumberPattern(tpsFmt, sizeof(tpsFmt), tpsV);
    // Row strips (invisible at rest, lit by hover or an active toggle).
    float gTpsA = 0.0f, gFpsA = 0.0f;
    // Rounding is FIXED by position (user rule): TPS is the top of the pair —
    // rounded top, SQUARE bottom; FPS is the bottom — square top, rounded
    // bottom; regardless of whether the neighbour is lit. Both lit -> one run
    // with a single rim (no seam line).
    const float tpsH = rowChannels("##tps_hl", tpsOn, lx, lx + leftW, y, k,
                                   gTpsA);
    const float fpsH = rowChannels("##fps_hl", gui.fpsEnabled, lx, lx + leftW,
                                   y + 50.0f * k, k, gFpsA);
    const float tpsHv[2] = { tpsH, fpsH };
    const float tpsAc[2] = { gTpsA, gFpsA };
    const float fpsHv[1] = { fpsH };
    const float fpsAc[1] = { gFpsA };
    const bool tpsLit = tpsH > 0.001f || gTpsA > 0.001f;
    const bool fpsLit = fpsH > 0.001f || gFpsA > 0.001f;
    if (tpsLit && fpsLit) {
        const float tops[2] = { y, y + 50.0f * k };
        drawStripRun(draw, lx, lx + leftW, k, tops, tpsHv, tpsAc, 2,
                     /*roundTopFirst=*/true, /*roundBottomLast=*/true);
    } else if (tpsLit) {
        const float tops[1] = { y };
        drawStripRun(draw, lx, lx + leftW, k, tops, tpsHv, tpsAc, 1,
                     /*roundTopFirst=*/true, /*roundBottomLast=*/false);
    } else if (fpsLit) {
        const float tops[1] = { y + 50.0f * k };
        drawStripRun(draw, lx, lx + leftW, k, tops, fpsHv, fpsAc, 1,
                     /*roundTopFirst=*/false, /*roundBottomLast=*/true);
    }
    const float block1 = timingControl(draw, mf, ImVec2(lx, y), leftW,
                                       "tps", "TPS",
                                       tpsV, 1.0, 1000000.0, tpsFmt, nullptr,
                                       tpsOn, k, 0.25, !timingLocked,
                                       nullptr, true, 0.001);
    if (skcOwnsTiming) {
        // Read-only while an SKC session holds the clock.
        tpsOn = true;
        tpsV = bot.getTps();
    } else if (isPlaying) {
        // Read-only rate while playing — swallow any edits and keep toggle on.
        tpsOn = true;
        tpsV = engine->runtimeTickRate();
    } else {
        gui.tpsValue = tpsV;
        gui.tpsEnabled = tpsOn;
        engine->tickRate = tpsOn ? gui.tpsValue : 240.0;
        if (gui.tpsValue != prevTpsV || gui.tpsEnabled != prevTpsOn) {
            Mod::get()->setSavedValue("gui_tps_enabled", gui.tpsEnabled);
            Mod::get()->setSavedValue("gui_tps_value", gui.tpsValue);
        }
    }
    y += block1 + 10.0f * k;

    // FPS row - directly under TPS, because the two are one decision: TPS sets
    // the LENGTH of a physics step, FPS sets how many of them the display gets
    // to run per frame. Nothing about accuracy lives here - the step stays
    // 1/runtimeTickRate regardless (see src/core/playback_fps.hpp) - so this row
    // stays editable DURING playback, like Speed and unlike TPS.
    //
    // 0 is the special value and means "follow TPS": one physics step per drawn
    // frame, which is Echo's default behaviour and the setting that takes the
    // wall clock out of the picture entirely. Without it the step budget is
    // still derived from the real frame rate, so a slow display costs headroom
    // instead of silently slowing the macro down.
    const double prevFpsV = gui.fpsValue;
    const bool prevFpsOn = gui.fpsEnabled;
    double fpsV = gui.fpsValue;
    bool fpsOn = gui.fpsEnabled;
    // LOCKED ONLY WHILE RECORDING, and editable during playback. Those are two
    // different questions and the row used to answer both with "a macro holds
    // the rate":
    //
    //   * record: the macro's stored fps is captured when the capture starts,
    //     so a number changed halfway through would describe neither half.
    //     Locked.
    //   * playback: the rate is a display decision, not part of the recorded
    //     input. The user asked to be able to change it mid-run, so the widget
    //     stays live and the module notices the difference (a push that does
    //     not match what the macro set is by construction a hand edit).
    const bool fpsRecording = isSkc
        ? (bot.getState() == skk::skc::State::Record ||
           bot.getPendingAction() == skk::skc::PendingAction::Record)
        : (engine->engineMode == MODE_CAPTURE);
    const bool fpsLocked = fpsRecording;
    const bool fpsMacro = skkbot2::fps::macroHolds();
    // What the macro put in force, so the row can mirror it. 0 means "follow
    // tps" - showing the tick rate here instead (as this used to) would push a
    // pinned value back on the very next frame, which the module would read as
    // a user edit and would then honour over the macro.
    const double macroFpsV =
        skkbot2::fps::target() > 0.0 ? skkbot2::fps::target() : 0.0;
    const bool macroFpsOn = skkbot2::fps::enabled();
    if (fpsMacro) {
        fpsV = macroFpsV;
        fpsOn = macroFpsOn;
    }
    // One thing to say, and it is the actionable one: how many physics steps this
    // frame rate asks for at the current TPS. With FPS at 0 the display follows
    // TPS, so this reads "1.00 st/f" - which is the fact that matters, without
    // spelling out the arithmetic the user already chose by leaving it at 0.
    //
    // FRACTIONAL, not ceil (user request 04.11.2026): the old "(int)ceil"
    // showed "2 st/f" at 240 tps / 180 fps, but the batch planner runs
    // floor + carry (physicsbypass.cpp planSteps) - the real pattern is the
    // cycling 1,1,2, which averages 1.33. The integer ceiling hid that and
    // made a non-integer ratio look identical to a true 2 st/f lock (120).
    static std::string fpsHint;
    {
        const double tickRate = skcOwnsTiming ? static_cast<double>(bot.getTps())
                             : isPlaying       ? static_cast<double>(engine->runtimeTickRate())
                                               : static_cast<double>(engine->tickRate);
        const double effFps = fpsV > 0.5 ? fpsV : tickRate;
        if (tickRate > 0.0 && effFps > 0.0) {
            fpsHint = fmt::format(" {:.2f} st/f", tickRate / effFps);
        } else {
            fpsHint.clear();
        }
    }
    // FPS shows ONE decimal always (user request 04.11.2026): the row is about
    // a display target, not physics, and "120.0" reads cleaner than the adaptive
    // "120" while still showing fractional targets like 144.1.
    char fpsFmt[16];
    std::snprintf(fpsFmt, sizeof(fpsFmt), "%%.1f");
    const float blockFps = timingControl(draw, mf, ImVec2(lx, y), leftW,
                                         "fps", "FPS",
                                         fpsV, 0.0, 1000000.0, fpsFmt, "",
                                         fpsOn, k, 0.25, !fpsLocked,
                                         fpsHint.c_str(), true, 0.001);
    if (!fpsLocked) {
        // Is this frame's value the user's, or just the macro's number being
        // mirrored back? Only the first kind may touch the saved setting -
        // writing the macro's rate into gui.fpsValue would make it the menu's
        // value after the session, which is exactly what endMacro() restores
        // over.
        const bool userEdited =
            !fpsMacro || fpsV != macroFpsV || fpsOn != macroFpsOn;
        if (userEdited) {
            gui.fpsValue = static_cast<double>(fpsV);
            gui.fpsEnabled = fpsOn;
        }
        // Push into the module EVERY frame, not only on change. The row stays
        // editable during playback, and the module tells a mirror from an edit
        // by comparing against what the macro set - so the push has to happen
        // for the comparison to have anything to compare.
        skkbot2::fps::setEnabled(fpsOn);
        skkbot2::fps::setTarget(fpsV > 0.5 ? static_cast<double>(fpsV)
                                           : skkbot2::fps::kFollowTps);
    }
    if (gui.fpsValue != prevFpsV || gui.fpsEnabled != prevFpsOn) {
        Mod::get()->setSavedValue("gui_fps_enabled", gui.fpsEnabled);
        Mod::get()->setSavedValue("gui_fps_value", gui.fpsValue);
    }
    y += blockFps + 10.0f * k;

    // NOTE: the timing MODE row (LOCK DELTA / REAL TIME) moved to the Settings
    // tab - MACRO BOT section, right after the Bot Engine row
    // (drawMacroBotSection, 31.10.2026, user request). Before this row there
    // was also a "Mode" caption: Lock Delta moves the level roughly one
    // smoothed-frame per drawn frame, Real Time feeds raw measured dt - both
    // at 1.0x wall speed now, the row is about grid smoothness, fine to live
    // under engine configuration.

    // Divider between the timing controls and the hitbox switches, matching
    // the one above the TPS row.
    draw->AddLine(ImVec2(lx, y), ImVec2(lx + leftW, y), theme::Line, 1.0f);
    y += 1.0f + 9.0f * k;

    // The HITBOXES stack scrolls inside its own clip when the
    // sub-rows outgrow the body (only this block scrolls: the timing rows above
    // and the NOCLIP column on the right keep their fixed positions).

    // HITBOXES row — visual-only collision-box overlay (see
    // src/hacks/hitboxes.cpp). Same row geometry as the TPS/Speed rows but
    // toggle-only (no value field): the label on the left, 44x24k toggle on
    // the right, both centered in the 40k row.
    //
    // The FILL sub-row (filled vs outline-only boxes) eases in ONLY while the
    // master is on — same nested-reveal pattern as the NOCLIP accordion's
    // Players/Death Flash rows.
    {
        const bool boxOn = engine->showHitboxes;
        ImFont* df = ImGui::GetFont();
        const float kt2 = uiTextScale();
        const float titleSize2 = pxSize(14.0f * kt2);
        const float rowH2 = 40.0f * k;
        const float togW2 = 44.0f * k;
        const float togH2 = 24.0f * k;
        const float subSize = pxSize(12.5f * kt2);
        const float subH = df ? df->CalcTextSizeA(subSize, FLT_MAX, 0.0f, "Fill").y
                              : ImGui::GetFontSize();
        const bool blocked = state().searchVisible;
        // Fill-subrow ease, computed early: the group-capsule layout below
        // depends on it (anim01 with the same id is idempotent within a frame,
        // so the later use reads the same value).
        const float fillT = anim01(ImGui::GetID("##hb_rows"),
                                   boxOn ? 1.0f : 0.0f, 11.0f);
        // Row strips (invisible at rest, lit by hover or an active toggle).
        // Neighbour presence decides the seams: traj merges with hitboxes
        // only while the Fill sub-row is collapsed (they are adjacent then).
        float gHbA = 0.0f, gTrA = 0.0f, gLmA = 0.0f;
        const float hbH = rowChannels("##hb_hl", boxOn, lx, lx + leftW, y, k,
                                      gHbA, /*easeHover=*/true,
                                      /*easeSpeed=*/5.0f);
        if (fillT <= 0.001f) {
            // Collapsed trio: hitboxes, trajectory and layout mode are
            // adjacent 50k tiles — draw ALL lit ones as ONE run with a single
            // rim, so touching square edges never double up into a stronger
            // seam line (user fix). Rounding: the run's top is rounded only
            // when hitboxes are in it (stack top), the bottom only when
            // Layout Mode is in it (stack bottom); middle rows stay square.
            const float trajTop = y + 50.0f * k;
            const float lmTop = y + 100.0f * k;
            const float trH = rowChannels("##traj_hl", engine->showTrajectory,
                                          lx, lx + leftW, trajTop, k, gTrA);
            const float lmH = rowChannels("##lm_hl", engine->layoutMode,
                                          lx, lx + leftW, lmTop, k, gLmA);
            const float topArr[3] = { y, trajTop, lmTop };
            const float hovArr[3] = { hbH, trH, lmH };
            const float actArr[3] = { gHbA, gTrA, gLmA };
            const bool lit[3] = {
                hbH > 0.001f || gHbA > 0.001f,
                trH > 0.001f || gTrA > 0.001f,
                lmH > 0.001f || gLmA > 0.001f,
            };
            int first = -1, last = -1;
            for (int i = 0; i < 3; ++i) {
                if (lit[i]) { last = i; if (first < 0) first = i; }
            }
            if (first >= 0) {
                // Trim the run to the lit span so the rim never wraps a dark
                // gap row; inner dark rows inside a contiguous lit span are
                // skipped for fills only (rim covers the union).
                drawStripRun(draw, lx, lx + leftW, k, topArr + first,
                             hovArr + first, actArr + first,
                             last - first + 1,
                             /*roundTopFirst=*/first == 0,
                             /*roundBottomLast=*/last == 2);
            }
        } else if (hbH > 0.001f || gHbA > 0.001f) {
            const float tops[1] = { y };
            const float hov[1] = { hbH };
            const float act[1] = { gHbA };
            // Hitboxes expanded (user rule 06.11): it now reads as a TRUE
            // single row full capsule — like Speed or Noclip. When the
            // master is turned off, the collapsed-trio rules above return.
            drawStripRun(draw, lx, lx + leftW, k, tops, hov, act, 1,
                         /*roundTopFirst=*/true, /*roundBottomLast=*/true);
        }

        // Master row.
        {
            const float textH2 = df ? df->CalcTextSizeA(titleSize2, FLT_MAX, 0.0f, "Hitboxes").y
                                    : ImGui::GetFontSize();
            const ImVec2 tPos2(lx, y + (rowH2 - textH2) * 0.5f);
            drawGradientText(draw, df, titleSize2, tPos2, "Hitboxes",
                             boxOn ? theme::rgbText(222, 230, 245) : theme::rgbText(179, 186, 204),
                             boxOn ? theme::rgbText(240, 245, 255) : theme::rgbText(179, 186, 204));
            const ImVec2 tog2(lx + leftW - togW2, y + (rowH2 - togH2) * 0.5f);
            // Only the toggle switch flips hitboxes (user 13.11): the text /
            // rest of the row is not clickable. The old whole-row
            // InvisibleButton (debug-era "log every transition") made clicking
            // the caption turn the overlay on.
            const bool togClicked = toggleAt("##beta_hitboxes", tog2, boxOn, !blocked);
            if (togClicked) {
                engine->showHitboxes = !boxOn;
                Mod::get()->setSavedValue("gui_hitboxes", engine->showHitboxes);
                skk::hitboxes::forceRefresh();
                skkbot2::log::info("skkBot2: hitboxes -> {}",
                    engine->showHitboxes ? "on" : "off");
            }
            y += rowH2;
        }

        // Fill sub-row: eases open with the master, and its control only takes
        // clicks once it is (almost) fully revealed — a still-hidden toggle
        // must not steal clicks (same 0.85f reveal gate as NOCLIP).
        // The 10k gap above the row lives INSIDE the eased block (NOCLIP's
        // extraH idiom), so a collapsed row leaves no dangling space, and the
        // row slides up with the reveal instead of jumping to a fixed offset.
        const float subGap = 10.0f * k;
        const float fillBlockH = subGap + rowH2;
        const float seen = fillT * fillBlockH;
        if (fillT > 0.001f && seen > 0.001f) {
            const float rowY = y + subGap * fillT;
            draw->PushClipRect(ImVec2(lx - 12.0f * k, y - 12.0f * k),
                               ImVec2(lx + leftW + 16.0f * k, y + seen), true);
            const bool subL = seen >= fillBlockH * 0.85f;
            const bool fillOn = boxOn && engine->hitboxFill;
            const ImVec2 tPos3(lx, rowY + (rowH2 - subH) * 0.5f);
            drawGradientText(draw, df, subSize, tPos3, "Fill",
                             fillOn ? theme::rgbText(222, 230, 245) : theme::rgbText(179, 186, 204),
                             fillOn ? theme::rgbText(240, 245, 255) : theme::rgbText(179, 186, 204));
            const ImVec2 tog3(lx + leftW - togW2, rowY + (rowH2 - togH2) * 0.5f);
            if (toggleAt("##beta_hitbox_fill", tog3, fillOn, boxOn && subL && !blocked)) {
                engine->hitboxFill = !engine->hitboxFill;
                Mod::get()->setSavedValue("gui_hitbox_fill", engine->hitboxFill);
                skk::hitboxes::forceRefresh();
            }
            draw->PopClipRect();
            y += seen;
        }
    }

    // Same 10k rhythm the TPS/Speed rows use.
    y += 10.0f * k;

    // TRAJECTORY block
    {
        const bool trajOn = engine->showTrajectory;
        const bool blocked = state().searchVisible;
        ImFont* df = ImGui::GetFont();
        const float kt2 = uiTextScale();
        const float titleSize2 = pxSize(14.0f * kt2);
        const float rowH2 = 40.0f * k;
        const float togW2 = 44.0f * k;
        const float togH2 = 24.0f * k;
        const float nfW2 = 100.0f * k;
        const float nfH2 = 34.0f * k;
        const float tgGap2 = 17.0f * k;

        // Master row
        {
            // Trajectory strip: rounded top only when hitboxes are ON (the
            // Fill sub-row opens above it, so nothing merges there); square
            // top when the trio is collapsed and the hitboxes strip is lit
            // right above. Bottom merges with the Layout Mode strip when lit.
            float gTra2A = 0.0f, gLm2A = 0.0f;
            const float trajH2 = rowChannels("##traj_hl", trajOn,
                                             lx, lx + leftW, y, k, gTra2A);
            const float lmH2 = rowChannels("##lm_hl", engine->layoutMode,
                                           lx, lx + leftW, y + 50.0f * k, k,
                                           gLm2A);
            const float fillT2 = anim01(ImGui::GetID("##hb_rows"),
                                        engine->showHitboxes ? 1.0f : 0.0f,
                                        11.0f);
            // Rounding is FIXED by position (user rule): with hitboxes OFF
            // Trajectory is the MIDDLE of the trio — square top AND bottom;
            // with hitboxes ON it is the TOP of the pair — rounded top, square
            // bottom. Layout Mode is always the bottom — square top, rounded
            // bottom. Both lit -> one run with a single rim.
            const bool trajIsTop = fillT2 > 0.5f;
            // While the trio is collapsed the trajectory/layout strips were
            // already drawn in the trio run above (single rim, no seam line) —
            // drawing them here again would double the shapes.
            const bool drawnInTrio = fillT2 <= 0.001f;
            const float topArr2[2] = { y, y + 50.0f * k };
            const float hovArr2[2] = { trajH2, lmH2 };
            const float actArr2[2] = { gTra2A, gLm2A };
            const bool trajLit = trajH2 > 0.001f || gTra2A > 0.001f;
            const bool lmLit = lmH2 > 0.001f || gLm2A > 0.001f;
            if (!drawnInTrio && trajLit && lmLit) {
                drawStripRun(draw, lx, lx + leftW, k, topArr2, hovArr2, actArr2,
                             2, /*roundTopFirst=*/trajIsTop,
                             /*roundBottomLast=*/true);
            } else if (!drawnInTrio && trajLit) {
                drawStripRun(draw, lx, lx + leftW, k, topArr2, hovArr2, actArr2,
                             1, trajIsTop, /*roundBottomLast=*/false);
            } else if (!drawnInTrio && lmLit) {
                drawStripRun(draw, lx, lx + leftW, k, topArr2 + 1,
                             hovArr2 + 1, actArr2 + 1, 1,
                             /*roundTopFirst=*/false, /*roundBottomLast=*/true);
            }
            const float tH = df ? df->CalcTextSizeA(titleSize2, FLT_MAX, 0.0f, "Trajectory").y
                                : ImGui::GetFontSize();
            const float tW = df ? df->CalcTextSizeA(titleSize2, FLT_MAX, 0.0f, "Trajectory").x
                                : 80.0f * k;
            const ImVec2 tPos(lx, y + (rowH2 - tH) * 0.5f);
            drawGradientText(draw, df, titleSize2, tPos, "Trajectory",
                             trajOn ? theme::rgbText(222, 230, 245) : theme::rgbText(179, 186, 204),
                             trajOn ? theme::rgbText(240, 245, 255) : theme::rgbText(179, 186, 204));

            // Beta badge
            {
                const float badgeFontSize = pxSize(9.5f * kt2);
                statusBadge(draw, df, badgeFontSize, k,
                            lx + tW + kBadgeGap * k, y + rowH2 * 0.5f, "BETA");
            }

            const float nfX = lx + leftW - nfW2 - tgGap2 - togW2;
            const float tgX = lx + leftW - togW2;
            float lenVal = static_cast<float>(engine->pathLength);
            if (numberField(mf, "##traj_length",
                            ImVec2(nfX, y + (rowH2 - nfH2) * 0.5f), nfW2, lenVal,
                            10.0f, 1200.0f, " f", "%.0f", trajOn && !blocked, 10.0f)) {
                engine->pathLength = std::clamp(static_cast<int>(lenVal), 10, 1200);
                Mod::get()->setSavedValue("gui_trajectory_length", engine->pathLength);
            }

            const ImVec2 tg(tgX, y + (rowH2 - togH2) * 0.5f);
            if (toggleAt("##beta_trajectory", tg, trajOn, !blocked)) {
                engine->showTrajectory = !trajOn;
                Mod::get()->setSavedValue("gui_trajectory", engine->showTrajectory);
                skkbot2::log::info(
                    "skkBot2: trajectory -> {} (level={})",
                    engine->showTrajectory ? "on" : "off",
                    PlayLayer::get() ? (PlayLayer::get()->m_level
                                            ? PlayLayer::get()->m_level->m_levelName.c_str()
                                            : "<no level obj>")
                                     : "none");
            }
            y += rowH2;
        }
    }

    // LAYOUT MODE - fixed row right after the scrolling HITBOXES stack.
    // GDH calls it "Layout Mode" (src/hacks/level/layout_mode.cpp:7, "Removes
    // all decoration and colour from levels"); flat master row, single knob.
    {
        const bool lmOn = engine->layoutMode;
        const bool lmBlocked = state().searchVisible;
        ImFont* ldf = ImGui::GetFont();
        const float lmKt = uiTextScale();
        const float lmSize = pxSize(14.0f * lmKt);
        const float lmRowH = 40.0f * k;
        const float lmTogW = 44.0f * k;
        const float lmTogH = 24.0f * k;
        const float lmTextH = ldf ? ldf->CalcTextSizeA(lmSize, FLT_MAX, 0.0f,
                                                      "Layout Mode").y
                                  : ImGui::GetFontSize();

        y += 10.0f * k;
        // Layout Mode's strip is drawn together with Trajectory's (merged
        // run, single rim) at the Trajectory site above.
        const ImVec2 lmTPos(lx, y + (lmRowH - lmTextH) * 0.5f);
        drawGradientText(draw, ldf, lmSize, lmTPos, "Layout Mode",
                         lmOn ? theme::rgbText(222, 230, 245) : theme::rgbText(179, 186, 204),
                         lmOn ? theme::rgbText(240, 245, 255) : theme::rgbText(179, 186, 204));
        const ImVec2 lmTog(lx + leftW - lmTogW, y + (lmRowH - lmTogH) * 0.5f);
        if (toggleAt("##beta_layout_mode", lmTog, lmOn, !lmBlocked)) {
            engine->layoutMode = !lmOn;
            Mod::get()->setSavedValue("gui_layout_mode", engine->layoutMode);
            skkbot2::Logger::get().info("skkBot2: layout mode -> {}",
                                        engine->layoutMode ? "on" : "off");
        }
        y += lmRowH;

        // Dependent row "Regular BG" (Echo "Use Regular Background",
        // silicate PlayLayer.cpp): keep the authored background and
        // middleground while the rest of the layout flattens. Lives in its
        // own full-capsule strip BELOW the fixed Layout Mode row (the
        // collapsed trio hb/traj/LM keeps its positional geometry above);
        // the slot opens only while Layout Mode is on (anim01 + clip), and
        // interactions are gated until the reveal settled.
        const float rSeen = anim01(ImGui::GetID("##lm_regular_bg_seen"),
                                   lmOn ? 1.0f : 0.0f, 11.0f);
        // Same extraH idiom as the Fill sub-row: the 10k gap lives INSIDE the
        // eased block, the row slides up with the reveal, and the clip opens
        // top-down (from the LM row's baseline), so collapsing looks identical.
        const float rGap = 10.0f * k;
        const float rBlockH = rGap + 40.0f * k;
        const float rSeenH = rSeen * rBlockH;
        if (rSeen > 0.001f && rSeenH > 0.001f) {
            const float rowY = y + rGap * rSeen;
            const bool rOn = engine->layoutModeRegularBg;
            const bool rL = rSeenH >= rBlockH * 0.85f;
            const bool rEnabled = lmOn && rL && !lmBlocked;
            const float rTextH = ldf
                                     ? ldf->CalcTextSizeA(lmSize, FLT_MAX, 0.0f,
                                                          "Regular BG").y
                                     : ImGui::GetFontSize();
            draw->PushClipRect(ImVec2(lx - 12.0f * k, y - 12.0f * k),
                               ImVec2(lx + leftW + 16.0f * k, y + rSeenH), true);
            // No row strip: dependent rows like the hitboxes "Fill" sub-row
            // render bare (no hover/active highlight) — user 12.11.
            const ImVec2 rTPos(lx, rowY + (40.0f * k - rTextH) * 0.5f);
            drawGradientText(draw, ldf, lmSize, rTPos, "Regular BG",
                             rOn ? theme::rgbText(222, 230, 245)
                                 : theme::rgbText(179, 186, 204),
                             rOn ? theme::rgbText(240, 245, 255)
                                 : theme::rgbText(179, 186, 204));
            const ImVec2 rTog(lx + leftW - lmTogW,
                              rowY + (40.0f * k - lmTogH) * 0.5f);
            if (toggleAt("##beta_layout_regular_bg", rTog, rOn, rEnabled)) {
                engine->layoutModeRegularBg = !rOn;
                Mod::get()->setSavedValue("gui_layout_regular_bg",
                                          engine->layoutModeRegularBg);
                skkbot2::Logger::get().info(
                    "skkBot2: layout regular bg -> {}",
                    engine->layoutModeRegularBg ? "on" : "off");
            }
            draw->PopClipRect();
            y += rSeenH;
        }

        // Dependent row "Default Art" (Echo layout.cpp): every block / slope
        // / spike / saw is redrawn as the standard GD object sized to its
        // real hitbox, on an art layer below the player. Same extraH idiom
        // and stacking as the Regular BG row above: the 10k gap lives inside
        // the eased block, the row sits after the previous block's seenH, the
        // clip opens top-down, and interactions are gated until reveal.
        const float aSeen = anim01(ImGui::GetID("##lm_default_art_seen"),
                                   lmOn ? 1.0f : 0.0f, 11.0f);
        const float aGap = 10.0f * k;
        const float aBlockH = aGap + 40.0f * k;
        const float aSeenH = aSeen * aBlockH;
        if (aSeen > 0.001f && aSeenH > 0.001f) {
            const float rowY = y + aGap * aSeen;
            const bool aOn = engine->layoutModeDefaultArt;
            const bool aL = aSeenH >= aBlockH * 0.85f;
            const bool aEnabled = lmOn && aL && !lmBlocked;
            const float aTextH = ldf
                                     ? ldf->CalcTextSizeA(lmSize, FLT_MAX, 0.0f,
                                                          "Default Art").y
                                     : ImGui::GetFontSize();
            draw->PushClipRect(ImVec2(lx - 12.0f * k, y - 12.0f * k),
                               ImVec2(lx + leftW + 16.0f * k, y + aSeenH), true);
            // No row strip: bare dependent row like Regular BG above.
            const ImVec2 aTPos(lx, rowY + (40.0f * k - aTextH) * 0.5f);
            drawGradientText(draw, ldf, lmSize, aTPos, "Default Art",
                             aOn ? theme::rgbText(222, 230, 245)
                                 : theme::rgbText(179, 186, 204),
                             aOn ? theme::rgbText(240, 245, 255)
                                 : theme::rgbText(179, 186, 204));
            const ImVec2 aTog(lx + leftW - lmTogW,
                              rowY + (40.0f * k - lmTogH) * 0.5f);
            if (toggleAt("##beta_layout_default_art", aTog, aOn, aEnabled)) {
                engine->layoutModeDefaultArt = !aOn;
                Mod::get()->setSavedValue("gui_layout_default_art",
                                          engine->layoutModeDefaultArt);
                skkbot2::Logger::get().info(
                    "skkBot2: layout default art -> {}",
                    engine->layoutModeDefaultArt ? "on" : "off");
            }
            draw->PopClipRect();
            y += aSeenH;
        }
    }

    // Close the timing clip and run its scrollbar. The content height is only
    // known now, after the eased rows settled, so the clamp runs post-draw and
    // the offset persists to the next frame. The thumb sits in the gutter
    // BETWEEN the columns (colGap), never over the left column's right-aligned
    // toggles.
    const float timingContentH = (y + state().timingScroll) - timingTop;
    draw->PopClipRect();
    applyTimingScroll(draw, ImVec2(lx - 12.0f * k, timingTop),
                      ImVec2(lx + leftW + 20.0f * k, bottom), timingTop, bottom,
                      timingContentH, k);

    // Right column: NOCLIP controls (master, P1/P2 targets, death flash). Its
    // own independent scroll region: the stack is tall and was previously cut
    // off at the bottom of the body with no way to reach the lower rows.
    drawNoClipSection(draw, mf, ImVec2(rx, top), ImVec2(rx + rightW, bottom), k);
}

// ---------------------------------------------------------------------------
// Body pages
// ---------------------------------------------------------------------------

void drawPagePlaceholder(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* note,
                         float k) {
    ImFont* mf = mono();
    const float kt = uiTextScale();
    const float nSize = pxSize(8.5f * kt);
    const float sSize = 1.4f * kt;
    const float nW = spacedWidth(mf, nSize, note, sSize);
    const float nY = (min.y + max.y) * 0.5f;
    const float textH = mf ? mf->CalcTextSizeA(nSize, FLT_MAX, 0, note).y
                           : ImGui::GetFontSize();
    spacedText(draw, mf, nSize, ImVec2(min.x + (max.x - min.x) * 0.5f - nW * 0.5f,
                                       nY - textH * 0.5f),
               note, theme::rgb(146, 156, 176), sSize);
}

// ---------------------------------------------------------------------------
// WAVE TRAIL FIX page (ASSIST tab). The drag-fix rebuilds the dart streak
// points every update instead of only on collisions, and the draw-fix
// replaces the vanilla stroke with a miter-joined band that supports a color
// override and a white outline. Visual only — no physics / input changes.
//
// Rows mirror the SETTINGS page (40k rows, 44x24k toggles, 160k fields). The
// sub-rows (drag / draw / color / opacity) live ONLY under their master, eased
// like the NOCLIP accordion.
// ---------------------------------------------------------------------------
float drawWaveTrailSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                           float bottom, float k, float openT) {
    auto* engine = ReplayEngine::get();
    State& st = state();
    ImFont* mf = mono();
    const float kt = uiTextScale();

    if (sectionHeading(draw, gridTL, width, top, "VISUAL", st.waveTrailOpen,
                       nullptr, k, 1.0f, "assist_wavetrail")) {
        st.waveTrailOpen = !st.waveTrailOpen;
    }
    const float rowH = 40.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float fieldW = 160.0f * k;
    const float gap = 10.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Trail").y
                           : ImGui::GetFontSize();

    const ImU32 capOn = theme::rgbText(222, 228, 242);
    const ImU32 capOff = theme::rgbText(179, 186, 204);

    const bool on = engine && engine->waveTrailFix;

    // Заголовок -> мастер-строка: 27k, контракт SPEED (устраняет наезд полосы
    // мастера на подпись секции).
    const float y = top + 27.0f * k;

    // Master row is always visible; sub-rows ease in only while on.
    const float rowsT = anim01(ImGui::GetID("##wt_rows"), on ? 1.0f : 0.0f, 11.0f);
    const float rowsH = rowH * 5.0f + gap * 4.0f;
    const float wtSeen = openT * (rowH + rowsT * rowsH);
    if (openT <= 0.001f || wtSeen <= 0.001f) return 27.0f * k;
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + wtSeen + 12.0f * k), true);
    // `seen` is the wave-trail block's own revealed height, measured from the
    // block top — the row-reveal gates only concern these rows.
    const float seen = wtSeen;
    const bool row1L = seen >= rowH * 0.85f;
    auto rowNL = [seen, rowH, gap](int n) -> bool {
        return seen >= (rowH + gap * static_cast<float>(n)) * 0.85f;
    };

    auto caption = [&](float ry, const char* text, bool active) {
        const ImVec2 tPos(gridTL.x, ry + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, text,
                         active ? capOn : capOff, active ? theme::rgbText(240, 245, 255) : capOff);
    };

    // Полосы-выделение секции WAVE TRAIL FIX: ТОЛЬКО мастер-кнопка (пользователь
    // 07.11 — зависимые строки полос не получают). Отступ заголовок -> строка
    // = 27k, как у SPEED на вкладке RECORD — иначе полоса мастера наезжает
    // на текст секции. Полная капсула.
    drawRowStrip(draw, "##wt_hl_master", on, gridTL.x, gridTL.x + width, y, k,
                 true, true);

    // Row 1 — master switch.
    {
        caption(y, "Wave Trail Fix", on);
        const ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##wt_master", tog, on, row1L)) {
            engine->waveTrailFix = !on;
            Mod::get()->setSavedValue("gui_wavetrail", engine->waveTrailFix);
            skkbot2::Logger::get().info("skkBot2: wave trail fix -> {}",
                                        engine->waveTrailFix ? "on" : "off");
        }
    }

    // Row 2 — drag fix (streak geometry rebuild).
    {
        const float ry = y + rowH + gap;
        const bool subOn = on && engine->waveTrailDragFix;
        caption(ry, "Drag Fix", subOn);
        const ImVec2 tog(gridTL.x + width - toggleW, ry + (rowH - toggleH) * 0.5f);
        if (toggleAt("##wt_drag", tog, engine->waveTrailDragFix, on && rowNL(1))) {
            engine->waveTrailDragFix = !engine->waveTrailDragFix;
            Mod::get()->setSavedValue("gui_wavetrail_drag", engine->waveTrailDragFix);
        }
    }

    // Row 3 — draw fix (stroke renderer).
    {
        const float ry = y + (rowH + gap) * 2.0f;
        const bool subOn = on && engine->waveTrailDrawFix;
        caption(ry, "Draw Fix", subOn);
        const ImVec2 tog(gridTL.x + width - toggleW, ry + (rowH - toggleH) * 0.5f);
        if (toggleAt("##wt_draw", tog, engine->waveTrailDrawFix, on && rowNL(2))) {
            engine->waveTrailDrawFix = !engine->waveTrailDrawFix;
            Mod::get()->setSavedValue("gui_wavetrail_draw", engine->waveTrailDrawFix);
        }
    }

    // Row 4 — trail opacity (0..100 %).
    {
        const float ry = y + (rowH + gap) * 3.0f;
        caption(ry, "Opacity", on);
        const float fx = gridTL.x + width - fieldW;
        const float fy = ry + (rowH - 34.0f * k) * 0.5f;
        float pct = engine ? engine->waveTrailOpacity * 100.0f : 100.0f;
        if (numberField(mf, "##wt_opacity", ImVec2(fx, fy), fieldW, pct, 0.0f, 100.0f,
                        "%", "%.0f", on && rowNL(3), 1.0f)) {
            if (engine) {
                engine->waveTrailOpacity = std::clamp(pct, 0.0f, 100.0f) * 0.01f;
                Mod::get()->setSavedValue("gui_wavetrail_opacity", engine->waveTrailOpacity);
            }
        }
    }

    // Row 5 — outline opacity (0..100 %), the white non-solid edge band.
    {
        const float ry = y + (rowH + gap) * 4.0f;
        caption(ry, "Outline", on);
        const float fx = gridTL.x + width - fieldW;
        const float fy = ry + (rowH - 34.0f * k) * 0.5f;
        float pct = engine ? engine->waveTrailOutlineOpacity * 100.0f : 65.0f;
        if (numberField(mf, "##wt_outline", ImVec2(fx, fy), fieldW, pct, 0.0f, 100.0f,
                        "%", "%.0f", on && rowNL(4), 1.0f)) {
            if (engine) {
                engine->waveTrailOutlineOpacity = std::clamp(pct, 0.0f, 100.0f) * 0.01f;
                Mod::get()->setSavedValue("gui_wavetrail_outline_opacity",
                                          engine->waveTrailOutlineOpacity);
            }
        }
    }

    draw->PopClipRect();
    // Нижний вылет полосы мастера (5k за контент) выносим в высоту секции,
    // иначе клип скролла режет его снизу при прокрутке в самый низ.
    return 27.0f * k + seen + openT * 5.0f * k;
}

// ---------------------------------------------------------------------------
// MACRO BOT section (Settings tab): macro engine / format selector.
// Allows choosing between:
// - SKK: Full physics replay engine (.skk)
// - SKC: Ultra-compact input replay engine (.skc)
// ---------------------------------------------------------------------------
float drawMacroBotSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                          float bottom, float k, float openT) {
    ImFont* mf = mono();
    const float kt = uiTextScale();
    State& st = state();
    auto& gui = skkbot2::gui::get();
    auto& bot = skk::skc::Bot::get();

    // Отступ заголовок -> первая строка: 27k от PINNED-заголовка MACRO BOT.
    // У этой секции нет своего in-flow заголовка: её рисует sticky-заголовок
    // на y, а стек секций начинается на viewTop = y + 20k, поэтому отсюда
    // добираем только недостающие 7k (иначе зазор был 47k вместо 27k,
    // как у ACCURACY, чей заголовок in-flow).
    const float y0 = top + 7.0f * k;

    const float rowH = 40.0f * k;
    const float pillW = 100.0f * k;
    const float pillH = 34.0f * k;
    const float pillGap = 10.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Engine").y
                           : ImGui::GetFontSize();
    const ImU32 capOff = theme::rgbText(179, 186, 204);

    // Six rows: Bot Engine, Format, Mode (Lock Delta / Real Time), Save Seed,
    // Block Player Input, Autosave. The old P2 Button (fourth in the
    // old count) is gone - the button now comes from the file, not from a
    // guess. Counting it here left the body a full row taller than what is
    // drawn, and since the caller adds this return value to the next
    // heading's y, the gap showed up as a blank band ABOVE the following
    // section's title rather than anywhere inside this one.
    const float rowsH = (rowH + pillGap) * 5.0f + rowH;
    const float seen = openT * rowsH;
    // 27k слот заголовка минус 20k, уже отстёгнутые под pinned-заголовок
    // (viewTop = y + 20k): свёрнутая секция оставляет 7k + 10k зазор.
    if (openT <= 0.001f || seen <= 0.001f) return 7.0f * k + kSectionGap * k;

    // Полосы-выделение MACRO BOT: ВСЕ 6 строк — один слитный прогон
    // (Bot Engine → Autosave), round оба конца, квадратные швы, один рим.
    // Активный канал — у Toggle-строк Save Seed / Block Player Input /
    // Autosave, пока они включены; строки с пилюлями — hover-only.
    // Клип тот же, что у контента ниже: без него полосы ещё скрытых строк
    // при сворачивании секции рисовались поверх следующих заголовков/строк
    // (баг подсветки при collapse, пользователь 10.11).
    {
        draw->PushClipRect(ImVec2(gridTL.x - 16.0f * k, y0 - 16.0f * k),
                           ImVec2(gridTL.x + width + 16.0f * k, y0 + seen + 16.0f * k), true);
        static const char* const mbIds[6] = {
            "##mb_hl_eng", "##mb_hl_fmt", "##mb_hl_mode",
            "##mb_hl_seed", "##mb_hl_bpi", "##mb_hl_as",
        };
        float tops[6], ho[6], ac[6];
        for (int i = 0; i < 6; ++i) {
            const float ry = y0 + static_cast<float>(i) * (rowH + pillGap);
            tops[i] = ry;
            bool act = false;
            if (i == 3)
                act = Mod::get()->getSavedValue<bool>("gui_record_seed", true);
            if (i == 4) act = bot.getIgnoreInputs();
            if (i == 5) act = skkbot2::autosave::enabled();
            ho[i] = rowChannels(mbIds[i], act, gridTL.x, gridTL.x + width, ry, k,
                                ac[i]);
        }
        drawStripRun(draw, gridTL.x, gridTL.x + width, k, tops, ho, ac, 6,
                     true, true);
        draw->PopClipRect();
    }

    draw->PushClipRect(ImVec2(gridTL.x - 16.0f * k, y0 - 16.0f * k),
                       ImVec2(gridTL.x + width + 16.0f * k, y0 + seen + 16.0f * k), true);

    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH + pillGap + rowH) * 0.85f;
    const bool row3L = seen >= ((rowH + pillGap) * 2.0f + rowH) * 0.85f;
    const bool row4L = seen >= ((rowH + pillGap) * 3.0f + rowH) * 0.85f;
    const bool row5L = seen >= ((rowH + pillGap) * 4.0f + rowH) * 0.85f;
    const bool row6L = seen >= rowsH * 0.85f;

    float y = y0;

    // Row 1 — Bot Engine pills: SKK vs SKC
    {
        const float by = y + (rowH - pillH) * 0.5f;

        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Bot Engine",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));

        const bool isSkk = (gui.macroBotType == Gui::MacroBotType::SKK);
        // Парковка снята 09.11: SKK снова доступен для тестов переделки
        // практики (BUG-121); memtrack служит сеткой безопасности.
        //
        // v4 (13.11, ported from the v1 bundle): two independent pills became
        // ONE segmented control. SKK and SKC are a single decision ("which
        // engine"), and the sliding thumb IS the answer, instead of two
        // lit/unlit pills the user has to compare. Sized to its content.
        static const char* const kEngineItems[2] = {"SKK", "SKC"};
        const float engineW = segmentedNaturalWidth(kEngineItems, 2);
        const int enginePick = segmentedControl("##set_bot_engine",
                                                ImVec2(gridTL.x + width - engineW, by),
                                                engineW, pillH, kEngineItems, 2,
                                                isSkk ? 0 : 1, row1L);
        if (enginePick == 0) {
            gui.macroBotType = Gui::MacroBotType::SKK;
            Mod::get()->setSavedValue("gui_macro_bot_type", static_cast<int>(Gui::MacroBotType::SKK));
            skkbot2::Logger::get().info("skkBot2: macro engine -> SKK (physics replay)");
        } else if (enginePick == 1) {
            gui.macroBotType = Gui::MacroBotType::SKC;
            Mod::get()->setSavedValue("gui_macro_bot_type", static_cast<int>(Gui::MacroBotType::SKC));
            skkbot2::Logger::get().info("skkBot2: macro engine -> SKC (input replay)");
        }
        y += rowH + pillGap;
    }

    // Row 2 — Format Info / Description badge
    {
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Format",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));

        const bool isSkk = (gui.macroBotType == Gui::MacroBotType::SKK);
        const char* infoStr = isSkk ? "SKK  ·  Physics Replay (.skk)" : "SKC  ·  Input Replay (.skc)";
        const float infoSize = pxSize(10.5f * kt);
        ImVec2 strSz = mf ? mf->CalcTextSizeA(infoSize, FLT_MAX, 0.0f, infoStr) : ImVec2(0, 0);
        const float badgeW = strSz.x + 20.0f * k;
        const float badgeH = 28.0f * k;
        const float bx = gridTL.x + width - badgeW;
        const float by = y + (rowH - badgeH) * 0.5f;

        draw->AddRectFilled(ImVec2(bx, by), ImVec2(bx + badgeW, by + badgeH),
                            theme::FieldDark, 14.0f * k);
        draw->AddRect(ImVec2(bx, by), ImVec2(bx + badgeW, by + badgeH),
                      theme::FieldBorder, 14.0f * k, 0, 1.0f);
        const ImVec2 textPos(bx + 10.0f * k, by + (badgeH - strSz.y) * 0.5f);
        draw->AddText(mf, infoSize, textPos, theme::rgbText(190, 202, 225), infoStr);

        y += rowH + pillGap;
    }

    // Row 3 — Timing Mode pills: Lock Delta vs Real Time. Moved here from the
    // main tab's TIMING column (31.10.2026): it is engine-adjacent
    // configuration, so it lives under the Bot Engine row. Same persisted
    // keys and the same dropCarry() the old row used.
    {
        const float by = y + (rowH - pillH) * 0.5f;

        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Mode",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));

        const bool isLockDelta = gui.lockDelta && !gui.realTime;
        // v4 (13.11, ported from the v1 bundle): one segmented control — the
        // two timing modes are mutually exclusive by definition, so they get
        // a mutually exclusive widget with a sliding thumb.
        static const char* const kTimingItems[2] = {"LOCK DELTA", "REAL TIME"};
        const float modeW = segmentedNaturalWidth(kTimingItems, 2);
        const int modePick = segmentedControl("##set_timing_mode",
                                              ImVec2(gridTL.x + width - modeW, by),
                                              modeW, pillH, kTimingItems, 2,
                                              isLockDelta ? 0 : 1, row3L);
        if (modePick == 0) {
            gui.lockDelta = true;
            gui.realTime = false;
            Mod::get()->setSavedValue("gui_timing_lock_delta", true);
            Mod::get()->setSavedValue("gui_timing_real_time", false);
            dropCarry();
            skkbot2::log::info("skkBot2: Timing Mode -> LOCK DELTA");
        } else if (modePick == 1) {
            gui.lockDelta = false;
            gui.realTime = true;
            Mod::get()->setSavedValue("gui_timing_lock_delta", false);
            Mod::get()->setSavedValue("gui_timing_real_time", true);
            dropCarry();
            skkbot2::log::info("skkBot2: Timing Mode -> REAL TIME");
        }
        y += rowH + pillGap;
    }

    // Row 4 — Save Seed: whether the RNG-reproduction data goes INTO THE
    // FILE on Save/Autosave. SKK: the setupRngSeed in the variance section;
    // SKC: the RNG journal tail (SKC's seed-equivalent — the file has no
    // dedicated seed field, the journal is what pins the world). In-memory
    // macro data is never touched: only the serialized bytes are gated.
    {
        const bool recSeed = Mod::get()->getSavedValue<bool>("gui_record_seed", true);
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Save Seed",
                         recSeed ? theme::rgbText(222, 228, 242) : capOff,
                         recSeed ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_bot_record_seed", tog, recSeed, true, row4L)) {
            Mod::get()->setSavedValue("gui_record_seed", !recSeed);
            skkbot2::Logger::get().info(
                "skkBot2: save seed in macro file -> {}", !recSeed ? "on" : "off");
        }
        y += rowH + pillGap;
    }

    // Row 5 — Block Player Input (Blocks physical clicks during SKC playback)
    {
        const bool ign = bot.getIgnoreInputs();
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Block Player Input",
                         ign ? theme::rgbText(222, 228, 242) : capOff,
                         ign ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_bot_ignore_inputs", tog, ign, true, row5L)) {
            bot.setIgnoreInputs(!ign);
            Mod::get()->setSavedValue("gui_skc_ignore_inputs", bot.getIgnoreInputs());
            skkbot2::Logger::get().info("EclipseBot: ignore inputs -> {}",
                                        bot.getIgnoreInputs() ? "on" : "off");
        }
        y += rowH + pillGap;
    }

    // Row 6 — Autosave: periodic rescue snapshots of the macro WHILE RECORDING
    // (only during record; playback/idle never write). SKK lands in
    // `<level>_autosave.skk`, SKC in `<level>_autosave.skc` (the live level's
    // name), both overwritten in place. Default on - the tester asked for
    // "never lose a recording".
    {
        const bool on = skkbot2::autosave::enabled();
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Autosave",
                         on ? theme::rgbText(222, 228, 242) : capOff,
                         on ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_bot_autosave", tog, on, true, row6L)) {
            skkbot2::autosave::setEnabled(!on);
        }
        y += rowH;
    }

    // Row 4 used to be a "P2 Button" selector here, because the converter's
    // per-click export carries the channel but not the button and the rest of
    // the .ast has nowhere to put one. It is gone because that is no longer
    // true: we ask the converter to re-emit the macro as Silicate (id 47),
    // whose record is (delta << 4) | (button << 2) | (p2 << 1) | holding, and
    // read that back with the SLC reader. The button now comes from the file,
    // which is the only place it was ever right to read it from - it is a
    // property of the macro's author, not of the level's gamemode.
    //
    // The fallback path (frame/p1/p2 only) still has no button to read, so it
    // assumes 1, and it is logged loudly when it runs.

    draw->PopClipRect();
    // 7k (27k-слот заголовка минус 20k pinned-заголовка) + вылет нижней
    // полосы (5k) в высоту секции.
    return 7.0f * k + seen + openT * 5.0f * k + kSectionGap * k;
}

// ---------------------------------------------------------------------------
// SETTINGS page: accuracy mode (Vanilla / CBS) + COS.
// Rows mirror the NOCLIP accordion exactly: same 40k height,
// same pill (100x34k) and toggle (44x24k) boxes, same caption typography.
// ---------------------------------------------------------------------------
float drawSettingsPage(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                       float bottom, float k, float openT, bool hideHeading = false) {
    auto* engine = ReplayEngine::get();
    ImFont* mf = mono();
    const float kt = uiTextScale();

    State& st = state();
    if (!hideHeading &&
        sectionHeading(draw, gridTL, width, top, "ACCURACY", st.settingsOpen, nullptr,
                       k, 1.0f, "settings_accuracy")) {
        st.settingsOpen = !st.settingsOpen;
    }
    const float y0 = top + 27.0f * k;

    const float rowH = 40.0f * k;
    const float pillW = 100.0f * k;
    const float pillH = 34.0f * k;
    const float pillGap = 10.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Mode").y
                           : ImGui::GetFontSize();
    const ImU32 capOff = theme::rgbText(179, 186, 204);
    float y = y0;

    // SKC is vanilla-only (BUG-86): its CBS/COS modes are dead end-to-end.
    // CBS/COS are now FROZEN for SKK too (05.11, same decision): SKK sessions
    // always run vanilla regardless of the menu or the macro's accuracy tag.
    // The controls stay in the layout per the user's request - "не давать
    // включать" (don't let them be turned on) - so with either bot selected
    // the pills and the COS toggle render locked grey instead of disappearing.
    // While a session is running the SKK controls stay locked too, because its
    // timing contract is taken at start.
    const auto botType = skkbot2::gui::get().macroBotType;
    const bool skcSelected =
        botType == Gui::MacroBotType::SKC;
    const bool skkSelected =
        botType == Gui::MacroBotType::SKK;
    const auto skcState = skk::skc::Bot::get().getState();
    const bool skcBusy = skcSelected &&
        (skcState == skk::skc::State::Record ||
         skcState == skk::skc::State::Playback);

    // Pick up CBS / COS from GD's own settings. This has to happen BEFORE the
    // collapsed-accordion early-return below: the section is collapsed by
    // default, so a sync placed after it would never run and the pills/toggle
    // would keep showing stale values until the user expanded ACCURACY.
    if (engine) {
        engine->syncAccuracyFromGame();
    }

    // Rows: Mode pills, COS toggle, Safe Mode.
    const float rowsH = (rowH + pillGap) * 2.0f + rowH;
    const float seen = openT * rowsH;
    if (openT <= 0.001f || seen <= 0.001f) return 27.0f * k + kSectionGap * k;

    // Полосы-выделение ACCURACY: ВСЕ 3 строки — один слитный прогон
    // (Mode → Click On Steps → Safe Mode), round оба конца, один рим.
    // Активный канал — у Toggle-строк COS / Safe Mode, пока они включены;
    // строка пилюль Mode — hover-only.
    // Клип тот же, что у контента: полосы скрытых строк не должны рисоваться
    // при сворачивании (см. MACRO BOT).
    {
        draw->PushClipRect(ImVec2(gridTL.x - 16.0f * k, y0 - 16.0f * k),
                           ImVec2(gridTL.x + width + 16.0f * k, y0 + seen + 16.0f * k), true);
        static const char* const acIds[3] = {
            "##acc_hl_mode", "##acc_hl_cos", "##acc_hl_safe",
        };
        float tops[3], ho[3], av[3];
        for (int i = 0; i < 3; ++i) {
            const float ry = y0 + static_cast<float>(i) * (rowH + pillGap);
            tops[i] = ry;
            bool act = false;
            if (i == 1) act = engine->cosEnabled && !(skcSelected || skkSelected);
            if (i == 2) act = engine->safeMode;
            ho[i] = rowChannels(acIds[i], act, gridTL.x, gridTL.x + width, ry, k,
                                av[i]);
        }
        drawStripRun(draw, gridTL.x, gridTL.x + width, k, tops, ho, av, 3,
                     true, true);
        draw->PopClipRect();
    }

    draw->PushClipRect(ImVec2(gridTL.x - 16.0f * k, y0 - 16.0f * k),
                       ImVec2(gridTL.x + width + 16.0f * k, y0 + seen + 16.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH + pillGap + rowH) * 0.85f;

    // Row 1 — accuracy MODE: mutually exclusive VANILLA / CBS pills. Clicking
    // either sets the mode outright (unlike the independent NOCLIP P1/P2
    // pills, one of the two is always active).
    {
        const float labelSize = pxSize(9.0f * kt);
        const float labelSpacing = 1.2f * kt;
        const float groupW = pillW * 2.0f + pillGap;
        const float gx = gridTL.x + width - groupW;
        const float by = y + (rowH - pillH) * 0.5f;

        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Mode",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));

        const bool isVanilla = engine->selectedAccuracyMode == AccuracyMode::Vanilla;
        const bool frozen = skcSelected || skkSelected;
        const bool pillsEnabled = !skcBusy && !frozen;
        // Both pills always visible. With SKC or SKK selected they lock grey:
        // both bots are vanilla-only now (SKC BUG-86; SKK freeze 05.11), so the
        // user request was "keep the controls, just don't let them be turned
        // on". VANILLA stays lit, CBS grey.
        if (selectionToggle("##set_acc_vanilla", ImVec2(gx, by),
                            ImVec2(pillW, pillH), mf, labelSize,
                            labelSpacing, "VANILLA",
                            isVanilla || frozen, pillsEnabled, row1L)) {
            engine->selectedAccuracyMode = AccuracyMode::Vanilla;
            Mod::get()->setSavedValue("gui_accuracy",
                                      static_cast<int>(AccuracyMode::Vanilla));
            ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                   engine->cosEnabled);
            skkbot2::Logger::get().info("skkBot2: accuracy mode -> Vanilla");
        }
        if (selectionToggle("##set_acc_cbs", ImVec2(gx + pillW + pillGap, by),
                            ImVec2(pillW, pillH), mf, labelSize, labelSpacing,
                            "CBS", !isVanilla && !frozen, pillsEnabled,
                            row1L)) {
            engine->selectedAccuracyMode = AccuracyMode::CBS;
            Mod::get()->setSavedValue("gui_accuracy",
                                      static_cast<int>(AccuracyMode::CBS));
            ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                   engine->cosEnabled);
            skkbot2::Logger::get().info("skkBot2: accuracy mode -> CBS");
        }
        y += rowH + pillGap;
    }

    // Row 2 — COS (Click On Steps) toggle. Visible for SKC and SKK but locked:
    // both force it off (vanilla-only), so the toggle renders off and dead.
    const bool cosOn = engine->cosEnabled && !(skcSelected || skkSelected);
    const bool cosEnabled = !skcBusy && !(skcSelected || skkSelected);
    {
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Click On Steps",
                         cosOn ? theme::rgbText(222, 228, 242) : capOff,
                         cosOn ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_acc_cos", tog, cosOn, cosEnabled, row2L)) {
            engine->cosEnabled = !engine->cosEnabled;
            Mod::get()->setSavedValue("gui_cos", engine->cosEnabled);
            ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                   engine->cosEnabled);
            skkbot2::Logger::get().info("skkBot2: COS -> {}",
                                       engine->cosEnabled ? "on" : "off");
        }
        y += rowH + pillGap;
    }

// SAFE MODE row — the last row of the page, INSIDE the clip so it scrolls with
    // the rest of the section. Single master toggle: the two sub-toggles
    // (Freeze Attempts / Freeze Jumps) and the whole Auto Save Mod row were
    // deleted at the user's request. Safe Mode now always freezes both effects,
    // matching GDH "Safe Mode" and EclipseMenu SafeMode.cpp, which hardcode them
    // with no submenu. Uses the section's own rowH so rowsH's third term is the
    // last increment — the only place y is advanced below.
    {
        ImVec2 smTog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        drawGradientText(draw, df, titleSize,
                         ImVec2(gridTL.x, y + (rowH - textH) * 0.5f), "Safe Mode",
                         engine->safeMode ? theme::rgbText(222, 228, 242) : capOff,
                         engine->safeMode ? theme::rgbText(240, 245, 255) : capOff);
        if (toggleAt("##set_safe_mode", smTog, engine->safeMode, true, true)) {
            engine->safeMode = !engine->safeMode;
            Mod::get()->setSavedValue("gui_safe_mode", engine->safeMode);
            // Re-applied on PlayLayer::init, so a level that is already loaded
            // only picks it up on re-entry — say so instead of implying the
            // current run changed retroactively.
            skkbot2::Logger::get().info(
                "skkBot2: safe mode -> {} (freezes attempts + jumps; applies on next level entry)",
                engine->safeMode ? "on" : "off");
        }
        y += rowH;
    }


    draw->PopClipRect();
    // 27k (отступ заголовка) + вылет нижней полосы (5k) в высоту секции.
    return 27.0f * k + seen + openT * 5.0f * k + kSectionGap * k;
}

// ---------------------------------------------------------------------------
// KEYBIND section: the menu toggle key. A SIBLING section of ACCURACY — its
// own page-level heading (same 1.0-scale style, always visible and clickable)
// and its own open/close animation, not a sub-accordion nested inside the
// ACCURACY block. One row underneath: caption on the left, the bound key as a
// pill on the right. Clicking the pill arms capture mode: the next physical
// key press (handled in Gui::handleKey / main.cpp's key listener) becomes the
// bind; Esc cancels. While armed the pill shows "PRESS A KEY" and stays lit.
// Returns the eased height it occupies (heading + rows) so the next section
// stacks under it.
// ---------------------------------------------------------------------------
float drawKeybindSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                         float k, float openT) {
    auto& gui = skkbot2::gui::get();
    ImFont* mf = mono();
    const float kt = uiTextScale();

    // No accordion heading here: this section is a standalone screen whose
    // "KEYBINDS" title is a pinned page heading drawn by drawBody, not a
    // collapsible section heading inside a stack. Only the rows reveal.
    const float y0 = top;

    const float rowH = 40.0f * k;
    const float pillW = 100.0f * k;
    const float pillH = 34.0f * k;
    // Same inter-row gap the TPS/Speed rows use, so keybind rows sit on the
    // page's global rhythm instead of their own tighter 6k.
    const float pillGap = 10.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Menu").y
                           : ImGui::GetFontSize();

    // Rows are grouped: each group starts with a caption + rule ("полосочка"),
    // which is where the binds live. MENU / TRANSPORT / ASSISTS.
    struct Row {
        bool isGroupHead;
        const char* label;
        int bindIndex;  // -1 for a group head
    };
    Row rows[Gui::kBindCount + Gui::kGroupCount];
    int rowCount = 0;
    for (int g = 0; g < Gui::kGroupCount; ++g) {
        if (g > 0) {
            rows[rowCount++] = { true, Gui::kBindGroupNames[g], -1 };
        }
        for (int i = 0; i < Gui::kBindCount; ++i) {
            const auto* info = gui.bindInfo(i);
            if (info && info->group == g) {
                rows[rowCount++] = { false, info->label, i };
            }
        }
    }

    // Group head is shorter than a full row: caption + rule sit on one line.
    const float headH = 26.0f * k;
    float rowsH = 0.0f;
    for (int r = 0; r < rowCount; ++r) {
        rowsH += rows[r].isGroupHead ? headH : rowH;
        if (!rows[r].isGroupHead) rowsH += pillGap;
    }

    const float seen = openT * rowsH;
    if (openT <= 0.001f || seen <= 0.001f) return kSectionGap * k;  // rows fully collapsed
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y0 - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y0 + seen + 12.0f * k), true);

    const float labelSize = pxSize(9.0f * kt);
    const float labelSpacing = 1.2f * kt;
    const float groupSize = pxSize(9.0f * kt);
    const float gx = gridTL.x + width - pillW;

    float y = y0;
    float rowBottom = y0;

    for (int r = 0; r < rowCount; ++r) {
        const Row& row = rows[r];

        if (row.isGroupHead) {
            // The separator band: caption, then a hairline rule across the rest
            // of the row. Only the caption + rule show once revealed; they carry
            // no control, so there is nothing to gate clicks on.
            if (y + headH <= y0 + seen) {
                const float capH = df ? df->CalcTextSizeA(groupSize, FLT_MAX, 0.0f,
                                                         row.label).y
                                      : ImGui::GetFontSize();
                const ImVec2 capPos(gridTL.x, y + (headH - capH) * 0.5f);
                drawGradientText(draw, df, groupSize, capPos, row.label,
                                 theme::rgb(150, 158, 178), theme::rgb(150, 158, 178));
                const float capW = df ? df->CalcTextSizeA(groupSize, FLT_MAX, 0.0f,
                                                          row.label).x
                                      : 60.0f * k;
                const float ruleY = y + headH - 6.0f * k;
                const float ruleX = gridTL.x + capW + 10.0f * k;
                if (ruleX < gridTL.x + width) {
                    draw->AddLine(ImVec2(ruleX, ruleY),
                                  ImVec2(gridTL.x + width, ruleY), theme::Line, 1.0f);
                }
            }
            y += headH;
            continue;
        }

        const int idx = row.bindIndex;
        const bool waiting = (gui.kbWaiting == idx);
        const char* keyLabel = waiting ? "PRESS A KEY"
                                       : Gui::keyName(gui.bindKeys[idx]);
        const bool rowLive = y + rowH <= y0 + seen + rowH * 0.15f;
        const bool revealed = (y + rowH) <= (y0 + seen) + 1.0f;

        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, row.label,
                         waiting ? theme::Accent : theme::rgbText(222, 228, 242),
                         waiting ? theme::Accent : theme::rgbText(240, 245, 255));

        const float by = y + (rowH - pillH) * 0.5f;
        char id[32];
        std::snprintf(id, sizeof(id), "##set_kb_%d", idx);
        if (selectionToggle(id, ImVec2(gx, by), ImVec2(pillW, pillH), mf,
                            labelSize, labelSpacing, keyLabel, waiting,
                            true, rowLive && revealed)) {
            // Single armed slot: clicking an armed pill disarms it, clicking any
            // other pill moves the capture there.
            gui.kbWaiting = waiting ? -1 : idx;
            skkbot2::Logger::get().info("skkBot2: keybind capture {}",
                                        gui.kbWaiting >= 0 ? row.label : "cancelled");
        }
        // Right-click on a live pill removes the bind entirely (NONE, 0). Gated
        // the same way as the pill itself (rowLive && revealed) so hidden rows
        // never take clicks; capture in progress is left alone — Delete inside
        // capture does the unbind there.
        if (!waiting && rowLive && revealed &&
            ImGui::IsMouseHoveringRect(ImVec2(gx, by),
                                       ImVec2(gx + pillW, by + pillH), true) &&
            ImGui::GetIO().MouseClicked[1] &&
            gui.bindKeys[idx] != 0) {
            gui.bindKeys[idx] = 0;
            gui.persistBind(static_cast<skkbot2::gui::Gui::Bind>(idx));
            skkbot2::Logger::get().info("skkBot2: keybind removed: {} (RMB)",
                                        row.label);
        }

        y += rowH + pillGap;
        rowBottom = y;
    }

    draw->PopClipRect();
    (void)rowBottom;
    return 20.0f * k + seen + kSectionGap * k;
}

// ---------------------------------------------------------------------------
inline void refreshFontList() {
    auto& st = state();
    st.fontItems.clear();
    st.fontItems.push_back("Default");

    const auto fontsDir = skkbot2::paths::fonts();
    std::error_code ec;
    std::filesystem::create_directories(fontsDir, ec);
    if (std::filesystem::exists(fontsDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(fontsDir, ec)) {
            if (entry.is_regular_file(ec)) {
                auto ext = skk::pathToUtf8(entry.path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
                if (ext == ".ttf" || ext == ".otf") {
                    st.fontItems.push_back(skk::pathToUtf8(entry.path().filename()));
                }
            }
        }
    }
    if (st.fontItems.size() > 2) {
        // Case-insensitive order WITHOUT copying. The old comparator made two
        // std::string copies and ran tolower over both on EVERY comparison, so
        // sorting n names cost 2*n*log(n) heap allocations. Same order, no
        // allocation: compare unsigned char by unsigned char, lowercased.
        auto lessNoCase = [](const std::string& a, const std::string& b) {
            const size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i) {
                const auto ca = static_cast<unsigned char>(
                    std::tolower(static_cast<unsigned char>(a[i])));
                const auto cb = static_cast<unsigned char>(
                    std::tolower(static_cast<unsigned char>(b[i])));
                if (ca != cb) return ca < cb;
            }
            return a.size() < b.size();
        };
        std::sort(st.fontItems.begin() + 1, st.fontItems.end(), lessNoCase);
    }
    st.fontsDirty = false;
}

// v4 ACCENT COLOR: shared hex → tint logic. Parses "#RRGGBB" (or "RRGGBB"),
// derives the internal hue rotation / saturation multiplier from the shipped
// base azure (120,205,255), repaints the palette and persists everything.
// Returns the normalised hex on success, "" on a parse failure.
// v4 VISUAL JSON sync — definition sits below (after currentAccentHex).
void syncThemeJson();

std::string applyAccentHex(const std::string& typed) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const size_t off = (!typed.empty() && typed[0] == '#') ? 1 : 0;
    if (typed.size() != off + 6) return "";
    int rgbv[3];
    for (int i = 0; i < 3; ++i) {
        const int hi = hexVal(typed[off + i * 2]);
        const int lo = hexVal(typed[off + i * 2 + 1]);
        if (hi < 0 || lo < 0) return "";
        rgbv[i] = hi * 16 + lo;
    }
    const float rf = rgbv[0] / 255.0f, gf = rgbv[1] / 255.0f, bf = rgbv[2] / 255.0f;
    const float mx = std::max({rf, gf, bf});
    const float mn = std::min({rf, gf, bf});
    const float d = mx - mn;
    float h = 0.0f;
    if (d > 1e-6f) {
        if (mx == rf) h = std::fmod(60.0f * (gf - bf) / d + 360.0f, 360.0f);
        else if (mx == gf) h = 60.0f * (bf - rf) / d + 120.0f;
        else h = 60.0f * (rf - gf) / d + 240.0f;
    }
    const float s = mx > 1e-6f ? d / mx : 0.0f;
    // Base azure (120,205,255): h = 202.29°, s = 0.5294, v = 1.0. The stored
    // rotation is an internal delta (may be negative / beyond 360).
    constexpr float kBaseH = 202.29f, kBaseS = 0.529412f;
    theme::tintHue = h - kBaseH;
    theme::tintSat = std::clamp(s / kBaseS, 0.0f, 2.0f);
    theme::refreshTint();
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", rgbv[0], rgbv[1], rgbv[2]);
    Mod::get()->setSavedValue("gui_tint_hue", theme::tintHue);
    Mod::get()->setSavedValue("gui_tint_sat", theme::tintSat);
    Mod::get()->setSavedValue("gui_tint_color", std::string(hex));
    syncThemeJson();
    return hex;
}

// v4 TEXT COLOR: the text has its OWN colour, independent of the accent.
// Parses "#RRGGBB" and stores the ABSOLUTE hue + a saturation floor derived
// from the picked colour's saturation (0.05..0.5, so pure red tints strongly,
// pastels gently; a grey pick ≈ neutral). Empty result on parse failure.
std::string applyTextHex(const std::string& typed) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const size_t off = (!typed.empty() && typed[0] == '#') ? 1 : 0;
    if (typed.size() != off + 6) return "";
    int rgbv[3];
    for (int i = 0; i < 3; ++i) {
        const int hi = hexVal(typed[off + i * 2]);
        const int lo = hexVal(typed[off + i * 2 + 1]);
        if (hi < 0 || lo < 0) return "";
        rgbv[i] = hi * 16 + lo;
    }
    const float rf = rgbv[0] / 255.0f, gf = rgbv[1] / 255.0f, bf = rgbv[2] / 255.0f;
    const float mx = std::max({rf, gf, bf});
    const float mn = std::min({rf, gf, bf});
    const float d = mx - mn;
    float h = 0.0f;
    if (d > 1e-6f) {
        if (mx == rf) h = std::fmod(60.0f * (gf - bf) / d + 360.0f, 360.0f);
        else if (mx == gf) h = 60.0f * (bf - rf) / d + 120.0f;
        else h = 60.0f * (rf - gf) / d + 240.0f;
    }
    const float s = mx > 1e-6f ? d / mx : 0.0f;
    theme::textHue = h;
    theme::textSatFloor = std::clamp(s, 0.05f, 0.5f);
    theme::refreshTint();
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", rgbv[0], rgbv[1], rgbv[2]);
    theme::textHex = hex;
    Mod::get()->setSavedValue("gui_text_hue", theme::textHue);
    Mod::get()->setSavedValue("gui_text_sat_floor", theme::textSatFloor);
    Mod::get()->setSavedValue("gui_text_color", theme::textHex);
    syncThemeJson();
    return hex;
}

// Current effective accent as "#RRGGBB" (the shipped azure through the live
// tint) — what the Accent Color row shows and Export writes.
std::string currentAccentHex() {
    const ImU32 col = theme::tintApply(120, 205, 255, 255);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                  static_cast<int>(col & 0xFF),
                  static_cast<int>((col >> 8) & 0xFF),
                  static_cast<int>((col >> 16) & 0xFF));
    return hex;
}

// Path of the visual-settings JSON (save-dir root, next to the logs).
std::filesystem::path themeJsonPath() { return paths::file("gui_theme.json"); }

struct VisualJson {
    bool hasSolid = false;
    bool solid = true;
    bool hasBlur = false;
    bool blur = true;
    std::string color;
    std::string text;
};

// Minimal parser for OUR json layout (flat object, no nesting/escapes).
VisualJson parseVisualJson(const std::string& body) {
    VisualJson out;
    auto findVal = [&](const char* key) -> std::string {
        const std::string pat = std::string("\"") + key + "\":";
        size_t p = body.find(pat);
        if (p == std::string::npos) return "";
        p += pat.size();
        while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
        const size_t e = body.find_first_of(",\n}", p);
        if (e == std::string::npos) return "";
        std::string v = body.substr(p, e - p);
        if (!v.empty() && v.front() == '"') {
            v.erase(0, 1);
            if (!v.empty() && v.back() == '"') v.pop_back();
        }
        return v;
    };
    const std::string solid = findVal("solid");
    if (!solid.empty()) {
        out.hasSolid = true;
        out.solid = solid == "true" || solid == "1";
    }
    const std::string blur = findVal("blur");
    if (!blur.empty()) {
        out.hasBlur = true;
        out.blur = blur == "true" || blur == "1";
    }
    out.color = findVal("color");
    out.text = findVal("text");
    return out;
}

bool writeVisualJson(bool solid, bool blur, const std::string& colorHex,
                     const std::string& textHex = "#FFFFFF") {
    // "text" is ALWAYS present (user 15.11): "#FFFFFF" = neutral text.
    const std::string textOut = textHex.empty() ? std::string("#FFFFFF") : textHex;
    std::ofstream out(themeJsonPath());
    if (!out) return false;
    out << "{\n"
        << "  \"solid\": " << (solid ? "true" : "false") << ",\n"
        << "  \"blur\": " << (blur ? "true" : "false") << ",\n"
        << "  \"color\": \"" << colorHex << "\""
        << (",\n  \"text\": \"" + textOut + "\"")
        << "\n}\n";
    return true;
}

// Keep gui_theme.json in step with the LIVE visual state (solid/blur toggles,
// accent/sat changes, accent import). Uses the effective accent from the tint.
void syncThemeJson() {
    writeVisualJson(theme::solidBackground, theme::blurEnabled,
                    currentAccentHex(), theme::textHex);
}

// ---------------------------------------------------------------------------
// v4 VISUAL JSON: gui_theme.json (в save-dir мода) — ЕДИНЫЙ источник истины
// для solid / blur / акцентного цвета. Создаётся при первом старте; Export /
// Import и стартовая загрузка работают с ним. Файлы открываются передачей
// std::filesystem::path напрямую (контракт path.string() — не нарушен).
// (объявление — выше, перед applyAccentHex)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// COSMETICS section (Settings tab): menu appearance toggles. A SIBLING of KEYBIND /
// LOGGING — same heading style, same open/close animation. Two rows: Solid
// Background switch and Menu Font picker.
// ---------------------------------------------------------------------------
float drawCosmeticsSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                           float k, float openT, bool hideHeading = false) {
    ImFont* mf = mono();
    const float kt = uiTextScale();
    State& st = state();

    // hideHeading: see drawKeybindSection — the pinned sticky heading already spells
    // this section out, so drawing its own heading again would double the label.
    if (!hideHeading &&
        sectionHeading(draw, gridTL, width, top, "COSMETICS", st.cosmeticsOpen, nullptr,
                       k, 1.0f, "settings_cosmetics")) {
        st.cosmeticsOpen = !st.cosmeticsOpen;
    }
    // Отступ заголовок -> первая строка: 27k, контракт SPEED (полоса Solid
    // не должна наезжать на заголовок COSMETICS).
    const float y = top + 27.0f * k;

    const float rowH = 40.0f * k;
    const float rowGap = 10.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float fieldW = 160.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Solid").y
                           : ImGui::GetFontSize();
    const ImU32 capOff = theme::rgbText(179, 186, 204);

    // 8 строк + хвост Export/Import (отступ 4k + кнопка 30k + хвост 10k).
    const float totalRowsH = rowH * 8.0f + rowGap * 7.0f + 44.0f * k;
    const float seen = openT * totalRowsH;
    if (openT <= 0.001f || seen <= 0.001f) return 27.0f * k + kSectionGap * k;

    // Полосы-выделение COSMETICS: ВСЕ 7 строк — один слитный прогон
    // (Solid Background → Blur → Window Animation → Font → Font Size →
    //  Accent Color → Color Sat → Text Color), round оба конца, один рим.
    // Активный канал — у Toggle-строк: Solid (пока включён), Blur (пока
    // включён), Window Animation (пока включена) — как у Solid.
    // Клип тот же, что у контента: полосы скрытых строк не должны рисоваться
    // при сворачивании (см. MACRO BOT).
    {
        draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                           ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
        static const char* const coIds[8] = {
            "##co_hl_solid", "##co_hl_blur", "##co_hl_anim", "##co_hl_font",
            "##co_hl_fs", "##co_hl_hue", "##co_hl_sat", "##co_hl_text",
        };
        float tops[8], ho[8], av[8];
        for (int i = 0; i < 8; ++i) {
            const float ry = y + static_cast<float>(i) * (rowH + rowGap);
            tops[i] = ry;
            const bool act = (i == 0 && theme::solidBackground) ||
                             (i == 1 && theme::blurEnabled) ||
                             (i == 2 && st.shellAnim);
            ho[i] = rowChannels(coIds[i], act, gridTL.x, gridTL.x + width, ry, k,
                                av[i]);
        }
        drawStripRun(draw, gridTL.x, gridTL.x + width, k, tops, ho, av, 8,
                     true, true);
        draw->PopClipRect();
    }
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH * 2.0f + rowGap) * 0.85f;
    const bool row3L = seen >= (rowH * 3.0f + rowGap * 2.0f) * 0.85f;
    const bool row4L = seen >= (rowH * 4.0f + rowGap * 3.0f) * 0.85f;
    const bool row5L = seen >= (rowH * 5.0f + rowGap * 4.0f) * 0.85f;
    const bool row6L = seen >= (rowH * 6.0f + rowGap * 5.0f) * 0.85f;
    const bool row7L = seen >= (rowH * 7.0f + rowGap * 6.0f) * 0.85f;
    const bool row8L = seen >= (rowH * 8.0f + rowGap * 7.0f) * 0.85f;
    const bool row9L =
        seen >= (rowH * 8.0f + rowGap * 7.0f + 34.0f * k) * 0.85f;

    float yy = y;

    // Row 1 — solid (non-transparent) background switch.
    {
        const bool on = theme::solidBackground;
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Solid Background",
                         on ? theme::rgbText(222, 228, 242) : capOff,
                         on ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_cos_solid", tog, on, true, row1L)) {
            theme::solidBackground = !on;
            Mod::get()->setSavedValue("gui_solid_background", theme::solidBackground);
            syncThemeJson();
            skkbot2::Logger::get().info("skkBot2: solid background -> {}",
                                        theme::solidBackground ? "on" : "off");
        }
        yy += rowH + rowGap;
    }

    // Row 2 — Blur toggle. Independent of Solid (user 14.11): turning the blur
    // off in the LIGHT-glass mode just replaces the backdrop with the flat
    // opaque base tone. The flag persists on its own.
    {
        const bool on = theme::blurEnabled;
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Blur",
                         on ? theme::rgbText(222, 228, 242) : capOff,
                         on ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_cos_blur", tog, on, true, row2L)) {
            theme::blurEnabled = !on;
            Mod::get()->setSavedValue("gui_shell_blur", theme::blurEnabled);
            syncThemeJson();
            skkbot2::Logger::get().info("skkBot2: blur -> {}",
                                        theme::blurEnabled ? "on" : "off");
        }
        yy += rowH + rowGap;
    }

    // Row 3 — Window Animation: toggle + duration in ONE row (the toggle on
    // the right, the seconds field just left of it, visible only when on).
    // Sits right under Blur (user 14.11) — both are "shell look" switches.
    {
        const bool on = st.shellAnim;
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Window Animation",
                         on ? theme::rgbText(222, 228, 242) : capOff,
                         on ? theme::rgbText(240, 245, 255) : capOff);
        const float togW = 44.0f * k;
        const float fx = gridTL.x + width - togW - 10.0f * k - fieldW;
        const float fy = yy + (rowH - 34.0f * k) * 0.5f;
        if (numberField(mf, "##set_cos_anim_time", ImVec2(fx, fy), fieldW,
                        st.shellAnimSec, 0.05f, 0.60f, "s", "%.2f",
                        row3L && on, 0.01f)) {
            st.shellAnimSec = std::clamp(st.shellAnimSec, 0.05f, 0.60f);
            Mod::get()->setSavedValue("gui_shell_anim_sec", st.shellAnimSec);
        }
        ImVec2 tog(gridTL.x + width - togW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_cos_anim", tog, on, true, row3L)) {
            st.shellAnim = !on;
            Mod::get()->setSavedValue("gui_shell_anim", st.shellAnim);
            skkbot2::Logger::get().info("skkBot2: window animation -> {}",
                                        st.shellAnim ? "on" : "off");
        }
        yy += rowH + rowGap;
    }

    // Row 4 — Menu Font picker + open fonts folder button
    {
        if (st.fontsDirty || st.fontItems.empty()) {
            refreshFontList();
        }

        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Font",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));

        const float btnSize = 34.0f * k;
        const float gap = 8.0f * k;
        const float folderX = gridTL.x + width - btnSize;
        const float fx = folderX - gap - fieldW;
        const float fy = yy + (rowH - 34.0f * k) * 0.5f;

        // Determine current selection
        std::string curFont = Mod::get()->getSavedValue<std::string>("gui_custom_font", "");
        int curIndex = 0;
        if (!curFont.empty()) {
            for (size_t i = 0; i < st.fontItems.size(); ++i) {
                if (st.fontItems[i] == curFont) {
                    curIndex = static_cast<int>(i);
                    break;
                }
            }
        }
        const char* preview = (curIndex >= 0 && curIndex < static_cast<int>(st.fontItems.size()))
                                  ? st.fontItems[curIndex].c_str()
                                  : (curFont.empty() ? "Default" : curFont.c_str());

        const int pick = dropdownField(mf, "##set_cos_font", ImVec2(fx, fy), fieldW,
                                       preview, st.fontItems, curIndex, row4L);
        if (pick >= 0 && pick < static_cast<int>(st.fontItems.size())) {
            std::string chosen = st.fontItems[pick];
            if (chosen == "Default") {
                Mod::get()->setSavedValue("gui_custom_font", std::string(""));
            } else {
                Mod::get()->setSavedValue("gui_custom_font", chosen);
            }
            skkbot2::Logger::get().info("skkBot2: switched font to '{}'", chosen);
            ImGuiCocos::get().reload();
        }

        // Open fonts folder button
        const ImVec2 folderPos(folderX, yy + (rowH - btnSize) * 0.5f);
        const ImVec2 folderSize(btnSize, btnSize);
        ImGui::SetCursorScreenPos(folderPos);
        ImGui::InvisibleButton("##set_cos_fonts_folder", folderSize);
        const bool folderHover = row4L && ImGui::IsItemHovered();
        const bool folderDown = row4L && ImGui::IsItemActive();
        if (folderDown && !st.fontsFolderDownPrev) {
            openFolderInExplorer(skkbot2::paths::fonts());
            st.fontsDirty = true;
        }
        st.fontsFolderDownPrev = folderDown;

        const float fhov = anim01(ImGui::GetID("##set_cos_fonts_folder") ^ 0x464F,
                                  folderHover ? 1.0f : 0.0f, 14.0f);
        if (fhov > 0.004f) {
            draw->AddRectFilled(folderPos, add(folderPos, folderSize),
                                withAlpha(theme::AccentSoft, fhov), 28.0f * k);
        }
        draw->AddRect(folderPos, add(folderPos, folderSize),
                      mixColor(theme::rgb(80, 100, 140), theme::rgb(126, 156, 212), fhov),
                      28.0f * k);
        const ImU32 iconCol = folderHover ? theme::Accent : theme::rgb(136, 148, 170);
        drawFolderGlyph(draw, ImVec2(folderX + btnSize * 0.5f, yy + rowH * 0.5f), k, iconCol);
        if (folderHover) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        yy += rowH + rowGap;
    }

    // Row 5 — Text Size (% multiplier across the menu)
    {
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Font Size",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));

        const float fx = gridTL.x + width - fieldW;
        const float fy = yy + (rowH - 34.0f * k) * 0.5f;

        float pct = std::round(theme::fontScale * 100.0f);
        if (numberField(mf, "##set_cos_font_size", ImVec2(fx, fy), fieldW,
                        pct, 60.0f, 150.0f, "%", "%.0f", row5L, 5.0f)) {
            theme::fontScale = std::clamp(pct / 100.0f, 0.60f, 1.50f);
            Mod::get()->setSavedValue("gui_font_scale", theme::fontScale);
        }
        yy += rowH + rowGap;
    }

    // Row 6 — Accent Color: the accent colour as a HEX string (#RRGGBB), the
    // standard colour-wheel convention the user asked for (colorscheme.ru):
    // no abstract degrees — you type a colour, the theme derives the hue
    // rotation from the base azure so the accent lands EXACTLY on it.
    static std::string cosAccentHex;
    if (cosAccentHex.empty()) cosAccentHex = currentAccentHex();
    {
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Accent Color",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
        const float fx = gridTL.x + width - fieldW;
        const float fy = yy + (rowH - 34.0f * k) * 0.5f;
        hexFieldSync("##set_cos_accent", cosAccentHex.c_str());
        if (hexField(mf, "##set_cos_accent", ImVec2(fx, fy), fieldW, row6L)) {
            const std::string typed = betaHexFieldCommitted("##set_cos_accent");
            const std::string norm = applyAccentHex(typed);
            if (!norm.empty()) {
                cosAccentHex = norm;
            } else {
                pushToast(st, "Invalid colour (use #RRGGBB)", theme::Red);
            }
        }
        yy += rowH + rowGap;
    }

    // Row 7 — Color Sat: global saturation multiplier (100% = shipped).
    {
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Color Sat",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
        const float fx = gridTL.x + width - fieldW;
        const float fy = yy + (rowH - 34.0f * k) * 0.5f;
        float satPct = theme::tintSat * 100.0f;
        if (numberField(mf, "##set_cos_sat", ImVec2(fx, fy), fieldW,
                        satPct, 0.0f, 200.0f, "%", "%.0f", row7L, 5.0f)) {
            theme::tintSat = std::clamp(satPct / 100.0f, 0.0f, 2.0f);
            theme::refreshTint();
            Mod::get()->setSavedValue("gui_tint_sat", theme::tintSat);
            // The accent colour display follows the saturation knob.
            cosAccentHex = currentAccentHex();
            syncThemeJson();
        }
        yy += rowH + rowGap;
    }

    // Row 8 — Text Color: the text has its OWN colour, independent of the
    // accent. Empty = shipped neutral text; "Default" resets to that.
    static std::string cosTextHex =
        theme::textHex.empty() ? "#FFFFFF" : theme::textHex;
    {
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Text Color",
                         theme::rgbText(222, 228, 242),
                         theme::rgbText(240, 245, 255));
        const float fx = gridTL.x + width - fieldW;
        const float fy = yy + (rowH - 34.0f * k) * 0.5f;
        hexFieldSync("##set_cos_text", cosTextHex.c_str());
        if (hexField(mf, "##set_cos_text", ImVec2(fx, fy), fieldW, row8L)) {
            const std::string typed = betaHexFieldCommitted("##set_cos_text");
            // #FFFFFF = the shipped neutral text (clears the text tint).
            if (typed == "Default" || typed == "default") {
                theme::textHue = -1.0f;
                theme::textHex.clear();
                theme::refreshTint();
                Mod::get()->setSavedValue("gui_text_color", std::string(""));
                cosTextHex = "#FFFFFF";
                syncThemeJson();
            } else {
                const std::string norm = applyTextHex(typed);
                if (!norm.empty()) {
                    if (norm == "#FFFFFF") {
                        // White IS the neutral: clear the tint instead of
                        // applying a 0-hue floor (which would red-wash it).
                        theme::textHue = -1.0f;
                        theme::textHex.clear();
                        theme::refreshTint();
                        Mod::get()->setSavedValue("gui_text_color", std::string(""));
                        syncThemeJson();
                    }
                    cosTextHex = norm;
                } else {
                    pushToast(st, "Invalid colour (use #RRGGBB)", theme::Red);
                }
            }
        }
        yy += rowH + rowGap;
    }

    // Row 8 — Export / Import of the visual settings. Excluded by design
    // (user 14.11): Font, Font Size and the window-open animation — they are
    // machine/personal choices, not a shareable "skin". Export writes a
    // JSON file next to the mod's save dir; Import reads it back and
    // applies with a toast.
    {
        const float btnW = 108.0f * k, btnH = 30.0f * k, bGap = 10.0f * k;
        const float bx = gridTL.x + (width - (btnW * 2 + bGap)) * 0.5f;
        const float by = yy + 4.0f * k;
        auto miniButton = [&](const char* id, const char* label, float x) -> bool {
            const ImVec2 p(x, by);
            ImGui::SetCursorScreenPos(p);
            ImGui::InvisibleButton(id, ImVec2(btnW, btnH));
            const bool hov = row9L && ImGui::IsItemHovered();
            const float t = anim01(ImGui::GetID(id) ^ 0x4558, hov ? 1.0f : 0.0f, 14.0f);
            draw->AddRectFilled(p, add(p, ImVec2(btnW, btnH)),
                                mixColor(theme::rgb(44, 50, 66), theme::rgb(62, 72, 94), t), 28.0f * k);
            draw->AddRect(p, add(p, ImVec2(btnW, btnH)),
                          mixColor(theme::rgb(75, 85, 108), theme::rgb(115, 130, 162), t), 28.0f * k);
            const ImVec2 tsz2 = ImGui::CalcTextSize(label);
            draw->AddText(ImVec2(p.x + (btnW - tsz2.x) * 0.5f, p.y + (btnH - tsz2.y) * 0.5f),
                          theme::rgbText(215, 224, 240), label);
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            return row9L && ImGui::IsItemClicked(0);
        };
        if (miniButton("##cos_exp", "Export", bx)) {
            pushToast(st, writeVisualJson(theme::solidBackground, theme::blurEnabled,
                                          cosAccentHex, theme::textHex)
                                 ? "Saved to gui_theme.json"
                                 : "Could not write gui_theme.json",
                      theme::Accent);
        }
        if (miniButton("##cos_imp", "Import", bx + btnW + bGap)) {
            std::ifstream in(themeJsonPath());
            if (!in) {
                pushToast(st, "gui_theme.json not found", theme::Red);
            } else {
                std::string body((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                const VisualJson vis = parseVisualJson(body);
                bool any = false;
                if (vis.hasSolid) {
                    theme::solidBackground = vis.solid;
                    Mod::get()->setSavedValue("gui_solid_background", theme::solidBackground);
                    any = true;
                }
                if (vis.hasBlur) {
                    theme::blurEnabled = vis.blur;
                    Mod::get()->setSavedValue("gui_shell_blur", theme::blurEnabled);
                    any = true;
                }
                if (!vis.color.empty()) {
                    const std::string norm = applyAccentHex(vis.color);
                    if (!norm.empty()) {
                        cosAccentHex = norm;
                        any = true;
                    }
                }
                if (!vis.text.empty() && vis.text != "#FFFFFF") {
                    const std::string norm = applyTextHex(vis.text);
                    if (!norm.empty()) {
                        cosTextHex = norm;
                        any = true;
                    }
                }
                pushToast(st, any ? "Visual settings applied"
                                  : "No visual settings found",
                          any ? theme::Accent : theme::Red);
            }
        }
        yy += btnH + 10.0f * k;
    }

    draw->PopClipRect();
    // 27k (отступ заголовка) + вылет нижней полосы (5k) в высоту секции.
    return 27.0f * k + seen + openT * 5.0f * k + kSectionGap * k;
}

// ---------------------------------------------------------------------------
float drawLoggingSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                         float k, float openT, bool hideHeading = false) {
    auto& logger = skkbot2::Logger::get();
    ImFont* mf = mono();
    const float kt = uiTextScale();
    State& st = state();

    // Page-level heading — same style/scale as the ACCURACY / KEYBIND headings.
    // hideHeading: see drawKeybindSection.
    if (!hideHeading &&
        sectionHeading(draw, gridTL, width, top, "LOGGING", st.loggingOpen, nullptr,
                       k, 1.0f, "settings_logging")) {
        st.loggingOpen = !st.loggingOpen;
    }
    // Отступ заголовок -> первая строка: 27k, контракт SPEED (полоса File
    // Logging не должна наезжать на заголовок LOGGING).
    const float y = top + 27.0f * k;

    const float rowH = 40.0f * k;
    const float pillW = 100.0f * k;
    const float pillH = 34.0f * k;
    const float pillGap = 10.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "File").y
                           : ImGui::GetFontSize();
    const ImU32 capOff = theme::rgbText(179, 186, 204);

    // Two nested reveals, both eased exactly like NOCLIP (rate 11):
    //  - openT  — the whole LOGGING block opens/closes from its heading.
    //  - rowsT  — the Split + Level rows appear ONLY while the "File Logging"
    //    master is on. With the master off the block shows just the master row.
    // The model must match the ACTUAL row layout exactly (BUG-116): rows 1-4
    // each advance rowH+10k (File Logging, Step Trace, Page Heap, Step
    // Profiler), then the Split row advances rowH+10k and the Level row
    // occupies a bare rowH. The old constants undercounted the inter-row gaps
    // by 10k per state (20k with the master off), so the section returned less
    // height than its content and the stack's docY fell short: at max scroll
    // the viewport clip always ate the bottom ~10k of the last visible row
    // (Level pills / Step Profiler), which read as a dark field crowding the
    // buttons from below.
    const float baseH = rowH * 4.0f + 40.0f * k;           // rows 1-4 with their gaps
    const float extraH = rowH * 2.0f + 10.0f * k;          // split row + gap + level row
    const bool fileOn = logger.isFileLogging();
    const float rowsT = anim01(ImGui::GetID("##beta_log_rows"),
                               fileOn ? 1.0f : 0.0f, 11.0f);
    const float seen = openT * (baseH + rowsT * extraH);
    if (openT <= 0.001f || seen <= 0.001f) return 27.0f * k;  // heading only (last in stack: no trailing gap)

    // Полосы-выделение LOGGING: ТОЛЬКО строки 1-4 (File Logging → Step Trace →
    // Page Heap → Step Profiler), один слитный прогон; Split Record/Play и
    // Level полос не получают (пользователь 07.11). Session-зависимые строки
    // рисуются в общем клипе по `seen` — доезжающие отсекаются молча.
    // Активный канал — у мастера File Logging, пока он включён.
    // Клип тот же, что у контента: полосы скрытых строк не должны рисоваться
    // при сворачивании (см. MACRO BOT).
    {
        draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                           ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
        static const char* const lgIds[4] = {
            "##lg_hl_file", "##lg_hl_trace", "##lg_hl_ph", "##lg_hl_prof",
        };
        float tops[4], ho[4], av[4];
        for (int i = 0; i < 4; ++i) {
            const float ry = y + static_cast<float>(i) * (rowH + 10.0f * k);
            tops[i] = ry;
            const bool act = (i == 0 && fileOn);
            ho[i] = rowChannels(lgIds[i], act, gridTL.x, gridTL.x + width, ry, k,
                                av[i]);
        }
        drawStripRun(draw, gridTL.x, gridTL.x + width, k, tops, ho, av, 4,
                     true, true);
        draw->PopClipRect();
    }
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    // Thresholds shifted one row down vs the pre-Page-Heap layout: each gate
    // opens when its own row is 85% revealed inside the clip.
    const bool rowPH = seen >= (rowH * 3.0f + 30.0f * k) * 0.85f;
    // Сдвинуто на одну строку вниз: между Page Heap и Split встал Step Profiler.
    const bool rowProf = seen >= (rowH * 4.0f + 40.0f * k) * 0.85f;
    const bool row2L = seen >= (rowH * 5.0f + 50.0f * k) * 0.85f;
    const bool row3L = seen >= (rowH * 6.0f + 50.0f * k) * 0.85f;

    // Row 1 — File Logging master switch.
    float yy = y;
    {
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "File Logging",
                         fileOn ? theme::rgbText(222, 228, 242) : capOff,
                         fileOn ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_log_file", tog, fileOn, true, row1L)) {
            logger.setFileLogging(!fileOn);
            Mod::get()->setSavedValue("gui_file_logging", logger.isFileLogging());
            skkbot2::Logger::get().info("skkBot2: file logging -> {}",
                                        logger.isFileLogging() ? "on" : "off");
        }
        yy += rowH + 10.0f * k;
    }

    {
        const bool traceOn = Mod::get()->getSavedValue<bool>("gui_step_trace", false);
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Step Trace",
                         traceOn ? theme::rgbText(222, 228, 242) : capOff,
                         traceOn ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_step_trace", tog, traceOn, true,
                     seen >= (rowH + 10.0f * k + rowH) * 0.85f)) {
            Mod::get()->setSavedValue("gui_step_trace", !traceOn);
            skkbot2::Logger::get().info("Step Trace -> {}", !traceOn ? "on" : "off");
        }
        yy += rowH + 10.0f * k;
    }

    // Row 3 - Page Heap opt-in (BUG-109 debug tool). ONE-WAY per process:
    // once enabled the standard heap never comes back until the game
    // restarts, so the toggle only flips the SAVED key; the runtime picks
    // it up at the next session start and the log line says so.
    {
        const bool phOn = Mod::get()->getSavedValue<bool>("gui_page_heap", false);
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Page Heap",
                         phOn ? theme::rgbText(222, 228, 242) : capOff,
                         phOn ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_page_heap", tog, phOn, true, rowPH)) {
            Mod::get()->setSavedValue("gui_page_heap", !phOn);
            skkbot2::Logger::get().info(
                "skkBot2: page heap debug -> {}{}",
                !phOn ? "on" : "off",
                !phOn ? " (applies at the next session start; ONE-WAY until game restart)"
                      : " (already-enabled heaps stay until game restart)");
        }
        yy += rowH + 10.0f * k;
    }

    // Row 4 - Step Profiler opt-in (расследование холодного per-step при
    // высоком TPS). Фоновый сэмплер главного потока ~1ms; отчёт - топ RVA
    // GeometryDash.exe в лог при остановке сессии. Diagnostic-only, офлайн.
    {
        const bool spOn = Mod::get()->getSavedValue<bool>("gui_step_profiler", false);
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Step Profiler",
                         spOn ? theme::rgbText(222, 228, 242) : capOff,
                         spOn ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_step_profiler", tog, spOn, true, rowProf)) {
            Mod::get()->setSavedValue("gui_step_profiler", !spOn);
            skkbot2::Logger::get().info(
                "skkBot2: step profiler -> {} (applies at the next session start)",
                !spOn ? "on" : "off");
        }
        yy += rowH + 10.0f * k;
    }

    // Rows 2-3 — Split Record/Play + Level picker. These exist only while the
    // master is on; rowsT collapses them away (the clip shrinks with it), and
    // each control is gated on rowsT so a hidden row never steals clicks.
    if (rowsT > 0.001f) {
        // Row 2 — Split Record/Play logs.
        {
            const bool splitOn = logger.isSplitFiles();
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Split Record/Play",
                             splitOn ? theme::rgbText(222, 228, 242) : capOff,
                             splitOn ? theme::rgbText(240, 245, 255) : capOff);
            ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
            if (toggleAt("##set_log_split", tog, splitOn, true, row2L && rowsT > 0.85f)) {
                logger.setSplitFiles(!splitOn);
                Mod::get()->setSavedValue("gui_split_logs", logger.isSplitFiles());
                skkbot2::Logger::get().info("skkBot2: split logs -> {}",
                                            logger.isSplitFiles() ? "on" : "off");
            }
            yy += rowH + 10.0f * k;
        }

        // Row 3 — level picker: five mutually exclusive pills.
        {
            const float labelSize = pxSize(9.0f * kt);
            const float labelSpacing = 1.2f * kt;
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            const int curLevel = static_cast<int>(logger.getLevel());
            drawGradientText(draw, df, titleSize, tPos, "Level",
                             theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));

            static const char* const kLevelNames[] = { "DEBUG", "INFO", "WARN",
                                                       "ERROR", "NONE" };
            const int nLevels = 5;
            const float groupW = pillW * static_cast<float>(nLevels) +
                                 pillGap * static_cast<float>(nLevels - 1);
            const float gx = gridTL.x + width - groupW;
            const float by = yy + (rowH - pillH) * 0.5f;
            for (int i = 0; i < nLevels; ++i) {
                char id[32];
                std::snprintf(id, sizeof(id), "##set_log_lvl%d", i);
                const float px = gx + static_cast<float>(i) * (pillW + pillGap);
                if (selectionToggle(id, ImVec2(px, by), ImVec2(pillW, pillH), mf,
                                    labelSize, labelSpacing, kLevelNames[i],
                                    i == curLevel, true,
                                    row3L && rowsT > 0.85f)) {
                    logger.setLevel(static_cast<skkbot2::Logger::Level>(i));
                    Mod::get()->setSavedValue("gui_log_level", i);
                    skkbot2::Logger::get().info("skkBot2: log level -> {}", kLevelNames[i]);
                }
            }
        }
    }
    draw->PopClipRect();
    // 27k (отступ заголовка) + вылет нижней полосы (5k) в высоту секции.
    return 27.0f * k + seen + openT * 5.0f * k;
}

// ---------------------------------------------------------------------------
// RENDER tab — three peer sections (VIDEO / AUDIO / FILTER), exactly the shape
// of the SETTINGS tab (ACCURACY / KEYBIND / LOGGING): the first section's
// heading is the tab heading drawn by drawBody, the other two draw their own
// page-level headings below; each has its own open/close animation (rate 11)
// and stacks under the previous one's eased height.
//
// Settings state lives in State::render (a Renderer::RenderSettings mirror).
// loadRenderSettings() seeds it from the mod's saved values once; every edit
// persists back (saveRenderSettings) and is pushed to the live renderer
// (applyRenderSettings), so a render started later picks the GUI values up.
// The same saved keys the classic GUI used are reused, so settings carry over.
// ---------------------------------------------------------------------------

void loadRenderSettings() {
    auto* mod = Mod::get();
    State& st = state();
    auto& r = st.render;
    r.width = static_cast<unsigned>(
        std::clamp(mod->getSavedValue<int>("gui_render_width", 1920), 320, 7680));
    r.height = static_cast<unsigned>(
        std::clamp(mod->getSavedValue<int>("gui_render_height", 1080), 240, 4320));
    r.fps = std::clamp(mod->getSavedValue<int>("gui_render_fps", 60), 24, 240);
    r.bitrateMbps = std::clamp(mod->getSavedValue<int>("gui_render_bitrate", 50), 1, 300);
    r.audioEnabled = mod->getSavedValue<bool>("gui_render_audio", false);
    r.audioCodec = mod->getSavedValue<std::string>("gui_render_audiocodec", "aac");
    r.audioBitrate =
        std::clamp(mod->getSavedValue<int>("gui_render_audiobitrate", 320), 32, 512);
    // A saved volume of exactly 0 is treated as "unset/accidental": the wheel
    // bug scrubbed the sliders to 0.00 while the user scrolled the page, and a
    // 0-volume render is silent audio=447pk of nothing. 0 is not a meaningful
    // render volume (mute the game instead), so fall back to the 1.0 default.
    float musicVol = mod->getSavedValue<float>("gui_render_musicvol", 1.0f);
    if (musicVol <= 0.0f) musicVol = 1.0f;
    float sfxVol = mod->getSavedValue<float>("gui_render_sfxvol", 1.0f);
    if (sfxVol <= 0.0f) sfxVol = 1.0f;
    r.musicVolume = std::clamp(musicVol, 0.0f, 1.0f);
    r.sfxVolume = std::clamp(sfxVol, 0.0f, 1.0f);
    r.fadeInSeconds =
        std::clamp(mod->getSavedValue<double>("gui_render_fadein", 0.0), 0.0, 10.0);
    r.fadeOutSeconds =
        std::clamp(mod->getSavedValue<double>("gui_render_fadeout", 0.0), 0.0, 10.0);
    r.audioFadeInSec =
        std::clamp(mod->getSavedValue<double>("gui_render_afadein", 0.0), 0.0, 10.0);
    r.audioFadeOutSec =
        std::clamp(mod->getSavedValue<double>("gui_render_afadeout", 0.0), 0.0, 10.0);
    r.monitorEnabled = mod->getSavedValue<bool>("gui_render_monitor", false);
    st.renderEncoder = mod->getSavedValue<std::string>("gui_render_encoder", "");
}

void saveRenderSettings() {
    auto* mod = Mod::get();
    const State& st = state();
    const auto& r = st.render;
    mod->setSavedValue("gui_render_width", static_cast<int>(r.width));
    mod->setSavedValue("gui_render_height", static_cast<int>(r.height));
    mod->setSavedValue("gui_render_fps", r.fps);
    mod->setSavedValue("gui_render_bitrate", r.bitrateMbps);
    mod->setSavedValue("gui_render_audio", r.audioEnabled);
    mod->setSavedValue("gui_render_audiocodec", std::string(r.audioCodec));
    mod->setSavedValue("gui_render_audiobitrate", r.audioBitrate);
    mod->setSavedValue("gui_render_musicvol", r.musicVolume);
    mod->setSavedValue("gui_render_sfxvol", r.sfxVolume);
    mod->setSavedValue("gui_render_fadein", r.fadeInSeconds);
    mod->setSavedValue("gui_render_fadeout", r.fadeOutSeconds);
    mod->setSavedValue("gui_render_afadein", r.audioFadeInSec);
    mod->setSavedValue("gui_render_afadeout", r.audioFadeOutSec);
    mod->setSavedValue("gui_render_monitor", r.monitorEnabled);
    mod->setSavedValue("gui_render_encoder", st.renderEncoder);
}

void applyRenderSettings() {
    skkbot2::render::Renderer::get().applySettings(state().render);
    skkbot2::render::Renderer::get().setEncoder(state().renderEncoder);
}

// One-time RENDER tab init: probe the bundled FFmpeg (the encoder list stays
// empty until that succeeds) and, if the user never picked an encoder, auto-
// select the most basic GPU H.264 encoder available, falling back to software
// libx264. Done on first tab show, not at mod load, because loading the FFmpeg
// DLLs is heavy (~160 MB); a failed probe is not retried every frame.
void initRenderTab() {
    State& st = state();
    if (st.renderInit) return;
    st.renderInit = true;
    loadRenderSettings();

    auto& renderer = skkbot2::render::Renderer::get();
    if (!renderer.isFFmpegLoaded()) {
        const bool ok = renderer.loadFFmpeg();
        if (ok) {
            skkbot2::Logger::get().info("Render: FFmpeg loaded from {}, {} encoder(s) available",
                                        skk::pathToUtf8(renderer.ffmpegDir()),
                                        renderer.availableEncoders().size());
        } else {
            // Name the folder the user can act on, not the internal unpack tree
            // that is wiped on exit.
            skkbot2::Logger::get().warn(
                "Render: FFmpeg missing ({}). Put the 7 DLLs into {}",
                renderer.lastError(),
                skk::pathToUtf8(skkbot2::paths::ffmpeg()));
        }
    }
    if (st.renderEncoder.empty()) {
        const auto& encoders = renderer.availableEncoders();
        // libx264 first, deliberately. The GPU encoders are faster but they are
        // picked by hardware: h264_nvenc is missing without an NVIDIA card,
        // h264_amf without an AMD one, h264_qsv without Intel's, and which one
        // is there decides what a fresh install gets - so the same mod picked a
        // different encoder depending on the machine. libx264 is in the build on
        // every machine, so naming it first makes the default the same everywhere
        // and leaves the hardware encoders as what you choose on purpose.
        const char* kPref[] = { "libx264", "h264_nvenc", "h264_amf", "h264_qsv" };
        for (const char* want : kPref) {
            auto it = std::find(encoders.begin(), encoders.end(), want);
            if (it != encoders.end()) {
                st.renderEncoder = *it;
                break;
            }
        }
    }
    applyRenderSettings();
}

// ---------------------------------------------------------------------------
// VIDEO section: output resolution, frame rate, bitrate and the video encoder.
// Its heading is the RENDER tab heading (drawBody draws it), so this draws
// only the rows and returns their eased height — same contract as
// drawSettingsPage for the ACCURACY section.
// ---------------------------------------------------------------------------
float drawVideoSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                       float k, float openT) {
    auto& renderer = skkbot2::render::Renderer::get();
    ImFont* mf = mono();
    const float kt = uiTextScale();
    State& st = state();

    const float rowH = 40.0f * k;
    const float rowGap = 10.0f * k;
    const float fieldW = 160.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Resolution").y
                           : ImGui::GetFontSize();
    const ImU32 capOff = theme::rgbText(179, 186, 204);
    auto& gui = skkbot2::gui::get();
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;

    // Resolution presets (same set as the classic GUI).
    static const char* const kResNames[] = {
        "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160", "7680x4320"
    };
    static const int kResW[] = { 1280, 1600, 1920, 2560, 3840, 7680 };
    static const int kResH[] = { 720, 900, 1080, 1440, 2160, 4320 };
    static_assert(sizeof(kResW) == sizeof(kResH), "resolution tables must match");

    // Encoder list: empty until FFmpeg is probed. Fall back to a single
    // pseudo-entry so the field still shows the saved pick instead of nothing.
    const auto& encoders = renderer.availableEncoders();
    std::vector<std::string> encItems(encoders.begin(), encoders.end());
    if (encItems.empty()) {
        encItems.push_back(st.renderEncoder.empty() ? "libx264" : st.renderEncoder);
    }

    const float rowsH = rowH * 7.0f + rowGap * 6.0f;  // res+fps+bitrate+codec+fade in+fade out+hide menu
    const float seen = openT * rowsH;
    if (openT <= 0.001f || seen <= 0.001f) return kSectionGap * k;
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, top - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, top + seen + 12.0f * k), true);
    const bool rowL = seen >= rowsH * 0.85f;  // all rows unlock together at the end
    float y = top;

    // Полосы-выделение RENDER/VIDEO: ВСЕ 7 строк — один слитный прогон
    // (пользователь 07.11), от Resolution до Hide Menu While Rendering:
    // roundTop у Resolution, roundBottom у Hide Menu, квадратные швы между
    // строками, один рим на весь прогон. Заливка покадровая — hover каждой
    // строки пойман своим каналом; активный канал только у Toggle-строки
    // Hide Menu.
    {
        static const char* const vidIds[7] = {
            "##vid_hl_res", "##vid_hl_fps", "##vid_hl_br", "##vid_hl_codec",
            "##vid_hl_fi", "##vid_hl_fo", "##vid_hl_hm",
        };
        float tops[7], ho[7], ac[7];
        for (int i = 0; i < 7; ++i) {
            const float ry = top + static_cast<float>(i) * (rowH + rowGap);
            tops[i] = ry;
            ho[i] = rowChannels(
                vidIds[i], i == 6 && gui.renderHideMenu, gridTL.x,
                gridTL.x + width, ry, k, ac[i]);
        }
        drawStripRun(draw, gridTL.x, gridTL.x + width, k, tops, ho, ac, 7,
                     true, true);
    }

    // Row 1 — Resolution (dropdown of presets; a custom WxH shows as-is).
    {
        int curRes = -1;
        for (int i = 0; i < 6; ++i) {
            if (kResW[i] == static_cast<int>(st.render.width) &&
                kResH[i] == static_cast<int>(st.render.height)) {
                curRes = i;
                break;
            }
        }
        char preview[32];
        if (curRes >= 0) {
            std::snprintf(preview, sizeof(preview), "%s", kResNames[curRes]);
        } else {
            std::snprintf(preview, sizeof(preview), "%ux%u", st.render.width,
                          st.render.height);
        }
        std::vector<std::string> resItems(std::begin(kResNames), std::end(kResNames));
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Resolution",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
        const float fx = gridTL.x + width - fieldW;
        const float fy = y + (rowH - 34.0f * k) * 0.5f;
        const int pick = dropdownField(mf, "##ren_res", ImVec2(fx, fy), fieldW,
                                       preview, resItems, curRes, rowL);
        if (pick >= 0 && pick < 6) {
            st.render.width = static_cast<unsigned>(kResW[pick]);
            st.render.height = static_cast<unsigned>(kResH[pick]);
            saveRenderSettings();
            applyRenderSettings();
            skkbot2::Logger::get().info("Render: resolution -> {}x{}", kResW[pick],
                                        kResH[pick]);
        }
        y += rowH + rowGap;
    }

    // Row 2 — FPS.
    {
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "FPS",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
        float fps = static_cast<float>(st.render.fps);
        const float fx = gridTL.x + width - fieldW;
        const float fy = y + (rowH - 34.0f * k) * 0.5f;
        if (numberField(mf, "##ren_fps", ImVec2(fx, fy), fieldW, fps, 24.0f, 240.0f,
                        nullptr, "%.0f", rowL, 1.0f)) {
            st.render.fps = static_cast<int>(fps);
            saveRenderSettings();
            applyRenderSettings();
            skkbot2::Logger::get().info("Render: fps -> {}", st.render.fps);
        }
        y += rowH + rowGap;
    }

    // Row 3 — Bitrate (ABR target, Mbps).
    {
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Bitrate",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
        float br = static_cast<float>(st.render.bitrateMbps);
        const float fx = gridTL.x + width - fieldW;
        const float fy = y + (rowH - 34.0f * k) * 0.5f;
        if (numberField(mf, "##ren_bitrate", ImVec2(fx, fy), fieldW, br, 1.0f, 300.0f,
                        "Mbps", "%.0f", rowL, 5.0f)) {
            st.render.bitrateMbps = static_cast<int>(br);
            saveRenderSettings();
            applyRenderSettings();
            skkbot2::Logger::get().info("Render: bitrate -> {} Mbps",
                                        st.render.bitrateMbps);
        }
        y += rowH + rowGap;
    }

    // Row 4 — Codec (the video encoder, by name).
    {
        int curEnc = -1;
        for (size_t i = 0; i < encItems.size(); ++i) {
            if (encItems[i] == st.renderEncoder) {
                curEnc = static_cast<int>(i);
                break;
            }
        }
        const char* preview = curEnc >= 0 ? encItems[curEnc].c_str()
                                          : (st.renderEncoder.empty() ? "libx264"
                                                                      : st.renderEncoder.c_str());
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Codec",
                         curEnc >= 0 ? theme::rgbText(222, 228, 242) : capOff,
                         curEnc >= 0 ? theme::rgbText(240, 245, 255) : capOff);
        const float fx = gridTL.x + width - fieldW;
        const float fy = y + (rowH - 34.0f * k) * 0.5f;
        const int pick = dropdownField(mf, "##ren_codec", ImVec2(fx, fy), fieldW,
                                       preview, encItems, curEnc, rowL);
        if (pick >= 0 && pick < static_cast<int>(encItems.size())) {
            st.renderEncoder = encItems[pick];
            saveRenderSettings();
            applyRenderSettings();
            skkbot2::Logger::get().info("Render: encoder -> {}", st.renderEncoder);
        }
        y += rowH + rowGap;
    }

    // Row 5 — Fade In (seconds; 0 = off). Video fade: the scene RGB is
    // multiplied by a ramp baked into the Y/UV passes (renderer.cpp
    // onFrameDraw).
    {
        float v = static_cast<float>(st.render.fadeInSeconds);
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Fade In",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
        const float fx = gridTL.x + width - fieldW;
        const float fy = y + (rowH - 34.0f * k) * 0.5f;
        if (numberField(mf, "##ren_fadein", ImVec2(fx, fy), fieldW, v, 0.0f, 10.0f,
                        "s", "%.1f", rowL, 0.1f)) {
            st.render.fadeInSeconds = static_cast<double>(v);
            saveRenderSettings();
            applyRenderSettings();
            skkbot2::Logger::get().info("Render: fade in -> {:.1f}s", v);
        }
        y += rowH + rowGap;
    }

    // Row 6 — Fade Out (fades to black over the live trailer after the level
    // completes).
    {
        float v = static_cast<float>(st.render.fadeOutSeconds);
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Fade Out",
                         theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
        const float fx = gridTL.x + width - fieldW;
        const float fy = y + (rowH - 34.0f * k) * 0.5f;
        if (numberField(mf, "##ren_fadeout", ImVec2(fx, fy), fieldW, v, 0.0f, 10.0f,
                        "s", "%.1f", rowL, 0.1f)) {
            st.render.fadeOutSeconds = static_cast<double>(v);
            saveRenderSettings();
            applyRenderSettings();
            skkbot2::Logger::get().info("Render: fade out -> {:.1f}s", v);
        }
        // Advances y for the toggle row that follows.
        y += rowH + rowGap;
    }

    // Row 7 — Hide Menu While Rendering. The capture copies the level into its
    // own FBO before the menu is drawn, so the menu cannot reach the video and
    // only costs a full ImGui pass per frame at output resolution. Turning this
    // off restores the menu during the render (useful for watching progress
    // readouts) at the previous speed.
    {
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        const bool on = gui.renderHideMenu;
        drawGradientText(draw, df, titleSize, tPos, "Hide Menu While Rendering",
                         on ? theme::rgbText(222, 228, 242) : capOff,
                         on ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##ren_hidemenu", tog, on, true, true)) {
            gui.renderHideMenu = !gui.renderHideMenu;
            saveRenderSettings();
            skkbot2::Logger::get().info("Render: hide menu while rendering -> {}",
                                        gui.renderHideMenu ? "on" : "off");
        }
        y += rowH + rowGap;
    }
    draw->PopClipRect();
    // Нижний вылет полосы Hide Menu (5k за контент) выносим в высоту,
    // иначе клип скролла режет её при прокрутке в самый низ.
    return seen + openT * 5.0f * k + kSectionGap * k;
}

// ---------------------------------------------------------------------------
// AUDIO section: master capture toggle, music/SFX volumes, bitrate and codec.
// The volume/bitrate/codec rows exist ONLY while the master is on — the same
// nested reveal the LOGGING section uses for its File Logging master (rowsT on
// top of openT, rate 11), so toggling audio grows/shrinks the block live.
// Draws its own heading; returns heading + rows eased height.
// ---------------------------------------------------------------------------
float drawAudioSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                       float k, float openT) {
    auto& renderer = skkbot2::render::Renderer::get();
    ImFont* mf = mono();
    const float kt = uiTextScale();
    State& st = state();

    if (sectionHeading(draw, gridTL, width, top, "AUDIO", st.audioOpen, nullptr,
                       k, 1.0f, "render_audio")) {
        st.audioOpen = !st.audioOpen;
    }
    // Отступ заголовок -> первая строка: 27k, контракт SPEED — иначе полоса
    // Capture Audio наезжает на заголовок AUDIO.
    const float y = top + 27.0f * k;

    const float rowH = 40.0f * k;
    const float rowGap = 10.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float fieldW = 160.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Music").y
                           : ImGui::GetFontSize();
    const ImU32 capOff = theme::rgbText(179, 186, 204);

    // Audio codecs present in the bundled FFmpeg (empty until probed; fall
    // back to the saved pick so the field is never blank).
    static const char* const kAudioCodecs[] = {
        "aac", "libmp3lame", "libopus", "ac3", "libvorbis", "flac",
    };
    std::vector<std::string> acItems;
    for (const char* name : kAudioCodecs) {
        if (renderer.hasAudioCodec(name)) acItems.emplace_back(name);
    }
    if (acItems.empty()) {
        acItems.emplace_back(st.render.audioCodec.empty() ? "aac"
                                                          : st.render.audioCodec);
    }

    // Nested reveal: master row always, the 7 dependent rows only while on
    // (music/sfx volumes, bitrate, codec, audio fade in/out, monitor).
    const float baseH = rowH;
    const float extraH = rowGap + rowH * 7.0f + rowGap * 6.0f;  // gap + 7 rows
    const float rowsT = anim01(ImGui::GetID("##beta_audio_rows"),
                               st.render.audioEnabled ? 1.0f : 0.0f, 11.0f);
    const float seen = openT * (baseH + rowsT * extraH);
    if (openT <= 0.001f || seen <= 0.001f) return 27.0f * k;
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH + rowGap + rowH) * 0.85f;
    const bool row3L = seen >= (2.0f * rowH + 2.0f * rowGap + rowH) * 0.85f;
    const bool row4L = seen >= (3.0f * rowH + 3.0f * rowGap + rowH) * 0.85f;
    const bool row5L = seen >= (4.0f * rowH + 4.0f * rowGap + rowH) * 0.85f;
    const bool row6L = seen >= (5.0f * rowH + 5.0f * rowGap + rowH) * 0.85f;
    const bool row7L = seen >= (6.0f * rowH + 6.0f * rowGap + rowH) * 0.85f;
    const bool row8L = seen >= (7.0f * rowH + 7.0f * rowGap + rowH) * 0.85f;
    const bool depL = rowsT > 0.85f;  // dependent rows must be settled to click

    // Полосы-выделение AUDIO (пользователь 07.11): Capture Audio в
    // выключенном состоянии — ОДНА полоса полной капсулы; во включённом —
    // слитный ПРОГОН от Capture Audio до Fade Out (round оба конца, квадратные
    // швы между строками, один рим). Зависимые строки рисуются внутри общего
    // клипа по `seen` — ещё не доезжающие строки отсекаются клипом молча.
    {
        static const char* const audIds[8] = {
            "##aud_hl_cap", "##aud_hl_music", "##aud_hl_sfx",
            "##aud_hl_br", "##aud_hl_codec", "##aud_hl_fi", "##aud_hl_fo",
            "##aud_hl_mon",
        };
        float tops[8], ho[8], ac[8];
        for (int i = 0; i < 8; ++i) {
            const float ry = y + (rowH + rowGap) * static_cast<float>(i);
            tops[i] = ry;
            ho[i] = rowChannels(
                audIds[i], i == 0 && st.render.audioEnabled, gridTL.x,
                gridTL.x + width, ry, k, ac[i]);
        }
        if (rowsT > 0.85f) {
            drawStripRun(draw, gridTL.x, gridTL.x + width, k, tops, ho, ac, 8,
                         true, true);
        } else {
            drawRowStrip(draw, audIds[0], st.render.audioEnabled, gridTL.x,
                         gridTL.x + width, y, k, true, true);
        }
    }

    // Row 1 — Capture Audio master switch.
    float yy = y;
    {
        const bool on = st.render.audioEnabled;
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Capture Audio",
                         on ? theme::rgbText(222, 228, 242) : capOff,
                         on ? theme::rgbText(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##ren_audio", tog, on, true, row1L)) {
            st.render.audioEnabled = !on;
            saveRenderSettings();
            applyRenderSettings();
            skkbot2::Logger::get().info("Render: audio -> {}",
                                        st.render.audioEnabled ? "on" : "off");
        }
        yy += rowH + rowGap;
    }

    // Rows 2-5 — only while the master is on (clipped away by rowsT).
    if (rowsT > 0.001f) {
        // Row 2 — Music Volume.
        {
            float v = st.render.musicVolume;
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Music Volume",
                             theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
            const float fx = gridTL.x + width - fieldW;
            const float fy = yy + (rowH - 34.0f * k) * 0.5f;
            if (numberField(mf, "##ren_musicvol", ImVec2(fx, fy), fieldW, v, 0.0f, 1.0f,
                            nullptr, "%.2f", row2L && depL, 0.01f)) {
                st.render.musicVolume = v;
                saveRenderSettings();
                applyRenderSettings();
                skkbot2::Logger::get().info("Render: music volume -> {:.2f}", v);
            }
            yy += rowH + rowGap;
        }

        // Row 3 — SFX Volume.
        {
            float v = st.render.sfxVolume;
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "SFX Volume",
                             theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
            const float fx = gridTL.x + width - fieldW;
            const float fy = yy + (rowH - 34.0f * k) * 0.5f;
            if (numberField(mf, "##ren_sfxvol", ImVec2(fx, fy), fieldW, v, 0.0f, 1.0f,
                            nullptr, "%.2f", row3L && depL, 0.01f)) {
                st.render.sfxVolume = v;
                saveRenderSettings();
                applyRenderSettings();
                skkbot2::Logger::get().info("Render: sfx volume -> {:.2f}", v);
            }
            yy += rowH + rowGap;
        }

        // Row 4 — Bitrate (kbps).
        {
            float v = static_cast<float>(st.render.audioBitrate);
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Bitrate",
                             theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
            const float fx = gridTL.x + width - fieldW;
            const float fy = yy + (rowH - 34.0f * k) * 0.5f;
            if (numberField(mf, "##ren_abitrate", ImVec2(fx, fy), fieldW, v, 32.0f, 512.0f,
                            "kbps", "%.0f", row4L && depL, 32.0f)) {
                st.render.audioBitrate = static_cast<int>(v);
                saveRenderSettings();
                applyRenderSettings();
                skkbot2::Logger::get().info("Render: audio bitrate -> {} kbps",
                                            st.render.audioBitrate);
            }
            yy += rowH + rowGap;
        }

        // Row 5 — Codec.
        {
            int cur = -1;
            for (size_t i = 0; i < acItems.size(); ++i) {
                if (acItems[i] == st.render.audioCodec) {
                    cur = static_cast<int>(i);
                    break;
                }
            }
            const char* preview = cur >= 0
                                      ? acItems[cur].c_str()
                                      : (st.render.audioCodec.empty() ? "aac"
                                                                      : st.render.audioCodec.c_str());
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Codec",
                             theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
            const float fx = gridTL.x + width - fieldW;
            const float fy = yy + (rowH - 34.0f * k) * 0.5f;
            const int pick = dropdownField(mf, "##ren_acodec", ImVec2(fx, fy), fieldW,
                                           preview, acItems, cur, row5L && depL);
            if (pick >= 0 && pick < static_cast<int>(acItems.size())) {
                st.render.audioCodec = acItems[pick];
                saveRenderSettings();
                applyRenderSettings();
                skkbot2::Logger::get().info("Render: audio codec -> {}",
                                            st.render.audioCodec);
            }
            yy += rowH + rowGap;
        }

        // Row 6 — Audio Fade In (seconds; 0 = off). Ramps the captured mixer
        // output up over the start of the track — independent of the video
        // fade (renderer.cpp writeAudioBlock).
        {
            float v = static_cast<float>(st.render.audioFadeInSec);
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Fade In",
                             theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
            const float fx = gridTL.x + width - fieldW;
            const float fy = yy + (rowH - 34.0f * k) * 0.5f;
            if (numberField(mf, "##ren_afadein", ImVec2(fx, fy), fieldW, v, 0.0f, 10.0f,
                            "s", "%.1f", row6L && depL, 0.1f)) {
                st.render.audioFadeInSec = static_cast<double>(v);
                saveRenderSettings();
                applyRenderSettings();
                skkbot2::Logger::get().info("Render: audio fade in -> {:.1f}s", v);
            }
            yy += rowH + rowGap;
        }

        // Row 7 — Audio Fade Out (ramps to silence over the live trailer at
        // the end, mirroring the video fade-out).
        {
            float v = static_cast<float>(st.render.audioFadeOutSec);
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Fade Out",
                             theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
            const float fx = gridTL.x + width - fieldW;
            const float fy = yy + (rowH - 34.0f * k) * 0.5f;
            if (numberField(mf, "##ren_afadeout", ImVec2(fx, fy), fieldW, v, 0.0f, 10.0f,
                            "s", "%.1f", row7L && depL, 0.1f)) {
                st.render.audioFadeOutSec = static_cast<double>(v);
                saveRenderSettings();
                applyRenderSettings();
                skkbot2::Logger::get().info("Render: audio fade out -> {:.1f}s", v);
            }
            yy += rowH + rowGap;
        }

        // Row 8 — Monitor: play the render mix through the normal audio
        // device while the render runs (silicate parity). Silent when the
        // render is paused (matches the frozen video); independent of the
        // user's game volumes during the render.
        {
            const bool on = st.render.monitorEnabled;
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Monitor",
                             theme::rgbText(222, 228, 242), theme::rgbText(240, 245, 255));
            ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
            if (toggleAt("##ren_monitor", tog, on, true, row8L && depL)) {
                st.render.monitorEnabled = !on;
                saveRenderSettings();
                skkbot2::Logger::get().info("Render: monitor -> {}",
                                            st.render.monitorEnabled ? "on" : "off");
            }
        }
    }
    draw->PopClipRect();
    // Нижний вылет полосы Fade Out (5k за контент) выносим в высоту,
    // иначе клип скролла режет её при прокрутке в самый низ.
    return 27.0f * k + seen + openT * 5.0f * k;
}

// ---------------------------------------------------------------------------
// Body scroll: the SETTINGS / RENDER tabs stack collapsible sections, and with
// everything open the stack outgrows the body viewport (rows would slide
// under the footer and become unreachable). The tab heading stays pinned;
// everything below it scrolls inside a clip. The scroll offset lives in State
// (persists across frames, since the content height is only known AFTER the
// sections are drawn — this runs post-draw to clamp it, handle the wheel and
// paint the scrollbar).
//
// The slim thumb sits in the right padding gutter (the body's right pad is 32k,
// the track at max.x-14k), so it never overlaps the right-aligned controls.
// Wheel: numberField only steps its value while the field is being EDITED, so
// an idle field never eats the wheel and the page scrolls under it (that's how
// the page scrolled before and silently scrubbed Music/SFX Volume to 0).
// ---------------------------------------------------------------------------
void applyRegionScroll(ImDrawList* draw, ImVec2 winMin, ImVec2 winMax,
                       float viewTop, float viewBot, float contentH, float k,
                       float& scroll, bool& dragging, float& dragY,
                       float& dragStart, bool& click) {
    // v4 WINDOW ANIMATION: inert during the closing ghost frames.
    if (gShellClosing) {
        dragging = false;
        click = false;
        return;
    }
    const float viewH = std::max(1.0f, viewBot - viewTop);
    const float maxScroll = std::max(0.0f, contentH - viewH);
    if (maxScroll <= 0.5f) {
        scroll = 0.0f;  // everything fits: never an offset
        return;         // (a stale ease entry re-seeds when it scrolls again)
    }
    scroll = std::clamp(scroll, 0.0f, maxScroll);

    // v4 SMOOTH SCROLL (14.11): the wheel used to teleport the content by a
    // fixed 48k step per notch — on a tall page that reads as jumpy and makes
    // it hard to track a row mid-flight. The scroll float the layout uses is
    // now an EASED value chasing a stored target: the wheel moves the target
    // instantly (rapid notches accumulate, no input is lost), while the
    // content glides after it with an exponential tail. Direct manipulation
    // (thumb drag, track page-jump) bypasses the ease — the thumb must stay
    // glued to the pointer. State is keyed by the scroll variable's address,
    // which is stable across frames (a member of State/view per region).
    struct ScrollEase {
        float target;
        float anim;
        float w;  // v4: eased thumb width (idle 5k -> hovered/dragging 8k)
    };
    static std::unordered_map<const void*, ScrollEase> anims;
    auto it = anims.find(&scroll);
    if (it == anims.end()) {
        it = anims.emplace(&scroll, ScrollEase{scroll, scroll, 5.0f}).first;
    }
    ScrollEase& ease = it->second;
    if (dragging) {
        // Direct manipulation: no easing lag.
        ease.target = scroll;
        ease.anim = scroll;
    }

    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(ImVec2(winMin.x, viewTop),
                                   ImVec2(winMax.x, viewBot)) &&
        ImGui::GetIO().MouseWheel != 0.0f) {
        const float step = 48.0f * k;
        ease.target = std::clamp(ease.target - ImGui::GetIO().MouseWheel * step,
                                 0.0f, maxScroll);
        ImGui::GetIO().MouseWheel = 0.0f;
    }
    ease.target = std::clamp(ease.target, 0.0f, maxScroll);

    // Exponential ease with a frame-rate independent speed; snap when the
    // tail is sub-pixel so the scrollbar stops exactly on the target.
    float dtE = ImGui::GetIO().DeltaTime;
    if (dtE <= 0.0f) dtE = 1e-3f;
    if (dtE > 0.05f) dtE = 0.05f;
    ease.anim += (ease.target - ease.anim) *
                 (1.0f - std::exp(-14.0f * dtE));
    if (std::fabs(ease.target - ease.anim) < 0.5f) ease.anim = ease.target;
    scroll = std::clamp(ease.anim, 0.0f, maxScroll);

    const float trackX = winMax.x - 14.0f * k;
    const float trackY0 = viewTop;
    const float trackY1 = viewBot;
    const float trackH = trackY1 - trackY0;
    const float thumbH = std::max(16.0f * k, trackH * viewH / contentH);
    const float thumbTravel = std::max(1.0f, trackH - thumbH);
    float thumbY = trackY0 + (scroll / maxScroll) * thumbTravel;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // v4 THUMB GROW: the slim 5k thumb widens to 8k while hovered or dragged.
    // The right edge stays pinned, so it grows leftwards into the gutter. Hit
    // rect follows the eased width — the loop is damped (exp ease) and stable.
    float dtW = ImGui::GetIO().DeltaTime;
    if (dtW <= 0.0f) dtW = 1e-3f;
    if (dtW > 0.05f) dtW = 0.05f;
    const float hitPadX = 6.0f * k;
    {
        float probeX = winMax.x - 11.5f * k - 8.0f * k * 0.5f;  // widest rect for hit-test
        const bool hov = ImGui::IsMouseHoveringRect(
            ImVec2(probeX - hitPadX, thumbY),
            ImVec2(probeX + 8.0f * k + hitPadX, thumbY + thumbH));
        const float wTarget = (dragging || hov) ? 8.0f : 5.0f;
        ease.w += (wTarget * k - ease.w) * (1.0f - std::exp(-18.0f * dtW));
        if (std::fabs(wTarget * k - ease.w) < 0.05f) ease.w = wTarget * k;
    }
    const float thumbW = ease.w;
    // v4 ALIGN FIX (retest): the thumb used to grow leftwards only — its right
    // edge stayed pinned while the track stayed 5k, so a fat thumb hung off
    // the track's left side. Now it grows BOTH ways around the track's center
    // line, so hover/drag stays visually centered on the rail.
    const float thumbX = winMax.x - 11.5f * k - thumbW * 0.5f;
    const ImVec2 thumbMin(thumbX - hitPadX, thumbY);
    const ImVec2 thumbMax(thumbX + thumbW + hitPadX, thumbY + thumbH);
    const bool hoveredThumb = ImGui::IsMouseHoveringRect(thumbMin, thumbMax);
    const bool hoveredTrack = ImGui::IsMouseHoveringRect(
        ImVec2(thumbX - hitPadX, trackY0), ImVec2(thumbX + thumbW + hitPadX, trackY1));
    if (ImGui::IsMouseClicked(0)) {
        if (hoveredThumb) {
            dragging = true;
            dragY = mouse.y;
            dragStart = scroll;
            click = true;
        } else if (hoveredTrack) {
            // Click on the track outside the thumb: page-jump towards the mouse.
            if (mouse.y < thumbY) {
                scroll = std::max(0.0f, scroll - viewH);
            } else {
                scroll = std::min(maxScroll, scroll + viewH);
            }
            // Direct manipulation wins over the ease this frame too.
            ease.target = scroll;
            ease.anim = scroll;
            click = true;
        }
    }
    if (dragging) {
        click = true;
        if (ImGui::IsMouseDown(0)) {
            const float dy = mouse.y - dragY;
            scroll =
                std::clamp(dragStart + (dy / thumbTravel) * maxScroll,
                           0.0f, maxScroll);
            // Direct manipulation wins over the ease this frame too.
            ease.target = scroll;
            ease.anim = scroll;
            thumbY = trackY0 + (scroll / maxScroll) * thumbTravel;
        } else {
            dragging = false;
        }
    }

    const bool thumbActive = dragging || hoveredThumb;
    draw->AddRectFilled(ImVec2(winMax.x - 9.0f * k - 5.0f * k, trackY0),
                        ImVec2(winMax.x - 9.0f * k, trackY1),
                        theme::rgb(24, 27, 34, 130), 3.0f * k);
    const ImU32 thumbCol = thumbActive ? theme::Accent : theme::rgb(140, 153, 182);
    draw->AddRectFilled(ImVec2(thumbX, thumbY),
                        ImVec2(thumbX + thumbW, thumbY + thumbH),
                        thumbActive ? thumbCol : withAlpha(thumbCol, 200), 3.0f * k);
    if (thumbActive) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

void applyBodyScroll(ImDrawList* draw, ImVec2 winMin, ImVec2 winMax,
                     float viewTop, float viewBot, float contentH, float k) {
    State& st = state();
    applyRegionScroll(draw, winMin, winMax, viewTop, viewBot, contentH, k,
                      st.bodyScroll, st.bodyScrollDragging, st.bodyScrollDragY,
                      st.bodyScrollDragStart, st.bodyScrollClick);
}

// RECORD tab: the HITBOXES stack scrolls in the left column only
// (the NOCLIP column on the right keeps its own fixed layout). Same mechanic
// as applyBodyScroll, minus the section stack — a flat block of rows.
// RECORD left column below the timing divider (TPS / Speed / HITBOXES /
// LAYOUT MODE): scrolls as one run.
void applyTimingScroll(ImDrawList* draw, ImVec2 winMin, ImVec2 winMax,
                       float viewTop, float viewBot, float contentH, float k) {
    if (state().searchVisible) {
        const float viewH = std::max(1.0f, viewBot - viewTop);
        const float maxScroll = std::max(0.0f, contentH - viewH);
        state().timingScroll = std::clamp(state().timingScroll, 0.0f, maxScroll);
        state().timingScrollDragging = false;
        return;
    }
    State& st = state();
    applyRegionScroll(draw, winMin, winMax, viewTop, viewBot, contentH, k,
                      st.timingScroll, st.timingScrollDragging,
                      st.timingScrollDragY, st.timingScrollDragStart,
                      st.timingScrollClick);
}

// RECORD right column (the NOCLIP stack): its own independent scroll.
void applyNoClipScroll(ImDrawList* draw, ImVec2 winMin, ImVec2 winMax,
                       float viewTop, float viewBot, float contentH, float k) {
    if (state().searchVisible) {
        const float viewH = std::max(1.0f, viewBot - viewTop);
        const float maxScroll = std::max(0.0f, contentH - viewH);
        state().noclipScroll = std::clamp(state().noclipScroll, 0.0f, maxScroll);
        state().noclipScrollDragging = false;
        return;
    }
    State& st = state();
    applyRegionScroll(draw, winMin, winMax, viewTop, viewBot, contentH, k,
                      st.noclipScroll, st.noclipScrollDragging,
                      st.noclipScrollDragY, st.noclipScrollDragStart,
                      st.noclipScrollClick);
}

// ---------------------------------------------------------------------------
// RENDER action button: the pinned START / STOP control at the bottom of the
// RENDER tab. It mirrors the classic GUI's rules exactly — the label, colour
// and behaviour all follow the live renderer state:
//   - recording   -> "STOP RENDER" (coral), always enabled, stop() on click;
//   - not recording -> "START RENDER" (accent), enabled only when a render can
//     actually begin (a level is open, the engine is playing back a macro and
//     FFmpeg is loaded). Hovering a disabled button explains what's missing.
// The button is drawn OUTSIDE the scroll clip, pinned just above the footer, so
// it never scrolls away — the scroll viewport above is shortened to make room.
// ---------------------------------------------------------------------------
void drawRenderAction(ImDrawList* draw, ImVec2 pos, float width, float k) {
    auto& renderer = skkbot2::render::Renderer::get();
    auto& dl = skkbot2::render::FFmpegDownloader::get();
    auto* engine = ReplayEngine::get();

    const bool recording = renderer.isRecording();
    const bool ffLoaded = renderer.isFFmpegLoaded();
    const bool isDlBusy = dl.isBusy();
    const bool dllsOnDisk = skkbot2::render::FFmpegDownloader::areDllsPresent();
    const bool needsFfmpeg = !recording && !ffLoaded;

    auto& renderGui = skkbot2::gui::get();
    auto& skcBot = skk::skc::Bot::get();
    const bool skcPlaying =
        renderGui.macroBotType == Gui::MacroBotType::SKC &&
        (skcBot.getState() == skk::skc::State::Playback || skcBot.isPlaybackArmed());
    const bool skkPlaying = engine->engineMode == MODE_EXECUTE && engine->activeSk;
    const bool inPlay = PlayLayer::get() && PlayLayer::get()->m_level &&
                        (skkPlaying || skcPlaying);
    const bool canStart = !recording && ffLoaded && inPlay;

    const float btnH = 40.0f * k;

    // Label determination
    std::string dynamicLabel;
    if (isDlBusy) {
        if (dl.state() == skkbot2::render::FFmpegDownloader::State::TestingSpeed) {
            dynamicLabel = "CHECKING MIRRORS...";
        } else if (dl.state() == skkbot2::render::FFmpegDownloader::State::Downloading) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "DOWNLOADING %s (%.0f%%)",
                          dl.activeMirrorName().c_str(), dl.progress() * 100.0f);
            dynamicLabel = buf;
        } else {
            dynamicLabel = "EXTRACTING DLLS...";
        }
    } else if (needsFfmpeg) {
        if (dllsOnDisk) {
            dynamicLabel = "RELOAD FFMPEG";
        } else {
            dynamicLabel = "DOWNLOAD FFMPEG";
        }
    } else if (recording) {
        dynamicLabel = "STOP RENDER";
    } else {
        dynamicLabel = "START RENDER";
    }

    const char* label = dynamicLabel.c_str();
    const bool enabled = recording || (needsFfmpeg && !isDlBusy) || canStart;
    const ImU32 fill = recording ? theme::Record : (isDlBusy ? theme::rgb(70, 85, 115) : theme::Accent);
    const ImU32 onFill = recording ? theme::rgb(255, 175, 155) : theme::AccentLight;

    const float sideSize = btnH;
    const float sideGap = 8.0f * k;
    const ImVec2 size(width - sideSize - sideGap, btnH);
    // v4: while a capture is running the action button breathes (slow coral
    // halo, same 2.4 rad/s rhythm as the footer status dot) — a long render is
    // visible at a glance and the STOP affordance stands out.
    if (recording) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.4f);
        drawSoftHalo(draw, pos, add(pos, size), btnH * 0.5f, theme::Record,
                     0.5f + 0.4f * pulse, k);
    }
    const bool fired = buttonPrimary("##beta_render_action", pos, size, label,
                                     nullptr, enabled, fill, onFill);
    if (fired) {
        if (recording) {
            skkbot2::gui::stopRender();
        } else if (needsFfmpeg && !isDlBusy) {
            if (dllsOnDisk) {
                renderer.reloadFFmpeg();
                const bool ok = renderer.loadFFmpeg();
                skkbot2::Logger::get().info(
                    "Render: FFmpeg reload -> {}, {} encoder(s){}",
                    ok ? "loaded" : "still missing",
                    renderer.availableEncoders().size(),
                    ok ? "" : ", see " + skk::pathToUtf8(render::Renderer::preferredFfmpegDir()));
            } else {
                dl.startDownload([](bool ok) {
                    if (ok) {
                        auto& r = skkbot2::render::Renderer::get();
                        r.reloadFFmpeg();
                        r.loadFFmpeg();
                    }
                });
            }
        } else {
            skkbot2::gui::startRender();
        }
    }

    // Square folder button
    {
        const ImVec2 sidePos(pos.x + width - sideSize, pos.y);
        const ImVec2 sideEnd = add(sidePos, ImVec2(sideSize, sideSize));

        ImGui::SetCursorScreenPos(sidePos);
        ImGui::InvisibleButton("##beta_open_videos", ImVec2(sideSize, sideSize));
        const bool sideHover = ImGui::IsItemHovered();
        if (sideHover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] &&
            ImGui::IsMouseHoveringRect(sidePos, sideEnd)) {
            if (needsFfmpeg) {
                render::Renderer::prepareFfmpegDropIn();
                openFolderInExplorer(skkbot2::paths::ffmpeg());
            } else {
                openFolderInExplorer(skkbot2::paths::videos());
            }
        }

        const float wash = anim01(ImGui::GetID("##beta_open_videos") ^ 0x4C47,
                                  sideHover ? 1.0f : 0.0f, 14.0f);
        draw->AddRectFilled(sidePos, sideEnd,
                            mixColor(theme::rgb(42, 56, 82, 210),
                                     theme::rgb(62, 80, 110, 225), wash),
                            28.0f * k);
        draw->AddRect(sidePos, sideEnd,
                      mixColor(theme::rgb(96, 128, 190), theme::rgb(146, 178, 240), wash),
                      28.0f * k, 0, 1.0f);
        drawFolderGlyph(draw, ImVec2(sidePos.x + sideSize * 0.5f,
                                     sidePos.y + sideSize * 0.5f), k,
                        sideHover ? theme::rgbText(226, 232, 244) : theme::rgb(185, 195, 215));
    }

    // Manual hit-test backup
    const bool pressInRect = enabled
        && ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left]
        && ImGui::IsMouseHoveringRect(pos, add(pos, size));
    if (pressInRect) {
        state().actionBtnClick = true;
        if (!fired && !ImGui::IsAnyItemActive()) {
            if (recording) {
                skkbot2::gui::stopRender();
            } else if (needsFfmpeg && !isDlBusy) {
                if (dllsOnDisk) {
                    renderer.reloadFFmpeg();
                    renderer.loadFFmpeg();
                } else {
                    dl.startDownload([](bool ok) {
                        if (ok) {
                            auto& r = skkbot2::render::Renderer::get();
                            r.reloadFFmpeg();
                            r.loadFFmpeg();
                        }
                    });
                }
            } else {
                skkbot2::gui::startRender();
            }
        }
    }
}

void drawBody(ImDrawList* draw, ImVec2 min, ImVec2 max, float k) {
    const float left = min.x + kBodyPadX * k;
    const float width = (max.x - min.x) - kBodyPadX * 2.0f * k;

    const char* head = "";
    bool* open = nullptr;
    bool drawPageHeading = true;
    switch (state().tab) {
        case Tab::Record: head = "RECORD & PLAY"; open = &state().recordOpen; break;
        case Tab::Assist:
            // The ASSIST tab is a single always-open page (its WAVE TRAIL FIX
            // section owns its own heading), so no collapsible page heading.
            drawPageHeading = false;
            break;
        case Tab::Render: head = "VIDEO"; open = &state().renderOpen; break;
        // Settings draws its pinned heading INSIDE the branch below: the sections
        // scroll under it, so the label has to become whichever section currently
        // sits at the top (ACCURACY -> COSMETICS -> LOGGING) instead of
        // staying "ACCURACY" and overlapping the text scrolling beneath it.
        // The keybind screen keeps its own fixed "KEYBINDS" title drawn below.
        case Tab::Settings: drawPageHeading = false; break;
    }

    const float y = min.y + kBodyPadTop * k;
    if (drawPageHeading && sectionHeading(draw, ImVec2(left, 0), width, y, head, *open,
                                          nullptr, k)) {
        *open = !*open;
    }

    // The Settings tab stacks three COLLAPSIBLE SECTIONS — ACCURACY, COSMETICS
    // and LOGGING — each an equal peer: its own page-level heading (always
    // visible and clickable) and its own open/close animation eased at the
    // same rate 11. They are NOT nested: LOGGING stays reachable with ACCURACY
    // closed, and every section's top sits at the previous one's eased bottom, so
    // opening/closing one slides the ones below it.
    // KEYBIND used to be a fourth section here; it now lives on its own screen
    // (state().keybindScreen) reached from the pinned KEYBINDS button below.
    if (state().tab == Tab::Settings && !state().keybindScreen) {
        const float viewTop = y + 20.0f * k;
        // A pinned KEYBINDS button sits at the bottom of the tab (above the
        // footer), mirroring the RENDER tab's START/STOP placement, so it is
        // always reachable and rows never slide underneath it.
        const float btnH = 40.0f * k;
        const float btnGap = 12.0f * k;
        const float viewBot = max.y - kBodyPadBottom * k - btnH - btnGap;

        // ACCURACY rows — the "ACCURACY" heading above is this tab's page
        // heading, drawn by drawBody. Only its rows ease in/out.
        const float setT = anim01(ImGui::GetID("##beta_settings_open"),
                                  state().settingsOpen ? 1.0f : 0.0f, 11.0f);
        // The whole section stack scrolls inside the body viewport (the tab
        // heading stays pinned above), so every section is drawn shifted up by
        // the persistent scroll offset and clipped to the viewport.
        float curY = viewTop - state().bodyScroll;
        // This Push pairs with the single Pop at the end of the stack below.
        // Losing it left a Pop with nothing to pop, which corrupted ImGui's clip
        // stack once per frame and wrecked the whole page. Kept 16k of slack
        // above viewTop so a heading sliding under the pinned label fades out
        // through the clip instead of being cut hard.
        draw->PushClipRect(ImVec2(left - width * 0.08f, viewTop - 16.0f * k),
                           ImVec2(left + width * 1.08f, viewBot), true);

        // Sticky-label bookkeeping. The page heading is PINNED at the top while
        // the sections scroll underneath it, so a fixed "ACCURACY" label ends up
        // sitting on top of KEYBIND/COSMETICS/LOGGING text. Instead we remember
        // each section's heading position in UNSCROLLED space and, after the
        // stack is drawn, paint the pinned heading with whichever section has
        // scrolled up to the top. Drawing it LAST puts it above the clip, so it
        // also hides that section's own in-flow heading at the same spot instead
        // of doubling it up.
        struct StickySection {
            const char* label;
            bool* open;
            float top;  // unscrolled y of this section's heading
        };
        // Four sections now: MACRO BOT, ACCURACY, COSMETICS, LOGGING.
        StickySection sticky[4];
        int stickyCount = 0;
        float docY = 0.0f;

        const float headH = 20.0f * k;
        auto headingHidden = [&](float cy) {
            return cy > y - headH && cy < y + headH;
        };

        // MACRO BOT — format selection (SKK vs SKC)
        const float botT = anim01(ImGui::GetID("##beta_macrobot_open"),
                                  state().macroBotOpen ? 1.0f : 0.0f, 11.0f);
        sticky[stickyCount++] = { "MACRO BOT", &state().macroBotOpen, docY };
        curY += drawMacroBotSection(draw, ImVec2(left, curY), width, curY, viewBot,
                                    k, botT);
        docY = curY + state().bodyScroll - viewTop;

        // ACCURACY — precision mode (Vanilla vs CBS)
        sticky[stickyCount++] = { "ACCURACY", &state().settingsOpen, docY };
        curY += drawSettingsPage(draw, ImVec2(left, curY), width, curY, viewBot,
                                 k, setT, headingHidden(curY));
        docY = curY + state().bodyScroll - viewTop;

        // COSMETICS — own heading + rows (above LOGGING).
        const float cosT = anim01(ImGui::GetID("##beta_cosmetics_open"),
                                  state().cosmeticsOpen ? 1.0f : 0.0f, 11.0f);
        sticky[stickyCount++] = { "COSMETICS", &state().cosmeticsOpen, docY };
        curY += drawCosmeticsSection(draw, ImVec2(left, curY), width, curY, k, cosT,
                                     headingHidden(curY));
        docY = curY + state().bodyScroll - viewTop;

        // LOGGING — own heading + rows (with its nested File Logging reveal).
        const float lgT = anim01(ImGui::GetID("##beta_logging_open"),
                                  state().loggingOpen ? 1.0f : 0.0f, 11.0f);
        sticky[stickyCount++] = { "LOGGING", &state().loggingOpen, docY };
        curY += drawLoggingSection(draw, ImVec2(left, curY), width, curY, k, lgT,
                                   headingHidden(curY));
        docY = curY + state().bodyScroll - viewTop;
        draw->PopClipRect();

        // Pick the section the pinned heading should speak for: the LAST one whose
        // heading has been scrolled past the viewport top, i.e. whose document
        // offset has been reached by bodyScroll. At scroll 0 that is ACCURACY (docY
        // 0), so the initial state is unchanged; scrolling down hands the label to
        // COSMETICS and then LOGGING, and scrolling back returns it.
        const StickySection* active = &sticky[0];
        for (int i = 0; i < stickyCount; ++i) {
            if (sticky[i].top <= state().bodyScroll + 1.0f) {
                active = &sticky[i];
            }
        }
        // Unique idStr: sectionHeading builds its ImGui ID from idStr, and the
        // default "main" is shared with every other page heading in the GUI.
        if (sectionHeading(draw, ImVec2(left, 0), width, y, active->label, *active->open,
                           nullptr, k, 1.0f, "settings_sticky")) {
            *active->open = !*active->open;
            if (!*active->open && active == &sticky[0]) {
                state().bodyScroll = 0.0f;
            }
        }

        // Scroll runs post-draw: the eased stack height (incl. the last section)
        // is only known now, so the clamp + wheel + scrollbar happen here.
        // docY is that height by construction (curY + bodyScroll - viewTop).
        applyBodyScroll(draw, min, max, viewTop, viewBot, docY, k);

        // Pinned KEYBINDS button, styled after the RENDER tab's START/STOP
        // action button (same buttonPrimary call, same accent fill, full body
        // width, pinned above the footer). Clicking it pushes the keybind
        // screen over this tab.
        if (buttonPrimary("##beta_open_keybinds", ImVec2(left, viewBot + btnGap),
                          ImVec2(width, btnH), "KEYBINDS", nullptr, true,
                          theme::Accent, theme::AccentLight)) {
            state().keybindScreen = true;
            // Open on a clean scroll: returning to this tab later must not land
            // the user mid-list, and the shared offset would otherwise still be
            // carrying the settings stack's position.
            state().bodyScroll = 0.0f;
            state().keybindOpen = true;
        }
        // Manual hit-test backup, same reason as drawRenderAction's (BUG-20):
        // on the first click after the window appears or after a drag this
        // fork's imgui can fail to deliver the InvisibleButton, and the drag
        // gate then turns the press into a window grab instead. The rect test
        // suppresses the drag gate whenever the press lands on the button.
        {
            const ImVec2 bp(left, viewBot + btnGap);
            const ImVec2 bs(width, btnH);
            if (ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] &&
                ImGui::IsMouseHoveringRect(bp, add(bp, bs)) &&
                !state().keybindScreen) {
                state().keybindScreen = true;
                state().bodyScroll = 0.0f;
                state().keybindOpen = true;
            }
        }
        return;
    }

    // The KEYBINDS screen, pushed over the Settings tab. Same scroll contract as
    // every other body list: the stack shifts up by the persistent offset and is
    // clipped to the viewport. The pinned BACK button below shortens the
    // viewport so rows never slide under it.
    if (state().tab == Tab::Settings && state().keybindScreen) {
        const float viewTop = y + 20.0f * k;
        const float btnH = 40.0f * k;
        const float btnGap = 12.0f * k;
        const float viewBot = max.y - kBodyPadBottom * k - btnH - btnGap;

        // Fixed title: nothing scrolls under it here (the screen is a single
        // section), so the plain page-heading slot works, driven by keybindOpen.
        if (sectionHeading(draw, ImVec2(left, 0), width, y, "KEYBINDS",
                           state().keybindOpen, nullptr, k, 1.0f,
                           "settings_keybinds_page")) {
            state().keybindOpen = !state().keybindOpen;
        }

        const float kbT = anim01(ImGui::GetID("##beta_keybind_open"),
                                 state().keybindOpen ? 1.0f : 0.0f, 11.0f);
        float curY = viewTop - state().bodyScroll;
        draw->PushClipRect(ImVec2(left - 16.0f * k, viewTop - 16.0f * k),
                           ImVec2(max.x + 16.0f * k, viewBot), true);
        curY += drawKeybindSection(draw, ImVec2(left, curY), width, curY, k, kbT);
        draw->PopClipRect();

        applyBodyScroll(draw, min, max, viewTop, viewBot,
                        (curY + state().bodyScroll) - viewTop, k);

        // BACK returns to Settings proper. Same accent pill as KEYBINDS so the
        // pair reads as a drill-in/drill-out, just mirrored to the left.
        if (buttonPrimary("##beta_back_keybinds", ImVec2(left, viewBot + btnGap),
                          ImVec2(width, btnH), "BACK", nullptr, true,
                          theme::Accent, theme::AccentLight)) {
            state().keybindScreen = false;
            state().bodyScroll = 0.0f;
        }
        {
            const ImVec2 bp(left, viewBot + btnGap);
            const ImVec2 bs(width, btnH);
            if (ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] &&
                ImGui::IsMouseHoveringRect(bp, add(bp, bs)) &&
                state().keybindScreen) {
                state().keybindScreen = false;
                state().bodyScroll = 0.0f;
            }
        }
        return;
    }

    // The Render tab stacks two peer sections — VIDEO, AUDIO — the same shape
    // as the Settings tab: VIDEO's heading is the tab heading above, AUDIO
    // draws its own below; each has its own open/close
    // animation (rate 11) and stacks under the previous one's eased height.
    if (state().tab == Tab::Render) {
        initRenderTab();  // one-time: probe FFmpeg, load saved settings

        // Отступ заголовок -> первая строка: 27k, контракт SPEED — иначе
        // полоса Resolution наезжает на заголовок VIDEO.
        const float viewTop = y + 27.0f * k;
        // A pinned START/STOP action button sits at the bottom of the tab
        // (above the footer), always reachable; the scroll viewport ends above
        // it so rows never slide underneath the button.
        const float btnH = 40.0f * k;
        const float btnGap = 12.0f * k;
        const float viewBot = max.y - kBodyPadBottom * k - btnH - btnGap;
        const float vidT = anim01(ImGui::GetID("##beta_video_open"),
                                  state().renderOpen ? 1.0f : 0.0f, 11.0f);
        // Same scroll contract as SETTINGS: the stack shifts up by the
        // persistent offset and is clipped to the body viewport.
        float curY = viewTop - state().bodyScroll;
        draw->PushClipRect(ImVec2(left - 16.0f * k, viewTop - 16.0f * k),
                           ImVec2(max.x + 16.0f * k, viewBot), true);
        curY += drawVideoSection(draw, ImVec2(left, curY), width, curY, k, vidT);

        const float audT = anim01(ImGui::GetID("##beta_audio_open"),
                                  state().audioOpen ? 1.0f : 0.0f, 11.0f);
        curY += drawAudioSection(draw, ImVec2(left, curY), width, curY, k, audT);
        draw->PopClipRect();

        applyBodyScroll(draw, min, max, viewTop, viewBot,
                        (curY + state().bodyScroll) - viewTop, k);

        // Pinned action button below the scroll viewport, full body width.
        drawRenderAction(draw, ImVec2(left, viewBot + btnGap), width, k);
        return;
    }

    // The ASSIST tab has no page-level heading (always open), so it dispatches
    // here before the *open gate.
    if (state().tab == Tab::Assist) {
        // No page-level heading on this tab, so the WAVE TRAIL FIX section
        // starts at the body's top edge — same slot the "ACCURACY" heading
        // occupies on the Settings tab.
        const float bodyBot = max.y - kBodyPadBottom * k;
        const float wtT = anim01(ImGui::GetID("##beta_wavetrail_open"),
                                 state().waveTrailOpen ? 1.0f : 0.0f, 11.0f);
        drawWaveTrailSection(draw, ImVec2(min.x + kBodyPadX * k, y), width,
                             y, bodyBot, k, wtT);
        return;
    }

    if (!*open) return;

    const float gridTop = y + 20.0f * k;
    const float bodyBot = max.y - kBodyPadBottom * k;
    switch (state().tab) {
        case Tab::Record:
            drawRecordPage(draw, ImVec2(min.x + kBodyPadX * k, gridTop), width,
                           gridTop, bodyBot, k);
            break;
        case Tab::Assist:
            break;
        case Tab::Render:
            // Handled above: the RENDER tab owns its own per-section open
            // animations, so it is dispatched right after the heading.
            break;
        case Tab::Settings:
            // Handled above: the ACCURACY page owns its own open animation,
            // so it is dispatched right after the heading, not here.
            break;
    }
}

// Search results dropdown (library filter). Drawn last, above the footer, so
// its rows win the hit-test over everything drawn earlier in the window.
std::string macroSmallInfo(const char* entry) {
    // `entry` is a "<stem> [<ext>]" key, so the size shown always belongs to the
    // file the row actually points at. Probing ".skc" first would report the
    // SKC file's size for the SKK row whenever a same-named SKC macro existed.
    namespace fs = std::filesystem;
    const auto dir = skkbot2::paths::replays();
    const auto stem = ReplayEngine::macroEntryStem(entry);
    const auto ext = ReplayEngine::macroEntryExt(entry);
    const auto path = ext.empty() ? dir / skk::stringToPath(stem + ".skc") : dir / skk::stringToPath(stem + ext);

    // fs::exists + fs::file_size is two filesystem syscalls, and this runs for
    // every matching row on every keystroke of the filter box. A file's size does
    // not change under us mid-session, so remember it. The key is the full path,
    // so a macro that is re-saved under the same name gets picked up rather than
    // showing a stale size.
    static std::map<std::string, std::string> cache;
    const std::string key = skk::pathToUtf8(path);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    std::error_code ec;
    std::string result = "saved in library";
    if (fs::exists(path, ec)) {
        const auto sz = fs::file_size(path, ec);
        const char* label = ext.empty() ? ".skc" : ext.c_str();
        char buf[48];
        if (sz >= 1024 * 1024) {
            std::snprintf(buf, sizeof(buf), "%.1f MB  ·  %s", static_cast<double>(sz) / (1024.0 * 1024.0), label);
        } else {
            std::snprintf(buf, sizeof(buf), "%.1f KB  ·  %s", static_cast<double>(sz) / 1024.0, label);
        }
        result = buf;
    }
    // Bound the cache: the library can be re-read between sessions and a stale
    // entry for a deleted macro must not pin memory forever.
    if (cache.size() > 512) cache.clear();
    cache.emplace(key, result);
    return result;
}

void drawSearchDropdown(ImDrawList* draw, float k) {
    auto& st = state();

    // v4 WINDOW ANIMATION: the dropdown is not drawn at all during the shell's
    // closing ghost frames.
    if (gShellClosing) {
        st.scrollDragging = false;
        return;
    }

    // Animate dropdown open / close
    st.searchOpenT = anim01(ImGui::GetID("##beta_macro_list_anim"),
                            st.searchVisible ? 1.0f : 0.0f, 16.0f);
    if (st.searchOpenT < 0.002f || st.searchW <= 0.0f) {
        st.scrollDragging = false;
        st.deleteConfirmMacro.clear();
        return;
    }

    // Dropdown entrance: on the rising edge (closed -> open) the cursor resets
    // to 0 and ramps to 1 while the list is open; rows reveal staggered off it
    // (slide-up + fade) exactly like the hover wash glides in. The falling edge
    // clears the toggle so the NEXT open re-staggers from scratch.
    if (st.searchVisible != st.searchEntrPrevOpen) {
        if (st.searchVisible) st.searchEntrCursor = 0.0f;
        st.searchEntrPrevOpen = st.searchVisible;
    }
    st.searchEntrCursor =
        std::min(1.0f, st.searchEntrCursor + ImGui::GetIO().DeltaTime / 0.34f);

    // Refresh icon spin animation
    if (st.refreshAngle > 0.001f) {
        st.refreshAngle = std::max(0.0f, st.refreshAngle - ImGui::GetIO().DeltaTime * 14.0f);
    }

    auto& gui = skkbot2::gui::get();
    const float kt = uiTextScale();

    // NOTE: the library list is refreshed (disk scan) when the dropdown OPENED
    // (folder button / tab switch / refresh icon), so both the engine list and
    // gui.macros are already current here — no per-frame re-sync is needed. The
    // filtered rows + their per-row size captions are cached in State and
    // rebuilt only when the query changes or the list is marked dirty.

    ImFont* mf = mono();
    std::string q(gui.macroName);
    std::transform(q.begin(), q.end(), q.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (state().filterDirty || state().filterQuery != q) {
        state().filtered.clear();
        state().filteredInfo.clear();
        for (auto const& name : gui.macros) {
            if (!name.empty() && (q.empty() || std::search(name.begin(), name.end(),
                q.begin(), q.end(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                }) != name.end())) {
                state().filtered.push_back(name);
                state().filteredInfo.push_back(macroSmallInfo(name.c_str()));
            }
        }
        state().filterQuery = q;
        state().filterDirty = false;
    }
    const std::vector<std::string>& filtered = state().filtered;
    const std::vector<std::string>& filteredInfo = state().filteredInfo;
    const int totalRows = static_cast<int>(filtered.size());

    const float w = state().searchW;
    const float pad = 8.0f * k;
    const float labelH = 8.0f * k + 18.0f * k;
    const float rowH = 52.0f * k;
    const float emptyH = 34.0f * k;

    // The list floats above the page content and expands down towards the footer.
    // Allow fitting up to 4 rows (or available space before footer).
    const float tlY = state().searchMin.y + 55.0f * k + 7.0f * k;
    ImVec2 tl(state().searchMin.x, tlY);
    const ImVec2 winMin = ImGui::GetWindowPos();
    const ImVec2 winMax = add(winMin, ImGui::GetWindowSize());
    const float footerFloor = winMax.y - kFooterH * k - 6.0f * k;
    const float availableH = std::max(0.0f, footerFloor - tlY);
    const int maxPossibleRows = availableH > 0.0f
        ? static_cast<int>((availableH - pad * 2.0f - labelH) / rowH)
        : 0;
    const int maxCapacity = std::max(4, maxPossibleRows);
    const int rowsShown = std::min(totalRows, maxCapacity);
    const float rowsH = rowsShown * rowH;
    const float fullBoxH = pad * 2.0f + labelH + (totalRows == 0 ? emptyH : rowsH);

    // Animate box height unfolding smoothly
    const float minHeaderH = pad * 2.0f + labelH;
    const float animBoxH = minHeaderH + (fullBoxH - minHeaderH) * st.searchOpenT;
    ImVec2 br(tl.x + w, std::min(tl.y + animBoxH, footerFloor));
    // Expose the panel rect to the shell's drag gate (clicking inside the list
    // must not start a window drag; clicking outside it moves the whole menu).
    state().searchBoxMax = br;

    // Row-aligned scroll: only when there are more macros than fit.
    const float maxScroll =
        std::max(0.0f, static_cast<float>(totalRows - rowsShown) * rowH);
    const bool scrollable = maxScroll > 0.5f;
    state().searchScroll = std::clamp(state().searchScroll, 0.0f, maxScroll);
    // v4 SMOOTH SCROLL (14.11): the macro list eases like the main regions —
    // the wheel moves a stored TARGET in whole rows (rapid notches accumulate
    // without losing input), the drawn offset chases it with an exponential
    // tail. Keyed on the scroll member, like applyRegionScroll.
    static std::unordered_map<const void*, std::pair<float, float>> listAnims;
    auto lit = listAnims.find(&state().searchScroll);
    if (!scrollable) {
        if (lit != listAnims.end()) listAnims.erase(lit);
        state().searchScroll = 0.0f;
    } else {
        if (lit == listAnims.end()) {
            lit = listAnims.emplace(&state().searchScroll,
                                    std::make_pair(state().searchScroll,
                                                   state().searchScroll))
                      .first;
        }
        auto& [lt, la] = lit->second;
        if (ImGui::IsMouseHoveringRect(tl, br) &&
            ImGui::GetIO().MouseWheel != 0.0f) {
            lt += ImGui::GetIO().MouseWheel > 0.0f ? -rowH : rowH;
            ImGui::GetIO().MouseWheel = 0.0f;
        }
        lt = std::clamp(lt, 0.0f, maxScroll);
        float dtL = ImGui::GetIO().DeltaTime;
        if (dtL <= 0.0f) dtL = 1e-3f;
        if (dtL > 0.05f) dtL = 0.05f;
        la += (lt - la) * (1.0f - std::exp(-14.0f * dtL));
        if (std::fabs(lt - la) < 0.5f) la = lt;
        state().searchScroll = std::clamp(la, 0.0f, maxScroll);
    }

    // Clip to the window so a deep dropdown never paints outside the shell.
    ImVec2 clipMin = tl, clipMax = br;
    clipMin.x = std::max(clipMin.x, winMin.x);
    clipMin.y = std::max(clipMin.y, winMin.y);
    clipMax.x = std::min(clipMax.x, winMax.x);
    clipMax.y = std::min(clipMax.y, winMax.y);
    if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) return;

    // Smooth fade for background & border with liquid glass drop glow
    drawSoftHalo(draw, tl, br, 28.0f * k, theme::rgb(70, 150, 255), 0.25f * st.searchOpenT, k);
    draw->AddRectFilled(tl, br, withAlpha(theme::ModalBg, st.searchOpenT), 28.0f * k);
    draw->AddRect(tl, br, withAlpha(theme::ModalBorder, st.searchOpenT), 28.0f * k, 0, 1.0f);

    // Modal background blocker: swallow clicks on empty areas inside the dropdown
    // so they never fall through to underlying widgets or window drag.
    ImGui::SetCursorScreenPos(tl);
    ImGui::InvisibleButton("##macro_list_modal_shield", ImVec2(br.x - tl.x, br.y - tl.y),
                           ImGuiButtonFlags_AllowOverlap);

    // The list is modal: everything below it on the RECORD page is registered
    // as disabled (with AllowOverlap) in drawRecordPage, so the entries here
    // (refresh, rows) always win the hit-test and clicks never fall through to
    // RECORD/PLAY.

    // Подпись «FROM YOUR LIBRARY» / «A FRESH START» убрана по запросу
    // 03.11.2026; банд лейбла сохраняет высоту — refresh-кнопка и строки
    // списка остались на своих местах.

    // Refresh button on the right of the label band (reloads the library list).
    const float refSize = 22.0f * k;
    const ImVec2 refPos(br.x - pad - refSize, tl.y + pad + (labelH - refSize) * 0.5f);
    if (iconButton("##beta_lib_refresh", refPos, refSize, theme::AccentSoft)) {
        gui.refreshMacroList();
        state().filterDirty = true;  // rebuild cached rows/captions next frame
        st.refreshAngle = 6.2831853f; // spin 360 degrees on refresh
    }
    const bool refHover = ImGui::IsItemHovered();
    drawRefreshGlyph(draw, ImVec2(refPos.x + refSize * 0.5f, refPos.y + refSize * 0.5f), k,
                     withAlpha(refHover ? theme::Accent : theme::rgb(146, 157, 178), st.searchOpenT),
                     st.refreshAngle);

    // Open the macros folder in Explorer, immediately left of the refresh icon.
    // Hand-rolled rather than iconButton() because it needs a one-shot on press,
    // not a persistent toggled state.
    const ImVec2 openPos(refPos.x - refSize - 6.0f * k, refPos.y);
    ImGui::SetCursorScreenPos(openPos);
    ImGui::InvisibleButton("##beta_lib_open", ImVec2(refSize, refSize));
    const bool openHover = ImGui::IsItemHovered();
    const bool openActive = ImGui::IsItemActive();
    if (openActive && !st.openFolderPrevDown) {
        openFolderInExplorer(skkbot2::paths::replays());
    }
    st.openFolderPrevDown = openActive;
    if (openHover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float openWash = anim01(ImGui::GetID("##beta_lib_open") ^ 0x4F50,
                                  openHover ? 1.0f : 0.0f, 14.0f);
    if (openWash > 0.004f) {
        draw->AddRectFilled(openPos, add(openPos, ImVec2(refSize, refSize)),
                            withAlpha(theme::AccentSoft, openWash), 28.0f * k);
    }
    drawFolderGlyph(draw, ImVec2(openPos.x + refSize * 0.5f, openPos.y + refSize * 0.5f), k,
                    withAlpha(openHover ? theme::Accent : theme::rgb(146, 157, 178), st.searchOpenT));

    // Slim interactive scrollbar on the right edge when the list overflows.
    if (scrollable) {
        const float trackX = br.x - 13.0f * k;
        const float trackW = 6.0f * k;
        const float trackY0 = tl.y + pad + labelH;
        const float trackY1 = br.y - pad;
        const float trackH = std::max(1.0f, trackY1 - trackY0);
        const float thumbH =
            std::max(16.0f * k, trackH * static_cast<float>(rowsShown) / static_cast<float>(totalRows));
        const float thumbTravel = std::max(1.0f, trackH - thumbH);
        float thumbY = trackY0 + (state().searchScroll / maxScroll) * thumbTravel;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        // v4 THUMB GROW (list): same polish as the region scrollbars — the 6k
        // thumb widens to 9k while hovered or dragged, growing symmetrically
        // around the track's center line so it stays ON the rail.
        float dtW = ImGui::GetIO().DeltaTime;
        if (dtW <= 0.0f) dtW = 1e-3f;
        if (dtW > 0.05f) dtW = 0.05f;
        const float cx = trackX + trackW * 0.5f;
        static std::unordered_map<const void*, float> listThumbW;
        auto wit = listThumbW.emplace(&state().searchScroll, trackW);
        float& curW = wit.first->second;
        {
            const float halfProbe = 9.0f * k * 0.5f + 6.0f * k;
            const bool hov = ImGui::IsMouseHoveringRect(ImVec2(cx - halfProbe, thumbY),
                                                        ImVec2(cx + halfProbe, thumbY + thumbH));
            const float wTarget = (st.scrollDragging || hov) ? 9.0f : 6.0f;
            curW += (wTarget * k - curW) * (1.0f - std::exp(-18.0f * dtW));
            if (std::fabs(wTarget * k - curW) < 0.05f) curW = wTarget * k;
        }
        const float thumbW = curW;
        const float thumbX = cx - thumbW * 0.5f;
        const float hitPadX = 6.0f * k;
        const ImVec2 thumbMin(thumbX - hitPadX, thumbY);
        const ImVec2 thumbMax(thumbX + thumbW + hitPadX, thumbY + thumbH);
        const ImVec2 trackMin(trackX, trackY0);
        const ImVec2 trackMax(trackX + trackW, trackY1);

        const bool hoveredThumb = ImGui::IsMouseHoveringRect(thumbMin, thumbMax);
        const bool hoveredTrack = ImGui::IsMouseHoveringRect(trackMin, trackMax);

        // Handle dragging and clicking on scrollbar
        if (ImGui::IsMouseClicked(0)) {
            if (hoveredThumb) {
                st.scrollDragging = true;
                st.scrollDragStartY = mouse.y;
                st.scrollDragStartScroll = st.searchScroll;
            } else if (hoveredTrack) {
                // Click on track outside thumb: page jump towards mouse
                if (mouse.y < thumbY) {
                    st.searchScroll = std::max(0.0f, st.searchScroll - rowH * rowsShown);
                } else {
                    st.searchScroll = std::min(maxScroll, st.searchScroll + rowH * rowsShown);
                }
                // Direct manipulation wins over the ease (same rule as
                // applyRegionScroll): resync the stored target/anim.
                if (auto sit = listAnims.find(&state().searchScroll);
                    sit != listAnims.end()) {
                    sit->second.first = st.searchScroll;
                    sit->second.second = st.searchScroll;
                }
            }
        }

        if (st.scrollDragging) {
            if (ImGui::IsMouseDown(0)) {
                const float dy = mouse.y - st.scrollDragStartY;
                const float scrollDelta = (dy / thumbTravel) * maxScroll;
                st.searchScroll = std::clamp(st.scrollDragStartScroll + scrollDelta, 0.0f, maxScroll);
                // Recompute thumbY for current frame draw
                thumbY = trackY0 + (st.searchScroll / maxScroll) * thumbTravel;
                if (auto sit = listAnims.find(&state().searchScroll);
                    sit != listAnims.end()) {
                    sit->second.first = st.searchScroll;
                    sit->second.second = st.searchScroll;
                }
            } else {
                st.scrollDragging = false;
            }
        }

        const bool thumbActive = st.scrollDragging || hoveredThumb;
        // Track background
        draw->AddRectFilled(trackMin, trackMax,
                            withAlpha(theme::rgb(24, 27, 34), st.searchOpenT), 3.0f * k);
        // Thumb
        const ImU32 thumbCol = thumbActive ? theme::AccentLight : theme::rgb(140, 153, 182);
        draw->AddRectFilled(ImVec2(thumbX, thumbY), ImVec2(thumbX + thumbW, thumbY + thumbH),
                            withAlpha(thumbCol, st.searchOpenT), 3.0f * k);
    }

    float ry = tl.y + pad + labelH;
    if (totalRows == 0) {
        const char* empty = "No matching macros. Press Record to create this one.";
        ImVec2 es = ImGui::GetFont()->CalcTextSizeA(pxSize(11.0f * kt), FLT_MAX, 0, empty);
        draw->AddText(ImGui::GetFont(), pxSize(11.0f * kt),
                      ImVec2(tl.x + (w - es.x) * 0.5f, ry + (emptyH - es.y) * 0.5f),
                      withAlpha(theme::rgbText(177, 187, 208), st.searchOpenT), empty);
        return;
    }

    const int first = std::min(
        totalRows > 0 ? static_cast<int>(state().searchScroll / rowH) : 0,
        std::max(0, totalRows - rowsShown));
    const int last = std::min(totalRows, first + rowsShown);
    // Panel clip
    draw->PushClipRect(ImVec2(tl.x + pad - 4.0f * k, tl.y + pad + labelH),
                       ImVec2(br.x - pad + 4.0f * k, br.y - pad), true);

#ifdef GEODE_IS_WINDOWS
    const bool rmbPhysDown = (GetAsyncKeyState(0x02) & 0x8000) != 0;
    const bool rmbClickedAny = (rmbPhysDown && !st.rmbDownPrev) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    st.rmbDownPrev = rmbPhysDown;
#else
    const bool rmbClickedAny = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
#endif

    for (int i = first; i < last; ++i) {
        const char* name = filtered[static_cast<size_t>(i)].c_str();
        const ImVec2 r0(tl.x + pad, ry);
        // When the list scrolls, reserve a right gutter for the slim scrollbar
        // so rows (hover wash / caption / arrow) never paint under it.
        const float rowRight = scrollable ? br.x - 18.0f * k : br.x - pad;
        const ImVec2 r1(rowRight, ry + rowH);
        const float sw = r1.x - r0.x;

        const bool isDeleting = (!st.deleteConfirmMacro.empty() && st.deleteConfirmMacro == name);

        // Buttons geometry if in deletion confirmation mode
        const float btnH = 22.0f * k;
        const float delBtnW = 48.0f * k;
        const float canBtnW = 48.0f * k;
        const float btnGap = 6.0f * k;
        const float delBtnX = r1.x - 8.0f * k - delBtnW;
        const float canBtnX = delBtnX - btnGap - canBtnW;

        // If deleting, main row hit area stops before the action buttons so they can be clicked
        const float rowHitW = isDeleting ? std::max(40.0f * k, canBtnX - r0.x - 4.0f * k) : sw;

        ImGui::SetCursorScreenPos(r0);
        const bool clicked = ImGui::InvisibleButton(
            (std::string("##macro_row") + std::to_string(i)).c_str(),
            ImVec2(rowHitW, r1.y - r0.y));
        const bool hovered = ImGui::IsItemHovered();
        const bool rightClicked = hovered && rmbClickedAny;

        if (rightClicked) {
            if (isDeleting) {
                st.deleteConfirmMacro.clear();
            } else {
                st.deleteConfirmMacro = name;
            }
        }

        // Row fill glides in; id is window-scoped so the key is stable per row.
        const float rh = anim01(ImGui::GetID("##macro_rows") + static_cast<ImGuiID>(i) + 0x3000,
                                hovered ? 1.0f : 0.0f, 14.0f);
        // Staggered entrance: row N of the visible window waits its turn off the
        // dropdown cursor (each row 0.11 later), eased in-out; the whole band
        // slides up + fades in exactly like the hover wash glides across a row.
        const float rv = std::clamp(st.searchEntrCursor * 1.35f -
                                        static_cast<float>(i - first) * 0.11f, 0.0f, 1.0f);
        const float er = rv * rv * (3.0f - 2.0f * rv) * st.searchOpenT;
        const float ryS = ry + (1.0f - er) * 6.0f * k;  // slide-up by 6k while fading
        const float btnY = ryS + (rowH - btnH) * 0.5f;

        if (isDeleting) {
            // Danger/alert recolor when awaiting delete confirmation
            draw->AddRectFilled(r0, r1, withAlpha(theme::rgb(140, 36, 45), 0.45f * er), 28.0f * k);
            draw->AddRect(r0, r1, withAlpha(theme::rgb(225, 70, 80), 0.85f * er), 28.0f * k, 0, 1.0f);
        } else if (rh * er > 0.004f) {
            draw->AddRectFilled(r0, r1, withAlpha(theme::rgb(74, 86, 112), rh * er), 28.0f * k);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        // v4 ACTIVE MARKER (14.11): the row of the macro currently loaded in
        // the engine wears a slim accent bar on its left edge. Before this,
        // nothing in the list told you WHICH macro was live — you had to
        // remember the name you clicked last session. Subtle: 2.5k bar, not a
        // full row recolor, so hover remains the primary channel.
        {
            auto* eng = ReplayEngine::get();
            auto& skcBot = skk::skc::Bot::get();
            const bool isActiveRow =
                (eng->activeSk && eng->activeSk->name == name) ||
                (eng->activeMacro && eng->activeMacro->name == name) ||
                skcBot.getMacroName() == name;
            if (isActiveRow && !isDeleting) {
                draw->AddRectFilled(ImVec2(r0.x, ryS + 3.0f * k),
                                    ImVec2(r0.x + 2.5f * k, ryS + rowH - 3.0f * k),
                                    withAlpha(theme::Accent, er), 2.0f * k);
            }
        }

        if (clicked && !rightClicked) {
            if (isDeleting) {
                st.deleteConfirmMacro.clear();
            } else {
                // The row key is "<stem> [<ext>]". macroName stays a bare stem
                // (it is what SAVE appends an extension to), and the clicked
                // format is passed on so a same-named SKK/SKC pair no longer
                // resolves by whichever file happened to exist first.
                const std::string stem = ReplayEngine::macroEntryStem(name);
                const std::string ext = ReplayEngine::macroEntryExt(name);
                std::snprintf(gui.macroName, sizeof(gui.macroName), "%s", stem.c_str());
                skkbot2::gui::loadMacroIntoMemory(stem, ext);
                gui.refreshMacroList();
                state().searchVisible = false;
                state().searchForced = false;
                break;
            }
        }

        ImFont* dp = ImGui::GetFont();
        const float tsl = std::clamp(er, 0.0f, 1.0f);  // per-row fade driver
        const ImU32 nameCol = isDeleting ? theme::rgb(255, 230, 230) : theme::rgbText(203, 214, 231);
        draw->AddText(dp, pxSize(12.0f * kt), ImVec2(r0.x + 10.0f * k, ryS + 6.0f * k),
                      withAlpha(nameCol, tsl), name);
        ImVec2 nw = dp->CalcTextSizeA(pxSize(12.0f * kt), FLT_MAX, 0, name);
        ImFont* sf = mf ? mf : dp;

        if (isDeleting) {
            draw->AddText(sf, pxSize(8.0f * kt), ImVec2(r0.x + 9.0f * k, ryS + 6.0f * k + nw.y + 3.0f * k),
                          withAlpha(theme::rgb(245, 125, 135), tsl), "CONFIRM DELETION");

            // Cancel button
            const ImVec2 canPos(canBtnX, btnY);
            const ImVec2 canSize(canBtnW, btnH);
            ImGui::SetCursorScreenPos(canPos);
            if (ImGui::InvisibleButton((std::string("##macro_cancel_") + std::to_string(i)).c_str(), canSize)) {
                st.deleteConfirmMacro.clear();
            }
            const bool canHover = ImGui::IsItemHovered();
            const ImU32 canBg = canHover ? theme::rgb(62, 72, 94) : theme::rgb(44, 50, 66);
            const ImU32 canBd = canHover ? theme::rgb(115, 130, 162) : theme::rgb(75, 85, 108);
            draw->AddRectFilled(canPos, add(canPos, canSize), withAlpha(canBg, tsl), 28.0f * k);
            draw->AddRect(canPos, add(canPos, canSize), withAlpha(canBd, tsl), 28.0f * k, 0, 1.0f);
            ImVec2 canSz = dp->CalcTextSizeA(pxSize(8.5f * kt), FLT_MAX, 0, "CANCEL");
            draw->AddText(dp, pxSize(8.5f * kt),
                          ImVec2(canBtnX + (canBtnW - canSz.x) * 0.5f, btnY + (btnH - canSz.y) * 0.5f),
                          withAlpha(theme::rgbText(215, 224, 240), tsl), "CANCEL");

            // Delete button
            const ImVec2 delPos(delBtnX, btnY);
            const ImVec2 delSize(delBtnW, btnH);
            ImGui::SetCursorScreenPos(delPos);
            bool doDelete = false;
            if (ImGui::InvisibleButton((std::string("##macro_delete_") + std::to_string(i)).c_str(), delSize)) {
                doDelete = true;
            }
            const bool delHover = ImGui::IsItemHovered();
            const ImU32 delBg = delHover ? theme::rgb(230, 52, 62) : theme::rgb(188, 38, 48);
            const ImU32 delBd = delHover ? theme::rgb(255, 95, 105) : theme::rgb(220, 68, 78);
            draw->AddRectFilled(delPos, add(delPos, delSize), withAlpha(delBg, tsl), 28.0f * k);
            draw->AddRect(delPos, add(delPos, delSize), withAlpha(delBd, tsl), 28.0f * k, 0, 1.0f);
            ImVec2 delSz = dp->CalcTextSizeA(pxSize(8.5f * kt), FLT_MAX, 0, "DELETE");
            draw->AddText(dp, pxSize(8.5f * kt),
                          ImVec2(delBtnX + (delBtnW - delSz.x) * 0.5f, btnY + (btnH - delSz.y) * 0.5f),
                          withAlpha(theme::rgb(255, 255, 255), tsl), "DELETE");

            if (canHover || delHover) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            if (doDelete) {
                namespace fs = std::filesystem;
                auto* engine = ReplayEngine::get();
                const auto dir = skkbot2::paths::replays();
                std::error_code ec;
                // Delete ONLY the file this row points at. Removing both
                // extensions destroyed a same-named macro of the other engine.
                const auto delStem = ReplayEngine::macroEntryStem(name);
                const auto delExt = ReplayEngine::macroEntryExt(name);
                fs::remove(dir / skk::stringToPath(delStem + (delExt.empty() ? std::string(".skc") : delExt)), ec);
                if (std::strcmp(gui.macroName, name) == 0) {
                    gui.macroName[0] = '\0';
                }
                if (engine && engine->activeSk && engine->activeSk->name == name) {
                    engine->discardActiveMacro();
                }
                if (engine && engine->activeMacro && engine->activeMacro->name == name) {
                    engine->discardActiveMacro();
                }
                auto& bot = skk::skc::Bot::get();
                if (bot.getMacroName() == name) {
                    bot.onResetEvent(skk::skc::Bot::ResetReason::UserClear, 0);
                    bot.setMacroName("");
                }
                gui.refreshMacroList();
                st.filterDirty = true;
                st.deleteConfirmMacro.clear();
                // v4 TOASTS: shell-styled feedback instead of the GD bubble.
                pushToast(st, fmt::format("Deleted: {}", name), theme::Record);
                break;
            }
        } else {
            const std::string& smallText = filteredInfo[static_cast<size_t>(i)];
            draw->AddText(sf, pxSize(8.0f * kt), ImVec2(r0.x + 9.0f * k, ryS + 6.0f * k + nw.y + 3.0f * k),
                          withAlpha(theme::rgb(166, 180, 205), tsl), smallText.c_str());
            // Arrow up-right.
            const ImVec2 a0(r1.x - 22.0f * k, ryS + 9.0f * k);
            draw->AddLine(ImVec2(a0.x + 1.0f, a0.y + 9.0f * k), ImVec2(a0.x + 10.0f * k, a0.y),
                          withAlpha(theme::rgb(165, 175, 196), tsl), 1.4f);
            draw->AddLine(ImVec2(a0.x + 10.0f * k, a0.y), ImVec2(a0.x + 2.0f * k, a0.y),
                          withAlpha(theme::rgb(165, 175, 196), tsl), 1.4f);
            draw->AddLine(ImVec2(a0.x + 10.0f * k, a0.y), ImVec2(a0.x + 2.0f * k, a0.y),
                          withAlpha(theme::rgb(165, 175, 196), tsl), 1.4f);
        }
        ry += rowH;
    }
    draw->PopClipRect();
}

} // namespace

void render(bool open) {
    auto& v = state();
    // v4 WINDOW ANIMATION: when `open` flips false but the exit animation is
    // enabled and still running, we keep rendering a few more frames — the
    // shell slides down over shellAnimSec, widgets inert, then the normal
    // close cleanup runs. When the animation is OFF (or already finished),
    // the instant path below applies as before.
    bool closing = false;
    if (!open) {
        if (v.shellAnim && v.shellAnimSec >= 0.05f && v.shellWasOpen &&
            v.shellCloseT < 1.0f) {
            closing = true;
        } else {
            // The window is gone: drop the search state so the dropdown never
            // resurrects stale (open / scrolled) on a later frame. Closing the
            // window must behave like pressing the folder button would.
            v.searchVisible = false;
            v.searchForced = false;
            // Arm the shell entrance for the next open (see the open path below).
            v.shellEntrPrev = false;
            v.shellWasOpen = false;
            v.shellCloseT = 0.0f;
            return;
        }
    } else {
        v.shellCloseT = 0.0f;
        v.shellWasOpen = true;
    }

    auto& view = state();
    gShellClosing = closing;
    // COSMETICS: load the persisted solid-background flag once (default ON —
    // solid background). drawShell reads it every frame.
    if (!view.cosmeticsInit) {
        view.cosmeticsInit = true;
        theme::fontScale = Mod::get()->getSavedValue<float>("gui_font_scale", 1.0f);
        if (theme::fontScale < 0.60f || theme::fontScale > 1.50f) {
            theme::fontScale = 1.0f;
        }
        // v4 WINDOW ANIMATION: persisted toggle + duration.
        view.shellAnim = Mod::get()->getSavedValue<bool>("gui_shell_anim", true);
        view.shellAnimSec =
            std::clamp(Mod::get()->getSavedValue<float>("gui_shell_anim_sec", 0.05f),
                       0.05f, 0.60f);
        // v4 VISUAL JSON: gui_theme.json — единый источник истины для
        // solid / blur / цвета. Нет файла (первый запуск / удалён) →
        // создаём с дефолтами. Файл открыт передачей path напрямую
        // (не-ASCII save-dir безопасен, path.string() запрещён).
        theme::solidBackground = true;
        theme::blurEnabled = true;
        bool loadedColor = false;
        {
            std::ifstream in(themeJsonPath());
            if (in) {
                std::string body((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                const VisualJson vis = parseVisualJson(body);
                if (vis.hasSolid) theme::solidBackground = vis.solid;
                if (vis.hasBlur) theme::blurEnabled = vis.blur;
                if (!vis.color.empty()) {
                    loadedColor = !applyAccentHex(vis.color).empty();
                }
                // "#FFFFFF" IS the neutral text — never a tint.
                if (!vis.text.empty() && vis.text != "#FFFFFF") {
                    applyTextHex(vis.text);
                }
            }
        }
        if (!loadedColor) {
            // Legacy path (raw hue/sat saved values) only when the JSON has
            // no usable colour; defaults 0/100% = shipped look.
            theme::tintHue = Mod::get()->getSavedValue<float>("gui_tint_hue", 0.0f);
            theme::tintSat = std::clamp(
                Mod::get()->getSavedValue<float>("gui_tint_sat", 1.0f), 0.0f, 2.0f);
            theme::refreshTint();
        }
        // Первое создание / выравнивание файла под загруженное состояние.
        syncThemeJson();
    }
    ImGui::GetIO().FontGlobalScale = theme::fontScale;
    // The engine reloads its macro list asynchronously (a save finishing on a
    // worker thread bumps macroListVersion). Cheap version check lets the shell
    // copy the fresh storedMacros — every frame is fine, no disk I/O here.
    if (view.seenMacroVersion != ReplayEngine::get()->macroListVersion) {
        view.seenMacroVersion = ReplayEngine::get()->macroListVersion;
        skkbot2::gui::get().syncMacros();
    }
    // applyStyle() mutates ImGui's GLOBAL style; snapshot it so the beta UI's
    // theme cannot leak into other popups/menus that render afterwards.
    const ImGuiStyle styleBefore = ImGui::GetStyle();
    applyStyle();

    const float k = uiScale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Design size at k==1, then scale with k so the bigger (resolution-driven)
    // UI keeps its proportions and never exceeds the display margins.
    const float baseW = std::clamp(display.x * 0.64f, 680.0f, 880.0f);
    const float baseH = std::clamp(display.y * 0.76f, 460.0f, 610.0f);
    const float width = view.expanded
        ? std::max(240.0f, display.x - 48.0f)
        : std::min(display.x - 48.0f, baseW * k);
    const float height = view.expanded
        ? std::max(240.0f, display.y - 48.0f)
        : std::min(display.y - 48.0f, baseH * k);

    if (!view.initialized) {
        view.pos = ImVec2((display.x - width) * 0.5f, (display.y - height) * 0.5f);
        // v4 PERSISTENCE: restore the last session's tab + accordion states
        // once, before the first frame lays anything out.
        loadUiState(view);
        loadWindowPos(view.pos, display.x, display.y);
        view.initialized = true;
    }
    view.pos.x = std::clamp(view.pos.x, 8.0f, std::max(8.0f, display.x - width - 8.0f));
    view.pos.y = std::clamp(view.pos.y, 8.0f, std::max(8.0f, display.y - height - 8.0f));

    // v4 shell entrance/exit (14.11, WINDOW ANIMATION): the window spawns 14px
    // low and glides up into place over the configured shellAnimSec; on close
    // it keeps rendering briefly, sliding back down 14px. With the COSMETICS
    // toggle OFF both are instant (offset stays 0). Progress is time-normal
    // (dt / sec) with a smoothstep ease, so the duration setting is honest.
    float shellOffsetY = 0.0f;
    if (closing) {
        float dtC = ImGui::GetIO().DeltaTime;
        if (dtC <= 0.0f) dtC = 1e-3f;
        if (dtC > 0.05f) dtC = 0.05f;
        view.shellCloseT += dtC / view.shellAnimSec;
        if (view.shellCloseT >= 1.0f) {
            // Exit animation finished: run the standard close cleanup and
            // stop. (Nothing has been begun this frame yet — safe return.)
            view.searchVisible = false;
            view.searchForced = false;
            view.shellEntrPrev = false;
            view.shellWasOpen = false;
            view.shellCloseT = 0.0f;
            return;
        }
        const float t = view.shellCloseT;
        shellOffsetY = t * t * (3.0f - 2.0f * t) * 14.0f * k;
        view.shellEntrT = 1.0f;  // entrance settled while exiting
    } else {
        if (!view.shellEntrPrev) {
            view.shellEntrPrev = true;
            view.shellEntrT = view.shellAnim && view.shellAnimSec >= 0.05f ? 0.0f : 1.0f;
        }
        if (view.shellEntrT < 1.0f && view.shellAnim && view.shellAnimSec >= 0.05f) {
            float dtE = ImGui::GetIO().DeltaTime;
            if (dtE <= 0.0f) dtE = 1e-3f;
            if (dtE > 0.05f) dtE = 0.05f;
            view.shellEntrT += dtE / view.shellAnimSec;
            if (view.shellEntrT > 1.0f) view.shellEntrT = 1.0f;
        } else {
            view.shellEntrT = 1.0f;
        }
        const float t = view.shellEntrT;
        shellOffsetY = (1.0f - t) * (1.0f - t) * (3.0f - 2.0f * t) * 14.0f * k;
    }
    ImGui::SetNextWindowPos(add(view.pos, ImVec2(0.0f, shellOffsetY)),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 28.0f * k);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##skk_quiet_beta", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
    // v4 WINDOW ANIMATION: during the closing slide every ImGui widget is
    // disabled — custom draw calls (drawShell, toasts) still render, but
    // nothing can be hovered/clicked in the ghost frames. The pair is ALWAYS
    // balanced (BeginDisabled(closing) / EndDisabled()): a conditional skip of
    // EndDisabled left the stack dirty and tripped Dear ImGui's assert.
    ImGui::BeginDisabled(closing);

    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = add(min, ImGui::GetWindowSize());
    ImDrawList* draw = ImGui::GetWindowDrawList();

    drawShell(draw, min, max, k);

    // Per-frame latch for the scrollbar-vs-window-drag gate: applyBodyScroll
    // (called from drawBody, below) sets it when the thumb/track ate the LMB
    // press, and the drag gate at the end of render() consumes it. Cleared
    // here first so it never survives into the next frame. The RENDER action
    // button latch (actionBtnClick) is cleared here for the same reason.
    view.bodyScrollClick = false;
    view.timingScrollClick = false;
    view.noclipScrollClick = false;
    view.actionBtnClick = false;

    // Refresh the library once when switching into RECORD.
    if (view.lastTab != view.tab) {
        view.lastTab = view.tab;
        // Each tab starts at the top of its own scrollable stack.
        view.bodyScroll = 0.0f;
        view.timingScroll = 0.0f;
        view.noclipScroll = 0.0f;
        // Leaving Settings drops the keybind sub-screen: it is a drill-in owned
        // by that tab, and coming back from RECORD should not land on the bind
        // list the user had already dismissed (or never opened).
        if (view.tab != Tab::Settings) {
            view.keybindScreen = false;
        }
        if (view.tab == Tab::Record) {
            skkbot2::gui::get().refreshMacroList();
            view.filterDirty = true;
        } else if (view.tab == Tab::Settings) {
            view.fontsDirty = true;
            view.searchVisible = false;
            view.searchForced = false;
        } else {
            view.searchVisible = false;
            view.searchForced = false;
        }
    }

    const float headerH = kHeaderH * k;
    const float footerH = kFooterH * k;
    const float headerBot = min.y + headerH;
    const ImVec2 footerMin(min.x, max.y - footerH);
    const ImVec2 footerMax = max;

    // One font everywhere: the wordmark uses the same regular Inter as all the
    // other text (the old semibold logo face is gone).
    // Translucent header background. v4: mode-aware — the old single azure tint
    // read as a bright band on the dark Solid shell; dark shell gets its own
    // deep glass tone that matches the δbot/Silicate reference.
    const ImU32 headerTint = theme::solidBackground ? theme::rgb(17, 22, 34, 170)
                                                    : theme::rgb(34, 46, 68, 150);
    // DIAG 3: header in loud flat orange — the notch, if it belongs to the
    // header fill or sits between the header arc and the wash arc, names
    // itself by colour.
    const ImU32 headerDraw = g_cornerDiagMode == 3
                                 ? IM_COL32(255, 150, 0, 255)
                                 : headerTint;
    draw->AddRectFilled(min, ImVec2(max.x, headerBot), headerDraw, 28.0f * k,
                        ImDrawFlags_RoundCornersTop);

    drawBrand(draw, add(min, ImVec2(34.0f * k, 0)), min.y + headerH * 0.5f, nullptr, k);
    drawTopTabs(draw, min, width, headerH, k);

    // Close button (X) on top-right of the header
    {
        const float btnSize = 22.0f * k;
        const ImVec2 cPos(min.x + width - 36.0f * k, min.y + headerH * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(cPos.x - btnSize * 0.5f, cPos.y - btnSize * 0.5f));
        if (ImGui::InvisibleButton("##beta_close_btn", ImVec2(btnSize, btnSize))) {
            skkbot2::gui::get().open = false;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            // v4: soft circular press target so the X behaves like every other
            // icon button in the shell.
            draw->AddCircleFilled(cPos, btnSize * 0.62f, theme::rgb(255, 120, 120, 30), 24);
            draw->AddCircle(cPos, btnSize * 0.62f, theme::rgb(255, 120, 120, 60), 24, 1.0f);
        }
        const ImU32 xCol = hovered ? theme::rgb(255, 120, 120) : theme::Muted;
        const float xr = 4.5f * k;
        draw->AddLine(ImVec2(cPos.x - xr, cPos.y - xr), ImVec2(cPos.x + xr, cPos.y + xr), xCol, 1.8f);
        draw->AddLine(ImVec2(cPos.x + xr, cPos.y - xr), ImVec2(cPos.x - xr, cPos.y + xr), xCol, 1.8f);
    }

    // Header bottom border is part of the shell chrome in the mock.
    draw->AddLine(ImVec2(min.x, headerBot), ImVec2(max.x, headerBot), theme::Line, 1.0f);

    drawBody(draw, ImVec2(min.x, headerBot), ImVec2(max.x, footerMin.y), k);

    drawFooter(draw, footerMin, footerMax, footerH, k);

    // The macro search dropdown floats above everything, incl. the footer.
    if (view.tab == Tab::Record) drawSearchDropdown(draw, k);

    // Silicat-style drag: grab the window only if the mouse goes down over a
    // spot that is NOT a button/input (decoration/empty space). We anchor on
    // IsAnyItemActive(): when the LMB press lands on a widget it becomes the
    // active item, so the gate is false there and the button stays clickable;
    // on empty space nothing becomes active and we start the grab. IsWindowHovered()
    // is unreliable in this fork while the button is held (it flips false the
    // moment the LMB goes down), so the grab is anchored to the window rect.
    const bool mouseInWin = ImGui::IsMouseHoveringRect(min, max);
    const bool leftHeld = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!leftHeld) {
        // v4 PERSISTENCE: the drag gesture ENDS here — this runs before the
        // reset below, so the release frame still sees the moved position.
        // (Writing inside the drag block never fired: dragGrab/dragMoved are
        // already false by the time that branch is reached on release.)
        if (view.dragMoved) saveWindowPos(view.pos);
        view.dragGrab = false;
        view.dragMoved = false;
    }
    if (!closing &&
        ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] && mouseInWin &&
        !ImGui::IsAnyItemActive() &&
        !view.bodyScrollClick &&
        !view.timingScrollClick &&
        !view.noclipScrollClick &&
        !view.actionBtnClick &&
        (!view.searchVisible ||
         !ImGui::IsMouseHoveringRect(view.searchMin, view.searchBoxMax))) {
        view.dragGrab = true;
        // No auto-dismiss here: the macro list closes only via the folder
        // button (or by picking a macro), so a stray click on empty window
        // space must neither close it nor start a window drag over it.
    }
    if (view.dragGrab && leftHeld) {
        if (const ImVec2 delta = ImGui::GetIO().MouseDelta; delta.x != 0.0f || delta.y != 0.0f) {
            view.dragMoved = true;
        }
        if (view.dragMoved) {
            view.pos = add(view.pos, ImGui::GetIO().MouseDelta);
            // Snap to whole pixels while dragging: the blur capture rect and
            // the glass shell both live on integer coords, so subpixel drift
            // would otherwise make the frosted edge wobble against the shell.
            view.pos.x = std::round(view.pos.x);
            view.pos.y = std::round(view.pos.y);
            view.pos.x = std::clamp(view.pos.x, 8.0f, std::max(8.0f, display.x - width - 8.0f));
            view.pos.y = std::clamp(view.pos.y, 8.0f, std::max(8.0f, display.y - height - 8.0f));
            // The window is NoMove, so SetWindowPos is a no-op here. The shell
            // gets re-positioned next frame by SetNextWindowPos(view.pos, Always)
            // at the top of render().
        }
    }

    // v4 TOASTS: pill feedback stacked under the header. Each toast eases in
    // (~0.2 s), holds ~2.6 s, fades out (~0.5 s); width capped to 60% of the
    // window. Drawn with the shell's own language: rounded glass pill, themed
    // accent dot, muted text. Expired toasts are popped from the front.
    if (!view.toasts.empty()) {
        const double now = ImGui::GetTime();
        const float kIn = 0.20f, kHold = 2.6f, kOut = 0.50f;
        while (!view.toasts.empty() && now - view.toasts.front().born > kIn + kHold + kOut) {
            view.toasts.pop_front();
        }
        // v4 TOASTS (retest 4): horizontally centered BETWEEN the STANDBY
        // status (left) and the 0 | 0 readout (right), vertically centered in
        // the footer band. No border, no accent dot — just the glass pill.
        // (retest 5): single pill only (pushToast replaces), and the pill is
        // TEXT-SIZED — the old fixed 26k height left a dead strip of glass
        // under the text, which read as a stray band at the bottom.
        const float gap = 0.0f;  // single pill: no stack spacing
        const float footerTop = footerMin.y;
        for (const auto& t : view.toasts) {
            const double age = now - t.born;
            float a = 1.0f;
            if (age < kIn) a = static_cast<float>(age / kIn);
            else if (age > kIn + kHold) a = 1.0f - static_cast<float>((age - kIn - kHold) / kOut);
            a = std::clamp(a, 0.0f, 1.0f);
            const float e = a * a * (3.0f - 2.0f * a);  // smoothstep alpha
            const ImVec2 tsz = ImGui::CalcTextSize(t.text.c_str());
            const float ph = tsz.y + 12.0f * k;
            const float stackH = ph;
            float ty = footerTop + (footerH - stackH) * 0.5f;  // centered in footer
            const float maxW = width * 0.60f;
            const float txtW = std::min(tsz.x, maxW - 40.0f * k);
            const float pw = txtW + 32.0f * k;
            // Horizontally centered between the status text (left) and the
            // input readout (right) — 75k margin past each.
            const float zoneL = min.x + 78.0f * k, zoneR = max.x - 78.0f * k;
            const float px0 = std::max(zoneL, (zoneL + zoneR) * 0.5f - pw * 0.5f);
            // v4 TOASTS (retest 6): NO pill background at all — the footer band
            // under the message stays transparent, only the TEXT fades in and
            // out. The strip of dark glass read as a stray band on the footer.
            // Clipped text so long names fade instead of overflowing.
            draw->PushClipRect(ImVec2(px0 + 16.0f * k, ty), ImVec2(px0 + pw - 10.0f * k, ty + ph), true);
            draw->AddText(ImVec2(px0 + 16.0f * k, ty + (ph - tsz.y) * 0.5f),
                          withAlpha(theme::rgbText(210, 220, 240), static_cast<int>(230 * e)), t.text.c_str());
            draw->PopClipRect();
            ty += ph + gap;
        }
    }

    ImGui::EndDisabled();

    ImGui::End();
    ImGui::PopStyleVar(2);
    // v4 PERSISTENCE: one fingerprint compare per frame, disk write only when
    // the tab/accordion fingerprint actually changed. (Skipped during the
    // closing ghost frames — nothing user-visible can change there.)
    if (!closing) syncUiStateSave(view);
    ImGui::GetIO().FontGlobalScale = 1.0f;
    // Restore the global style snapshot so the beta theme doesn't leak into
    // other windows/popups (the classic GUI, GD menus) for the next frames.
    ImGui::GetStyle() = styleBefore;
}

} // namespace skkbot2::gui::beta_gui
