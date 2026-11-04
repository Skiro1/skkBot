cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(skkBot2 VERSION 0.1.0 LANGUAGES CXX C)

include(CTest)

# skkBot2 M1 sources: record/play engine (SKK3) + physics bypass/TPS + native
# imGui GUI. Everything else from the original skkBot tree was pruned
# per the skkBot2 spec; pruned modules that the engine still references are
# provided as no-op stubs (see src/{lang,render,hacks}/...).
set(SKKBOT2_SOURCES
    src/main.cpp
    src/gui/skkbot_gui.cpp
    src/gui/beta_gui.cpp
    src/gui/blur.cpp
    src/format/skk_format.cpp
    src/format/skk3_format.cpp
    src/format/codec.cpp
    src/format/gdr2/gdr2.cpp
    src/format/slc/slc.cpp
    src/format/GBR/gbr.cpp
    src/format/nhm/nhm.cpp
    src/format/xe/xe.cpp
    src/format/re4/re4.cpp
    src/format/ttrl/ttrl.cpp
    src/format/echo/echo.cpp
    src/format/ast/ast.cpp
    src/core/checkpoint_handler.cpp
    src/core/replay_engine.cpp
    src/core/step_trace.cpp
    src/core/checkpoint_system.cpp
    src/core/editor_playback.cpp
    src/core/full_player_checkpoint.cpp
    src/core/practice_fix_bridge.cpp
    src/core/gd_option_tracer.cpp
    src/core/frame_stepper.cpp
    src/core/playback_fps.cpp
    src/core/keybinds.cpp
    src/core/autosave_recorder.cpp
    src/core/audio_stub.cpp
    src/core/rng_lock.cpp
    src/hooks/rng_hooks.cpp
    src/hacks/physicsbypass.cpp
    src/hacks/render_hooks.cpp
    src/hacks/wave_trail_fix.cpp
    src/hacks/hitboxes.cpp
    src/hacks/practice_music_sync.cpp
    src/hacks/music_speed.cpp
    src/hacks/layout_mode.cpp
    src/hacks/safe_mode.cpp
    src/trajectory/show_trajectory.cpp
    src/physics/step_grid_hooks.cpp
    src/physics/near_movers.cpp
    src/physics/cocos_string_opt.cpp
    src/physics/particle_spam_opt.cpp
    src/physics/phys_ring_jump.cpp
    src/logger/logger.cpp
    src/skc/skc_bot.cpp
    src/skc/skc_checkpoint.cpp
    src/skc/skc_format.cpp
    src/skc/skc_frame.cpp
    src/skc/skc_hooks.cpp
    src/skc/skc_dash_determinism.cpp
    src/skc/skc_range.cpp
)

if (WIN32)
    # Video renderer (ported from skkBot): FBO capture + FFmpeg runtime
    # (DLLs loaded dynamically from resources/libraries) + FMOD audio capture.
    list(APPEND SKKBOT2_SOURCES
        src/render/capture.cpp
        src/render/hwprobe.cpp
        src/render/renderer.cpp
        src/render/audiocap.cpp
        src/render/effect_batch.cpp
        src/render/effect_copies.cpp
    )
endif()

# --- Astral .ast decoder (WAMR) ------------------------------------------------
# The .ast container is a zstd bit stream whose record layout we only partly
# recovered; every native guess broke on some macro. So we run the official
# converter, a Rust program compiled to WebAssembly, and use only its AST path.
#
# WAMR (wasm-micro-runtime) is the embedded interpreter. wasm3 was tried first
# and rejected: v0.5.0 traps inside its allocator on this module, v0.9.0 returns
# garbage from m3_GetMemorySize. WAMR runs the module clean on the first try.
set(WAMR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/wamr)
set(WAMR_AST_WASM ${WAMR_DIR}/obot_converter_bg.wasm)

