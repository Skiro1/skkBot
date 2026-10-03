#ifndef _skk_playback_align_hpp
#define _skk_playback_align_hpp

#include "skkBot.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace skk::playback_align {

    // Position coverage tolerances shared by the armed-waiting takeover and the
    // reset-spawn coverage gate (also used by the editor playtest attempt arm).
    inline constexpr float kPlaybackTightTolerance = 8.0f;
    inline constexpr float kPlaybackTakeoverYTolerance = 24.0f;

    // Level-time label for the given gameplay layer at the current physics
    // boundary. Works for both PlayLayer and an editor playtest (LevelEditorLayer
    // is a GJBaseGameLayer with the same gameState).
    inline int computePlaybackTick(GJBaseGameLayer* layer) {
        if (!layer) {
            return 0;
        }

        auto* engine = ReplayEngine::get();
        int tick = static_cast<int>(layer->m_gameState.m_levelTime * engine->runtimeTickRate());
        return std::max(0, tick + 1);
    }

    inline size_t findFirstInputAtTick(std::vector<MacroAction>& inputs, int tick) {
        auto it = std::lower_bound(inputs.begin(), inputs.end(), tick, [](MacroAction const& action, int targetTick) {
            return static_cast<int>(action.frame) < targetTick;
        });
        return static_cast<size_t>(std::distance(inputs.begin(), it));
    }

    inline size_t findFirstSkInputAtTick(std::vector<SkInput>& inputs, int tick) {
        auto it = std::lower_bound(inputs.begin(), inputs.end(), tick, [](SkInput const& input, int targetTick) {
            return input.tick < targetTick;
        });
        return static_cast<size_t>(std::distance(inputs.begin(), it));
    }

    inline size_t findFirstAnchorAtTick(std::vector<PlaybackAnchor> const& anchors, int tick) {
        auto it = std::lower_bound(anchors.begin(), anchors.end(), tick, [](PlaybackAnchor const& anchor, int targetTick) {
            return anchor.tick < targetTick;
        });
        return static_cast<size_t>(std::distance(anchors.begin(), it));
    }

    inline size_t findFirstPerFrameAtTick(std::vector<SkPerFrame> const& frames, int tick) {
        auto it = std::lower_bound(frames.begin(), frames.end(), tick, [](SkPerFrame const& frame, int targetTick) {
            return frame.tick < targetTick;
        });
        return static_cast<size_t>(std::distance(frames.begin(), it));
    }

    // Non-standard-start support: find the recorded tick whose p1 X coordinate
    // best matches the reference X (where the character currently spawns/stands).
    // GD levels advance along X, so X is the alignment axis; Y at the same X
    // varies across attempts (jump height/phase), so it is deliberately ignored.
    // Returns -1 when the recording has NO frame within `tolerance` of refX -
    // i.e. the macro never covered that spot and there is nothing to align to.
    inline int matchRecordedTickByPosition(std::vector<SkPerFrame> const& frames, float refX, float tolerance) {
        if (frames.empty()) {
            return -1;
        }
        int bestTick = -1;
        float bestDx = FLT_MAX;
        for (auto const& frame : frames) {
            float dx = std::abs(frame.p1x - refX);
            if (dx < bestDx && dx <= tolerance) {
                bestDx = dx;
                bestTick = frame.tick;
            }
        }
        return bestTick;
    }

    inline int matchRecordedAnchorTickByPosition(std::vector<PlaybackAnchor> const& anchors, float refX, float tolerance) {
        if (anchors.empty()) {
            return -1;
        }
        int bestTick = -1;
        float bestDx = FLT_MAX;
        for (auto const& anchor : anchors) {
            float dx = std::abs(anchor.player1.motion.position.x - refX);
            if (dx < bestDx && dx <= tolerance) {
                bestDx = dx;
                bestTick = anchor.tick;
            }
        }
        return bestTick;
    }

    // Combined X+Y proximity match for a LIVE takeover. The player is mid-flight
    // at (refX, refY), so matching X alone could snap them vertically to a
    // recorded frame whose Y at the same X is far away. Both deltas bound the
    // visual jump the takeover is allowed to make. Returns the tick of the
    // closest frame within both tolerances (min |dx| wins), or -1.
    inline int matchRecordedTickByPositionXY(std::vector<SkPerFrame> const& frames,
                                             float refX, float refY,
                                             float xTolerance, float yTolerance) {
        if (frames.empty()) {
            return -1;
        }
        int bestTick = -1;
        float bestDx = FLT_MAX;
        for (auto const& frame : frames) {
            float dx = std::abs(frame.p1x - refX);
            if (dx > xTolerance) {
                continue;
            }
            float dy = std::abs(frame.p1y - refY);
            if (dy > yTolerance) {
                continue;
            }
            if (dx < bestDx) {
                bestDx = dx;
                bestTick = frame.tick;
            }
        }
        return bestTick;
    }

    inline int matchRecordedAnchorTickByPositionXY(std::vector<PlaybackAnchor> const& anchors,
                                                   float refX, float refY,
                                                   float xTolerance, float yTolerance) {
        if (anchors.empty()) {
            return -1;
        }
        int bestTick = -1;
        float bestDx = FLT_MAX;
        for (auto const& anchor : anchors) {
            float dx = std::abs(anchor.player1.motion.position.x - refX);
            if (dx > xTolerance) {
                continue;
            }
            float dy = std::abs(anchor.player1.motion.position.y - refY);
            if (dy > yTolerance) {
                continue;
            }
            if (dx < bestDx) {
                bestDx = dx;
                bestTick = anchor.tick;
            }
        }
        return bestTick;
    }

    // Start-pos alignment: nearest pose the recording actually covers, within
    // the same tight window as the coverage gate (kPlaybackTightTolerance /
    // kPlaybackTakeoverYTolerance). The previous 50px-euclidean search returned
    // the EARLIEST in-window anchor (tick 187 at (285.8,2975.3) for a spawn at
    // (315,3015)) even when an ~matching pose existed right at the spawn, and
    // its offset was measured from the game-time label instead of the step-clock
    // baseline - a negative offset put playback into a blind pre-roll before the
    // recording's first frame and the "activated" bot silently fed nothing.
    inline int matchStartPosTargetTick(ReplayEngine* engine,
                                       std::vector<PlaybackAnchor> const& anchors,
                                       float refX, float refY) {
        if (auto* frames = engine->activePerFrames(); frames && !frames->empty()) {
            int tick = matchRecordedTickByPositionXY(
                *frames, refX, refY, kPlaybackTightTolerance, kPlaybackTakeoverYTolerance);
            if (tick >= 0) {
                return tick;
            }
        }
        if (!anchors.empty()) {
            return matchRecordedAnchorTickByPositionXY(
                anchors, refX, refY, kPlaybackTightTolerance, kPlaybackTakeoverYTolerance);
        }
        return -1;
    }

    // Live step-clock baseline for position alignment: the tick the engine will
    // use for the very next physics step (effectiveTick = stepTick + tickOffset).
    // At the reset moment the step clock was just re-armed, so a fresh attempt
    // sits at step 1 - NOT the game-time label (which diverges from the step
    // grid after restarts). Works for PlayLayer resets and editor playtests alike.
    inline int playbackBaselineTick(GJBaseGameLayer* layer, ReplayEngine* engine) {
        if (engine->stepClock) {
            return std::max(0, engine->stepPairCount + 1);
        }
        return computePlaybackTick(layer);
    }
}

#endif