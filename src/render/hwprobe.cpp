#include "hwprobe.hpp"
#include "../utils.hpp"

#ifdef GEODE_IS_WINDOWS

#include <windows.h>

#include <cstdio>
#include <ctime>

#include <libavcodec/codec.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>

namespace skkbot2 {
namespace render {

namespace {

// Resolved by hand rather than through ff_t. ffLoadFunctions treats every
// symbol it lists as REQUIRED and hard-fails if one is missing, which is right
// for the render pipeline and exactly wrong here: a FFmpeg build without
// AVHWFrames must still render, just without zero-copy. So this loads its own
// copies of the two DLLs and tolerates every symbol being absent.
//
// Signatures follow the headers we actually ship (FFmpeg 7.x, avutil-61),
// which differ from older examples floating around: ctx_create takes five
// arguments and an AVHWDeviceType ENUM (not a type-name string), there is no
// av_hwdevice_ctx_destroy at all (an AVBufferRef is released with
// av_buffer_unref), and the type list is walked with
// av_hwdevice_iterate_types rather than guessed.
struct hw_fns {
    decltype(av_hwdevice_ctx_create)* ctx_create = nullptr;
    decltype(av_hwdevice_iterate_types)* iterate_types = nullptr;
    decltype(av_hwdevice_get_type_name)* type_name = nullptr;
    decltype(avcodec_get_hw_config)* get_hw_config = nullptr;
    decltype(avcodec_find_encoder_by_name)* find_encoder = nullptr;
    decltype(av_buffer_unref)* buffer_unref = nullptr;
    decltype(av_strerror)* strerror = nullptr;
};

bool loadHwFns(const std::filesystem::path& dir, hw_fns& out, bool& dllsOk) {
    auto openDll = [&](const char* name) -> HMODULE {
        std::filesystem::path p = dir / name;
        return LoadLibraryW(p.wstring().c_str());
    };
    // avutil ships the hw-device API, avcodec the encoder and the hw-config
    // query. Either one missing is enough to disqualify the path.
    HMODULE avutil = openDll("avutil-61.dll");
    HMODULE avcodec = openDll("avcodec-63.dll");
    dllsOk = (avutil != nullptr && avcodec != nullptr);
    if (!dllsOk) {
        return false;
    }
    out.ctx_create = reinterpret_cast<decltype(out.ctx_create)>(
        GetProcAddress(avutil, "av_hwdevice_ctx_create"));
    out.iterate_types = reinterpret_cast<decltype(out.iterate_types)>(
        GetProcAddress(avutil, "av_hwdevice_iterate_types"));
    out.type_name = reinterpret_cast<decltype(out.type_name)>(
        GetProcAddress(avutil, "av_hwdevice_get_type_name"));
    out.buffer_unref = reinterpret_cast<decltype(out.buffer_unref)>(
        GetProcAddress(avutil, "av_buffer_unref"));
    // Nothing in this file may reference an FFmpeg symbol directly: the whole
    // stack is loaded at runtime and there is no import lib, so a direct call
    // is an unresolved external at link time rather than a runtime failure.
    out.strerror = reinterpret_cast<decltype(out.strerror)>(
        GetProcAddress(avutil, "av_strerror"));
    out.get_hw_config = reinterpret_cast<decltype(out.get_hw_config)>(
        GetProcAddress(avcodec, "avcodec_get_hw_config"));
    out.find_encoder = reinterpret_cast<decltype(out.find_encoder)>(
        GetProcAddress(avcodec, "avcodec_find_encoder_by_name"));

    // Deliberately does NOT FreeLibrary: the handles stay alive for the
    // process, matching what the real loader does with the same DLLs. Freeing
    // them here would unload avutil underneath the running renderer.
    return out.ctx_create && out.iterate_types && out.type_name &&
           out.buffer_unref && out.get_hw_config && out.find_encoder &&
           out.strerror;
}

std::string avErr(const hw_fns& fns, int code) {
    char buf[160] = {0};
    fns.strerror(code, buf, sizeof(buf));
    return buf;
}

// Probes one hwdevice type. Creates and immediately unrefs the context: the
// question is "can this driver give us a device at all", and a leaked
// AVBufferRef per render would be a slow leak in a mod that may be left
// rendering for hours.
bool tryDevice(const hw_fns& fns, AVHWDeviceType type, std::string& err) {
    AVBufferRef* dev = nullptr;
    const int rc = fns.ctx_create(&dev, type, nullptr, nullptr, 0);
    if (rc < 0) {
        err = fns.strerror ? avErr(fns, rc) : ("error " + std::to_string(rc));
        return false;
    }
    if (dev) {
        fns.buffer_unref(&dev);
    }
    return true;
}

HwProbe g_probe;
bool g_probed = false;

}  // namespace

HwProbe probeHwFrames(const std::filesystem::path& ffmpegDir) {
    if (g_probed) {
        return g_probe;
    }
    g_probed = true;
    HwProbe& p = g_probe;

    hw_fns fns;
    loadHwFns(ffmpegDir, fns, p.ffmpegDlls);
    p.hwApi = (fns.ctx_create && fns.iterate_types && fns.type_name &&
               fns.buffer_unref && fns.get_hw_config && fns.find_encoder);
    if (!p.hwApi) {
        // Distinguish "no DLLs" from "DLLs but no hw API" - they mean very
        // different things for a future zero-copy attempt.
        if (!p.ffmpegDlls) {
            p.cudaErr = "avutil/avcodec not loadable from " + skk::pathToUtf8(ffmpegDir);
        } else {
            p.cudaErr = "FFmpeg built without the AVHWFrames API";
        }
        return p;
    }

    // 1. Driver-side CUDA. LoadLibrary only, no calls: we are asking whether
    // the entry point exists, not whether we can use it. cuda is genuinely not
    // universal (Intel/AMD machines, some driver installs), and that is the
    // entire reason any future toggle must default OFF.
    {
        HMODULE nv = LoadLibraryW(L"nvcuda.dll");
        p.cudaDriver = (nv != nullptr);
        if (nv) {
            FreeLibrary(nv);
        }
    }

    // 2. Which hwdevice types does this FFmpeg build even know about? Asked via
    // iterate_types rather than assumed, because a build without --enable-cuda
    // will not list AV_HWDEVICE_TYPE_CUDA at all - and that is a property of
    // the DLL we ship, not of the user's machine.
    std::string types;
    for (AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;;) {
        t = fns.iterate_types(t);
        if (t == AV_HWDEVICE_TYPE_NONE) {
            break;
        }
        if (!types.empty()) {
            types += ",";
        }
        types += fns.type_name ? fns.type_name(t) : "?";
        if (types.size() > 120) {
            types += "...";
            break;
        }
    }
    p.hwTypes = types;

    // 3. Can FFmpeg actually get a device of each type? D3D11 is probed too and
    // not as an afterthought: on Windows it needs no nvcuda at all and may be
    // the cheaper path, so "is CUDA available" alone is the wrong question.
    p.cudaDevice = tryDevice(fns, AV_HWDEVICE_TYPE_CUDA, p.cudaErr);
    p.d3d11Device = tryDevice(fns, AV_HWDEVICE_TYPE_D3D11VA, p.d3d11Err);

    // 3. What does the encoder itself advertise? This is the decisive half:
    // a perfectly working CUDA device is useless if hevc_nvenc was built
    // without a device-context method, and that is a property of how the DLL
    // was compiled, not of the machine.
    const AVCodec* enc = fns.find_encoder("hevc_nvenc");
    if (!enc) {
        p.cudaErr += p.cudaErr.empty() ? "" : "; ";
        p.cudaErr += "hevc_nvenc not found in this FFmpeg build";
        return p;
    }
    p.nvencFound = true;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = fns.get_hw_config(enc, i);
        if (!cfg) {
            break;
        }
        ++p.nvencHwConfigs;
        p.nvencMethods |= cfg->methods;
        const AVPixelFormat fmt = static_cast<AVPixelFormat>(cfg->pix_fmt);
        if (fmt == AV_PIX_FMT_CUDA) {
            p.nvencCuda = true;
        }
        if (fmt == AV_PIX_FMT_D3D11) {
            p.nvencD3D11 = true;
        }
        // One config is enough to answer the question; walking them all only
        // risks a runaway loop on a malformed build.
        break;
    }

