#pragma once


#include <Geode/Geode.hpp>
#include <logger/logger.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

namespace skk::offsets {

struct Site {
    const char* name;
    uintptr_t rva;
    const char* sig;
    const char* semantics;
    const char* source;
    const char* module;
};


inline constexpr Site kTpsExpectedTicks{
    "tps.expected_ticks",
    0x3408D9,
    "FF 90 ?? ?? ?? ?? F3 0F 10 ?? ?? ?? ?? ?? F3 44 0F 10 ?? ?? ?? ?? 00 F3 41 0F 5D",
    "store счётчика ожидаемых тиков внешнего физического цикла (TPS bypass); патч-сайт = RVA+6",
    "IDA 2.2091 (sub_1403406D0) + байты бинаря; сигнатура ранее в physicsbypass.cpp",
    "gd",
};

inline constexpr Site kStepGridStepCount{
    "step_grid.step_count",
    0x340B18,
    "44 89 6C 24 50 45 0F 57 FF F2 45 0F 5A FD",
    "store начального счётчика внутреннего цикла шагов физики (r13d в 2.2091)",
    "silicate-мидхуки, байты сверены по бинарю 2.2091 (Xenon alpha.21: 0x340B18)",
    "gd",
};

inline constexpr Site kStepGridRestoreDt{
    "step_grid.restore_dt",
    0x341BC7,
    "F3 0F 10 86 20 05 00 00 41 0F 2F C4 0F 97 C0 88 86 1D 05 00 00",
    "пост-цикловое восстановление фактически потреблённого dt (xmm9)",
    "silicate-мидхуки, байты сверены по бинарю 2.2091 (Xenon alpha.21: 0x341BC7)",
    "gd",
};

inline constexpr Site kRngTeleport{
    "rng.teleport_lcg",
    0x31604A,
    "48 C1 E8 10 25 FF 7F 00 00 66 0F 6E C8 0F 5B C9",
    "после inline-LCG телепорт-портала: shr rax,10; and eax,0x7FFF",
    "байты из GeometryDash.exe 2.2091 (снято 05.11.2026)",
    "gd",
};

inline constexpr Site kRngShake1{
    "rng.shake_rand_1",
    0x34808A,
    "FF 15 ?? ?? ?? ?? BB 01 00 00 00 66 0F 6E C0 41 8B C7",
    "call rand() для shake (сайт 1 из 4), после вызова mov ebx,1",
    "байты из GeometryDash.exe 2.2091 (снято 05.11.2026); disp32 IAT замаскирован",
    "gd",
};
inline constexpr Site kRngShake2{
    "rng.shake_rand_2",
    0x3480B8,
    "FF 15 ?? ?? ?? ?? 66 0F 6E C0 0F 5B C0 F3 0F 5E 05 ?? ?? ?? ??",
    "call rand() для shake (сайт 2 из 4)",
    "байты из GeometryDash.exe 2.2091 (снято 05.11.2026); disp замаскированы",
    "gd",
};
inline constexpr Site kRngShake3{
    "rng.shake_rand_3",
    0x3480E2,
    "FF 15 ?? ?? ?? ?? 66 0F 6E F8 0F 5B FF F3 0F 5E 3D ?? ?? ?? ??",
    "call rand() для shake (сайт 3 из 4)",
    "байты из GeometryDash.exe 2.2091 (снято 05.11.2026); disp замаскированы",
    "gd",
};
inline constexpr Site kRngShake4{
    "rng.shake_rand_4",
    0x348100,
    "FF 15 ?? ?? ?? ?? 66 0F 6E D0 0F 5B D2 F3 0F 5E 15 ?? ?? ?? ??",
    "call rand() для shake (сайт 4 из 4)",
    "байты из GeometryDash.exe 2.2091 (снято 05.11.2026); disp замаскированы",
    "gd",
};

inline constexpr Site kToggleGameVariable{
    "diag.toggle_game_variable",
    0x27E770,
    "48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 20 48 8B FA 48 8B 0D ?? ?? ?? ??",
    "GameManager::toggleGameVariable — пролог функции",
    "биндинги 2.2091; байты из GeometryDash.exe 2.2091 (снято 05.11.2026)",
    "gd",
};

inline constexpr Site kCocosStringInitWithFormat{
    "cocos.ccs_string_init_with_format",
    0x107A0,
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 "
    "48 8B E9 49 8B F8 B9 00 90 01 00 48 8B F2 FF 15 ?? ?? ?? ??",
    "CCString::initWithFormatAndValist — пролог (cocos-модуль!)",
    "байты из libcocos2d.dll 2.2091 (снято 05.11.2026); disp call замаскирован; сигнатура уникальна в .text",
    "cocos",
};


inline constexpr uintptr_t kRngPrimaryGlobalRva = 0x90BF68;
inline constexpr uintptr_t kRngTeleportGlobalRva = 0x90BFF0;

inline constexpr Site kRngPrimaryGlobal{
    "rng.primary_global",
    kRngPrimaryGlobalRva,
    nullptr,
    "data-глобал primary RNG (LCG 214013/2531011 = 0x343FD/0x269EC3)",
    "верифицируется автоматически: RVA должен встречаться среди целей imul rax,[rip+d],0x343FD (см. verifyDataGlobals)",
    "gd",
};
inline constexpr Site kRngTeleportGlobal{
    "rng.teleport_global",
    kRngTeleportGlobalRva,
    nullptr,
    "data-глобал teleport RNG",
    "верифицируется автоматически: imul на 0x316039 (конец рядом с сайтом rng.teleport_lcg) указывает на этот RVA",
    "gd",
};


inline constexpr uintptr_t kFieldPauseFlagOffset = 13144;
inline constexpr uintptr_t kFieldResumeGraceCtrOffset = 13220;

inline constexpr Site kFieldPauseFlag{
    "field.gjbasegamelayer.pause_flag",
    kFieldPauseFlagOffset,
    nullptr,
    "офсет поля GJBaseGameLayer: байтовый pause/frozen-флаг (0 = пауза, физика стоит)",
    "IDA 2.2081 (байтовое поле на объекте, сигнатуры не бывает); при апдейте — ручная перепроверка по декомпиляту",
    "gd",
};
inline constexpr Site kFieldResumeGraceCtr{
    "field.gjbasegamelayer.resume_grace_ctr",
    kFieldResumeGraceCtrOffset,
    nullptr,
    "офсет поля GJBaseGameLayer: int32 счётчик resume-grace (>=2 = grace-окно)",
    "IDA 2.2081 (поле на объекте, сигнатуры не бывает); при апдейте — ручная перепроверка по декомпиляту",
    "gd",
};

inline constexpr Site kUiLayerKeyDown{
    "uilayer.key_down",
    0x6711b0,
    "83 FA 1B 75 19 48 81 C1 C8 00 00 00 0F 28 DA 41 B0 01 BA 19 27 00 00 48 8B 01 48 FF 60 10 C3",
    "UILayer::keyDown: ветка Escape (cmp edx,0x1b; форвард 10009 через [this+0xc8])",
    "дизасемблер бинаря 2.2091 (tools/disasm_rva.py); краш BUG-105b",
    "gd",
};

inline constexpr Site kUiLayerHandleAction{
    "uilayer.handle_action",
    0x6713c0,
    "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC B0 00 00 00",
    "UILayer::handleAction: диспетчер keybind-действий 10001..10030 (прыжок, пауза-10009 и пр.)",
    "дизасемблер бинаря 2.2091; краш BUG-105b (чтение [this+0x100] при this=+0x220)",
    "gd",
};

inline constexpr Site kCocosDispatchToTopDelegate{
    "cocos.dispatch_to_top_delegate",
    0x463a0,
    "48 89 5C 24 20 57 41 56 41 57 48 83 EC 30 48 8B D9 49 8B F9 48 8B 49 38 45 0F B6 F0 44 8B FA",
    "CCKeyboardDispatcher::dispatchToTopDelegate: единственная точка, где cocos "
    "выбирает top CCKeyboardDelegate и зовёт его keyDown/handleAction (диагностика BUG-105b)",
    "дизасемблер libcocos2d.dll 2.2091 (tools/disasm_rva.py); crashlog-стек крашей Escape",
    "cocos",
};

inline constexpr const Site* kSites[] = {
    &kTpsExpectedTicks,
    &kStepGridStepCount,
    &kStepGridRestoreDt,
    &kRngTeleport,
    &kRngShake1,
    &kRngShake2,
    &kRngShake3,
    &kRngShake4,
    &kToggleGameVariable,
    &kRngPrimaryGlobal,
    &kRngTeleportGlobal,
    &kCocosStringInitWithFormat,
    &kFieldPauseFlag,
    &kFieldResumeGraceCtr,
    &kUiLayerKeyDown,
    &kUiLayerHandleAction,
    &kCocosDispatchToTopDelegate,
};


inline void parsePattern(std::string_view text, std::vector<uint8_t>& bytes,
                         std::vector<bool>& mask) {
    for (size_t pos = 0; pos < text.size();) {
        while (pos < text.size() && text[pos] == ' ') ++pos;
        if (pos >= text.size()) break;
        if (text[pos] == '?') {
            bytes.push_back(0);
            mask.push_back(false);
            while (pos < text.size() && text[pos] == '?') ++pos;
        } else {
            auto hexVal = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
                if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
                if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
                return 0;
            };
            char hi = text[pos++];
            char lo = text[pos++];
            bytes.push_back(static_cast<uint8_t>((hexVal(hi) << 4) | hexVal(lo)));
            mask.push_back(true);
        }
    }
}

