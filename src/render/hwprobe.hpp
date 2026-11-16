#pragma once

#include <string>

// Runtime capability probe for the zero-copy (GPU -> NVENC) path.
//
// WHY THIS EXISTS. The idea behind pulling in CUDA/NVENC libraries by hand is
// sound in one specific way: NVENC can ingest a frame straight out of device
// memory, which would remove BOTH of our NV12 copies AND the PCIe readback,
// not just the one copy recorded as the deferred item in the tracker.
//
// Everything else in that advice does not apply here: we never DECODE video
// (no nvcuvid / NVDEC / NvDecoder - there is no input file), we run no CUDA
// kernels (our per-pixel work is OpenGL shaders, so no cudart / .cu), and we
// already encode on the hardware through FFmpeg -> hevc_nvenc. A hard link
// against cuda.lib would also be the wrong SHAPE for a Geode mod, which must
// load on a machine with no CUDA Toolkit: our whole FFmpeg stack is already
// resolved at runtime via LoadLibraryW + GetProcAddress, and this belongs in
// the same pattern rather than in the import table.
//
// This probe answers three questions with numbers instead of assumptions:
//   1. is the driver's CUDA entry point present at all (cuda is NOT universal);
//   2. does the FFmpeg build we actually ship expose the AVHWFrames API;
//   3. does hevc_nvenc advertise a device context, and via which methods
//      (CUDA and/or D3D11 - D3D11 matters because on Windows it needs no
//      nvcuda at all and may be the cheaper path).
//
// It is strictly informational. Nothing in the renderer consumes this yet, and
// no UI toggle is wired up: a toggle for a path that does not exist is a
// control for a hypothesis. The tracker's recorded precondition still stands -
// a way to verify the video is not silently corrupted is required BEFORE any
// zero-copy work starts.
//
// HOW TO READ methods (nvencMethods). Bit values from libavcodec/codec.h:
//   0x01 HW_DEVICE_CTX  the encoder accepts a bare device context
//   0x02 HW_FRAMES_CTX  the encoder accepts a frames context WE build on our
//                       own device  <- the one a zero-copy feed needs
//   0x04 INTERNAL       the encoder makes and owns its hwframes itself
// Measured on this machine: methods=0x2, i.e. HW_FRAMES_CTX and nothing else.
// That is the workable case. INTERNAL alone would have meant the encoder
// insists on its own context and a transfer copy is unavoidable.

namespace skkbot2 {
namespace render {

struct HwProbe {
    bool ffmpegDlls = false;   // our shipped avutil/avcodec loaded
    bool hwApi = false;        // av_hwdevice_* / avcodec_get_hw_config resolved
    bool cudaDriver = false;   // nvcuda.dll present (driver-side CUDA)
    bool cudaDevice = false;   // av_hwdevice_ctx_create("cuda") succeeded
    bool d3d11Device = false;  // av_hwdevice_ctx_create("d3d11") succeeded
    bool nvencFound = false;   // hevc_nvenc present in the FFmpeg build
    int nvencMethods = 0;      // bitmask from avcodec_get_hw_config, union over configs
    bool nvencCuda = false;    // that mask includes HW_DEVICE_CTX via CUDA
    bool nvencD3D11 = false;   // ... and a D3D11 pixel format config
    int nvencHwConfigs = 0;    // how many hw configs hevc_nvenc advertises
    // Whether nvcuda exposes the GL-interop entry points. THE last open link:
    // no FFmpeg function can put a GL texture into CUDA memory, only the
    // driver's cuGraphics* interop can. Without these the zero-copy plan stops
    // here regardless of how willing the encoder is.
    bool glInterop = false;
    // cuGraphicsResourceGetTexture - the DOCUMENTED path - is ABSENT in driver
    // 610.88 on this machine, under any versioned name. Reported separately so a
    // future driver adding it back is visible instead of silent.
    bool glTexObjectApi = false;
    std::string hwTypes;       // hwdevice types this FFmpeg build knows at all
    std::string cudaErr;
    std::string d3d11Err;
};

// Runs once, on first call, and caches. Never throws, never aborts the render:
// every failure path is recorded in the struct and reported as a single line.
HwProbe probeHwFrames(const std::filesystem::path& ffmpegDir);

// One-line, human-readable summary for the render log.
std::string hwProbeLine(const HwProbe& p);

}  // namespace render
}  // namespace skkbot2
