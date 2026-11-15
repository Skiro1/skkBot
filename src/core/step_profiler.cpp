#include "step_profiler.hpp"

#include <Geode/Geode.hpp>

#include <atomic>
#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "logger/logger.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <psapi.h>

using namespace geode::prelude;

namespace skkbot2::debug {

namespace {

std::atomic<unsigned long> g_mainTid{0};
std::atomic<bool> g_running{false};
std::atomic<bool> g_pending{false};
std::string g_label;
std::map<unsigned long long, unsigned long long> g_buckets;
std::thread g_thread;
std::string g_pendingLabel;
double g_startWall = 0.0;
unsigned long long g_samples = 0;
unsigned long long g_misses = 0;

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct ModuleInfo {
    unsigned long long base = 0;
    unsigned long long size = 0;
    std::wstring name;
};

std::vector<ModuleInfo> g_modules;
double g_modulesRefreshAt = 0.0;

void refreshModules() {
    g_modules.clear();
    HMODULE handles[512]{};
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    if (!EnumProcessModulesEx(proc, handles, sizeof(handles), &needed,
                              LIST_MODULES_ALL)) {
        return;
    }
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), 512);
    MODULEINFO mi{};
    for (size_t i = 0; i < count; ++i) {
        if (!GetModuleInformation(proc, handles[i], &mi, sizeof(mi))) continue;
        wchar_t name[MAX_PATH]{};
        GetModuleBaseNameW(proc, handles[i], name, MAX_PATH);
        ModuleInfo info;
        info.base = reinterpret_cast<unsigned long long>(mi.lpBaseOfDll);
        info.size = mi.SizeOfImage;
        info.name = name;
        g_modules.push_back(info);
    }
    std::sort(g_modules.begin(), g_modules.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) {
                  return a.base < b.base;
              });
}

int findModule(unsigned long long addr) {
    size_t lo = 0, hi = g_modules.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (addr < g_modules[mid].base) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (lo == 0) return -1;
    const ModuleInfo& m = g_modules[lo - 1];
    if (addr - m.base < m.size) return static_cast<int>(lo - 1);
    return -1;
}

void samplerLoop(unsigned long tid) {
    refreshModules();
    g_modulesRefreshAt = nowSec() + 1.0;

    HANDLE thread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE, tid);
    if (!thread) {
        ++g_misses;
        return;
    }

    timeBeginPeriod(1);
    CONTEXT ctx{};
    while (g_running.load(std::memory_order_relaxed)) {
        if (nowSec() >= g_modulesRefreshAt) {
            refreshModules();
            g_modulesRefreshAt = nowSec() + 1.0;
        }
        Sleep(1);
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            ++g_misses;
            continue;
        }
        ctx.ContextFlags = CONTEXT_CONTROL;
        const BOOL ok = GetThreadContext(thread, &ctx);
        ResumeThread(thread);
        if (!ok || ctx.Rip == 0) {
            ++g_misses;
            continue;
        }
        ++g_samples;
        const unsigned long long rip = ctx.Rip;
        const int modIdx = findModule(rip);
        const unsigned long long key =
            (static_cast<unsigned long long>(modIdx + 1) << 48) |
            (modIdx >= 0 ? rip - g_modules[modIdx].base : 0);
        ++g_buckets[key];
    }
    timeEndPeriod(1);
    CloseHandle(thread);
}

struct TopEntry {
    unsigned long long key = 0;
    unsigned long long count = 0;
};

}

void profilerStartNow(const char* label);

bool stepProfilerOptIn() {
    return Mod::get()->getSavedValue<bool>("gui_step_profiler", false);
}

void profilerNoteMainThread() {
    unsigned long expected = 0;
    const unsigned long tid = GetCurrentThreadId();
    const bool captured = g_mainTid.compare_exchange_strong(expected, tid);
    if ((captured || expected == tid) && g_pending.load(std::memory_order_relaxed)) {
        if (g_pending.exchange(false, std::memory_order_relaxed)) {
            profilerStart(g_pendingLabel.c_str());
        }
    }
}

