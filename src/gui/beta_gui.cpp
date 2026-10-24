#include "gui/beta_gui.hpp"
#include "gui/beta_actions.hpp"
#include "gui/beta_widgets.hpp"
#include "gui/blur.hpp"
#include "gui/skkbot_gui.hpp"

#include "skkBot.hpp"
#include "skc/skc_bot.hpp"
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
#include <cstdio>
#include <filesystem>
#include <string>

using namespace geode::prelude;

// Beta GUI вЂ” "abstract-modern-ui-redesign" shell. Layout mirrors the React
// mock (docs/abstract-modern-ui-redesign): 88px header (brand wordmark,
// centered mono top-tabs with 2px indicator, window actions), then
// a body region, then a 66px footer (engine status + shortcuts + INPUTS
// readout). Pure Dear ImGui (GD fork, 1.93.0).

namespace skkbot2::gui::beta_gui {

// Page-wide vertical rhythm. Every accordion adds this after its content so the
// whitespace from a section's LAST row to the NEXT accordion's heading is the
// same everywhere вЂ” previously the stacked Settings/Render sections butted
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
    // ease in/out here вЂ” the "KEYBINDS" title is a pinned page heading.
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

    // RENDER tab: the settings mirror. Loaded once from the mod's saved values
    // (loadRenderSettings) the first time the RENDER tab is shown, then kept in
    // sync вЂ” every edit persists back AND is applied to the live renderer, so a
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
    // IsAnyItemActive() вЂ” but this GD-fork imgui build can miss the hover/press
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
    // refreshed вЂ” never per frame. Before the cache, drawing the list called
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
};

