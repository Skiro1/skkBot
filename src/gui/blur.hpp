#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <imgui.h>

namespace skkbot2::gui::blur {

void newFrame();
void submit(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, float radius = 3.0f, float alpha = 1.0f);
bool available();

/// BUG-132 diagnostics: when set, the final upsample pass ignores the glass
/// colour and outputs a flat tint instead — RED where the SDF mask is 0 (the
/// pasted/rounded-away zone), GREEN where the glass is fully opaque. Makes the
/// mask geometry directly visible on screen.
void setDebugTint(bool on);

/// BUG-132 decisive probe: adds a screen-px bias to the blur mask's corner
/// radius for one mode of the F10 cycle (+8 = DIAG 4, -8 = DIAG 5). If the
/// corner notch grows/shrinks with the bias, the mask/chrome arc mismatch is
/// confirmed and its sign measured.
void setDebugRoundBias(float biasPx);

} // namespace skkbot2::gui::blur
