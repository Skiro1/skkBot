#ifndef _skk_playback_align_hpp
#define _skk_playback_align_hpp

#include "skkBot.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace skk::playback_align {

    inline constexpr float kPlaybackTightTolerance = 8.0f;
    inline constexpr float kPlaybackTakeoverYTolerance = 24.0f;

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

    inline int playbackBaselineTick(GJBaseGameLayer* layer, ReplayEngine* engine) {
        if (engine->stepClock) {
            return std::max(0, engine->stepPairCount + 1);
        }
        return computePlaybackTick(layer);
    }
}

#endif