State& state() {
    static State value;
    return value;
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

void drawShell(ImDrawList* draw, ImVec2 min, ImVec2 max, float k) {
    const float r = 28.0f * k;
    const float time = static_cast<float>(ImGui::GetTime());

    // 0. Hardware Dual-Kawase Background Blur under Liquid Glass. The capture
    // runs before this window's own draw data is emitted each frame, so the
    // region under the glass is pristine game content and blurs live while the
    // window is being dragged (the drag logic snaps the shell to whole pixels).
    // The blur region's alpha tracks the menu-opacity multiplier: the blur
    // paints opaque by default, so without this a lowered opacity would leave a
    // fully opaque blur slab under a clear shell вЂ” the menu would look flat and
    // the blur would read as a flickering solid square instead of fading with
    // the glass.
    // COSMETICS: solidBackground skips the blur entirely (no transparent
    // background at all) and the glass base below paints fully opaque, so the
    // menu reads as a flat solid panel.
    if (blur::available() && !theme::solidBackground) {
        blur::submit(draw, min, max, r, 1.0f, theme::menuOpacity);
    }

    // 1. Translucent Liquid Glass base background (smooth rounded canvas)
    const float cycle = std::sin(time * 0.9f);
    ImU32 glassBg = mixColor(theme::Bg, theme::rgb(20, 30, 48, 140), (cycle + 1.0f) * 0.5f);
    if (theme::solidBackground) {
        // Keep the hue but drop the translucency: a flat, fully opaque panel.
        glassBg = withAlpha(theme::rgb(16, 22, 36, 255), 1.0f);
    }
    draw->AddRectFilled(min, max, glassBg, r);

    // 2. Clean neutral glass edge (no blue glow)
    draw->AddRect(min, max, theme::rgb(205, 216, 234, 55), r, 0, 1.0f);
}

void drawBrand(ImDrawList* draw, ImVec2 topLeft, float cy, ImFont* bold, float k) {
    float x = topLeft.x;

    // Brand symbol вЂ” three input-wave bars, like the mock's mark.
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
    // across the bar then never lands in a dead zone between captions вЂ” that
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
        ? mixColor(theme::rgb(240, 242, 247), theme::AccentLight, switchAmt)
        : theme::rgb(240, 242, 247);
    const float len = (slideW - pad * 2.0f) * 0.5f;
    draw->AddRectFilled(ImVec2(slideX - len, indY), ImVec2(slideX + len, indY + indH), indCol);
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
    // NOCLIP header) keep 11px вЂ” computing from the base would shrink them too.
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
    const ImU32 titleCol = mixColor(theme::rgb(215, 223, 238), theme::Text, hov);

    // The rule line sits on the optical center of the ACTUAL caption, not on
    // its top edge. Measuring the real string matters: glyphs like '&' carry
    // their own ink box (descenders / overshoot), so centering on "0" alone
    // lands the line a hair off the true middle of "RECORD & PLAY".
    InkMeasure ink;
    const bool haveInk = measureGlyphInk(df, size, title, ink);
    const float lineY = haveInk ? rowY + (ink.y0 + ink.y1) * 0.5f : rowY + size * 0.5f;

    // Hover cue: the WHOLE heading row lights up (a full-width band), not just the
    // caption. The band is centered on the caption's ink so the wash sits even
    // above and below the text, and its ends are rounded вЂ” no sharp corners and
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

    // Engine status (left).
    const float lx = min.x + 33.0f * k;
    draw->AddCircleFilled(ImVec2(lx + 2.5f * k, cy), 2.5f * k, status.color, 16);
    draw->AddCircle(ImVec2(lx + 2.5f * k, cy), 4.2f * k, withAlpha(status.color, 0.25f), 16);
    const float tSize = pxSize(9.0f * kt);
    const float tSpacing = 1.2f * kt;
    const float statusTextY = cy - (mf ? mf->CalcTextSizeA(tSize, FLT_MAX, 0, "STANDBY").y
                                       : ImGui::GetFontSize()) * 0.5f;
    spacedText(draw, mf, tSize, ImVec2(lx + 12.0f * k, statusTextY),
               status.text, status.color, tSpacing);

    // Input readout (right): current | total, unpadded so the idle state reads
    // "0 | 0" instead of a wide zero-padded "0000 / 0000" slab.
    auto& gui = skkbot2::gui::get();
    auto* engine = ReplayEngine::get();
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
    const bool live = (gui.macroBotType == Gui::MacroBotType::SKC)
        ? (skk::skc::Bot::get().getState() == skk::skc::State::Record || skk::skc::Bot::get().getState() == skk::skc::State::Playback)
        : (engine->engineMode == MODE_EXECUTE || engine->engineMode == MODE_CAPTURE);

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
    const ImU32 barCol = theme::Muted;
    const ImU32 numCol = theme::Text;
    float rx = readoutRight - ts.x;
    draw->AddText(mf, numSize, ImVec2(rx, cy - digitH * 0.5f), numCol, totalS);
    rx -= gap + barW;
    draw->AddText(mf, numSize, ImVec2(rx, cy - digitH * 0.5f), barCol, "|");
    rx -= gap + cs.x;
    draw->AddText(mf, numSize, ImVec2(rx, cy - digitH * 0.5f), numCol, curS);
}

// ---------------------------------------------------------------------------
// Record page вЂ” the redesign's transport / timing / orbit-visual workspace
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
            borderC = theme::rgb(178, 199, 235);
        } else {
            // "Available" reads as a blue-tinted dark fill + blue border, so
            // an enabled PLAY is unmistakably clickable next to a flat
            // disabled one; hover lifts fill/border/text a smooth step.
            bg = mixColor(theme::rgb(42, 56, 82, 205), theme::rgb(60, 78, 108, 220), hov);
            fg = mixColor(theme::rgb(215, 224, 240), theme::rgb(231, 238, 248), hov);
            borderC = mixColor(theme::rgb(96, 128, 190), theme::rgb(146, 178, 240), hov);
        }
        if (!enabled) {
            bg = theme::rgb(44, 54, 74, 115); fg = theme::Muted;
            borderC = theme::rgb(122, 138, 165, 70);
        }
        kbdB = theme::rgb(74, 96, 140); kbdT = theme::rgb(170, 185, 210);
    }

    const float effR = std::min(28.0f * k, std::min(size.x, size.y) * 0.5f);
    // Hover glow: a SOFT halo behind the button
    if (hov > 0.004f) {
        drawSoftHalo(draw, pos, add(pos, size), effR, theme::Accent, hov * 0.8f, k);
    }
    // While the button is ACTIVE (recording/playing) it slowly breathes: a
    // pulsing soft halo so the live state reads as alive вЂ” subtle, not strong.
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
                                          theme::rgb(226, 232, 244), hov);
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
// wheel anywhere over the row) and THEN the enable toggle. No range track вЂ” the
// value is entered directly, so the block is a single short row. Returns the
// row height so callers can stack rows.
float timingControl(ImDrawList* draw, ImFont* mf, ImVec2 pos, float width,
                    const char* uid, const char* title,
                    float& value, float vmin, float vmax, const char* fmt,
                    const char* suffix, bool& enabled,
                    float k, float wheelStep = 0.0f, bool allowToggle = true,
                    const char* titleSuffix = nullptr, bool revealed = true) {
    // Unique ID scope so two stacked rows (TPS + Speed) don't collide.
    ImGui::PushID(uid);
    (void)mf;  // title/values render in the default font; mono goes to callers
    // The macro dropdown paints an opaque panel over the whole Record page,
    // so while it is open the per-row inputs must not take clicks.
    const bool blocked = state().searchVisible;
    // `revealed` is the accordion gate: while the owning section is still
    // easing open (or is collapsed) the row is visible but must not register
    // clicks вЂ” a clipped-out control would otherwise steal them from whatever
    // sits underneath. Defaults to true so TPS / FPS call sites are unchanged.
    const bool canToggle = allowToggle && !blocked && revealed;
    const bool canInput = enabled && !blocked && revealed;
    const float rowH = 40.0f * k;
    ImFont* df = ImGui::GetFont();
    const float kt = uiTextScale();
    ImVec2 ts = df->CalcTextSizeA(pxSize(14.0f * kt), FLT_MAX, 0, title);
    const ImVec2 tPos(pos.x, pos.y + (rowH - ts.y) * 0.5f);
    const ImU32 c0 = enabled ? theme::rgb(222, 230, 245) : theme::rgb(179, 186, 204);
    const ImU32 c1 = enabled ? theme::rgb(240, 245, 255) : theme::rgb(179, 186, 204);
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

    float v1 = value;
    const bool nfChanged = numberField(mf, "##nfield",
                                       ImVec2(nfX, pos.y + (rowH - nfH) * 0.5f),
                                       nfW, v1, vmin, vmax, suffix, fmt, canInput, wheelStep);
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
// settings вЂ” master switch, P1/P2 targets, death flash. The values live on
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
                           ImVec2(to.x + 16.0f * k, speedContentY + speedSeen), true);
        const bool speedRowL = speedSeen >= speedRowH * 0.85f;
        const bool syncRowL = speedSeen >= (speedRowH + syncGap + speedRowH) * 0.85f;
        const bool ctrl = listOpen ? false : true;

        // Row 1 - Speed (the accordion's first control).
        const double prevSpeedV = gui.speedhackValue;
        const bool prevSpeedOn = gui.speedhackEnabled;
        float speedV = static_cast<float>(gui.speedhackValue);
        bool speedOn = gui.speedhackEnabled;
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
                             syncOn ? theme::rgb(222, 230, 245)
                                    : theme::rgb(179, 186, 204),
                             syncOn ? theme::rgb(240, 245, 255)
                                    : theme::rgb(179, 186, 204));
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
    // with the column edge (no inset вЂ” TPS/Speed titles start at pos.x).
    const float titleSize = pxSize(14.0f * kt);
    const ImU32 capOn = theme::rgb(222, 228, 242);
    const ImU32 capOff = theme::rgb(179, 186, 204);
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    // Controls mirror the TPS/Speed shapes exactly: the P1/P2 pills use the
    // same box as the timing number field (100x34k), the toggles match the TPS
    // toggle (44x24k), and the rows are the same 40k height.
    const float pillW = 100.0f * k;
    const float pillH = 34.0f * k;
    // Control rows mirror the TPS/Speed topline: title text on the left and
    // the control on the right, both optically centered in one 40k row вЂ” so
    // the toggle sits on the SAME vertical level as its caption.
    const float rowH = 40.0f * k;
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "NOCLIP").y
                           : ImGui::GetFontSize();

    // Accordion: the rows grow/shrink smoothly instead of popping. The
    // content is drawn inside a clip that follows the eased open height, and
    // each row only registers its interactive control once it is (almost)
    // fully revealed вЂ” a still-hidden control must not steal clicks.
    //
    // Two nested reveals, both eased exactly like the block itself (rate 11):
    //  - openT  вЂ” the whole NOCLIP block opens/closes from the mini heading.
    //  - rowsT  вЂ” rows 2-3 (Players + Death Flash) appear ONLY while the master
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
                           ImVec2(to.x + 16.0f * k, y + seen), true);
        const float rowTop2 = rowH + 10.0f * k;
        const float rowTop3 = rowH * 2.0f + 20.0f * k;
        const bool row1L = seen >= rowH * 0.85f;
        const bool row2L = seen >= (rowTop2 + rowH) * 0.85f;
        const bool row3L = seen >= (rowTop3 + rowH) * 0.85f;
        // The list is window-modal: its rows live in the left column, so the right
        // column would stay clickable unless we gate every control on it.
        const bool ctrl = listOpen ? false : true;

        // Row 1 вЂ” master switch.
        {
            const ImVec2 tPos(from.x, y + (rowH - textH) * 0.5f);
            // Both states render through the SAME drawGradientText path: the
            // plain AddText branch lands glyphs at the fractional tPos.x while
            // drawGradientText snaps it to whole pixels, so mixing them made
            // the caption jump sideways on every toggle.
            drawGradientText(draw, df, titleSize, tPos, "Noclip",
                             noclipOn ? theme::rgb(222, 230, 245) : capOff,
                             noclipOn ? theme::rgb(240, 245, 255) : capOff);
            ImVec2 tog(from.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
            if (toggleAt("##nc_master", tog, noclipOn, ctrl && row1L)) {
                engine->collisionBypass = !noclipOn;
                Mod::get()->setSavedValue("gui_noclip", engine->collisionBypass);
            }
            y += rowH + 10.0f * k;
        }

        // Row 2 вЂ” player targets: two pills the exact size of the TPS number field.
        {
            const float labelSize = pxSize(9.0f * kt);
            const float labelSpacing = 1.2f * kt;
            const float btnW = pillW;  // 100k вЂ” same box as the TPS/Speed value field
            const float btnH = pillH;  // 34k вЂ” ditto
            const float gap = 10.0f * k;
            const float groupW = btnW * 2.0f + gap;
            const float gx = from.x + width - groupW;
            const float by = y + (rowH - btnH) * 0.5f;

            const ImVec2 tPos(from.x, y + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Players",
                             noclipOn ? theme::rgb(222, 230, 245) : capOff,
                             noclipOn ? theme::rgb(240, 245, 255) : capOff);
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

        // Row 3 вЂ” death flash. Hidden entirely while the master is off (rowsT
        // collapses rows 2-3 away; it only renders, like Players, while on).
        {
            const bool flashOn = noclipOn && engine->noclipDeathFlash;
            const ImVec2 tPos(from.x, y + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Death Flash",
                             flashOn ? theme::rgb(222, 230, 245) : capOff,
                             flashOn ? theme::rgb(240, 245, 255) : capOff);
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
    // "Р Р°СЃС‚РѕСЏРЅРёРµ РјРµР¶РґСѓ С„СЂРµР№Рј СЃС‚РµРїРїРµСЂРѕРј Рё РґРµР°С‚ С„Р»РµС€ РґРѕР»Р¶РЅРѕ Р±С‹С‚СЊ С‚Р°РєРёРј Р¶Рµ РєР°Рє Рё
    //  СЂР°СЃС‚РѕСЏРЅРёРµ РјРµР¶РґСѓ РЅРѕСѓРєР»РёРїРѕРј (РєРѕС‚РѕСЂС‹Р№ С‚РёРїР° "RECORD & PLAY") Рё РЅРѕСѓРєР»РёРїРѕРј (РєРѕС‚РѕСЂС‹Р№ РєРЅРѕРїРєР°)!
    //  Р РІ Р·Р°РєСЂС‹С‚РѕРј РІРёРґРµ С‚РѕР¶Рµ!"
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
        // a sliver of a collapsed row вЂ” rows sit only 10k apart, so any pad
        // >= 10k leaves part of a cap visible.
        draw->PushClipRect(ImVec2(from.x - 12.0f * k, fsContentY - 12.0f * k),
                           ImVec2(to.x + 16.0f * k, fsContentY + fsSeen), true);

        const float rowTop2 = rowH + 10.0f * k;
        const float rowTop3 = rowTop2 + rowH + 10.0f * k;
        const float rowTop4 = rowTop3 + rowH + 10.0f * k;
        const bool row1L = fsSeen >= rowH * 0.85f;
        const bool row2L = fsSeen >= (rowTop2 + rowH) * 0.85f;
        const bool row3L = fsSeen >= (rowTop3 + rowH) * 0.85f;
        const bool row4L = fsSeen >= (rowTop4 + rowH) * 0.85f;
        const bool ctrl = listOpen ? false : true;

        float fy = fsContentY;

        // Row 1 вЂ” Master switch (Frame Stepper toggle)
        {
            const bool stepperOn = FrameStepper::get().isEnabled();
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
                             stepperOn ? theme::rgb(222, 230, 245) : capOff,
                             stepperOn ? theme::rgb(240, 245, 255) : capOff);
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

        // Row 2 вЂ” Auto Pause On Death. An OPTION rather than a step control, so it
        // lives in the always-visible band: it can be armed without turning the
        // stepper on, and stays armed for the rest of the session.
        {
            const bool apOn = FrameStepper::get().isAutoPauseOnDeathEnabled();
            const ImVec2 tPos(from.x, fy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Auto Pause On Death",
                             apOn ? theme::rgb(222, 230, 245) : capOff,
                             apOn ? theme::rgb(240, 245, 255) : capOff);
            ImVec2 tog(from.x + width - toggleW, fy + (rowH - toggleH) * 0.5f);
            if (toggleAt("##fs_autopause", tog, apOn, ctrl && row2L)) {
                FrameStepper::get().setAutoPauseOnDeathEnabled(!apOn);
            }
            fy += rowH + 10.0f * k;
        }

        // Rows 3-4 вЂ” step controls, revealed only while the master is on.
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

        // Row 4 вЂ” Hold To Step. Governs the STEP BUTTON only (the keybind always
        // supports both press and hold). ON = hold-only: a press steps nothing.
        // OFF = tap-only: a press is worth the count, holding adds nothing.
        {
            const bool holdOn = FrameStepper::get().isHoldToStepEnabled();
            const ImVec2 tPos(from.x, fy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Hold To Step",
                             holdOn ? theme::rgb(222, 230, 245) : capOff,
                             holdOn ? theme::rgb(240, 245, 255) : capOff);
            // MENU badge: this switch only governs the menu STEP button, the keybind
            // always supports press and hold вЂ” hence MENU, not BETA.
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
    applyNoClipScroll(draw, from, to, ncTop, to.y, ncContentH, k);
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

    // Macro label row.
    const float labelSize = pxSize(8.5f * kt);
    const float labelSpacing = 1.45f * kt;
    const float labelY = y + 2.0f * k;
    spacedText(draw, mf, labelSize, ImVec2(lx, labelY), "YOUR MACRO", theme::Eyebrow,
               labelSpacing);
    const int count = static_cast<int>(gui.macros.size());
    char countBuf[48];
    std::snprintf(countBuf, sizeof(countBuf), "%d saved", count);
    const float countSize = pxSize(9.0f * kt);
    ImVec2 countW = mf ? mf->CalcTextSizeA(countSize, FLT_MAX, 0, countBuf) : ImVec2(0, 0);
    const float countX = lx + leftW - countW.x - 12.0f * k;
    // The "{n} saved" caption is purely informational: the library list is
    // opened only from the folder button next to the name field.
    draw->AddText(mf, countSize, ImVec2(countX, y + 3.0f * k),
                  theme::rgb(130, 140, 162), countBuf);
    y += 20.0f * k;

    // Macro search box.
    const float srchH = 52.0f * k;
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
    ImGui::PushStyleColor(ImGuiCol_Text, vec4(theme::rgb(228, 233, 246)));
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
    // Only an explicit toggle (folder button) opens it вЂ” typing in the name
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
            auto file = dir / (stem + ".skc");
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
    y += 1.0f + 9.0f * k;

    // Everything below the divider (TPS / Speed / Sync Music / HITBOXES /
    // LAYOUT MODE) becomes a single scrolling region.
    const float timingTop = y;
    y -= state().timingScroll;
    draw->PushClipRect(ImVec2(lx - 12.0f * k, timingTop),
                       ImVec2(lx + leftW + 16.0f * k, bottom), true);


    // TPS row. The last value stays visible/kept even while the toggle is off
    // (the value square and toggle just render disabled).
    // During playback (engineMode == MODE_EXECUTE), TPS is mandatory: the toggle
    // is locked ON and cannot be switched off by the user.
    const double prevTpsV = gui.tpsValue;
    const bool prevTpsOn = gui.tpsEnabled;
    float tpsV = static_cast<float>(gui.tpsValue);
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
        tpsV = isSkc ? bot.getTps() : static_cast<float>(engine->runtimeTickRate());
    }
    const float block1 = timingControl(draw, mf, ImVec2(lx, y), leftW,
                                       "tps", "TPS",
                                       tpsV, 1.0f, 1000000.0f, "%.0f", nullptr,
                                       tpsOn, k, 1.0f, !timingLocked);
    if (skcOwnsTiming) {
        // Read-only while an SKC session holds the clock.
        tpsOn = true;
        tpsV = bot.getTps();
    } else if (isPlaying) {
        // Read-only rate while playing — swallow any edits and keep toggle on.
        tpsOn = true;
        tpsV = static_cast<float>(engine->runtimeTickRate());
    } else {
        gui.tpsValue = static_cast<double>(tpsV);
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
    float fpsV = static_cast<float>(gui.fpsValue);
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
    const float macroFpsV = static_cast<float>(
        skkbot2::fps::target() > 0.0 ? skkbot2::fps::target() : 0.0);
    const bool macroFpsOn = skkbot2::fps::enabled();
    if (fpsMacro) {
        fpsV = macroFpsV;
        fpsOn = macroFpsOn;
    }
    // One thing to say, and it is the actionable one: how many physics steps this
    // frame rate asks for at the current TPS. With FPS at 0 the display follows
    // TPS, so this reads "1 st/f" - which is the fact that matters, without
    // spelling out the arithmetic the user already chose by leaving it at 0.
    static std::string fpsHint;
    {
        const double tickRate = skcOwnsTiming ? static_cast<double>(bot.getTps())
                             : isPlaying       ? static_cast<double>(engine->runtimeTickRate())
                                               : static_cast<double>(engine->tickRate);
        const double effFps = fpsV > 0.5 ? static_cast<double>(fpsV) : tickRate;
        if (tickRate > 0.0 && effFps > 0.0) {
            fpsHint = fmt::format(" {} st/f", (int)std::ceil(tickRate / effFps));
        } else {
            fpsHint.clear();
        }
    }
    const float blockFps = timingControl(draw, mf, ImVec2(lx, y), leftW,
                                         "fps", "FPS",
                                         fpsV, 0.0f, 1000.0f, "%.0f", "",
                                         fpsOn, k, 1.0f, !fpsLocked,
                                         fpsHint.c_str());
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

    // Divider between the timing controls and the hitbox switches, matching
    // the one above the TPS row.
    draw->AddLine(ImVec2(lx, y), ImVec2(lx + leftW, y), theme::Line, 1.0f);
    y += 1.0f + 9.0f * k;

    // The HITBOXES stack scrolls inside its own clip when the
    // sub-rows outgrow the body (only this block scrolls: the timing rows above
    // and the NOCLIP column on the right keep their fixed positions).

    // HITBOXES row вЂ” visual-only collision-box overlay (see
    // src/hacks/hitboxes.cpp). Same row geometry as the TPS/Speed rows but
    // toggle-only (no value field): the label on the left, 44x24k toggle on
    // the right, both centered in the 40k row.
    //
    // The FILL sub-row (filled vs outline-only boxes) eases in ONLY while the
    // master is on вЂ” same nested-reveal pattern as the NOCLIP accordion's
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

        // Master row.
        {
            const float textH2 = df ? df->CalcTextSizeA(titleSize2, FLT_MAX, 0.0f, "Hitboxes").y
                                    : ImGui::GetFontSize();
            const ImVec2 tPos2(lx, y + (rowH2 - textH2) * 0.5f);
            drawGradientText(draw, df, titleSize2, tPos2, "Hitboxes",
                             boxOn ? theme::rgb(222, 230, 245) : theme::rgb(179, 186, 204),
                             boxOn ? theme::rgb(240, 245, 255) : theme::rgb(179, 186, 204));
            const ImVec2 tog2(lx + leftW - togW2, y + (rowH2 - togH2) * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(lx, y));
            const bool rowClicked = ImGui::InvisibleButton("##beta_hb_row", ImVec2(leftW - togW2 - 4.0f * k, rowH2), blockedButtonFlags(!blocked));
            const bool togClicked = toggleAt("##beta_hitboxes", tog2, boxOn, !blocked);
            if ((togClicked || (rowClicked && !blocked))) {
                engine->showHitboxes = !boxOn;
                Mod::get()->setSavedValue("gui_hitboxes", engine->showHitboxes);
                skk::hitboxes::forceRefresh();
                // The hitbox overlay used to come on "by itself"; nothing else
                // writes this flag, so log every transition to pin down whether
                // a click landed here or the saved value came back wrong.
                skkbot2::log::info("skkBot2: hitboxes -> {}",
                    engine->showHitboxes ? "on" : "off");
            }
            y += rowH2;
        }

        // Fill sub-row: eases open with the master, and its control only takes
        // clicks once it is (almost) fully revealed вЂ” a still-hidden toggle
        // must not steal clicks (same 0.85f reveal gate as NOCLIP).
        // The 10k gap above the row lives INSIDE the eased block (NOCLIP's
        // extraH idiom), so a collapsed row leaves no dangling space, and the
        // row slides up with the reveal instead of jumping to a fixed offset.
        const float subGap = 10.0f * k;
        const float fillT = anim01(ImGui::GetID("##hb_rows"), boxOn ? 1.0f : 0.0f, 11.0f);
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
                             fillOn ? theme::rgb(222, 230, 245) : theme::rgb(179, 186, 204),
                             fillOn ? theme::rgb(240, 245, 255) : theme::rgb(179, 186, 204));
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
            const float tH = df ? df->CalcTextSizeA(titleSize2, FLT_MAX, 0.0f, "Trajectory").y
                                : ImGui::GetFontSize();
            const float tW = df ? df->CalcTextSizeA(titleSize2, FLT_MAX, 0.0f, "Trajectory").x
                                : 80.0f * k;
            const ImVec2 tPos(lx, y + (rowH2 - tH) * 0.5f);
            drawGradientText(draw, df, titleSize2, tPos, "Trajectory",
                             trajOn ? theme::rgb(222, 230, 245) : theme::rgb(179, 186, 204),
                             trajOn ? theme::rgb(240, 245, 255) : theme::rgb(179, 186, 204));

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
        const ImVec2 lmTPos(lx, y + (lmRowH - lmTextH) * 0.5f);
        drawGradientText(draw, ldf, lmSize, lmTPos, "Layout Mode",
                         lmOn ? theme::rgb(222, 230, 245) : theme::rgb(179, 186, 204),
                         lmOn ? theme::rgb(240, 245, 255) : theme::rgb(179, 186, 204));
        const ImVec2 lmTog(lx + leftW - lmTogW, y + (lmRowH - lmTogH) * 0.5f);
        if (toggleAt("##beta_layout_mode", lmTog, lmOn, !lmBlocked)) {
            engine->layoutMode = !lmOn;
            Mod::get()->setSavedValue("gui_layout_mode", engine->layoutMode);
            skkbot2::Logger::get().info("skkBot2: layout mode -> {}",
                                        engine->layoutMode ? "on" : "off");
        }
        y += lmRowH;
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
// override and a white outline. Visual only вЂ” no physics / input changes.
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
    const float y = top + 20.0f * k;

    const float rowH = 40.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float fieldW = 160.0f * k;
    const float gap = 10.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Trail").y
                           : ImGui::GetFontSize();

    const ImU32 capOn = theme::rgb(222, 228, 242);
    const ImU32 capOff = theme::rgb(179, 186, 204);

    const bool on = engine && engine->waveTrailFix;

    // Master row is always visible; sub-rows ease in only while on.
    const float rowsT = anim01(ImGui::GetID("##wt_rows"), on ? 1.0f : 0.0f, 11.0f);
    const float rowsH = rowH * 5.0f + gap * 4.0f;
    const float wtSeen = openT * (rowH + rowsT * rowsH);
    if (openT <= 0.001f || wtSeen <= 0.001f) return 20.0f * k;
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
                         active ? capOn : capOff, active ? theme::rgb(240, 245, 255) : capOff);
    };

    // Row 1 вЂ” master switch.
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

    // Row 2 вЂ” drag fix (streak geometry rebuild).
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

    // Row 3 вЂ” draw fix (stroke renderer).
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

    // Row 4 вЂ” trail opacity (0..100 %).
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

    // Row 5 вЂ” outline opacity (0..100 %), the white non-solid edge band.
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
    return 20.0f * k + seen;
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

    const float y0 = top;

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
    const ImU32 capOff = theme::rgb(179, 186, 204);

    // Three rows: Bot Engine, Format, Block Player Input. The old fourth (P2
    // Button) is gone - the button now comes from the file, not from a guess.
    // Counting it here left the body a full row taller than what is drawn, and
    // since the caller adds this return value to the next heading's y, the gap
    // showed up as a blank band ABOVE the following section's title rather than
    // anywhere inside this one.
    const float rowsH = (rowH + pillGap) * 2.0f + rowH;
    const float seen = openT * rowsH;
    if (openT <= 0.001f || seen <= 0.001f) return seen + kSectionGap * k;

    draw->PushClipRect(ImVec2(gridTL.x - 16.0f * k, y0 - 16.0f * k),
                       ImVec2(gridTL.x + width + 16.0f * k, y0 + seen + 16.0f * k), true);

    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH + pillGap + rowH) * 0.85f;
    const bool row3L = seen >= rowsH * 0.85f;

    float y = y0;

    // Row 1 — Bot Engine pills: SKK vs SKC
    {
        const float labelSize = pxSize(9.0f * kt);
        const float labelSpacing = 1.2f * kt;
        const float groupW = pillW * 2.0f + pillGap;
        const float gx = gridTL.x + width - groupW;
        const float by = y + (rowH - pillH) * 0.5f;

        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Bot Engine",
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));

        const bool isSkk = (gui.macroBotType == Gui::MacroBotType::SKK);
        if (selectionToggle("##set_bot_skk", ImVec2(gx, by), ImVec2(pillW, pillH), mf,
                            labelSize, labelSpacing, "SKK", isSkk, true, row1L)) {
            gui.macroBotType = Gui::MacroBotType::SKK;
            Mod::get()->setSavedValue("gui_macro_bot_type", static_cast<int>(Gui::MacroBotType::SKK));
            skkbot2::Logger::get().info("skkBot2: macro engine -> SKK (physics replay)");
        }
        if (selectionToggle("##set_bot_skc", ImVec2(gx + pillW + pillGap, by),
                            ImVec2(pillW, pillH), mf, labelSize, labelSpacing, "SKC",
                            !isSkk, true, row1L)) {
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
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));

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
        draw->AddText(mf, infoSize, textPos, theme::rgb(190, 202, 225), infoStr);

        y += rowH + pillGap;
    }

    // Row 3 — Block Player Input (Blocks physical clicks during SKC playback)
    {
        const bool ign = bot.getIgnoreInputs();
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Block Player Input",
                         ign ? theme::rgb(222, 228, 242) : capOff,
                         ign ? theme::rgb(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_bot_ignore_inputs", tog, ign, true, row3L)) {
            bot.setIgnoreInputs(!ign);
            Mod::get()->setSavedValue("gui_skc_ignore_inputs", bot.getIgnoreInputs());
            skkbot2::Logger::get().info("EclipseBot: ignore inputs -> {}",
                                        bot.getIgnoreInputs() ? "on" : "off");
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
    return seen + kSectionGap * k;
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
    const float y0 = top + 20.0f * k;

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
    const ImU32 capOff = theme::rgb(179, 186, 204);
    float y = y0;

    // SKC is an input-only format: its click stream is a whole number of frames
    // per Action and it deliberately carries no sub-tick offset field. With CBS
    // on, GD splits a step into two dt/2 half-steps, so "one frame" stops being
    // one frame and the recorded stream no longer maps onto the replayed
    // physics. Bot::forceVanillaAccuracy already pins these off on every
    // resetLevel; this blocks the SETTING so the UI cannot offer a choice the
    // bot silently overrides.
    const bool skcSelected =
        skkbot2::gui::get().macroBotType == Gui::MacroBotType::SKC;

    // Pick up CBS / COS from GD's own settings. This has to happen BEFORE the
    // collapsed-accordion early-return below: the section is collapsed by
    // default, so a sync placed after it would never run and the pills/toggle
    // would keep showing stale values until the user expanded ACCURACY.
    if (engine) {
        engine->syncAccuracyFromGame();
    }

    // Three rows: Mode pills, COS toggle, Safe Mode. This still counted four -
    // Auto Save Mod and the Freeze Attempts / Freeze Jumps sub-toggles are gone
    // - which over-reported the body by a row and pushed the next heading down.
    const float rowsH = (rowH + pillGap) * 2.0f + rowH;
    const float seen = openT * rowsH;
    if (openT <= 0.001f || seen <= 0.001f) return 20.0f * k + kSectionGap * k;

    draw->PushClipRect(ImVec2(gridTL.x - 16.0f * k, y0 - 16.0f * k),
                       ImVec2(gridTL.x + width + 16.0f * k, y0 + seen + 16.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH + pillGap + rowH) * 0.85f;

    // Row 1 вЂ” accuracy MODE: mutually exclusive VANILLA / CBS pills. Clicking
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
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));

        // SKC forces vanilla at runtime, so show VANILLA as the active pill and
        // grey both out rather than echoing a SKK-only setting the bot ignores.
        const bool isVanilla =
            skcSelected || engine->selectedAccuracyMode == AccuracyMode::Vanilla;
        if (selectionToggle("##set_acc_vanilla", ImVec2(gx, by), ImVec2(pillW, pillH), mf,
                            labelSize, labelSpacing, "VANILLA", isVanilla,
                            !skcSelected, row1L)) {
            engine->selectedAccuracyMode = AccuracyMode::Vanilla;
            Mod::get()->setSavedValue("gui_accuracy",
                                      static_cast<int>(AccuracyMode::Vanilla));
            ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                   engine->cosEnabled);
            skkbot2::Logger::get().info("skkBot2: accuracy mode -> Vanilla");
        }
        if (selectionToggle("##set_acc_cbs", ImVec2(gx + pillW + pillGap, by),
                            ImVec2(pillW, pillH), mf, labelSize, labelSpacing, "CBS",
                            !isVanilla, !skcSelected, row1L)) {
            engine->selectedAccuracyMode = AccuracyMode::CBS;
            Mod::get()->setSavedValue("gui_accuracy",
                                      static_cast<int>(AccuracyMode::CBS));
            ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                   engine->cosEnabled);
            skkbot2::Logger::get().info("skkBot2: accuracy mode -> CBS");
        }
        y += rowH + pillGap;
    }

    // Row 2 вЂ” COS (Click On Steps) toggle.
    {
        const bool cosOn = !skcSelected && engine->cosEnabled;
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Click On Steps",
                         cosOn ? theme::rgb(222, 228, 242) : capOff,
                         cosOn ? theme::rgb(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_acc_cos", tog, cosOn, !skcSelected, row2L)) {
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
                         engine->safeMode ? theme::rgb(222, 228, 242) : capOff,
                         engine->safeMode ? theme::rgb(240, 245, 255) : capOff);
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
    return 20.0f * k + seen + kSectionGap * k;
}

