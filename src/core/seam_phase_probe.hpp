#pragma once

#include "core/step_trace.hpp"
#include "skc/skc_bot.hpp"
#include "skc/skc_frame.hpp"
#include "skkBot.hpp"
#include "logger/logger.hpp"
#include "trajectory/trajectory.hpp"

#include <algorithm>
#include <vector>

namespace skkbot2::seam_probe {
inline std::vector<int> skcCenters;
inline std::vector<int> skkCenters;
inline int skkGeneration = -1;

inline int frame() {
    const auto& bot = skk::skc::Bot::get();
    if (bot.isRecording() || bot.isPlaying())
        return static_cast<int>(skk::skc::FrameClock::get().getFrame());
    return ReplayEngine::get()->stepPairCount;
}

inline void arm(int center) {
    if (!step_trace::enabled() || skk::trajectory::isSimulating()) return;
    const auto& bot = skk::skc::Bot::get();
    const bool skc = bot.isRecording() || bot.isPlaying();
    auto* engine = ReplayEngine::get();
    if (!skc && engine->engineMode != MODE_CAPTURE && engine->engineMode != MODE_EXECUTE)
        return;
    auto& centers = skc ? skcCenters : skkCenters;
    if (!skc && skkGeneration != engine->sessionCounter) {
        skkCenters.clear();
        skkGeneration = engine->sessionCounter;
    }
    if (std::find(centers.begin(), centers.end(), center) != centers.end()) return;
    if (centers.size() == 128) centers.erase(centers.begin());
    centers.push_back(center);
}

inline bool wanted(PlayerObject* player, int f) {
    if (!step_trace::enabled() || !player || skk::trajectory::isSimulating()) return false;
    auto* layer = PlayLayer::get();
    if (!layer || (player != layer->m_player1 && player != layer->m_player2)) return false;
    const auto& bot = skk::skc::Bot::get();
    const bool skc = bot.isRecording() || bot.isPlaying();
    auto* engine = ReplayEngine::get();
    if (!skc && engine->engineMode != MODE_CAPTURE && engine->engineMode != MODE_EXECUTE)
        return false;
    auto const& centers = skc ? skcCenters : skkCenters;
    return std::any_of(centers.begin(), centers.end(), [f](int center) {
        return f >= center - 2 && f <= center + 4;
    });
}

inline void log(char const* phase, PlayerObject* player, float dt = 0.f,
                int checkpointFrame = -1, int button = 0) {
    const int f = checkpointFrame >= 0 ? checkpointFrame : frame();
    if (!wanted(player, f)) return;
    auto* layer = PlayLayer::get();
    const auto& bot = skk::skc::Bot::get();
    auto* engine = ReplayEngine::get();
    const auto it = player->m_holdingButtons.find(1);
    skkbot2::log::info(
        "[SEAM-PHASE] phase={} f={} clock={} skcRec={} skcPlay={} skkMode={} p={} "
        "dt={:a} button={} time={:a} progress={} step={} node=({:a},{:a}) phys=({:a},{:a}) "
        "vy={:a} ground={} ground2={} jumpBuf={} wasBuf={} stateBuf={} held={} queue={}",
        phase, f, frame(), bot.isRecording(), bot.isPlaying(), static_cast<int>(engine->engineMode),
        player == layer->m_player2 ? 2 : 1, dt, button, layer->m_gameState.m_levelTime,
        layer->m_gameState.m_currentProgress, layer->m_currentStep,
        player->getPositionX(), player->getPositionY(), player->m_position.x, player->m_position.y,
        static_cast<double>(player->m_yVelocity), player->m_isOnGround, player->m_isOnGround2,
        player->m_jumpBuffered, player->m_wasJumpBuffered,
        static_cast<unsigned>(player->m_stateJumpBuffered),
        it != player->m_holdingButtons.end() && it->second, layer->m_queuedButtons.size());
}
}
