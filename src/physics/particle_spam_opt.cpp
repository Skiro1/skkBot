#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/CCDrawNode.hpp>
#include <Geode/modify/CCParticleSystem.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Geode/binding/HardStreak.hpp>
#include <Geode/binding/PointNode.hpp>
#include "skc/skc_bot.hpp"
#include "skc/skc_frame.hpp"
#include "skkBot.hpp"
#include "core/hotpath.hpp"
#include "physics/phys_ring_jump.hpp"

using namespace geode::prelude;

// Ring / Player particle and circle spam optimization:
// When macro playback runs at high TPS (e.g. 3414 TPS) on spam sections,
// repeated orb activations spawn hundreds of circle effects (RingObject::spawnCircle)
// and player jump particles in the exact same spot within mere milliseconds.
// In vanilla GD this creates a massive backlog of CCNode particle systems and sprite actions.
// This throttle ensures circles and spawn effects are not duplicated more than once per frame.

namespace {
    static bool isSpammingFast() {
        auto* engine = ReplayEngine::get();
        if (engine && (engine->engineMode == MODE_EXECUTE || skk::skc::Bot::get().isPlaying())) {
            return true;
        }
        return false;
    }

    // Per-frame throttle, COUNT per NODE instead of a global cap.
    //
    // History:
    //   * Node keying failed first (2747477): the check was "is this the node
    //     that passed last frame", and dual P1/P2 alternates every call, so it
    //     never matched and everything passed through.
    //   * Then a global kMaxPerFrame=2: the first two CALLS in a frame pass.
    //     That is safe for a single spam ring and for P1+P2, but every
    //     DISTINCT ring/trigger object beyond the first two callers within one
    //     drawn frame loses its circle/effect - a visual difference from
    //     vanilla (and Echo, which touches none of these in visible playback).
    //
    // Now: up to kMaxPerNode passes PER NODE per frame, and up to
    // kMaxNodes distinct nodes per frame. Identical nodes keep their previous
    // budget (the storm from repeated same-node activation stays throttled),
    // while distinct nodes no longer steal each other's budget, so multi-orb
    // frames keep every object's visual. Cost: ≤8 pointer compares per call.
    template <int Site>
    static bool oncePerVisualFrame(void* node) {
        static constexpr unsigned kMaxPerNode = 2;
        static constexpr unsigned kMaxNodes = 8;
        struct Entry { void* node; unsigned passes; };
        static Entry entries[kMaxNodes];
        static uint32_t lastFrame = 0;
        static unsigned used = 0;
        if (!isSpammingFast()) {
            lastFrame = skkbot2::hot::visualFrame();
            used = 0;
            return true;
        }
        const uint32_t f = skkbot2::hot::visualFrame();
        if (lastFrame != f) {
            lastFrame = f;
            used = 0;
        }
        for (unsigned i = 0; i < used; ++i) {
            if (entries[i].node == node) {
                if (entries[i].passes >= kMaxPerNode) return false;
                ++entries[i].passes;
                return true;
            }
        }
        if (used >= kMaxNodes) return false;  // absurd-storm guard
        entries[used].node = node;
        entries[used].passes = 1;
        ++used;
        return true;
    }

    // One-shot log of GD's own performance mode at the first throttled site.
    inline void logPerformanceModeOnce() {
        static bool logged = false;
        if (!logged) {
            logged = true;
            auto* gm = GameManager::sharedState();
            if (gm) {
                skkbot2::log::info(
                    "playback visuals: GD m_performanceMode={} (true = GD skips orb circle-waves "
                    "and several effects on its own, independent of our hooks)",
                    gm->m_performanceMode ? "true" : "false");
            }
        }
    }

    // Gate for visual-only playback paths: throttle during playback, vanilla
    // outside it (normal play keeps every vanilla visual untouched).
    template <int Site>
    static bool visualThrottledThisFrame(void* node) {
        logPerformanceModeOnce();
        return !oncePerVisualFrame<Site>(node);
    }
}