// ---------------------------------------------------------------------------
// KEYBIND section: the menu toggle key. A SIBLING section of ACCURACY вЂ” its
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
                         waiting ? theme::Accent : theme::rgb(222, 228, 242),
                         waiting ? theme::Accent : theme::rgb(240, 245, 255));

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
    const float y = top + 20.0f * k;

    const float rowH = 40.0f * k;
    const float rowGap = 10.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float fieldW = 160.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Solid").y
                           : ImGui::GetFontSize();
    const ImU32 capOff = theme::rgb(179, 186, 204);

    const float totalRowsH = rowH * 3.0f + rowGap * 2.0f;
    const float seen = openT * totalRowsH;
    if (openT <= 0.001f || seen <= 0.001f) return 20.0f * k + kSectionGap * k;  // heading only
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH * 2.0f + rowGap) * 0.85f;
    const bool row3L = seen >= totalRowsH * 0.85f;

    float yy = y;

    // Row 1 — solid (non-transparent) background switch.
    {
        const bool on = theme::solidBackground;
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Solid Background",
                         on ? theme::rgb(222, 228, 242) : capOff,
                         on ? theme::rgb(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_cos_solid", tog, on, true, row1L)) {
            theme::solidBackground = !on;
            Mod::get()->setSavedValue("gui_solid_background", theme::solidBackground);
            skkbot2::Logger::get().info("skkBot2: solid background -> {}",
                                        theme::solidBackground ? "on" : "off");
        }
        yy += rowH + rowGap;
    }

    // Row 2 — Menu Font picker + open fonts folder button
    {
        if (st.fontsDirty || st.fontItems.empty()) {
            refreshFontList();
        }

        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Font",
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));

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
                                       preview, st.fontItems, curIndex, row2L);
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
        const bool folderHover = row2L && ImGui::IsItemHovered();
        const bool folderDown = row2L && ImGui::IsItemActive();
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

    // Row 3 — Text Size (% multiplier across the menu)
    {
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Font Size",
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));

        const float fx = gridTL.x + width - fieldW;
        const float fy = yy + (rowH - 34.0f * k) * 0.5f;

        float pct = std::round(theme::fontScale * 100.0f);
        if (numberField(mf, "##set_cos_font_size", ImVec2(fx, fy), fieldW,
                        pct, 60.0f, 150.0f, "%", "%.0f", row3L, 5.0f)) {
            theme::fontScale = std::clamp(pct / 100.0f, 0.60f, 1.50f);
            Mod::get()->setSavedValue("gui_font_scale", theme::fontScale);
        }
        yy += rowH + rowGap;
    }

    draw->PopClipRect();
    return 20.0f * k + seen + kSectionGap * k;
}

