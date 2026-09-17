#include "logger.hpp"

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
        fs::path logPath = Mod::get()->getSaveDir() / name;
        target.open(logPath, std::ios::app);
    }
    return &target;
}

void Logger::openLogFile() {
    namespace fs = std::filesystem;
    fs::path logPath = Mod::get()->getSaveDir() / "skkbot2.log";

    m_file.open(logPath, std::ios::app);
    if (!m_file.is_open()) {
        log(Error, "Failed to open log file: " + logPath.string());
    }
}

void Logger::log(Level level, const std::string& msg) {
    if (level < m_level) return;

    const char* levelStrs[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    const char* levelStr = (level >= 0 && level <= 3) ? levelStrs[level] : "???";

    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm;
    localtime_s(&tm, &timeT);

    std::ostringstream ss;
    ss << "[" << std::put_time(&tm, "%H:%M:%S") << "." << std::setw(3) << std::setfill('0') << ms.count() << "]";
    ss << " [" << levelStr << "] " << msg;

    std::string output = ss.str();

    geode::log::info("{}", output);

    if (m_fileLogging) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ofstream* f = selectLogFile();
        if (f && f->is_open()) {
            *f << output << std::endl;
            f->flush();
        }
    }
}
