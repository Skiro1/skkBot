#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// kNoPadWithHalfSpacing is 1 << 26 - it is NOT in the
// public ImGuiSelectableFlags_ enum in 1.93, it lives in imgui_internal.h
// (imgui_internal.h:1108) right next to the Selectable() code that reads it
// (imgui_widgets.cpp:7428). We mirror the value instead of including the
// header, because imgui_internal.h:499 calls `log(x)` and becomes ambiguous
// against geode::prelude's logger in every TU that has already pulled Geode
// in. blur.cpp gets away with it only because it includes the header first,
// before any `using namespace geode::prelude` - an order we must not depend on.
//
// On an ImGui bump, re-read imgui_internal.h:1108. A stale value here fails
// silently (the mask simply does not match), so the static_assert below is the
// only guard we get: it fires if the bit ever collides with a public flag.
inline constexpr ImGuiSelectableFlags kNoPadWithHalfSpacing = 1 << 26;
static_assert((kNoPadWithHalfSpacing &
               (ImGuiSelectableFlags_None | ImGuiSelectableFlags_NoAutoClosePopups |
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                ImGuiSelectableFlags_Disabled | ImGuiSelectableFlags_AllowOverlap |
                ImGuiSelectableFlags_Highlight | ImGuiSelectableFlags_SelectOnNav)) == 0,
              "kNoPadWithHalfSpacing collides with a public ImGuiSelectableFlags bit");

// Shared building blocks for the redesigned beta GUI. The palette and helpers
// mirror `docs/abstract-modern-ui-redesign` (graphite shell + lime accent,
// mono eyebrow typography, letter-spaced labels). Pure Dear ImGui
// (GD fork, 1.93.0): ImDrawList + standard widgets, no DOM, no third-party
// UI libs.

namespace skkbot2::gui::beta_widgets {

namespace theme {
constexpr ImU32 rgb(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

// Liquid Glass palette: translucent layered glass, glossy specular highlights,
// deep oceanic/sapphire tints and luminous cyan/azure glow.
// Apple iOS style Liquid Glass: significantly more transparent (alpha ~110-155) so the background game is clearly visible.
//
// ACCENT FAMILY + BACKGROUND ALPHAS: fixed shipped values (the CUSTOMIZATION
// section was removed 26.09, so nothing retunes these at runtime).
constexpr ImU32 Bg            = rgb(16, 22, 36, 130);         // highly translucent deep liquid canvas
constexpr ImU32 ShellTop      = rgb(26, 40, 68, 145);         // glass gradient top
constexpr ImU32 ShellBot      = rgb(17, 24, 40, 155);         // glass gradient bottom
constexpr ImU32 Frame         = rgb(18, 26, 44, 120);         // translucent panel
constexpr ImU32 Line          = rgb(200, 225, 255, 38);       // glass separator
constexpr ImU32 LineFooter    = rgb(200, 225, 255, 48);       // glass footer rule
constexpr ImU32 Text          = rgb(250, 251, 249);           // warm off-white glass text (softer than pure white on cool glass)
constexpr ImU32 Accent        = rgb(120, 205, 255);           // luminous liquid cyan/azure
constexpr ImU32 AccentLight   = rgb(220, 246, 255);           // bright specular glint
constexpr ImU32 AccentSoft    = rgb(100, 195, 255, 36);       // azure glow 14%
constexpr ImU32 AccentDeep    = rgb(65, 145, 235);            // deep sapphire
constexpr ImU32 OnAccent      = rgb(8, 14, 24);               // contrast on bright azure
constexpr ImU32 Muted         = rgb(226, 232, 242, 168);      // glass dimmed, warmer + more readable
constexpr ImU32 Eyebrow       = rgb(226, 237, 250, 195);      // glass label, warmer/softer
constexpr ImU32 FooterBg      = rgb(16, 22, 36, 130);         // = Bg: footer band blends into the menu glass
constexpr ImU32 ModalBg       = rgb(14, 18, 28, 255);         // opaque solid dark modal
constexpr ImU32 ModalBorder   = rgb(180, 220, 255, 90);       // luminous glass rim
constexpr ImU32 ToastBg       = rgb(15, 24, 40, 175);
constexpr ImU32 ToastBorder   = rgb(180, 220, 255, 85);
constexpr ImU32 Secondary     = rgb(180, 215, 255, 24);       // liquid glass container
constexpr ImU32 SecondaryBorder = rgb(190, 225, 255, 55);     // refractive inner border
constexpr ImU32 SecondaryHover = rgb(190, 230, 255, 42);      // liquid sheen on hover
constexpr ImU32 Outline       = rgb(180, 220, 255, 42);
constexpr ImU32 OutlineText   = rgb(248, 250, 249);           // warm glass white
constexpr ImU32 Record        = rgb(255, 140, 120);           // warm coral liquid
constexpr ImU32 RecordSoft    = rgb(255, 140, 120, 36);
constexpr ImU32 RecordText    = rgb(45, 15, 10);
constexpr ImU32 Red           = rgb(255, 105, 130);           // errors
constexpr ImU32 RedSoft       = rgb(255, 105, 130, 40);
constexpr ImU32 Mint          = rgb(90, 240, 190);            // liquid mint/emerald
constexpr ImU32 Amber         = rgb(255, 195, 95);            // liquid amber
constexpr ImU32 FieldDark     = rgb(34, 46, 68, 150);         // translucent lighter dark (glass-blending)
constexpr ImU32 FieldBorder   = rgb(180, 220, 255, 90);       // unified glass rim (= ModalBorder)

// The menu-opacity multiplier (0..1), applied to the glass backgrounds by
// The menu-opacity multiplier (0..1). Kept here (not just in State) so
// drawShell can scale the BLUR region alpha with it too: the blur region
// paints with its own alpha=1 by default, so without this the blur stays a
// fully opaque slab while the glass on top goes clear. With the CUSTOMIZATION
// section removed (26.09) this is always 1.0 — the shipped Liquid Glass
// translucency — but drawShell still multiplies by it so the wiring stays.
// ---------------------------------------------------------------------------
inline float menuOpacity = 1.0f;

// COSMETICS: when true, the menu drops the transparent Liquid Glass look —
// the hardware blur region is skipped and the glass base becomes fully opaque
// (a flat solid panel). Default true (solid background); toggled from
// the COSMETICS section in Settings. Persisted as "gui_solid_background".
inline bool solidBackground = true;

// COSMETICS: user font scale multiplier across the entire menu (persisted as "gui_font_scale").
// Default 1.0f (range 0.60f .. 1.50f).
inline float fontScale = 1.0f;

} // namespace theme

inline ImVec4 vec4(ImU32 color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

inline ImU32 withAlpha(ImU32 color, float alpha) {
    ImVec4 value = vec4(color);
    value.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(value);
}

inline ImVec2 add(ImVec2 a, ImVec2 b) {
    return ImVec2(a.x + b.x, a.y + b.y);
}

// ---------------------------------------------------------------------------
// Frame tweens. The redesign is immediate-mode ImGui: every widget redraws from
// scratch each frame and its state flips instantly. Each keyed value eases
// toward `target` every frame (exponential smoothing), so hovers, toggles and
// accordions glide instead of snapping. Keys are ImGuiIDs — stable, scoped
// (PushID-aware) — so stacked or looped widgets can't collide.
// ---------------------------------------------------------------------------
inline std::unordered_map<ImGuiID, float>& tweenPool() {
    static std::unordered_map<ImGuiID, float> pool;
    return pool;
}

// Normalized [0,1] eased toward `target` (typically 0 or 1). Higher `speed`
// = snappier approach.
//
// Two guards keep the tail from reading as a freeze:
//  - A hitched frame (dt > 50ms) previously fell back to dt = 1e-3, which
//    advanced the tween by almost nothing and visibly STALLED it for a frame.
//    Clamping keeps the motion advancing instead.
//  - Pure exponential easing decelerates forever: the last few percent of the
//    travel is under a pixel per frame, so an accordion looked like it hung
//    right before closing. `minStep` floors the per-frame advance (still far
//    below one pixel for row-sized content) so the tail resolves promptly.
inline float anim01(ImGuiID key, float target, float speed = 12.0f) {
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f) dt = 1e-3f;
    if (dt > 0.05f) dt = 0.05f;
    float& v = tweenPool()[key];
    const float remain = target - v;
    float step = remain * (1.0f - std::exp(-speed * dt));
    constexpr float minStep = 0.006f;
    if (std::fabs(remain) > minStep && std::fabs(step) < minStep) {
        step = remain > 0.0f ? minStep : -minStep;
    }
    v += step;
    if (std::fabs(v - target) < 0.0015f) v = target;
    return v;
}

// Eased float anywhere (not clamped to 0..1), for sliding geometry.
inline float animF(ImGuiID key, float target, float speed = 12.0f) {
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.1f) dt = 1e-3f;
    float& v = tweenPool()[key];
    v += (target - v) * (1.0f - std::exp(-speed * dt));
    if (std::fabs(v - target) < 0.015f) v = target;
    return v;
}

// Linear blend of two scalars.
inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Blend two colors by t (0 -> a, 1 -> b).
inline ImU32 mixColor(ImU32 a, ImU32 b, float t) {
    const ImVec4 ca = vec4(a);
    const ImVec4 cb = vec4(b);
    const float w = std::clamp(t, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(ca.x + (cb.x - ca.x) * w, ca.y + (cb.y - ca.y) * w,
               ca.z + (cb.z - ca.z) * w, ca.w + (cb.w - ca.w) * w));
}

// Draw a horizontal line with a multi-step gradient between col0 and col1.
inline void drawHGradientLine(ImDrawList* draw, ImVec2 p0, ImVec2 p1, ImU32 col0, ImU32 col1,
                             float thickness = 1.0f, int segments = 12) {
    const float dx = p1.x - p0.x;
    if (std::abs(dx) < 0.5f) return;
    const float y = p0.y;
    const float step = dx / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(segments);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(segments);
        const float x0 = p0.x + step * i;
        const float x1 = (i == segments - 1) ? p1.x : (p0.x + step * (i + 1));
        const ImU32 c = mixColor(col0, col1, (t0 + t1) * 0.5f);
        draw->AddLine(ImVec2(x0, y), ImVec2(x1, y), c, thickness);
    }
}

