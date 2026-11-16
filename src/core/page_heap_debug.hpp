#pragma once

// BUG-109 (05.11): хирургический full page heap для поимки портчика кучи.
//
// Классический путь через gflags/IFEO (page heap с запуска процесса)
// невозможен в этой среде: в чистом geode 6.0.0-alpha.1 есть
// детерминированное UAF-чтение на старте (CustomMenuLayer::init, см.
// docs/Баги.md BUG-109 события 2-3) - page heap де коммитит освобождённые
// страницы и игра падает ещё до меню. Поэтому page heap включается В
// РАНТАЙМЕ, только на время бот-сессий (SKK/SKC запись и проигрывание):
// старт игры остаётся чистым, а любая выходящая за границы запись в кучу
// во время сессии падает мгновенно с точным стеком виновника (Geode
// crashlog -> модуль + RVA; для нашего DLL RVA резолвится через PDB).
//
// Включение затрагивает только НОВЫЕ аллокации (уже живые блоки остаются
// в старой раскладке) - для ловли портчика в активной сессии этого
// достаточно. Полный page heap жрёт память: держать включённым только на
// сессию, снимать в stop/haltExecution.

#ifdef _WIN32
#include <windows.h>
#endif

#include "logger/logger.hpp"

#include <Geode/loader/Mod.hpp>

#include <vector>

namespace skkbot2::debug {

// Opt-in gate (05.11.2026): page heap is ONE-WAY per process and itself
// slows every allocation, so it must never ride along silently with a
// recording session. It now runs only when the user turned it on in
// SETTINGS -> LOGGING ("Page Heap" toggle, saved key gui_page_heap).
inline bool pageHeapOptIn() {
    return geode::Mod::get()->getSavedValue<bool>("gui_page_heap", false);
}

// mode: 2 = full page heap, 0 = standard. HeapCompatibilityInformation = 0.
inline void setFullPageHeap(bool on) {
#ifdef _WIN32
    if (on && !pageHeapOptIn()) return;  // opt-in: тихо, без лога
    static int state = -1;  // -1 неизвестно, 0 выключен, 1 включён
    const int want = on ? 1 : 0;
    if (state == want) return;

    // ВАЖНО (сессия 20:43–21:01): HeapSetInformation(HeapCompatibilityInformation)
    // ОДНОСТОРОННЯЯ - однажды переведённая куча не возвращается в стандартный
    // режим, все попытки disable дают ok=0 (в логе было "disabled on 0/7").
    // Page heap остаётся на ВЕСЬ процесс до перезапуска игры - это осознанный
    // режим охоты на BUG-109, а не переключатель. Выключение = перезапуск GD.
    if (!on) {
        if (state == 1) {
            log::info("[PAGEHEAP] cannot disable (one-way per process) - "
                      "restart the game to return to the standard heap");
        }
        if (state < 0) state = 0;  // disable до первого enable - просто запоминаем
        return;
    }
    if (state == 1) return;  // уже включён - повторно не ставим

    const ULONG mode = 2u;
    void* modePtr = const_cast<void*>(static_cast<const void*>(&mode));
    unsigned ok = 0;
    unsigned total = 0;

    // ВАЖНО (краш 16:08): HeapSetInformation НЕ принимает NULL-хендл -
    // RtlSetHeapInformation разыменовывает heap+0x10 и падает. Дефолтная
    // куча процесса берётся только через GetProcessHeap().
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

}  // namespace skkbot2::debug
