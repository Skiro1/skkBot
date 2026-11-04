// Call accounting for the per-tick hot path.
//
// The bot is suspected of costing ~36 s of wall time against another bot on
// the SAME renderer (Xenon's kerender: 2:04 with Xenon's bot, 2:40 with ours).
// So the question is not the renderer at all - ours is not slower, Xenon's is
// slightly faster - it is what the bot adds inside the simulation.
//
// That is not answerable from the render stages. `sim` there measures
// m_pScheduler->update() as a whole, which contains GD's own physics AND
// everything we hang off it, and it varied 4x between two levels (2.04 ms on
// Artemis 13, 0.50 ms on Stereo Madness) without saying which half moved.
//
// Two shapes of counter, deliberately:
//   Count  - increments an integer. For a hook that can fire per object per
//            tick at 10^5-10^6 calls/sec.
//   Timed  - also accumulates milliseconds.
//
// The original note here said a clock read per object-tick "would distort
// exactly the thing being measured" and therefore the per-player hooks were
// left untimed. That was wrong by five orders of magnitude: steady_clock::now()
// is ~20 ns, so timing a 240 calls/sec hook costs 240 x 40 ns = 10 us per wall
// second, or 0.001%. The three player hooks are the ones that run on EVERY
// player object EVERY tick with the menu closed, and they were the only always-on
// paths with no number attached at all - which is the whole question the counter
// set was built to answer.
//
// Game thread only, so plain uint64_t: no atomics, no false sharing.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace skkbot2 {
namespace hot {

enum Slot {
    SchedUpdate = 0,   // CCScheduler::update hook - ONCE per our drive step
    SchedVanilla,  // GD's own CCScheduler::update, called from inside our hook
    PhysStep,      // one GD 240Hz physics step, counted where it is forced
    ImguiHook, // whole imgui draw hook body: capture + imgui itself
    PlayerUpdate,      // PlayerObject::update, trajectory side
    WtsPlayerUpdate,   // PlayerObject::update, wave-trail side
    SkcPlayLayer,      // PlayLayer hook used by SKC playback
    MacroPlayerUpdate, // PlayerObject::update, replay-engine side
    SafeModeJumps,     // incrementJumps
    SkEffectSetup,     // EffectGameObject::customObjectSetup
    BlurSnap,          // full-screen backdrop copy behind the menu blur
    ProcCmdBase,       // vanilla GJBaseGameLayer::processCommands body
    PqBtnBase,         // vanilla GJBaseGameLayer::processQueuedButtons body
    CheckColl,         // vanilla GJBaseGameLayer::checkCollisions
    PlayerBtn,         // vanilla GJBaseGameLayer::handleButton (whole click chain)
    PlayerPushBtn,     // vanilla PlayerObject::pushButton
    PlayerRelBtn,      // vanilla PlayerObject::releaseButton
    EffectPlayerBtn,   // vanilla GJEffectManager::playerButton
    JumpLogic,         // raw sub_14038B900: jump/press physics body inside pushButton
    RingAct,           // raw sub_140398C00: per-object activation inside pushButton
    BtnEventDispatch,  // raw sub_140231FF0: per-button event dispatch inside handleButton
    RingJump,          // PlayerObject::ringJump (probe)
    CircleWaveCreate,  // CCCircleWave::create (probe)
    GjGameEvent,       // GJBaseGameLayer::gameEventTriggered (probe: event dispatch cost)
    RingTriggerFx,     // EnhancedGameObject::playTriggerEffect (probe+skip)
    RingActByPlayer,   // EnhancedGameObject::activatedByPlayer (probe)
    RingPowerOff,      // EnhancedGameObject::powerOffObject (probe)
    RingPerfGate,      // vanilla ringJump WITH our performance-mode gate
    RingNoGate,        // vanilla ringJump WITHOUT gate (gate not taken)
    ActivateStreak,    // PlayerObject::activateStreak (probe)
    SetYVel,           // PlayerObject::setYVelocity (probe)
    RunBallRot2,       // PlayerObject::runBallRotation2 (probe)
    PlaceStreakPoint,  // PlayerObject::placeStreakPoint (probe)
    StreakAddPoint,    // HardStreak::addPoint (probe + Echo dedupe)
    StreakDraw,        // HardStreak::draw (CCDrawNode virtual, probe)
    StreakUpdateStroke, // HardStreak::updateStroke (our wave-trail fix renderer)
    ParticleSysUpdate, // CCParticleSystem::update (simulation of all emitters)
    ParticleQuadDraw,  // CCParticleSystemQuad::draw (GPU-side particle rendering)
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

// Visual frame identity for per-frame throttling of playback visuals.
//
// The full-skip variant (9f04418) restored the fps but dropped every visual
// the user cares about (trail, orb circles, spawn effects). Visuals drawn at
// 1800 calls/s look identical to visuals drawn once per rendered frame, so
// the hooks now gate on this counter instead of returning unconditionally:
// first caller inside a driven frame runs, the rest early-return. Bumped
// once per driven frame at the top of SpeedControlScheduler::update, which
// fires exactly once per frame in every mode.
inline std::atomic<uint32_t> visualFrameCounter{0};
inline void bumpVisualFrame() { visualFrameCounter.fetch_add(1, std::memory_order_relaxed); }
inline uint32_t visualFrame() { return visualFrameCounter.load(std::memory_order_relaxed); }

inline double now() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Calls only. Cheapest thing that still answers "how hot is this hook".
struct Count {
    uint64_t* slot;
    explicit Count(Slot s) : slot(&stats().calls[static_cast<size_t>(s)]) {
        ++(*slot);
    }
};

// Calls plus wall time. Per scheduler update, never per object.
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

}  // namespace hot
}  // namespace skkbot2