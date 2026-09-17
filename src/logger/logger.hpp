#pragma once

#include <chrono>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <string>

// skkBot2 logger: a full, self-contained logging module (ported from skkBot,
// adapted for the skkBot-based skkBot2 tree). Every engine `log::` call
// is routed through this Logger singleton, which mirrors messages into the
// Geode console AND (optionally) into a save-dir log file. Split Record/Play
// file routing lets record/play sessions land in dedicated logs, like skkBot.
namespace skkbot2 {

class Logger {
public:
    enum Level {
        Debug = 0,
        Info = 1,
        Warn = 2,
        Error = 3,
        None = 4
    };

    // Active bot session. Drives split-file routing: when split-files mode is
    // ON and a session is active, log lines go to skkbot2_record.log /
    // skkbot2_play.log instead of the main skkbot2.log.
    enum class SessionMode {
        None = 0,      // disabled / menus / level select
        Record = 1,
        Playback = 2
    };

    static Logger& get();

    void setLevel(Level level);
    Level getLevel() const;
    void setFileLogging(bool enabled);
    bool isFileLogging() const;
    void setSplitFiles(bool enabled);
    bool isSplitFiles() const;
    void setSessionMode(SessionMode mode);
    SessionMode getSessionMode() const { return m_sessionMode; }

    template<typename... Args>
    void debug(const std::string& fmt, Args&&... args) {
        log(Debug, fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
    }

    template<typename... Args>
    void info(const std::string& fmt, Args&&... args) {
        log(Info, fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
    }

    template<typename... Args>
    void warn(const std::string& fmt, Args&&... args) {
        log(Warn, fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
    }

    template<typename... Args>
    void error(const std::string& fmt, Args&&... args) {
        log(Error, fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...));
    }

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void openLogFile();
    void log(Level level, const std::string& msg);
    // Pick the ofstream to write a message into based on split-files mode +
    // active session. Returns &m_file (main log) by default (lazily opens the
    // per-mode file on first use).
    std::ofstream* selectLogFile();

    Level m_level;
    bool m_fileLogging;
    bool m_splitFiles;
    SessionMode m_sessionMode;
    std::ofstream m_file;
    std::ofstream m_fileRecord;
    std::ofstream m_filePlay;
    std::mutex m_mutex;
};

} // namespace skkbot2

// Free functions reached by the engine's log call sites. The engine logs via
// the fully-qualified `skkbot2::log::debug/info/warn/error` so every message
// flows through the skkbot2 Logger (console + file + split record/play routing).
namespace skkbot2::log {

template<typename... Args>
void debug(const std::string& fmt, Args&&... args) {
    Logger::get().debug(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void info(const std::string& fmt, Args&&... args) {
    Logger::get().info(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void warn(const std::string& fmt, Args&&... args) {
    Logger::get().warn(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void error(const std::string& fmt, Args&&... args) {
    Logger::get().error(fmt, std::forward<Args>(args)...);
}

} // namespace skkbot2::log
