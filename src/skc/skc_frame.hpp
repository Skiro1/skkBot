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

private:
    FrameClock() = default;

    uint32_t m_frame = 0;
};

} // namespace skk::skc
