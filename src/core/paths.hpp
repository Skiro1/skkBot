#ifndef _paths_hpp
#define _paths_hpp

// Single source of truth for every folder and file the mod reads or writes.
//
// Nothing here is an absolute path. Everything is derived from Geode's per-mod
// save directory (`Mod::get()->getSaveDir()`), which is
//   Windows: %LOCALAPPDATA%\GeometryDash\geode\mods\<mod id>
//   macOS:   ~/Library/Application Support/GeometryDash/geode/mods/<mod id>
//   Android: <game>/geode/mods/<mod id>
// so any machine that runs the mod gets the same layout, and an update or a
// reinstall never moves the user's macros and videos.
//
// Why a header and not constants scattered over the call sites: there were 28
// places building `<save dir> / "replays"` and friends by hand, plus the FFmpeg
// search order in the renderer. One of them disagreed with the folder an
// "open folder" button opened, and the report we got was "the bot looks in the
// wrong folder". Adding a folder now means adding one line here, and the
// startup log prints the resolved absolute paths on every PC (see
// `summary()` / main.cpp), so a log is enough to answer "where does it look?".

#include <Geode/loader/Mod.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skkbot2::paths {

namespace fs = std::filesystem;

// Log files live directly in the data root, not in a subfolder: they are the
// first thing a user is asked for, and the mod folder is where they look.
inline constexpr std::string_view kMainLogName = "skkbot2.log";
inline constexpr std::string_view kRecordLogName = "skkbot2_record.log";
inline constexpr std::string_view kPlayLogName = "skkbot2_play.log";

// The data root: everything the mod owns.
inline fs::path dataRoot() {
    return geode::Mod::get()->getSaveDir();
}

inline fs::path replays() { return dataRoot() / "replays"; }
inline fs::path videos() { return dataRoot() / "videos"; }
inline fs::path fonts() { return dataRoot() / "fonts"; }

// User drop-in for the FFmpeg DLLs. First place the renderer probes, and the
// folder the RENDER tab's small square button opens.
inline fs::path ffmpeg() { return dataRoot() / "ffmpeg"; }

// Where Geode unpacks the resources bundled inside the .geode. Read-only in
// practice: LoaderImpl deletes the whole tree on game exit, so a user can put a
// file there and lose it on the next restart.
inline fs::path resources() {
    return geode::Mod::get()->getResourcesDir();
}

// Any file in the data root (log files, debug dumps like inputs_dump.txt).
inline fs::path file(std::string_view name) {
    return dataRoot() / fs::path(std::string(name));
}

inline fs::path logFile(std::string_view name) { return file(name); }

// Every folder the mod creates at load. Keep in sync with `ensureDataFolders`.
inline std::vector<fs::path> dataFolders() {
    return { replays(), videos(), fonts(), ffmpeg() };
}

// Where the renderer looks for the FFmpeg DLLs, in order. The save-dir drop-in
// comes first on purpose:
//   * `geode/unzipped/<id>/resources/<id>` (resources()) is where Geode unpacks
//     the copy bundled inside the .geode -- but that whole tree is DELETED when
//     the game exits, so it is a moving target: users cannot put a file there
//     and expect it to survive, and one failed unpack leaves nothing at all;
//   * `geode/mods/<id>/ffmpeg` is a plain folder next to replays/ and videos/,
//     which is what the RENDER tab's folder button opens and what a user can
//     actually find;
//   * the data root itself, for a user who drops the DLLs next to the logs.
// The resources dir stays as the second candidate so a mod update that ships
// the DLLs keeps working without any manual step.
inline std::vector<fs::path> ffmpegSearchPaths() {
    return { ffmpeg(), resources(), dataRoot() };
}

// Creates every folder above. Returns the paths that could not be created, so
// the caller can say so in the log instead of failing silently later.
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

// Resolved absolute paths, in the order they are printed at startup. Used by
// main.cpp for the `[PATHS]` block and by the GUI diagnostics; a space-free
// label keeps the line greppable in a log from any PC.
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

} // namespace skkbot2::paths

#endif // _paths_hpp
