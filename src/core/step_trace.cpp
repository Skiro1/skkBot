#include "step_trace.hpp"

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
constexpr size_t kDriftRingSize = 1u << 17;

enum class Mode : uint8_t { None, Record, Playback };

// Cached "gui_step_trace" flag. Game-thread only (the scheduler hook and the
// per-step hooks all run there). refreshCache() is called once per drawn
// frame; the per-substep paths read this bool instead of paying a geode
// getSavedValue JSON lookup 3414 times a second at high TPS.
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
    std::array<DriftProbe, kDriftRingSize> skk{};
    std::array<DriftProbe, kDriftRingSize> skc{};
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

        // A new recording establishes a fresh reference. Switching to playback
        // preserves it; a new playback merely allows a new first-mismatch report.
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

    // A new ATTEMPT inside the same session restarts the level at frame 0. That
    // used to call begin() again, which truncated the file and destroyed the
    // rows of the attempt that just died - exactly the rows a "why did it die at
    // frame 367" investigation needs. A frame reset is a bookmark now: flush
    // what we have, note the row index, and keep appending. The frame column in
    // each row is what tells the attempts apart (frame drops back to 0).
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

        // Fixed 16-byte little-endian file header: magic, version, row size,
        // then the active TPS as float64. The filename identifies mode.
        std::array<uint8_t, 16> header{};
        header[0] = 'S'; header[1] = 'T'; header[2] = 'R'; header[3] = 'C';
        put16(header.data() + 4, kVersion);
        put16(header.data() + 6, static_cast<uint16_t>(kRowBytes));
        putDouble(header.data() + 8, ReplayEngine::get()->runtimeTickRate());
        // TPS occupies bytes 8..15; mode is encoded by the filename.
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

} // namespace

void record(GJBaseGameLayer* layer, int frame, unsigned batch) {
    auto& out = writer();
    if (!g_enabledCache || !layer) {
        if (out.mode != Mode::None) out.close();
        return;
    }
    auto* engine = ReplayEngine::get();
    Mode mode = Mode::None;
    if (engine->engineMode == MODE_CAPTURE) mode = Mode::Record;
    else if (engine->engineMode == MODE_EXECUTE) mode = Mode::Playback;
    if (mode == Mode::None) {
        if (out.mode != Mode::None) out.close();
        return;
    }
    if (out.mode != mode) {
        out.begin(mode);
    } else if (frame <= 1 && out.rows > 2) {
        out.markAttempt(frame);
    }
    out.append(layer, frame, batch);
}

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
    writer().close();
    skcWriter().close();
}

bool enabled() {
    return g_enabledCache;
}

void refreshCache() {
    g_enabledCache = geode::Mod::get()->getSavedValue<bool>("gui_step_trace", false);
}

} // namespace skkbot2::step_trace
