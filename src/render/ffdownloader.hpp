#pragma once

#include "../logger/logger.hpp"
#include "../utils.hpp"
#include "../core/paths.hpp"
#include "ffmpeg.hpp"

#include <Geode/utils/file.hpp>
#include <Geode/utils/web.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace skkbot2 {
namespace render {

class FFmpegDownloader {
public:
    enum class State {
        Idle,
        TestingSpeed,
        Downloading,
        Extracting,
        Finished,
        Failed
    };

    struct Mirror {
        std::string name;
        std::string url;
        double pingMs = 999999.0;
        double speedBps = 0.0;
    };

    static FFmpegDownloader& get() {
        static FFmpegDownloader instance;
        return instance;
    }

    // Checks whether all 7 required FFmpeg DLLs exist in the target ffmpeg/ folder.
    static bool areDllsPresent() {
        const auto dir = skkbot2::paths::ffmpeg();
        std::error_code ec;
        for (const char* dll : FFMPEG_DLLS) {
            if (!std::filesystem::exists(dir / dll, ec)) {
                return false;
            }
        }
        return true;
    }

    State state() const { return m_state.load(); }
    bool isBusy() const {
        auto s = m_state.load();
        return s == State::TestingSpeed || s == State::Downloading || s == State::Extracting;
    }

    float progress() const { return m_progress.load(); }
    const std::string& statusMessage() const { return m_statusMessage; }
    const std::string& activeMirrorName() const { return m_activeMirrorName; }
    const std::string& lastError() const { return m_lastError; }

    void startDownload(std::function<void(bool success)> onComplete = nullptr) {
        if (isBusy()) return;

        m_onComplete = std::move(onComplete);
        m_state.store(State::TestingSpeed);
        m_progress.store(0.0f);
        m_lastError.clear();
        m_statusMessage = "Testing mirrors speed...";

        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }

        m_workerThread = std::thread([this]() {
            runWorker();
        });
    }

    void cancel() {
        m_cancelled.store(true);
    }

    ~FFmpegDownloader() {
        m_cancelled.store(true);
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
    }

private:
    FFmpegDownloader() {
        m_mirrors = {
            { "GitHub", "https://github.com/Skiro1/skkBot/releases/download/ffmpeg/dll.zip" }
        };
    }