inline float uiScale() {
    // Compact LAYOUT scale. The mock is authored at ~1000x680 (k==1), but the
    // menu must not dominate the screen, so the layout sits well below 1.0 on
    // every >=720p display. UI feedback moved it up from 0.72 to 0.80 ("a bit
    // bigger"), staying well clear of the old 1000x680 bulk. Fonts keep their
    // own larger scale via uiTextScale().
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (disp.y <= 0.0f) return 0.80f;
    const float k = disp.y / 1080.0f;
    return std::clamp(k, 0.76f, 0.80f);
}

// Text scale: deliberately LARGER than uiScale(). The mock's mono labels are
// 8-12px — too small in-game — so fonts get their own stronger boost. Using a
// separate factor keeps the compact shell from blowing up with the text.
inline float uiTextScale() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (disp.y <= 0.0f) return 1.0f * theme::fontScale;
    const float k = disp.y / 820.0f;
    return std::clamp(k, 1.0f, 1.5f) * theme::fontScale;
}

// Snap a requested font size to a whole pixel count. The GD ImGui fork bakes
// a glyph set per size, rounding the request to the nearest integer
// (GetRoundedFontSize/IM_ROUND), then draws each glyph scaled by
// size/baked->Size. Passing a fractional size (e.g. `11.0f * uiTextScale()` on
// 1080p = 14.49) makes that scale != 1.0, so glyph quads land on sub-pixel
// extents and text renders blurry. A whole-pixel size rounds to itself, so
// baked->Size == size and scale == 1.0 — the baked glyphs map 1:1 to pixels.
inline float pxSize(float v) {
    return std::round(v);
}

inline bool hitRect(ImVec2 min, ImVec2 max) {
    return ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max);
}

// Disabled widgets in the GD fork still claim ImGui's HoveredId first (hit-testing
// inside a window is first-submitted-wins), which would block modal overlays
// submitted afterwards — e.g. the macro dropdown that covers the RECORD page
// while it is open. ImGuiButtonFlags_AllowOverlap makes an item publish
// g.HoveredIdAllowOverlap so items submitted later can still be hovered/clicked.
inline ImGuiButtonFlags blockedButtonFlags(bool enabled) {
    return enabled ? ImGuiButtonFlags_None : ImGuiButtonFlags_AllowOverlap;
}

