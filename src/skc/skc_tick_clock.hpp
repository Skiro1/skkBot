#pragma once

namespace skkbot2::skc {
    // Cumulative physics dt consumed by processCommands. Advanced once per
    // completed physics step from the SkcBaseGameLayer::processCommands hook
    // (after the vanilla body, so a mid-body read gives the total through the
    // PREVIOUS step - exactly the interval a Cocos action started during the
    // current step has lived for when read at the next event).
    void advanceTickDt(double dt);

    // Total physics time (in game seconds) accumulated so far.
    double tickDtTotal();
}
