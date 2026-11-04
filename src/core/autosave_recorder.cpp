#include "core/autosave_recorder.hpp"

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

// ---------------------------------------------------------------------------
// Autosave while recording.
//
// Shape copied from ReplayEngine::saveActiveMacro (checkpoint_system.cpp):
// snapshot on the game thread, serialize + compress + write on a detached
// worker, single-flight guard so slow snapshots throttle the trigger instead
// of stacking threads.
//
// Deliberately narrower than the real save:
//   * no captureVarianceSnapshot / staging flush / practice-checkpoint
//     materialization - those mutate live recording state and a rescue file
//     must never be allowed to distort the stream it evacuates;
//   * no name-bumping in the directory - the autosave name is fixed
//     (`<level>_autosave`), so the file always means "the current/latest
//     recording with that name" and never piles up _1, _2, _3 copies;
//   * writeBinarySafe / saveToFile keep atomic-replace semantics, so a crash
//     mid-write cannot truncate a previous good snapshot.
// ---------------------------------------------------------------------------

namespace skkbot2::autosave {

namespace {

bool g_cachedEnabled = false; // default OFF (user request, 31.10)
bool g_cachedLoaded = false;

std::atomic<bool> g_skkJob{false};
std::atomic<bool> g_skcJob{false};

// Main-thread-only (every call site is applyCustomTickPlan / GUI).
double g_lastSkk = -1.0e9;
double g_lastSkc = -1.0e9;
std::size_t g_lastSkcCount = 0;

constexpr double kSkkInterval = 10.0; // wall seconds between SKK snapshots
constexpr double kSkcInterval = 5.0;  // SKC encode is cheaper, refresh faster

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

} // namespace

bool enabled() {
    // Lazy one-time load of the persisted toggle: the first tick may happen
    // before the settings UI ever runs.
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

// ---------------------------------------------------------------------------
// Rescue name = the live level's name (user request, 31.10: "автосейв
// берет название уровня") + "_autosave". Sanitized for the filesystem exactly
// like the render-video file names in renderer.cpp: control bytes and the
// Windows-forbidden set collapse to '_', whitespace/dots at the edges are
// trimmed (Windows names may not end with a dot), the length is capped on an
// UTF-8 boundary. A name that sanitizes away to nothing falls back to the
// mod's own plain "autosave" - never the macro name (the file means "this
// LEVEL's rescue snapshot", not "someone's finished macro").
// ---------------------------------------------------------------------------
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

    // Byte cap generous for localized names; cut on a UTF-8 boundary.
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

// ---------------------------------------------------------------------------
// SKK: snapshot the whole live SkMacro (per-frame layout included), set the
// write metadata exactly like saveActiveMacro does, then serialize + write
// off-thread. A several-MB .skk costs nothing on the game loop this way.
// ---------------------------------------------------------------------------
static void skkWriteSnapshot(std::shared_ptr<SkMacro> snapshot) {
    std::error_code ec;
    auto directory = directoryPrepared(ec);
    bool ok = false;
    if (!ec) {
        normalizeSkPersistenceTiming(*snapshot);
        // Duration mirrors the real save's rule.
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
        ok = snapshot->saveToPath(directory / (snapshot->name + ".skk"));
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
}

static void skkSnapshot(ReplayEngine* engine, bool sync = false) {
    if (!engine->skMode || !engine->activeSk) return;
    if (engine->activeSk->inputs.empty() &&
        engine->activeSk->persistenceAttempts.empty() &&
        engine->activeSk->perFrames.empty()) {
        return;
    }

    // Cheap vector copies on the game thread; zstd + write on a worker (or
    // inline when sync - a flush at stop must not race the game closing).
    std::shared_ptr<SkMacro> snapshot =
        std::make_shared<SkMacro>(*engine->activeSk);
    snapshot->author = GJAccountManager::get()->m_username;
    snapshot->recordTimestamp = static_cast<int64_t>(std::time(nullptr));
    snapshot->fileFormat = SkFileFormat::SKK3;
    // Level-based rescue name, resolved on the MAIN thread (PlayLayer read);
    // the worker only serializes + writes what it was handed.
    snapshot->name = skkAutosaveName();

    if (sync) {
        skkWriteSnapshot(std::move(snapshot));
        return;
    }

    bool expected = false;
    if (!g_skkJob.compare_exchange_strong(expected, true)) {
        // Previous snapshot still serializing: the next tick fires again, and
        // the delay only grows with the recording - the right back-pressure.
        return;
    }
    std::thread([snapshot = std::move(snapshot)]() mutable {
        skkWriteSnapshot(std::move(snapshot));
        g_skkJob.store(false);
    }).detach();
}

// ---------------------------------------------------------------------------
// SKC: the Action stream is tiny (input-only format), so the snapshot copy is
// free; encode + atomic replace still run off-thread for uniformity.
// No validateActions() here: this is a rescue file, and a mid-hold practice
// seam that the validator would refuse at a real SAVE is exactly the data
// worth keeping alive.
// ---------------------------------------------------------------------------
static void skcWriteSnapshot(std::shared_ptr<skk::skc::Macro> snapshot,
                             const std::string& name) {
    std::error_code ec;
    auto directory = directoryPrepared(ec);
    bool ok = false;
    if (!ec) {
        ok = skk::skc::saveToFile(directory / (name + ".skc"), *snapshot);
    }
    if (!ok) {
        skkbot2::log::error("[AUTOSAVE] SKC snapshot '{}' failed ({} actions)",
                            name, snapshot->actions.size());
    } else {
        skkbot2::log::info("[AUTOSAVE] SKC snapshot '{}' ({} actions)",
                           name, snapshot->actions.size());
    }
}

static void skcSnapshot(skk::skc::Bot& bot, bool sync = false) {
    std::size_t count = bot.getInputCount();
    if (count == 0 || count == g_lastSkcCount) return;

    // Level-based rescue name, resolved on the MAIN thread (PlayLayer read).
    const std::string rescueName = skcAutosaveName();
    std::shared_ptr<skk::skc::Macro> snapshot =
        std::make_shared<skk::skc::Macro>(bot.getMacro());

    if (sync) {
        // Flush-at-stop path: write inline so closing the game right after
        // Stop cannot kill the write (a detached thread dies with it).
        skcWriteSnapshot(std::move(snapshot), rescueName);
        g_lastSkcCount = count;
        return;
    }

    bool expected = false;
    if (!g_skcJob.compare_exchange_strong(expected, true)) {
        return;
    }
    g_lastSkcCount = count;
    std::thread([snapshot = std::move(snapshot), name = rescueName]() {
        skcWriteSnapshot(std::move(snapshot), name);
        g_skcJob.store(false);
    }).detach();
}

void tickRecording() {
    // BUG (04.11): g_cachedEnabled стартует false, а ленивая загрузка из
    // saved.json живёт только внутри enabled() - который тик-путь не вызывает
    // (его вызывает лишь GUI-тумблер в момент клика). Сессия, начатая после
    // перезапуска игры с уже включённым автосейвом, молча не писала ничего:
    // ни периодические снапшоты, ни флаш при Stop (в логе сессии 12:08 нет ни
    // одной строки [AUTOSAVE] при gui_autosave_record=true). Загружаем через
    // enabled() - тот самый ленивый вход.
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
        g_lastSkcCount = 0;
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

    // Fire both engines immediately and SYNCHRONOUSLY: this is the stop/exit
    // moment, a detached worker would race the game closing and the rescue
    // file would never land (the exact "stop did not save" report, 04.11).
    // Overlap with a running tick snapshot is harmless: the sync write simply
    // writes a fresher copy over it (atomic replace).
    auto* engine = ReplayEngine::get();
    auto& bot = skk::skc::Bot::get();
    if (bot.isRecording()) {
        skcSnapshot(bot, /*sync=*/true);
    }
    if (engine && engine->engineMode == MODE_CAPTURE) {
        skkSnapshot(engine, /*sync=*/true);
    }
}

} // namespace skkbot2::autosave
