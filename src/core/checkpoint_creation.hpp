#pragma once

#include "core/checkpoint_handler.hpp"
#include <Geode/modify/CheckpointObject.hpp>
#include <optional>

// BUG-141: GD 2.2091 хранит m_pendingCheckpoint до позднего storeCheckpoint.
// Данные живут на самом объекте: отменённый pending освобождает и снимок,
// повторно выделенный по тому же адресу объект не наследует старое состояние.
class $modify(SkkCheckpointCreation, CheckpointObject) {
public:
    struct Snapshot {
        uint64_t generation = 0;
        uint32_t frame = 0;
        CheckpointStateBundle state;
    };
    struct Fields {
        std::optional<Snapshot> skc;
        std::optional<Snapshot> skk;
    };
};
