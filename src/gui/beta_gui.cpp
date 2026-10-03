#include "gui/beta_gui.hpp"
#include "gui/beta_actions.hpp"
#include "gui/beta_widgets.hpp"
#include "gui/blur.hpp"
#include "gui/skkbot_gui.hpp"

#include "skkBot.hpp"
#include "logger/logger.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace geode::prelude;

// Beta GUI — "abstract-modern-ui-redesign" shell. Layout mirrors the React
// mock (docs/abstract-modern-ui-redesign): 88px header (brand wordmark,
// centered mono top-tabs with 2px indicator, window actions), then
// a body region, then a 66px footer (engine status + shortcuts + INPUTS
// readout). Pure Dear ImGui (GD fork, 1.93.0).

namespace skkbot2::gui::beta_gui {
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
    bool settingsOpen = true;
    bool noclipOpen = true;        // NOCLIP accordion in the Record column
    bool frameStepperOpen = false; // FRAME STEPPER accordion in the Record column
    bool keybindOpen = true;       // KEYBIND accordion in the Settings tab
    bool cosmeticsOpen = true;     // COSMETICS accordion (under KEYBIND)
    bool loggingOpen = true;       // LOGGING accordion in the Settings tab
    bool cosmeticsInit = false;    // gui_solid_background loaded once

    bool waveTrailOpen = true;     // WAVE TRAIL FIX accordion in the Assist tab

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

    // RECORD tab: the HITBOXES stack in the left column can outgrow
    // the body (every sub-row open), so it scrolls inside its own clip. Same
    // contract as bodyScroll — separate state so the two tabs never fight.
    float recordScroll = 0.0f;
    bool recordScrollDragging = false;
    float recordScrollDragY = 0.0f;
    float recordScrollDragStart = 0.0f;
    bool recordScrollClick = false;

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
    std::vector<std::string> filteredInfo; // matching "12.3 KB · .skk" captions
    std::string filterQuery;               // query the caches were built for
    bool filterDirty = true;               // rebuild on next frame

    // Where the Speed row starts (absolute Y in the window). The macro list
    // grows down to this line and scrolls beyond it, so the speedhack controls
    // always stay visible under the panel.
    float speedTop = 0.0f;

    // Folder button press latch (press-toggle is edge-triggered on IsItemActive).
    bool folderDownPrev = false;

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
    // fully opaque blur slab under a clear shell — the menu would look flat and
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
    auto* engine = ReplayEngine::get();
    size_t cur = 0, total = 0;
    if (engine->activeSk) total = engine->activeSk->inputs.size();
    if (engine->activeMacro) total = std::max(total, engine->activeMacro->inputs.size());
    if (engine->engineMode == MODE_EXECUTE) cur = engine->executeIndex;
    else if (engine->engineMode == MODE_CAPTURE) cur = total;
    const bool live = engine->engineMode == MODE_EXECUTE || engine->engineMode == MODE_CAPTURE;

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
    // pulsing soft halo so the live state reads as alive — subtle, not strong.
    if (active && enabled) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.2f);
        drawSoftHalo(draw, pos, add(pos, size), effR, theme::Accent,
                     (0.16f + 0.14f * pulse) / 0.32f, k);
    }

    draw->AddRectFilled(pos, add(pos, size), bg, effR);
    // Liquid glass internal specular glint along top rim
    if (enabled) {
        draw->AddLine(ImVec2(pos.x + 8.0f * k, pos.y + 1.0f * k),
                      ImVec2(pos.x + size.x - 8.0f * k, pos.y + 1.0f * k),
                      theme::rgb(255, 255, 255, style == TransportStyle::Lime ? 55 : 42), 1.0f);
    }
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
    const float kbdW = 24.0f * k;
    const float kbdH = 25.0f * k;
    const float kx = pos.x + size.x - 22.0f * k - kbdW;
    draw->AddRect(ImVec2(kx, cy - kbdH * 0.5f), ImVec2(kx + kbdW, cy + kbdH * 0.5f),
                  enabled ? kbdB : withAlpha(kbdB, 0.4f), 28.0f * k);
    ImVec2 ks = mf ? mf->CalcTextSizeA(pxSize(10.0f * kt), FLT_MAX, 0, kbd) : ImGui::CalcTextSize(kbd);
    draw->AddText(mf, pxSize(10.0f * kt), ImVec2(kx + (kbdW - ks.x) * 0.5f, cy - ks.y * 0.5f),
                  enabled ? kbdT : withAlpha(kbdT, 0.4f), kbd);

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
    if (enabled) {
        draw->AddLine(ImVec2(pos.x + 4.0f * k, pos.y + 1.0f * k),
                      ImVec2(pos.x + size.x - 4.0f * k, pos.y + 1.0f * k),
                      theme::rgb(255, 255, 255, 32), 1.0f);
    }
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
// wheel anywhere over the row) and THEN the enable toggle. No range track — the
// value is entered directly, so the block is a single short row. Returns the
// row height so callers can stack rows.
float timingControl(ImDrawList* draw, ImFont* mf, ImVec2 pos, float width,
                    const char* uid, const char* title,
                    float& value, float vmin, float vmax, const char* fmt,
                    const char* suffix, bool& enabled,
                    float k, float wheelStep = 0.0f) {
    // Unique ID scope so two stacked rows (TPS + Speed) don't collide.
    ImGui::PushID(uid);
    (void)mf;  // title/values render in the default font; mono goes to callers
    // The macro dropdown paints an opaque panel over the whole Record page,
    // so while it is open the per-row inputs must not take clicks.
    const bool blocked = state().searchVisible;
    const bool uiOn = enabled && !blocked;
    const float rowH = 40.0f * k;
    ImFont* df = ImGui::GetFont();
    const float kt = uiTextScale();
    ImVec2 ts = df->CalcTextSizeA(pxSize(14.0f * kt), FLT_MAX, 0, title);
    const ImVec2 tPos(pos.x, pos.y + (rowH - ts.y) * 0.5f);
    const ImU32 c0 = uiOn ? theme::rgb(222, 230, 245) : theme::rgb(179, 186, 204);
    const ImU32 c1 = uiOn ? theme::rgb(240, 245, 255) : theme::rgb(179, 186, 204);
    drawGradientText(draw, df, pxSize(14.0f * kt), tPos, title, c0, c1);

    // Value square first, then the enable toggle — both vertically centered in
    // the row. The mouse wheel steps the value anywhere over the row (the rows
    // above the scrollable HITBOXES block own the wheel there) but NOT while
    // the field is being typed into. The row handler runs BEFORE the field's
    // own and zeroes the wheel, so only one step happens per notch.
    const float nfW = 100.0f * k;
    const float nfH = 34.0f * k;
    const float tgGap = 17.0f * k;
    const float tgW = 44.0f * k;
    const float nfX = pos.x + width - nfW - tgGap - tgW;
    const float tgX = pos.x + width - tgW;
    const bool editing = numberEditing(ImGui::GetID("##nfield"));
    if (uiOn && !editing && ImGui::IsMouseHoveringRect(pos, add(pos, ImVec2(width, rowH))) &&
        ImGui::GetIO().MouseWheel != 0.0f) {
        // One `wheelStep` per wheel EVENT (sign only, not raw magnitude): the
        // GD hook reports |MouseWheel| > 1 for a single physical notch, so
        // scaling by the magnitude skipped values (TPS 18 -> 20). Consume the
        // wheel so nothing else re-steps the same notch this frame.
        const float w = ImGui::GetIO().MouseWheel;
        const float step = wheelStep > 0.0f ? wheelStep : (vmax - vmin) * 0.02f;
        value = std::clamp(value + (w > 0.0f ? step : -step), vmin, vmax);
        ImGui::GetIO().MouseWheel = 0.0f;
    }

    float v1 = value;
    const bool nfChanged = numberField(mf, "##nfield",
                                       ImVec2(nfX, pos.y + (rowH - nfH) * 0.5f),
                                       nfW, v1, vmin, vmax, suffix, fmt, uiOn, wheelStep);
    const bool tgChanged = toggleAt("##ntoggle", ImVec2(tgX, pos.y + (rowH - 24.0f * k) * 0.5f),
                                    uiOn, true);
    if (tgChanged) enabled = !enabled;
    if (nfChanged) value = v1;

    ImGui::PopID();
    return rowH;
}