if (WIN32)
    set(WAMR_SOURCES
        ${WAMR_DIR}/iwasm/common/wasm_application.c
        ${WAMR_DIR}/iwasm/common/wasm_blocking_op.c
        ${WAMR_DIR}/iwasm/common/wasm_c_api.c
        ${WAMR_DIR}/iwasm/common/wasm_exec_env.c
        ${WAMR_DIR}/iwasm/common/wasm_memory.c
        ${WAMR_DIR}/iwasm/common/wasm_native.c
        ${WAMR_DIR}/iwasm/common/wasm_runtime_common.c
        ${WAMR_DIR}/iwasm/common/wasm_shared_memory.c
        ${WAMR_DIR}/iwasm/common/arch/invokeNative_general.c
        ${WAMR_DIR}/iwasm/interpreter/wasm_interp_classic.c
        ${WAMR_DIR}/iwasm/interpreter/wasm_loader.c
        ${WAMR_DIR}/iwasm/interpreter/wasm_runtime.c
        ${WAMR_DIR}/shared/mem-alloc/mem_alloc.c
        ${WAMR_DIR}/shared/mem-alloc/ems/ems_alloc.c
        ${WAMR_DIR}/shared/mem-alloc/ems/ems_gc.c
        ${WAMR_DIR}/shared/mem-alloc/ems/ems_hmu.c
        ${WAMR_DIR}/shared/mem-alloc/ems/ems_kfc.c
        ${WAMR_DIR}/shared/platform/windows/platform_init.c
        ${WAMR_DIR}/shared/platform/windows/win_atomic.cpp
        ${WAMR_DIR}/shared/platform/windows/win_clock.c
        ${WAMR_DIR}/shared/platform/windows/win_file.c
        ${WAMR_DIR}/shared/platform/windows/win_malloc.c
        ${WAMR_DIR}/shared/platform/windows/win_memmap.c
        ${WAMR_DIR}/shared/platform/windows/win_socket.c
        ${WAMR_DIR}/shared/platform/windows/win_thread.c
        ${WAMR_DIR}/shared/platform/windows/win_time.c
        ${WAMR_DIR}/shared/platform/windows/win_util.c
    )
    file(GLOB WAMR_UTILS_SOURCES "${WAMR_DIR}/shared/utils/*.c")
    list(APPEND WAMR_SOURCES ${WAMR_UTILS_SOURCES})

    add_library(wamr STATIC ${WAMR_SOURCES})
    target_include_directories(wamr PUBLIC
        ${WAMR_DIR}
        ${WAMR_DIR}/iwasm/include
        ${WAMR_DIR}/iwasm/common
        ${WAMR_DIR}/iwasm/interpreter
        ${WAMR_DIR}/shared/utils
        ${WAMR_DIR}/shared/mem-alloc
        ${WAMR_DIR}/shared/platform/include
        ${WAMR_DIR}/shared/platform/windows
        ${WAMR_DIR}/shared/platform/common
        ${WAMR_DIR}/shared/platform/common/libc-util
    )
    # MSVC leaves __x86_64__ undefined and WAMR refuses to guess its target.
    # The module needs bulk memory (719 memory.copy, 554 memory.fill) and the
    # sign-extension operators, so both are mandatory.
    target_compile_definitions(wamr PUBLIC
        __x86_64__
        # Both WAMR headers assume a Windows DLL build and stamp every API with
        # __declspec(dllimport) unless their own "compiling" macro is set. We
        # link the interpreter statically, and both guards are plain #ifndef,
        # so the clean way to say "static" is to hand them empty definitions.
        WASM_RUNTIME_API_EXTERN=
        WASM_API_EXTERN=
        BH_MALLOC=wasm_runtime_malloc
        BH_FREE=wasm_runtime_free
        WASM_ENABLE_INTERP=1
        WASM_ENABLE_FAST_INTERP=0
        WASM_ENABLE_BULK_MEMORY=1
        WASM_ENABLE_SIGN_EXTENSION=1
        WASM_ENABLE_REF_TYPES=0
        WASM_ENABLE_LITTLE_ENDIAN=1
        WASM_ENABLE_THREAD_MGR=0
        WASM_ENABLE_LINUX_PERF=0
        WASM_ENABLE_GC=0
        WASM_ENABLE_MULTI_MEMORY=0
        WASM_ENABLE_MEMORY_PROFILING=0
    )
    if (MSVC)
        target_compile_options(wamr PRIVATE /W0)
    else()
        target_compile_options(wamr PRIVATE -w)
    endif()

    # os_mremap is a POSIX primitive; WAMR's Windows platform never defines it.
    # The AST path uses no shared memory, so a stub is the honest answer.
    set(WAMR_STUB ${CMAKE_CURRENT_SOURCE_DIR}/libs/wamr/os_mremap_stub.c)
    target_sources(wamr PRIVATE ${WAMR_STUB})

    # Embed the converter module rather than shipping a 1.9 MB sidecar, so the
    # .geode stays self-contained. MSVC compiles the byte array in ~4 s.
    file(READ ${WAMR_AST_WASM} WAMR_AST_HEX HEX)
    string(REGEX REPLACE "(..)" "0x\\1," WAMR_AST_BYTES "${WAMR_AST_HEX}")
    set(WAMR_BLOB ${CMAKE_CURRENT_BINARY_DIR}/generated/ast_wasm_blob.cpp)
    file(WRITE ${WAMR_BLOB}
        "extern \"C\" const unsigned char skk_ast_wasm[] = {${WAMR_AST_BYTES}0x00};\n"
        "extern \"C\" const unsigned int skk_ast_wasm_len = sizeof(skk_ast_wasm) - 1;\n")

    list(APPEND SKKBOT2_SOURCES
        src/format/ast/ast_wasm.cpp
        ${WAMR_BLOB}
    )
    set(SKKBOT2_WAMR wamr)
else()
    # The WAMR sources we vendor are the Windows platform port only.
    list(APPEND SKKBOT2_SOURCES src/format/ast/ast_wasm_stub.cpp)
    set(SKKBOT2_WAMR "")
