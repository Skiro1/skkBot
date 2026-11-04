#pragma once

#if defined(_WIN32)
#include <Windows.h>
#include <dwmapi.h>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace skkbot2::timing {

// Ported directly from Echo bot (Logic/win_refresh_clock.cpp).
// The screen ticks at a steady rate (e.g. 240Hz/144Hz/60Hz) & shows 1 frame per tick,
// but GD's raw dt fluctuates and stutters.
// This queries DwmGetCompositionTimingInfo to count screen ticks since last frame
// and uses (ticks * tick length) as the dt instead, bounded by QPC to prevent drift.

inline bool g_windows_said_no = false;
inline unsigned long long g_ticks_last_frame = 0;
inline bool g_is_first_frame = true;
inline LARGE_INTEGER g_qpc_last{};
inline double g_real_total = 0.0, g_used_total = 0.0;
inline double g_gd_average = 0.0;

inline bool ask_windows_for_ticks(unsigned long long& total_ticks, double& seconds_per_tick) {
    if (g_windows_said_no) return false;

    DWM_TIMING_INFO info = {};
    info.cbSize = sizeof(info);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info))) {
        g_windows_said_no = true;
        return false;
    }

    if (info.rateRefresh.uiNumerator == 0 || info.rateRefresh.uiDenominator == 0) return false;
    seconds_per_tick = static_cast<double>(info.rateRefresh.uiDenominator) / static_cast<double>(info.rateRefresh.uiNumerator);
    total_ticks = info.cRefresh;
    if (total_ticks == 0) {
        g_windows_said_no = true;
        return false;
    }
    return true;
}

inline double refresh_frame_time(double gd_time) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    double real = g_qpc_last.QuadPart ? static_cast<double>(now.QuadPart - g_qpc_last.QuadPart) / static_cast<double>(frequency.QuadPart) : -1.0;
    g_qpc_last = now;
    if (!(real >= 0.0) || real > 0.25) g_is_first_frame = true;

    unsigned long long total_ticks = 0;
    double seconds_per_tick = 0.0;
    if (!ask_windows_for_ticks(total_ticks, seconds_per_tick)) return gd_time;

    unsigned long long ticks_this_frame = total_ticks - g_ticks_last_frame;
    g_ticks_last_frame = total_ticks;

    if (g_is_first_frame) {
        g_is_first_frame = false;
        g_real_total = g_used_total = 0.0;
        g_gd_average = gd_time;
        return gd_time;
    }
    g_gd_average += (gd_time - g_gd_average) * 0.1;
    if (g_gd_average < seconds_per_tick * 0.75) {
        g_real_total = g_used_total = 0.0;
        return gd_time;
    }
    g_real_total += real;

    double screen_time = static_cast<double>(ticks_this_frame) * seconds_per_tick;
    if (ticks_this_frame == 0) {
        // Rendering faster than the monitor: no whole tick landed this frame.
        // Returning 0 here would freeze level time in that frame and then
        // release it in a burst later (stutter). Use the QPC delta instead so
        // the average stays 1.0x.
        screen_time = real;
    }
    if (std::abs(screen_time - gd_time) > seconds_per_tick * 2.0) screen_time = gd_time;
    if (std::abs(g_used_total + screen_time - g_real_total) > seconds_per_tick)
        screen_time = std::max(g_real_total - g_used_total, 0.0);
    g_used_total += screen_time;
    double settled = std::min(g_used_total, g_real_total);
    g_used_total -= settled;
    g_real_total -= settled;
    return screen_time;
}

inline void reset_refresh_clock() {
    g_is_first_frame = true;
    g_qpc_last = {};
    g_real_total = g_used_total = 0.0;
    g_gd_average = 0.0;
    g_ticks_last_frame = 0;
}

} // namespace skkbot2::timing
#else
namespace skkbot2::timing {
inline double refresh_frame_time(double gd_time) { return gd_time; }
inline void reset_refresh_clock() {}
}
#endif
