#ifndef SKK_ACCURACY_MODE_HPP
#define SKK_ACCURACY_MODE_HPP

#include <cstdint>

enum class AccuracyMode : uint8_t {
    Vanilla = 0,
    CBS = 1,
};

inline bool isExternalAccuracyMod(AccuracyMode mode) {
    static_cast<void>(mode);
    return false;
}

inline AccuracyMode sanitizeAccuracyMode(int rawMode) {
    // Old files may still carry CBF (2) / SIP (3) accuracy tags; both are now
    // permanently degraded to CBS (best native level) since SIP/CBF are
    // declared incompatible at load time.
    if (rawMode == static_cast<int>(AccuracyMode::CBS)) {
        return AccuracyMode::CBS;
    }
    return AccuracyMode::Vanilla;
}

inline bool usesTimedAccuracy(AccuracyMode mode) {
    return mode != AccuracyMode::Vanilla;
}

inline bool usesStepBasedAccuracy(AccuracyMode mode) {
    return mode == AccuracyMode::CBS;
}

inline bool usesNativeQueuedPlaybackForAccuracyMode(AccuracyMode mode) {
    return mode == AccuracyMode::CBS;
}

#endif