inline void applyStyle() {
    const float k = uiScale();
    const float r = 28.0f * k;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = r;
    style.ChildRounding = r;
    style.FrameRounding = r;
    style.PopupRounding = r;
    style.GrabRounding = r;
    style.WindowPadding = ImVec2(0, 0);
    style.FramePadding = ImVec2(10.0f * k, 6.0f * k);
    style.ItemSpacing = ImVec2(10.0f * k, 9.0f * k);
    style.ItemInnerSpacing = ImVec2(8.0f * k, 6.0f * k);
    style.ScrollbarSize = 6.0f * k;
    style.ScrollbarRounding = r;
    style.SelectableRounding = r;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.SeparatorTextBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = vec4(theme::Text);
    colors[ImGuiCol_TextDisabled] = vec4(theme::Muted);
    colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0); // Window background drawn entirely by drawShell with true rounded corners
    colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg] = vec4(theme::ModalBg);
    colors[ImGuiCol_Border] = vec4(theme::Line);
    colors[ImGuiCol_FrameBg] = vec4(theme::FieldDark);
    colors[ImGuiCol_FrameBgHovered] = vec4(theme::AccentSoft);
    colors[ImGuiCol_FrameBgActive] = vec4(theme::AccentSoft);
    colors[ImGuiCol_Button] = vec4(theme::Secondary);
    colors[ImGuiCol_ButtonHovered] = vec4(theme::AccentSoft);
    colors[ImGuiCol_ButtonActive] = vec4(theme::AccentSoft);
    colors[ImGuiCol_CheckMark] = vec4(theme::Accent);
    colors[ImGuiCol_SliderGrab] = vec4(theme::AccentDeep);
    colors[ImGuiCol_SliderGrabActive] = vec4(theme::AccentLight);
    colors[ImGuiCol_Separator] = vec4(theme::Line);
    colors[ImGuiCol_SeparatorHovered] = vec4(theme::Accent);
    colors[ImGuiCol_SeparatorActive] = vec4(theme::Accent);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab] = vec4(theme::Outline);
    colors[ImGuiCol_ScrollbarGrabHovered] = vec4(theme::SecondaryBorder);
    colors[ImGuiCol_ScrollbarGrabActive] = vec4(theme::Accent);
    colors[ImGuiCol_TextSelectedBg] = vec4(theme::AccentSoft);
    // Selectable rows (dropdown popups): a rich sapphire glass pill highlight,
    // with clear distinct states matching the 28px rounding.
    colors[ImGuiCol_Header] = vec4(theme::rgb(55, 95, 175, 160));
    colors[ImGuiCol_HeaderHovered] = vec4(theme::rgb(70, 120, 215, 200));
    colors[ImGuiCol_HeaderActive] = vec4(theme::rgb(85, 145, 240, 230));
}

// HDR Bloom & Soft Halo around rounded shape: clean multi-stage feathered aura
// simulating high dynamic range light emission and crystal glass diffusion.
inline void drawSoftHalo(ImDrawList* draw, ImVec2 min, ImVec2 max, float radius,
                         ImU32 col, float amount, float k) {
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    const float effRadius = std::min(radius, std::min(w, h) * 0.5f);
    // Proportional glow layers that look gorgeous both on large windows and compact buttons
    struct Layer {
        float off, th, al;
    };
    const Layer layers[] = {
        {1.2f * k, 1.8f * k, 0.40f},
        {3.0f * k, 2.2f * k, 0.24f},
        {6.0f * k, 3.0f * k, 0.12f},
        {10.0f * k, 4.0f * k, 0.05f},
    };
    for (const Layer& l : layers) {
        const float a = amount * l.al;
        if (a <= 0.002f) continue;
        draw->AddRect(add(min, ImVec2(-l.off, -l.off)),
                      add(max, ImVec2(l.off, l.off)),
                      withAlpha(col, a), effRadius + l.off, 0, l.th);
    }
}

// Since all text now uses the ONE default font (Roboto / font_main), the old
// per-glyph letter-spacing is gone: `spacing` is accepted for signature
// compatibility but ignored, and these helpers measure/draw plain text.
// `font` may be null -> fall back to the default font.
inline float spacedWidth(ImFont* font, float size, const char* text, float /*spacing*/) {
    size = pxSize(size);  // integer size -> scale == 1.0 in the fork -> crisp glyphs
    if (!font) return ImGui::CalcTextSize(text).x;
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

inline void spacedText(ImDrawList* draw, ImFont* font, float size, ImVec2 pos,
                       const char* text, ImU32 color, float /*spacing*/) {
    size = pxSize(size);  // integer size -> scale == 1.0 -> crisp glyphs
    if (font) {
        draw->AddText(font, size, ImVec2(std::round(pos.x), pos.y), color, text);
    } else {
        draw->AddText(ImVec2(std::round(pos.x), pos.y), color, text);
    }
}

// Draw text with a subtle horizontal gradient between col0 and col1 (character by character)
inline void drawGradientText(ImDrawList* draw, ImFont* font, float size, ImVec2 pos,
                            const char* text, ImU32 col0, ImU32 col1) {
    if (!text || !*text) return;
    size = pxSize(size);
    ImFont* useFont = font ? font : ImGui::GetFont();
    const size_t len = std::strlen(text);
    if (len <= 1) {
        spacedText(draw, useFont, size, pos, text, col0, 0.0f);
        return;
    }
    // Glyph x positions come from PREFIX measurements of the whole string, not
    // from measuring each character on its own. An isolated measurement loses
    // the font's own advance handling, so the running cursor drifted and
    // neighbouring glyphs drifted into each other — most visible on the longer
    // captions ("Frame Stepper"), which read as "letters stuck together".
    // Slicing the string at text + i reproduces exactly where a plain
    // AddText(text) would put glyph i, so the gradient stays pixel-identical in
    // layout to ordinary text.
    const float x0 = std::round(pos.x);
    for (size_t i = 0; i < len; ++i) {
        char ch[2] = { text[i], '\0' };
        const float t = static_cast<float>(i) / static_cast<float>(len - 1);
        const ImU32 c = mixColor(col0, col1, t);
        const float gx = useFont
            ? x0 + useFont->CalcTextSizeA(size, FLT_MAX, 0.0f, text, text + i).x
            : x0 + ImGui::CalcTextSize(text, text + i).x;
        if (useFont) {
            draw->AddText(useFont, size, ImVec2(gx, pos.y), c, ch);
        } else {
            draw->AddText(ImVec2(gx, pos.y), c, ch);
        }
    }
}

inline bool quietButton(const char* id, ImVec2 min, ImVec2 size,
                        const char* label, ImU32 color, bool enabled = true) {
    ImGui::SetCursorScreenPos(min);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
    ImGui::EndDisabled();

    ImVec2 max = add(min, size);
    const bool hovered = enabled && ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max,
                        !enabled ? withAlpha(theme::Secondary, 0.45f)
                                 : hovered ? withAlpha(color, 0.22f) : theme::Secondary,
                        8.0f * uiScale());
    draw->AddRect(min, max, withAlpha(color, enabled ? 0.60f : 0.18f),
                  8.0f * uiScale(), 0, 1.0f);
    ImVec2 text = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(min.x + (size.x - text.x) * 0.5f,
                         min.y + (size.y - text.y) * 0.5f),
                  enabled ? theme::Text : theme::Muted, label);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}

