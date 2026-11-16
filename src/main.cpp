#include <imgui.h>
#include <dbghelp.h>
#include <chrono>

#include "skkBot.hpp"
#include "core/hotpath.hpp"
#include "core/memtrack.hpp"
#include "core/playback_fps.hpp"
#include "format/skk3_format.hpp"
#include "skc/skc_bot.hpp"
#include "skc/skc_format.hpp"
#include "gui/blur.hpp"
#include "core/paths.hpp"
#include "gui/skkbot_gui.hpp"
#include "hacks/practice_music_sync.hpp"
#include "logger/logger.hpp"
#include "render/renderer.hpp"
#ifdef GEODE_IS_WINDOWS
#include "physics/step_grid_hooks.hpp"
#include "physics/cocos_string_opt.hpp"
#include "core/rng_lock.hpp"
#include "core/ui_input_wall.hpp"
#include "version_offsets.hpp"
#endif

#include <Geode/Geode.hpp>
#include <Geode/utils/Keyboard.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/utils/string.hpp>
#include <imgui-cocos.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <filesystem>
#include <typeinfo>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp")

using namespace geode::prelude;
using namespace skkbot2::gui;

$on_mod(Loaded) {
    auto& logger = skkbot2::Logger::get();
    logger.setLevel(skkbot2::Logger::Info);
    logger.setFileLogging(true);
    skkbot2::memtrack::initModules();

    static uintptr_t s_geodeCallSite = 0;
    {
        auto patchLog = [](const std::string& msg) {
            skkbot2::log::error("[GEODE-PATCH] {}", msg);
            skkbot2::Logger::get().flush();
        };
        do {
            HMODULE geode = GetModuleHandleW(L"Geode.dll");
            if (!geode) { patchLog("failed: Geode.dll handle not found"); break; }
            const uintptr_t base = (uintptr_t)geode;
            const auto dos = (IMAGE_DOS_HEADER*)base;
            const auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);

            uintptr_t iatSlotRva = 0;
            {
                const IMAGE_DATA_DIRECTORY& impDir =
                    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
                for (auto desc = (IMAGE_IMPORT_DESCRIPTOR*)(base +
                         impDir.VirtualAddress);
                     !iatSlotRva && desc->Name; ++desc) {
                    const char* dllName = (const char*)(base + desc->Name);
                    if (_stricmp(dllName, "KERNEL32.dll") != 0) continue;
                    auto oft = (IMAGE_THUNK_DATA64*)(base +
                        desc->OriginalFirstThunk);
                    for (int i = 0; oft[i].u1.AddressOfData; ++i) {
                        if (oft[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
                        auto byName = (IMAGE_IMPORT_BY_NAME*)(base +
                            (uintptr_t)oft[i].u1.AddressOfData);
                        if (!strcmp((const char*)byName->Name,
                                "K32EnumProcessModules")) {
                            iatSlotRva = (uintptr_t)desc->FirstThunk +
                                (uintptr_t)i * sizeof(IMAGE_THUNK_DATA64);
                            break;
                        }
                    }
                }
            }
            if (!iatSlotRva) {
                patchLog("failed: K32EnumProcessModules import not found");
                break;
            }

            auto sec = (IMAGE_SECTION_HEADER*)((uintptr_t)&nt->OptionalHeader +
                nt->FileHeader.SizeOfOptionalHeader);
            const IMAGE_SECTION_HEADER* text = nullptr;
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                if (!memcmp(sec->Name, ".text\0\0\0", 8)) { text = sec; break; }
            }
            if (!text) { patchLog("failed: .text section not found"); break; }
            uint8_t* tstart = (uint8_t*)(base + text->VirtualAddress);
            const size_t tsize = text->Misc.VirtualSize;

            const uintptr_t iatSlotAddr = base + iatSlotRva;
            uintptr_t thunkAddr = 0;
            for (size_t i = 0; i + 6 <= tsize; ++i) {
                if (tstart[i] == 0xFF && tstart[i + 1] == 0x25) {
                    int32_t disp;
                    memcpy(&disp, &tstart[i + 2], 4);
                    const uintptr_t after = base + text->VirtualAddress + i + 6;
                    if ((uintptr_t)((int64_t)after + (int64_t)disp)
                        == iatSlotAddr) {
                        thunkAddr = base + text->VirtualAddress + i;
                        break;
                    }
                }
            }
            if (!thunkAddr) {
                patchLog("failed: jmp-thunk to K32EnumProcessModules not found");
                break;
            }

            uintptr_t site = 0;
            int matches = 0;
            for (size_t i = 0; i + 11 <= tsize; ++i) {
                if (tstart[i] != 0x41 || tstart[i + 1] != 0xB8 ||
                    tstart[i + 6] != 0xE8)
                    continue;
                uint32_t imm;
                memcpy(&imm, &tstart[i + 2], 4);
                if (imm != 0x400 && imm != 0x2000) continue;
                int32_t rel;
                memcpy(&rel, &tstart[i + 7], 4);
                const uintptr_t after = base + text->VirtualAddress + i + 11;
                if ((uintptr_t)((int64_t)after + (int64_t)rel) == thunkAddr) {
                    ++matches;
                    site = base + text->VirtualAddress + i;
                }
            }
            if (matches != 1) {
                patchLog(fmt::format(
                    "failed: expected exactly 1 call site, found {}", matches));
                break;
            }
            s_geodeCallSite = site;
            uint8_t* immPtr = (uint8_t*)(site + 2);
            uint32_t cur;
            memcpy(&cur, immPtr, 4);
            if (cur == 0x2000) {
                patchLog(fmt::format("already patched at +0x{:X}", site - base));
                break;
            }
            if (cur != 0x400) {
                patchLog(fmt::format(
                    "failed: unexpected imm 0x{:X} at +0x{:X}", cur, site - base));
                break;
            }
            DWORD protect = 0;
            if (!VirtualProtect((LPVOID)site, 8, PAGE_EXECUTE_READWRITE,
                    &protect)) {
                patchLog("failed: VirtualProtect");
                break;
            }
            const uint32_t newVal = 0x2000;
            memcpy(immPtr, &newVal, 4);
            DWORD temp = 0;
            VirtualProtect((LPVOID)site, 8, protect, &temp);
            FlushInstructionCache(GetCurrentProcess(), immPtr, 4);
            patchLog(fmt::format(
                "applied: EnumProcessModules cb 0x400 -> 0x2000 at Geode.dll+0x{:X}",
                site - base));
        } while (false);
    }

    {
        auto patchLog = [](const std::string& msg) {
            skkbot2::log::error("[GEODE-PATCH2] {}", msg);
            skkbot2::Logger::get().flush();
        };
        do {
            HMODULE geode = GetModuleHandleW(L"Geode.dll");
            if (!geode) { patchLog("failed: Geode.dll handle not found"); break; }
            const uintptr_t base = (uintptr_t)geode;
            const auto dos = (IMAGE_DOS_HEADER*)base;
            const auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
            auto sec = (IMAGE_SECTION_HEADER*)((uintptr_t)&nt->OptionalHeader +
                nt->FileHeader.SizeOfOptionalHeader);
            const IMAGE_SECTION_HEADER* text = nullptr;
            const IMAGE_SECTION_HEADER* pdata = nullptr;
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                if (!memcmp(sec->Name, ".text\0\0\0", 8)) { text = sec; }
                if (!memcmp(sec->Name, ".pdata\0", 8)) { pdata = sec; }
            }
            if (!text) { patchLog("failed: .text section not found"); break; }
            if (!pdata) { patchLog("failed: .pdata section not found"); break; }
            if (!s_geodeCallSite) {
                patchLog("failed: stage1 call site not found (stage1 failed)");
                break;
            }
            const uint32_t siteRva = (uint32_t)(s_geodeCallSite - base);

            auto funcs =
                (IMAGE_RUNTIME_FUNCTION_ENTRY*)(base + pdata->VirtualAddress);
            const size_t count =
                pdata->Misc.VirtualSize / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
            const IMAGE_RUNTIME_FUNCTION_ENTRY* owner = nullptr;
            {
                size_t lo = 0, hi = count;
                while (lo < hi) {
                    const size_t mid = (lo + hi) / 2;
                    if (funcs[mid].EndAddress <= siteRva) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                if (lo < count && funcs[lo].BeginAddress <= siteRva &&
                    funcs[lo].EndAddress > siteRva) {
                    owner = &funcs[lo];
                }
            }
            if (!owner) {
                patchLog(fmt::format(
                    "failed: no .pdata entry covers call site RVA 0x{:X}",
                    siteRva));
                break;
            }
            uint8_t* entry = (uint8_t*)(base + owner->BeginAddress);
            if (entry[0] == 0x31 && entry[1] == 0xC0 && entry[2] == 0xC3) {
                patchLog(fmt::format(
                    "already stubbed at Geode.dll+0x{:X}",
                    (uintptr_t)owner->BeginAddress));
                break;
            }
            static constexpr uint8_t kStub[3] = { 0x31, 0xC0, 0xC3 };
            DWORD protect = 0;
            if (!VirtualProtect((LPVOID)entry, sizeof(kStub),
                    PAGE_EXECUTE_READWRITE, &protect)) {
                patchLog("failed: VirtualProtect");
                break;
            }
            memcpy(entry, kStub, sizeof(kStub));
            DWORD temp = 0;
            VirtualProtect((LPVOID)entry, sizeof(kStub), protect, &temp);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(kStub));
            patchLog(fmt::format(
                "stubbed: userTriedToLoadDLLs -> return false at Geode.dll+0x{:X}",
                (uintptr_t)owner->BeginAddress));
        } while (false);
    }

