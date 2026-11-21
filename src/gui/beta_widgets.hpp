#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

inline constexpr ImGuiSelectableFlags kNoPadWithHalfSpacing = 1 << 26;
static_assert((kNoPadWithHalfSpacing &
               (ImGuiSelectableFlags_None | ImGuiSelectableFlags_NoAutoClosePopups |
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                ImGuiSelectableFlags_Disabled | ImGuiSelectableFlags_AllowOverlap |
                ImGuiSelectableFlags_Highlight | ImGuiSelectableFlags_SelectOnNav)) == 0,
              "kNoPadWithHalfSpacing collides with a public ImGuiSelectableFlags bit");

#include <algorithm>
#include <cmath>

namespace skkbot2::gui::beta_widgets {

namespace theme {
inline float tintHue = 0.0f;
inline float tintSat = 1.0f;

constexpr ImU32 rgbRaw(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

inline ImU32 tintApply(int r, int g, int b, int a) {
    const float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    const float mx = std::max({rf, gf, bf});
    const float mn = std::min({rf, gf, bf});
    const float d = mx - mn;
    if (tintHue == 0.0f && tintSat == 1.0f) return IM_COL32(r, g, b, a);
    float h = 0.0f;
    if (d > 1e-6f) {
        if (mx == rf) h = std::fmod(60.0f * (gf - bf) / d + 360.0f, 360.0f);
        else if (mx == gf) h = 60.0f * (bf - rf) / d + 120.0f;
        else h = 60.0f * (rf - gf) / d + 240.0f;
    }
    float s = mx > 1e-6f ? d / mx : 0.0f;
    const float v = mx;
    h = std::fmod(h + tintHue, 360.0f);
    s = std::clamp(s * tintSat, 0.0f, 1.0f);
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float ro, go, bo;
    if (h < 60.0f)       { ro = c; go = x; bo = 0.0f; }
    else if (h < 120.0f) { ro = x; go = c; bo = 0.0f; }
    else if (h < 180.0f) { ro = 0.0f; go = c; bo = x; }
    else if (h < 240.0f) { ro = 0.0f; go = x; bo = c; }
    else if (h < 300.0f) { ro = x; go = 0.0f; bo = c; }
    else                 { ro = c; go = 0.0f; bo = x; }
    return IM_COL32(static_cast<int>(std::lround((ro + m) * 255.0f)),
                    static_cast<int>(std::lround((go + m) * 255.0f)),
                    static_cast<int>(std::lround((bo + m) * 255.0f)), a);
}

inline ImU32 rgb(int r, int g, int b, int a = 255) {
    return tintApply(r, g, b, a);
}

inline float textHue = -1.0f;
inline float textSatFloor = 0.20f;
inline std::string textHex;
inline ImU32 textTint(int r, int g, int b, int a) {
    if (textHue < 0.0f) return IM_COL32(r, g, b, a);
    const float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    const float mx = std::max({rf, gf, bf});
    const float mn = std::min({rf, gf, bf});
    const float d = mx - mn;
    const float v = mx;
    const float s0 = mx > 1e-6f ? d / mx : 0.0f;
    const float h = textHue;
    const float s = std::clamp(std::max(s0, textSatFloor), 0.0f, 1.0f);
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float ro, go, bo;
    if (h < 60.0f)       { ro = c; go = x; bo = 0.0f; }
    else if (h < 120.0f) { ro = x; go = c; bo = 0.0f; }
    else if (h < 180.0f) { ro = 0.0f; go = c; bo = x; }
    else if (h < 240.0f) { ro = 0.0f; go = x; bo = c; }
    else if (h < 300.0f) { ro = x; go = 0.0f; bo = c; }
    else                 { ro = c; go = 0.0f; bo = x; }
    return IM_COL32(static_cast<int>(std::lround((ro + m) * 255.0f)),
                    static_cast<int>(std::lround((go + m) * 255.0f)),
                    static_cast<int>(std::lround((bo + m) * 255.0f)), a);
}

inline ImU32 rgbText(int r, int g, int b, int a = 255) {
    return textTint(r, g, b, a);
}

inline ImU32 Bg            = rgbRaw(16, 22, 36, 130);
inline ImU32 ShellTop      = rgbRaw(26, 40, 68, 145);
inline ImU32 ShellBot      = rgbRaw(17, 24, 40, 155);
inline ImU32 Frame         = rgbRaw(18, 26, 44, 120);
inline ImU32 Line          = rgbRaw(200, 225, 255, 38);
inline ImU32 LineFooter    = rgbRaw(200, 225, 255, 48);
inline ImU32 Text          = rgbRaw(250, 251, 249);
inline ImU32 Accent        = rgbRaw(120, 205, 255);
inline ImU32 AccentLight   = rgbRaw(220, 246, 255);
inline ImU32 AccentSoft    = rgbRaw(100, 195, 255, 36);
inline ImU32 AccentDeep    = rgbRaw(65, 145, 235);
inline ImU32 OnAccent      = rgbRaw(8, 14, 24);
inline ImU32 Muted         = rgbRaw(226, 232, 242, 168);
inline ImU32 Eyebrow       = rgbRaw(226, 237, 250, 195);
inline ImU32 FooterBg      = rgbRaw(16, 22, 36, 130);
inline ImU32 ModalBg       = rgbRaw(14, 18, 28, 255);
inline ImU32 ModalBorder   = rgbRaw(180, 220, 255, 90);
inline ImU32 ToastBg       = rgbRaw(15, 24, 40, 175);
inline ImU32 ToastBorder   = rgbRaw(180, 220, 255, 85);
inline ImU32 Secondary     = rgbRaw(180, 215, 255, 24);
inline ImU32 SecondaryBorder = rgbRaw(190, 225, 255, 55);
inline ImU32 SecondaryGrey     = rgbRaw(242, 246, 252, 96);
inline ImU32 SecondaryGreyBorder = rgbRaw(206, 209, 216, 55);
inline ImU32 SecondaryHover = rgbRaw(190, 230, 255, 42);
inline ImU32 Outline       = rgbRaw(180, 220, 255, 42);
inline ImU32 OutlineText   = rgbRaw(248, 250, 249);
inline ImU32 Record        = rgbRaw(255, 140, 120);
inline ImU32 RecordSoft    = rgbRaw(255, 140, 120, 36);
inline ImU32 RecordText    = rgbRaw(45, 15, 10);
inline ImU32 Red           = rgbRaw(255, 105, 130);
inline ImU32 RedSoft       = rgbRaw(255, 105, 130, 40);
inline ImU32 Mint          = rgbRaw(90, 240, 190);
inline ImU32 Amber         = rgbRaw(255, 195, 95);
inline ImU32 FieldDark     = rgbRaw(34, 46, 68, 150);
inline ImU32 FieldBorder   = rgbRaw(180, 220, 255, 90);

inline void refreshTint() {
    Bg               = tintApply(16, 22, 36, 130);
    ShellTop         = tintApply(26, 40, 68, 145);
    ShellBot         = tintApply(17, 24, 40, 155);
    Frame            = tintApply(18, 26, 44, 120);
    Line             = tintApply(200, 225, 255, 38);
    LineFooter       = tintApply(200, 225, 255, 48);
    Text             = textTint(250, 251, 249, 255);
    Accent           = tintApply(120, 205, 255, 255);
    AccentLight      = tintApply(220, 246, 255, 255);
    AccentSoft       = tintApply(100, 195, 255, 36);
    AccentDeep       = tintApply(65, 145, 235, 255);
    OnAccent         = tintApply(8, 14, 24, 255);
    Muted            = textTint(226, 232, 242, 168);
    Eyebrow          = textTint(226, 237, 250, 195);
    FooterBg         = tintApply(16, 22, 36, 130);
    ModalBg          = tintApply(14, 18, 28, 255);
    ModalBorder      = tintApply(180, 220, 255, 90);
    ToastBg          = tintApply(15, 24, 40, 175);
    ToastBorder      = tintApply(180, 220, 255, 85);
    Secondary        = tintApply(180, 215, 255, 24);
    SecondaryBorder  = tintApply(190, 225, 255, 55);
    SecondaryGrey    = tintApply(242, 246, 252, 96);
    SecondaryGreyBorder = tintApply(206, 209, 216, 55);
    SecondaryHover   = tintApply(190, 230, 255, 42);
    Outline          = tintApply(180, 220, 255, 42);
    OutlineText      = textTint(248, 250, 249, 255);
    Record           = tintApply(255, 140, 120, 255);
    RecordSoft       = tintApply(255, 140, 120, 36);
    RecordText       = tintApply(45, 15, 10, 255);
    Red              = tintApply(255, 105, 130, 255);
    RedSoft          = tintApply(255, 105, 130, 40);
    Mint             = tintApply(90, 240, 190, 255);
    Amber            = tintApply(255, 195, 95, 255);
    FieldDark        = tintApply(34, 46, 68, 150);
    FieldBorder      = tintApply(180, 220, 255, 90);
}

inline float menuOpacity = 1.0f;

inline bool solidBackground = true;

inline bool blurEnabled = true;

inline float fontScale = 1.0f;

}

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

inline std::unordered_map<ImGuiID, float>& tweenPool() {
    static std::unordered_map<ImGuiID, float> pool;
    return pool;
}

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

inline float animF(ImGuiID key, float target, float speed = 12.0f) {
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.1f) dt = 1e-3f;
    float& v = tweenPool()[key];
    v += (target - v) * (1.0f - std::exp(-speed * dt));
    if (std::fabs(v - target) < 0.015f) v = target;
    return v;
}

inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

inline ImU32 mixColor(ImU32 a, ImU32 b, float t) {
    const ImVec4 ca = vec4(a);
    const ImVec4 cb = vec4(b);
    const float w = std::clamp(t, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(ca.x + (cb.x - ca.x) * w, ca.y + (cb.y - ca.y) * w,
               ca.z + (cb.z - ca.z) * w, ca.w + (cb.w - ca.w) * w));
}

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
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (disp.y <= 0.0f) return 0.80f;
    const float k = disp.y / 1080.0f;
    return std::clamp(k, 0.76f, 0.80f);
}