// Redesign .button-primary: filled lime, dark text, kbd chip on the right.
inline bool buttonPrimary(const char* id, ImVec2 pos, ImVec2 size,
                          const char* label, const char* kbd = nullptr,
                          bool enabled = true, ImU32 fill = theme::Accent,
                          ImU32 onFill = theme::AccentLight,
                          bool* heldOut = nullptr) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
    ImGui::EndDisabled();
    // Sample "is held" HERE, right after the button item: callers that poll
    // IsItemActive() after the widget has run its own draw code can read a
    // different last-item and silently miss the hold, which is what made the
    // STEP button step only on tap while the keybind held fine.
    if (heldOut) *heldOut = enabled && ImGui::IsItemActive();

    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool fakeFill = hovered && enabled;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Disabled contrast: the label used to sit at 45% alpha over a 45%-alpha
    // Secondary fill, and the border at 18% — on the dark panel the whole pill
    // vanished, so "START RENDER" (disabled whenever a render cannot start) was
    // effectively unreadable and looked like a rendering glitch rather than an
    // inactive control. Raised all three so the shape and its caption stay
    // legible while still clearly reading as not-pressable.
    ImU32 bg = !enabled ? withAlpha(theme::Secondary, 0.62f)
                        : (fakeFill ? onFill : fill);
    draw->AddRectFilled(pos, add(pos, size), bg, 28.0f * uiScale());
    draw->AddRect(pos, add(pos, size),
                  !enabled ? withAlpha(fill, 0.45f) : bg, 28.0f * uiScale(), 0, 1.0f);
    ImU32 labelCol = !enabled ? theme::rgb(255, 255, 255) : theme::OnAccent;

    ImFont* mono = nullptr; // use bold/default label below
    ImVec2 tl = ImGui::CalcTextSize(label);
    float y = pos.y + (size.y - tl.y) * 0.5f;
    float x = pos.x + (size.x - tl.x) * 0.5f;
    if (kbd && enabled) {
        float kbdW = ImGui::CalcTextSize(kbd).x + 10.0f * uiScale();
        x = pos.x + 18.0f * uiScale();
        // shift label left by sharing the row
        float labelX = pos.x + 18.0f * uiScale();
        x = labelX;
        draw->AddText(ImVec2(x, y), labelCol, label);
        ImVec2 kpos(x + tl.x + 14.0f * uiScale(), pos.y + (size.y - 21.0f) * 0.5f);
        ImVec2 ksize(kbdW, 21.0f);
        draw->AddRect(kpos, add(kpos, ksize), theme::Outline, 28.0f * uiScale(), 0, 1.0f);
        ImVec2 kw = ImGui::CalcTextSize(kbd);
        draw->AddText(ImVec2(kpos.x + (ksize.x - kw.x) * 0.5f,
                             kpos.y + (ksize.y - kw.y) * 0.5f),
                      theme::OnAccent, kbd);
    } else {
        draw->AddText(ImVec2(x, y), labelCol, label);
    }
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}

// Redesign .button-outline: transparent, muted text, subtle border.
inline bool buttonOutline(const char* id, ImVec2 pos, ImVec2 size,
                          const char* label, bool enabled = true) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
    ImGui::EndDisabled();

    const bool hovered = enabled && ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect(pos, add(pos, size),
                  !enabled ? withAlpha(theme::Outline, 0.6f)
                           : hovered ? theme::OutlineText : theme::Outline,
                  28.0f * uiScale(), 0, 1.0f);
    if (hovered && enabled) {
        draw->AddRectFilled(pos, add(pos, size), theme::AccentSoft, 28.0f * uiScale());
    }
    ImVec2 t = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(pos.x + (size.x - t.x) * 0.5f, pos.y + (size.y - t.y) * 0.5f),
                  !enabled ? theme::Muted
                           : hovered ? theme::AccentLight : theme::OutlineText,
                  label);
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}

// Redesign .button-secondary / .button-play: dark fill, lime-ish border.
inline bool buttonSecondary(const char* id, ImVec2 pos, ImVec2 size,
                            const char* label, const char* kbd = nullptr,
                            bool enabled = true, bool active = false,
                            ImU32 accent = theme::SecondaryBorder) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
    ImGui::EndDisabled();

    const bool hovered = enabled && ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImU32 bg = !enabled ? withAlpha(theme::Secondary, 0.5f)
                        : hovered ? theme::SecondaryHover : theme::Secondary;
    ImU32 border = !enabled ? withAlpha(theme::SecondaryBorder, 0.5f)
                            : active ? accent : hovered ? theme::AccentDeep : theme::SecondaryBorder;
    if (active) bg = withAlpha(theme::Accent, 0.20f);
    draw->AddRectFilled(pos, add(pos, size), bg, 28.0f * uiScale());
    draw->AddRect(pos, add(pos, size), border, 28.0f * uiScale(), 0, 1.0f);

    ImU32 labelCol = !enabled ? theme::Muted
                              : active ? theme::Accent : theme::OutlineText;
    ImVec2 tl = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(pos.x + (size.x - tl.x) * 0.5f, pos.y + (size.y - tl.y) * 0.5f),
                  labelCol, label);
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}

// Redesign .icon-button: 30x30 ghost square used in window-actions / search.
inline bool iconButton(const char* id, ImVec2 pos, float size,
                       ImU32 hoverColor, bool enabled = true) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size), blockedButtonFlags(enabled));
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    const float h = anim01(ImGui::GetID(id) ^ 0x4943, hovered ? 1.0f : 0.0f, 14.0f);
    if (h > 0.004f) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pos, add(pos, ImVec2(size, size)),
                            withAlpha(hoverColor, h), 28.0f * uiScale());
        draw->AddRect(pos, add(pos, ImVec2(size, size)),
                      withAlpha(theme::Accent, 0.85f * h), 28.0f * uiScale(), 0, 1.0f);
    }
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}

