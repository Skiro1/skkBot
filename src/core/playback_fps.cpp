#include "core/playback_fps.hpp"

#include "logger/logger.hpp"
#include "render/renderer.hpp"

#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlatformToolbox.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/modify/CCDirector.hpp>

#include <algorithm>
#include <cmath>

static constexpr double kFrameTimeBudgetMin = 0.016;
static constexpr double kFrameTimeBudgetMax = 0.032;
static constexpr int kFloorCap = 8;
static constexpr int kHardCap = 512;

static double g_emaStepCost = 0.0;

static constexpr const char* kVsyncKey = "0030";
static constexpr bool kVsyncOffValue = false;
static constexpr bool kVsyncOnValue = true;

static constexpr const char* kUnlockKey = "0116";
static constexpr bool kUnlockValue = true;

static constexpr float kUnlockFloor = 60.0f;

inline void applyVerticalSync(bool enable) {
    PlatformToolbox::toggleVerticalSync(enable);
}

namespace {

bool g_enabled = false;
double g_target = 0.0;

bool g_held = false;
float g_before = 0.0f;
float g_applied = -1.0f;

bool g_vsyncSeen = false;
bool g_beforeVsyncVar = false;
bool g_beforeUnlockVar = false;
bool g_verified = false;
float g_beforeCustomFps = 0.0f;

unsigned long long g_capHits = 0;

bool g_macroHeld = false;
bool g_savedEnabled = false;
double g_savedTarget = 0.0;

bool g_macroEnabled = false;
double g_macroTarget = 0.0;
bool g_userOverride = false;

void applyMacroRate(skkbot2::fps::MacroDisplayRate const& rate) {
    using Kind = skkbot2::fps::MacroDisplayRate::Kind;
    switch (rate.kind) {
        case Kind::Absent:
            g_macroEnabled = false;
            g_macroTarget = g_target;
            g_enabled = false;
            break;
        case Kind::FollowTps:
            g_macroEnabled = true;
            g_macroTarget = skkbot2::fps::kFollowTps;
            g_enabled = true;
            g_target = skkbot2::fps::kFollowTps;
            break;
        case Kind::Pinned: {
            const double value =
                std::isfinite(rate.fps) ? std::max(1.0, rate.fps) : 1.0;
            g_macroEnabled = true;
            g_macroTarget = value;
            g_enabled = true;
            g_target = value;
            break;
        }
    }
}

const char* kindName(skkbot2::fps::MacroDisplayRate const& rate) {
    using Kind = skkbot2::fps::MacroDisplayRate::Kind;
    switch (rate.kind) {
        case Kind::FollowTps: return "follow tps";
        case Kind::Pinned: return "pinned";
        case Kind::Absent: break;
    }
    return "not stored";
}

bool renderOwnsFrame() {
    return skkbot2::render::Renderer::get().isRecording();
}

float desiredInterval(double tps) {
    if (!g_enabled) {
        return -1.0f;
    }
    const double fps = (g_target == skkbot2::fps::kFollowTps) ? tps : g_target;
    if (!(fps > 0.0) || !std::isfinite(fps)) {
        return -1.0f;
    }
    return static_cast<float>(1.0 / fps);
}

void dropExtraDelta() {
    if (auto* play = PlayLayer::get()) {
        play->m_extraDelta = 0.0f;
    }
}

double menuSavedFps() {
    return geode::Mod::get()->getSavedValue<double>("gui_fps_value", 0.0);
}

}

