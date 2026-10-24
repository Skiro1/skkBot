#include "skc_checkpoint.hpp"

#include "skkBot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <logger/logger.hpp>

using namespace geode::prelude;

namespace skk::skc {

PracticeFix& PracticeFix::get() {
    static PracticeFix instance;
    return instance;
}

void PracticeFix::capture(uint32_t frame, PlayLayer* playLayer,
                          CheckpointObject* checkpoint) {
    if (!playLayer) return;

    // ReplayEngine owns the lock bookkeeping (fast_srand state, locked flag,
    // seed, teleport chain). SKK's own capture call reads it from there
    // (checkpoint_system.cpp:1127-1132) - do the same instead of touching the
    // GD globals directly, or the checkpoint would capture RNG that does not
    // match what playback will actually restore against.
    auto* engine = ReplayEngine::get();
    const AnchorRngState rng = engine ? engine->captureRngState()
                                      : AnchorRngState{};

    SkcCheckpoint cp;
    // Do NOT add +1 here. SKK does (`checkpoint_system.cpp:1133-1134`) because
    // it derives its tick from `m_levelTime * tickRate`, which at the capture
    // point already reads one behind. SKC's FrameClock ticks after the physics
    // step, so at the same point it already reads the frame the snapshot
    // belongs to. Adding +1 on top double-counts and throws the retry two frames
    // ahead - measured: dx went from 0 to exactly two frames of movement.
    //
    // The snapshot is taken before that frame's physics step runs, so the state
    // it holds is the end of frame `frame - 1`, and the retry continues from
    // `frame - 1` (see onResetEvent / loadFromCheckpoint).
    cp.frame = frame;
    cp.progressFrame =
        checkpoint ? checkpoint->m_gameState.m_currentProgress / 2 : 0;
    cp.state = CheckpointStateManager::capture(
        static_cast<int>(frame), -1, playLayer, rng);

    // Log before the move - afterwards the object is moved-from.
    // info for the same reason as the restore line above: the per-seam snapshot
    // record is what a multi-checkpoint verification reads back.
    skkbot2::log::info(
        "[SKC] practice snapshot via SKK CheckpointStateManager: frame={} "
        "progress={} hasFull={} fullP2={} varianceSlots={}",
        cp.frame, cp.progressFrame, cp.state.hasFullState,
        cp.state.hasFullPlayer2, cp.state.variance.size());
    ++m_seams;

    m_checkpoints.push_back(std::move(cp));
}

std::optional<uint32_t> PracticeFix::apply(
    PlayLayer* playLayer, CheckpointObject* checkpoint) {

    if (!playLayer || m_checkpoints.empty())
        return std::nullopt;

    // Match the snapshot to the checkpoint GD is restoring. Take the LAST match:
    // a checkpoint placed twice at the same progress must use the newest
    // capture, and the vector is append-ordered.
    const uint32_t targetProgress =
        checkpoint ? checkpoint->m_gameState.m_currentProgress / 2 : 0;

    SkcCheckpoint const* best = nullptr;
    for (const auto& cp : m_checkpoints) {
        if (cp.progressFrame == targetProgress) {
            best = &cp;
        }
    }
    if (!best) return std::nullopt;

    // Deliberately DO NOT call CheckpointStateManager::restore here.
    //
    // GD's own loadFromCheckpoint (already called by our hook, above) restores
    // the player exactly, and that is all an input-only format needs: Xenon -
    // also input-only - does precisely this and nothing else
    // (Xenon-master/src/checkpoint/checkpoint.cpp:26-31), only nudging its frame
    // counter.
    //
    // Overlaying SKK's full-state bundle on top used to be "more correct", but
    // it is what put SKC at dx=0 with a stubborn y-only drift: the bundle
    // rewrites ~200 player fields, and any one of them landing a hair off
    // (slope state, ground contact, yVelocity family) bends the vertical
    // dynamics while x - a pure clock - stays perfect. The drift then survived
    // every frame-number correction, because the frame was never the problem.
    //
    // SKK's own restore path is untouched and still used by SKK itself.
    // info, not debug: seam crossings are the thing a multi-checkpoint run has to
    // be verified against, and debug output does not reach the log file at all,
    // so a level with three checkpoints produced a log with zero mention of any
    // of them. One line per seam is what makes "did all three join cleanly"
    // answerable from the log afterwards.
    skkbot2::log::info(
        "[SKC] practice restore (vanilla state, SKK bundle NOT applied): "
        "frame={} progress={} snapshots={}",
        best->frame, best->progressFrame, m_checkpoints.size());

    return best->frame;
}

void PracticeFix::clear() {
    m_checkpoints.clear();
    m_seams = 0;
}

uint32_t PracticeFix::lastCheckpointFrame() const {
    if (m_checkpoints.empty()) return 0;
    return m_checkpoints.back().frame;
}

} // namespace skk::skc