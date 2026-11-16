#include "step_trace.hpp"
#include "core/seam_journal.hpp"

#include "core/paths.hpp"
#include "skkBot.hpp"
#include "utils.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace skkbot2::step_trace {
namespace {

constexpr uint32_t kMaxRows = 400'000;
constexpr uint32_t kFlushRows = 1024;
constexpr uint16_t kVersion = 1;
constexpr size_t kRowBytes = 40;
constexpr size_t kDriftRingSize = 1u << 12;

enum class Mode : uint8_t { None, Record, Playback };

bool g_enabledCache = false;

struct DriftProbe {
    uint32_t frame = 0;
    float p1x = 0.0f;
    float p1y = 0.0f;
    double p1vy = 0.0;
    float p2x = 0.0f;
    float p2y = 0.0f;
    double p2vy = 0.0;
    bool hasP2 = false;
    bool valid = false;
};

struct DriftChecker {
    std::array<DriftProbe, kDriftRingSize> skk;
    std::array<DriftProbe, kDriftRingSize> skc;
    Mode skkMode = Mode::None;
    Mode skcMode = Mode::None;
    bool skkReported = false;
    bool skcReported = false;

    void check(GJBaseGameLayer* layer, int frame, bool recording,
               bool playback, bool isSkc) {
        if (!g_enabledCache || !layer || !layer->m_player1 || frame < 0) {
            return;
        }
        const Mode next = recording ? Mode::Record : playback ? Mode::Playback : Mode::None;
        Mode& mode = isSkc ? skcMode : skkMode;
        bool& reported = isSkc ? skcReported : skkReported;
        auto& ring = isSkc ? skc : skk;

        if (next == Mode::Record && mode != Mode::Record) {
            for (auto& slot : ring) slot.valid = false;
            reported = false;
        } else if (next == Mode::Playback && mode != Mode::Playback) {
            reported = false;
        }
        mode = next;
        if (next == Mode::None) return;

        DriftProbe now;
        now.frame = static_cast<uint32_t>(frame);
        now.p1x = layer->m_player1->getPositionX();
        now.p1y = layer->m_player1->getPositionY();
        now.p1vy = layer->m_player1->m_yVelocity;
        if (layer->m_player2) {
            now.hasP2 = true;
            now.p2x = layer->m_player2->getPositionX();
            now.p2y = layer->m_player2->getPositionY();
            now.p2vy = layer->m_player2->m_yVelocity;
        }
        now.valid = true;

        auto& reference = ring[static_cast<size_t>(frame) % kDriftRingSize];
        if (next == Mode::Record) {
            reference = now;
            return;
        }
        if (reported || !reference.valid || reference.frame != now.frame) return;

        const bool mismatch = reference.p1x != now.p1x || reference.p1y != now.p1y ||
            reference.p1vy != now.p1vy || reference.hasP2 != now.hasP2 ||
            (now.hasP2 && (reference.p2x != now.p2x || reference.p2y != now.p2y ||
                           reference.p2vy != now.p2vy));
        if (!mismatch) return;

        reported = true;
        const char* bot = isSkc ? "SKC" : "SKK";
        geode::log::warn(
            "[DRIFT] {} first exact RECORD/PLAY mismatch at frame {}: "
            "P1 rec=({:.9g},{:.9g}) vy={:.17g} play=({:.9g},{:.9g}) vy={:.17g}; "
            "P2 present rec={} play={} rec=({:.9g},{:.9g}) vy={:.17g} "
            "play=({:.9g},{:.9g}) vy={:.17g}",
            bot, frame, reference.p1x, reference.p1y, reference.p1vy,
            now.p1x, now.p1y, now.p1vy, reference.hasP2, now.hasP2,
            reference.p2x, reference.p2y, reference.p2vy,
            now.p2x, now.p2y, now.p2vy);
    }
};

DriftChecker& driftChecker() {
    static DriftChecker checker;
    return checker;
}

struct Writer {
    std::ofstream file;
    std::array<uint8_t, kFlushRows * kRowBytes> buffer{};
    uint32_t buffered = 0;
    uint32_t rows = 0;
    uint32_t attempts = 1;
    Mode mode = Mode::None;
    int session = -1;
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point started{};