inline bool toggleAt(const char* id, ImVec2 min, bool value, bool enabled = true,
                     bool interactive = true) {
    const float k = uiScale();
    const ImVec2 size(44.0f * k, 24.0f * k);
    const ImGuiID key = ImGui::GetID(id);
    bool clicked = false, hovered = false;
    if (interactive) {
        ImGui::SetCursorScreenPos(min);
        ImGui::BeginDisabled(!enabled);
        clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
        ImGui::EndDisabled();
        hovered = enabled && ImGui::IsItemHovered();
    }

    ImVec2 max = add(min, size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImU32 track = !enabled ? withAlpha(theme::Secondary, 0.5f)
                           : value ? theme::Accent : theme::Secondary;
    ImU32 border = !enabled ? theme::Outline
                            : value ? theme::Accent
                                    : hovered ? theme::AccentDeep : theme::Line;
    draw->AddRectFilled(min, max, track, size.y * 0.5f);
    draw->AddRect(min, max, border, size.y * 0.5f, 0, 1.0f);
    // Hover glow: a SOFT halved halo instead of one hard ring — three
    // concentric layers feather outward so the read stays crisp yet gentle.
    const float hg = anim01(key ^ 0x474C, hovered ? 1.0f : 0.0f, 13.0f);
    if (hg > 0.004f) drawSoftHalo(draw, min, max, size.y * 0.5f, theme::AccentLight, hg, k);
    const float pad = 4.0f * k;
    const float thumb = 15.0f * k;
    // The knob slides between its two stops instead of teleporting.
    const float slide = anim01(key ^ 0x4B4E, value ? 1.0f : 0.0f, 16.0f);
    const float x = min.x + pad + slide * (size.x - thumb - pad * 2.0f);
    const float cy = min.y + size.y * 0.5f;
    draw->AddCircleFilled(ImVec2(x + thumb * 0.5f, cy), thumb * 0.5f,
                          !enabled ? theme::Muted
                                   : value ? theme::OnAccent : theme::OutlineText,
                          20);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}

// Bounds of the rasterized INK of a string, relative to the pen start (X) and
// the line top (Y), in screen pixels. Advance boxes include the empty digit
// bearings and side space, so centering on the ink is what makes a rendered
// value look optically centered on both axes inside its field.
struct InkMeasure {
    float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
};

inline bool measureGlyphInk(ImFont* font, float size, const char* text,
                            InkMeasure& out) {
    if (!font || !text) return false;
    ImFontBaked* baked = font->GetFontBaked(size);
    if (!baked || baked->Size <= 0.0f) return false;
    const float scale = size / baked->Size;
    float pen = 0.0f;
    bool ok = false;
    for (const char* c = text; *c; ++c) {
        const ImFontGlyph* g = baked->FindGlyph((ImWchar)*c);
        if (!g || !g->Visible) continue;
        const float gx0 = pen + g->X0 * scale;
        const float gx1 = pen + g->X1 * scale;
        const float gy0 = g->Y0 * scale;
        const float gy1 = g->Y1 * scale;
        if (!ok) {
            out.x0 = gx0; out.x1 = gx1; out.y0 = gy0; out.y1 = gy1;
            ok = true;
        } else {
            out.x0 = std::min(out.x0, gx0);
            out.x1 = std::max(out.x1, gx1);
            out.y0 = std::min(out.y0, gy0);
            out.y1 = std::max(out.y1, gy1);
        }
        pen += g->AdvanceX * scale;
    }
    return ok;
}

// Redesign .selection-button: a check-style picker used for the no-clip target
// players (P1 / P2). Selected rows get an accent outline + filled check box; the
// whole control dims when disabled.
inline bool selectionToggle(const char* id, ImVec2 pos, ImVec2 size, ImFont* mf,
                            float labelSize, float labelSpacing,
                            const char* label, bool selected, bool enabled = true,
                            bool interactive = true) {
    const float k = uiScale();
    const ImGuiID key = ImGui::GetID(id);
    bool clicked = false, hovered = false;
    if (interactive) {
        ImGui::SetCursorScreenPos(pos);
        ImGui::BeginDisabled(!enabled);
        clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
        ImGui::EndDisabled();
        hovered = enabled && ImGui::IsItemHovered();
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Hover ramps the neutral fill/border up; picking a pill eases both into the
    // lime "selected" pair. With no checkbox this tint IS the whole selection
    // read, so the target fill/border are a bit punchier than the neutral ones.
    const float hoverT = anim01(key ^ 0x4849, hovered ? 1.0f : 0.0f, 13.0f);
    const float selT = anim01(key ^ 0x4348, selected ? 1.0f : 0.0f, 16.0f);
    ImU32 bg = !enabled ? theme::rgb(36, 42, 56, 150)
              : mixColor(mixColor(theme::rgb(34, 46, 68, 150), theme::rgb(48, 60, 78, 170), hoverT),
                         theme::rgb(62, 78, 102, 190), selT);
    ImU32 border = !enabled ? withAlpha(theme::Outline, 0.55f)
                   : mixColor(mixColor(theme::rgb(58, 74, 100), theme::rgb(108, 138, 197), hoverT),
                              theme::rgb(128, 160, 222), selT);
    const float effR = std::min(28.0f * k, std::min(size.x, size.y) * 0.5f);
    if (hoverT > 0.004f) {
        drawSoftHalo(draw, pos, add(pos, size), effR, theme::AccentLight, hoverT, k);
    }
    draw->AddRectFilled(pos, add(pos, size), bg, effR);
    draw->AddRect(pos, add(pos, size), border, effR, 0, 1.0f);
    if (hoverT > 0.004f) {
        draw->AddRectFilled(add(pos, ImVec2(2.0f * k, 2.0f * k)),
                            add(add(pos, size), ImVec2(-2.0f * k, -2.0f * k)),
                            withAlpha(theme::Accent, 0.06f * hoverT), std::max(0.0f, effR - 2.0f * k));
    }

    // The caption is centered in the pill; selection is the fill alone.
    // Center on the glyph INK (not the advance box): mono bearings make the
    // rendered paint narrower/shorter than CalcTextSize, so advance-centering
    // drifts the label off-center on BOTH axes. spacedText draws the whole
    // label in one AddText call (per-glyph spacing is gone), so the ink box
    // alone is the true extent — no trailing (n-1)*spacing term, that phantom
    // width used to shove long labels like "VANILLA" off-center to the left.
    const int n = label ? static_cast<int>(std::strlen(label)) : 0;
    const float cx = pos.x + size.x * 0.5f;
    const float cy = pos.y + size.y * 0.5f;
    float sx, ty;
    if (mf && n > 0) {
        InkMeasure ink;
        if (measureGlyphInk(mf, labelSize, label, ink)) {
            sx = cx - (ink.x0 + ink.x1) * 0.5f;
            ty = cy - (ink.y0 + ink.y1) * 0.5f;
        } else {
            const float lw = spacedWidth(mf, labelSize, label, labelSpacing);
            const float textH = mf->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, label).y;
            sx = pos.x + (size.x - lw) * 0.5f;
            ty = cy - textH * 0.5f;
        }
    } else {
        const float lw = spacedWidth(mf, labelSize, label, labelSpacing);
        const float textH = mf ? mf->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, label).y
                               : ImGui::GetFontSize();
        sx = pos.x + (size.x - lw) * 0.5f;
        ty = cy - textH * 0.5f;
    }
    ImU32 base = !enabled ? theme::Muted
                 : mixColor(theme::rgb(157, 172, 198), theme::rgb(173, 188, 212), hoverT);
    ImU32 tcol = selected ? mixColor(base, theme::rgb(213, 228, 244), selT) : base;
    spacedText(draw, mf, labelSize, ImVec2(sx, ty), label, tcol, labelSpacing);
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}

// Redesign .number-field: compact box with a centered mono value + suffix.
// Click to type the value with the keyboard (digits, '.' separator, backspace;
// Enter or clicking away commits, Esc reverts); the mouse wheel still steps the
// value within [min, max] while hovering. Dragging to scrub is gone — while
// editing the pointer is busy selecting text. Returns true when the value
// changed.
// `wheelStep` is the fixed per-notch wheel delta (e.g. TPS +-1); when <= 0 it
// falls back to the old proportional (max-min)*0.02 step.