class $modify(SkkRingEffectOpt, RingObject) {
    void spawnCircle() {
        if (visualThrottledThisFrame<0>(this)) return;
        RingObject::spawnCircle();
    }
};

class $modify(SkkRingJumpProbe, PlayerObject) {
    void ringJump(RingObject* object, bool skipCheck) {
        skkbot2::hot::Timed t(skkbot2::hot::RingJump);
        if (skkbot2::phys::ringJumpEnabled() && skkbot2::phys::tryRingJump(this, object, skipCheck)) {
            return;
        }
        // Playback only: GD's own performance mode gates the visual circle-wave
        // block inside ringJump (!GM->m_performanceMode), the exact block that
        // measured 630ms per wall second on Schafs (2492 ringJump calls/s).
        // Physics inside ringJump never reads the flag, so swap it around the
        // vanilla call and restore: zero persistent state, drift-free.
        //
        // Two separate Timed slots so the log proves BOTH facts at once:
        // whether the gate was ever taken (calls) and what the gated vanilla
        // run costs versus the ungated one (ms).
        if (isSpammingFast()) {
            auto* gm = GameManager::sharedState();
            if (gm && !gm->m_performanceMode) {
                gm->m_performanceMode = true;
                {
                    skkbot2::hot::Timed t(skkbot2::hot::RingPerfGate);
                    PlayerObject::ringJump(object, skipCheck);
                }
                gm->m_performanceMode = false;
                return;
            }
        }
        {
            skkbot2::hot::Timed t(skkbot2::hot::RingNoGate);
            PlayerObject::ringJump(object, skipCheck);
        }
    }
};

// Measurement only (never skipped): the two gameEventTriggered dispatches at
// the top of every ringJump hit run the whole trigger/event system. If these
// dominate after the visual block is gone, that's the next target.
class $modify(SkkGameEventProbe, GJBaseGameLayer) {
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        skkbot2::hot::Timed t(skkbot2::hot::GjGameEvent);
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }
};

// playTriggerEffect is the ring's own particle/animation (visual only; GD's
// low-detail "hasNoEffects" levels already skip it without breaking anything).
// Skipped during playback, measured always.
class $modify(SkkRingFxOpt, EffectGameObject) {
    void playTriggerEffect() {
        skkbot2::hot::Timed t(skkbot2::hot::RingTriggerFx);
        if (visualThrottledThisFrame<1>(this)) return;
        EffectGameObject::playTriggerEffect();
    }
};

// Measurement only: activatedByPlayer / powerOffObject touch object state
// (activation flags), so no skip until we are sure they are visual-only.
class $modify(SkkRingStateProbe, EnhancedGameObject) {
    void activatedByPlayer(PlayerObject* player) {
        skkbot2::hot::Timed t(skkbot2::hot::RingActByPlayer);
        EnhancedGameObject::activatedByPlayer(player);
    }

    void powerOffObject() {
        skkbot2::hot::Timed t(skkbot2::hot::RingPowerOff);
        EnhancedGameObject::powerOffObject();
    }
};

// ------------------------------------------------------------------------------
// Streak (trail) chain - ported from Echo after verifying what it actually does:
//
//   activateStreak skip   - Echo skips it ONLY when the player is hidden
//                           (hide_p1/hide_p2). We now per-frame throttle it
//                           instead (full skip dropped the visuals users need).
//   EchoHardStreak::addPoint
//                         - dedupe: a point equal to the last one (<1e-6 px)
//                           is NOT added. At high TPS the player adds many
//                           identical points per rendered frame; this is where
//                           the trail storm dies without any visible loss.
//   EchoWaveTrail         - full custom batched wave-trail mesh (out of scope).
//   EchoDrawNode          - GL buffer trim (candidate for a later step).
//
// resumeStroke is `win inline`, i.e. compiled INTO vanilla activateStreak -
// that was our 337us/call; it cannot be hooked, but addPoint dedupe and the
// per-frame throttle cut the same work from the node side.
// ------------------------------------------------------------------------------

