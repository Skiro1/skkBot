#pragma once

class GJBaseGameLayer;

namespace skkbot2::step_trace {

void refreshCache();
bool enabled();

void record(GJBaseGameLayer* layer, int frame, unsigned batch);
void recordSkc(GJBaseGameLayer* layer, int frame, unsigned batch,
               bool recording, bool playback);
void checkDrift(GJBaseGameLayer* layer, int frame, bool recording,
                bool playback, bool skc);
void finish();

}
