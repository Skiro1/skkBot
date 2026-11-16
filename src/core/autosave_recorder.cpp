#include "core/autosave_recorder.hpp"
#include "core/autosave_write_order.hpp"

#include "skkBot.hpp"
#include "core/paths.hpp"
#include "skc/skc_bot.hpp"
#include "skc/skc_format.hpp"
#include "format/skk_format.hpp"

#include <Geode/Geode.hpp>

#include <chrono>
#include <ctime>
#include <memory>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>


namespace skkbot2::autosave {

namespace {

bool g_cachedEnabled = false;
bool g_cachedLoaded = false;

std::atomic<bool> g_skkJob{false};
std::atomic<bool> g_skcJob{false};
SnapshotWriteOrder g_skkWriteOrder;
SnapshotWriteOrder g_skcWriteOrder;

double g_lastSkk = -1.0e9;
double g_lastSkc = -1.0e9;

constexpr double kSkkInterval = 10.0;
constexpr double kSkcInterval = 5.0;

double nowSeconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

namespace fs = std::filesystem;

fs::path directoryPrepared(std::error_code& ec) {
    auto dir = skkbot2::paths::replays();
    fs::create_directories(dir, ec);
    return dir;
}

}

bool enabled() {
    if (!g_cachedLoaded) {
        g_cachedLoaded = true;
        g_cachedEnabled = Mod::get()->getSavedValue<bool>("gui_autosave_record", false);
        skkbot2::log::info("[AUTOSAVE] {}", g_cachedEnabled ? "on" : "off");
    }
    return g_cachedEnabled;
}

void setEnabled(bool on) {
    g_cachedEnabled = on;
    Mod::get()->setSavedValue("gui_autosave_record", on);
    skkbot2::log::info("[AUTOSAVE] {}", on ? "enabled" : "disabled");
}

std::string skcAutosaveName() {
    return skkAutosaveName();
}

std::string skkAutosaveName() {
    std::string raw;
    if (auto* playLayer = PlayLayer::get()) {
        if (playLayer->m_level) {
            raw = playLayer->m_level->m_levelName;
        }
    }

    for (auto& ch : raw) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch < 32 || uch == 0x7F || ch == '/' || ch == '\\' || ch == ':' ||
            ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' ||
            ch == '|') {
            ch = '_';
        }
    }

    auto isEdge = [](char ch) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        return uch == ' ' || uch == '\t' || uch == '.' || uch == '_';
    };
    while (!raw.empty() && isEdge(raw.front())) raw.erase(raw.begin());
    while (!raw.empty() && isEdge(raw.back())) raw.pop_back();

    constexpr size_t kMaxBytes = 96;
    if (raw.size() > kMaxBytes) {
        size_t cut = kMaxBytes;
        while (cut > 0 && (raw[cut] & 0xC0) == 0x80) --cut;
        raw.resize(cut);
    }

    if (raw == "." || raw == "..") raw.clear();
    if (raw.empty()) return std::string("autosave");
    return raw + "_autosave";
}

static void skkWriteSnapshot(std::shared_ptr<SkMacro> snapshot, uint64_t revision) {
    const bool ran = g_skkWriteOrder.run(revision, snapshot->name, [&]() {
        std::error_code ec;
        auto directory = directoryPrepared(ec);
        bool ok = false;
        if (!ec) {
            normalizeSkPersistenceTiming(*snapshot);
            snapshot->duration = 0.0;
            double safeFramerate = std::isfinite(snapshot->framerate) &&
                                           snapshot->framerate > 0.0
                ? snapshot->framerate
                : 240.0;
            for (auto const& input : snapshot->inputs) {
                double inputTime = input.hasAbsoluteTime()
                    ? input.timeSeconds
                    : static_cast<double>(std::max(0, input.tick)) / safeFramerate;
                if (!input.hasAbsoluteTime() && std::isfinite(input.cbsTimeOffset) &&
                    input.cbsTimeOffset >= 0.0) {
                    inputTime += input.cbsTimeOffset;
                }
                snapshot->duration = std::max(snapshot->duration, inputTime);
            }
            ok = snapshot->saveToPath(directory / skk::stringToPath(snapshot->name + ".skk"));
        }
        if (!ok) {
            skkbot2::log::error("[AUTOSAVE] SKK snapshot to '{}' failed{}",
                                 snapshot->name, ec.message().empty()
                                     ? " (cannot prepare replay directory)"
                                     : " (" + ec.message() + ")");
        } else {
            skkbot2::log::info(
                "[AUTOSAVE] SKK snapshot '{}' (inputs={} frames={})",
                snapshot->name, snapshot->inputs.size(), snapshot->perFrames.size());
        }
        return ok;
    });
    if (!ran) skkbot2::log::debug("[AUTOSAVE] SKK stale snapshot skipped: revision={}", revision);
}