// RECORD tab scrollbar for the HITBOXES stack (defined below,
// after drawRecordPage — forward-declared here because the block that uses it
// comes first in the file).
void applyRecordScroll(ImDrawList* draw, ImVec2 winMin, ImVec2 winMax,
                       float viewTop, float viewBot, float contentH, float k);

// Right column of the Record tab: a compact NOCLIP accordion (a scaled-down
// clone of the page headings) that drives the engine's collision-bypass
// settings — master switch, P1/P2 targets, death flash. The values live on
// ReplayEngine and are persisted via the same gui_noclip* keys the classic
// GUI uses, so both shells stay in sync.
void drawNoClipSection(ImDrawList* draw, ImFont* mf, ImVec2 from, ImVec2 to, float k) {
    auto* engine = ReplayEngine::get();
    const float kt = uiTextScale();

    // Border between the columns.
    draw->AddLine(ImVec2(from.x - 22.0f * k, from.y), ImVec2(from.x - 22.0f * k, to.y),
                  theme::Line, 1.0f);

    const float width = to.x - from.x;
    float y = from.y;

    // Mini accordion heading (smaller "RECORD & PLAY" clone, always drawn so the
    // collapsed state can be reopened by clicking the row). While the macro list
    // (left column) is open it is treated as window-modal: nothing here takes
    // clicks, matching the transport / file / timing controls.
    const bool listOpen = state().searchVisible;
    State& st = state();
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
                           ImVec2(to.x + 16.0f * k, y + seen + 12.0f * k), true);
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

        // Row 3 — death flash. Hidden entirely while the master is off (rowsT
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

    // -----------------------------------------------------------------------
    // Mini accordion heading for FRAME STEPPER (placeholder under NOCLIP)
    // -----------------------------------------------------------------------
    // The user requested:
    // "Растояние между фрейм степпером и деат флеш должно быть таким же как и
    //  растояние между ноуклипом (который типа "RECORD & PLAY") и ноуклипом (который кнопка)!
    //  И в закрытом виде тоже!"
    //
    // In closed state (openT == 0, seen == 0):
    //   fsHeadingY = from.y + 27k (distance from NOCLIP heading to FRAME STEPPER is 27k).
    // In open state (openT == 1):
    //   Distance from Death Flash toggle to FRAME STEPPER heading line is 29.5k,
    //   matching the top distance from NOCLIP heading line to Noclip toggle (29.5k).
    // Animated smoothly without jumps or leaks.
    const float fsHeadingY = noclipContentY + seen + openT * 16.0f * k;
    if (sectionHeading(draw, from, width, fsHeadingY, "FRAME STEPPER", st.frameStepperOpen,
                       nullptr, k, 0.8f, "frame_stepper")) {
        if (!listOpen) st.frameStepperOpen = !st.frameStepperOpen;
    }

    const float fsOpenT = anim01(ImGui::GetID("##fs_accordion"),
                                 st.frameStepperOpen ? 1.0f : 0.0f, 11.0f);
    if (fsOpenT >= 0.002f) {
        const float fsContentY = fsHeadingY + 27.0f * k;
        const float fsSlotH = 34.0f * k;
        const float fsSeen = fsOpenT * fsSlotH;
        draw->PushClipRect(ImVec2(from.x - 2.0f * k, fsContentY),
                           ImVec2(to.x + 2.0f * k, fsContentY + fsSeen), true);
        const char* ph = "COMING SOON";
        const float phSize = pxSize(9.0f * kt);
        ImVec2 phSz = mf ? mf->CalcTextSizeA(phSize, FLT_MAX, 0.0f, ph) : ImGui::CalcTextSize(ph);
        spacedText(draw, mf, phSize,
                   ImVec2(from.x + (width - phSz.x) * 0.5f, fsContentY + (fsSlotH - phSz.y) * 0.5f),
                   ph, withAlpha(theme::rgb(146, 156, 176), fsOpenT), 1.2f * kt);
        draw->PopClipRect();
    }
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
    // Specular liquid top glint
    draw->AddLine(ImVec2(sb.x + 5.0f * k, sb.y + 1.0f * k),
                  ImVec2(eb.x - 5.0f * k, sb.y + 1.0f * k),
                  theme::rgb(255, 255, 255, 28), 1.0f);
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
    // Only an explicit toggle (folder button) opens it — typing in the name
    // field must NOT dump the whole library.
    state().searchVisible = state().searchForced;

    y += srchH + 10.0f * k;

    // Transport buttons.
    const bool recActive = engine->engineMode == MODE_CAPTURE ||
                           engine->pendingAction == ReplayEngine::PendingAction::Record;
    const bool playActive = engine->engineMode == MODE_EXECUTE ||
                            engine->pendingPlaybackStart ||
                            engine->pendingAction == ReplayEngine::PendingAction::Play;
    const bool renderBusy = render::Renderer::get().isRecording();
    const bool hasInputs = engine->hasMacroInputs();
    // While the macro list is open it is modal: everything under it is
    // registered disabled (with AllowOverlap) so the list swallows the clicks.
    const bool listOpen = state().searchVisible;
    const float tBtnW = (leftW - 12.0f * k) * 0.5f;
    const float tBtnH = 60.0f * k;

    const bool saveBusy = engine->macroSaveBusy();
    const bool idle = engine->engineMode == MODE_DISABLED && !renderBusy && !saveBusy && !recActive && !playActive;
    const bool canClear = idle && !saveBusy && (hasInputs || gui.macroName[0] != '\0') && !listOpen;

    if (transportButton(draw, "##beta_record", ImVec2(lx, y), ImVec2(tBtnW, tBtnH),
                        recActive ? "STOP" : "RECORD", "R",
                        !playActive && !renderBusy && !listOpen && !saveBusy, TransportStyle::Lime, recActive,
                        recActive ? 1 : 0, k)) {
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
    if (transportButton(draw, "##beta_play", ImVec2(lx + tBtnW + 12.0f * k, y),
                        ImVec2(tBtnW, tBtnH),
                        playActive ? "STOP" : "PLAY", "P",
                        !recActive && !renderBusy && !listOpen && !saveBusy && (hasInputs || playActive),
                        TransportStyle::Dark,
                        playActive, playActive ? 1 : 2, k)) {
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

    y += tBtnH + 6.0f * k;

    // File buttons (SAVE / LOAD).
    const float fBtnW = (leftW - 12.0f * k) * 0.5f;
    const float fBtnH = 40.0f * k;
    const char* saveLabel = saveBusy ? "SAVING..." : "SAVE";
    if (fileButton(draw, "##beta_save", ImVec2(lx, y), ImVec2(fBtnW, fBtnH), saveLabel, 0,
                   idle && hasInputs && gui.macroName[0] && !listOpen && !saveBusy, k)) {
        if (engine->activeSk) engine->activeSk->name = gui.macroName;
        if (engine->activeMacro) engine->activeMacro->name = gui.macroName;
        if (engine->saveActiveMacro()) {
            gui.refreshMacroList();
            skkbot2::Logger::get().info("skkBot2: saved macro '{}'", gui.macroName);
        }
    }
    if (fileButton(draw, "##beta_clear", ImVec2(lx + fBtnW + 12.0f * k, y),
                   ImVec2(fBtnW, fBtnH), "CLEAR", 2,
                   canClear, k)) {
        skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
        engine->haltExecution();
        engine->discardActiveMacro();
        gui.macroName[0] = '\0';
        gui.refreshMacroList();
    }

    y += fBtnH + 10.0f * k;
    // Timing controls divider.
    draw->AddLine(ImVec2(lx, y), ImVec2(lx + leftW, y), theme::Line, 1.0f);
    y += 1.0f + 9.0f * k;

    // TPS row. The last value stays visible/kept even while the toggle is off
    // (the value square and toggle just render disabled).
    // While a replay owns the clock (beginReplayAccuracyEnvironment applies the
    // macro's declared TPS on PLAY) the row shows that LIVE rate with the
    // toggle ON — the control must report the effective state, not look dead.
    // Turning the toggle off hands the rate back to the menu's own value
    // (dropPlaybackTickRateOverride) without disturbing the replay's accuracy.
    const double prevTpsV = gui.tpsValue;
    const bool prevTpsOn = gui.tpsEnabled;
    float tpsV = static_cast<float>(gui.tpsValue);
    bool tpsOn = gui.tpsEnabled;
    const bool playbackOverriding =
        engine->engineMode == MODE_EXECUTE && engine->playbackTickRateOverrideActive;
    if (playbackOverriding) {
        tpsOn = true;
        tpsV = static_cast<float>(engine->runtimeTickRate());
    }
    const float block1 = timingControl(draw, mf, ImVec2(lx, y), leftW,
                                       "tps", "TPS",
                                       tpsV, 1.0f, 1000000.0f, "%.0f", nullptr,
                                       tpsOn, k, 1.0f);
    if (playbackOverriding) {
        // The replay's rate is read-only here — swallow any edit the user
        // managed on the disabled field and keep the engine's rate intact.
        tpsV = static_cast<float>(engine->runtimeTickRate());
        if (!tpsOn) {
            // User turned the toggle off mid-playback: return to the menu TPS.
            engine->dropPlaybackTickRateOverride();
            tpsV = static_cast<float>(gui.tpsValue);
            tpsOn = gui.tpsEnabled;
        }
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

    // Speed row. The macro list expands down to this block, so remember where
    // it starts for the dropdown's height cap.
    state().speedTop = y;
    const double prevSpeedV = gui.speedhackValue;
    const bool prevSpeedOn = gui.speedhackEnabled;
    float speedV = static_cast<float>(gui.speedhackValue);
    bool speedOn = gui.speedhackEnabled;
    const float block2 = timingControl(draw, mf, ImVec2(lx, y), leftW,
                                       "speed", "Speed",
                                       speedV, 0.01f, 100.0f, "%.2f", nullptr,
                                       speedOn, k, 0.01f);
    gui.speedhackValue = static_cast<double>(speedV);
    engine->gameSpeed = speedOn ? gui.speedhackValue : 1.0;
    gui.speedhackEnabled = speedOn;
    if (gui.speedhackValue != prevSpeedV || gui.speedhackEnabled != prevSpeedOn) {
        Mod::get()->setSavedValue("gui_speedhack_enabled", gui.speedhackEnabled);
        Mod::get()->setSavedValue("gui_speedhack_value", gui.speedhackValue);
    }
    y += block2 + 11.0f * k;

    // Divider between the timing controls and the hitbox switches, matching
    // the one above the TPS row.
    draw->AddLine(ImVec2(lx, y), ImVec2(lx + leftW, y), theme::Line, 1.0f);
    y += 1.0f + 9.0f * k;

    // The HITBOXES stack scrolls inside its own clip when the
    // sub-rows outgrow the body (only this block scrolls: the timing rows above
    // and the NOCLIP column on the right keep their fixed positions).
    const float hbTop = y;
    draw->PushClipRect(ImVec2(lx - 12.0f * k, hbTop),
                       ImVec2(lx + leftW + 16.0f * k, bottom), true);
    y -= state().recordScroll;

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

        // Master row.
        {
            const float textH2 = df ? df->CalcTextSizeA(titleSize2, FLT_MAX, 0.0f, "Hitboxes").y
                                    : ImGui::GetFontSize();
            const ImVec2 tPos2(lx, y + (rowH2 - textH2) * 0.5f);
            drawGradientText(draw, df, titleSize2, tPos2, "Hitboxes",
                             boxOn ? theme::rgb(222, 230, 245) : theme::rgb(179, 186, 204),
                             boxOn ? theme::rgb(240, 245, 255) : theme::rgb(179, 186, 204));
            const ImVec2 tog2(lx + leftW - togW2, y + (rowH2 - togH2) * 0.5f);
            if (toggleAt("##beta_hitboxes", tog2, boxOn, !blocked)) {
                engine->showHitboxes = !boxOn;
                Mod::get()->setSavedValue("gui_hitboxes", engine->showHitboxes);
            }
            y += rowH2;
        }

        // Fill sub-row: eases open with the master, and its control only takes
        // clicks once it is (almost) fully revealed — a still-hidden toggle
        // must not steal clicks (same 0.85f reveal gate as NOCLIP).
        const float fillT = anim01(ImGui::GetID("##hb_rows"), boxOn ? 1.0f : 0.0f, 11.0f);
        const float seen = fillT * rowH2;
        if (fillT > 0.001f && seen > 0.001f) {
            draw->PushClipRect(ImVec2(lx - 12.0f * k, y - 12.0f * k),
                               ImVec2(lx + leftW + 16.0f * k, y + seen + 12.0f * k), true);
            const bool subL = seen >= rowH2 * 0.85f;
            const bool fillOn = boxOn && engine->hitboxFill;
            const ImVec2 tPos3(lx, y + (rowH2 - subH) * 0.5f);
            drawGradientText(draw, df, subSize, tPos3, "Fill",
                             fillOn ? theme::rgb(222, 230, 245) : theme::rgb(179, 186, 204),
                             fillOn ? theme::rgb(240, 245, 255) : theme::rgb(179, 186, 204));
            const ImVec2 tog3(lx + leftW - togW2, y + (rowH2 - togH2) * 0.5f);
            if (toggleAt("##beta_hitbox_fill", tog3, fillOn, boxOn && subL && !blocked)) {
                engine->hitboxFill = !engine->hitboxFill;
                Mod::get()->setSavedValue("gui_hitbox_fill", engine->hitboxFill);
            }
            draw->PopClipRect();
            y += seen;
        }
    }

    // TRAJECTORY block — same row geometry as the HITBOXES block above it:
    // master toggle, then the Length sub-row that eases in only while the
    // master is on.
    {
        const bool trajOn = engine->showTrajectory;
        const bool blocked = state().searchVisible;
        ImFont* df = ImGui::GetFont();
        const float kt2 = uiTextScale();
        const float titleSize2 = pxSize(14.0f * kt2);
        const float rowH2 = 40.0f * k;
        const float togW2 = 44.0f * k;
        const float togH2 = 24.0f * k;
        const float fieldW2 = 110.0f * k;
        const float subH2 = 34.0f * k;
        const float subSize2 = pxSize(13.0f * kt2);

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
            ImFont* bf = df ? df : ImGui::GetFont();
            const float badgeFontSize = pxSize(9.5f * kt2);
            const ImVec2 textSz = bf ? bf->CalcTextSizeA(badgeFontSize, FLT_MAX, 0.0f, "BETA")
                                     : ImVec2(24.0f * k, badgeFontSize);
            InkMeasure bm;
            const bool haveB = measureGlyphInk(bf, badgeFontSize, "BETA", bm);
            const float inkW = haveB ? (bm.x1 - bm.x0) : textSz.x;
            const float inkH = haveB ? (bm.y1 - bm.y0) : (badgeFontSize * 0.70f);

            // Generous proportional padding so text sits cleanly within the capsule
            const float padY = 4.0f * k;
            const float badgeH = std::round(inkH + padY * 2.0f);
            const float padX = std::round(badgeH * 0.5f + 3.0f * k);
            const float badgeW = std::round(inkW + padX * 2.0f);

            const ImVec2 bMin(lx + tW + 8.0f * k, y + (rowH2 - badgeH) * 0.5f);
            const ImVec2 bMax(bMin.x + badgeW, bMin.y + badgeH);
            const float bRadius = badgeH * 0.5f;

            draw->AddRectFilled(bMin, bMax, theme::rgb(55, 95, 175, 90), bRadius);
            draw->AddRect(bMin, bMax, theme::rgb(110, 165, 255, 180), bRadius, 0, 1.0f);

            const float bcx = bMin.x + badgeW * 0.5f;
            const float bcy = bMin.y + badgeH * 0.5f;
            float tx, ty;
            if (haveB) {
                tx = bcx - (bm.x0 + bm.x1) * 0.5f;
                ty = bcy - (bm.y0 + bm.y1) * 0.5f;
            } else {
                tx = bcx - textSz.x * 0.5f;
                ty = bcy - 0.43f * badgeFontSize;
            }
            draw->AddText(bf, badgeFontSize, ImVec2(std::round(tx), std::round(ty)),
                          theme::rgb(170, 215, 255), "BETA");

            const ImVec2 tg(lx + leftW - togW2, y + (rowH2 - togH2) * 0.5f);
            if (toggleAt("##beta_trajectory", tg, trajOn, !blocked)) {
                engine->showTrajectory = !trajOn;
                Mod::get()->setSavedValue("gui_trajectory", engine->showTrajectory);
                skkbot2::Logger::get().info("skkBot2: trajectory -> {}",
                                            engine->showTrajectory ? "on" : "off");
            }
            y += rowH2;
        }
    }

    // Close the HITBOXES scroll clip and run the scrollbar: the
    // content height is only known now, after the eased rows settled.
    // The thumb sits in the gutter BETWEEN the columns (colGap), never on top
    // of the right-aligned toggles of the left column.
    const float hbContentH = (y + state().recordScroll) - hbTop;
    draw->PopClipRect();
    applyRecordScroll(draw, ImVec2(lx, hbTop), ImVec2(lx + leftW + 20.0f * k, bottom),
                      hbTop, bottom, hbContentH, k);
    y = hbTop + hbContentH;

    // Right column: NOCLIP controls (master, P1/P2 targets, death flash).
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

    if (sectionHeading(draw, gridTL, width, top, "COSMETIC", st.waveTrailOpen,
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
    const float seen = openT * (rowH + rowsT * rowsH);
    if (openT <= 0.001f || seen <= 0.001f) return 20.0f * k;
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    auto rowNL = [seen, rowH, gap](int n) -> bool {
        return seen >= (rowH + gap * static_cast<float>(n)) * 0.85f;
    };

    auto caption = [&](float ry, const char* text, bool active) {
        const ImVec2 tPos(gridTL.x, ry + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, text,
                         active ? capOn : capOff, active ? theme::rgb(240, 245, 255) : capOff);
    };

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
    return 20.0f * k + seen;
}

// ---------------------------------------------------------------------------
// SETTINGS page: accuracy mode (Vanilla / CBS) + COS. The tab's single
// collapsible heading is the page-level "ACCURACY" row itself (drawn by
// drawBody, same slot as "RECORD & PLAY"), so this page owns only the rows
// underneath it. Rows mirror the NOCLIP accordion exactly: same 40k height,
// same pill (100x34k) and toggle (44x24k) boxes, same caption typography.
// Returns the eased height the rows occupy, so the KEYBIND / LOGGING sections
// below can stack under it and slide with the same animation.
// ---------------------------------------------------------------------------
float drawSettingsPage(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                       float bottom, float k, float openT) {
    auto* engine = ReplayEngine::get();
    ImFont* mf = mono();
    const float kt = uiTextScale();

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
    float y = top;

    // Accordion reveal, eased exactly like NOCLIP (rate 11): rows grow/shrink
    // inside a clip that follows the eased open height, and each control only
    // registers once it is (almost) fully revealed — a hidden control must not
    // steal clicks.
    const float rowsH = rowH * 2.0f + pillGap;  // mode row + gap + COS row
    const float seen = openT * rowsH;
    if (openT <= 0.001f || seen <= 0.001f) return 0.0f;

    if (engine) {
        engine->syncAccuracyFromGame();
    }

    draw->PushClipRect(ImVec2(gridTL.x - 16.0f * k, y - 16.0f * k),
                       ImVec2(gridTL.x + width + 16.0f * k, y + seen + 16.0f * k), true);
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
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));

        const bool isVanilla = engine->selectedAccuracyMode == AccuracyMode::Vanilla;
        if (selectionToggle("##set_acc_vanilla", ImVec2(gx, by), ImVec2(pillW, pillH), mf,
                            labelSize, labelSpacing, "VANILLA", isVanilla, true, row1L)) {
            engine->selectedAccuracyMode = AccuracyMode::Vanilla;
            Mod::get()->setSavedValue("gui_accuracy",
                                      static_cast<int>(AccuracyMode::Vanilla));
            ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                   engine->cosEnabled);
            skkbot2::Logger::get().info("skkBot2: accuracy mode -> Vanilla");
        }
        if (selectionToggle("##set_acc_cbs", ImVec2(gx + pillW + pillGap, by),
                            ImVec2(pillW, pillH), mf, labelSize, labelSpacing, "CBS",
                            !isVanilla, true, row1L)) {
            engine->selectedAccuracyMode = AccuracyMode::CBS;
            Mod::get()->setSavedValue("gui_accuracy",
                                      static_cast<int>(AccuracyMode::CBS));
            ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                   engine->cosEnabled);
            skkbot2::Logger::get().info("skkBot2: accuracy mode -> CBS");
        }
        y += rowH + pillGap;
    }

    // Row 2 — COS (Click On Steps) toggle.
    {
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Click On Steps",
                         engine->cosEnabled ? theme::rgb(222, 228, 242) : capOff,
                         engine->cosEnabled ? theme::rgb(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_acc_cos", tog, engine->cosEnabled, true, row2L)) {
            engine->cosEnabled = !engine->cosEnabled;
            Mod::get()->setSavedValue("gui_cos", engine->cosEnabled);
            ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                   engine->cosEnabled);
            skkbot2::Logger::get().info("skkBot2: COS -> {}",
                                       engine->cosEnabled ? "on" : "off");
        }
    }
    draw->PopClipRect();
    return seen;
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
    State& st = state();

    // Page-level heading — same style/scale as the ACCURACY heading drawn by
    // drawBody, so the three settings sections read as peers. Unique idStr so
    // the heading's button ID never collides with the others.
    if (sectionHeading(draw, gridTL, width, top, "KEYBIND", st.keybindOpen, nullptr,
                       k, 1.0f, "settings_keybind")) {
        st.keybindOpen = !st.keybindOpen;
    }
    const float y = top + 20.0f * k;

    const float rowH = 40.0f * k;
    const float pillW = 100.0f * k;
    const float pillH = 34.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Menu").y
                           : ImGui::GetFontSize();

    // Rows reveal exactly like the ACCURACY page (rate 11): the heading stays
    // put, the row eases in/out inside a clip that follows the eased height,
    // and the control only registers once it is (almost) fully revealed.
    const float seen = openT * rowH;
    if (openT <= 0.001f || seen <= 0.001f) return 20.0f * k;  // heading only
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;

    // Row — menu toggle key.
    {
        const float labelSize = pxSize(9.0f * kt);
        const float labelSpacing = 1.2f * kt;
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Open Menu",
                         theme::rgb(222, 228, 242), theme::rgb(240, 245, 255));

        const float gx = gridTL.x + width - pillW;
        const float by = y + (rowH - pillH) * 0.5f;
        const char* keyLabel = gui.keybindWaiting ? "PRESS A KEY"
                                                  : Gui::keyName(gui.menuKeybind);
        if (selectionToggle("##set_kb_menu", ImVec2(gx, by), ImVec2(pillW, pillH), mf,
                            labelSize, labelSpacing, keyLabel, gui.keybindWaiting,
                            true, row1L)) {
            // Arming capture here; the press itself lands in Gui::handleKey,
            // which stores the key, persists it and clears the flag.
            gui.keybindWaiting = !gui.keybindWaiting;
            skkbot2::Logger::get().info("skkBot2: keybind capture {}",
                                        gui.keybindWaiting ? "armed" : "cancelled");
        }
    }
    draw->PopClipRect();
    return 20.0f * k + seen;
}