namespace skkbot2::fps {

void restoreVsyncOnStartup() {
    auto* gm = GameManager::get();
    if (!gm) return;

    const bool vsyncVar = gm->getGameVariable(kVsyncKey);
    if (!vsyncVar) {
        gm->setGameVariable(kVsyncKey, kVsyncOnValue);
        applyVerticalSync(kVsyncOnValue);
        skkbot2::log::info(
            "[TIMING] startup: VSync was off ({}=false) from a previous session "
            "- restored to on. FPS is currently disabled, so nothing should have "
            "turned it off.", kVsyncKey);
    }

    skkbot2::restorePacing();
}

void setEnabled(bool on) {
    if (g_macroHeld && !g_userOverride && on != g_macroEnabled) {
        g_userOverride = true;
        g_savedEnabled = on;
        skkbot2::log::info(
            "[TIMING] FPS switched {} by hand during playback; the macro's "
            "display rate will not be re-asserted this session",
            on ? "on" : "off");
    }
    g_enabled = on;
}

void setTarget(double f) {
    if (g_macroHeld && !g_userOverride && f != g_macroTarget) {
        g_userOverride = true;
        g_savedTarget = f;
        skkbot2::log::info(
            "[TIMING] FPS set by hand to {:.1f} during playback; the macro's "
            "display rate will not be re-asserted this session",
            f == skkbot2::fps::kFollowTps ? 0.0 : f);
    }
    g_target = f;
}
bool enabled() { return g_enabled; }
double target() { return g_target; }
unsigned long long capHits() { return g_capHits; }
void noteCapHit() { ++g_capHits; }
void resetCounters() { g_capHits = 0; }

void release() {
    if (!g_held) {
        return;
    }
    g_held = false;
    g_verified = false;
    resetCounters();
    g_applied = -1.0f;
    if (auto* director = cocos2d::CCDirector::sharedDirector()) {
        if (g_before > 0.0f) {
            director->setAnimationInterval(g_before);
        }
    }
    if (auto* gm = GameManager::get(); gm && g_vsyncSeen) {
        gm->m_customFPSTarget = g_beforeCustomFps;
        gm->setGameVariable(kUnlockKey, g_beforeUnlockVar);
        gm->updateCustomFPS();
        gm->setGameVariable(kVsyncKey, kVsyncOnValue);
        applyVerticalSync(kVsyncOnValue);
        skkbot2::log::info(
            "[TIMING] vsync ON ({}) and toggleVerticalSync(true) called; it was "
            "{}, unlock gate {} back to {}, m_customFPSTarget back to {:.1f}",
            kVsyncKey, g_beforeVsyncVar, kUnlockKey, g_beforeUnlockVar,
            static_cast<double>(g_beforeCustomFps));
        g_vsyncSeen = false;
    }
    skkbot2::restorePacing();
    g_before = 0.0f;
    dropExtraDelta();
}

bool beginMacro(MacroDisplayRate const& rate) {
    if (g_macroHeld) {
        updateMacro(rate);
        return true;
    }
    g_savedEnabled = g_enabled;
    g_savedTarget = g_target;
    g_macroHeld = true;
    g_userOverride = false;
    applyMacroRate(rate);
    skkbot2::log::info(
        "[TIMING] macro display rate: {} ({:.1f}) - menu had {} at {:.1f} "
        "(menu slider saved: {:.1f}), restored on stop",
        kindName(rate), rate.kind == MacroDisplayRate::Kind::Pinned ? rate.fps : 0.0,
        g_savedEnabled ? "it on" : "it off",
        g_savedTarget == skkbot2::fps::kFollowTps ? 0.0 : g_savedTarget,
        menuSavedFps());
    return true;
}

void updateMacro(MacroDisplayRate const& rate) {
    if (!g_macroHeld) {
        return;
    }
    if (g_userOverride) {
        return;
    }
    applyMacroRate(rate);
}

void endMacro() {
    if (g_macroHeld) {
        g_macroHeld = false;
        g_userOverride = false;
        g_enabled = g_savedEnabled;
        g_target = g_savedTarget;
        skkbot2::log::info("[TIMING] macro display rate released (menu back to {} "
                           "at {:.1f}, menu slider saved: {:.1f})",
                           g_enabled ? "on" : "off",
                           g_target == skkbot2::fps::kFollowTps ? 0.0 : g_target,
                           menuSavedFps());
    }
    release();
}

bool macroHolds() { return g_macroHeld; }
bool macroUserOverride() { return g_userOverride; }

MacroDisplayRate MacroDisplayRate::fromWire(double stored) {
    if (!std::isfinite(stored)) {
        return absent();
    }
    if (std::signbit(stored)) {
        return followTps();
    }
    if (stored == 0.0) {
        return absent();
    }
    return pinned(stored);
}

double MacroDisplayRate::toWire() const {
    switch (kind) {
        case Kind::FollowTps:
            return std::copysign(0.0, -1.0);
        case Kind::Pinned:
            return fps > 0.0 && std::isfinite(fps) ? fps : 0.0;
        case Kind::Absent:
            break;
    }
    return 0.0;
}

MacroDisplayRate captureForStorage(double tps) {
    if (!g_enabled) {
        return MacroDisplayRate::absent();
    }
    if (g_target == skkbot2::fps::kFollowTps) {
        if (!std::isfinite(tps) || !(tps > 0.0)) {
            return MacroDisplayRate::absent();
        }
        return MacroDisplayRate::followTps();
    }
    if (!std::isfinite(g_target) || !(g_target > 0.0)) {
        return MacroDisplayRate::absent();
    }
    return MacroDisplayRate::pinned(g_target);
}

void sync(double tps, double gameSpeed) {
    if (renderOwnsFrame()) {
        release();
        return;
    }

    const float want = desiredInterval(tps);
    if (want <= 0.0f) {
        release();
        return;
    }

    auto* director = cocos2d::CCDirector::sharedDirector();
    if (!director) {
        return;
    }
    const float live = director->getAnimationInterval();
    if (g_held && live == g_applied && live == want) {
        return;
    }
    if (!g_held) {
        g_before = live;
        g_held = true;
        if (auto* gm = GameManager::get()) {
            g_beforeCustomFps = gm->m_customFPSTarget;
            g_beforeVsyncVar = gm->getGameVariable(kVsyncKey);
            g_beforeUnlockVar = gm->getGameVariable(kUnlockKey);
            g_vsyncSeen = true;
            gm->setGameVariable(kVsyncKey, kVsyncOffValue);
            applyVerticalSync(kVsyncOffValue);
            gm->setGameVariable(kUnlockKey, kUnlockValue);
            skkbot2::log::info(
                "[TIMING] pinned display {:.1f} fps via GD's own rate "
                "(m_customFPSTarget {:.1f}->{:.1f}, vsync checkbox {}={}->{}, "
                "unlock gate {}={}->{}, restored on release)", 1.0 / want,
                static_cast<double>(g_beforeCustomFps),
                static_cast<double>(1.0 / want), kVsyncKey, g_beforeVsyncVar,
                kVsyncOffValue, kUnlockKey, g_beforeUnlockVar, kUnlockValue);
            if (1.0 / want <= kUnlockFloor) {
                skkbot2::log::warn(
                    "[TIMING] {:.1f} fps is at or below {:.0f}, so GD's own "
                    "updateCustomFPS() forces 60; the pin itself still runs at "
                    "{:.1f} but GD will not show that", 1.0 / want, kUnlockFloor,
                    1.0 / want);
            }
        }
        skkbot2::log::info("[TIMING] SDL pacing: {}", skkbot2::pacingState());
        if (const auto applied = skkbot2::applyPacingForPin(); !applied.empty()) {
            skkbot2::log::warn("[TIMING] {}", applied);
        }
    }
    if (auto* gm = GameManager::get()) {
        gm->m_customFPSTarget = static_cast<float>(1.0 / want);
        gm->updateCustomFPS();
        if (!g_verified) {
            g_verified = true;
            const auto gdInterval = director->getAnimationInterval();
            skkbot2::log::info(
                "[TIMING] GD's own updateCustomFPS() produced interval {:.6f} "
                "({:.1f} fps) for a target of {:.6f} ({:.1f} fps); unlock gate {} "
                "is {}, m_customFPSTarget is {:.1f}, m_vsyncEnabled is {}",
                gdInterval, 1.0 / gdInterval, want, 1.0 / want, kUnlockKey,
                gm->getGameVariable(kUnlockKey),
                static_cast<double>(gm->m_customFPSTarget), gm->m_vsyncEnabled);
        }
    }
    director->setAnimationInterval(want);
    if (live != want) {
        dropExtraDelta();
    }
    g_applied = want;
}

void noteStepCost(double seconds) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) return;
    if (g_emaStepCost == 0.0) {
        g_emaStepCost = seconds;
    } else {
        g_emaStepCost += (seconds - g_emaStepCost) * 0.1;
    }
}