class $modify(SkkStreakProbe, PlayerObject) {
    void placeStreakPoint() {
        skkbot2::hot::Timed t(skkbot2::hot::PlaceStreakPoint);
        PlayerObject::placeStreakPoint();
    }
};

class $modify(SkkHardStreakOpt, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        skkbot2::hot::Timed t(skkbot2::hot::StreakAddPoint);
        auto* points = m_pointArray;
        auto* last = points && points->count() > 0
            ? static_cast<PointNode*>(points->lastObject())
            : nullptr;
        if (last && last->m_point.getDistanceSq(point) < 1e-6f) return;
        HardStreak::addPoint(point);
    }

    void draw() {
        skkbot2::hot::Timed t(skkbot2::hot::StreakDraw);
        HardStreak::draw();
    }
};

// Per-activation player-side subprobes: the log found the hog. activateStreak
// is 337us per call (98.8% of vanilla ringJump time) and it is PURELY visual:
// m_regularTrail->resumeStroke() + wave trail resume + m_fadeOutStreak=true.
// m_fadeOutStreak is only read by deactivateStreak/placeStreakPoint/
// rotateGameplay (streak visuals), never by physics. Echo's hook does the
// same skip (hooks.cpp:2131 player_hidden). Skipped during playback only.
class $modify(SkkPlayerSubProbe, PlayerObject) {
    void activateStreak() {
        skkbot2::hot::Timed t(skkbot2::hot::ActivateStreak);
        if (visualThrottledThisFrame<2>(this)) return;
        PlayerObject::activateStreak();
    }

    void setYVelocity(double velocity, int type) {
        skkbot2::hot::Timed t(skkbot2::hot::SetYVel);
        PlayerObject::setYVelocity(velocity, type);
    }

    void runBallRotation2() {
        skkbot2::hot::Timed t(skkbot2::hot::RunBallRot2);
        PlayerObject::runBallRotation2();
    }
};

class $modify(SkkPlayerEffectOpt, PlayerObject) {
    void spawnCircle() {
        if (visualThrottledThisFrame<3>(this)) return;
        PlayerObject::spawnCircle();
    }

    void spawnDualCircle() {
        if (visualThrottledThisFrame<4>(this)) return;
        PlayerObject::spawnDualCircle();
    }

    void spawnScaleCircle() {
        if (visualThrottledThisFrame<5>(this)) return;
        PlayerObject::spawnScaleCircle();
    }

    void playSpawnEffect() {
        if (visualThrottledThisFrame<6>(this)) return;
        PlayerObject::playSpawnEffect();
    }
};

// ------------------------------------------------------------------------------
// Particle cost probe (measure only, never skipped).
//
// Echo does NOT touch particle systems in visible playback: its only
// particle-related skips are under Logic::predicting (its simulation passes,
// not the visible frame) and hide/show for hidden players. Our system runs
// vanilla too (we hold no CCParticleSystem hooks) - this pair of slots just
// proves the cost is either negligible (leave vanilla forever) or worth
// porting MIT-licensed techniques from Minimum/MinimumReborn.
//
// update(float) is virtual on the CCParticleSystem base and NOT overridden by
// CCParticleSystemQuad, so this single hook observes every emitter in the
// level (player trails, orb emitters, trigger-spawned systems). draw() is
// virtual on Quad, so it observes the actual GPU-side submission.
// ------------------------------------------------------------------------------

class $modify(SkkParticleProbe, cocos2d::CCParticleSystem) {
    void update(float dt) {
        skkbot2::hot::Timed t(skkbot2::hot::ParticleSysUpdate);
        CCParticleSystem::update(dt);
    }
};

class $modify(SkkParticleQuadProbe, cocos2d::CCParticleSystemQuad) {
    void draw() {
        skkbot2::hot::Timed t(skkbot2::hot::ParticleQuadDraw);
        CCParticleSystemQuad::draw();
    }
};