// ---------------------------------------------------------------------------
// COSMETICS section: menu appearance toggles. A SIBLING section of KEYBIND /
// LOGGING — same heading style, same open/close animation. One row: the
// transparent-background (Liquid Glass blur) master switch. Default OFF = the
// shipped translucency; ON = flat solid panel (blur disabled).
// ---------------------------------------------------------------------------
float drawCosmeticsSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                           float k, float openT) {
    ImFont* mf = mono();
    const float kt = uiTextScale();
    State& st = state();

    if (sectionHeading(draw, gridTL, width, top, "COSMETICS", st.cosmeticsOpen, nullptr,
                       k, 1.0f, "settings_cosmetics")) {
        st.cosmeticsOpen = !st.cosmeticsOpen;
    }
    const float y = top + 20.0f * k;

    const float rowH = 40.0f * k;
    const float toggleW = 44.0f * k;
    const float toggleH = 24.0f * k;
    const float titleSize = pxSize(14.0f * kt);
    ImFont* df = ImGui::GetFont();
    const float textH = df ? df->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Solid").y
                           : ImGui::GetFontSize();
    const ImU32 capOff = theme::rgb(179, 186, 204);

    const float seen = openT * rowH;
    if (openT <= 0.001f || seen <= 0.001f) return 20.0f * k;  // heading only
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;

    // Row — solid (non-transparent) background switch.
    {
        const bool on = theme::solidBackground;
        const ImVec2 tPos(gridTL.x, y + (rowH - textH) * 0.5f);
        drawGradientText(draw, df, titleSize, tPos, "Solid Background",
                         on ? theme::rgb(222, 228, 242) : capOff,
                         on ? theme::rgb(240, 245, 255) : capOff);
        ImVec2 tog(gridTL.x + width - toggleW, y + (rowH - toggleH) * 0.5f);
        if (toggleAt("##set_cos_solid", tog, on, true, row1L)) {
            theme::solidBackground = !on;
            Mod::get()->setSavedValue("gui_solid_background", theme::solidBackground);
            skkbot2::Logger::get().info("skkBot2: solid background -> {}",
                                        theme::solidBackground ? "on" : "off");
        }
    }
    draw->PopClipRect();
    return 20.0f * k + seen;
}