// ---------------------------------------------------------------------------
float drawLoggingSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                         float k, float openT, bool hideHeading = false) {
    auto& logger = skkbot2::Logger::get();
    ImFont* mf = mono();
    const float kt = uiTextScale();
    State& st = state();

    // Page-level heading вЂ” same style/scale as the ACCURACY / KEYBIND headings.
    // hideHeading: see drawKeybindSection.
    if (!hideHeading &&
        sectionHeading(draw, gridTL, width, top, "LOGGING", st.loggingOpen, nullptr,
                       k, 1.0f, "settings_logging")) {
        st.loggingOpen = !st.loggingOpen;
    }
    const float y = top + 20.0f * k;

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
    const ImU32 capOff = theme::rgb(179, 186, 204);

    // Two nested reveals, both eased exactly like NOCLIP (rate 11):
    //  - openT  вЂ” the whole LOGGING block opens/closes from its heading.
    //  - rowsT  вЂ” the Split + Level rows appear ONLY while the "File Logging"
    //    master is on. With the master off the block shows just the master row.
    const float baseH = rowH;                              // master row
    const float extraH = rowH * 2.0f + 20.0f * k;          // gap + split + gap + level
    const bool fileOn = logger.isFileLogging();
    const float rowsT = anim01(ImGui::GetID("##beta_log_rows"),
                               fileOn ? 1.0f : 0.0f, 11.0f);
    const float seen = openT * (baseH + rowsT * extraH);
    if (openT <= 0.001f || seen <= 0.001f) return 20.0f * k;  // heading only (last in stack: no trailing gap)
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH + 10.0f * k + rowH) * 0.85f;
    const bool row3L = seen >= (rowH * 2.0f + 20.0f * k + rowH) * 0.85f;

    // Row 1 вЂ” File Logging master switch.
    float yy = y;
    {
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "File Logging",
                         fileOn ? theme::rgb(222, 228, 242) : capOff,
                         fileOn ? theme::rgb(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_log_file", tog, fileOn, true, row1L)) {
            logger.setFileLogging(!fileOn);
            Mod::get()->setSavedValue("gui_file_logging", logger.isFileLogging());
            skkbot2::Logger::get().info("skkBot2: file logging -> {}",
                                        logger.isFileLogging() ? "on" : "off");
        }
        yy += rowH + 10.0f * k;
    }

    // Rows 2-3 вЂ” Split Record/Play + Level picker. These exist only while the
    // master is on; rowsT collapses them away (the clip shrinks with it), and
    // each control is gated on rowsT so a hidden row never steals clicks.
    if (rowsT > 0.001f) {
        // Row 2 вЂ” Split Record/Play logs.
        {
            const bool splitOn = logger.isSplitFiles();
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Split Record/Play",
                             splitOn ? theme::rgb(222, 228, 242) : capOff,
                             splitOn ? theme::rgb(240, 245, 255) : capOff);
            ImVec2 tog(gridTL.x + width - toggleW, yy + (rowH - toggleH) * 0.5f);
            if (toggleAt("##set_log_split", tog, splitOn, true, row2L && rowsT > 0.85f)) {
                logger.setSplitFiles(!splitOn);
                Mod::get()->setSavedValue("gui_split_logs", logger.isSplitFiles());
                skkbot2::Logger::get().info("skkBot2: split logs -> {}",
                                            logger.isSplitFiles() ? "on" : "off");
            }
            yy += rowH + 10.0f * k;
        }

        // Row 3 вЂ” level picker: five mutually exclusive pills.
        {
            const float labelSize = pxSize(9.0f * kt);
            const float labelSpacing = 1.2f * kt;
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            const int curLevel = static_cast<int>(logger.getLevel());
            drawGradientText(draw, df, titleSize, tPos, "Level",
                             theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));

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
                                    i == curLevel, true, row3L && rowsT > 0.85f)) {
                    logger.setLevel(static_cast<skkbot2::Logger::Level>(i));
                    Mod::get()->setSavedValue("gui_log_level", i);
                    skkbot2::Logger::get().info("skkBot2: log level -> {}", kLevelNames[i]);
                }
            }
        }
    }
    draw->PopClipRect();
    return 20.0f * k + seen;
}

