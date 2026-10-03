cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(skkBot2 VERSION 0.1.0 LANGUAGES CXX)

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
    src/core/checkpoint_handler.cpp
    src/core/replay_engine.cpp
    src/core/checkpoint_system.cpp
    src/core/editor_playback.cpp
    src/core/full_player_checkpoint.cpp
    src/core/practice_fix_bridge.cpp
    src/core/frame_stepper.cpp
    src/core/keybinds.cpp
    src/core/audio_stub.cpp
    src/core/rng_lock.cpp
    src/hooks/rng_hooks.cpp
    src/hacks/physicsbypass.cpp
    src/hacks/render_hooks.cpp
    src/hacks/wave_trail_fix.cpp
    src/hacks/hitboxes.cpp
    src/hacks/trajectory.cpp
    src/physics/step_grid_hooks.cpp
    src/logger/logger.cpp
)

if (WIN32)
    # Video renderer (ported from skkBot): FBO capture + FFmpeg runtime
    # (DLLs loaded dynamically from resources/libraries) + FMOD audio capture.
    list(APPEND SKKBOT2_SOURCES
        src/render/capture.cpp
        src/render/renderer.cpp
        src/render/audiocap.cpp
    )
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
    target_link_libraries(${PROJECT_NAME} ${SKKBOT2_LINK_LIBRARIES} Crypt32 Advapi32 safetyhook)
elseif (APPLE)
    target_link_libraries(${PROJECT_NAME} ${SKKBOT2_LINK_LIBRARIES} "-framework IOKit")
endif()

setup_geode_mod(${PROJECT_NAME})
