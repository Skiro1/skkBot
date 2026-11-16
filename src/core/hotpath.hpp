
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace skkbot2 {
namespace hot {

enum Slot {
    SchedUpdate = 0,
    SchedVanilla,
    PhysStep,
    ImguiHook,
    PlayerUpdate,
    WtsPlayerUpdate,
    SkcPlayLayer,
    MacroPlayerUpdate,
    SafeModeJumps,
    SkEffectSetup,
    BlurSnap,
    ProcCmdBase,
    PqBtnBase,
    CheckColl,
    PlayerBtn,
    PlayerPushBtn,
    PlayerRelBtn,
    EffectPlayerBtn,
    JumpLogic,
    RingAct,
    BtnEventDispatch,
    RingJump,
    CircleWaveCreate,
    GjGameEvent,
    RingTriggerFx,
    RingActByPlayer,
    RingPowerOff,
    RingPerfGate,
    RingNoGate,
    ActivateStreak,
    SetYVel,
    RunBallRot2,
    PlaceStreakPoint,
    StreakAddPoint,
    StreakDraw,
    StreakUpdateStroke,
    ParticleSysUpdate,
    ParticleQuadDraw,
    N
};

inline const char* name(Slot s) {
    switch (s) {
        case SchedUpdate:
            return "sched.update(hook, all)";
        case SchedVanilla:
            return "sched.update(GD base, inside ours)";
        case PhysStep:
            return "physics.step(1/240)";
        case ImguiHook:
            return "imgui.hook(whole)";
        case PlayerUpdate:
            return "player.update(traj)";
        case WtsPlayerUpdate:
            return "player.update(wts)";
        case SkcPlayLayer:
            return "playlayer(skc)";
        case MacroPlayerUpdate:
            return "player.update(macro)";
        case SafeModeJumps:
            return "safeMode.jumps";
        case SkEffectSetup:
            return "effect.setup";
        case BlurSnap:
            return "blur.snapshot";
        case ProcCmdBase:
            return "processCommands(vanilla)";
        case PqBtnBase:
            return "queuedButtons(vanilla)";
        case CheckColl:
            return "checkCollisions";
        case PlayerBtn:
            return "handleButton(vanilla)";
        case PlayerPushBtn:
            return "pushButton(vanilla)";
        case PlayerRelBtn:
            return "releaseButton(vanilla)";
        case EffectPlayerBtn:
            return "effect.playerButton";
        case JumpLogic:
            return "jumpLogic(raw)";
        case RingAct:
            return "ring.activate(raw)";
        case BtnEventDispatch:
            return "btnEvent.dispatch(raw)";
        case RingJump:
            return "player.ringJump";
        case CircleWaveCreate:
            return "circleWave.create";
        case GjGameEvent:
            return "gjbase.gameEvent";
        case RingTriggerFx:
            return "ring.triggerEffect";
        case RingActByPlayer:
            return "ring.activatedByPlayer";
        case RingPowerOff:
            return "ring.powerOffObject";
        case RingPerfGate:
            return "ringJump.vanilla(perfgate)";
        case RingNoGate:
            return "ringJump.vanilla(ungated)";
        case ActivateStreak:
            return "player.activateStreak";
        case SetYVel:
            return "player.setYVelocity";
        case RunBallRot2:
            return "player.runBallRotation2";
        case PlaceStreakPoint:
            return "player.placeStreakPoint";
        case StreakAddPoint:
            return "streak.addPoint";
        case StreakDraw:
            return "streak.draw";
        case StreakUpdateStroke:
            return "streak.updateStroke";
        case ParticleSysUpdate:
            return "particle.update";
        case ParticleQuadDraw:
            return "particle.draw";
        default:
            return "?";
    }
}

struct Stats {
    std::array<uint64_t, N> calls{};
    std::array<double, N> ms{};
};

inline Stats& stats() {
    static Stats s;
    return s;
}

inline std::atomic<uint32_t> visualFrameCounter{0};
inline void bumpVisualFrame() { visualFrameCounter.fetch_add(1, std::memory_order_relaxed); }
inline uint32_t visualFrame() { return visualFrameCounter.load(std::memory_order_relaxed); }

inline double now() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Count {
    uint64_t* slot;
    explicit Count(Slot s) : slot(&stats().calls[static_cast<size_t>(s)]) {
        ++(*slot);
    }
};

struct Timed {
    Stats& s = stats();
    size_t idx;
    double t0;
    explicit Timed(Slot sl) : idx(static_cast<size_t>(sl)), t0(now()) {}
    ~Timed() {
        s.calls[idx]++;
        s.ms[idx] += (now() - t0) * 1e3;
    }
};

}
}