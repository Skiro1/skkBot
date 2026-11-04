// BUG-90: dash-stop rotation determinism.
//
// While dashing, PlayerObject's gameplay rotation is FROZEN by
// updateRotation() (early-return on m_isDashing), and the visible spin is a
// Cocos action instead: updateDashArt() runs
//     CCRepeatForever(CCRotateBy(0.3s, dir * 288 * m_gravityMod))
// on m_iconSprite - 960 deg/s in classic mode under normal gravity.
//
// stopDashing() then bakes that action-driven rotation into gameplay state:
//     float iconSpriteRotation = m_iconSprite->getRotation();
//     m_iconSprite->setRotation(0.0f);
//     ...
//     setRotation(iconSpriteRotation);   // <- gameplay rotation
//
// The action advances with time fed to the Cocos scheduler, which in the
// playback world does not exactly equal the physics tick sum (measured: after
// a 0.9375 s / 225-tick dash at 240 TPS, the recorded world baked 900.000 deg
// while the replayed world baked 896.49 deg - a 3.51 deg base offset that then
// lives invisibly inside gameplay rotation and resurfaces at every quantized
// event, one physics step off, until the run dies).
//
// Fix: snapshot base rotation + icon spin rate at every updateDashArt call
// while dashing, and in a post-hook of stopDashing rewrite the baked gameplay
// rotation to the tick-canonical value base + rate * (sum of physics dt since
// the dash spin started). The rec world computed exactly this value, so the
// replay now matches the recording bit-for-bit at dash seams.

#include <Geode/modify/PlayerObject.hpp>
#include <Geode/binding/PlayLayer.hpp>

#include "skc_bot.hpp"
#include "skc_frame.hpp"
#include "skc_tick_clock.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace skkbot2::skc {
namespace {
    double g_tickDtTotal = 0.0;
}

void advanceTickDt(double dt) {
    g_tickDtTotal += dt;
}

double tickDtTotal() {
    return g_tickDtTotal;
}
}

namespace {
    struct DashClockSnapshot {
        bool active = false;
        // tickDtTotal() read mid-spin-start step (= total through the
        // previous step, since advanceTickDt runs after the vanilla body).
        double startTickDt = 0.0;
        // Player rotation right after updateDashArt's setRotation.
        float baseRot = 0.0f;
        // Icon spin rate in deg per game-second (deltaAngle / 0.3 s).
        float ratePerSec = 0.0f;
    };
}

class $modify(SkcDashRotation, PlayerObject) {
    struct Fields {
        DashClockSnapshot dash;
        float preStopRot = 0.0f;
    };

    void updateDashArt() {
        PlayerObject::updateDashArt();

        auto& bot = skk::skc::Bot::get();
        if (!(bot.isRecording() || bot.isPlaying()))
            return;
        if (!this->m_isDashing)
            return;

        // Real gameplay players only - the trajectory sim stops its own
        // dummies through this same virtual, and their rotation is fake.
        auto* pl = PlayLayer::get();
        if (!pl || (this != pl->m_player1 && this != pl->m_player2))
            return;

        // Replicate updateDashArt's icon-spin action parameters so the
        // canonical integral matches what the live action would have done.
        float dir = (this->m_isSideways == this->m_isUpsideDown) ? 1.0f : -1.0f;
        float deltaAngle = dir * 288.0f * this->m_gravityMod;

        if (this->m_isPlatformer) {
            // 5.770002f is the player's global speed multiplier at 1x speed.
            constexpr float kTimeModNormal = 5.770002f;
            const float dashLength = std::sqrt(
                static_cast<float>(this->m_dashX) * static_cast<float>(this->m_dashX) +
                static_cast<float>(this->m_dashY) * static_cast<float>(this->m_dashY));
            const float normalizedSpeed =
                std::min(dashLength / (3.0f * kTimeModNormal), 1.0f);
            const float mod = (normalizedSpeed * (2.0f - 0.3f)) + 0.3f;
            deltaAngle *= mod;
        }

        m_fields->dash.active = true;
        m_fields->dash.startTickDt = skkbot2::skc::tickDtTotal();
        m_fields->dash.baseRot = this->getRotation();
        m_fields->dash.ratePerSec = deltaAngle / 0.3f;

        skkbot2::log::debug(
            "[SKC] dash-spin-snap f={} rec={} play={} base={:a} rate={:a} "
            "startTickDt={:a} ud={} side={} gmod={:a}",
            skk::skc::FrameClock::get().getFrame(),
            bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
            static_cast<double>(m_fields->dash.baseRot),
            static_cast<double>(m_fields->dash.ratePerSec),
            m_fields->dash.startTickDt,
            this->m_isUpsideDown ? 1 : 0,
            this->m_isSideways ? 1 : 0,
            static_cast<double>(this->m_gravityMod));
    }

    void stopDashing() {
        m_fields->preStopRot = this->getRotation();
        PlayerObject::stopDashing();

        auto& bot = skk::skc::Bot::get();
        if (!(bot.isRecording() || bot.isPlaying()))
            return;
        if (!m_fields->dash.active)
            return;
        m_fields->dash.active = false;

        // In platformer stopDashing re-runs runNormalRotation(...) after the
        // icon restore; overwriting its start rotation would fight the
        // platformer rotation action. Classic-mode replay is the SKC target.
        if (this->m_isPlatformer) {
            skkbot2::log::debug(
                "[SKC] dash-rot-canon f={} rec={} play={} platformer - not "
                "correcting, gd={:a}",
                skk::skc::FrameClock::get().getFrame(),
                bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                static_cast<double>(this->getRotation()));
            return;
        }

        const double elapsed = skkbot2::skc::tickDtTotal() - m_fields->dash.startTickDt;
        const float gdRot = this->getRotation();
        const float canon = static_cast<float>(
            static_cast<double>(m_fields->dash.baseRot) +
            static_cast<double>(m_fields->dash.ratePerSec) * elapsed);

        // The live action's rotation is never exactly reproduced by the
        // scheduler (that is the bug), so apply the correction whenever the
        // bake differs, and log it so record/play pairs can be checked 1:1.
        this->setRotation(canon);

        skkbot2::log::info(
            "[SKC] dash-rot-canon f={} rec={} play={} gd={:a} canon={:a} "
            "base={:a} rate={:a} elapsed={:a}",
            skk::skc::FrameClock::get().getFrame(),
            bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
            static_cast<double>(gdRot), static_cast<double>(canon),
            static_cast<double>(m_fields->dash.baseRot),
            static_cast<double>(m_fields->dash.ratePerSec), elapsed);
    }
};