    // 4. The last open link in the chain, and the only one the FFmpeg API
    // cannot answer for us: getting our GL textures into CUDA memory. There is
    // no FFmpeg function for that - it is the driver's GL/CUDA interop, reached
    // through nvcuda.dll exports. Without them the plan dies here no matter
    // how willing the encoder is.
    //
    // Resolution only, no calls: cuCtxCreate on the game's thread would
    // initialise CUDA inside a live D3D/OpenGL process for a feature that is
    // still turned off. This probe has to stay free of side effects.
    if (p.cudaDriver) {
        HMODULE nv = LoadLibraryW(L"nvcuda.dll");
        if (nv) {
            // The first version checked three symbols and called the machine
            // green; the spike then failed on it, because
            // cuGraphicsResourceGetTexture does not exist in this driver at all.
            // A probe that answers "are a few entry points present" when the
            // thing that matters needs a dozen is worse than none.
            auto has = [&](const char* n) { return GetProcAddress(nv, n) != nullptr; };
            p.glInterop =
                has("cuGraphicsGLRegisterImage") &&
                has("cuGraphicsResourceSetMapFlags") &&
                has("cuGraphicsMapResources") &&
                has("cuGraphicsUnmapResources") &&
                has("cuGraphicsResourceGetMappedPointer") &&
                has("cuGraphicsUnregisterResource");
            p.glTexObjectApi = has("cuGraphicsResourceGetTexture");
            if (!p.glInterop) {
                p.cudaErr += p.cudaErr.empty() ? "" : "; ";
                p.cudaErr += "nvcuda lacks the GL interop entry points";
            }
            FreeLibrary(nv);
        }
    }
    return p;
}

std::string hwProbeLine(const HwProbe& p) {
    std::string s;
    auto add = [&](const char* k, bool v) {
        if (!s.empty()) s += " ";
        s += k;
        s += v ? "=yes" : "=no";
    };
    add("dlls", p.ffmpegDlls);
    add("hwapi", p.hwApi);
    add("nvcuda", p.cudaDriver);
    add("cudaDev", p.cudaDevice);
    add("d3d11Dev", p.d3d11Device);
    add("nvenc", p.nvencFound);
    s += " methods=0x" ;
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%x", p.nvencMethods);
        s += buf;
    }
    s += " nvencCfgs=" + std::to_string(p.nvencHwConfigs);
    add("nvencCUDA", p.nvencCuda);
    add("nvencD3D11", p.nvencD3D11);
    add("glInterop", p.glInterop);
    add("glTexObj", p.glTexObjectApi);
    if (!p.hwTypes.empty()) {
        s += " types=" + p.hwTypes;
    }
    if (!p.cudaErr.empty()) {
        s += " | cuda: " + p.cudaErr;
    }
    if (!p.d3d11Err.empty()) {
        s += " | d3d11: " + p.d3d11Err;
    }
    return s;
}

}  // namespace render
}  // namespace skkbot2

#else  // !GEODE_IS_WINDOWS - the renderer is Windows-only.

namespace skkbot2 {
namespace render {

HwProbe probeHwFrames(const std::string&) {
    HwProbe p;
    p.cudaErr = "renderer is not available on this platform";
    return p;
}

std::string hwProbeLine(const HwProbe&) {
    return "n/a (non-Windows)";
}

}  // namespace render
}  // namespace skkbot2

#endif
