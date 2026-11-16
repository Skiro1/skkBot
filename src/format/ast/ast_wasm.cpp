#include "format/ast/ast_wasm.hpp"

#include "logger/logger.hpp"
#include "wasm_export.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

// The converter module, embedded at build time from
// libs/wamr/obot_converter_bg.wasm.
extern "C" const unsigned char skk_ast_wasm[];
extern "C" const unsigned int skk_ast_wasm_len;

namespace skk::ast::wasm {
namespace {

// The converter is a Rust program compiled by wasm-bindgen. It talks to its
// host through 21 imports under the module name "wbg". None of them carry
// information we need - they exist because the bundle may poke at the DOM
// while it sets up - so all of them are stubs, with two exceptions that must
// be modelled honestly because the Rust side branches on them:
//
//   * object handles are i32 indices into a host-side array. Click wrappers
//     live there, plus a few sentinels, and is_undefined / instanceof_Window
//     compare against what we stored.
//   * __wbindgen_throw is a Rust panic. We must not trap the game over it, so
//     it is a no-op and any failure surfaces through the loader's own result.
enum Tag : uint8_t {
    TagUndefined = 0,
    TagWindow = 1,
    TagError = 2,
    TagClick = 3,
};

std::vector<uint32_t> g_handles;
std::vector<uint8_t> g_tags;

wasm_module_inst_t g_instance = nullptr;
wasm_exec_env_t g_env = nullptr;

uint32_t addHandle(uint8_t tag, uint32_t value) {
    g_handles.push_back(value);
    g_tags.push_back(tag);
    return static_cast<uint32_t>(g_handles.size() - 1);
}

void nop(wasm_exec_env_t, wasm_val_t*) {}
void cloneRef(wasm_exec_env_t, wasm_val_t* args) { args[0].of.i32 = args[0].of.i32; }
void getTime(wasm_exec_env_t, wasm_val_t* args) { args[0].of.f64 = 0.0; }

void clickNew(wasm_exec_env_t, wasm_val_t* args) {
    args[0].of.i32 = static_cast<int32_t>(addHandle(
        TagClick, static_cast<uint32_t>(args[0].of.i32)));
}
void makeUndefined(wasm_exec_env_t, wasm_val_t* args) {
    args[0].of.i32 = static_cast<int32_t>(addHandle(TagUndefined, 0));
}
void makeWindow(wasm_exec_env_t, wasm_val_t* args) {
    args[0].of.i32 = static_cast<int32_t>(addHandle(TagWindow, 0));
}
void makeError(wasm_exec_env_t, wasm_val_t* args) {
    args[0].of.i32 = static_cast<int32_t>(addHandle(TagError, 0));
}
void isUndefined(wasm_exec_env_t, wasm_val_t* args) {
    auto idx = static_cast<uint32_t>(args[0].of.i32);
    args[0].of.i32 = (idx < g_tags.size() && g_tags[idx] == TagUndefined) ? 1 : 0;
}
void isWindow(wasm_exec_env_t, wasm_val_t* args) {
    auto idx = static_cast<uint32_t>(args[0].of.i32);
    args[0].of.i32 = (idx < g_tags.size() && g_tags[idx] == TagWindow) ? 1 : 0;
}
void report(wasm_exec_env_t, wasm_val_t* args) {
    auto* mem = static_cast<uint8_t*>(wasm_runtime_addr_app_to_native(g_instance, 0));
    if (!mem || !args[0].of.i32 || !args[1].of.i32) return;
    skkbot2::log::info("[AST/wasm] {}", std::string(
        reinterpret_cast<char*>(mem + args[0].of.i32),
        static_cast<size_t>(args[1].of.i32)));
}

NativeSymbol g_symbols[] = {
    {"__wbindgen_object_drop_ref", nop, "(i)", nullptr},
    {"__wbg_click_new", clickNew, "(i)i", nullptr},
    {"__wbg_log_5d2f2656ef4f052f", report, "(ii)", nullptr},
    {"__wbg_error_2704ae79e4c75ea3", report, "(ii)", nullptr},
    {"__wbg_stack_658279fe44541cf6", nop, "(ii)", nullptr},
    {"__wbg_error_f851667af71bcfc6", report, "(ii)", nullptr},
    {"__wbg_newnoargs_e258087cd0daa0ea", makeError, "(ii)i", nullptr},
    {"__wbg_call_27c0f87801dedf93", makeError, "(ii)i", nullptr},
    {"__wbindgen_throw", nop, "(ii)", nullptr},
    {"__wbg_instanceof_Window_f401953a2cf86220", isWindow, "(i)i", nullptr},
    {"__wbg_document_5100775d18896c16", makeUndefined, "(i)i", nullptr},
    {"__wbg_body_edb1908d3ceff3a1", makeUndefined, "(i)i", nullptr},
    {"__wbg_new_abda76e883ba8a5f", makeError, "()i", nullptr},
    {"__wbg_new0_7d84e5b2cd9fdc73", makeError, "()i", nullptr},
    {"__wbg_self_ce0dbfc45cf2f5be", makeWindow, "()i", nullptr},
    {"__wbg_window_c6fb939a7f436783", makeWindow, "()i", nullptr},
    {"__wbg_globalThis_d1e6af4856ba331b", makeUndefined, "()i", nullptr},
    {"__wbg_global_207b558942527489", makeUndefined, "()i", nullptr},
    {"__wbg_getTime_2bc4375165f02d15", getTime, "(i)F", nullptr},
    {"__wbindgen_is_undefined", isUndefined, "(i)i", nullptr},
    {"__wbindgen_object_clone_ref", cloneRef, "(i)i", nullptr},
};

constexpr uint32_t kSymbolCount = sizeof(g_symbols) / sizeof(g_symbols[0]);
constexpr size_t kPoolSize = 16 * 1024 * 1024;

wasm_val_t i32(int32_t v) {
    wasm_val_t x;
    std::memset(&x, 0, sizeof(x));
    x.kind = WASM_I32;
    x.of.i32 = v;
    return x;
}

wasm_function_inst_t lookup(const char* name) {
    return wasm_runtime_lookup_function(g_instance, name);
}

bool startRuntime(std::string& reason) {
    RuntimeInitArgs init;
    std::memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_Pool;
    init.mem_alloc_option.pool.heap_buf = std::malloc(kPoolSize);
    init.mem_alloc_option.pool.heap_size = kPoolSize;
    if (!init.mem_alloc_option.pool.heap_buf) {
        reason = "не удалось выделить пул для wasm";
        return false;
    }
    if (!wasm_runtime_full_init(&init)) {
        reason = "wasm-рантайм не инициализировался";
        return false;
    }
    if (!wasm_runtime_register_natives("wbg", g_symbols, kSymbolCount)) {
        reason = "не удалось зарегистрировать импорты wbg";
        return false;
    }

    char err[256] = {0};
    // The embedded module lives in .rdata, and the loader is handed a mutable
    // pointer whether it needs one or not. Hand it a copy we own.
    static std::vector<uint8_t> moduleCopy;
    moduleCopy.assign(skk_ast_wasm, skk_ast_wasm + skk_ast_wasm_len);
    wasm_module_t mod =
        wasm_runtime_load(moduleCopy.data(), skk_ast_wasm_len, err, sizeof(err));
    if (!mod) {
        reason = std::string("wasm не загрузился: ") + err;
        return false;
    }
    g_instance = wasm_runtime_instantiate(mod, 64 * 1024, 64 * 1024, err, sizeof(err));
    if (!g_instance) {
        reason = std::string("wasm не инстанцировался: ") + err;
        return false;
    }
    g_env = wasm_runtime_create_exec_env(g_instance, 64 * 1024);
    if (!g_env) {
        reason = "не удалось создать окружение исполнения wasm";
        return false;
    }

    // wasm-bindgen's own start hook. It initialises the Rust allocator, and the
    // module is unusable - every allocation traps - until it has run. The web
    // glue calls it explicitly after instantiating, so we do too.
    if (!wasm_runtime_call_wasm_a(g_env, lookup("__wbindgen_start"), 0, nullptr, 0, nullptr)) {
        reason = "wasm не прошёл инициализацию";
        return false;
    }

    skkbot2::log::info("[AST/wasm] конвертер Astral загружен");
    return true;
}

bool ensureRuntime(std::string& reason) {
    if (g_instance && g_env) return true;
    static std::once_flag once;
    std::call_once(once, [&] { startRuntime(reason); });
    return g_instance != nullptr && g_env != nullptr;
}

}  // namespace

bool convert(std::span<const uint8_t> data, Output& out, std::string& outReason) {
    std::string reason;
    if (!ensureRuntime(reason)) {
        outReason = reason.empty() ? "wasm-рантайм не инициализирован" : reason;
        return false;
    }

    const auto n = static_cast<int32_t>(data.size());

    // The loader takes the bytes through the module's own allocator and returns
    // its result through a 16-byte slot on the wasm stack. Note that
    // __wbindgen_malloc takes TWO arguments - size and alignment - and that
    // converter_load is void: its outcome lands in that slot.
    wasm_val_t newStack;
    wasm_val_t reserveSlot = i32(-16);
    if (!wasm_runtime_call_wasm_a(g_env, lookup("__wbindgen_add_to_stack_pointer"), 1,
                                  &newStack, 1, &reserveSlot)) {
        outReason = "не удалось выделить слот результата в wasm";
        return false;
    }
    const auto retptr = static_cast<uint32_t>(newStack.of.i32);

    wasm_val_t mallocArgs[2] = {i32(n), i32(1)};
    wasm_val_t dataPtr;
    if (!wasm_runtime_call_wasm_a(g_env, lookup("__wbindgen_malloc"), 1, &dataPtr, 2,
                                  mallocArgs)) {
        outReason = "wasm не смог выделить память под файл";
        return false;
    }

    auto* mem = static_cast<uint8_t*>(wasm_runtime_addr_app_to_native(g_instance, 0));
    std::memcpy(mem + dataPtr.of.i32, data.data(), data.size());

    wasm_val_t converter;
    if (!wasm_runtime_call_wasm_a(g_env, lookup("converter_new"), 1, &converter, 0, nullptr)) {
        outReason = "не удалось создать конвертер";
        return false;
    }

    wasm_val_t loadArgs[5] = {
        i32(static_cast<int32_t>(retptr)), i32(converter.of.i32), i32(dataPtr.of.i32),
        i32(n), i32(kFormatAstV9),
    };
    if (!wasm_runtime_call_wasm_a(g_env, lookup("converter_load"), 0, nullptr, 5, loadArgs)) {
        outReason = "конвертер упал на разборе файла";
        return false;
    }

    // The slot holds (payload, isError). On error the payload is a handle to a
    // JS value we cannot read, so the only honest thing to say is that the
    // converter rejected the file.
    mem = static_cast<uint8_t*>(wasm_runtime_addr_app_to_native(g_instance, 0));
    const auto* slot = reinterpret_cast<const uint32_t*>(mem + retptr);
    const bool failed = slot[1] != 0;

    wasm_val_t count;
    const bool counted =
        wasm_runtime_call_wasm_a(g_env, lookup("converter_length"), 1, &count, 1, &converter);

    wasm_val_t tps;
    std::memset(&tps, 0, sizeof(tps));
    tps.kind = WASM_I64;
    // tps comes back as f32, not f64. Read as a double, 240 becomes 5.59e-315,
    // which is small enough to look like a real measurement.
    wasm_runtime_call_wasm_a(g_env, lookup("converter_get_fps"), 1, &tps, 1, &converter);

    if (!failed && counted) {
        out.tps = static_cast<double>(tps.of.f32);
        out.clicks.clear();
        out.clicks.reserve(static_cast<size_t>(count.of.i32));
        for (int32_t i = 0; i < count.of.i32; ++i) {
            wasm_val_t clickArgs[2] = {i32(converter.of.i32), i32(i)};
            wasm_val_t click;
            if (!wasm_runtime_call_wasm_a(g_env, lookup("converter_click_at"), 1, &click, 2,
                                          clickArgs)) {
                outReason = "не удалось прочитать действие из конвертера";
                return false;
            }
            wasm_val_t frame, p1, p2;
            wasm_runtime_call_wasm_a(g_env, lookup("__wbg_get_click_frame"), 1, &frame, 1, &click);
            wasm_runtime_call_wasm_a(g_env, lookup("__wbg_get_click_p1"), 1, &p1, 1, &click);
            wasm_runtime_call_wasm_a(g_env, lookup("__wbg_get_click_p2"), 1, &p2, 1, &click);
            out.clicks.push_back(Click{static_cast<uint32_t>(frame.of.i32),
                                       static_cast<Kind>(p1.of.i32),
                                       static_cast<Kind>(p2.of.i32)});
        }
    }

    // Hand the 16 bytes back whatever happened above.
    wasm_val_t giveBack = i32(16);
    wasm_runtime_call_wasm_a(g_env, lookup("__wbindgen_add_to_stack_pointer"), 1, &newStack, 1,
                             &giveBack);

    if (failed) {
        outReason = "конвертер не смог разобрать файл";
        return false;
    }
    if (!counted) {
        outReason = "конвертер не отдал число действий";
        return false;
    }
    return true;
}

bool saveAs(std::span<const uint8_t> data, uint32_t formatId,
            std::vector<uint8_t>& out, std::string& outReason) {
    out.clear();
    std::string reason;
    if (!ensureRuntime(reason)) {
        outReason = reason.empty() ? "wasm-рантайм не инициализирован" : reason;
        return false;
    }

    const auto n = static_cast<int32_t>(data.size());

    wasm_val_t newStack;
    wasm_val_t reserveSlot = i32(-16);
    if (!wasm_runtime_call_wasm_a(g_env, lookup("__wbindgen_add_to_stack_pointer"), 1,
                                  &newStack, 1, &reserveSlot)) {
        outReason = "не удалось выделить слот под загрузку";
        return false;
    }
    const auto loadSlot = static_cast<uint32_t>(newStack.of.i32);

    wasm_val_t mallocArgs[2] = {i32(n), i32(1)};
    wasm_val_t dataPtr;
    if (!wasm_runtime_call_wasm_a(g_env, lookup("__wbindgen_malloc"), 1, &dataPtr, 2,
                                  mallocArgs)) {
        outReason = "wasm не смог выделить память под файл";
        return false;
    }
    auto* mem = static_cast<uint8_t*>(wasm_runtime_addr_app_to_native(g_instance, 0));
    std::memcpy(mem + dataPtr.of.i32, data.data(), data.size());

    wasm_val_t converter;
    if (!wasm_runtime_call_wasm_a(g_env, lookup("converter_new"), 1, &converter, 0, nullptr)) {
        outReason = "не удалось создать конвертер";
        return false;
    }

    wasm_val_t loadArgs[5] = {
        i32(static_cast<int32_t>(loadSlot)), i32(converter.of.i32), i32(dataPtr.of.i32),
        i32(n), i32(kFormatAstV9),
    };
    if (!wasm_runtime_call_wasm_a(g_env, lookup("converter_load"), 0, nullptr, 5, loadArgs)) {
        outReason = "конвертер упал на разборе файла";
        return false;
    }
    mem = static_cast<uint8_t*>(wasm_runtime_addr_app_to_native(g_instance, 0));
    if (reinterpret_cast<const uint32_t*>(mem + loadSlot)[1] != 0) {
        outReason = "конвертер отверг файл";
        return false;
    }

    // A second 16-byte slot, this time for (ptr, len).
    if (!wasm_runtime_call_wasm_a(g_env, lookup("__wbindgen_add_to_stack_pointer"), 1,
                                  &newStack, 1, &reserveSlot)) {
        outReason = "не удалось выделить слот под результат";
        return false;
    }
    const auto saveSlot = static_cast<uint32_t>(newStack.of.i32);

    wasm_val_t saveArgs[3] = {
        i32(static_cast<int32_t>(saveSlot)), i32(converter.of.i32), i32(static_cast<int32_t>(formatId)),
    };
    if (!wasm_runtime_call_wasm_a(g_env, lookup("converter_save"), 0, nullptr, 3, saveArgs)) {
        outReason = "converter_save не вызвался";
        return false;
    }

    mem = static_cast<uint8_t*>(wasm_runtime_addr_app_to_native(g_instance, 0));
    uint32_t ptr = 0, len = 0;
    std::memcpy(&ptr, mem + saveSlot, 4);
    std::memcpy(&len, mem + saveSlot + 4, 4);
    if (ptr == 0 || len == 0 || len > 64u * 1024u * 1024u) {
        outReason = "пустой или неправдоподобный результат";
        return false;
    }
    out.resize(len);
    // Same base pointer the loader used, so the copy is a plain memcpy rather
    // than a runtime call that may not exist in this WAMR build.
    mem = static_cast<uint8_t*>(wasm_runtime_addr_app_to_native(g_instance, 0));
    std::memcpy(out.data(), mem + ptr, len);
    return true;
}

}  // namespace skk::ast::wasm
