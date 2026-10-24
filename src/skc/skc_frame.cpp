#include "skc_frame.hpp"

namespace skk::skc {

FrameClock& FrameClock::get() {
    static FrameClock instance;
    return instance;
}

void FrameClock::tick() {
    ++m_frame;
}

void FrameClock::reset() {
    m_frame = 0;
}

} // namespace skk::skc