inline float uiTextScale() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (disp.y <= 0.0f) return 1.0f * theme::fontScale;
    const float k = disp.y / 820.0f;
    return std::clamp(k, 1.0f, 1.5f) * theme::fontScale;
}

inline float pxSize(float v) {
    return std::round(v);
}

inline bool hitRect(ImVec2 min, ImVec2 max) {
    return ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max);
}

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
    colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
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
    colors[ImGuiCol_Header] = vec4(theme::rgb(55, 95, 175, 160));
    colors[ImGuiCol_HeaderHovered] = vec4(theme::rgb(70, 120, 215, 200));
    colors[ImGuiCol_HeaderActive] = vec4(theme::rgb(85, 145, 240, 230));
}

inline void drawSoftHalo(ImDrawList* draw, ImVec2 min, ImVec2 max, float radius,
                         ImU32 col, float amount, float k) {
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    const float effRadius = std::min(radius, std::min(w, h) * 0.5f);
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

inline void drawShellShadow(ImDrawList* draw, ImVec2 min, ImVec2 max,
                            float radius, ImU32 col, float amount, float k) {
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    const float effRadius = std::min(radius, std::min(w, h) * 0.5f);
    constexpr int kLayers = 9;
    for (int i = 0; i < kLayers; i++) {
        const float t = static_cast<float>(i) / static_cast<float>(kLayers - 1);
        const float off = (1.5f + 12.5f * t) * k;
        const float a = amount * 0.045f * (1.0f - t) * (1.0f - t);
        if (a <= 0.002f) continue;
        draw->AddRectFilled(add(min, ImVec2(-off, -off)),
                            add(max, ImVec2(off, off)),
                            withAlpha(col, a), effRadius + off);
    }
}

inline float spacedWidth(ImFont* font, float size, const char* text, float ) {
    size = pxSize(size);
    if (!font) return ImGui::CalcTextSize(text).x;
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

inline void spacedText(ImDrawList* draw, ImFont* font, float size, ImVec2 pos,
                       const char* text, ImU32 color, float ) {
    size = pxSize(size);
    if (font) {
        draw->AddText(font, size, ImVec2(std::round(pos.x), pos.y), color, text);
    } else {
        draw->AddText(ImVec2(std::round(pos.x), pos.y), color, text);
    }
}

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

inline bool buttonPrimary(const char* id, ImVec2 pos, ImVec2 size,
                          const char* label, const char* kbd = nullptr,
                          bool enabled = true, ImU32 fill = theme::Accent,
                          ImU32 onFill = theme::AccentLight,
                          bool* heldOut = nullptr) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, size, blockedButtonFlags(enabled));
    ImGui::EndDisabled();
    if (heldOut) *heldOut = enabled && ImGui::IsItemActive();

    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool fakeFill = hovered && enabled;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImU32 bg = !enabled ? withAlpha(theme::Secondary, 0.62f)
                        : (fakeFill ? onFill : fill);
    draw->AddRectFilled(pos, add(pos, size), bg, 28.0f * uiScale());
    draw->AddRect(pos, add(pos, size),
                  !enabled ? withAlpha(fill, 0.45f) : bg, 28.0f * uiScale(), 0, 1.0f);
    ImU32 labelCol = !enabled ? theme::rgb(255, 255, 255) : theme::OnAccent;

    ImFont* mono = nullptr;
    ImVec2 tl = ImGui::CalcTextSize(label);
    float y = pos.y + (size.y - tl.y) * 0.5f;
    float x = pos.x + (size.x - tl.x) * 0.5f;
    if (kbd && enabled) {
        float kbdW = ImGui::CalcTextSize(kbd).x + 10.0f * uiScale();
        x = pos.x + 18.0f * uiScale();
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
    const float hg = anim01(key ^ 0x474C, hovered ? 1.0f : 0.0f, 13.0f);
    if (hg > 0.004f) drawSoftHalo(draw, min, max, size.y * 0.5f, theme::AccentLight, hg, k);
    const float pad = 4.0f * k;
    const float thumb = 15.0f * k;
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
                 : mixColor(theme::rgb(157, 172, 198), theme::rgbText(173, 188, 212), hoverT);
    ImU32 tcol = selected ? mixColor(base, theme::rgbText(213, 228, 244), selT) : base;
    spacedText(draw, mf, labelSize, ImVec2(sx, ty), label, tcol, labelSpacing);
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && enabled;
}


struct NumberEditState {
    char buf[32] = {};
    bool editing = false;
    bool pendingFocus = false;
    double lastVal = -1e300;
    float flashStart = -1e30f;
};

inline NumberEditState& numberEditState(ImGuiID key) {
    static std::unordered_map<ImGuiID, NumberEditState> s;
    return s[key];
}

inline float fontAscentPx(ImFont* font, float size) {
    if (font) {
        if (ImFontBaked* baked = font->GetFontBaked(size))
            if (baked->Size > 0.0f) return baked->Ascent * (size / baked->Size);
    }
    return size * 0.8f;
}

inline void formatNumberPattern(char* out, size_t size, double value,
                                int maxDecimals = 6) {
    int decimals = 0;
    double scaled = value;
    while (decimals < maxDecimals &&
           std::abs(scaled - std::round(scaled)) > 1e-9) {
        scaled *= 10.0;
        ++decimals;
    }
    std::snprintf(out, size, "%%.%df", decimals);
}

template <typename Real>
inline bool numberField(ImFont* font, const char* id, ImVec2 pos, float width,
                        Real& value, Real min, Real max, const char* suffix,
                        const char* fmt, bool enabled,
                        Real wheelStep = Real(0),
                        Real fineWheelStep = Real(0)) {
    static_assert(std::is_floating_point_v<Real>,
                  "numberField only handles floating-point values");
    const float k = uiScale();
    const ImVec2 size(width, 34.0f * k);
    const ImGuiID key = ImGui::GetID(id);
    NumberEditState& edit = numberEditState(key);
    if (!enabled) edit.editing = false;
    if (!edit.editing) {
        std::snprintf(edit.buf, sizeof(edit.buf), fmt,
                      static_cast<double>(value));
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool hovered = enabled && ImGui::IsMouseHoveringRect(pos, add(pos, size));
    const float bT = anim01(key ^ 0x4E46, (hovered || edit.editing) ? 1.0f : 0.0f, 14.0f);
    const ImU32 boxBg = !enabled ? theme::rgb(36, 42, 56, 150) : theme::rgb(34, 46, 68, 150);
    const ImU32 boxBd = !enabled ? theme::rgb(54, 62, 82)
                                 : mixColor(theme::rgb(80, 100, 140), theme::rgb(126, 156, 212), bT);
    draw->AddRectFilled(pos, add(pos, size), boxBg, 28.0f * k);
    draw->AddRect(pos, add(pos, size), boxBd, 28.0f * k);

    bool changed = false;

    if (edit.editing) {
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
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(theme::rgbText(220, 227, 243)));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, vec4(withAlpha(theme::Accent, 0.35f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 28.0f * k);
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

        if (ImGui::IsItemDeactivated()) {
            edit.editing = false;
            const Real old = value;
            char* end = nullptr;
            const double parsed = std::strtod(edit.buf, &end);
            if (end != edit.buf && end && *end == '\0') {
                value = std::clamp(static_cast<Real>(parsed), min, max);
                changed = std::abs(static_cast<double>(value) -
                                   static_cast<double>(old)) > 1e-9;
            }
        }
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    } else {
        if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
            const float w = ImGui::GetIO().MouseWheel;
            Real step = (ImGui::GetIO().KeyShift && fineWheelStep > Real(0))
                ? fineWheelStep
                : (wheelStep > Real(0) ? wheelStep : (max - min) * Real(0.02));
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
            edit.pendingFocus = true;
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
                          !enabled ? theme::rgb(158, 167, 187) : theme::rgbText(220, 227, 243),
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
    if (std::abs(static_cast<double>(value) - edit.lastVal) > 1e-9) {
        edit.lastVal = static_cast<double>(value);
        edit.flashStart = static_cast<float>(ImGui::GetTime());
    }
    const float age = static_cast<float>(ImGui::GetTime()) - edit.flashStart;
    const float ft = std::clamp(1.0f - age / 0.40f, 0.0f, 1.0f);
    if (enabled && ft > 0.004f) {
        const float fk = ft * ft * (3.0f - 2.0f * ft);
        drawSoftHalo(draw, pos, add(pos, size), 28.0f * k, theme::Accent, 1.1f * fk, k);
    }
    return changed;
}

struct HexEditState {
    char buf[16];
    bool editing = false;
    bool pendingFocus = false;
};
inline std::unordered_map<ImGuiID, HexEditState> g_hexEdits;

inline bool hexField(ImFont* font, const char* id, ImVec2 pos, float width,
                     bool enabled) {
    const float k = uiScale();
    const ImVec2 size(width, 34.0f * k);
    const ImGuiID key = ImGui::GetID(id);
    auto it = g_hexEdits.find(key);
    if (it == g_hexEdits.end()) {
        it = g_hexEdits.emplace(key, HexEditState{}).first;
    }
    HexEditState& edit = it->second;
    if (!enabled) edit.editing = false;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool hovered = enabled && ImGui::IsMouseHoveringRect(pos, add(pos, size));
    const float bT = anim01(key ^ 0x4845, (hovered || edit.editing) ? 1.0f : 0.0f, 14.0f);
    const ImU32 boxBg = !enabled ? theme::rgb(36, 42, 56, 150) : theme::rgb(34, 46, 68, 150);
    const ImU32 boxBd = !enabled ? theme::rgb(54, 62, 82)
                                 : mixColor(theme::rgb(80, 100, 140), theme::rgb(126, 156, 212), bT);
    draw->AddRectFilled(pos, add(pos, size), boxBg, 28.0f * k);
    draw->AddRect(pos, add(pos, size), boxBd, 28.0f * k);

    bool changed = false;
    if (hovered && ImGui::IsMouseClicked(0)) {
        edit.editing = true;
        edit.pendingFocus = true;
    }

    if (edit.editing) {
        if (edit.pendingFocus) {
            ImGui::SetKeyboardFocusHere();
            edit.pendingFocus = false;
        }
        if (font) ImGui::PushFont(font);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(theme::rgbText(220, 227, 243)));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, vec4(withAlpha(theme::Accent, 0.35f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 28.0f * k);
        const float padY = std::max(0.0f, (size.y - ImGui::GetFontSize()) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * k, padY));
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 2.0f * k, pos.y));
        ImGui::SetNextItemWidth(size.x - 4.0f * k);
        char editId[40];
        std::snprintf(editId, sizeof(editId), "%s_it", id);
        ImGui::InputText(editId, edit.buf, sizeof(edit.buf));
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(6);
        if (font) ImGui::PopFont();

        if (ImGui::IsItemDeactivated()) {
            edit.editing = false;
            auto isHexDigit = [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
            };
            const char* p = edit.buf;
            if (*p == '#') ++p;
            bool ok = true;
            for (int i = 0; i < 6; ++i) ok = ok && p[i] && isHexDigit(p[i]);
            ok = ok && p[6] == '\0';
            if (!ok) {
                auto eqNoCase = [](const char* a, const char* b) {
                    while (*a && *b) {
                        if (std::tolower((unsigned char)*a) !=
                            std::tolower((unsigned char)*b)) return false;
                        ++a; ++b;
                    }
                    return *a == '\0' && *b == '\0';
                };
                ok = eqNoCase(edit.buf, "default");
            }
            changed = ok;
        }
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    } else {
        draw->AddText(font ? font : ImGui::GetFont(),
                      pxSize(13.0f * uiTextScale()),
                      ImVec2(pos.x + (size.x - ImGui::CalcTextSize(edit.buf).x) * 0.5f,
                             pos.y + (size.y - ImGui::CalcTextSize(edit.buf).y) * 0.5f),
                      theme::rgbText(220, 227, 243), edit.buf);
        if (enabled && hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    return changed;
}

inline void hexFieldSync(const char* id, const char* text) {
    const ImGuiID key = ImGui::GetID(id);
    auto it = g_hexEdits.find(key);
    if (it == g_hexEdits.end()) {
        it = g_hexEdits.emplace(key, HexEditState{}).first;
    }
    if (!it->second.editing) {
        std::snprintf(it->second.buf, sizeof(it->second.buf), "%s", text);
    }
}

inline std::string betaHexFieldCommitted(const char* id) {
    auto it = g_hexEdits.find(ImGui::GetID(id));
    return it != g_hexEdits.end() ? std::string(it->second.buf) : std::string();
}

inline ImGuiID& dropdownOpenId() {
    static ImGuiID id = 0;
    return id;
}

inline float& dropdownScroll() {
    static float s = 0.0f;
    return s;
}

inline ImVec2& dropdownOpenPos() {
    static ImVec2 p(0.0f, 0.0f);
    return p;
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
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float kt = uiTextScale();
    const float vSize = pxSize(13.0f * kt);
    ImFont* useFont = font ? font : ImGui::GetFont();
    std::string previewStr = (preview && *preview) ? preview : "-";

    const float maxPreviewW = std::max(10.0f * k, size.x - 26.0f * k);
    float textW = useFont ? useFont->CalcTextSizeA(vSize, FLT_MAX, 0.0f, previewStr.c_str()).x
                          : ImGui::CalcTextSize(previewStr.c_str()).x;
    if (textW > maxPreviewW) {
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
                           : theme::rgbText(220, 227, 243),
                  text);
    draw->PopClipRect();

    const float cx = pos.x + size.x - 12.0f * k;
    const float cy = pos.y + size.y * 0.5f;
    const float cs = 3.6f * k;
    draw->AddTriangleFilled(ImVec2(cx - cs, cy - cs * 0.55f),
                            ImVec2(cx + cs, cy - cs * 0.55f),
                            ImVec2(cx, cy + cs * 0.75f),
                            !enabled ? theme::rgb(122, 131, 150)
                                     : theme::rgb(143, 153, 174));

ImGuiID& openId = dropdownOpenId();
float& scroll = dropdownScroll();
ImVec2& openPos = dropdownOpenPos();
if (clicked && enabled) {
    if (openId == key) {
        openId = 0;
    } else {
        openId = key;
        scroll = 0.0f;
        openPos = pos;
    }
}
if (!enabled && openId == key) openId = 0;
if (ImGui::IsKeyPressed(ImGuiKey_Escape) && openId == key) openId = 0;

int picked = -1;
if (openId != key) return picked;

if (std::abs(pos.x - openPos.x) > 1.0f || std::abs(pos.y - openPos.y) > 1.0f) {
    openId = 0;
    return picked;
}

const ImVec2 clipMin = draw->GetClipRectMin();
const ImVec2 clipMax = draw->GetClipRectMax();
if (pos.y + size.y <= clipMin.y || pos.y >= clipMax.y ||
    pos.x + size.x <= clipMin.x || pos.x >= clipMax.x) {
    openId = 0;
    return picked;
}

const float itemH = 26.0f * k;
const float r = 28.0f * k;
const float pad = 8.0f * k;
const float gapY = 2.0f * k;
const float rowGap = 3.0f * k;

const float rows = static_cast<float>(items.size());
const float contentH = rows * itemH + std::max(0.0f, rows - 1.0f) * rowGap;
const float panelW = size.x;
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

const bool overPanel = ImGui::IsMouseHoveringRect(panelMin, panelMax);
const bool clickNow = ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2);

if (ImGui::GetIO().MouseWheel != 0.0f) {
    if (overPanel && maxScroll > 0.0f) {
        scroll = std::clamp(scroll - ImGui::GetIO().MouseWheel * itemH, 0.0f, maxScroll);
        ImGui::GetIO().MouseWheel = 0.0f;
    } else {
        openId = 0;
        return picked;
    }
}

if ((clickNow || ImGui::IsMouseDown(0)) && !hovered && !overPanel) {
    openId = 0;
    return picked;
}

const ImVec2 savedCursorPos = ImGui::GetCursorScreenPos();
ImGui::SetCursorScreenPos(panelMin);
ImGui::InvisibleButton("##dd_panel_hit", ImVec2(panelW, panelH),
                       ImGuiButtonFlags_MouseButtonLeft);
ImGui::SetCursorScreenPos(savedCursorPos);

ImDrawList* fg = ImGui::GetForegroundDrawList();
fg->PushClipRect(panelMin, panelMax, true);
fg->AddRectFilled(panelMin, panelMax, theme::ModalBg, r);

const float listTop = panelMin.y + pad;
const ImU32 textCol = enabled ? theme::rgbText(220, 227, 243) : theme::rgb(158, 167, 187);
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

    std::string label = items[i];
    if (useFont->CalcTextSizeA(vSize, FLT_MAX, 0.0f, label.c_str()).x > maxLabelW) {
        while (!label.empty() &&
               useFont->CalcTextSizeA(vSize, FLT_MAX, 0.0f, (label + "...").c_str()).x > maxLabelW) {
            label.pop_back();
        }
        label += "...";
    }

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

if (clickNow && !hovered && !overPanel) openId = 0;
return picked;
}

inline float segmentedNaturalWidth(const char* const* items, int count,
                                   float pad = 22.0f) {
    if (count <= 0) return 0.0f;
    ImFont* df = ImGui::GetFont();
    const float ts = pxSize(10.0f * uiTextScale());
    float w = 0.0f;
    for (int i = 0; i < count; ++i) {
        w += df->CalcTextSizeA(ts, FLT_MAX, 0.0f, items[i]).x;
    }
    return w + pad * (float)count;
}

inline int segmentedControl(const char* id, ImVec2 pos, float width, float height,
                            const char* const* items, int count, int current,
                            bool enabled = true) {
    if (count <= 0) return current;
    if (count > 8) count = 8;
    const float k = uiScale();
    const ImGuiID key = ImGui::GetID(id);
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height),
                                                blockedButtonFlags(enabled));
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const float radius = height * 0.5f;
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                        !enabled ? theme::rgb(30, 38, 54, 150)
                                 : theme::rgb(22, 30, 46, 190), radius);
    draw->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                  withAlpha(theme::Line, enabled ? 1.0f : 0.5f), radius, 0, 1.0f);

    ImFont* df = ImGui::GetFont();
    const float ts = pxSize(10.0f * uiTextScale());
    const float inset = 3.0f * k;
    const float gap = 2.0f * k;

    float lw[8];
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        lw[i] = df->CalcTextSizeA(ts, FLT_MAX, 0.0f, items[i]).x;
        total += lw[i];
    }
    const float avail = std::max(12.0f, width - inset * 2.0f - gap * (float)(count - 1));
    float segX[8], segW[8];
    float cursor = pos.x + inset;
    for (int i = 0; i < count; ++i) {
        segW[i] = total > 0.0f ? avail * (lw[i] / total) : avail / (float)count;
        segX[i] = cursor;
        cursor += segW[i] + gap;
    }

    const float ft = std::clamp(animF(key ^ 0x5347, (float)current, 14.0f),
                                0.0f, (float)(count - 1));
    const int i0 = (int)std::floor(ft);
    const int i1 = std::min(i0 + 1, count - 1);
    const float f = ft - (float)i0;
    const float thumbX = lerp(segX[i0], segX[i1], f);
    const float thumbW = lerp(segW[i0], segW[i1], f);
    draw->AddRectFilled(ImVec2(thumbX + inset * 0.5f, pos.y + inset),
                        ImVec2(thumbX + thumbW - inset * 0.5f, pos.y + height - inset),
                        enabled ? withAlpha(theme::Accent, 0.85f) : withAlpha(theme::Muted, 0.35f),
                        (height - inset * 2.0f) * 0.5f);

    for (int i = 0; i < count; ++i) {
        const float cx = segX[i] + segW[i] * 0.5f;
        const float cy = pos.y + height * 0.5f;
        const bool sel = (i == current);
        const ImU32 col = !enabled ? theme::Muted
                                   : sel ? theme::OnAccent
                                         : mixColor(theme::Muted, theme::Text, 0.35f);
        const float tw = lw[i];
        draw->AddText(df, ts, ImVec2(cx - tw * 0.5f, cy - ts * 0.5f), col, items[i]);
    }
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (clicked && enabled) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        int hit = 0;
        for (int i = 0; i < count; ++i) {
            if (m.x >= segX[i] && m.x < segX[i] + segW[i] + gap) { hit = i; break; }
        }
        if (hit != current) return hit;
    }
    return -1;
}

}