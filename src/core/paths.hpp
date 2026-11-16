#ifndef _paths_hpp
#define _paths_hpp


#include <Geode/loader/Mod.hpp>

#include "../utils.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skkbot2::paths {

namespace fs = std::filesystem;

inline constexpr std::string_view kMainLogName = "skkbot2.log";
inline constexpr std::string_view kRecordLogName = "skkbot2_record.log";
inline constexpr std::string_view kPlayLogName = "skkbot2_play.log";

inline fs::path dataRoot() {
    return skk::stringToPath(geode::Mod::get()->getSaveDir().string());
}

inline fs::path replays() { return dataRoot() / "replays"; }
inline fs::path videos() { return dataRoot() / "videos"; }
inline fs::path fonts() { return dataRoot() / "fonts"; }

inline fs::path ffmpeg() { return dataRoot() / "ffmpeg"; }

inline fs::path resources() {
    return skk::stringToPath(geode::Mod::get()->getResourcesDir().string());
}

inline fs::path file(std::string_view name) {
    return dataRoot() / fs::path(std::string(name));
}

inline fs::path logFile(std::string_view name) { return file(name); }

inline std::vector<fs::path> dataFolders() {
    return { replays(), videos(), fonts(), ffmpeg() };
}

inline std::vector<fs::path> ffmpegSearchPaths() {
    return { ffmpeg(), resources(), dataRoot() };
}

inline std::vector<fs::path> ensureDataFolders() {
    std::vector<fs::path> failed;
    for (auto const& dir : dataFolders()) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            failed.push_back(dir);
        }
    }
    return failed;
}

inline std::vector<std::pair<std::string, fs::path>> summary() {
    return {
        { "data", dataRoot() },
        { "replays", replays() },
        { "videos", videos() },
        { "fonts", fonts() },
        { "ffmpeg", ffmpeg() },
        { "resources", resources() },
        { "log", logFile(kMainLogName) },
    };
}

}

#endif // _paths_hpp
