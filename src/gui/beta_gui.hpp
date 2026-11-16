#pragma once

// Beta ImGui Quiet Canvas for skkBot2. It is a sparse, focus-first control
// sheet rather than a second copy of the classic window layout.
// Pure Dear ImGui (GD fork, 1.93.0): ImDrawList + standard widgets, no DOM,
// no cocos nodes, no third-party UI libs. Public entry point:
//   skkbot2::gui::beta_gui::render(bool open)
// called from Gui::draw() (see skkbot_gui.cpp) on every imgui frame.

namespace skkbot2::gui::beta_gui {

void render(bool open);

} // namespace skkbot2::gui::beta_gui
