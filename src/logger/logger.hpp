#pragma once

#include <chrono>
#include <exception>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

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

    enum class SessionMode {
        None = 0,
        Record = 1,
        Playback = 2
    };

    static Logger& get();

    void setLevel(Level level);
    Level getLevel() const;
    [[nodiscard]] bool wouldLog(Level level) const { return level >= m_level; }
    void setFileLogging(bool enabled);
    bool isFileLogging() const;
    void setSplitFiles(bool enabled);
    bool isSplitFiles() const;
    void setSessionMode(SessionMode mode);
    SessionMode getSessionMode() const { return m_sessionMode; }
    void flush();

    template<typename... Args>
    void debug(std::string_view fmt, Args&&... args) {
        if (!wouldLog(Debug)) return;
        log(Debug, formatForLog(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void info(std::string_view fmt, Args&&... args) {
        if (!wouldLog(Info)) return;
        log(Info, formatForLog(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void warn(std::string_view fmt, Args&&... args) {
        if (!wouldLog(Warn)) return;
        log(Warn, formatForLog(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void error(std::string_view fmt, Args&&... args) {
        if (!wouldLog(Error)) return;
        log(Error, formatForLog(fmt, std::forward<Args>(args)...));
    }

private:

    template<typename... Args>
    static std::string formatForLog(std::string_view fmt, Args&&... args) {
        try {
            return fmt::format(fmt::runtime(fmt), std::forward<Args>(args)...);
        } catch (const std::exception& e) {
            std::string text(fmt);
            text += "  [LOG FORMAT ERROR: ";
            text += e.what();
            text += "]";
            return text;
        }
    }

    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void openLogFile();
    void log(Level level, const std::string& msg);
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

}

namespace skkbot2::log {

template<typename... Args>
void debug(std::string_view fmt, Args&&... args) {
    if (!Logger::get().wouldLog(Logger::Debug)) return;
    Logger::get().debug(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void info(std::string_view fmt, Args&&... args) {
    if (!Logger::get().wouldLog(Logger::Info)) return;
    Logger::get().info(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void warn(std::string_view fmt, Args&&... args) {
    if (!Logger::get().wouldLog(Logger::Warn)) return;
    Logger::get().warn(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void error(std::string_view fmt, Args&&... args) {
    if (!Logger::get().wouldLog(Logger::Error)) return;
    Logger::get().error(fmt, std::forward<Args>(args)...);
}

}
