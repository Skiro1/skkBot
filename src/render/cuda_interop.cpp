#include "cuda_interop.hpp"

#ifdef GEODE_IS_WINDOWS

#include <windows.h>
#include <GL/gl.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>

#include "../logger/logger.hpp"

namespace skkbot2 {
namespace render {

namespace {

using CUresult = int;
constexpr CUresult CUDA_SUCCESS = 0;

typedef CUresult (CUDAAPI *PFN_cuInit)(unsigned int Flags);
typedef CUresult (CUDAAPI *PFN_cuDeviceGet)(CUdevice *device, int ordinal);
typedef CUresult (CUDAAPI *PFN_cuCtxCreate)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult (CUDAAPI *PFN_cuCtxDestroy)(CUcontext ctx);
typedef CUresult (CUDAAPI *PFN_cuCtxPushCurrent)(CUcontext ctx);
typedef CUresult (CUDAAPI *PFN_cuCtxPopCurrent)(CUcontext *pctx);
typedef CUresult (CUDAAPI *PFN_cuCtxGetCurrent)(CUcontext *pctx);
typedef CUresult (CUDAAPI *PFN_cuGraphicsGLRegisterBuffer)(CUgraphicsResource *pCudaResource, unsigned int buffer, unsigned int Flags);
typedef CUresult (CUDAAPI *PFN_cuGraphicsMapResources)(unsigned int count, CUgraphicsResource *resources, CUstream hStream);
typedef CUresult (CUDAAPI *PFN_cuGraphicsResourceGetMappedPointer)(CUdeviceptr *pDevPtr, size_t *pSize, CUgraphicsResource resource);
typedef CUresult (CUDAAPI *PFN_cuGraphicsUnmapResources)(unsigned int count, CUgraphicsResource *resources, CUstream hStream);
typedef CUresult (CUDAAPI *PFN_cuGraphicsUnregisterResource)(CUgraphicsResource resource);
typedef CUresult (CUDAAPI *PFN_cuMemcpy2DAsync)(const CUDA_MEMCPY2D *pCopy, CUstream hStream);
typedef CUresult (CUDAAPI *PFN_cuStreamCreate)(CUstream *phStream, unsigned int Flags);
typedef CUresult (CUDAAPI *PFN_cuStreamDestroy)(CUstream hStream);
typedef CUresult (CUDAAPI *PFN_cuStreamSynchronize)(CUstream hStream);
typedef CUresult (CUDAAPI *PFN_cuGetErrorString)(CUresult error, const char **pStr);
typedef CUresult (CUDAAPI *PFN_cuGetErrorName)(CUresult error, const char **pStr);

struct CudaDriverFns {
    PFN_cuInit cuInit = nullptr;
    PFN_cuDeviceGet cuDeviceGet = nullptr;
    PFN_cuCtxCreate cuCtxCreate = nullptr;
    PFN_cuCtxDestroy cuCtxDestroy = nullptr;
    PFN_cuCtxPushCurrent cuCtxPushCurrent = nullptr;
    PFN_cuCtxPopCurrent cuCtxPopCurrent = nullptr;
    PFN_cuCtxGetCurrent cuCtxGetCurrent = nullptr;
    PFN_cuGraphicsGLRegisterBuffer cuGraphicsGLRegisterBuffer = nullptr;
    PFN_cuGraphicsMapResources cuGraphicsMapResources = nullptr;
    PFN_cuGraphicsResourceGetMappedPointer cuGraphicsResourceGetMappedPointer = nullptr;
    PFN_cuGraphicsUnmapResources cuGraphicsUnmapResources = nullptr;
    PFN_cuGraphicsUnregisterResource cuGraphicsUnregisterResource = nullptr;
    PFN_cuMemcpy2DAsync cuMemcpy2DAsync = nullptr;
    PFN_cuStreamCreate cuStreamCreate = nullptr;
    PFN_cuStreamDestroy cuStreamDestroy = nullptr;
    PFN_cuStreamSynchronize cuStreamSynchronize = nullptr;
    PFN_cuGetErrorString cuGetErrorString = nullptr;
    PFN_cuGetErrorName cuGetErrorName = nullptr;
};

static CudaDriverFns s_cuda;

static const char* cudaErrStr(CUresult res) {
    if (s_cuda.cuGetErrorString) {
        const char* str = nullptr;
        if (s_cuda.cuGetErrorString(res, &str) == CUDA_SUCCESS && str) {
            return str;
        }
    }
    return "unknown CUDA error";
}

}

CudaInterop& CudaInterop::get() {
    static CudaInterop instance;
    return instance;
}

bool CudaInterop::loadSymbols() {
    if (m_symbolsLoaded) {
        return true;
    }

    m_nvcuda = LoadLibraryW(L"nvcuda.dll");
    if (!m_nvcuda) {
        skkbot2::log::warn("[CUDA] nvcuda.dll not found");
        return false;
    }

    auto getProc = [this](const char* name, const char* alt = nullptr) -> FARPROC {
        FARPROC p = GetProcAddress(m_nvcuda, name);
        if (!p && alt) {
            p = GetProcAddress(m_nvcuda, alt);
        }
        return p;
    };

    s_cuda.cuInit = reinterpret_cast<PFN_cuInit>(getProc("cuInit"));
    s_cuda.cuDeviceGet = reinterpret_cast<PFN_cuDeviceGet>(getProc("cuDeviceGet"));
    s_cuda.cuCtxCreate = reinterpret_cast<PFN_cuCtxCreate>(getProc("cuCtxCreate_v2", "cuCtxCreate"));
    s_cuda.cuCtxDestroy = reinterpret_cast<PFN_cuCtxDestroy>(getProc("cuCtxDestroy_v2", "cuCtxDestroy"));
    s_cuda.cuCtxPushCurrent = reinterpret_cast<PFN_cuCtxPushCurrent>(getProc("cuCtxPushCurrent_v2", "cuCtxPushCurrent"));
    s_cuda.cuCtxPopCurrent = reinterpret_cast<PFN_cuCtxPopCurrent>(getProc("cuCtxPopCurrent_v2", "cuCtxPopCurrent"));
    s_cuda.cuCtxGetCurrent = reinterpret_cast<PFN_cuCtxGetCurrent>(getProc("cuCtxGetCurrent"));
    s_cuda.cuGraphicsGLRegisterBuffer = reinterpret_cast<PFN_cuGraphicsGLRegisterBuffer>(getProc("cuGraphicsGLRegisterBuffer"));
    s_cuda.cuGraphicsMapResources = reinterpret_cast<PFN_cuGraphicsMapResources>(getProc("cuGraphicsMapResources"));
    s_cuda.cuGraphicsResourceGetMappedPointer = reinterpret_cast<PFN_cuGraphicsResourceGetMappedPointer>(getProc("cuGraphicsResourceGetMappedPointer_v2", "cuGraphicsResourceGetMappedPointer"));
    s_cuda.cuGraphicsUnmapResources = reinterpret_cast<PFN_cuGraphicsUnmapResources>(getProc("cuGraphicsUnmapResources"));
    s_cuda.cuGraphicsUnregisterResource = reinterpret_cast<PFN_cuGraphicsUnregisterResource>(getProc("cuGraphicsUnregisterResource"));
    s_cuda.cuMemcpy2DAsync = reinterpret_cast<PFN_cuMemcpy2DAsync>(getProc("cuMemcpy2DAsync_v2", "cuMemcpy2DAsync"));
    s_cuda.cuStreamCreate = reinterpret_cast<PFN_cuStreamCreate>(getProc("cuStreamCreate"));
    s_cuda.cuStreamDestroy = reinterpret_cast<PFN_cuStreamDestroy>(getProc("cuStreamDestroy_v2", "cuStreamDestroy"));
    s_cuda.cuStreamSynchronize = reinterpret_cast<PFN_cuStreamSynchronize>(getProc("cuStreamSynchronize"));
    s_cuda.cuGetErrorString = reinterpret_cast<PFN_cuGetErrorString>(getProc("cuGetErrorString"));
    s_cuda.cuGetErrorName = reinterpret_cast<PFN_cuGetErrorName>(getProc("cuGetErrorName"));

    if (!s_cuda.cuInit || !s_cuda.cuCtxPushCurrent || !s_cuda.cuCtxPopCurrent ||
        !s_cuda.cuGraphicsGLRegisterBuffer || !s_cuda.cuGraphicsMapResources ||
        !s_cuda.cuGraphicsResourceGetMappedPointer || !s_cuda.cuGraphicsUnmapResources ||
        !s_cuda.cuGraphicsUnregisterResource || !s_cuda.cuMemcpy2DAsync) {
        skkbot2::log::error("[CUDA] Required CUDA driver symbols missing in nvcuda.dll");
        FreeLibrary(m_nvcuda);
        m_nvcuda = nullptr;
        return false;
    }

    CUresult res = s_cuda.cuInit(0);
    if (res != CUDA_SUCCESS) {
        skkbot2::log::error("[CUDA] cuInit(0) failed: {} ({})", cudaErrStr(res), res);
        FreeLibrary(m_nvcuda);
        m_nvcuda = nullptr;
        return false;
    }

    m_symbolsLoaded = true;
    skkbot2::log::info("[CUDA] Driver symbols successfully loaded and initialized");
    return true;
}

bool CudaInterop::initContext(CUcontext existingCtx) {
    if (!loadSymbols()) {
        return false;
    }

    destroy();

    if (existingCtx) {
        m_ctx = existingCtx;
        m_ownsContext = false;
        skkbot2::log::info("[CUDA] Attached to external CUDA context ({})", existingCtx);
    } else {
        CUdevice dev = 0;
        CUresult res = s_cuda.cuDeviceGet(&dev, 0);
        if (res != CUDA_SUCCESS) {
            skkbot2::log::error("[CUDA] cuDeviceGet failed: {}", cudaErrStr(res));
            return false;
        }
        res = s_cuda.cuCtxCreate(&m_ctx, 0, dev);
        if (res != CUDA_SUCCESS) {
            skkbot2::log::error("[CUDA] cuCtxCreate failed: {}", cudaErrStr(res));
            return false;
        }
        m_ownsContext = true;
        skkbot2::log::info("[CUDA] Created standalone CUDA context ({})", m_ctx);
    }

    s_cuda.cuCtxPushCurrent(m_ctx);
    CUresult res = s_cuda.cuStreamCreate(&m_stream, 0);
    CUcontext dummy = nullptr;
    s_cuda.cuCtxPopCurrent(&dummy);

    if (res != CUDA_SUCCESS) {
        skkbot2::log::error("[CUDA] cuStreamCreate failed: {}", cudaErrStr(res));
        destroy();
        return false;
    }

    m_initialized = true;
    return true;
}

bool CudaInterop::registerGlBuffer(int slot, unsigned int glBufferId) {
    if (!m_initialized || glBufferId == 0) {
        return false;
    }

    if (!wglGetCurrentContext()) {
        skkbot2::log::error("[CUDA] registerGlBuffer: No active OpenGL context on current thread!");
        return false;
    }

    if (slot >= static_cast<int>(m_resources.size())) {
        m_resources.resize(slot + 1, nullptr);
    }

    if (m_resources[slot]) {
        unregisterGlBuffer(slot);
    }

    s_cuda.cuCtxPushCurrent(m_ctx);
    CUgraphicsResource res = nullptr;
    CUresult r = s_cuda.cuGraphicsGLRegisterBuffer(&res, glBufferId, 1);
    CUcontext dummy = nullptr;
    s_cuda.cuCtxPopCurrent(&dummy);

    if (r != CUDA_SUCCESS) {
        skkbot2::log::error("[CUDA] cuGraphicsGLRegisterBuffer(slot={}, pbo={}) failed: {} ({})",
                            slot, glBufferId, cudaErrStr(r), r);
        return false;
    }

    m_resources[slot] = res;
    return true;
}

void CudaInterop::unregisterGlBuffer(int slot) {
    if (slot < 0 || slot >= static_cast<int>(m_resources.size())) {
        return;
    }
    if (m_resources[slot]) {
        if (wglGetCurrentContext() && m_ctx) {
            s_cuda.cuCtxPushCurrent(m_ctx);
            s_cuda.cuGraphicsUnregisterResource(m_resources[slot]);
            CUcontext dummy = nullptr;
            s_cuda.cuCtxPopCurrent(&dummy);
        }
        m_resources[slot] = nullptr;
    }
}

void CudaInterop::unregisterAll() {
    for (size_t s = 0; s < m_resources.size(); ++s) {
        unregisterGlBuffer(static_cast<int>(s));
    }
    m_resources.clear();
}

bool CudaInterop::copyGlBufferToNv12(int slot,
                                     CUdeviceptr dstY, size_t dstPitchY,
                                     CUdeviceptr dstUV, size_t dstPitchUV,
                                     unsigned int width, unsigned int height) {
    if (!m_initialized || slot < 0 || slot >= static_cast<int>(m_resources.size()) || !m_resources[slot]) {
        return false;
    }

    if (!wglGetCurrentContext()) {
        skkbot2::log::error("[CUDA] copyGlBufferToNv12: No active OpenGL context on calling thread!");
        return false;
    }

    CUcontext cur = nullptr;
    if (s_cuda.cuCtxGetCurrent) {
        s_cuda.cuCtxGetCurrent(&cur);
    }
    const bool needPush = (cur != m_ctx);
    if (needPush) {
        s_cuda.cuCtxPushCurrent(m_ctx);
    }

    CUresult res = s_cuda.cuGraphicsMapResources(1, &m_resources[slot], m_stream);
    if (res != CUDA_SUCCESS) {
        if (needPush) {
            CUcontext dummy = nullptr;
            s_cuda.cuCtxPopCurrent(&dummy);
        }
        skkbot2::log::error("[CUDA] cuGraphicsMapResources(slot={}) failed: {} ({})",
                            slot, cudaErrStr(res), res);
        return false;
    }

    CUdeviceptr srcDevPtr = 0;
    size_t srcSize = 0;
    res = s_cuda.cuGraphicsResourceGetMappedPointer(&srcDevPtr, &srcSize, m_resources[slot]);
    if (res != CUDA_SUCCESS || srcDevPtr == 0) {
        s_cuda.cuGraphicsUnmapResources(1, &m_resources[slot], m_stream);
        if (needPush) {
            CUcontext dummy = nullptr;
            s_cuda.cuCtxPopCurrent(&dummy);
        }
        skkbot2::log::error("[CUDA] cuGraphicsResourceGetMappedPointer(slot={}) failed: {} ({})",
                            slot, cudaErrStr(res), res);
        return false;
    }

    CUDA_MEMCPY2D cpyY = {};
    cpyY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cpyY.srcDevice = srcDevPtr;
    cpyY.srcPitch = width;

    cpyY.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpyY.dstDevice = dstY;
    cpyY.dstPitch = dstPitchY;

    cpyY.WidthInBytes = width;
    cpyY.Height = height;

    res = s_cuda.cuMemcpy2DAsync(&cpyY, m_stream);
    if (res != CUDA_SUCCESS) {
        skkbot2::log::error("[CUDA] cuMemcpy2DAsync(Y) failed: {} ({})", cudaErrStr(res), res);
    }

    const size_t yBytes = static_cast<size_t>(width) * height;
    CUDA_MEMCPY2D cpyUV = {};
    cpyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cpyUV.srcDevice = srcDevPtr + yBytes;
    cpyUV.srcPitch = width;

    cpyUV.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpyUV.dstDevice = dstUV;
    cpyUV.dstPitch = dstPitchUV;

    cpyUV.WidthInBytes = width;
    cpyUV.Height = height / 2;

    res = s_cuda.cuMemcpy2DAsync(&cpyUV, m_stream);
    if (res != CUDA_SUCCESS) {
        skkbot2::log::error("[CUDA] cuMemcpy2DAsync(UV) failed: {} ({})", cudaErrStr(res), res);
    }

    s_cuda.cuGraphicsUnmapResources(1, &m_resources[slot], m_stream);


    if (needPush) {
        CUcontext dummy = nullptr;
        s_cuda.cuCtxPopCurrent(&dummy);
    }
    return true;
}

void CudaInterop::syncStream() {
    if (m_initialized && m_stream) {
        s_cuda.cuCtxPushCurrent(m_ctx);
        s_cuda.cuStreamSynchronize(m_stream);
        CUcontext dummy = nullptr;
        s_cuda.cuCtxPopCurrent(&dummy);
    }
}

void CudaInterop::destroy() {
    unregisterAll();

    if (m_stream) {
        s_cuda.cuCtxPushCurrent(m_ctx);
        s_cuda.cuStreamDestroy(m_stream);
        CUcontext dummy = nullptr;
        s_cuda.cuCtxPopCurrent(&dummy);
        m_stream = nullptr;
    }

    if (m_ownsContext && m_ctx) {
        s_cuda.cuCtxDestroy(m_ctx);
        m_ctx = nullptr;
    }

    m_ownsContext = false;
    m_ctx = nullptr;
    m_initialized = false;
}

}
}

#endif // GEODE_IS_WINDOWS