// ---------------------------------------------------------------------------
// RENDER tab вЂ” three peer sections (VIDEO / AUDIO / FILTER), exactly the shape
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
// only the rows and returns their eased height вЂ” same contract as
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
    const ImU32 capOff = theme::rgb(179, 186, 204);
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

    // Row 1 вЂ” Resolution (dropdown of presets; a custom WxH shows as-is).
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
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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

    // Row 2 вЂ” FPS.
    {
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "FPS",
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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

    // Row 3 вЂ” Bitrate (ABR target, Mbps).
    {
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Bitrate",
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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

    // Row 4 вЂ” Codec (the video encoder, by name).
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
                         curEnc >= 0 ? theme::rgb(222, 228, 242) : capOff,
                         curEnc >= 0 ? theme::rgb(240, 245, 255) : capOff);
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

    // Row 5 вЂ” Fade In (seconds; 0 = off). Video fade: the scene RGB is
    // multiplied by a ramp baked into the Y/UV passes (renderer.cpp
    // onFrameDraw).
    {
        float v = static_cast<float>(st.render.fadeInSeconds);
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Fade In",
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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

    // Row 6 вЂ” Fade Out (fades to black over the live trailer after the level
    // completes).
    {
        float v = static_cast<float>(st.render.fadeOutSeconds);
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Fade Out",
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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
                         on ? theme::rgb(222, 228, 242) : capOff,
                         on ? theme::rgb(240, 245, 255) : capOff);
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
    return seen + kSectionGap * k;
}

// ---------------------------------------------------------------------------
// AUDIO section: master capture toggle, music/SFX volumes, bitrate and codec.
// The volume/bitrate/codec rows exist ONLY while the master is on вЂ” the same
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
    const float y = top + 20.0f * k;

    const float rowH = 40.0f * k;
    const float rowGap = 10.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float fieldW = 160.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Music").y
                           : ImGui::GetFontSize();
    const ImU32 capOff = theme::rgb(179, 186, 204);

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

    // Nested reveal: master row always, the 6 dependent rows only while on
    // (music/sfx volumes, bitrate, codec, audio fade in/out).
    const float baseH = rowH;
    const float extraH = rowGap + rowH * 6.0f + rowGap * 5.0f;  // gap + 6 rows
    const float rowsT = anim01(ImGui::GetID("##beta_audio_rows"),
                               st.render.audioEnabled ? 1.0f : 0.0f, 11.0f);
    const float seen = openT * (baseH + rowsT * extraH);
    if (openT <= 0.001f || seen <= 0.001f) return 20.0f * k;
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH + rowGap + rowH) * 0.85f;
    const bool row3L = seen >= (2.0f * rowH + 2.0f * rowGap + rowH) * 0.85f;
    const bool row4L = seen >= (3.0f * rowH + 3.0f * rowGap + rowH) * 0.85f;
    const bool row5L = seen >= (4.0f * rowH + 4.0f * rowGap + rowH) * 0.85f;
    const bool row6L = seen >= (5.0f * rowH + 5.0f * rowGap + rowH) * 0.85f;
    const bool row7L = seen >= (6.0f * rowH + 6.0f * rowGap + rowH) * 0.85f;
    const bool depL = rowsT > 0.85f;  // dependent rows must be settled to click

    // Row 1 вЂ” Capture Audio master switch.
    float yy = y;
    {
        const bool on = st.render.audioEnabled;
        const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Capture Audio",
                         on ? theme::rgb(222, 228, 242) : capOff,
                         on ? theme::rgb(240, 245, 255) : capOff);
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

    // Rows 2-5 вЂ” only while the master is on (clipped away by rowsT).
    if (rowsT > 0.001f) {
        // Row 2 вЂ” Music Volume.
        {
            float v = st.render.musicVolume;
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Music Volume",
                             theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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

        // Row 3 вЂ” SFX Volume.
        {
            float v = st.render.sfxVolume;
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "SFX Volume",
                             theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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

        // Row 4 вЂ” Bitrate (kbps).
        {
            float v = static_cast<float>(st.render.audioBitrate);
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Bitrate",
                             theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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

        // Row 5 вЂ” Codec.
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
                             theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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

        // Row 6 вЂ” Audio Fade In (seconds; 0 = off). Ramps the captured mixer
        // output up over the start of the track вЂ” independent of the video
        // fade (renderer.cpp writeAudioBlock).
        {
            float v = static_cast<float>(st.render.audioFadeInSec);
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Fade In",
                             theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
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

        // Row 7 вЂ” Audio Fade Out (ramps to silence over the live trailer at
        // the end, mirroring the video fade-out).
        {
            float v = static_cast<float>(st.render.audioFadeOutSec);
            const ImVec2 tPos(gridTL.x, yy + (rowH - textH) * 0.5f);
            drawGradientText(draw, df, titleSize, tPos, "Fade Out",
                             theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));
            const float fx = gridTL.x + width - fieldW;
            const float fy = yy + (rowH - 34.0f * k) * 0.5f;
            if (numberField(mf, "##ren_afadeout", ImVec2(fx, fy), fieldW, v, 0.0f, 10.0f,
                            "s", "%.1f", row7L && depL, 0.1f)) {
                st.render.audioFadeOutSec = static_cast<double>(v);
                saveRenderSettings();
                applyRenderSettings();
                skkbot2::Logger::get().info("Render: audio fade out -> {:.1f}s", v);
            }
        }
    }
    draw->PopClipRect();
    return 20.0f * k + seen;
}

// ---------------------------------------------------------------------------
// Body scroll: the SETTINGS / RENDER tabs stack collapsible sections, and with
// everything open the stack outgrows the body viewport (rows would slide
// under the footer and become unreachable). The tab heading stays pinned;
// everything below it scrolls inside a clip. The scroll offset lives in State
// (persists across frames, since the content height is only known AFTER the
// sections are drawn вЂ” this runs post-draw to clamp it, handle the wheel and
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
    State& st = state();
    const float viewH = std::max(1.0f, viewBot - viewTop);
    const float maxScroll = std::max(0.0f, contentH - viewH);
    if (maxScroll <= 0.5f) {
        scroll = 0.0f;  // everything fits: never an offset
        return;
    }
    scroll = std::clamp(scroll, 0.0f, maxScroll);

    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(ImVec2(winMin.x, viewTop),
                                   ImVec2(winMax.x, viewBot)) &&
        ImGui::GetIO().MouseWheel != 0.0f) {
        const float step = 48.0f * k;
        scroll = std::clamp(scroll - ImGui::GetIO().MouseWheel * step,
                            0.0f, maxScroll);
        ImGui::GetIO().MouseWheel = 0.0f;
    }

    const float trackX = winMax.x - 14.0f * k;
    const float trackW = 5.0f * k;
    const float trackY0 = viewTop;
    const float trackY1 = viewBot;
    const float trackH = trackY1 - trackY0;
    const float thumbH = std::max(16.0f * k, trackH * viewH / contentH);
    const float thumbTravel = std::max(1.0f, trackH - thumbH);
    float thumbY = trackY0 + (scroll / maxScroll) * thumbTravel;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float hitPadX = 6.0f * k;
    const ImVec2 thumbMin(trackX - hitPadX, thumbY);
    const ImVec2 thumbMax(trackX + trackW + hitPadX, thumbY + thumbH);
    const bool hoveredThumb = ImGui::IsMouseHoveringRect(thumbMin, thumbMax);
    const bool hoveredTrack = ImGui::IsMouseHoveringRect(
        ImVec2(trackX - hitPadX, trackY0), ImVec2(trackX + trackW + hitPadX, trackY1));
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
            thumbY = trackY0 + (scroll / maxScroll) * thumbTravel;
        } else {
            dragging = false;
        }
    }

    const bool thumbActive = dragging || hoveredThumb;
    draw->AddRectFilled(ImVec2(trackX, trackY0), ImVec2(trackX + trackW, trackY1),
                        theme::rgb(24, 27, 34, 130), 3.0f * k);
    const ImU32 thumbCol = thumbActive ? theme::Accent : theme::rgb(140, 153, 182);
    draw->AddRectFilled(ImVec2(trackX, thumbY),
                        ImVec2(trackX + trackW, thumbY + thumbH),
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
// as applyBodyScroll, minus the section stack вЂ” a flat block of rows.
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
    State& st = state();
    applyRegionScroll(draw, winMin, winMax, viewTop, viewBot, contentH, k,
                      st.noclipScroll, st.noclipScrollDragging,
                      st.noclipScrollDragY, st.noclipScrollDragStart,
                      st.noclipScrollClick);
}

