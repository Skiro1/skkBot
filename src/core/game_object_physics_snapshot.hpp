#ifndef _game_object_physics_snapshot_hpp
#define _game_object_physics_snapshot_hpp


#include <Geode/Geode.hpp>
#include <Geode/binding/GameObjectPhysics.hpp>

#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skk {

struct GameObjectPhysicsSnapshot {
    GameObject* gameObject = nullptr;
    cocos2d::CCPoint velocity;
    cocos2d::CCPoint direction;
    float targetAngle = 0.f;
    float turnSpeed = 0.f;
    int unkInt1 = 0;
    int lastUpdateStep = 0;
    int exclusiveStep = 0;
};

static_assert(sizeof(GameObjectPhysicsSnapshot) == sizeof(GameObjectPhysics),
    "GameObjectPhysicsSnapshot layout must match the 2.2091 binding");

using GameObjectPhysicsSnapshotMap = std::vector<std::pair<int, GameObjectPhysicsSnapshot>>;

inline void captureGameObjectPhysics(
    std::unordered_map<int, GameObjectPhysics> const& source,
    GameObjectPhysicsSnapshotMap& out) {
    out.clear();
    out.reserve(source.size());
    for (auto const& entry : source) {
        GameObjectPhysicsSnapshot snap;
        std::memcpy(&snap, &entry.second, sizeof(GameObjectPhysics));
        out.emplace_back(entry.first, snap);
    }
}

inline void restoreGameObjectPhysics(
    GameObjectPhysicsSnapshotMap const& snapshot,
    std::unordered_map<int, GameObjectPhysics>& destination) {
    destination.clear();
    for (auto const& entry : snapshot) {
        auto [it, inserted] = destination.try_emplace(entry.first);
        if (inserted) {
            std::memcpy(&it->second, &entry.second, sizeof(GameObjectPhysics));
        }
    }
}

}

#endif
