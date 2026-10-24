#pragma once

#include <chrono>
#include <exception>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

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
    // Cheap gate for the templates below. The level test used to live inside
    // log(), which is AFTER fmt::format has already parsed the format string
    // and built the message - so a suppressed Debug line still cost a runtime
    // format-string parse, a std::string temporary and a second allocation. With
    // the level set to Debug and a drift probe running 24 times a second, that
    // was the mod's largest per-frame cost and none of it produced output.
    [[nodiscard]] bool wouldLog(Level level) const { return level >= m_level; }
    void setFileLogging(bool enabled);
    bool isFileLogging() const;
    void setSplitFiles(bool enabled);
    bool isSplitFiles() const;
    void setSessionMode(SessionMode mode);
    SessionMode getSessionMode() const { return m_sessionMode; }
    // REMOVED 19.10.2026: the render-session flag and logs/render.txt.
    //
    // render.txt was deliberately NOT gated by the file-logging switch, on the
    // argument that the render summary is the one measurement that cannot be
    // reconstructed afterwards. That argument was right about the measurement
    // and wrong about the consequence: it meant a file nobody asked for appeared
    // on every render, which is not a free safety net but a permanent write the
    // operator cannot turn off. The user cannot get "no logs at all" while one
    // of them keeps writing.
    //
    // What replaced it is nothing clever: render diagnostics now go wherever
    // every other line goes, and follow the same switch. Anyone measuring a
    // render turns file logging on, exactly like for any other diagnosis.

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
    // A log line must never be able to take the game down.

    // These format strings are checked at RUN time (fmt::runtime), because the
    // wrapper takes a string_view. So "{:.0f} {:.0f} {:.0f} {}" with three
    // arguments is not a compile error - it is a thrown fmt::format_error, and
    // an exception escaping a log call inside a GD hook is an unhandled
    // exception crash of the whole game. That is exactly what happened on
    // 19.10.2026: startRecording lost its startFrame argument, so beginning a
    // recording killed Geometry Dash with "argument not found".
    //
    // Formatting failures therefore fall back to the raw format string plus the
    // reason. The line stays recognisable, the placeholders stay visible, and
    // the recording it was announcing still starts.
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

} // namespace skkbot2::log