    void runWorker() {
        m_cancelled.store(false);

        // 1. Measure speed / latency for mirrors
        std::string chosenUrl = m_mirrors[0].url;
        std::string chosenName = m_mirrors[0].name;
        double bestSpeed = -1.0;

        for (auto& mirror : m_mirrors) {
            if (m_cancelled.load()) {
                m_state.store(State::Idle);
                return;
            }

            m_statusMessage = "Checking speed: " + mirror.name + "...";
            skkbot2::log::info("[FFMPEG-DL] Testing speed for {}: {}", mirror.name, mirror.url);

            auto t0 = std::chrono::steady_clock::now();
            auto req = geode::utils::web::WebRequest();
            req.timeout(std::chrono::seconds(7));
            // Read first 512 KB to test real transfer rate
            req.downloadRange({0, 524287});
            auto res = req.getSync(mirror.url);
            auto t1 = std::chrono::steady_clock::now();

            if (res.ok() && !res.data().empty()) {
                double sec = std::chrono::duration<double>(t1 - t0).count();
                double bps = static_cast<double>(res.data().size()) / std::max(0.001, sec);
                mirror.speedBps = bps;
                mirror.pingMs = sec * 1000.0;
                skkbot2::log::info("[FFMPEG-DL] {} speed: {:.1f} KB/s ({:.0f} ms)",
                                   mirror.name, bps / 1024.0, mirror.pingMs);
                if (bps > bestSpeed) {
                    bestSpeed = bps;
                    chosenUrl = mirror.url;
                    chosenName = mirror.name;
                }
            } else {
                skkbot2::log::warn("[FFMPEG-DL] {} failed speed check (code {})", mirror.name, res.code());
            }
        }

        m_activeMirrorName = chosenName;
        m_state.store(State::Downloading);
        m_statusMessage = "Downloading dll.zip from " + chosenName + "...";
        skkbot2::log::info("[FFMPEG-DL] Selected mirror {} ({}), starting download", chosenName, chosenUrl);

        const auto ffmpegDir = skkbot2::paths::ffmpeg();
        std::error_code ec;
        std::filesystem::create_directories(ffmpegDir, ec);

        const auto zipPath = ffmpegDir / "dll.zip";

        auto dlReq = geode::utils::web::WebRequest();
        dlReq.timeout(std::chrono::seconds(180));
        dlReq.onProgress([this](geode::utils::web::WebProgress const& p) {
            if (p.downloadTotal() > 0) {
                float pct = static_cast<float>(p.downloaded()) / static_cast<float>(p.downloadTotal());
                m_progress.store(std::clamp(pct, 0.0f, 1.0f));
            }
        });

        auto dlRes = dlReq.getSync(chosenUrl);

        if (m_cancelled.load()) {
            std::filesystem::remove(zipPath, ec);
            m_state.store(State::Idle);
            return;
        }

        if (!dlRes.ok() || dlRes.data().empty()) {
            // Fallback to the other mirror if the chosen one failed during full download
            std::string fallbackUrl;
            std::string fallbackName;
            for (const auto& m : m_mirrors) {
                if (m.url != chosenUrl) {
                    fallbackUrl = m.url;
                    fallbackName = m.name;
                    break;
                }
            }

            if (!fallbackUrl.empty()) {
                m_statusMessage = "Retrying from fallback " + fallbackName + "...";
                skkbot2::log::warn("[FFMPEG-DL] Primary download failed (code {}), falling back to {}",
                                   dlRes.code(), fallbackName);
                m_activeMirrorName = fallbackName;
                auto fbReq = geode::utils::web::WebRequest();
                fbReq.timeout(std::chrono::seconds(180));
                fbReq.onProgress([this](geode::utils::web::WebProgress const& p) {
                    if (p.downloadTotal() > 0) {
                        float pct = static_cast<float>(p.downloaded()) / static_cast<float>(p.downloadTotal());
                        m_progress.store(std::clamp(pct, 0.0f, 1.0f));
                    }
                });
                dlRes = fbReq.getSync(fallbackUrl);
            }
        }

        if (!dlRes.ok() || dlRes.data().empty()) {
            m_lastError = "Download failed (HTTP " + std::to_string(dlRes.code()) + ")";
            m_state.store(State::Failed);
            m_statusMessage = "Download failed: " + m_lastError;
            skkbot2::log::error("[FFMPEG-DL] {}", m_lastError);
            if (m_onComplete) m_onComplete(false);
            return;
        }

        m_state.store(State::Extracting);
        m_statusMessage = "Extracting FFmpeg DLLs...";
        skkbot2::log::info("[FFMPEG-DL] Download finished ({} bytes), writing to disk", dlRes.data().size());

        // Geode v6: file-API принимают asp::fs::PathView (UTF-8 строка) —
        // конвертируем через наш мост.
        auto saveRes = dlRes.into(skk::pathToUtf8(zipPath));
        if (!saveRes) {
            m_lastError = "Failed to save dll.zip: " + saveRes.unwrapErr();
            m_state.store(State::Failed);
            m_statusMessage = m_lastError;
            skkbot2::log::error("[FFMPEG-DL] {}", m_lastError);
            if (m_onComplete) m_onComplete(false);
            return;
        }

        // Unzip into ffmpegDir
        auto unzipRes = geode::utils::file::Unzip::intoDir(
            skk::pathToUtf8(zipPath), skk::pathToUtf8(ffmpegDir), true /* deleteZipAfter */);
        if (!unzipRes) {
            m_lastError = "Unzip failed: " + unzipRes.unwrapErr();
            m_state.store(State::Failed);
            m_statusMessage = m_lastError;
            skkbot2::log::error("[FFMPEG-DL] {}", m_lastError);
            if (m_onComplete) m_onComplete(false);
            return;
        }

        // Verify that the DLLs actually arrived
        if (!areDllsPresent()) {
            m_lastError = "Archive unpacked but some DLLs are missing";
            m_state.store(State::Failed);
            m_statusMessage = m_lastError;
            skkbot2::log::error("[FFMPEG-DL] {}", m_lastError);
            if (m_onComplete) m_onComplete(false);
            return;
        }

        m_progress.store(1.0f);
        m_state.store(State::Finished);
        m_statusMessage = "FFmpeg DLLs successfully installed!";
        skkbot2::log::info("[FFMPEG-DL] All 7 FFmpeg DLLs installed into {}", skk::pathToUtf8(ffmpegDir));

        if (m_onComplete) {
            m_onComplete(true);
        }
    }

    std::atomic<State> m_state{State::Idle};
    std::atomic<float> m_progress{0.0f};
    std::atomic<bool> m_cancelled{false};
    std::string m_statusMessage;
    std::string m_activeMirrorName;
    std::string m_lastError;
    std::vector<Mirror> m_mirrors;
    std::thread m_workerThread;
    std::function<void(bool success)> m_onComplete;
};

} // namespace render
} // namespace skkbot2
