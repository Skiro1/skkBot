#pragma once

class GJBaseGameLayer;

namespace skkbot2::step_trace {

// Refreshes the cached "gui_step_trace" flag. The per-step paths (record,
// recordSkc, checkDrift) run once per PHYSICS SUBSTEP - at tps 3414 that is
// 3414 calls/s, and each used to pay a geode getSavedValue JSON lookup before
// it could decide it had nothing to do. The cache is refreshed once per drawn
// frame from the scheduler hook instead; a GUI toggle takes effect on the
// next frame.
void refreshCache();
bool enabled();

// Called once after the SKK per-step capture/playback work has completed.
// The writer is dormant unless the user enables Step Trace in Settings.
void record(GJBaseGameLayer* layer, int frame, unsigned batch);
// SKC uses an independent transport state, so it cannot be inferred from the
// SKK ReplayEngine mode. The row format is shared to keep the comparator common.
void recordSkc(GJBaseGameLayer* layer, int frame, unsigned batch,
               bool recording, bool playback);
// Opt-in RECORD -> PLAY state comparison. Captures the live post-physics state
// before SKK reconciliation (or after the SKC physics step) and reports only
// the first exact mismatch for each playback session.
void checkDrift(GJBaseGameLayer* layer, int frame, bool recording,
                bool playback, bool skc);
void finish();

} // namespace skkbot2::step_trace
