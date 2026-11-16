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
    // INFO, not Debug: the diag push/release lines fire up to ~14k times a
    // second in a dual playback (Schafs), and every line above the level still
    // pays fmt::format. At Debug that measured as a share of the frame budget
    // hiding inside the [HOT] "GD base scheduler" number (BUG-70 session,
    // 21:45). The instrument stays: raise the level in the GUI (Log Level)
    // when a button-seam or drift hunt needs the flood back.
    logger.setLevel(skkbot2::Logger::Info);
    logger.setFileLogging(true);
    // BUG-120 (B): capture the module table once for describeSite resolution
    // in the memtrack DOUBLE FREE report.
    skkbot2::memtrack::initModules();

    // --- BUG-120: runtime patch of a Geode alpha.1 loader bug.
    //
    // Loader::Impl::userTriedToLoadDLLs() (loader/src/platform/windows/
    // LoaderImpl.cpp:30) passes `mods.size()` (1024 BYTES) as the cb argument
    // of EnumProcessModules instead of 1024*sizeof(HMODULE) (8192). Only the
    // first 128 HMODULE slots are filled, but the loop runs `needed/8` times:
    // everything past index 128 is uninitialized stack read as HMODULE, the
    // GetModuleFileNameExW call fails on the garbage handle and an
    // uninitialized wchar buffer is scanned to NUL and copied - which on
    // page-heap deterministically AVs inside memcpy from a dead pointer.
    // GD 2.2081 loads >128 DLLs, so this fires on every MenuLayer::init.
    //
    // Patch site (verified by live disassembly of Geode.dll):
    //   RVA 0x124111: 41 B8 00 04 00 00   mov r8d, 400h   (cb = 1024)
    //   RVA 0x124117: E8 14 F5 92 00      call K32EnumProcessModules
    //
    // Strategy: resolve the IAT slot of K32EnumProcessModules, find the FF 25
    // rip-relative jump thunk to it, then locate the unique `mov r8d, 400h` +
    // `call <thunk>` pair in .text and rewrite the immediate to 0x2000 (8192).
    // Anchoring on the IAT makes a false-positive hit essentially impossible.
    // The found call-site address is shared with stage 2, which maps it to its
    // owner function via .pdata.
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

            // 1) IAT slot RVA of K32EnumProcessModules.
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
                            // The FirstThunk array is the IAT: the slot we
                            // need is (FirstThunk RVA) + i*sizeof(slot).
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

            // 2) The .text section bounds (code to scan).
            auto sec = (IMAGE_SECTION_HEADER*)((uintptr_t)&nt->OptionalHeader +
                nt->FileHeader.SizeOfOptionalHeader);
            const IMAGE_SECTION_HEADER* text = nullptr;
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                if (!memcmp(sec->Name, ".text\0\0\0", 8)) { text = sec; break; }
            }
            if (!text) { patchLog("failed: .text section not found"); break; }
            uint8_t* tstart = (uint8_t*)(base + text->VirtualAddress);
            const size_t tsize = text->Misc.VirtualSize;

            // 3) Find the `jmp [rip+disp32]` thunk whose target is the IAT slot.
            const uintptr_t iatSlotAddr = base + iatSlotRva;
            uintptr_t thunkAddr = 0;
            for (size_t i = 0; i + 6 <= tsize; ++i) {
                if (tstart[i] == 0xFF && tstart[i + 1] == 0x25) {
                    int32_t disp;
                    memcpy(&disp, &tstart[i + 2], 4);
                    // instruction: bytes i..i+5, rip after it = base+Virt+i+6
                    const uintptr_t after = base + text->VirtualAddress + i + 6;
                    if ((uintptr_t)((int64_t)after + (int64_t)disp)
                        == iatSlotAddr) {
                        // Target of `call rel32` is the START of the target
                        // instruction, not the byte after it - store the
                        // instruction's own address.
                        thunkAddr = base + text->VirtualAddress + i;
                        break;
                    }
                }
            }
            if (!thunkAddr) {
                patchLog("failed: jmp-thunk to K32EnumProcessModules not found");
                break;
            }

            // 4) Unique `41 B8 imm32` followed by `E8 rel32` whose target is
            //    the thunk; assert imm == 0x400 (the known bug value).
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
            const uint32_t newVal = 0x2000; // 1024 * sizeof(HMODULE)
            memcpy(immPtr, &newVal, 4);
            DWORD temp = 0;
            VirtualProtect((LPVOID)site, 8, protect, &temp);
            FlushInstructionCache(GetCurrentProcess(), immPtr, 4);
            patchLog(fmt::format(
                "applied: EnumProcessModules cb 0x400 -> 0x2000 at Geode.dll+0x{:X}",
                site - base));
        } while (false);
    }

    // Stage 2: stub out userTriedToLoadDLLs entirely. The function has TWO
    // bugs and the first fix only removed one of them. The second one is
    // unpatchable by constants: `GetModuleFileNameExW(..., modName.size())`
    // passes 1024 as capacity but the buffer is std::array<wchar_t, MAX_PATH>
    // (260) - long DLL paths overflow the stack frame. The function only
    // exists to show a popup for manually-dropped DLLs; `return false` is
    // harmless and kills both bugs. Located ADDRESS-AGNOSTICALLY: stage 1
    // already found the unique EnumProcessModules call site inside it; the
    // owning function's start comes from the PE .pdata exception table
    // (RUNTIME_FUNCTION), which follows the actual code layout of whatever
    // Geode.dll build is installed. (Hardcoded prologue failed live
    // 00:59: 0 matches -> crash returned at Geode.dll+0x124392.)
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

            // .pdata: sorted RUNTIME_FUNCTION array by BeginAddress. Find the
            // entry covering the call site; its BeginAddress is the true
            // function start.
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
            // xor eax,eax; ret  ==  return false (the ABI returns bool in
            // al/eax; the this pointer and all locals just die).
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

    // --- DEBUG (BUG-120): silent-exit detector. Re-enabled: the startup AV
    // turned out to be the f28c166 shader scope bug, NOT these handlers
    // (bisect 8dee5ab still crashed, root found via Geode crash trace).
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

    // --- BUG-120 finale: first-chance VEH logger. The UEF probe above only
    // fires AFTER the dispatcher unwound, so its stack misses the faulting
    // frames. The VEH below runs BEFORE dispatch: with page-heap enabled the
    // first access violation IS the corruption, and its stack at that moment
    // is the true culprit chain. Throttled to the first 16 fatal-range
    // exceptions per process to stay readable.
    static CONTEXT g_crashCtx;
    static EXCEPTION_RECORD g_crashRec;
    static EXCEPTION_POINTERS g_crashPtrs;
    static MINIDUMP_EXCEPTION_INFORMATION g_crashInfo;
    static HANDLE g_crashEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        // Dump watcher: writes the minidump from the parked faulting thread
        // (MiniDumpWriteDump cannot run inside the VEH itself - E_INVALIDARG).
        uintptr_t self = reinterpret_cast<uintptr_t>(CreateThread(nullptr, 0,
            [](LPVOID) -> DWORD {
                for (;;) {
                    WaitForSingleObject(g_crashEvent, INFINITE);
                    // BUG-131 diag v2: locals ON THIS THREAD'S STACK hold the
                    // true fault context/record - MiniDumpWithThreadInfo dumps
                    // every thread stack, so the crash dump carries the real
                    // RIP (the caller that jumped to the bad address) even
                    // though the dumped ExceptionStream context is the parked
                    // Sleep context.
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
                        0x00001000 /*MiniDumpWithThreadInfo - stacks only, small*/,
                        &g_crashInfo, nullptr, nullptr);
                    CloseHandle(h);
                    skkbot2::log::error(
                        "[EXC-FC] minidump {} (ok={} err={})", name,
                        (int)ok, (unsigned long)GetLastError());
                    skkbot2::Logger::get().flush();
                    // The crash event is manual-reset; without resetting it
                    // this loop re-enters immediately and re-writes dumps
                    // forever (live 08:21: one file per ~0.15s until death).
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
        if (code < 0xC0000000 || code == 0xE06D7363) return 0; // skip C++ exc ids + SEH leftovers
        --sVehBudget;
        // BUG-120 (B) 09.11: MiniDumpWriteDump CANNOT be called from inside
        // the crashing thread's VEH (E_INVALIDARG live 03:29). Instead: park
        // the faulting thread by returning EXCEPTION_CONTINUE_EXECUTION after
        // the VEH - the faulting instruction retries forever (the process is
        // already dead) - while a dedicated watcher thread (spawned below)
        // wakes on an event and writes the dump from a copied CONTEXT.
        if (code == 0xC0000005 || code == 0xC0000374) {
            static bool sDumpQueued = false;
            if (sDumpQueued) {
                // A second faulting thread: park it too, otherwise cascading
                // faults spam the dispatcher while the first crash hangs the
                // process (live 08:21: 8 extra faults in ~6s).
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
            // BUG-131 diag v2 (13.11): the previous attempt logged through
            // skkbot2::log::error, which mirrors to geode::log::info BEFORE the
            // file write (logger.cpp:141) - live crash 14:27 left skkbot2.log
            // at 0 bytes, so the [EXC-FC] lines never reached disk. In a VEH
            // the geode log pipeline is not safe. All diagnostics now go
            // through a raw CreateFileW/WriteFile crash log (no heap, no fmt,
            // no Logger, no geode log) - synchronous and durable.
            auto crashWrite = [](const char* line) {
                HANDLE h = CreateFileW(
                    skkbot2::paths::logFile("skkbot2_crash.log").wstring().c_str(),
                    FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) return;
                DWORD written = 0;
                WriteFile(h, line, lstrlenA(line), &written, nullptr);
                CloseHandle(h); // close per line = unbuffered, survives kill
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
            // op/data only defined for AV records; heap corruption (0xC0000374)
            // may carry 0 params - guard everything.
            const bool av = code == 0xC0000005;
            const int op =
                av ? (info->ExceptionRecord->ExceptionInformation[0] == 0
                             ? 0 /*read*/
                             : (info->ExceptionRecord->ExceptionInformation[0] ==
                                       1
                                   ? 1 /*write*/
                                   : 2 /*exec*/))
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
            // Let the watcher own the process; this thread never returns.
            for (;;) Sleep(INFINITE);
        }
        const auto addr = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
        const auto paddr = (uintptr_t)info->ExceptionRecord->ExceptionInformation[1];
        const int op = (info->ExceptionRecord->ExceptionInformation[0] == 0)
                           ? 0 /*read*/
                           : (info->ExceptionRecord->ExceptionInformation[0] == 1 ? 1 /*write*/
                                                                                  : 2 /*exec*/);
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
        return 0; // continue search - debuggers/handlers proceed normally
    });

    // Access-violation / fastfail probe: log the fatal code + faulting
    // address so a silent AV (no dialog, kernel-terminated process) is still
    // identifiable from the log tail. BUG-120 final step: resolve the fault
    // address to module+offset and (dbghelp) the nearest function symbol,
    // because the Geode crash dialog does not always appear for this crash.
    {
        LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(
            [](EXCEPTION_POINTERS* info) -> LONG {
                if (info && info->ExceptionRecord) {
                    // BUG-120 (B): full minidump on fatal-range exceptions so
                    // the heap-corruption culprits (Second dtor call, ALL
                    // stacks) can be opened offline in WinDbg with the exe
                    // PDBs. Written before the log tail so the dump survives
                    // the process teardown.
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
                                    /*MiniDumpWithFullMemoryThreadStacks*/,
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
                    // Module name + offset from the faulting address.
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
                        // Nearest function symbol via dbghelp (best effort;
                        // anything failing here must never skip the module
                        // line above).
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
                            // Call stack: up to 24 frames of raw addresses.
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
                                    // Keep just the file name part.
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
    // BUILD-STAMP (14.11): the first log line carries the compile date/time so
    // any session report starts by proving WHICH build produced the log (the
    // PLAY-diagnostics session repeatedly analysed logs from stale dlls).
    logger.info("skkBot2 v{} initializing, build {} {} (M1: SKK3 Record/Play + TPS)",
                MOD_VERSION, __DATE__, __TIME__);
    logger.info("========================================");

    namespace fs = std::filesystem;
    // Ensure all standard mod data folders exist from startup
    auto failedDirs = skkbot2::paths::ensureDataFolders();
    for (auto const& f : failedDirs) {
        logger.error("Failed to create mod data folder: {}", skk::pathToUtf8(f));
    }
    skkbot2::render::Renderer::prepareFfmpegDropIn();

    for (auto const& [label, p] : skkbot2::paths::summary()) {
        logger.info("[PATHS] {:<10} -> {}", label, skk::pathToUtf8(p));
    }

    // Per-frame v4 codec self-check: synthetic round-trip + v3/v4 size ratio.
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

    // CRC-32C integrity self-check: clean round-trip + corrupted-byte rejection.
    auto crc = skk::skk3::selfTestCrc();
    logger.info("skk3 crc-32c integrity self-test: ok={}{}", crc.ok,
        crc.ok ? "" : " detail=" + crc.detail);
    if (!crc.ok) {
        logger.error("skk3 crc-32c integrity self-test FAILED - new .skk files lack "
            "corruption detection ({})", crc.detail);
    }
    // Header self-test. Separate from the codec one on purpose: that one
    // checks that new files are written and read consistently, and this one
    // checks the promise the other half of the change rests on - that .skk
    // files written before the display rate existed keep loading. A codec
    // round-trip cannot catch that, because it only ever sees current files.
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

    // .skc codec self-check, same idea as the skk3 one above: a broken
    // recorder would silently write unreadable macros.
    if (!skk::skc::selfTest()) {
        logger.error("skc codec self-test FAILED - do not trust new .skc files");
    } else {
        logger.info("skc codec self-test ok");
    }

    // RNG-журнал v3 (advance-кодирование LCG-шагов): round-trip + метрика.
    if (!skk::skc::selfTestRngJournal()) {
        logger.error("skc rng-journal self-test FAILED - rng tail may bloat or corrupt");
    }

#ifdef GEODE_IS_WINDOWS
    // Реестр версионно-хрупких мест: побайтовая сверка всех сайтов при старте.
    // ОБЯЗАТЕЛЬНО до установки любых хуков/патчей: мидхуки перезаписывают
    // первые байты сайтов трамплинами, и сверка после установки видела бы
    // уже пропатченные байты («НЕ НАЙДЕН» на исправных сайтах, ложные
    // «ПЕРЕМЕЩЁН» по случайным совпадениям). Проблемные сайты видны в логе
    // сразу, а не через баги в рантайме.
    {
        const int problems = skk::offsets::logReport();
        if (problems > 0) {
            logger.warn("offsets: {} сайтов не сверились - смотри строки выше; "
                        "при новом билде GD правится src/version_offsets.hpp",
                        problems);
        }
    }

    // Install the step-grid midhooks NOW (module load), never lazily from
    // inside GJBaseGameLayer::update - patching trampoline bytes on the same
    // thread currently executing the target function is a freeze.
    skk::installStepGridHooks();

    // BUG-105b v3: raw-хуки UILayer::keyDown/handleAction (safetyhook).
    // Тоже при загрузке модуля; реестр офсетов выше уже сверил байты сайтов.
    skk::installUiInputWallHooks();
    // Read-only. Prints the key behind every option GD's own settings flip, so
    // the vertical-sync switch can be identified by observation. See
    // src/core/gd_option_tracer.cpp for why it is a tracer and not a guess.
    skkbot2::fps::installOptionKeyTracer();

    // Install RNG midhooks (teleport LCG interception) at module load.
    skk::installRngMidhooks();

    // CCString-оптимизация: тоже после отчёта (хук перезаписывает байты сайта).
    skk::installCocosStringOpt();
#endif

    skkbot2::gui::get().init();

    // If the user's FPS toggle is OFF, any previous session that left VSync
    // disabled (game crashed while a pin was active, or the user quit mid-pin)
    // needs to be cleaned up. This runs after gui.init() which loads
    // gui_fps_enabled and pushes it into the fps module, so the module's own
    // g_enabled is already the user's intended state.
    if (!skkbot2::fps::enabled()) {
        skkbot2::fps::restoreVsyncOnStartup();
    }

    ImGuiCocos::get()
        .setup([] {
            // Rasterize a real TTF at a DPI-aware size so text is crisp (the
            // default imgui font is tiny and gets upscaled -> blurry), and so
            // Cyrillic renders instead of boxes.
            // Custom font: any TTF in the `fonts` folder can be picked via COSMETICS;
            // falls back to the bundled Roboto (`font_main.ttf`).
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
            // ONE font for the whole bot: every label, readout, number and the
            // wordmark render in the font loaded above.
        })
.draw([] {
              // Times the WHOLE imgui draw hook body. ImGui's draw hook is where
              // the render issues its capture, so this slot contains the capture
              // plus ImGui itself, and Stage::Grab inside it is already counted
              // separately. The difference is what ImGui costs on its own -
              // which nobody could read before, because the hook runs INSIDE
              // swapBuffers and was therefore buried in the swap stage, where it
              // was mistaken for vsync wait. RAII so the early return below for
              // a hidden menu still lands the elapsed time.
              skkbot2::hot::Timed skkbot2Timing(skkbot2::hot::ImguiHook);
              auto& renderer = skkbot2::render::Renderer::get();

            // The hot-path table used to be printed only at the end of a render
            // (renderer.cpp:2141), which made it useless for the question it was
            // written for: what does the bot add to the game OUTSIDE recording?
            // This prints it periodically during ordinary play, so the numbers
            // can be read from the log after a level without starting a render.
            // Throttled to keep it out of the hot path's own noise.
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
                    // The one number the whole counter set exists for: what the
                    // MOD costs, with GD's own physics subtracted.
                    //
                    // sched.update(hook) alone is misleading. The hook runs once
                    // per drive step and calls CCScheduler::update up to
                    // kMaxBatchesPerFrame times, so the outer timer mostly
                    // measures the game's simulation - measured 287.9ms per
                    // wall second on 2026-10-18, which reads as "the bot eats
                    // 29% of the frame" and is not what that number means.
                    // SchedUpdate - SchedVanilla is the bot's own share.
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

            // Issue any capture pending from the renderer's last pts boundary
            // BEFORE the GUI overlay is drawn, so the bot menu never leaks
            // into the video (the capture reads a fresh backbuffer).
            renderer.onFrameDraw();

            // While a render is running and the level is actually being captured, the bot
            // draws NOTHING. The capture above already copied the level into its own
            // FBO, so every pixel of ImGui output is discarded - but it still costs
            // a full per-frame pass: gradient text, capsules, animations, blur
            // regions, all laid out and vertex-converted at 4K. Measured at ~10 s
            // over a 45 s level, so nothing is drawn here.
            //
            // The pause is the exception, and it is what makes this usable: a
            // paused level produces no frames, so a menu costs nothing and the
            // menu (with its STOP RENDER button) has to be reachable. Pause, stop
            // the render, resume - back in the level the menu vanishes again.
            if (renderer.isRecording() && !renderer.levelPaused() &&
                skkbot2::gui::get().renderHideMenu) {
                return;
            }

            // Reset blur regions for new ImGui frame
            skkbot2::gui::blur::newFrame();

            // The capture above swaps the cocos view to the OUTPUT size for its
            // visit()-based grab; switch back so the imgui menu that follows is
            // laid out (and vertex-converted) at the real window size — the same
            // height it has outside a render.
            renderer.useWindowViewForUi();

            skkbot2::gui::get().draw();
        });

    // Configurable keybind toggles the menu from anywhere (default F8).
    KeyboardInputEvent().listen([](KeyboardInputData& data) {
        auto& gui = skkbot2::gui::get();
        const bool down = data.action != KeyboardInputData::Action::Release;
        const bool repeat = data.action == KeyboardInputData::Action::Repeat;
        // Raw input can report right-Alt (AltGr) as Ctrl and vice versa; the
        // cocos enum then loses which modifier it really was. The physical
        // scan code is the ground truth, so disambiguate Alt/Ctrl/Shift by it.
        int key = static_cast<int>(data.key);
        switch (data.native.extra & 0xFF) {
            case 0x38: key = 0xA5; break; // Alt (left scan 0x38, right scan E0 0x38)
            case 0x1D: key = 0xA3; break; // Ctrl (left scan 0x1D, right scan E0 0x1D)
            case 0x2A: key = 0xA0; break; // Left Shift
            case 0x36: key = 0xA1; break; // Right Shift
            default: break;
        }
        // While capturing a new keybind, consume the key regardless of ImGui focus.
        if (gui.kbWaiting >= 0) {
            gui.handleKey(key, down, repeat);
            return ListenerResult::Propagate;
        }
        // Don't trigger hotkeys while typing into an ImGui text input field when menu is open
        if (gui.open && down && ImGui::GetIO().WantTextInput) {
            return ListenerResult::Propagate;
        }
        gui.handleKey(key, down, repeat);
        return ListenerResult::Propagate;
    }).leak();

    // Dispatch right and middle mouse clicks to ImGui (gd-imgui-cocos only touches left click).
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

    // Practice Music Sync (always on, no toggle): unlock GD's hidden item 17 so
    // GD's own "Practice Music Sync" option is live and the layer's
    // m_practiceMusicSync gets set on every level load. Applied at module load
    // as well as from PlayLayer::init so GD's Game Options screen reads it as
    // enabled even before any level is opened. See hacks/practice_music_sync.cpp.
    skk::practice_music::applyUnlock();

    logger.info("skkBot2 ready");
}