static void skkSnapshot(ReplayEngine* engine, bool sync = false) {
    if (!engine->skMode || !engine->activeSk) return;
    if (!sync && engine->activeSk->inputs.empty() &&
        engine->activeSk->persistenceAttempts.empty() &&
        engine->activeSk->perFrames.empty() &&
        !engine->persistenceCaptureAttempt.hasData() &&
        (!engine->captureStaging || (engine->captureStaging->inputs.empty() &&
                                    engine->captureStaging->perFrames.empty()))) {
        return;
    }

    std::shared_ptr<SkMacro> snapshot =
        std::make_shared<SkMacro>(*engine->activeSk);
    engine->appendCaptureStagingTo(*snapshot);
    engine->appendPendingPersistenceTo(*snapshot);
    engine->captureVarianceSnapshot(*snapshot);
    if (sync) {
        skkbot2::log::info(
            "[AUTOSAVE] SKK финальный снимок: inputs={} frames={} chain={} variance={}",
            snapshot->inputs.size(), snapshot->perFrames.size(),
            snapshot->persistenceAttempts.size(), snapshot->varianceOverrides.size());
    }
    snapshot->wireSeedStamp =
        geode::Mod::get()->getSavedValue<bool>("gui_record_seed", true) ? 1 : -1;
    snapshot->author = GJAccountManager::get()->m_username;
    snapshot->recordTimestamp = static_cast<int64_t>(std::time(nullptr));
    snapshot->fileFormat = SkFileFormat::SKK3;
    snapshot->name = skkAutosaveName();
    const auto revision = g_skkWriteOrder.issue();

    if (sync) {
        skkWriteSnapshot(std::move(snapshot), revision);
        return;
    }

    bool expected = false;
    if (!g_skkJob.compare_exchange_strong(expected, true)) {
        return;
    }
    std::thread([snapshot = std::move(snapshot), revision]() mutable {
        skkWriteSnapshot(std::move(snapshot), revision);
        g_skkJob.store(false);
    }).detach();
}

static void skcWriteSnapshot(std::shared_ptr<skk::skc::Macro> snapshot,
                             const std::string& name, uint64_t revision) {
    const bool ran = g_skcWriteOrder.run(revision, name, [&]() {
        std::error_code ec;
        auto directory = directoryPrepared(ec);
        bool ok = false;
        if (!ec) {
            ok = skk::skc::saveToFile(directory / skk::stringToPath(name + ".skc"), *snapshot);
        }
        if (!ok) {
            skkbot2::log::error("[AUTOSAVE] SKC snapshot '{}' failed ({} actions)",
                                 name, snapshot->actions.size());
        } else {
            skkbot2::log::info("[AUTOSAVE] SKC snapshot '{}' ({} actions)",
                                 name, snapshot->actions.size());
        }
        return ok;
    });
    if (!ran) skkbot2::log::debug("[AUTOSAVE] SKC stale snapshot skipped: revision={}", revision);
}

static void skcSnapshot(skk::skc::Bot& bot, bool sync = false) {
    std::size_t count = bot.getInputCount();
    if (count == 0 && !sync) return;

    const std::string rescueName = skcAutosaveName();
    std::shared_ptr<skk::skc::Macro> snapshot =
        std::make_shared<skk::skc::Macro>(bot.getMacro());
    snapshot->wireSeedStamp =
        geode::Mod::get()->getSavedValue<bool>("gui_record_seed", true) ? 1 : -1;

    const auto revision = g_skcWriteOrder.issue();

    if (sync) {
        skcWriteSnapshot(std::move(snapshot), rescueName, revision);
        return;
    }

    bool expected = false;
    if (!g_skcJob.compare_exchange_strong(expected, true)) {
        return;
    }
    std::thread([snapshot = std::move(snapshot), name = rescueName, revision]() {
        skcWriteSnapshot(std::move(snapshot), name, revision);
        g_skcJob.store(false);
    }).detach();
}

void tickRecording() {
    if (!enabled()) return;

    auto* engine = ReplayEngine::get();
    const double t = nowSeconds();

    auto& bot = skk::skc::Bot::get();
    if (bot.isRecording()) {
        if (t - g_lastSkc >= kSkcInterval) {
            g_lastSkc = t;
            skcSnapshot(bot);
        }
    } else {
        g_lastSkc = t;
    }

    if (engine && engine->engineMode == MODE_CAPTURE) {
        if (t - g_lastSkk >= kSkkInterval) {
            g_lastSkk = t;
            skkSnapshot(engine);
        }
    } else {
        g_lastSkk = t;
    }
}

void flushRecording() {
    if (!enabled()) return;

    auto* engine = ReplayEngine::get();
    auto& bot = skk::skc::Bot::get();
    if (bot.isRecording()) {
        skcSnapshot(bot, true);
    }
    if (engine && engine->engineMode == MODE_CAPTURE) {
        skkSnapshot(engine, true);
    }
}

}
