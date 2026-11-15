#pragma once

#include <vector>


struct TimedAutoclickerAction {
    float offset = 0.0f;
    bool player2 = false;
    bool pressed = false;
};

struct TimedAutoclickerPlayerState {
    bool holding = false;
    bool userHolding = false;
    double timeUntilNextEdge = 0.0;
};

struct Autoclicker {
    struct TickResult {
        bool p1Fire = false;
        bool p1Press = false;
        bool p2Fire = false;
        bool p2Press = false;
    };

    static Autoclicker* get() {
        static Autoclicker instance;
        return &instance;
    }

    bool enabled = false;
    bool isAutoclickerInput = false;
    bool userHoldingP1 = false;
    bool userHoldingP2 = false;

    bool isTimedMode() const { return false; }

    void reset() {}

    void trackUserInput(bool, bool) {}

    TimedAutoclickerPlayerState captureTimedPlayerState(bool) const {
        return {};
    }

    void restoreTimedPlayerState(bool, TimedAutoclickerPlayerState const&) {}

    std::vector<TimedAutoclickerAction> buildTimedTickActions(double) {
        return {};
    }

    TickResult processTick() {
        return {};
    }
};
