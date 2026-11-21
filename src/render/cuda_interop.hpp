#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <GL/gl.h>

#ifndef CUDA_VERSION
#define CUDA_VERSION 12000
#endif

#ifndef CUDAAPI
#define CUDAAPI __stdcall
#endif

using CUdevice = int;
using CUcontext = void*;
using CUstream = void*;
using CUdeviceptr = unsigned long long;
using CUgraphicsResource = void*;

namespace skkbot2 {
namespace render {

using ::CUdevice;
using ::CUcontext;
using ::CUstream;
using ::CUdeviceptr;
using ::CUgraphicsResource;

enum CUmemorytype_enum {
    CU_MEMORYTYPE_HOST = 0x01,
    CU_MEMORYTYPE_DEVICE = 0x02,
    CU_MEMORYTYPE_ARRAY = 0x03,
    CU_MEMORYTYPE_UNIFIED = 0x04
};

struct CUDA_MEMCPY2D {
    size_t srcXInBytes = 0;
    size_t srcY = 0;
    CUmemorytype_enum srcMemoryType = CU_MEMORYTYPE_DEVICE;
    const void *srcHost = nullptr;
    CUdeviceptr srcDevice = 0;
    void *srcArray = nullptr;
    size_t srcPitch = 0;

    size_t dstXInBytes = 0;
    size_t dstY = 0;
    CUmemorytype_enum dstMemoryType = CU_MEMORYTYPE_DEVICE;
    void *dstHost = nullptr;
    CUdeviceptr dstDevice = 0;
    void *dstArray = nullptr;
    size_t dstPitch = 0;

    size_t WidthInBytes = 0;
    size_t Height = 0;
};

class CudaInterop {
public:
    static CudaInterop& get();

    bool isLoaded() const { return m_symbolsLoaded; }
    bool isReady() const { return m_initialized; }

    bool loadSymbols();

    bool initContext(CUcontext existingCtx = nullptr);

    bool registerGlBuffer(int slot, unsigned int glBufferId);

    void unregisterGlBuffer(int slot);

    void unregisterAll();

    bool copyGlBufferToNv12(int slot,
                            CUdeviceptr dstY, size_t dstPitchY,
                            CUdeviceptr dstUV, size_t dstPitchUV,
                            unsigned int width, unsigned int height);

    void syncStream();

    void destroy();

    CUcontext context() const { return m_ctx; }
    CUstream stream() const { return m_stream; }

private:
    CudaInterop() = default;
    ~CudaInterop() { destroy(); }

    bool m_symbolsLoaded = false;
    bool m_initialized = false;
    bool m_ownsContext = false;

    HMODULE m_nvcuda = nullptr;
    CUcontext m_ctx = nullptr;
    CUstream m_stream = nullptr;

    std::vector<CUgraphicsResource> m_resources;
};

}
}

#endif // GEODE_IS_WINDOWS