void profilerStart(const char* label) {
    if (!stepProfilerOptIn()) return;
    const unsigned long tid = g_mainTid.load(std::memory_order_relaxed);
    if (tid == 0) {
        g_pendingLabel = label;
        g_pending.store(true, std::memory_order_relaxed);
        skkbot2::log::info(
            "[PROF] start deferred ({}): waiting for the scheduler hook to "
            "capture the main thread", label);
        return;
    }
    profilerStartNow(label);
}

void profilerStartNow(const char* label) {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;

    const unsigned long tid = g_mainTid.load(std::memory_order_relaxed);
    if (tid == 0) {
        g_running.store(false, std::memory_order_relaxed);
        skkbot2::log::info(
            "[PROF] not started ({}): main thread id not captured yet - "
            "the scheduler hook has not run", label);
        return;
    }

    g_buckets.clear();
    g_samples = 0;
    g_misses = 0;
    g_label = label;
    g_startWall = nowSec();
    g_thread = std::thread(samplerLoop, tid);
    skkbot2::log::info(
        "[PROF] sampling started: label={} mainTid={} (~1ms period)",
        label, tid);
}

void profilerStopAndReport() {
    bool expected = true;
    if (!g_running.compare_exchange_strong(expected, false)) {
        g_pending.store(false, std::memory_order_relaxed);
        return;
    }

    if (g_thread.joinable()) g_thread.join();

    const double dur = nowSec() - g_startWall;
    skkbot2::log::info(
        "[PROF] report: label={} window={:.1f}s samples={} misses={} buckets={}",
        g_label, dur, g_samples, g_misses, g_buckets.size());
    if (g_samples == 0) return;

    auto moduleName = [&](int idx) -> std::string {
        if (idx < 0 || idx >= static_cast<int>(g_modules.size()))
            return "OTHER";
        const std::wstring& w = g_modules[idx].name;
        std::string s;
        s.reserve(w.size());
        for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
        return s;
    };

    {
        std::map<int, unsigned long long> perMod;
        for (const auto& [key, count] : g_buckets) {
            perMod[static_cast<int>(key >> 48) - 1] += count;
        }
        std::vector<TopEntry> mods;
        for (const auto& [idx, count] : perMod) {
            mods.push_back(
                {static_cast<unsigned long long>(idx + 1) << 48, count});
        }
        std::sort(mods.begin(), mods.end(),
                  [](const TopEntry& a, const TopEntry& b) {
                      return a.count > b.count;
                  });
        skkbot2::log::info("[PROF] per module:");
        for (const auto& m : mods) {
            const int idx = static_cast<int>(m.key >> 48) - 1;
            skkbot2::log::info(
                "[PROF]   {:>18}  {:5.1f}%  ({} samples)",
                moduleName(idx),
                100.0 * static_cast<double>(m.count) /
                    static_cast<double>(g_samples),
                m.count);
        }
    }

    std::vector<TopEntry> top;
    top.reserve(g_buckets.size());
    for (const auto& [key, count] : g_buckets) {
        top.push_back({key, count});
    }
    std::sort(top.begin(), top.end(),
              [](const TopEntry& a, const TopEntry& b) {
                  return a.count > b.count;
              });

    const size_t kTop = std::min<size_t>(top.size(), 24);
    for (size_t i = 0; i < kTop; ++i) {
        const int idx = static_cast<int>(top[i].key >> 48) - 1;
        const unsigned long long rva = top[i].key & 0xFFFFFFFFFFFFull;
        skkbot2::log::info(
            "[PROF]   {:2}. {:>18} +0x{:08X}  {:5.1f}%  ({} samples)",
            i + 1, moduleName(idx), rva,
            100.0 * static_cast<double>(top[i].count) /
                static_cast<double>(g_samples),
            top[i].count);
    }
    skkbot2::log::info(
        "[PROF] GeometryDash.exe RVA map offline against "
        "bindings/2.2091/GeometryDash.bro; our module = skiro1.skk-bot.dll; "
        "OTHER = heap/syscall stubs (ntdll waits etc.)");
}

}
