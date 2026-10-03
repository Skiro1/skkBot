#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <imgui.h>

namespace skkbot2::gui::blur {

void newFrame();
void submit(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, float radius = 3.0f, float alpha = 1.0f);
bool available();

} // namespace skkbot2::gui::blur