    static void put16(uint8_t* out, uint16_t value) {
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>(value >> 8);
    }
    static void put32(uint8_t* out, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            out[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    static void putFloat(uint8_t* out, float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        put32(out, bits);
    }
    static void putDouble(uint8_t* out, double value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (unsigned i = 0; i < 8; ++i)
            out[i] = static_cast<uint8_t>(bits >> (i * 8));
    }

    void flushRows() {
        if (file && buffered) {
            file.write(reinterpret_cast<const char*>(buffer.data()),
                       static_cast<std::streamsize>(buffered) * kRowBytes);
            file.flush();
            buffered = 0;
        }
    }

    void close() {
        flushRows();
        if (file) file.close();
        mode = Mode::None;
        buffered = rows = 0;
    }

    void markAttempt(int frame) {
        flushRows();
        ++attempts;
        geode::log::info(
            "[TRACE] attempt {} begins at row {} (frame reset to {}); earlier "
            "rows are kept",
            attempts, rows, frame);
    }

    void begin(Mode next, bool skc = false) {
        close();
        file.clear();
        session = ReplayEngine::get()->sessionCounter;
        generation = ReplayEngine::get()->macroGeneration;
        mode = next;
        attempts = 1;
        started = std::chrono::steady_clock::now();
        const auto path = paths::logFile(skc
            ? (next == Mode::Record ? "skc-steps-record.bin" : "skc-steps-playback.bin")
            : (next == Mode::Record ? "steps-record.bin" : "steps-playback.bin"));
        file.open(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            mode = Mode::None;
            geode::log::warn("[TRACE] couldn't open step trace output");
            return;
        }

        std::array<uint8_t, 16> header{};
        header[0] = 'S'; header[1] = 'T'; header[2] = 'R'; header[3] = 'C';
        put16(header.data() + 4, kVersion);
        put16(header.data() + 6, static_cast<uint16_t>(kRowBytes));
        putDouble(header.data() + 8, ReplayEngine::get()->runtimeTickRate());
        file.write(reinterpret_cast<const char*>(header.data()), header.size());
        geode::log::info("[TRACE] step trace started: {}",
            skk::pathToUtf8(path));
    }

    void append(GJBaseGameLayer* layer, int frame, unsigned batch) {
        if (!file || rows >= kMaxRows || !layer || !layer->m_player1) return;
        auto* player = layer->m_player1;
        uint8_t* out = buffer.data() + static_cast<size_t>(buffered) * kRowBytes;
        put32(out, static_cast<uint32_t>(std::max(frame, 0)));
        putFloat(out + 4, player->getPositionX());
        putFloat(out + 8, player->getPositionY());
        putFloat(out + 12, player->getRotation());
        putDouble(out + 16, player->m_yVelocity);
        putDouble(out + 24, layer->m_gameState.m_levelTime);
        uint8_t held = 0;
        for (int button = 1; button <= 3; ++button) {
            auto it = player->m_holdingButtons.find(button);
            if (it != player->m_holdingButtons.end() && it->second)
                held |= static_cast<uint8_t>(1u << (button - 1));
        }
        out[32] = held;
        out[33] = player->m_isDead ? 1 : 0;
        put16(out + 34, static_cast<uint16_t>(std::min(batch, 65535u)));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        put32(out + 36, static_cast<uint32_t>(std::max<int64_t>(elapsed, 0)));
        ++buffered;
        ++rows;
        if (buffered == kFlushRows) flushRows();
        if (rows == kMaxRows) {
            flushRows();
            geode::log::info("[TRACE] reached {}-row safety cap", kMaxRows);
        }
    }
};

Writer& writer() {
    static Writer instance;
    return instance;
}

Writer& skcWriter() {
    static Writer instance;
    return instance;
}

struct Journal {
    std::ofstream file;
    int session = -1;
    uint64_t generation = 0;
    uint32_t rows = 0;
    bool failed = false;
    bool dirty = false;
    bool capped = false;

    void finish() {
        if (!dirty || failed || !file.is_open()) return;
        seam_journal::Event end;
        end.kind = seam_journal::Kind::End;
        append(end);
        file.flush();
        dirty = false;
    }
    void begin(bool playback, bool force = false) {
        auto* engine = ReplayEngine::get();
        const int next = engine->sessionCounter;
        if (!force && session == next && generation == engine->macroGeneration) return;
        finish();
        if (file.is_open()) file.close();
        file.clear(); session = next; generation = engine->macroGeneration;
        rows = 0; failed = dirty = capped = false;
        const auto path = paths::logFile(playback ? "skk-state-playback.bin" : "skk-state-record.bin");
        file.open(path, std::ios::binary | std::ios::trunc);
        std::array<uint8_t, 16> header{ 'S', 'K', 'S', 'J' };
        Writer::put16(header.data() + 4, 1);
        Writer::put16(header.data() + 6, seam_journal::kRowBytes);
        Writer::putDouble(header.data() + 8, engine->runtimeTickRate());
        file.write(reinterpret_cast<char const*>(header.data()), header.size());
        if (!file) {
            failed = true;
            geode::log::error("[SEAM-JOURNAL] не удалось открыть {}", skk::pathToUtf8(path));
        } else {
            geode::log::info("[SEAM-JOURNAL] открыт {} session={} generation={} (flat SKK)",
                skk::pathToUtf8(path), session, generation);
        }
    }
    void append(seam_journal::Event const& event) {
        if (failed) return;
        if (rows >= kMaxRows && event.kind != seam_journal::Kind::End) {
            if (capped) return;
            capped = true;
            seam_journal::Event gap;
            gap.kind = seam_journal::Kind::Gap;
            const auto bytes = seam_journal::encode(gap);
            file.write(reinterpret_cast<char const*>(bytes.data()), bytes.size());
            dirty = true;
            geode::log::warn("[SEAM-JOURNAL] достигнут лимит; сверка неполна");
        } else {
            const auto bytes = seam_journal::encode(event);
            file.write(reinterpret_cast<char const*>(bytes.data()), bytes.size());
            ++rows;
            dirty = true;
        }
        if (event.kind != seam_journal::Kind::Sample || rows % kFlushRows == 0) file.flush();
        if (!file) {
            failed = true;
            geode::log::error("[SEAM-JOURNAL] ошибка записи; сверка неполна");
        }
    }
};

Journal& journal(bool playback) {
    static Journal record, play;
    return playback ? play : record;
}

seam_journal::Player journalPlayer(PlayerObject* player) {
    seam_journal::Player p;
    if (!player) return p;
    p.x = player->getPositionX(); p.y = player->getPositionY();
    p.rotation = player->getRotation();
    p.vy = player->m_yVelocity; p.vyb = player->m_yVelocityBeforeSlope;
    for (int button = 1; button <= 3; ++button) {
        auto it = player->m_holdingButtons.find(button);
        if (it != player->m_holdingButtons.end() && it->second) p.held |= 1u << (button - 1);
    }
    p.visual = visualFlagsOf(player);
    if (player->m_dashRing) p.ring = static_cast<GameObject*>(player->m_dashRing)->m_objectID;
    p.flags = (player->m_isDead ? 1u : 0u) | (player->m_isOnGround ? 2u : 0u)
        | (player->m_jumpBuffered ? 4u : 0u) | (player->m_wasJumpBuffered ? 8u : 0u);
    return p;
}

void journalEvent(seam_journal::Kind kind, GJBaseGameLayer* layer, int frame, bool playback = false) {
    auto* engine = ReplayEngine::get();
    if (!g_enabledCache || !engine->skMode || !engine->activeSk ||
        engine->canUsePersistenceCapture() || engine->isPersistencePlaybackActive()) return;
    if (kind == seam_journal::Kind::Sample && !playback) {
        const auto* macro = engine->captureStaging ? engine->captureStaging : engine->activeSk;
        if (engine->captureDeathPending || engine->levelRestarting || !layer ||
            !layer->m_player1 || layer->m_player1->m_isDead ||
            (layer->m_player2 && layer->m_player2->m_isDead) ||
            macro->perFrames.empty() || macro->perFrames.back().tick != frame) return;
    }
    auto& out = journal(playback);
    out.begin(playback);
    seam_journal::Event event;
    event.kind = kind; event.tick = frame;
    const auto& trace = writer();
    if (trace.mode == (playback ? Mode::Playback : Mode::Record) &&
        trace.session == engine->sessionCounter && trace.generation == engine->macroGeneration) {
        event.traceRow = trace.rows ? trace.rows - (kind == seam_journal::Kind::Sample ? 1 : 0) : 0;
    } else {
        event.traceRow = UINT32_MAX;
    }
    if (layer) {
        event.time = layer->m_gameState.m_levelTime;
        event.active = (layer->m_player1 ? 1u : 0u)
            | (layer->m_player2 && layer->m_player2->getParent() ? 2u : 0u);
        event.p1 = journalPlayer(layer->m_player1);
        if (event.active & 2u) event.p2 = journalPlayer(layer->m_player2);
    }
    out.append(event);
}

}

void record(GJBaseGameLayer* layer, int frame, unsigned batch, bool captureStep) {
    auto& out = writer();
    if (!g_enabledCache || !layer) {
        if (out.mode != Mode::None) out.close();
        journal(false).finish();
        journal(true).finish();
        return;
    }
    auto* engine = ReplayEngine::get();
    Mode mode = Mode::None;
    if (engine->engineMode == MODE_CAPTURE) mode = Mode::Record;
    else if (engine->engineMode == MODE_EXECUTE) mode = Mode::Playback;
    if (mode == Mode::None) {
        if (out.mode != Mode::None) out.close();
        journal(false).finish();
        journal(true).finish();
        return;
    }
    if (out.mode != mode || out.session != engine->sessionCounter || out.generation != engine->macroGeneration) {
        journal(false).finish();
        journal(true).finish();
        out.begin(mode);
        if (engine->skMode && !engine->canUsePersistenceCapture() && !engine->isPersistencePlaybackActive())
            journal(mode == Mode::Playback).begin(mode == Mode::Playback, mode == Mode::Playback);
    } else if (frame <= 1 && out.rows > 2) {
        out.markAttempt(frame);
    }
    const auto before = out.rows;
    out.append(layer, frame, batch);
    if (out.rows != before && (mode == Mode::Playback || captureStep))
        journalEvent(seam_journal::Kind::Sample, layer, frame, mode == Mode::Playback);
    if (out.rows != before && out.rows == kMaxRows && engine->skMode &&
        !engine->canUsePersistenceCapture() && !engine->isPersistencePlaybackActive()) {
        seam_journal::Event gap;
        gap.kind = seam_journal::Kind::Gap;
        journal(mode == Mode::Playback).append(gap);
    }
}

void seal(GJBaseGameLayer* layer, int frame) { journalEvent(seam_journal::Kind::Seal, layer, frame); }
void trim(int frame) { journalEvent(seam_journal::Kind::Trim, nullptr, frame); }
void wipe() { journalEvent(seam_journal::Kind::Wipe, nullptr, 0); }

void recordSkc(GJBaseGameLayer* layer, int frame, unsigned batch,
               bool recording, bool playback) {
    auto& out = skcWriter();
    if (!g_enabledCache || !layer) {
        if (out.mode != Mode::None) out.close();
        return;
    }
    const Mode mode = recording ? Mode::Record : playback ? Mode::Playback : Mode::None;
    if (mode == Mode::None) {
        if (out.mode != Mode::None) out.close();
        return;
    }
    if (out.mode != mode) {
        out.begin(mode, true);
    } else if (frame <= 1 && out.rows > 2) {
        out.markAttempt(frame);
    }
    out.append(layer, frame, batch);
}

void checkDrift(GJBaseGameLayer* layer, int frame, bool recording,
                bool playback, bool skc) {
    driftChecker().check(layer, frame, recording, playback, skc);
}

void finish() {
    journal(false).finish();
    journal(true).finish();
    writer().close();
    skcWriter().close();
}

bool enabled() {
    return g_enabledCache;
}

void refreshCache() {
    const bool next = geode::Mod::get()->getSavedValue<bool>("gui_step_trace", false);
    if (g_enabledCache && !next) {
        for (bool playback : {false, true}) {
            auto& out = journal(playback);
            if (!out.dirty) continue;
            seam_journal::Event gap;
            gap.kind = seam_journal::Kind::Gap;
            out.append(gap);
        }
        finish();
    }
    g_enabledCache = next;
    if (ReplayEngine::get()->engineMode == MODE_DISABLED) {
        journal(false).finish();
        journal(true).finish();
        writer().close();
    }
}

}