inline bool matchesAt(const uint8_t* addr, std::string_view sig) {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    parsePattern(sig, bytes, mask);
    for (size_t j = 0; j < bytes.size(); ++j) {
        if (mask[j] && addr[j] != bytes[j]) return false;
    }
    return true;
}

inline size_t moduleSizeOf(uint8_t* base) {
#ifdef GEODE_IS_WINDOWS
    if (!base) return 0;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
#else
    (void)base;
    return 0;
#endif
}


inline std::vector<std::pair<size_t, size_t>> executableSections(uint8_t* base, size_t size) {
    std::vector<std::pair<size_t, size_t>> out;
#ifdef GEODE_IS_WINDOWS
    if (!base || size == 0) return out;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const size_t va = static_cast<size_t>(sec->VirtualAddress);
        const size_t vsz = static_cast<size_t>(sec->Misc.VirtualSize);
        if (va >= size) continue;
        out.emplace_back(va, std::min(vsz, size - va));
    }
#else
    (void)base; (void)size;
#endif
    return out;
}

inline bool verifyDataGlobals(const Site& site, uint8_t* base, size_t size) {
    if (!base || size == 0 || site.sig != nullptr) return false;
    static constexpr uint8_t kImm[4] = {0xFD, 0x43, 0x03, 0x00};
    const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(base);
    auto ranges = executableSections(base, size);
    if (ranges.empty()) ranges.emplace_back(0, size);
    for (auto [start, len] : ranges) {
        if (len < 11) continue;
        for (size_t i = start; i + 11 <= start + len; ++i) {
            if (std::memcmp(base + i, kImm, 4) != 0) continue;
            if (i < 7) continue;
            const uint8_t rex = base[i - 7];
            const uint8_t op = base[i - 6];
            const uint8_t modrm = base[i - 5];
            if (rex != 0x48 || op != 0x69 || (modrm & 0xC7) != 0x05) continue;
            int32_t disp = 0;
            std::memcpy(&disp, base + (i - 4), 4);
            const uintptr_t insnStart = baseAddr + (i - 7);
            const uintptr_t target = insnStart + 11 + static_cast<uintptr_t>(disp);
            if (target - baseAddr == site.rva) return true;
        }
    }
    return false;
}

