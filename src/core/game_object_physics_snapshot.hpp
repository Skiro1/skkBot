#ifndef _game_object_physics_snapshot_hpp
#define _game_object_physics_snapshot_hpp

// GD 2.2091 / Geode v6: биндинговый GameObjectPhysics содержит анонимные
// union-ы с cocos2d::CCPoint, у которого копирующий конструктор пользовательский
// (нетривиальный). По стандарту это делает копирование/перемещение самого
// GameObjectPhysics УДАЛЁННЫМ, поэтому std::unordered_map<int, GameObjectPhysics>
// нельзя ни копировать, ни перемещать (ни map, ни pair) — снапшоты чекпоинтов
// падают компиляцией.
//
// Решение: храним снимок в тривиально копируемом зеркале (layout идентичен
// биндинговому типу, проверяется static_assert), а в GD-карту пишем через
// try_emplace (конструирование на месте — дефолтный конструктор жив) +
// memcpy значения (тип стандарт-layout, тривиально разрушаемый; так же
// делают Silicate/Xenon для непереносимых полей).

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

// Снять копию GD-карты в тривиально копируемое зеркало.
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

// Восстановить GD-карту из зеркала. Значения конструируются на месте
// (try_emplace), затем поверх пишутся байты снимка.
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

} // namespace skk

#endif
