#pragma once

#include "format/skk_format.hpp"
#include "format/replay.hpp"
#include <vector>

namespace skk::bridge {

SkPracticeFixCheckpoint bundleToWire(
    CheckpointStateBundle const& bundle,
    double tps
);

CheckpointStateBundle wireToBundle(
    SkPracticeFixCheckpoint const& wire,
    double tps
);

std::vector<SkPracticeFixCheckpoint> captureRestorePointsToWire(
    std::unordered_map<CheckpointObject*, CheckpointStateBundle> const& restorePoints,
    double tps
);

std::vector<CheckpointStateBundle> wireToBundles(
    std::vector<SkPracticeFixCheckpoint> const& wires,
    double tps
);

}
