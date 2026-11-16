#ifndef _skk_anti_ssb_hpp
#define _skk_anti_ssb_hpp

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>


namespace skk::ssb {

inline constexpr double kSpeedPx[5] = {251.1601, 311.5801, 387.4201, 468.0002, 576.0002};

inline int speedIndex(float playerSpeed) {
    if (playerSpeed < 0.8f) {
        return 0;
    }
    if (playerSpeed < 1.0f) {
        return 1;
    }
    if (playerSpeed < 1.2f) {
        return 2;
    }
    if (playerSpeed < 1.45f) {
        return 3;
    }
    return 4;
}

inline double factor(PlayerObject* player, double tps) {
    if (!player || !(tps > 0.0)) {
        return 1.0;
    }
    const double nominal = kSpeedPx[speedIndex(player->m_playerSpeed)] / tps;
    if (!(nominal > 0.0)) {
        return 1.0;
    }
    const float x = player->getPositionX();
    const double real = static_cast<double>(
        static_cast<float>(static_cast<double>(x) + nominal) - x);
    if (!(real > 0.0)) {
        return 1.0;
    }
    return std::clamp(real / nominal, 0.5, 2.0);
}

inline double timeProgressPercent(GJBaseGameLayer* layer) {
    if (!layer || !layer->m_level) {
        return 0.0;
    }
    double percent = 0.0;
    if (layer->m_level->m_timestamp > 0) {
        percent = layer->m_gameState.m_levelTime * 240.0 /
                  static_cast<double>(layer->m_level->m_timestamp) * 100.0;
    } else if (layer->m_levelLength > 0.0 && layer->m_player1) {
        percent = layer->m_player1->getPositionX() * 100.0 / layer->m_levelLength;
    }
    return std::clamp(percent, 0.0, 100.0);
}

inline double normalizedRate(double tps) {
    return (tps > 0.0) ? tps : 240.0;
}

inline bool progressFixNeeded(double tps) {
    tps = normalizedRate(tps);
    return std::abs(tps - 240.0) > 0.001;
}

class ScrollClock {
public:
    double advance(PlayLayer* layer, double tps) {
        const double nominal = 1.0 / normalizedRate(tps);
        if (!layer || !layer->m_player1 || !layer->m_started ||
            layer->m_hasCompletedLevel || layer->m_isPaused || layer->m_isPlatformer) {
            m_last = -1.0;
            return nominal;
        }
        const float now = layer->timeForPos(layer->m_player1->m_position, 0,
                                            layer->m_gameState.m_currentChannel, true, 0);
        double delta = (m_last >= 0.0) ? std::abs(static_cast<double>(now) - m_last) : -1.0;
        m_last = static_cast<double>(now);
        if (!(delta > 0.0)) {
            return nominal;
        }
        return delta;
    }

    void reset() { m_last = -1.0; }

private:
    double m_last = -1.0;
};

}

#endif