inline std::optional<uintptr_t> resolve(const Site& site, uint8_t* base, size_t size) {
    if (!base || size == 0) return std::nullopt;
    if (site.sig == nullptr) return std::nullopt;

    if (site.rva != 0 && site.rva + 32 < size &&
        matchesAt(base + site.rva, site.sig)) {
        return std::optional<uintptr_t>(reinterpret_cast<uintptr_t>(base) + site.rva);
    }

    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    parsePattern(site.sig, bytes, mask);
    const size_t patLen = bytes.size();
    if (size < patLen) return std::nullopt;

    std::vector<std::pair<size_t, size_t>> ranges = executableSections(base, size);
    if (ranges.empty()) {
        skkbot2::log::warn(
            "offsets: '{}' — исполняемые секции не распознаны, скан по всему модулю",
            site.name);
        ranges.emplace_back(0, size);
    }

    std::optional<uintptr_t> best;
    size_t bestDist = ~size_t{0};
    int hits = 0;
    for (auto [start, len] : ranges) {
        if (len < patLen) continue;
        for (size_t i = start; i + patLen <= start + len; ++i) {
            bool hit = true;
            for (size_t j = 0; j < patLen; ++j) {
                if (mask[j] && base[i + j] != bytes[j]) { hit = false; break; }
            }
            if (!hit) continue;
            ++hits;
            const uintptr_t addr = reinterpret_cast<uintptr_t>(base) + i;
            const size_t dist = site.rva != 0
                ? static_cast<size_t>(i > site.rva ? i - site.rva : site.rva - i)
                : 0;
            if (dist < bestDist) {
                bestDist = dist;
                best = addr;
            }
        }
    }
    if (hits > 1) {
        skkbot2::log::warn(
            "offsets: '{}' — {} совпадений сигнатуры, выбрано ближайшее к RVA 0x{:X}",
            site.name, hits, site.rva);
    }
    return best;
}

