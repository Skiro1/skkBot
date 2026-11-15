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
#include <Geode/modify/CCCallFunc.hpp>
#include <Geode/modify/CCCallFuncN.hpp>
#include <Geode/modify/CCCallFuncO.hpp>
#include <Geode/binding/HardStreak.hpp>
#include <Geode/binding/PointNode.hpp>
#include "skc/skc_bot.hpp"
#include "skc/skc_frame.hpp"
#include "skkBot.hpp"
#include "core/hotpath.hpp"
#include "core/crashprobe.hpp"
#include "physics/phys_ring_jump.hpp"

using namespace geode::prelude;

namespace skkbot2::crashprobe {
static std::atomic<double> g_until{0.0};

static double nowSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void armWindow(double seconds) {
    g_until.store(nowSec() + seconds, std::memory_order_relaxed);
}

bool inWindow() {
    return nowSec() < g_until.load(std::memory_order_relaxed);
}
}


namespace {
    static bool isSpammingFast() {
        auto* engine = ReplayEngine::get();
        if (engine && (engine->engineMode == MODE_EXECUTE || skk::skc::Bot::get().isPlaying())) {
            return true;
        }
        return false;
    }

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
        if (used >= kMaxNodes) return false;
        entries[used].node = node;
        entries[used].passes = 1;
        ++used;
        return true;
    }

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

class $modify(SkkGameEventProbe, GJBaseGameLayer) {
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        skkbot2::hot::Timed t(skkbot2::hot::GjGameEvent);
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }
};

class $modify(SkkRingFxOpt, EffectGameObject) {
    void playTriggerEffect() {
        skkbot2::hot::Timed t(skkbot2::hot::RingTriggerFx);
        if (visualThrottledThisFrame<1>(this)) return;
        EffectGameObject::playTriggerEffect();
    }
};

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


class $modify(SkkStreakProbe, PlayerObject) {
    void placeStreakPoint(cocos2d::CCPoint pos) {
        skkbot2::hot::Timed t(skkbot2::hot::PlaceStreakPoint);
        PlayerObject::placeStreakPoint(pos);
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

    void stopStroke() {
        HardStreak::stopStroke();
    }

    void draw() {
        skkbot2::hot::Timed t(skkbot2::hot::StreakDraw);
        HardStreak::draw();
    }
};


class $modify(SkkPlayerSubProbe, PlayerObject) {
    void createFadeOutDartStreak() {
        PlayerObject::createFadeOutDartStreak();
    }

    void fadeOutStreak2(float duration) {
        PlayerObject::fadeOutStreak2(duration);
    }

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
        const bool throttled = visualThrottledThisFrame<3>(this);
        if (throttled) return;
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