#if 1
    std::set_terminate([] {
        skkbot2::log::error("[TERMINATE] std::terminate reached from thread!");
        if (auto eptr = std::current_exception()) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception& e) {
                skkbot2::log::error("[TERMINATE] exception: {} ({})",
                    e.what(), typeid(e).name());
            } catch (...) {
                skkbot2::log::error("[TERMINATE] non-std exception in flight");
            }
        } else {
            skkbot2::log::error("[TERMINATE] no exception object in flight (bare terminate)");
        }
        skkbot2::Logger::get().flush();
        std::abort();
    });
    std::signal(SIGABRT, [](int) {
        skkbot2::log::error("[ABORT] SIGABRT - silent-exit path confirmed");
        skkbot2::Logger::get().flush();
    });

    static CONTEXT g_crashCtx;
    static EXCEPTION_RECORD g_crashRec;
    static EXCEPTION_POINTERS g_crashPtrs;
    static MINIDUMP_EXCEPTION_INFORMATION g_crashInfo;
    static HANDLE g_crashEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        uintptr_t self = reinterpret_cast<uintptr_t>(CreateThread(nullptr, 0,
            [](LPVOID) -> DWORD {
                for (;;) {
                    WaitForSingleObject(g_crashEvent, INFINITE);
                    volatile CONTEXT dumpCtx = g_crashCtx;
                    volatile EXCEPTION_RECORD dumpRec = g_crashRec;
                    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
                    if (!dbg) continue;
                    using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD,
                        void*, void*, void*);
                    auto fn = (DumpFn)GetProcAddress(dbg, "MiniDumpWriteDump");
                    if (!fn) continue;
                    const auto ts =
                        std::chrono::system_clock::now().time_since_epoch();
                    char name[64];
                    snprintf(name, sizeof(name), "crash_v_%lld.dmp",
                        (long long)std::chrono::duration_cast<
                            std::chrono::seconds>(ts).count());
                    HANDLE h = CreateFileW(
                        skkbot2::paths::logFile(name).wstring().c_str(),
                        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (h == INVALID_HANDLE_VALUE) continue;
                    BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), h,
                        0x00001000 ,
                        &g_crashInfo, nullptr, nullptr);
                    CloseHandle(h);
                    skkbot2::log::error(
                        "[EXC-FC] minidump {} (ok={} err={})", name,
                        (int)ok, (unsigned long)GetLastError());
                    skkbot2::Logger::get().flush();
                    ResetEvent(g_crashEvent);
                }
                return 0;
            }, nullptr, 0, nullptr));
        (void)self;
    }
    AddVectoredExceptionHandler(1, [](PEXCEPTION_POINTERS info) -> LONG {
        static int sVehBudget = 16;
        if (sVehBudget <= 0 || !info || !info->ExceptionRecord) return 0;
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code < 0xC0000000 || code == 0xE06D7363) return 0;
        --sVehBudget;
        if (code == 0xC0000005 || code == 0xC0000374) {
            static bool sDumpQueued = false;
            if (sDumpQueued) {
                skkbot2::log::error("[EXC-FC] cascading fault parked tid={}",
                    GetCurrentThreadId());
                skkbot2::Logger::get().flush();
                for (;;) Sleep(INFINITE);
            }
            sDumpQueued = true;
            g_crashCtx = *info->ContextRecord;
            g_crashRec = *info->ExceptionRecord;
            g_crashPtrs.ExceptionRecord = &g_crashRec;
            g_crashPtrs.ContextRecord = &g_crashCtx;
            g_crashInfo.ThreadId = GetCurrentThreadId();
            g_crashInfo.ExceptionPointers = &g_crashPtrs;
            g_crashInfo.ClientPointers = FALSE;
            SetEvent(g_crashEvent);
            auto crashWrite = [](const char* line) {
                HANDLE h = CreateFileW(
                    skkbot2::paths::logFile("skkbot2_crash.log").wstring().c_str(),
                    FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) return;
                DWORD written = 0;
                WriteFile(h, line, lstrlenA(line), &written, nullptr);
                CloseHandle(h);
            };
            char cl[256];
            {
                const auto ts = std::chrono::system_clock::now()
                                    .time_since_epoch();
                wsprintfA(cl, "==== crash tid=%lu t=%lld ====\r\n",
                    GetCurrentThreadId(),
                    (long long)std::chrono::duration_cast<std::chrono::seconds>(
                        ts)
                        .count());
                crashWrite(cl);
            }
            const bool av = code == 0xC0000005;
            const int op =
                av ? (info->ExceptionRecord->ExceptionInformation[0] == 0
                             ? 0
                             : (info->ExceptionRecord->ExceptionInformation[0] ==
                                       1
                                   ? 1
                                   : 2 ))
                   : -1;
            const auto dataAddr =
                av ? info->ExceptionRecord->ExceptionInformation[1] : 0;
            const auto ctxRip = info->ContextRecord->Rip;
            const auto ctxRsp = info->ContextRecord->Rsp;
            HMODULE cmod = nullptr;
            char cmodDesc[160] = "?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCWSTR)ctxRip, &cmod) && cmod) {
                WCHAR wpathC[MAX_PATH] = {};
                GetModuleFileNameW(cmod, wpathC, MAX_PATH);
                char utf8C[MAX_PATH * 4] = {};
                WideCharToMultiByte(CP_UTF8, 0, wpathC, -1, utf8C,
                    sizeof(utf8C), nullptr, nullptr);
                char* cname = utf8C;
                for (char* p = utf8C; *p; ++p)
                    if (*p == '\\' || *p == '/') cname = p + 1;
                wsprintfA(cmodDesc, "%s+0x%X", cname,
                    (unsigned)(ctxRip - (uintptr_t)cmod));
            } else {
                wsprintfA(cmodDesc, "raw 0x%p", (void*)ctxRip);
            }
            wsprintfA(cl,
                "fault AV op=%s data=0x%p RIP=%s RSP=0x%p\r\n",
                op == 0 ? "read" : op == 1 ? "write" : "exec",
                (void*)dataAddr, cmodDesc, (void*)ctxRsp);
            crashWrite(cl);
            void* frames[32] = {};
            USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
            for (USHORT i = 0; i < n; ++i) {
                HMODULE fmod = nullptr;
                char desc[160] = {};
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCWSTR)frames[i], &fmod) && fmod) {
                    WCHAR wpath3[MAX_PATH] = {};
                    GetModuleFileNameW(fmod, wpath3, MAX_PATH);
                    char utf83[MAX_PATH * 4] = {};
                    WideCharToMultiByte(CP_UTF8, 0, wpath3, -1,
                        utf83, sizeof(utf83), nullptr, nullptr);
                    char* name = utf83;
                    for (char* p = utf83; *p; ++p)
                        if (*p == '\\' || *p == '/') name = p + 1;
                    wsprintfA(desc, "%s+0x%X", name,
                        (unsigned)((uintptr_t)frames[i] - (uintptr_t)fmod));
                } else {
                    wsprintfA(desc, "0x%p", frames[i]);
                }
                wsprintfA(cl, "fault-frame#%d %s\r\n", (int)i, desc);
                crashWrite(cl);
            }
            skkbot2::log::error("[EXC-FC] dump queued, crashing thread parked");
            skkbot2::Logger::get().flush();
            for (;;) Sleep(INFINITE);
        }
        const auto addr = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
        const auto paddr = (uintptr_t)info->ExceptionRecord->ExceptionInformation[1];
        const int op = (info->ExceptionRecord->ExceptionInformation[0] == 0)
                           ? 0
                           : (info->ExceptionRecord->ExceptionInformation[0] == 1 ? 1
                                                                                  : 2 );
        skkbot2::log::error("[EXC-FC] code=0x{:08X} proc={} ({}) at addr-rip=0x{:X} data=0x{:X}",
            code, op, op == 0 ? "read" : op == 1 ? "write" : "exec", addr, paddr);
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)addr, &mod) && mod) {
            WCHAR wpath[MAX_PATH] = {};
            GetModuleFileNameW(mod, wpath, MAX_PATH);
            char utf8[MAX_PATH * 4] = {};
            WideCharToMultiByte(CP_UTF8, 0, wpath, -1, utf8, sizeof(utf8), nullptr, nullptr);
            char* name = utf8;
            for (char* p = utf8; *p; ++p) if (*p == '\\' || *p == '/') name = p + 1;
            skkbot2::log::error("[EXC-FC] rip in {}+0x{:X}", name, addr - (uintptr_t)mod);
        }
        void* frames[24] = {};
        USHORT n = CaptureStackBackTrace(0, 24, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) {
            HMODULE fmod = nullptr;
            char desc[160] = {};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCWSTR)frames[i], &fmod) && fmod) {
                WCHAR wpath2[MAX_PATH] = {};
                GetModuleFileNameW(fmod, wpath2, MAX_PATH);
                char utf82[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, wpath2, -1, utf82, sizeof(utf82), nullptr, nullptr);
                char* name = utf82;
                for (char* p = utf82; *p; ++p) if (*p == '\\' || *p == '/') name = p + 1;
                wsprintfA(desc, "%s+0x%X", name,
                    (unsigned)((uintptr_t)frames[i] - (uintptr_t)fmod));
            } else {
                wsprintfA(desc, "0x%p", frames[i]);
            }
            skkbot2::log::error("[EXC-FC] frame#{} {}", (int)i, desc);
        }
        skkbot2::Logger::get().flush();
        return 0;
    });

    {
        LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(
            [](EXCEPTION_POINTERS* info) -> LONG {
                if (info && info->ExceptionRecord) {
                    if (info->ExceptionRecord->ExceptionCode >= 0xC0000000) {
                        HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
                        auto writeDump = [&](const char* suffix) {
                            if (!dbg) return;
                            using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE,
                                MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                PMINIDUMP_USER_STREAM_INFORMATION,
                                PMINIDUMP_CALLBACK_INFORMATION);
                            auto fn = (DumpFn)GetProcAddress(
                                dbg, "MiniDumpWriteDump");
                            if (!fn) return;
                            const auto ts =
                                std::chrono::system_clock::now().time_since_epoch();
                            char name[64];
                            wsprintfA(name, "crash_%lld%s.dmp",
                                (long long)std::chrono::duration_cast<
                                    std::chrono::seconds>(ts).count(),
                                suffix);
                            MINIDUMP_EXCEPTION_INFORMATION mei = {
                                GetCurrentThreadId(), info, FALSE};
                            HANDLE h = CreateFileW(
                                (skkbot2::paths::logFile(name).wstring()).c_str(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (h == INVALID_HANDLE_VALUE) return;
                            fn(GetCurrentProcess(), GetCurrentProcessId(), h,
                                (MINIDUMP_TYPE)0x00001000
                                    ,
                                &mei, nullptr, nullptr);
                            CloseHandle(h);
                            skkbot2::log::error(
                                "[EXC-FC] minidump written: {}", name);
                        };
                        writeDump("");
                    }
                    const auto addr =
                        (uintptr_t)info->ExceptionRecord->ExceptionAddress;
                    skkbot2::log::error("[EXC] code=0x{:08X} addr=0x{:X}",
                        (unsigned long long)(uintptr_t)info->ExceptionRecord->ExceptionCode,
                        (unsigned long long)addr);
                    HMODULE mod = nullptr;
                    if (GetModuleHandleExW(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &mod) && mod) {
                        WCHAR wpath[MAX_PATH] = {};
                        GetModuleFileNameW(mod, wpath, MAX_PATH);
                        char utf8[MAX_PATH * 4] = {};
                        WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                            utf8, sizeof(utf8), nullptr, nullptr);
                        const uintptr_t base = (uintptr_t)mod;
                        skkbot2::log::error(
                            "[EXC] fault module '{}' + 0x{:X} (base 0x{:X})",
                            utf8, addr - base, base);
                        static bool sSymInit = false;
                        if (!sSymInit) {
                            sSymInit = SymInitialize(GetCurrentProcess(),
                                nullptr, TRUE) != 0;
                        }
                        if (sSymInit) {
                            char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
                            auto* sym = (SYMBOL_INFO*)symBuf;
                            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                            sym->MaxNameLen = 255;
                            DWORD64 disp = 0;
                            if (SymFromAddr(GetCurrentProcess(),
                                    (DWORD64)addr, &disp, sym)) {
                                skkbot2::log::error(
                                    "[EXC] nearest symbol {} + 0x{:X}",
                                    sym->Name, (unsigned long long)disp);
                            }
                            void* frames[24] = {};
                            USHORT n = CaptureStackBackTrace(
                                0, 24, frames, nullptr);
                            for (USHORT i = 0; i < n; ++i) {
                                HMODULE fmod = nullptr;
                                if (GetModuleHandleExW(
                                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        (LPCWSTR)frames[i], &fmod) && fmod) {
                                    WCHAR wpath2[MAX_PATH] = {};
                                    GetModuleFileNameW(fmod, wpath2, MAX_PATH);
                                    char utf82[256] = {};
                                    WideCharToMultiByte(CP_UTF8, 0,
                                        wpath2, -1, utf82, sizeof(utf82),
                                        nullptr, nullptr);
                                    char* name = utf82;
                                    for (char* p = utf82; *p; ++p)
                                        if (*p == '\\' || *p == '/') name = p + 1;
                                    skkbot2::log::error(
                                        "[EXC] frame#{} {}+0x{:X}",
                                        (int)i, name,
                                        (uintptr_t)frames[i] - (uintptr_t)fmod);
                                }
                            }
                        }
                    }
                    skkbot2::Logger::get().flush();
                }
                return EXCEPTION_CONTINUE_SEARCH;
            });
        (void)prev;
    }
