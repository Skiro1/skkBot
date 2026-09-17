#pragma once

// Stub replacement for the removed skkBot trajectory module.
// The replay engine / checkpoint system only consult whether a trajectory
// simulation is running; the native-imGui port never runs one, so the stub
// always reports false.

class TrajectoryPredictionService {
public:
    static TrajectoryPredictionService& get() {
        static TrajectoryPredictionService instance;
        return instance;
    }

    bool isActiveSimulation() const {
        return false;
    }

private:
    TrajectoryPredictionService() = default;
};