// Editing state (the typed string + edit-mode flag) lives across frames, keyed
// by the field's unique ImGui ID. The buffer is re-seeded from the current
// value on every idle frame, so entering edit mode always starts from the live
// value and Esc simply reverts to it.
struct NumberEditState {
    char buf[32] = {};
    bool editing = false;
    bool pendingFocus = false;  // give the InputText keyboard focus on its 1st frame
    float lastVal = -1e30f;     // last applied value — detects the "change pop"
    float flashStart = -1e30f;  // wall-clock time of the last value change
};

inline NumberEditState& numberEditState(ImGuiID key) {
    static std::unordered_map<ImGuiID, NumberEditState> s;
    return s[key];
}

// Line-top to baseline distance in pixels for `font` rendered at `size`.
inline float fontAscentPx(ImFont* font, float size) {
    if (font) {
        if (ImFontBaked* baked = font->GetFontBaked(size))
            if (baked->Size > 0.0f) return baked->Ascent * (size / baked->Size);
    }
    return size * 0.8f;
}

inline bool numberField(ImFont* font, const char* id, ImVec2 pos, float width,
                        float& value, float min, float max, const char* suffix,
                        const char* fmt, bool enabled,
                        float wheelStep = 0.0f) {
    const float k = uiScale();
    const ImVec2 size(width, 34.0f * k);
    const ImGuiID key = ImGui::GetID(id);
    NumberEditState& edit = numberEditState(key);
    if (!enabled) edit.editing = false;
    // Keep the edit buffer synced to the live value while idle, so entering
    // edit mode always starts from the current formatted value.
    if (!edit.editing) std::snprintf(edit.buf, sizeof(edit.buf), fmt, value);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Box + border (amber while hovered or typing): drawn in BOTH modes, the
    // transparent InputText just paints its glyphs on top of the idle look.
    const bool hovered = enabled && ImGui::IsMouseHoveringRect(pos, add(pos, size));
    const float bT = anim01(key ^ 0x4E46, (hovered || edit.editing) ? 1.0f : 0.0f, 14.0f);
    const ImU32 boxBg = !enabled ? theme::rgb(36, 42, 56, 150) : theme::rgb(34, 46, 68, 150);
    const ImU32 boxBd = !enabled ? theme::rgb(54, 62, 82)
                                 : mixColor(theme::rgb(80, 100, 140), theme::rgb(126, 156, 212), bT);
    draw->AddRectFilled(pos, add(pos, size), boxBg, 28.0f * k);
    draw->AddRect(pos, add(pos, size), boxBd, 28.0f * k);

    bool changed = false;

    if (edit.editing) {
        // Keyboard editing: a transparent InputText over the pixel box. The
        // fork's InputText keeps the typed string in its own per-item state, so
        // the temporary buffer only has to persist across the frames we render
        // the widget (it does — it lives in NumberEditState).
        constexpr ImGuiInputTextFlags flags = ImGuiInputTextFlags_CharsDecimal |
                                              ImGuiInputTextFlags_AutoSelectAll;
        if (edit.pendingFocus) {
            ImGui::SetKeyboardFocusHere();
            edit.pendingFocus = false;
        }
        if (font) ImGui::PushFont(font);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(theme::rgb(220, 227, 243)));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, vec4(withAlpha(theme::Accent, 0.35f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 28.0f * k);
        // Vertical centering in the 34k box: pad by half the leftover height.
        const float padY = std::max(0.0f, (size.y - ImGui::GetFontSize()) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * k, padY));
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 2.0f * k, pos.y));
        ImGui::SetNextItemWidth(size.x - 4.0f * k);
        char editId[40];
        std::snprintf(editId, sizeof(editId), "%s_it", id);
        ImGui::InputText(editId, edit.buf, sizeof(edit.buf), flags);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(6);
        if (font) ImGui::PopFont();

        // Enter, clicking elsewhere, or Esc all deactivate the item; on the
        // frame after that we fold the typed (or reverted) string back into
        // `value`. Esc made InputText restore the pre-edit buffer, so an
        // invalid/escaped edit simply parses back to the old value.
        if (ImGui::IsItemDeactivated()) {
            edit.editing = false;
            const float old = value;
            char* end = nullptr;
            const float parsed = std::strtof(edit.buf, &end);
            if (end != edit.buf && end && *end == '\0') {
                value = std::clamp(parsed, min, max);
                changed = std::abs(value - old) > 1e-6f;
            }
        }
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    } else {
        // Idle: wheel steps the value, a click starts typing, and the live
        // value is drawn ink-centered with the pixel look.
        // The wheel steps while the cursor is over the field BOX (a small,
        // deliberate target) and consumes the event so the body scroll does
        // not also move under it. Scrolling the page works anywhere outside
        // the box (or via the scrollbar), so a page scroll can never silently
        // scrub a value — but hovering the value itself still adjusts it.
        if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
            const float w = ImGui::GetIO().MouseWheel;
            const float step = wheelStep > 0.0f ? wheelStep : (max - min) * 0.02f;
            value = std::clamp(value + (w > 0.0f ? step : -step), min, max);
            ImGui::GetIO().MouseWheel = 0.0f;
            changed = true;
        }

        ImGui::SetCursorScreenPos(pos);
        ImGui::BeginDisabled(!enabled);
        const bool clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
        ImGui::EndDisabled();
        if (clicked && enabled) {
            edit.editing = true;
            edit.pendingFocus = true;  // seed buf already holds fmt(value)
        }
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

        char buf[32];
        std::snprintf(buf, sizeof(buf), fmt, value);
        const float kt = uiTextScale();
        const float vSize = pxSize(13.0f * kt);
        const float sSize = pxSize(10.5f * kt);
        ImVec2 vw = font ? font->CalcTextSizeA(vSize, FLT_MAX, 0.0f, buf) : ImVec2(0, 0);
        const float gap = suffix ? 4.0f * k : 0.0f;
        if (font) {
            // Center the VALUE's rasterized ink in the field; the small suffix
            // sits right after it on the shared baseline.
            InkMeasure vm;
            const bool haveV = measureGlyphInk(font, vSize, buf, vm);
            const float aV = fontAscentPx(font, vSize);
            const float inkW = haveV ? (vm.x1 - vm.x0) : vw.x;
            const float inkH = haveV ? (vm.y1 - vm.y0) : vw.y;
            const float inkL = haveV ? vm.x0 : 0.0f;
            const float inkT = haveV ? vm.y0 : 0.0f;
            const float tx = pos.x + (size.x - inkW) * 0.5f - inkL;
            const float ty = pos.y + (size.y - inkH) * 0.5f - inkT;
            draw->AddText(font, vSize, ImVec2(tx, ty),
                          !enabled ? theme::rgb(158, 167, 187) : theme::rgb(220, 227, 243),
                          buf);
            if (suffix) {
                const float aS = fontAscentPx(font, sSize);
                draw->AddText(font, sSize,
                              ImVec2(tx + vw.x + gap, ty + (aV - aS)),
                              !enabled ? theme::rgb(122, 131, 150) : theme::rgb(143, 153, 174),
                              suffix);
            }
        }
    }
    // Every value change (wheel step or committed edit) fires a short accent
    // pop around the field: a concentric ring that flashes in and fades out.
    // Renewed on every change, so scrubbing with the wheel pulses in rhythm.
    if (std::abs(value - edit.lastVal) > 1e-6f) {
        edit.lastVal = value;
        edit.flashStart = static_cast<float>(ImGui::GetTime());
    }
    const float age = static_cast<float>(ImGui::GetTime()) - edit.flashStart;
    const float ft = std::clamp(1.0f - age / 0.40f, 0.0f, 1.0f);
    if (enabled && ft > 0.004f) {
        const float fk = ft * ft * (3.0f - 2.0f * ft);  // smoothstep fade-out
        drawSoftHalo(draw, pos, add(pos, size), 28.0f * k, theme::Accent, 1.1f * fk, k);
    }
    return changed;
}