#endif

    logger.info("========================================");
    logger.info("skkBot2 v{} initializing, build {} {} (M1: SKK3 Record/Play + TPS)",
                MOD_VERSION, __DATE__, __TIME__);
    logger.info("========================================");

    namespace fs = std::filesystem;
    auto failedDirs = skkbot2::paths::ensureDataFolders();
    for (auto const& f : failedDirs) {
        logger.error("Failed to create mod data folder: {}", skk::pathToUtf8(f));
    }
    skkbot2::render::Renderer::prepareFfmpegDropIn();

    for (auto const& [label, p] : skkbot2::paths::summary()) {
        logger.info("[PATHS] {:<10} -> {}", label, skk::pathToUtf8(p));
    }

    auto pf = skk::skk3::selfTestPerFrameCodec();
    logger.info(
        "skk3 per-frame codec v4 self-test: ok={} frames={} v3={}B v4={}B ratio={:.1f}x",
        pf.ok, pf.frames, pf.v3Bytes, pf.v4Bytes,
        pf.v4Bytes > 0 ? static_cast<double>(pf.v3Bytes) / static_cast<double>(pf.v4Bytes) : 0.0);
    if (!pf.ok) {
        using Stage = skk::skk3::PerFrameSelfTestResult::Stage;
        auto stageName = [&]() -> const char* {
            switch (pf.stage) {
                case Stage::ReadFailed: return "read-failed";
                case Stage::CountMismatch: return "count-mismatch";
                case Stage::FieldMismatch: return "field-mismatch";
                case Stage::RewriteMismatch: return "rewrite-mismatch";
                default: return "unknown";
            }
        };
        logger.error("skk3 per-frame codec v4 self-test FAILED - do not trust new .skk files "
            "(stage={} failIdx={} expected={} decoded={} detail={})",
            stageName(), pf.failIdx, pf.expected, pf.decoded, pf.failDetail);
    }

    auto crc = skk::skk3::selfTestCrc();
    logger.info("skk3 crc-32c integrity self-test: ok={}{}", crc.ok,
        crc.ok ? "" : " detail=" + crc.detail);
    if (!crc.ok) {
        logger.error("skk3 crc-32c integrity self-test FAILED - new .skk files lack "
            "corruption detection ({})", crc.detail);
    }
    auto hd = skk::skk3::selfTestHeader();
    logger.info(
        "skk3 header self-test: ok={} oldShape={}B", hd.ok, hd.oldShapeHeaderBytes);
    if (!hd.ok) {
        logger.error("skk3 header self-test FAILED - {}",
            hd.detail.empty() ? "no detail" : hd.detail);
    }

    skk::skc::Bot::get().setIgnoreInputs(
        Mod::get()->getSavedValue<bool>("gui_skc_ignore_inputs", true)
    );

    if (!skk::skc::selfTest()) {
        logger.error("skc codec self-test FAILED - do not trust new .skc files");
    } else {
        logger.info("skc codec self-test ok");
    }

    if (!skk::skc::selfTestRngJournal()) {
        logger.error("skc rng-journal self-test FAILED - rng tail may bloat or corrupt");
    }