inline std::optional<uintptr_t> rvaOf(const Site& site, uint8_t* base, size_t size) {
    auto addr = resolve(site, base, size);
    if (!addr) return std::nullopt;
    return *addr - reinterpret_cast<uintptr_t>(base);
}

inline int logReport() {
    auto* gdBase = reinterpret_cast<uint8_t*>(geode::base::get());
    auto* cocosBase = reinterpret_cast<uint8_t*>(geode::base::getCocos());
    const size_t gdSize = moduleSizeOf(gdBase);
    const size_t cocosSize = moduleSizeOf(cocosBase);
    if (!gdBase || gdSize == 0) {
        skkbot2::log::warn("offsets: модуль игры нечитаем, отчёт пропущен");
        return 0;
    }

    int problems = 0;
    skkbot2::log::info("offsets: проверка {} сайтов (GD build)",
                       static_cast<int>(sizeof(kSites) / sizeof(kSites[0])));
    for (const Site* site : kSites) {
        const bool isCocos = std::string_view(site->module) == "cocos";
        uint8_t* base = isCocos ? cocosBase : gdBase;
        size_t size = isCocos ? cocosSize : gdSize;
        if (isCocos && (!base || size == 0)) {
            skkbot2::log::warn("offsets: {:<26} cocos-модуль нечитаем, сайт пропущен",
                               site->name);
            continue;
        }
        if (site->sig == nullptr) {
            const bool isField = std::string_view(site->name).starts_with("field.");
            if (isField) {
                skkbot2::log::info(
                    "offsets: {:<26} field-offset {} (ручная перепроверка при апдейте)",
                    site->name, site->rva);
            } else if (verifyDataGlobals(*site, base, size)) {
                skkbot2::log::info(
                    "offsets: {:<26} OK data-глобал RVA 0x{:X} (подтверждён imul-сканом)",
                    site->name, site->rva);
            } else {
                skkbot2::log::error(
                    "offsets: {:<26} НЕ ПОДТВЕРЖДЁН: RVA 0x{:X} не найден среди целей "
                    "imul rax,[rip+d],0x343FD — глобал переехал",
                    site->name, site->rva);
                ++problems;
            }
            continue;
        }
        auto rva = rvaOf(*site, base, size);
        if (rva && *rva == site->rva) {
            skkbot2::log::info("offsets: {:<26} OK RVA 0x{:X} (байты совпали){}",
                               site->name, *rva, isCocos ? " [cocos]" : "");
        } else if (rva) {
            skkbot2::log::warn(
                "offsets: {:<26} ПЕРЕМЕЩЁН: сигнатура найдена на 0x{:X} (ожидался 0x{:X}){}",
                site->name, *rva, site->rva, isCocos ? " [cocos]" : "");
            ++problems;
        } else {
            skkbot2::log::error("offsets: {:<26} НЕ НАЙДЕН (сигнатура не совпала){}",
                                site->name, isCocos ? " [cocos]" : "");
            ++problems;
        }
    }
    return problems;
}

}
