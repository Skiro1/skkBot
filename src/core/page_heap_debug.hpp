#pragma once


#ifdef _WIN32
#include <windows.h>
#endif

#include "logger/logger.hpp"

#include <Geode/loader/Mod.hpp>

#include <vector>

namespace skkbot2::debug {

inline bool pageHeapOptIn() {
    return geode::Mod::get()->getSavedValue<bool>("gui_page_heap", false);
}

inline void setFullPageHeap(bool on) {
#ifdef _WIN32
    if (on && !pageHeapOptIn()) return;
    static int state = -1;
    const int want = on ? 1 : 0;
    if (state == want) return;

    if (!on) {
        if (state == 1) {
            log::info("[PAGEHEAP] cannot disable (one-way per process) - "
                      "restart the game to return to the standard heap");
        }
        if (state < 0) state = 0;
        return;
    }
    if (state == 1) return;

    const ULONG mode = 2u;
    void* modePtr = const_cast<void*>(static_cast<const void*>(&mode));
    unsigned ok = 0;
    unsigned total = 0;

    HANDLE defaultHeap = GetProcessHeap();
    if (defaultHeap) {
        ++total;
        if (HeapSetInformation(defaultHeap, HeapCompatibilityInformation, modePtr, sizeof(mode))) ++ok;
    }
    const ULONG count = GetProcessHeaps(0, nullptr);
    if (count) {
        std::vector<HANDLE> heaps(count);
        if (GetProcessHeaps(count, heaps.data()) == count) {
            for (HANDLE h : heaps) {
                if (!h) continue;
                ++total;
                if (HeapSetInformation(h, HeapCompatibilityInformation, modePtr, sizeof(mode))) ++ok;
            }
        }
    }
    state = 1;
    log::info("[PAGEHEAP] ENABLED on {}/{} heaps (mode={}) - ONE-WAY, active until game restart",
        ok, total, static_cast<unsigned>(mode));
#else
    (void)on;
#endif
}

}
