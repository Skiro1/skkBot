#pragma once

#include "skc_format.hpp"

#include <Geode/Geode.hpp>

namespace skk::skc {

class FrameClock {
public:
    static FrameClock& get();

    void tick();

    void reset();

    [[nodiscard]] uint32_t getFrame() const { return m_frame; }
    void setFrame(uint32_t frame) { m_frame = frame; }

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

}