double stepCostMs() {
    return g_emaStepCost * 1000.0;
}

int stepCap(double tps, double gameSpeed) {
    if (g_emaStepCost > 0.0 && std::isfinite(g_emaStepCost)) {
        double frameInterval = 1.0 / 60.0;
        if (auto* director = cocos2d::CCDirector::sharedDirector()) {
            const float interval = director->getAnimationInterval();
            if (interval > 0.0f && std::isfinite(interval)) {
                frameInterval = static_cast<double>(interval);
            }
        }
        const double adaptiveBudget = std::clamp(frameInterval * 1.5, kFrameTimeBudgetMin, kFrameTimeBudgetMax);
        double cap = adaptiveBudget / g_emaStepCost;
        if (!std::isfinite(cap)) {
            cap = static_cast<double>(kHardCap);
        }
        cap = std::max(cap, static_cast<double>(kFloorCap));
        cap = std::min(cap, static_cast<double>(kHardCap));
        return std::max(1, static_cast<int>(cap));
    }

    float fps = 0.0f;
    if (auto* director = cocos2d::CCDirector::sharedDirector()) {
        const float interval = director->getAnimationInterval();
        if (interval > 0.0f && std::isfinite(interval)) {
            fps = 1.0f / interval;
        }
    }
    if (!(fps > 0.0f)) {
        fps = 60.0f;
    }

    const double speed = std::max(1.0, gameSpeed);
    double need = std::ceil(tps / static_cast<double>(fps) * speed);
    if (!std::isfinite(need) || need < 1.0) {
        need = 1.0;
    }

    double cap = need * 4.0;
    cap = std::max(cap, static_cast<double>(kFloorCap));
    cap = std::min(cap, static_cast<double>(kHardCap));
    return std::max(1, static_cast<int>(cap));
}

}