// Dropdown field: a compact value box that opens a themed popup list below it.
// Same box/border/hover look as numberField so the two read as one control
// family. `preview` is the text shown while closed (the current value);
// `curIndex` is the currently selected item (-1 = none / custom value, only
// shown through `preview`). Returns the index the user picked this frame, or
// -1 if the popup was not used.
// ---- Dropdown list state ----------------------------------------------------
// A single open slot is enough: opening one list closes the other, which is what
// a dropdown is supposed to do anyway. Keyed by ImGuiID, so the two lists on
// the same page never fight over it.
inline ImGuiID& dropdownOpenId() {
    static ImGuiID id = 0;
    return id;
}

inline float& dropdownScroll() {
    static float s = 0.0f;
    return s;
}

inline int dropdownField(ImFont* font, const char* id, ImVec2 pos, float width,
                         const char* preview, const std::vector<std::string>& items,
                         int curIndex, bool enabled) {
    const float k = uiScale();
    const ImVec2 size(width, 34.0f * k);
    const ImGuiID key = ImGui::GetID(id);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const bool hovered = enabled && ImGui::IsMouseHoveringRect(pos, add(pos, size));
    const float bT = anim01(key ^ 0x4446, hovered ? 1.0f : 0.0f, 14.0f);
    const ImU32 boxBg = !enabled ? theme::rgb(36, 42, 56, 150)
                                 : theme::rgb(34, 46, 68, 150);
    const ImU32 boxBd = !enabled ? theme::rgb(54, 62, 82)
                                 : mixColor(theme::rgb(80, 100, 140),
                                            theme::rgb(126, 156, 212), bT);
    draw->AddRectFilled(pos, add(pos, size), boxBg, 28.0f * k);
    draw->AddRect(pos, add(pos, size), boxBd, 28.0f * k);

    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
    ImGui::EndDisabled();
    // No OpenPopup() here: the list is drawn by hand further down. Opening an
    // ImGui popup from here is what latched its position to the mouse.
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Preview value: ink-centered horizontally AND vertically, matching
    // numberField. If preview text exceeds available space (width minus chevron and padding),
    // truncate with "..." so it never overflows or collides with the chevron.
    const float kt = uiTextScale();
    const float vSize = pxSize(13.0f * kt);
    ImFont* useFont = font ? font : ImGui::GetFont();
    std::string previewStr = (preview && *preview) ? preview : "-";

    const float maxPreviewW = std::max(10.0f * k, size.x - 26.0f * k);
    float textW = useFont ? useFont->CalcTextSizeA(vSize, FLT_MAX, 0.0f, previewStr.c_str()).x
                          : ImGui::CalcTextSize(previewStr.c_str()).x;
    if (textW > maxPreviewW) {
        // Truncate characters until it fits with ellipsis
        while (!previewStr.empty() && textW > maxPreviewW) {
            previewStr.pop_back();
            std::string candidate = previewStr + "...";
            textW = useFont ? useFont->CalcTextSizeA(vSize, FLT_MAX, 0.0f, candidate.c_str()).x
                            : ImGui::CalcTextSize(candidate.c_str()).x;
        }
        previewStr += "...";
    }

    const char* text = previewStr.c_str();
    InkMeasure vm;
    const bool haveV = measureGlyphInk(font, vSize, text, vm);
    const float inkW = haveV ? (vm.x1 - vm.x0)
                             : (useFont ? useFont->CalcTextSizeA(vSize, FLT_MAX, 0.0f, text).x : textW);
    const float inkH = haveV ? (vm.y1 - vm.y0) : vSize * 0.8f;
    const float inkL = haveV ? vm.x0 : 0.0f;
    const float inkT = haveV ? vm.y0 : 0.0f;
    const float availW = size.x - 20.0f * k;
    const float tx = pos.x + (availW - inkW) * 0.5f - inkL;
    const float ty = pos.y + (size.y - inkH) * 0.5f - inkT;

    draw->PushClipRect(ImVec2(pos.x + 3.0f * k, pos.y),
                       ImVec2(pos.x + size.x - 18.0f * k, pos.y + size.y), true);
    draw->AddText(font, vSize, ImVec2(tx, ty),
                  !enabled ? theme::rgb(158, 167, 187)
                           : theme::rgb(220, 227, 243),
                  text);
    draw->PopClipRect();

    // Chevron: a small solid triangle on the right edge (drawn as geometry so
    // it never depends on a glyph being baked).
    const float cx = pos.x + size.x - 12.0f * k;
    const float cy = pos.y + size.y * 0.5f;
    const float cs = 3.6f * k;
    draw->AddTriangleFilled(ImVec2(cx - cs, cy - cs * 0.55f),
                            ImVec2(cx + cs, cy - cs * 0.55f),
                            ImVec2(cx, cy + cs * 0.75f),
                            !enabled ? theme::rgb(122, 131, 150)
                                     : theme::rgb(143, 153, 174));

    // ---- The list ---------------------------------------------------------------
// The list is NOT an ImGui popup any more, and that is the whole point. Every
// symptom that came out of the popup traces back to the popup being a real
// ImGui window:
//   * its position is latched from the MOUSE at OpenPopup() time
//     (imgui.cpp:12506 -> NavCalcPreferredRefPos) and consumed verbatim
//     (imgui.cpp:7884), so the list opened wherever the cursor was, not under
//     the field - clicking the right half of the 160px field dropped it up to
//     160px to the right;
//   * popups take style.PopupBorderSize, not style.WindowBorderSize
//     (imgui.cpp:7777), so the list kept a 1px border nothing else in the shell
//     has, standing alone in the gap - the "strange stripe";
//   * its size came from auto-fit plus size constraints, and Selectable() with
//     size.x == 0 reported its width from window->WorkRect, which describes the
//     size carried over from the previous frame - the window was measuring the
//     content and the content was measuring the window;
//   * and it was free to grow a scrollbar of its own on the right.
// Drawn by hand here, none of that is reachable. It goes on the FOREGROUND draw
// list so the shell's column clip rect cannot cut it off: escaping that clip is
// the only reason it ever needed to be a popup at all.
//
// Left and right insets are the same `pad` constant used in both directions, so
// the gaps are equal by construction rather than by agreement between a size
// constraint and a remembered frame size.
//
// One behaviour difference from the popup it replaces: a click outside used to
// be swallowed by the modal popup, and now it also reaches whatever is under
// the cursor (the list closes AND that control fires).
ImGuiID& openId = dropdownOpenId();
float& scroll = dropdownScroll();
if (clicked && enabled) {
    if (openId == key) {
        openId = 0;
    } else {
        openId = key;
        scroll = 0.0f;
    }
}
if (!enabled && openId == key) openId = 0;
if (ImGui::IsKeyPressed(ImGuiKey_Escape) && openId == key) openId = 0;

int picked = -1;
if (openId != key) return picked;

const float itemH = 26.0f * k;
const float r = 28.0f * k;
const float pad = 8.0f * k;
const float gapY = 2.0f * k;
const float rowGap = 3.0f * k;

const float rows = static_cast<float>(items.size());
const float contentH = rows * itemH + std::max(0.0f, rows - 1.0f) * rowGap;
// Same width as the field, always - that is what makes the box and the list
// share both vertical edges.
const float panelW = size.x;
// Cap the panel so a long list (every font on disk) cannot run off the bottom
// of the screen; the overflow scrolls on the wheel.
const float dispH = ImGui::GetIO().DisplaySize.y;
const float capH = dispH > 0.0f ? dispH * 0.45f : 240.0f * k;
const float panelH = 2.0f * pad + std::min(contentH, std::max(3.0f * itemH, capH));
const float viewH = panelH - 2.0f * pad;

const float belowY = pos.y + size.y + gapY;
const float aboveY = pos.y - gapY - panelH;
const float panelY = (dispH > 0.0f && belowY + panelH > dispH && aboveY > 0.0f) ? aboveY : belowY;

const ImVec2 panelMin(pos.x, panelY);
const ImVec2 panelMax(pos.x + panelW, panelY + panelH);
const float maxScroll = std::max(0.0f, contentH - viewH);
scroll = std::clamp(scroll, 0.0f, maxScroll);

// The open list must OWN the mouse over its whole panel. The rows below are
// hand-drawn on the foreground list, not ImGui items, so they never claim the
// click: without a real item here the press that picks "3840x2160" also lands
// on whatever sits underneath, and on the RENDER tab that is the Codec row —
// choosing a resolution closed the list and instantly opened the Codec dropdown
// (the same double-hit applied to every list: fonts, audio codec). An invisible
// button submitted BEFORE the widgets drawn later in the frame claims
// HoveredId/ActiveId, so those later controls no longer see the press.
ImGui::SetCursorScreenPos(panelMin);
ImGui::InvisibleButton("##dd_panel_hit", ImVec2(panelW, panelH),
                       ImGuiButtonFlags_MouseButtonLeft);

const bool clickNow = ImGui::IsMouseClicked(0);
const bool overPanel = ImGui::IsMouseHoveringRect(panelMin, panelMax);
if (overPanel && ImGui::GetIO().MouseWheel != 0.0f) {
    scroll = std::clamp(scroll - ImGui::GetIO().MouseWheel * itemH, 0.0f, maxScroll);
}

ImDrawList* fg = ImGui::GetForegroundDrawList();
fg->PushClipRect(panelMin, panelMax, true);
fg->AddRectFilled(panelMin, panelMax, theme::ModalBg, r);

const float listTop = panelMin.y + pad;
const ImU32 textCol = enabled ? theme::rgb(220, 227, 243) : theme::rgb(158, 167, 187);
const float maxLabelW = panelW - 2.0f * pad;
for (size_t i = 0; i < items.size(); ++i) {
    const float rowY = listTop + static_cast<float>(i) * (itemH + rowGap) - scroll;
    if (rowY + itemH < listTop || rowY > listTop + viewH) continue;

    const ImVec2 rowMin(panelMin.x + pad, rowY);
    const ImVec2 rowMax(panelMax.x - pad, rowY + itemH);
    const bool rowHover = enabled && ImGui::IsMouseHoveringRect(rowMin, rowMax);
    if (rowHover || static_cast<int>(i) == curIndex) {
        fg->AddRectFilled(rowMin, rowMax,
                          rowHover ? theme::rgb(70, 120, 215, 200)
                                   : theme::rgb(55, 95, 175, 160),
                          r);
    }

    // A label wider than the row would be hard-clipped mid-glyph, so shorten it
    // with an ellipsis before drawing.
    std::string label = items[i];
    if (useFont->CalcTextSizeA(vSize, FLT_MAX, 0.0f, label.c_str()).x > maxLabelW) {
        while (!label.empty() &&
               useFont->CalcTextSizeA(vSize, FLT_MAX, 0.0f, (label + "...").c_str()).x > maxLabelW) {
            label.pop_back();
        }
        label += "...";
    }

    // Ink-centred, same as the closed state: optical middle, not advance box.
    InkMeasure vm;
    const bool haveInk = measureGlyphInk(useFont, vSize, label.c_str(), vm);
    const float inkW = haveInk ? (vm.x1 - vm.x0)
                               : useFont->CalcTextSizeA(vSize, FLT_MAX, 0.0f, label.c_str()).x;
    const float inkH = haveInk ? (vm.y1 - vm.y0) : vSize * 0.8f;
    fg->AddText(useFont, vSize,
                ImVec2(rowMin.x + ((rowMax.x - rowMin.x) - inkW) * 0.5f - (haveInk ? vm.x0 : 0.0f),
                       rowMin.y + ((rowMax.y - rowMin.y) - inkH) * 0.5f - (haveInk ? vm.y0 : 0.0f)),
                textCol, label.c_str());

    if (rowHover && clickNow) {
        picked = static_cast<int>(i);
        openId = 0;
    }
}
fg->PopClipRect();
fg->AddRect(panelMin, panelMax, theme::ModalBorder, r, 0, 1.0f);

// A click on neither the field nor the list closes it. The field itself is
// excluded so the very click that opened the list does not shut it again.
if (clickNow && !hovered && !overPanel) openId = 0;
return picked;
}

}  // namespace skkbot2::gui::beta_widgets