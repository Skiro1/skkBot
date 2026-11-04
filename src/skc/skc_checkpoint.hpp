#pragma once

#include "core/checkpoint_handler.hpp"
#include "format/replay.hpp"

#include <Geode/Geode.hpp>
#include <optional>
#include <vector>

namespace skk::skc {

// ---------------------------------------------------------------------------
// SKC practice fix = SKK's practice fix.
//
// SKC used to carry its own hand-rolled PracticeFix: a FullPlayerCheckpoint per
// player, a sparse variance map, direct writes to the GD RNG globals and a
// manual pushButton loop. Every one of those was a weaker copy of something
// SKK's CheckpointStateManager already does properly - and SKK's copy carries
// fixes that the hand-rolled one did not have at all:
//
//   - BUG-13: without m_shipRotation / m_rotationSpeed a ship respawn starts
//     LEVEL instead of banked, so the trajectory diverges from that moment on.
//   - BUG-18: FullPlayerCheckpoint's colour fields default to {0,0,0}, so
//     applying an uncaptured P2 paints the player completely BLACK.
//   - the latch-mask restore (InputStateRestorer), which is what makes a hold
//     that spans a checkpoint survive at all.
//   - variance rewind WITH the zeroing pass, broken-object re-disable and EMS
//     persistent items.
//
// SKC stores no physics of its own, so on a practice respawn these fields are
// the ONLY thing standing between the replay and a desync. Reimplementing them
// was strictly worse than calling the thing that already works. This class is
// now a thin adapter: it keeps the two keys SKC needs (its own FrameClock frame
// for trimming, and the checkpoint progress for matching) and delegates every
// single capture and restore to CheckpointStateManager.
//
// What CANNOT be reused is ReplayEngine::truncateRecordedDataAfter /
// closeSeamHoldEdges: they operate on SkInput and activeSk->inputs, SKK's own
// stream types. SKC's Action list is a different structure, so the truncation
// rule is mirrored in Bot::onResetEvent - but the rule itself is SKK's
// (checkpoint_system.cpp:537-563), and the state restore above is SKK's code.
// ---------------------------------------------------------------------------
struct SkcCheckpoint {
    // Keep the GD object identity as well as its time/progress key. Platformer
    // setup-checkpoints are restored through m_activatedCheckpoint and can
    // share a levelTime key with a different synthetic checkpoint object.
    CheckpointObject* object = nullptr;
    // FrameClock frame at capture - SKC's own clock, the unit recordInput
    // stamps events with, so trim/seek bounds are in the recording's units.
    uint32_t frame = 0;

    // The checkpoint's m_currentProgress/2 at capture. The only stable identity
    // a CheckpointObject carries, used to match a restore back to its snapshot.
    // Valid for classic levels; in platformer mode progress is x-based and
    // non-monotone (walking left moves it backwards), so two different
    // checkpoints can share it - use timeFrame there instead.
    uint32_t progressFrame = 0;

    // The checkpoint's m_levelTime converted to the recording's frame grid.
    // m_levelTime is monotone within an attempt and is restored verbatim with
    // the CheckpointObject, so this identifies the snapshot exactly even when
    // the player backtracks. Filled for every capture, used only in platformer.
    uint32_t timeFrame = 0;

    // Mode the snapshot was captured in. Selects which identity key apply()
    // compares; stored per snapshot so the decision does not depend on the
    // level the macro is later played on.
    bool platformer = false;

    // The actual payload: SKK's bundle, verbatim.
    CheckpointStateBundle state;
};

class PracticeFix {
public:
    static PracticeFix& get();

    // Capture via SKK's CheckpointStateManager, while the physics is still live.
    // Called from the storeCheckpoint hook while recording.
    void capture(uint32_t frame, PlayLayer* playLayer, CheckpointObject* checkpoint);
    void trackPlatformerCheckpoint(CheckpointObject* checkpoint);
    [[nodiscard]] CheckpointObject* latestPlatformerCheckpoint() const;
    [[nodiscard]] size_t getPlatformerCheckpointCount() const {
        return m_platformerCheckpoints.size();
    }

    // Match the snapshot to the checkpoint GD is restoring and hand it to SKK's
    // CheckpointStateManager::restore. Returns the FrameClock frame of the
    // matched snapshot (the trim boundary), or std::nullopt when nothing
    // matches.
    [[nodiscard]] std::optional<uint32_t> apply(
        PlayLayer* playLayer, CheckpointObject* checkpoint);

    void clear();

    // Truncation bound: the FrameClock frame of the most recent snapshot.
    [[nodiscard]] uint32_t lastCheckpointFrame() const;

    [[nodiscard]] size_t getSnapshotCount() const { return m_checkpoints.size(); }

    // Seams crossed since the last clear(). A run with N snapshots should show
    // exactly N restores; a level replayed from the top must not show fewer.
    // Reported at the end of a playback so a multi-checkpoint run is verifiable
    // from the log without reading every line.
    [[nodiscard]] uint32_t seamCount() const { return m_seams; }

private:
    PracticeFix() = default;

    // Ordered by capture time; apply() searches backwards for the newest match.
    std::vector<SkcCheckpoint> m_checkpoints;
    std::vector<CheckpointObject*> m_platformerCheckpoints;
    uint32_t m_seams = 0;
};

} // namespace skk::skc