endif()

add_library(${PROJECT_NAME} SHARED ${SKKBOT2_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    libs/ffmpeg-headers
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

# Bindings codegen: build it from the local bindings clone, never download it.
#
# The bindings CMakeLists downloads the newest prebuilt Codegen.exe from
# geode-sdk/bindings releases whenever the cached copy in the build tree is more
# than a day old. That binary has moved on to emitting
#     #include <Geode/mapping/<base>.hpp>
# for every generated class, and those headers only exist in SDKs newer than the
# 5.11.1 this mod targets. The result is not a subtle error: every cocos binding
# fails with C1083 (mapping/CCSprite.hpp and 600 more), the bindings project does
# not compile, and the mod stops building entirely. Deleting build/ is enough to
# trigger it, because the cached binary goes with it.
#
# The codegen SOURCE in the local clone (codegen/src) predates mapping headers
# and generates exactly the bindings the SDK expects, so building it locally is
# both correct and reproducible. Cached and FORCEd so the bindings subproject
# sees it: its own check is `if (NOT DEFINED SKIP_BUILDING_CODEGEN)`, and a cache
# variable is defined from the first configure on.
set(SKIP_BUILDING_CODEGEN OFF CACHE BOOL
    "Build bindings codegen from source instead of downloading the latest release" FORCE)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Vendored Dear ImGui + matcool's gd-imgui-cocos host (same wiring as skkBot).
add_subdirectory(libs/imgui)

CPMAddPackage(
    NAME GDReplayFormat
    GITHUB_REPOSITORY maxnut/GDReplayFormat
    GIT_TAG 77bd505853541a87a12895dc23de05f6585d0515
    DOWNLOAD_ONLY YES
)
add_library(libGDR INTERFACE)
target_include_directories(libGDR INTERFACE "${GDReplayFormat_SOURCE_DIR}/include")

# Provide our own vendored Dear ImGui (libs/imgui, currently 1.93.0) so the
# gd-imgui-cocos host compiles against the same single imgui copy as the GUI
# code instead of pulling its own version via HAS_IMGUI=OFF.
set(HAS_IMGUI ON CACHE BOOL "Use vendored imgui from libs/imgui" FORCE)

CPMAddPackage(
    NAME gd-imgui-cocos
    GITHUB_REPOSITORY matcool/gd-imgui-cocos
    GIT_TAG b93f08ccef778a53ebba09b20c347f6a63980119
)
target_include_directories(${PROJECT_NAME} PRIVATE ${gd-imgui-cocos_SOURCE_DIR}/src)

# zstd: needed by SKK3 payload compression.
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.7
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(zstd)
if(NOT zstd_POPULATED)
    FetchContent_Populate(zstd)
endif()

# Build zstd static from lib/
set(ZSTD_SRC "${zstd_SOURCE_DIR}/lib")
file(GLOB ZSTD_LIB_SOURCES
    "${ZSTD_SRC}/common/*.c"
    "${ZSTD_SRC}/compress/*.c"
    "${ZSTD_SRC}/decompress/*.c"
)
add_library(zstd_static STATIC ${ZSTD_LIB_SOURCES})
target_include_directories(zstd_static PUBLIC "${ZSTD_SRC}")
target_compile_definitions(zstd_static PRIVATE ZSTD_STATIC_LINKING_ONLY=1)
if(MSVC)
    target_compile_options(zstd_static PRIVATE /W0)
else()
    target_compile_options(zstd_static PRIVATE -w)
endif()

set(SKKBOT2_LINK_LIBRARIES imgui-cocos imgui libGDR zstd_static)

target_compile_definitions(${PROJECT_NAME} PRIVATE MOD_VERSION="${CMAKE_PROJECT_VERSION}")

if (WIN32)
    CPMAddPackage("gh:cursey/safetyhook#2f28386")
    target_link_libraries(${PROJECT_NAME} ${SKKBOT2_LINK_LIBRARIES} Crypt32 Advapi32 dwmapi safetyhook ${SKKBOT2_WAMR})

    # Always emit a PDB for the DLL. Geode's crash reports only carry raw
    # offsets (skiro1.skk-bot.dll + 0x...), which are unresolvable without a
    # matching symbol file Р Р†Р вЂљРІР‚Сњ a null-vtable crash inside the game loop produced a
    # trace nobody could attribute. The PDB is written next to the binary; put
    # it in the .geode zip so the loader can find it next to the DLL.
    target_compile_options(${PROJECT_NAME} PRIVATE /Zi /FS)
    target_link_options(${PROJECT_NAME} PRIVATE /DEBUG)
    set_property(TARGET ${PROJECT_NAME}
                 PROPERTY PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Release")
elseif (APPLE)
    target_link_libraries(${PROJECT_NAME} ${SKKBOT2_LINK_LIBRARIES} "-framework IOKit")
endif()

setup_geode_mod(${PROJECT_NAME})
