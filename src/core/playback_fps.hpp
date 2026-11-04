#pragma once

#include <Geode/Geode.hpp>

namespace skkbot2 {

// Display-FPS control for macro sessions.
//
// WHY THIS EXISTS. The scheduler runs floor(carried_time / step) whole 1/tps
// steps per frame, where carried_time is real elapsed time. The loop used to be
// capped at a CONSTANT 8 batches, which happened to be twice what 240 tps needs
// at 60 fps - so it looked fine and then stopped being fine:
//
//     fps | steps needed (tps=240) | cap 8 | result
//     120 |            2           |   8  | ok
//      60 |            4           |   8  | ok
//      30 |            8           |   8  | exactly at the limit
//      20 |           12           |   8  | the macro plays 1.6x slow
//      10 |           24           |   8  | the macro plays 3x slow
//
// And it failed SILENTLY: the leftover carry was simply clamped away, so a
// stall showed up as "the bot is a bit slow today" and never as an error. Both
// reference bots derive the budget from the frame rate instead of fixing it -
// Echo computes it from the animation interval (`unlocked_step_cap()`),
// Silicate makes it a setting and adds a `dynamic_upr` that measures actual
// seconds-per-step. This does the Echo version: the budget follows the FPS that
// is really in effect, so a slow display costs headroom, never correctness.
//
// WHAT IS NOT TOUCHED. The step itself stays `1/runtimeTickRate` rounded once
// (§3). Changing the display rate changes HOW MANY of those steps run per
// frame, never their length, so a macro recorded at 240 tps reproduces at 240
// tps whether the game draws it at 30 fps or 240. That is the whole reason the
// feature is safe to add: it is a scheduling decision, not a physics one.
//
// Model (one toggle + one number, the same two controls as Echo):
//   off          - never touch the animation interval; the budget still adapts
//   on, fps = 0  - pin the display to TPS: one physics step per drawn frame
//   on, fps > 0  - pin the display to that rate
//
// "fps = 0" is Echo's default behaviour (`set_level_interval(1.f / rate)`) and
// it is the setting that removes the wall clock from the picture entirely.
//
// VERTICAL SYNC IS PART OF THE TOGGLE, NOT A SEPARATE CONCERN.
//
// setAnimationInterval decides when the game WANTS to draw; the compositor
// decides when it is ALLOWED to. With vsync on, the presentation blocks on the
// display refresh (180 Hz here), so a request for 60 simply becomes "wait
// longer" and the level runs at whatever the panel does - the setting appears to
// hold and changes nothing. That is not a rounding detail, it is the whole
// mechanism failing to engage.
//
// So pinning also switches GD's own vsync flag off, and turning the toggle back
// off restores whatever the flag said before. Restoring is not politeness: a
// vsync state that outlives the session would leave the user's game visibly
// different from how they configured it, on a machine where they came to look
// at a bot, not at their display settings.
//
// WHAT WAS WRONG HERE, IN WRITING, because it cost a day.
//
// The paragraph this replaces claimed that GameManager::m_vsyncEnabled was "a
// field whose name says what it is, and which GD reads to decide how it
// presents". The run log disproved the second half in one line:
//
//     pinned display 240.0 fps ... vsync false->false
//
// False before the write. It was never on, so nothing turned off. A name that
// says what a field is turns out to be a HYPOTHESIS about the field; the
// evidence is watching the field change the thing it was named after. Two
// hours went into game variable "0116" the day before, and this was the same
// mistake with better manners - the difference is only that this one had a
// plausible-sounding justification attached.
//
// The switch is the SDL MOD's (`zmx.sdl` settings `disable-vsync` and
// `uncap-framerate`, read out of geode.settings), consumed by the presenter at
// startup. No GD field and no game variable moves it. This mod cannot either, so
// it does the only thing left: read them and say so, rather than leaving a
// setting that looks broken instead of blocked.

// What stands between a pinned FPS and the display actually running at it.
//
// Raw state of the two SDL-mod settings as "key=value, key=value", or why they
// are not there at all - empty when nothing is in the way, otherwise a sentence
// naming the SDL mod settings that are off. Printed on every pin, so the next
// report about this feature is a reading rather than an inference - the last two
// rounds were both spent reasoning about a setting nobody had read.
//
// The switch is NOT in GD. Per the Echo reference (mac_step.cpp:62) it is the SDL
// mod's `disable-vsync` and `uncap-framerate`, read out of geode.settings and
// consumed by the presenter at startup - which is why no GameManager field and
// no game variable moves it, and why it read false in the run that "did not turn
// vsync off". The checkbox itself is written through Mod::setSettingValue by
// applyPacingForPin() and put back by restorePacing().
[[nodiscard]] std::string pacingState();

// Turns disable-vsync and uncap-framerate ON, remembering what they were, and
// returns a sentence saying what changed.
//
// The box ends up UNTICKED, which is the whole point and the part that was
// missing when the only behaviour was to report it. The RUNNING session is
// unaffected either way: the presenter consumed the old value at startup and
// nothing a mod does later reaches it. So the message says a restart is needed
// instead of implying it worked now.
std::string applyPacingForPin();

// Puts both back. Called from release(), on the same terms as the interval.
void restorePacing();

namespace fps {

// The FPS value the GUI uses for "follow TPS" (one physics step per drawn
// frame). It is a CONTROL value, not a storage value - see MacroDisplayRate.
inline constexpr double kFollowTps = -1.0;

// --- what a macro says about the display rate -------------------------------
//
// THREE states, not two, and the wire format carries all three at no cost:
//
//   Absent     +0.0   the macro says nothing. FPS was off while recording, or
//                     the file predates the field, or a foreign importer built
//                     the macro. Playback must NOT touch FPS at all: a macro
//                     that says nothing about the display must not switch it on
//                     (the user's rule: "other macros have no fps, so do not
//                     enable it").
//   FollowTps  -0.0   recorded with FPS on and fps == tps. Playback turns FPS
//                     on and pins the display to the tick rate.
//   Pinned      n>0   recorded with FPS on at n != tps. Playback turns FPS on
//                     and pins n.
//
// Minus zero is the third state because it is the one bit pattern a float has
// spare: -0.0 compares equal to +0.0 under every comparison and disappears
// under every arithmetic operation, but the sign bit survives the raw IEEE
// write both formats use (skc_format.cpp readLeFloat/writeLeFloat and
// skk3_format.cpp writeLE<double> copy bits, they do not convert). So the
// third state costs no bytes and no version bump. Old files contain +0.0 and
// therefore decode as Absent, which is the correct reading of them.
struct MacroDisplayRate {
    enum class Kind { Absent, FollowTps, Pinned };

