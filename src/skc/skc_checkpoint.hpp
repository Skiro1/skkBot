#pragma once

#include "core/checkpoint_handler.hpp"
#include "format/replay.hpp"

#include <Geode/Geode.hpp>
#include <optional>
#include <vector>

namespace skk::skc {

struct SkcCheckpoint {
    CheckpointObject* object = nullptr;
    uint32_t frame = 0;

    uint32_t progressFrame = 0;

    uint32_t timeFrame = 0;

    bool platformer = false;

    CheckpointStateBundle state;
};

class PracticeFix {
public:
    static PracticeFix& get();

    void capture(uint32_t frame, PlayLayer* playLayer, CheckpointObject* checkpoint,
                 CheckpointStateBundle state);
    [[nodiscard]] uint64_t generation() const { return m_generation; }
    void trackPlatformerCheckpoint(CheckpointObject* checkpoint);
    [[nodiscard]] CheckpointObject* latestPlatformerCheckpoint() const;
    [[nodiscard]] size_t getPlatformerCheckpointCount() const {
        return m_platformerCheckpoints.size();
    }

    [[nodiscard]] std::optional<uint32_t> apply(
        PlayLayer* playLayer, CheckpointObject* checkpoint);

    void clear();
    void syncStoredCheckpoints(PlayLayer* playLayer);
    [[nodiscard]] std::optional<uint32_t> frameFor(CheckpointObject* checkpoint) const;

    [[nodiscard]] uint32_t lastCheckpointFrame() const;

    [[nodiscard]] size_t getSnapshotCount() const { return m_checkpoints.size(); }

    [[nodiscard]] uint32_t seamCount() const { return m_seams; }

private:
    PracticeFix() = default;

    std::vector<SkcCheckpoint> m_checkpoints;
    std::vector<CheckpointObject*> m_platformerCheckpoints;
    uint32_t m_seams = 0;
    uint64_t m_generation = 0;
};

}