#ifdef GEODE_IS_WINDOWS
    {
        const int problems = skk::offsets::logReport();
        if (problems > 0) {
            logger.warn("offsets: {} сайтов не сверились - смотри строки выше; "
                        "при новом билде GD правится src/version_offsets.hpp",
                        problems);
        }
    }

    skk::installStepGridHooks();

    skk::installUiInputWallHooks();
    skkbot2::fps::installOptionKeyTracer();

    skk::installRngMidhooks();

    skk::installCocosStringOpt();
#endif

    skkbot2::gui::get().init();

    if (!skkbot2::fps::enabled()) {
        skkbot2::fps::restoreVsyncOnStartup();
    }

    ImGuiCocos::get()
        .setup([] {
            auto& io = ImGui::GetIO();
            float factor = std::max(1.0f, geode::utils::getDisplayFactor());
            int fontSize = static_cast<int>(std::round(18.0f * factor));

            const auto resDir = skkbot2::paths::resources();
            const auto fontsFolder = skkbot2::paths::fonts();
            std::error_code ec;
            std::filesystem::create_directories(fontsFolder, ec);

            std::string customFont = Mod::get()->getSavedValue<std::string>("gui_custom_font", "");
            ImFont* loaded = nullptr;

            if (!customFont.empty() && customFont != "Default") {
                const auto customPath = fontsFolder / customFont;
                if (std::filesystem::exists(customPath, ec)) {
                    ImFontConfig customCfg;
                    customCfg.OversampleH = 3;
                    customCfg.OversampleV = 3;
                    std::string fontPathStr = skk::pathToUtf8(customPath);
                    loaded = io.Fonts->AddFontFromFileTTF(
                        fontPathStr.c_str(), static_cast<float>(fontSize), &customCfg,
                        io.Fonts->GetGlyphRangesCyrillic());
                    if (loaded) {
                        skkbot2::Logger::get().info(
                            "skkBot2 custom font '{}' loaded (size {})", customFont, fontSize);
                    } else {
                        skkbot2::Logger::get().warn(
                            "skkBot2 could not load custom font '{}'; falling back to default", customFont);
                    }
                } else {
                    skkbot2::Logger::get().warn(
                        "skkBot2 custom font '{}' not found on disk; falling back to default", customFont);
                }
            }

            if (!loaded) {
                ImFontConfig cfg;
                cfg.OversampleH = 3;
                cfg.OversampleV = 3;
                cfg.GlyphExtraAdvanceX = -0.45f;
                auto defaultFontPath = skk::pathToUtf8(resDir / "font_main.ttf");
                loaded = io.Fonts->AddFontFromFileTTF(
                    defaultFontPath.c_str(), static_cast<float>(fontSize), &cfg,
                    io.Fonts->GetGlyphRangesCyrillic());
                if (loaded) {
                    skkbot2::Logger::get().info(
                        "skkBot2 default font loaded (size {})", fontSize);
                } else {
                    skkbot2::Logger::get().warn(
                        "skkBot2 could not load default font; using default");
                }
            }

            if (loaded) {
                io.FontDefault = loaded;
                io.FontGlobalScale = 1.0f;
            }
        })
