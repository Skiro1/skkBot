#pragma once

#include "format/replay.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>

using namespace geode::prelude;

class InputStateRestorer {
public:
    static uint8_t captureLatchMask(PlayerObject* player);
    static void restoreInputState(GJBaseGameLayer* playLayer, PlaybackAnchor const& anchor);
    static void restoreInputState(GJBaseGameLayer* playLayer, CheckpointStateBundle const& checkpoint);
};

class PlayerStateRestorer {
public:
    static PlayerStateBundle captureState(PlayerObject* player, bool isPlatformer, bool isDual, bool isTwoPlayer);
    static void restoreState(PlayerObject* player, PlayerStateBundle const& state);

    static float positionalDrift(PlayerObject* player, PlayerStateBundle const& state);
    static float rotationDrift(PlayerObject* player, PlayerStateBundle const& state);
    static float velocityDrift(PlayerObject* player, PlayerStateBundle const& state);
    static bool needsReconciliation(
        PlayerObject* player,
        PlayerStateBundle const& state,
        float positionTolerance = 0.01f,
        float rotationTolerance = 0.1f,
        float velocityTolerance = 0.01f
    );
    // Structural divergence only (mode flags / gravity orientation / death):
    // used when the per-frame stream owns position+velocity, so GD keeps its
    // own visual state machines (dash-ring hold, spider flip) intact.
    static bool needsModeReconciliation(
        PlayerObject* player,
        PlayerStateBundle const& state
    );
};

class AnchorReconciler {
public:
    static PlaybackAnchor captureAnchor(
        int tick,
        GJBaseGameLayer* playLayer,
        PlayerObject* player1,
        PlayerObject* player2,
        AnchorRngState const& rng
    );

    static bool reconcile(
        GJBaseGameLayer* playLayer,
        PlayerObject* player1,
        PlayerObject* player2,
        PlaybackAnchor const& anchor,
        bool force = false,
        bool modeOnly = false
    );
};

class CheckpointStateManager {
public:
    static CheckpointStateBundle capture(
        int tick,
        int priorTick,
        GJBaseGameLayer* playLayer,
        AnchorRngState const& rng
    );

    static void restore(GJBaseGameLayer* playLayer, CheckpointStateBundle const& checkpoint);
};
