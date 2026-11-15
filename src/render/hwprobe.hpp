#pragma once

#include <string>


namespace skkbot2 {
namespace render {

struct HwProbe {
    bool ffmpegDlls = false;
    bool hwApi = false;
    bool cudaDriver = false;
    bool cudaDevice = false;
    bool d3d11Device = false;
    bool nvencFound = false;
    int nvencMethods = 0;
    bool nvencCuda = false;
    bool nvencD3D11 = false;
    int nvencHwConfigs = 0;
    bool glInterop = false;
    bool glTexObjectApi = false;
    std::string hwTypes;
    std::string cudaErr;
    std::string d3d11Err;
};

HwProbe probeHwFrames(const std::filesystem::path& ffmpegDir);

std::string hwProbeLine(const HwProbe& p);

}
}
