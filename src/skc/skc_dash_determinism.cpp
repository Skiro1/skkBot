

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
        double startTickDt = 0.0;
        float baseRot = 0.0f;
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

        auto* pl = PlayLayer::get();
        if (!pl || (this != pl->m_player1 && this != pl->m_player2))
            return;

        float dir = (this->m_isSideways == this->m_isUpsideDown) ? 1.0f : -1.0f;
        float deltaAngle = dir * 288.0f * this->m_gravityMod;

        if (this->m_isPlatformer) {
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

