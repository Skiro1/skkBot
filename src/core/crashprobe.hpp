// BUG-120 (B): shared probe window. CCCallFunc::execute is extremely hot,
// so it only logs inside an armed window (armed by the fadeStreak2 /
// dart-fade / stopStroke hooks and by the practice restore path). If a call
// kills a PointNode again, the windowed log names the exact CCCallFunc
// (target pointer + handler + callfn bytes) that executed the dtor.
#pragma once

namespace skkbot2::crashprobe {

// Arm/extend the logging window by `seconds` from now.
void armWindow(double seconds);

// True while the window is open.
bool inWindow();

} // namespace skkbot2::crashprobe
