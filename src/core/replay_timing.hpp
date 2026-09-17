#pragma once

#include "core/accuracy_mode.hpp"

#include <algorithm>
#include <cmath>

namespace skk::replay_timing {

enum class ExactInputDispatch {
    Immediate,
    QueueNative,
    Wait,
};

enum class QueuedInputPlayerMatch {
    Exact,
    PreferInverted,
};

inline bool sameTimestampSlice(double lhs, double rhs, double epsilon = 0.000000001) {
    if (!std::isfinite(lhs) || !std::isfinite(rhs)) {
        return lhs == rhs;
    }
    return std::abs(lhs - rhs) <= epsilon;
}

inline bool shouldSkipRepeatedProcessSlice(
    bool timedPlayback,
    int lastTick,
    int tick,
    bool hasMacro,
    int lastStepDelta,
    int stepDelta,
    double lastTimestampSlice,
    double timestampSlice
) {
    if (lastTick != tick || tick == 0 || !hasMacro) {
        return false;
    }
    if (!timedPlayback) {
        return true;
    }
    return lastStepDelta == stepDelta && sameTimestampSlice(lastTimestampSlice, timestampSlice);
}

inline bool shouldReconcileAnchorsBeforePhysics(bool stepBasedPlayback) {
    static_cast<void>(stepBasedPlayback);
    return false;
}

inline bool shouldReconcileAnchorsAfterPhysics(bool stepBasedPlayback) {
    static_cast<void>(stepBasedPlayback);
    return true;
}

inline bool shouldTrustHandleTimestampFallback(AccuracyMode mode) {
    return mode == AccuracyMode::CBS;
}

inline bool shouldSuppressSubstepTrail(bool substepMidStep, bool substepSuppressTrail) {
    return substepMidStep && substepSuppressTrail;
}

template <typename Iterator>
inline Iterator findQueuedInputMatch(
    Iterator begin,
    Iterator end,
    int button,
    bool down,
    bool player2,
    QueuedInputPlayerMatch playerMatch,
    bool allowUniqueButtonFallback
) {
    auto matchesButton = [&](auto const& queued) {
        return queued.button == button && queued.down == down;
    };
    auto findForPlayer = [&](bool wantedPlayer2) {
        return std::find_if(begin, end, [&](auto const& queued) {
            return matchesButton(queued) && queued.player2 == wantedPlayer2;
        });
    };

    if (playerMatch == QueuedInputPlayerMatch::PreferInverted) {
        auto inverted = findForPlayer(!player2);
        if (inverted != end) {
            return inverted;
        }
    }

    auto exact = findForPlayer(player2);
    if (exact != end) {
        return exact;
    }

    if (allowUniqueButtonFallback) {
        Iterator candidate = end;
        int matchCount = 0;
        for (auto it = begin; it != end; ++it) {
            if (!matchesButton(*it)) {
                continue;
            }
            candidate = it;
            ++matchCount;
            if (matchCount > 1) {
                return end;
            }
        }
        if (matchCount == 1) {
            return candidate;
        }
    }

    return end;
}

inline float exactTimeOffsetToSubstepPhase(double offset, double tickRate) {
    if (!std::isfinite(offset) || offset <= 0.0 || !std::isfinite(tickRate) || tickRate <= 0.0) {
        return 0.0f;
    }

    double phase = offset * tickRate;
    if (!std::isfinite(phase) || phase <= 0.0) {
        return 0.0f;
    }
    return static_cast<float>(std::clamp(phase, 0.0, 0.999999));
}

inline ExactInputDispatch classifyExactInputDispatch(
    double targetTimestamp,
    double sliceStartTimestamp,
    double sliceEndTimestamp,
    double epsilon = 0.000001
) {
    if (!std::isfinite(targetTimestamp) || !std::isfinite(sliceStartTimestamp) || !std::isfinite(sliceEndTimestamp)) {
        return ExactInputDispatch::Wait;
    }
    if (targetTimestamp <= sliceStartTimestamp + epsilon) {
        return ExactInputDispatch::Immediate;
    }
    if (targetTimestamp >= sliceEndTimestamp - epsilon) {
        return ExactInputDispatch::Wait;
    }
    return ExactInputDispatch::QueueNative;
}

inline bool shouldPreReconcileAnchorForExactInput(
    bool stepBasedPlayback,
    bool hasExactTimestamp,
    int inputTick,
    int effectiveTick,
    ExactInputDispatch dispatch
) {
    if (!stepBasedPlayback || !hasExactTimestamp || inputTick != effectiveTick) {
        return false;
    }
    return dispatch == ExactInputDispatch::QueueNative;
}

inline double playbackRuntimeTps(double macroTps, double requiredTps) {
    double result = std::isfinite(macroTps) && macroTps > 0.0 ? macroTps : 240.0;
    if (std::isfinite(requiredTps) && requiredTps > result) {
        result = requiredTps;
    }
    return result;
}

inline int materializeTickFromTime(double timeSeconds, double runtimeTps) {
    if (!std::isfinite(timeSeconds) || timeSeconds <= 0.0 || !std::isfinite(runtimeTps) || runtimeTps <= 0.0) {
        return 0;
    }
    double rawTick = timeSeconds * runtimeTps;
    double rounded = std::nearbyint(rawTick);
    if (std::abs(rawTick - rounded) < 1e-6) {
        return static_cast<int>(rounded);
    }
    return static_cast<int>(std::floor(rawTick));
}

inline double targetTimestampForPlaybackInput(
    double macroStartTimestamp,
    double tickStartTimestamp,
    double inputTimeSeconds,
    double cbsTimeOffset
) {
    if (std::isfinite(inputTimeSeconds) && inputTimeSeconds >= 0.0
        && std::isfinite(macroStartTimestamp) && macroStartTimestamp >= 0.0) {
        return macroStartTimestamp + inputTimeSeconds;
    }
    return tickStartTimestamp + std::max(0.0, cbsTimeOffset);
}

}
