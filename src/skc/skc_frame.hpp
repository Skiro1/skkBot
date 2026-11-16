#pragma once

#include "skc_format.hpp"

#include <Geode/Geode.hpp>

namespace skk::skc {

// ---------------------------------------------------------------------------
// The frame counter — the SKC clock.
//
// SKK can trust `m_currentProgress / 2` because it pins position every tick
// anyway. SKC stores no physics, so its clock IS its truth: record and playback
// must advance it identically or the whole format is worthless.
//
// GD's own progress counter is useless for this: `m_currentProgress` is derived
// from level time at a fixed 480 Hz rate and is only corrected for TPS by the
// ReplayEngine's updateProgressbar hooks. Every reference bot that ships a
// physics-free format (silicate via the 0x238BAA midhook, astral via its own
// integer tick) counts physics steps itself instead. We do the same, but
// instead of a midhook we piggyback on processCommands, which GD calls once
// per physics step — the same source the SKK step clock uses.
//
// The counter resets on level (re)start and on editor playtest entry, and is
// monotone within a single attempt.
// ---------------------------------------------------------------------------
class FrameClock {
public:
    static FrameClock& get();

    // Called from the processCommands hook: exactly one physics step happened.
    void tick();

    // Called on resetLevel / fullReset / editor playtest start.
    void reset();

    [[nodiscard]] uint32_t getFrame() const { return m_frame; }
    void setFrame(uint32_t frame) { m_frame = frame; }

    // BUG-70 levelTime-deficit probe. processCommands is the per-iteration
    // physics signal, and the dt it carries is exactly the time that
    // iteration integrates; m_levelTime must accumulate sum(dt) / warp.
    // Counting calls split by half/full and summing the dt lets the
    // [BAT] window probe separate "update integrated less time" from
    // "levelTime was written less than what was integrated".
    void noteCall(float dt, bool isHalfTick) {
        if (isHalfTick) {
            ++m_probeHalf;
        } else {
            ++m_probeFull;
        }
        m_probeDtSum += static_cast<double>(dt);
    }
    void probeReset() {
        m_probeHalf = 0;
        m_probeFull = 0;
        m_probeDtSum = 0.0;
    }
    [[nodiscard]] uint64_t probeHalf() const { return m_probeHalf; }
    [[nodiscard]] uint64_t probeFull() const { return m_probeFull; }
    [[nodiscard]] double probeDtSum() const { return m_probeDtSum; }

private:
    FrameClock() = default;

    uint32_t m_frame = 0;
    uint64_t m_probeHalf = 0;
    uint64_t m_probeFull = 0;
    double m_probeDtSum = 0.0;
};

} // namespace skk::skc
