#include "logger.hpp"
#include "core/paths.hpp"
#include "utils.hpp"

#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace geode::prelude;
using namespace skkbot2;

Logger& Logger::get() {
    static Logger instance;
    return instance;
}

Logger::Logger()
    : m_level(Info)
    , m_fileLogging(false)
    , m_splitFiles(false)
    , m_sessionMode(SessionMode::None) {
    // No log file is opened here — file logging is OFF until the user (or
    // main.cpp init) enables it via setFileLogging(true).
    log(Info, "skkBot2 logger initialized");
}

Logger::~Logger() {
    if (m_file.is_open()) {
        m_file.close();
    }
}

void Logger::setLevel(Level level) {
    m_level = level;
}

Logger::Level Logger::getLevel() const {
    return m_level;
}

void Logger::setFileLogging(bool enabled) {
    // Opening the file AFTER changing the flag keeps the mode-switch line
    // itself out of the file it would (re)route — the main log is initialised
    // on the next message, in the state the flag was left in.
    m_fileLogging = enabled;
    if (enabled && !m_file.is_open()) {
        openLogFile();
    }
}

bool Logger::isFileLogging() const {
    return m_fileLogging;
}

void Logger::setSplitFiles(bool enabled) {
    if (m_splitFiles == enabled) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_splitFiles = enabled;
        if (m_fileRecord.is_open()) m_fileRecord.close();
        if (m_filePlay.is_open()) m_filePlay.close();
    }
    if (enabled) {
        log(Info, "SPLIT_LOG: Record/Play now routed to skkbot2_record.log / skkbot2_play.log");
    } else {
        log(Info, "SPLIT_LOG: Record/Play routed back to skkbot2.log");
    }
}

bool Logger::isSplitFiles() const {
    return m_splitFiles;
}

void Logger::setSessionMode(SessionMode mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessionMode = mode;
}

std::ofstream* Logger::selectLogFile() {
    if (!m_splitFiles || m_sessionMode == SessionMode::None || !m_fileLogging) {
        return &m_file;
    }
    namespace fs = std::filesystem;
    const char* name = (m_sessionMode == SessionMode::Record)
        ? "skkbot2_record.log" : "skkbot2_play.log";
    std::ofstream& target = (m_sessionMode == SessionMode::Record)
        ? m_fileRecord : m_filePlay;
    if (!target.is_open()) {
        fs::path logPath = skkbot2::paths::logFile(name);
        target.open(logPath, std::ios::app);
    }
    return &target;
}

void Logger::openLogFile() {
    namespace fs = std::filesystem;
    fs::path logPath = skkbot2::paths::logFile(skkbot2::paths::kMainLogName);

    m_file.open(logPath, std::ios::app);
    if (!m_file.is_open()) {
        log(Error, "Failed to open log file: " + skk::pathToUtf8(logPath));
    }
}

void Logger::log(Level level, const std::string& msg) {
    if (level < m_level) return;

    // Timestamp via fmt (fmt/format.h is already in every TU that includes
    // logger.hpp). The old path built the same string through
    // std::ostringstream + std::put_time, which goes through the C locale -
    // one of the slowest formatted-output paths in the standard library. At
    // the drift probe's line rate that was measurable per frame.
    const char* levelStrs[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    const char* levelStr = (level >= 0 && level <= 3) ? levelStrs[level] : "???";

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::time_t timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &timeT);

    const std::string output = fmt::format(
        "[{:02d}:{:02d}:{:02d}.{:03d}] [{}] {}",
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long long>(ms.count()), levelStr, msg);

    // GD console mirror: INFO and above. The mirror re-allocates and locks
    // geode's log pipeline per line; mirroring the debug flood (thousands of
    // lines a second in a dual playback) turned it into a measurable share of
    // the frame budget. The log FILE below keeps every level unchanged.
    if (level >= Info) {
        geode::log::info("{}", output);
    }

    // Every line follows the user's file-logging switch, render diagnostics
    // included. Until 19.10.2026 render.txt was written unconditionally, on the
    // argument that the render summary cannot be reconstructed afterwards. That
    // argument was right about the measurement and wrong about the consequence:
    // it made a permanent write the operator could not switch off, so "no logs"
    // was not actually available. See the note in logger.hpp.
    if (!m_fileLogging) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::ofstream* f = selectLogFile(); f && f->is_open()) {
        // No explicit flush and no std::endl: the ofstream buffers, so the
        // OS write happens when the internal buffer fills instead of once per
        // line. An explicit flush here was a syscall per line - at the
        // playback flood rate that was hundreds of milliseconds per wall
        // second, hiding inside the [HOT] "GD base scheduler" share because
        // the probes fire inside GD's own update walk. Diagnostics are not
        // ordered data: losing the last buffered kilobyte if the game dies is
        // the right trade, and WARN/ERROR below stay immediate.
        *f << output << '\n';
        if (level >= Warn) {
            f->flush();
        }
    }
}