    Kind kind = Kind::Absent;
    double fps = 0.0;

    static MacroDisplayRate absent() { return {}; }

    static MacroDisplayRate followTps() {
        MacroDisplayRate rate;
        rate.kind = Kind::FollowTps;
        return rate;
    }

    static MacroDisplayRate pinned(double value) {
        MacroDisplayRate rate;
        rate.kind = Kind::Pinned;
        rate.fps = value;
        return rate;
    }

    // The only place the sign of zero is read. Everything else asks `kind`.
    [[nodiscard]] static MacroDisplayRate fromWire(double stored);

    // +0.0 / -0.0 / n, ready for Header::fps or SkMacro::displayFps.
    [[nodiscard]] double toWire() const;

    [[nodiscard]] bool present() const { return kind != Kind::Absent; }
    [[nodiscard]] bool followsTps() const { return kind == Kind::FollowTps; }
};

void setEnabled(bool on);
void setTarget(double fps);
[[nodiscard]] bool enabled();
[[nodiscard]] double target();

// "Lock delta": exactly one physics step per drawn frame, no carry and no
// catch-up. Echo calls the same thing lock_display_fps and describes it as
// "moves the level exactly one frame for every frame drawn, essentially smooth
// fix".
//
// It is a companion to the FPS toggle, not a substitute for it, and the two are
// not interchangeable: with FPS at 0 the display interval already IS 1/tps, so
// one step per frame happens by itself and locking changes nothing. Pinning a
// display rate LOWER than the tick rate is where it bites - 60 fps against 240
// tps wants four steps a frame, and locking forces one, which runs the level at
// a quarter of its tick rate. That is the same "GD mysteriously runs slow"
// report lock delta earns when it is switched on without knowing what the
// display rate is.
//
// LOCK DELTA is not a setting and is not declared here: it has no setter, no
// saved value, and the menu no longer offers a button for it. It engages
// whenever a display rate is actually being held - the menu toggle or a macro's
// rate (physicsbypass.cpp: `fps::macroHolds() || fps::enabled()`) - and is
// skipped while the renderer owns the frame, for the reason above.
//
// The name came from Echo's variable, and Echo's own menu label disagrees with
// it: GUI/pages.cpp:584 binds the row labelled Lock FPS to logic.lock_delta -
// one feature, two names, and we inherited both.
//
// Implemented in physicsbypass.cpp:
//
//     if (locked) frameDt = CCDirector::sharedDirector()->getAnimationInterval();
//
// (Echo, updater.cpp:341-344.) The MEASURED frame time is replaced by the
// CONFIGURED interval and the ordinary planner derives the step count from
// that, so the level advances by exactly 1/fps per drawn frame however long
// the frame really took. Deterministic, and unlike clamping steps to 1 it is
// correct at any tps/fps pair: the clamp coincides only at fps == tps and ran
// the macro at a quarter speed at fps 60 / tps 240.
//
// Which is also why the substitution is gated on the display really delivering
// the interval (the cadence window in physicsbypass.cpp): pinning a rate the
// machine cannot reach and then advancing the clock by it makes the level run
// SLOW, and that failure is the one this whole family of features was blamed for.

// Called from the scheduler hook on every frame the bot is engaged. Idempotent
// and cheap: it compares the desired interval against the live one and only
// calls setAnimationInterval when they differ, so it survives the user moving
// the slider mid-run, another mod touching the interval, and GD re-setting it
// on level load without any of those becoming a fight.
//
// The engine is NOT passed in. This header is included from places that do not
// have the full ReplayEngine definition yet, and a forward declaration here
// would make those fail on an incomplete type - the whole module needs from the
// engine is the tick rate and the game speed, both plain doubles.
void sync(double tps, double gameSpeed);

// Put back whatever interval was live before us. Must be called from every
// exit path that could have held one: session stop, a level restart, and the
// transition back to vanilla scheduling. Leaking a pinned 30 fps into normal
// play would be far worse than never pinning at all.
//
// Also releases the FPS gate (0116) and m_customFPSTarget, both of which ARE
// saved and restored, never separately: a half released pin - right rate, gate
// still open - is a state nobody can reach on purpose and nobody can diagnose
// from the outside.
//
// Vertical Sync (0030) is the exception and deliberately so. It is NOT restored
// to what it was found: the requirement is "fps on -> vsync off, fps off -> vsync
// ON", and restoring the snapshot broke the second half whenever the user had
// run with vsync off already. release() forces it on and says in the log what it
// overrode. The snapshot is kept only so that log line is honest.
//
// Both are reset here and not anywhere else: g_verified (the GD read-back) and
// g_capHits (the step-budget counter) are per-pin facts, and both were set without
// ever being cleared, which silently disabled the two diagnostics this module
// exists to produce.
void release();

// Arms a read-only tracer on GameManager::toggleGameVariable, which prints the
// key behind every option GD's own settings flip. It exists because the field
// name m_vsyncEnabled turned out to be a hypothesis about a field rather than
// evidence about it - the run log showed it was already false before anything
// wrote to it. Nothing is modified: the hook logs and calls through.
bool installOptionKeyTracer();

// Called ONCE at mod startup. If the previous session left VSync off (the game
// was closed mid-pin, or the mod reloaded while a pin was held), this restores
// GD's VSync game variable and the SDL-mod's pacing settings so the user does
// not inherit stale state. Only runs when gui_fps_enabled is false — if FPS is
// on, the first sync() call will set the correct state anyway.
void restoreVsyncOnStartup();

// The per-frame step budget, derived from the MEASURED cost of one physics
// step - never from the frame rate.
[[nodiscard]] int stepCap(double tps, double gameSpeed);

// Feed the budget's view of what one physics step costs. Called once per
// executed step with that step's wall time.
//
// HISTORY, because the wrong anchor was tried first and it made things worse.
// The budget was then derived from the DELIVERED frame rate, and that is a
// self-reinforcing loop: a heavier frame lowers the delivered fps, the budget
// grows, the next frame runs even more steps, grows longer still, and the
// display collapses to 1 fps - measured on the user's machine at tps 3414
// within one session. The frame rate is an OUTPUT of the loop, not an input.
// The per-step cost is the input that actually decides how much physics a
// frame can afford, and it does not move when the budget moves, so the loop
// stays open.
void noteStepCost(double seconds);

// The current smoothed per-step cost, in milliseconds - for diagnostics and
// the budget-hit warning, so the log states the measured fact instead of
// guessing from rates.
[[nodiscard]] double stepCostMs();

// --- macro-carried rate -----------------------------------------------------
//
// The same contract TPS already has, one level up. A macro records the frame
// rate it was made under, and playback turns that into the live setting for the
// length of the session, then hands the previous setting back on stop - exactly
// what `engine->tickRate = declaredTps` / `= savedTickRate` does for TPS.
//
// The difference from the GUI toggle is ownership: the GUI setting belongs to
// the user and must survive a session untouched, so beginMacro() takes a COPY
// and endMacro() puts that copy back. Writing straight into setEnabled() would
// make "the menu now shows the macro's rate" true, which is not what anyone
// asked for and is the kind of thing nobody notices until the next session
// quietly runs at a rate nobody chose.
//
// ALL THREE STATES ENGAGE, including Absent. A macro with no FPS information
// has to be able to say "turn FPS off for this run", otherwise playing it on a
// machine where the user left FPS on would run it at a rate it was never
// recorded under. Absent therefore saves the menu state, forces `enabled` off,
// and endMacro() hands the menu state back like every other state.
[[nodiscard]] bool beginMacro(MacroDisplayRate const& rate);
void updateMacro(MacroDisplayRate const& rate);
void endMacro();

// True while a macro owns the display rate. The GUI no longer freezes the row
// because of it (playback is editable, recording is not - see setEnabled), but
// it needs it to show the rate actually in force instead of the saved one.
[[nodiscard]] bool macroHolds();

// True once the user has changed the rate BY HAND during this session. From
// then on updateMacro() stops re-asserting the macro's number, because a
// respawn re-arms the environment and would otherwise silently undo the edit
// the user just made.
[[nodiscard]] bool macroUserOverride();

// What a recording should store:
//   FPS off                  -> Absent  (+0.0, field meaningless)
//   FPS on, rate == tps      -> FollowTps (-0.0)
//   FPS on, rate != tps      -> Pinned (n)
[[nodiscard]] MacroDisplayRate captureForStorage(double tps);

// Measurement. `capHits` counts frames where the loop stopped at the budget
// instead of at the carry - that is the exact condition under which a macro
// cannot keep up, and it used to be invisible.
[[nodiscard]] unsigned long long capHits();
void noteCapHit();
void resetCounters();

}  // namespace fps
}  // namespace skkbot2