.draw([] {
              skkbot2::hot::Timed skkbot2Timing(skkbot2::hot::ImguiHook);
              auto& renderer = skkbot2::render::Renderer::get();

            static double lastDump = 0.0;
            if (!renderer.isRecording()) {
                const double now = skkbot2::hot::now();
                if (now - lastDump > 5.0) {
                    lastDump = now;
                    auto& hs = skkbot2::hot::stats();
                    std::string table = "[HOT] hot path, last 5s of play:";
                    for (size_t i = 0; i < skkbot2::hot::N; i++) {
                        if (hs.calls[i] == 0 && hs.ms[i] == 0.0) continue;
                        const double perSec = hs.calls[i] / 5.0;
                        const char* slotName =
                            skkbot2::hot::name(static_cast<skkbot2::hot::Slot>(i));
                        table += std::string("\n  ") + slotName + " = " +
                                 std::to_string(hs.calls[i]) + " calls (" +
                                 std::to_string(static_cast<long long>(perSec)) + "/s)";
                        if (hs.ms[i] > 0.0) {
                            table += ", " + std::to_string(hs.ms[i] / 5.0) +
                                     "ms per wall second";
                        }
                    }
                    skkbot2::log::info("{}", table);
                    {
                        auto& s = skkbot2::hot::stats();
                        const double hookMs = s.ms[static_cast<size_t>(skkbot2::hot::SchedUpdate)];
                        const double gdMs = s.ms[static_cast<size_t>(skkbot2::hot::SchedVanilla)];
                        const double perHook = s.calls[static_cast<size_t>(skkbot2::hot::SchedUpdate)];
                        std::string verdict =
                            std::string("\n  >>> mod's own share = ") +
                            std::to_string(std::max(0.0, hookMs - gdMs) / 5.0) +
                            "ms per wall second, " +
                            std::to_string(perHook > 0 ? std::max(0.0, hookMs - gdMs) / perHook : 0.0) +
                            "us per hook call (" +
                            std::to_string(hookMs / 5.0) +
                            "ms hook total, " + std::to_string(gdMs / 5.0) +
                            "ms of it GD's base scheduler)";
                        verdict += "\n  >>> always-on per-player hooks: " +
                                   std::to_string(
                                       (s.ms[static_cast<size_t>(skkbot2::hot::PlayerUpdate)] +
                                        s.ms[static_cast<size_t>(skkbot2::hot::WtsPlayerUpdate)] +
                                        s.ms[static_cast<size_t>(skkbot2::hot::MacroPlayerUpdate)]) /
                                        5.0) +
                                   "ms per wall second";
                        skkbot2::log::info("{}", verdict);
                    }
                    for (size_t i = 0; i < skkbot2::hot::N; i++) {
                        hs.calls[i] = 0;
                        hs.ms[i] = 0.0;
                    }
                }
            }

            renderer.onFrameDraw();

            if (renderer.isRecording() && !renderer.levelPaused() &&
                skkbot2::gui::get().renderHideMenu) {
                return;
            }

            skkbot2::gui::blur::newFrame();

            renderer.useWindowViewForUi();

            skkbot2::gui::get().draw();
        });

    KeyboardInputEvent().listen([](KeyboardInputData& data) {
        auto& gui = skkbot2::gui::get();
        const bool down = data.action != KeyboardInputData::Action::Release;
        const bool repeat = data.action == KeyboardInputData::Action::Repeat;
        int key = static_cast<int>(data.key);
        switch (data.native.extra & 0xFF) {
            case 0x38: key = 0xA5; break;
            case 0x1D: key = 0xA3; break;
            case 0x2A: key = 0xA0; break;
            case 0x36: key = 0xA1; break;
            default: break;
        }
        if (gui.kbWaiting >= 0) {
            gui.handleKey(key, down, repeat);
            return ListenerResult::Propagate;
        }
        if (gui.open && down && ImGui::GetIO().WantTextInput) {
            return ListenerResult::Propagate;
        }
        gui.handleKey(key, down, repeat);
        return ListenerResult::Propagate;
    }).leak();

    MouseInputEvent().listen([](MouseInputData& data) {
        if (ImGuiCocos::get().isInitialized()) {
            auto& io = ImGui::GetIO();
            int btn = -1;
            if (data.button == MouseInputData::Button::Right) btn = 1;
            else if (data.button == MouseInputData::Button::Middle) btn = 2;
            if (btn >= 0) {
                io.AddMouseButtonEvent(btn, data.action == MouseInputData::Action::Press);
            }
        }
        return ListenerResult::Propagate;
    }).leak();

    skk::practice_music::applyUnlock();

    logger.info("skkBot2 ready");
}