// ---------------------------------------------------------------------------
float drawLoggingSection(ImDrawList* draw, ImVec2 gridTL, float width, float top,
                         float k, float openT) {
    auto& logger = skkbot2::Logger::get();
    ImFont* mf = mono();
    const float kt = uiTextScale();
    State& st = state();

    // Page-level heading — same style/scale as the ACCURACY / KEYBIND headings.
    if (sectionHeading(draw, gridTL, width, top, "LOGGING", st.loggingOpen, nullptr,
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
    //  - openT  — the whole LOGGING block opens/closes from its heading.
    //  - rowsT  — the Split + Level rows appear ONLY while the "File Logging"
    //    master is on. With the master off the block shows just the master row.
    const float baseH = rowH;                              // master row
    const float extraH = rowH * 2.0f + 20.0f * k;          // gap + split + gap + level
    const bool fileOn = logger.isFileLogging();
    const float rowsT = anim01(ImGui::GetID("##beta_log_rows"),
                               fileOn ? 1.0f : 0.0f, 11.0f);
    const float seen = openT * (baseH + rowsT * extraH);
    if (openT <= 0.001f || seen <= 0.001f) return 20.0f * k;  // heading only
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, y - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, y + seen + 12.0f * k), true);
    const bool row1L = seen >= rowH * 0.85f;
    const bool row2L = seen >= (rowH + 10.0f * k + rowH) * 0.85f;
    const bool row3L = seen >= (rowH * 2.0f + 20.0f * k + rowH) * 0.85f;

    // Row 1 — File Logging master switch.
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

    // Rows 2-3 — Split Record/Play + Level picker. These exist only while the
    // master is on; rowsT collapses them away (the clip shrinks with it), and
    // each control is gated on rowsT so a hidden row never steals clicks.
    if (rowsT > 0.001f) {
        // Row 2 — Split Record/Play logs.
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

        // Row 3 — level picker: five mutually exclusive pills.
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
        renderer.loadFFmpeg();
        skkbot2::Logger::get().info("Render: FFmpeg loaded, {} encoder(s) available",
                                    renderer.availableEncoders().size());
    }
    if (st.renderEncoder.empty()) {
        const auto& encoders = renderer.availableEncoders();
        const char* kPref[] = { "h264_nvenc", "h264_amf", "h264_qsv", "libx264" };
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
    const ImU32 capOff = theme::rgb(179, 186, 204);

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

    const float rowsH = rowH * 6.0f + rowGap * 5.0f;  // res+fps+bitrate+codec+fade in+fade out
    const float seen = openT * rowsH;
    if (openT <= 0.001f || seen <= 0.001f) return 0.0f;
    draw->PushClipRect(ImVec2(gridTL.x - 12.0f * k, top - 12.0f * k),
                       ImVec2(gridTL.x + width + 12.0f * k, top + seen + 12.0f * k), true);
    const bool rowL = seen >= rowsH * 0.85f;  // all rows unlock together at the end
    float y = top;

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

    // Row 2 — FPS.
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

    // Row 3 — Bitrate (ABR target, Mbps).
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

    // Row 5 — Fade In (seconds; 0 = off). Video fade: the scene RGB is
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

    // Row 6 — Fade Out (fades to black over the live trailer after the level
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
    }
    draw->PopClipRect();
    return seen;
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

    // Row 1 — Capture Audio master switch.
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

    // Rows 2-5 — only while the master is on (clipped away by rowsT).
    if (rowsT > 0.001f) {
        // Row 2 — Music Volume.
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

        // Row 3 — SFX Volume.
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

        // Row 4 — Bitrate (kbps).
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

        // Row 6 — Audio Fade In (seconds; 0 = off). Ramps the captured mixer
        // output up over the start of the track — independent of the video
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

        // Row 7 — Audio Fade Out (ramps to silence over the live trailer at
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
    const ImVec2 thumbMin(trackX, thumbY);
    const ImVec2 thumbMax(trackX + trackW, thumbY + thumbH);
    const bool hoveredThumb = ImGui::IsMouseHoveringRect(thumbMin, thumbMax);
    const bool hoveredTrack = ImGui::IsMouseHoveringRect(
        ImVec2(trackX, trackY0), ImVec2(trackX + trackW, trackY1));

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
// as applyBodyScroll, minus the section stack — a flat block of rows.
void applyRecordScroll(ImDrawList* draw, ImVec2 winMin, ImVec2 winMax,
                       float viewTop, float viewBot, float contentH, float k) {
    State& st = state();
    applyRegionScroll(draw, winMin, winMax, viewTop, viewBot, contentH, k,
                      st.recordScroll, st.recordScrollDragging,
                      st.recordScrollDragY, st.recordScrollDragStart,
                      st.recordScrollClick);
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
    auto* engine = ReplayEngine::get();

    const bool recording = renderer.isRecording();
    const bool ffLoaded = renderer.isFFmpegLoaded();
    const bool inPlay = PlayLayer::get() && PlayLayer::get()->m_level &&
                        engine->engineMode == MODE_EXECUTE && engine->activeSk;
    const bool canStart = !recording && ffLoaded && inPlay;

    const float btnH = 40.0f * k;
    const char* label = recording ? "STOP RENDER" : "START RENDER";
    const bool enabled = recording || canStart;
    // Recording uses the coral Record colour (its semantic), Start uses the
    // accent fill. A disabled button keeps its shape but dims.
    const ImU32 fill = recording ? theme::Record : theme::Accent;
    const ImU32 onFill = recording ? theme::rgb(255, 175, 155) : theme::AccentLight;

    const ImVec2 size(width, btnH);
    const bool fired = buttonPrimary("##beta_render_action", pos, size, label,
                                     nullptr, enabled, fill, onFill);
    if (fired) {
        if (recording) {
            skkbot2::gui::stopRender();
        } else {
            skkbot2::gui::startRender();
        }
    }

    // Manual hit-test backup (BUG-20): on the FIRST click after the window
    // appears or after a drag, this fork's imgui can fail to register the
    // InvisibleButton as hovered/active, so the click never reaches the
    // buttonPrimary callback AND IsAnyItemActive() stays false, letting the
    // drag gate turn the press into a window grab instead. The rect test
    // below suppresses the drag gate every time the press lands inside the
    // button, and — only when ImGui never delivered the click (fired==false
    // and no item went active) — delivers the action itself, so the first
    // click always starts/stops the render.
    const bool pressInRect = enabled
        && ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left]
        && ImGui::IsMouseHoveringRect(pos, add(pos, size));
    if (pressInRect) {
        state().actionBtnClick = true;
        if (!fired && !ImGui::IsAnyItemActive()) {
            if (recording) {
                skkbot2::gui::stopRender();
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
        case Tab::Settings: head = "ACCURACY"; open = &state().settingsOpen; break;
    }

    const float y = min.y + kBodyPadTop * k;
    if (drawPageHeading && sectionHeading(draw, ImVec2(left, 0), width, y, head, *open,
                                          nullptr, k)) {
        *open = !*open;
    }

    // The Settings tab stacks four COLLAPSIBLE SECTIONS — ACCURACY, KEYBIND,
    // COSMETICS and LOGGING — each an equal peer: its own page-level heading (always
    // visible and clickable) and its own open/close animation eased at the
    // same rate 11. They are NOT nested: KEYBIND and LOGGING stay reachable
    // with ACCURACY closed, and every section's top sits at the previous one's
    // eased bottom, so opening/closing one slides the ones below it.
    if (state().tab == Tab::Settings) {
        const float viewTop = y + 20.0f * k;
        const float viewBot = max.y - kBodyPadBottom * k;

        // ACCURACY rows — the "ACCURACY" heading above is this tab's page
        // heading, drawn by drawBody. Only its rows ease in/out.
        const float setT = anim01(ImGui::GetID("##beta_settings_open"),
                                  state().settingsOpen ? 1.0f : 0.0f, 11.0f);
        // The whole section stack scrolls inside the body viewport (the tab
        // heading stays pinned above), so every section is drawn shifted up by
        // the persistent scroll offset and clipped to the viewport.
        float curY = viewTop - state().bodyScroll;
        draw->PushClipRect(ImVec2(left - 16.0f * k, viewTop - 16.0f * k),
                           ImVec2(max.x + 16.0f * k, viewBot), true);
        // No inter-section gap: the heading band is 20k with the caption
        // optically centered, so the caption-to-caption distance from one
        // section's LAST row to the NEXT heading equals the distance from a
        // heading to its FIRST row (30k both ways) — the stack reads as one
        // even rhythm, "no divisions".
        curY += drawSettingsPage(draw, ImVec2(left, curY), width, curY, viewBot,
                                 k, setT);

        // KEYBIND — own heading + rows.
        const float kbT = anim01(ImGui::GetID("##beta_keybind_open"),
                                 state().keybindOpen ? 1.0f : 0.0f, 11.0f);
        curY += drawKeybindSection(draw, ImVec2(left, curY), width, curY, k, kbT);

        // COSMETICS — own heading + rows (under KEYBIND, above LOGGING).
        const float cosT = anim01(ImGui::GetID("##beta_cosmetics_open"),
                                  state().cosmeticsOpen ? 1.0f : 0.0f, 11.0f);
        curY += drawCosmeticsSection(draw, ImVec2(left, curY), width, curY, k, cosT);

        // LOGGING — own heading + rows (with its nested File Logging reveal).
        const float lgT = anim01(ImGui::GetID("##beta_logging_open"),
                                 state().loggingOpen ? 1.0f : 0.0f, 11.0f);
        curY += drawLoggingSection(draw, ImVec2(left, curY), width, curY, k, lgT);
        draw->PopClipRect();

        // Scroll runs post-draw: the eased stack height (incl. the last section)
        // is only known now, so the clamp + wheel + scrollbar happen here.
        applyBodyScroll(draw, min, max, viewTop, viewBot,
                        (curY + state().bodyScroll) - viewTop, k);
        return;
    }

    // The Render tab stacks two peer sections — VIDEO, AUDIO — the same shape
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
std::string macroSmallInfo(const char* name) {
    namespace fs = std::filesystem;
    const auto path = Mod::get()->getSaveDir() / "replays" / (std::string(name) + ".skk");
    std::error_code ec;
    const auto sz = fs::file_size(path, ec);
    if (ec) return "saved in library";
    char buf[48];
    if (sz >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB  ·  .skk", static_cast<double>(sz) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f KB  ·  .skk", static_cast<double>(sz) / 1024.0);
    }
    return buf;
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
    // Specular top highlight
    draw->AddLine(ImVec2(tl.x + 6.0f * k, tl.y + 1.0f * k),
                  ImVec2(br.x - 6.0f * k, tl.y + 1.0f * k),
                  theme::rgb(255, 255, 255, static_cast<int>(35 * st.searchOpenT)), 1.0f);
    draw->AddRect(tl, br, withAlpha(theme::ModalBorder, st.searchOpenT), 28.0f * k, 0, 1.0f);

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
                std::snprintf(gui.macroName, sizeof(gui.macroName), "%s", name);
                skkbot2::gui::loadMacroIntoMemory(name);
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
                const auto replayPath = Mod::get()->getSaveDir() / "replays" / (std::string(name) + ".skk");
                std::error_code ec;
                fs::remove(replayPath, ec);
                if (std::strcmp(gui.macroName, name) == 0) {
                    gui.macroName[0] = '\0';
                }
                if (engine && engine->activeSk && engine->activeSk->name == name) {
                    engine->discardActiveMacro();
                }
                if (engine && engine->activeMacro && engine->activeMacro->name == name) {
                    engine->discardActiveMacro();
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
    // COSMETICS: load the persisted solid-background flag once (default ON —
    // solid background). drawShell reads it every frame.
    if (!view.cosmeticsInit) {
        view.cosmeticsInit = true;
        theme::solidBackground = Mod::get()->getSavedValue<bool>("gui_solid_background", true);
    }
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
    view.recordScrollClick = false;
    view.actionBtnClick = false;

    // Refresh the library once when switching into RECORD.
    if (view.lastTab != view.tab) {
        view.lastTab = view.tab;
        // Each tab starts at the top of its own scrollable stack.
        view.bodyScroll = 0.0f;
        view.recordScroll = 0.0f;
        if (view.tab == Tab::Record) {
            skkbot2::gui::get().refreshMacroList();
            view.filterDirty = true;
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
        !view.recordScrollClick &&
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
    // Restore the global style snapshot so the beta theme doesn't leak into
    // other windows/popups (the classic GUI, GD menus) for the next frames.
    ImGui::GetStyle() = styleBefore;
}

} // namespace skkbot2::gui::beta_gui
