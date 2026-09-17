#ifndef _skk_anti_ssb_hpp
#define _skk_anti_ssb_hpp

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>

// Anti-SSB (scroll speed bug) support for skkBot.
//
// What SSB actually is (survey of silicate/grape/GDH/Eclipse/toasty-lite):
// the game integrates x as fl32(x + fl32(speedPx / TPS)); at large absolute x
// the float32 rounding changes the representable increment, so the REAL per-tick
// dx differs from the nominal one and the position<->time mapping (progress %,
// audio phase, timeForPos) slowly disagrees with the tick grid.
//
// The player-side correction during skkBot playback is already covered by the
// PerFrame layer (x/y/vy/vyBeforeSlope forced every tick - stronger than the
// position-forcing "SSB fix" ships elsewhere). This module provides the missing
// world-side and measurement pieces:
//   1. time-based progress recompute at TPS != 240 (PlayLayer hooks),
//   2. K_ssb measurement factor (analytic position->tick conversion, audio dt
//      compensation for the future renderer),
//   3. ScrollClock: timeForPos-delta pacing for step budgets when no per-frame
//      stream is available (legacy input-only playback, render passes).
//
// NOTE: the K factor is advisory only. Never shove the player onto a computed
// "ideal x line" - the game's accumulator re-derives x on the next tick and
// porta/orb/spider teleports would fight the correction (the old skkBot needed
// its 40px discontinuity rule for exactly this reason; the per-frame stream
// makes all of that unnecessary).

namespace skk::ssb {

// GD's px/s scroll speeds for m_playerSpeed {0.7, 0.9, 1.1, 1.3, 1.6}
// (DrawGridLayer::m_slowSpeed .. m_fastestSpeed / LevelTools::getSpeed).
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

// K_ssb: actually representable per-tick dx divided by the nominal one, at the
// player's current absolute x. Returns 1.0 when unmeasurable. Silicate's
// "formula given by ij9", made double-safe and range-guarded.
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

// Time-derived progress percent - immune to scroll drift because the game's
// own trigger clock (m_levelTime vs the level's ideal timestamp length in
// 240-TPS ticks) is what the audio/trigger phase actually follows.
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

// True when the vanilla %/stats bookkeeping assumes the native 240 grid and
// must be fed time-derived values instead.
inline double normalizedRate(double tps) {
    return (tps > 0.0) ? tps : 240.0;
}

inline bool progressFixNeeded(double tps) {
    tps = normalizedRate(tps);
    return std::abs(tps - 240.0) > 0.001;
}

// Grape-style pacing clock: consume the game's own position-derived elapsed
// time per frame instead of wall-clock. Only meaningful while x is monotone
// in time (classic levels; platformers excluded on purpose).
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
