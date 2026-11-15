#pragma once

#include <Geode/Geode.hpp>

namespace skkbot2 {


[[nodiscard]] std::string pacingState();

std::string applyPacingForPin();

void restorePacing();

namespace fps {

inline constexpr double kFollowTps = -1.0;

struct MacroDisplayRate {
    enum class Kind { Absent, FollowTps, Pinned };

    Kind kind = Kind::Absent;
    double fps = 0.0;

    static MacroDisplayRate absent() { return {}; }

    static MacroDisplayRate followTps() {
        MacroDisplayRate rate;
        rate.kind = Kind::FollowTps;
        return rate;
    }

    static MacroDisplayRate pinned(double value) {
        MacroDisplayRate rate;
        rate.kind = Kind::Pinned;
        rate.fps = value;
        return rate;
    }

    [[nodiscard]] static MacroDisplayRate fromWire(double stored);

    [[nodiscard]] double toWire() const;

    [[nodiscard]] bool present() const { return kind != Kind::Absent; }
    [[nodiscard]] bool followsTps() const { return kind == Kind::FollowTps; }
};

void setEnabled(bool on);
void setTarget(double fps);
[[nodiscard]] bool enabled();
[[nodiscard]] double target();


void sync(double tps, double gameSpeed);

void release();

bool installOptionKeyTracer();

void restoreVsyncOnStartup();

[[nodiscard]] int stepCap(double tps, double gameSpeed);

void noteStepCost(double seconds);

[[nodiscard]] double stepCostMs();

[[nodiscard]] bool beginMacro(MacroDisplayRate const& rate);
void updateMacro(MacroDisplayRate const& rate);
void endMacro();

[[nodiscard]] bool macroHolds();

[[nodiscard]] bool macroUserOverride();

[[nodiscard]] MacroDisplayRate captureForStorage(double tps);

[[nodiscard]] unsigned long long capHits();
void noteCapHit();
void resetCounters();

}
}