// ---------------------------------------------------------------------------
// RENDER action button: the pinned START / STOP control at the bottom of the
// RENDER tab. It mirrors the classic GUI's rules exactly вЂ” the label, colour
// and behaviour all follow the live renderer state:
//   - recording   -> "STOP RENDER" (coral), always enabled, stop() on click;
//   - not recording -> "START RENDER" (accent), enabled only when a render can
//     actually begin (a level is open, the engine is playing back a macro and
//     FFmpeg is loaded). Hovering a disabled button explains what's missing.
// The button is drawn OUTSIDE the scroll clip, pinned just above the footer, so
// it never scrolls away вЂ” the scroll viewport above is shortened to make room.
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
                        sideHover ? theme::rgb(226, 232, 244) : theme::rgb(185, 195, 215));
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

    // The Settings tab stacks three COLLAPSIBLE SECTIONS вЂ” ACCURACY, COSMETICS
    // and LOGGING вЂ” each an equal peer: its own page-level heading (always
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

        // ACCURACY rows вЂ” the "ACCURACY" heading above is this tab's page
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

    // The Render tab stacks two peer sections вЂ” VIDEO, AUDIO вЂ” the same shape
    // as the Settings tab: VIDEO's heading is the tab heading above, AUDIO
    // draws its own below; each has its own open/close
    // animation (rate 11) and stacks under the previous one's eased height.
    if (state().tab == Tab::Render) {
        initRenderTab();  // one-time: probe FFmpeg, load saved settings

        const float viewTop = y + 20.0f * k;
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
        // starts at the body's top edge вЂ” same slot the "ACCURACY" heading
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
    const auto path = ext.empty() ? dir / (stem + ".skc") : dir / (stem + ext);

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
    // gui.macros are already current here вЂ” no per-frame re-sync is needed. The
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
    if (scrollable && ImGui::IsMouseHoveringRect(tl, br) &&
        ImGui::GetIO().MouseWheel != 0.0f) {
        const float wd = ImGui::GetIO().MouseWheel;
        state().searchScroll =
            std::clamp(state().searchScroll + (wd > 0.0f ? -rowH : rowH), 0.0f, maxScroll);
        ImGui::GetIO().MouseWheel = 0.0f;
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

    const float labelY = tl.y + pad;
    spacedText(draw, mf, pxSize(8.0f * kt), ImVec2(tl.x + 10.0f * k, labelY),
               totalRows > 0 ? "FROM YOUR LIBRARY" : "A FRESH START",
               withAlpha(theme::rgb(152, 166, 190), st.searchOpenT), 1.2f * kt);

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

        const ImVec2 thumbMin(trackX, thumbY);
        const ImVec2 thumbMax(trackX + trackW, thumbY + thumbH);
        const ImVec2 trackMin(trackX, trackY0);
        const ImVec2 trackMax(trackX + trackW, trackY1);

        const ImVec2 mouse = ImGui::GetIO().MousePos;
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
            }
        }

        if (st.scrollDragging) {
            if (ImGui::IsMouseDown(0)) {
                const float dy = mouse.y - st.scrollDragStartY;
                const float scrollDelta = (dy / thumbTravel) * maxScroll;
                st.searchScroll = std::clamp(st.scrollDragStartScroll + scrollDelta, 0.0f, maxScroll);
                // Recompute thumbY for current frame draw
                thumbY = trackY0 + (st.searchScroll / maxScroll) * thumbTravel;
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
        draw->AddRectFilled(ImVec2(trackX, thumbY), ImVec2(trackX + trackW, thumbY + thumbH),
                            withAlpha(thumbCol, st.searchOpenT), 3.0f * k);
    }

    float ry = tl.y + pad + labelH;
    if (totalRows == 0) {
        const char* empty = "No matching macros. Press Record to create this one.";
        ImVec2 es = ImGui::GetFont()->CalcTextSizeA(pxSize(11.0f * kt), FLT_MAX, 0, empty);
        draw->AddText(ImGui::GetFont(), pxSize(11.0f * kt),
                      ImVec2(tl.x + (w - es.x) * 0.5f, ry + (emptyH - es.y) * 0.5f),
                      withAlpha(theme::rgb(177, 187, 208), st.searchOpenT), empty);
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
        const ImU32 nameCol = isDeleting ? theme::rgb(255, 230, 230) : theme::rgb(203, 214, 231);
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
                          withAlpha(theme::rgb(215, 224, 240), tsl), "CANCEL");

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
const auto replayPathSkk = dir / (std::string(name) + ".skk");
                auto replayPathSkc = dir / (std::string(name) + ".skc");
                std::error_code ec;
                // Delete ONLY the file this row points at. Removing both
                // extensions destroyed a same-named macro of the other engine.
                const auto delStem = ReplayEngine::macroEntryStem(name);
                const auto delExt = ReplayEngine::macroEntryExt(name);
                fs::remove(dir / (delStem + (delExt.empty() ? std::string(".skc") : delExt)), ec);
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
                Notification::create(fmt::format("Deleted macro: {}", name), NotificationIcon::Info)->show();
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
    if (!open) {
        // The window is gone: drop the search state so the dropdown never
        // resurrects stale (open / scrolled) on a later frame. Closing the
        // window must behave like pressing the folder button would.
        auto& v = state();
        v.searchVisible = false;
        v.searchForced = false;
        return;
    }

    auto& view = state();
    // COSMETICS: load the persisted solid-background flag once (default ON вЂ”
    // solid background). drawShell reads it every frame.
    if (!view.cosmeticsInit) {
        view.cosmeticsInit = true;
        theme::solidBackground = Mod::get()->getSavedValue<bool>("gui_solid_background", true);
        theme::fontScale = Mod::get()->getSavedValue<float>("gui_font_scale", 1.0f);
        if (theme::fontScale < 0.60f || theme::fontScale > 1.50f) {
            theme::fontScale = 1.0f;
        }
    }
    ImGui::GetIO().FontGlobalScale = theme::fontScale;
    // The engine reloads its macro list asynchronously (a save finishing on a
    // worker thread bumps macroListVersion). Cheap version check lets the shell
    // copy the fresh storedMacros вЂ” every frame is fine, no disk I/O here.
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
        view.initialized = true;
    }
    view.pos.x = std::clamp(view.pos.x, 8.0f, std::max(8.0f, display.x - width - 8.0f));
    view.pos.y = std::clamp(view.pos.y, 8.0f, std::max(8.0f, display.y - height - 8.0f));

    ImGui::SetNextWindowPos(view.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 28.0f * k);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##skk_quiet_beta", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);

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
    // Translucent header background (unified dark, glass-blending).
    draw->AddRectFilled(min, ImVec2(max.x, headerBot), theme::rgb(34, 46, 68, 150), 28.0f * k,
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
        view.dragGrab = false;
        view.dragMoved = false;
    }
    if (ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left] && mouseInWin &&
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

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::GetIO().FontGlobalScale = 1.0f;
    // Restore the global style snapshot so the beta theme doesn't leak into
    // other windows/popups (the classic GUI, GD menus) for the next frames.
    ImGui::GetStyle() = styleBefore;
}

} // namespace skkbot2::gui::beta_gui
