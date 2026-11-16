#pragma once

#include "logger/logger.hpp"
#include "../utils.hpp"

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#endif

namespace skkbot2 {
namespace render {

#ifdef GEODE_IS_WINDOWS
#define FFMPEG_FN(ident) decltype(ident)* ident

struct ff_t {
    FFMPEG_FN(avformat_alloc_output_context2);
    FFMPEG_FN(avio_open);
    FFMPEG_FN(avio_close);
    FFMPEG_FN(avformat_new_stream);
    FFMPEG_FN(avcodec_parameters_from_context);
    FFMPEG_FN(avformat_write_header);
    FFMPEG_FN(av_write_trailer);
    FFMPEG_FN(avformat_free_context);

    FFMPEG_FN(avcodec_find_encoder_by_name);
    FFMPEG_FN(avcodec_alloc_context3);
    FFMPEG_FN(avcodec_open2);
    FFMPEG_FN(avcodec_send_frame);
    FFMPEG_FN(avcodec_receive_packet);
    FFMPEG_FN(avcodec_free_context);
    FFMPEG_FN(av_codec_iterate);
    FFMPEG_FN(av_codec_is_encoder);

    FFMPEG_FN(av_dict_set);
    FFMPEG_FN(av_dict_free);

    FFMPEG_FN(av_frame_alloc);
    FFMPEG_FN(av_frame_free);
    FFMPEG_FN(av_frame_get_buffer);
    FFMPEG_FN(av_frame_make_writable);
    FFMPEG_FN(av_frame_unref);

    FFMPEG_FN(av_packet_alloc);
    FFMPEG_FN(av_packet_free);
    FFMPEG_FN(av_packet_unref);
    FFMPEG_FN(av_packet_rescale_ts);
    FFMPEG_FN(av_interleaved_write_frame);
    FFMPEG_FN(av_write_frame);

    FFMPEG_FN(av_strerror);

    FFMPEG_FN(sws_getContext);
    FFMPEG_FN(sws_scale);
    FFMPEG_FN(sws_freeContext);

    FFMPEG_FN(av_log_set_callback);
    FFMPEG_FN(av_log_set_level);
};

#undef FFMPEG_FN

static const char* const FFMPEG_DLLS[] = {
    "avutil-61.dll",
    "swresample-7.dll",
    "swscale-10.dll",
    "avcodec-63.dll",
    "avformat-63.dll",
    "avfilter-12.dll",
    "avdevice-63.dll",
};

static const char* const FFMPEG_SYMBOLS[] = {
    "avformat_alloc_output_context2",
    "avio_open",
    "avio_close",
    "avformat_new_stream",
    "avcodec_parameters_from_context",
    "avformat_write_header",
    "av_write_trailer",
    "avformat_free_context",
    "avcodec_find_encoder_by_name",
    "avcodec_alloc_context3",
    "avcodec_open2",
    "avcodec_send_frame",
    "avcodec_receive_packet",
    "avcodec_free_context",
    "av_codec_iterate",
    "av_codec_is_encoder",
    "av_dict_set",
    "av_dict_free",
    "av_frame_alloc",
    "av_frame_free",
    "av_frame_get_buffer",
    "av_frame_make_writable",
    "av_frame_unref",
    "av_packet_alloc",
    "av_packet_free",
    "av_packet_unref",
    "av_packet_rescale_ts",
    "av_interleaved_write_frame",
    "av_write_frame",
    "av_strerror",
    "sws_getContext",
    "sws_scale",
    "sws_freeContext",
    "av_log_set_callback",
    "av_log_set_level",
};

inline std::string g_lastFfLogMsg;
inline void clearLastFfLogMsg() { g_lastFfLogMsg.clear(); }
inline const std::string& getLastFfLogMsg() { return g_lastFfLogMsg; }

inline void ffLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_INFO) return;
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, vl);
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) return;
    if (level <= AV_LOG_ERROR) {
        g_lastFfLogMsg = line;
        skkbot2::log::error("[FFMPEG] {}", line);
    } else if (level <= AV_LOG_WARNING) {
        skkbot2::log::warn("[FFMPEG] {}", line);
    } else {
        skkbot2::log::info("[FFMPEG] {}", line);
    }
}

inline std::string ffErrString(ff_t* table, int code) {
    if (!table || !table->av_strerror) return "av-error " + std::to_string(code);
    char buf[128] = {0};
    table->av_strerror(code, buf, sizeof(buf));
    return buf;
}

inline void ffRegisterDllDir(std::filesystem::path const& dir) {
    static std::vector<std::wstring> registered;
    const std::wstring wide = dir.wstring();
    for (auto const& seen : registered) {
        if (seen == wide) return;
    }
    registered.push_back(wide);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                             LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(wide.c_str());
}

inline bool ffLoadFunctions(ff_t* table, std::filesystem::path const& dir,
                            std::string& errorMsg) {
    ffRegisterDllDir(dir);

    std::vector<HMODULE> mods;
    std::string firstError;
    for (const char* dll : FFMPEG_DLLS) {
        std::error_code ec;
        const std::filesystem::path full = dir / dll;
        if (!std::filesystem::exists(full, ec)) {
            if (firstError.empty()) {
                firstError = std::string(dll) + " is missing";
            }
            continue;
        }
        HMODULE mod = LoadLibraryExW(
            full.wstring().c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (mod) {
            mods.push_back(mod);
        } else if (firstError.empty()) {
            firstError = std::string(dll) + " failed (error " +
                         std::to_string(GetLastError()) + ")";
        }
    }
    if (mods.empty()) {
        errorMsg = "no FFmpeg DLLs in " + skk::pathToUtf8(dir);
        if (!firstError.empty()) errorMsg += " (" + firstError + ")";
        return false;
    }

    auto** fnSlot = reinterpret_cast<void**>(table);
    size_t i = 0;
    for (const char* name : FFMPEG_SYMBOLS) {
        fnSlot[i] = nullptr;
        for (HMODULE mod : mods) {
            void* p = reinterpret_cast<void*>(GetProcAddress(mod, name));
            if (p) {
                fnSlot[i] = p;
                break;
            }
        }
        if (!fnSlot[i]) {
            errorMsg = std::string("FFmpeg symbol not found: ") + name;
            return false;
        }
        i++;
    }
    if (table->av_log_set_callback) {
        table->av_log_set_callback(ffLogCallback);
    }
    if (table->av_log_set_level) {
        table->av_log_set_level(AV_LOG_INFO);
    }
    return true;
}

static_assert(sizeof(ff_t) >= sizeof(void*) * 32);

#else  // !GEODE_IS_WINDOWS — the renderer is Windows-only (DLL loading).
struct ff_t {};
inline std::string ffErrString(ff_t*, int code) {
    return "av-error " + std::to_string(code);
}
inline bool ffLoadFunctions(ff_t*, std::filesystem::path const&,
                           std::string& errorMsg) {
    errorMsg = "renderer is not available on this platform";
    return false;
}
#endif

}
}