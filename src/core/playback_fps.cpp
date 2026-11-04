#include "core/playback_fps.hpp"

#include "logger/logger.hpp"
#include "render/renderer.hpp"

#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlatformToolbox.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/modify/CCDirector.hpp>

#include <algorithm>
#include <cmath>

// The frame-time budget: how much wall time one drawn frame may spend running
// physics steps before the batch loop must stop. An adaptive budget clamped
// between 16ms and 32ms prevents the display from collapsing below 30-60 FPS
// during intense click-storms / spam sections, while leaving enough room
// for full tick processing.
static constexpr double kFrameTimeBudgetMin = 0.016;
static constexpr double kFrameTimeBudgetMax = 0.032;
static constexpr int kFloorCap = 8;
// The absolute bound. The carry only ever grows by real elapsed time, so
// reaching this needs a multi-second frame (a long alt-tab, a breakpoint). The
// bound is what stops the next frame from owing a second of steps in turn.
static constexpr int kHardCap = 512;

// The measured cost of ONE physics step, in wall seconds - the input the step
// budget is derived from.
//
// HISTORY, because both wrong anchors were tried first and the second made
// things worse. The budget was a constant 8 batches until the display dropped:
// at 20 fps a frame owed 12 steps, the loop stopped at 8, and the leftover
// carry was clamped away - the macro played slower with nothing logged. It was
// then derived from the CONFIGURED interval, which lies whenever the machine
// cannot hold the setting (pinned 360 fps, delivering 90). The last attempt
// derived it from the DELIVERED frame rate - and that is a self-reinforcing
// loop: a heavier frame lowers the delivered fps, the budget grows, the next
// frame runs even more steps, grows longer still, and the display collapses.
// Measured on the user's machine at tps 3414: 180 fps -> 1 fps within one
// session. The frame rate is an OUTPUT of that loop, not an input.
//
// The per-step cost is the input that actually decides how much physics a
// frame can afford, and it does not move when the budget moves, so the loop
// stays open. cap = kFrameTimeBudget / cost: the system settles at the
// fastest sustainable pace, frames never exceed ~50 ms of physics, and
// slowdown remains only when tps * cost > 1 - i.e. the machine genuinely
// cannot run that many steps per second.
static double g_emaStepCost = 0.0;

// The Vertical Sync checkbox. Established by observation on 19.10.2026: the
// tracer caught GD passing these keys to toggleGameVariable, one per checkbox the
// user clicked - 0030 eleven times, 0115 twice, 0023 six times, 0116 twice - and
// the user identified 0030 as Vertical Sync. Polarity is the only thing left
// taken on faith, and it is taken in the plain direction: a checkbox labelled
// Vertical Sync is true when ticked, so switching vsync off is false. Confirmed
// by the run that followed: it logged 0030=true->false and the user saw the box
// come off.
static constexpr const char* kVsyncKey = "0030";
static constexpr bool kVsyncOffValue = false;
static constexpr bool kVsyncOnValue = true;

// "Unlock FPS", and it was wrong to leave it alone.
//
// Disassembled 19.10.2026 from GeometryDash.exe, GameManager::updateCustomFPS at
// RVA 0x18AC40, with the constants read out of the binary rather than guessed:
//
//   void GameManager::updateCustomFPS() {
//       if (!getGameVariable("0116")) return;         // <- the gate, literal "0116"
//       if (m_customFPSTarget > 60.0f)
//           CCDirector::sharedDirector()->setAnimationInterval(1.0f / m_customFPSTarget);
//       else
//           CCDirector::sharedDirector()->setAnimationInterval(1.0 / 60.0);
//   }
//
// string 0x14055D5E8 = "0116", threshold 60.0f, numerator 1.0f, fallback
// 0.0166666675. m_customFPSTarget lives at this+0x430.
//
// This explains the symptom that sent us here. The log printed
// "interval now 0.016667" on a 117 fps pin: with 0116 false the function returns
// at the first line and the interval is simply left at GD's own 1/60, so
// m_customFPSTarget was being set and then ignored. The user's warning - "0116 is
// the unlock fps checkbox, but not the unlock itself" - was right about the
// second half and the disassembly is right about the first: it is BOTH, and it
// only bites once m_customFPSTarget is also above 60.
//
// Writing it here is not the same write as the 19.10 rollback. That one set 0116
// and m_customFPSTarget with no idea what either did. Now both are known: the
// gate gates exactly one call - setAnimationInterval - and touches no game state.
// Per-frame setAnimationInterval below remains authoritative regardless, because
// GD calls updateCustomFPS from only three sites (0x32F5CC, 0x37AF16, 0x37B28F -
// two of them inside MoreVideoOptionsLayer, which is android-only).
static constexpr const char* kUnlockKey = "0116";
static constexpr bool kUnlockValue = true;

// m_customFPSTarget must exceed this or GD's own path forces 60. Read out of the
// comiss at 0x14018AC9C, not guessed.
static constexpr float kUnlockFloor = 60.0f;

// How vsync is actually switched, and why writing the variable alone was never
// going to be enough.
//
// Disassembled 19.10.2026. The options page (RVA 0x378E00) builds its toggles
// with addToggle(label, key, description), and the four video ones read straight
// out of the binary's string table:
//
//   0x378EE5  "Fixes screen tearing in fullscreen mode."  -> addToggle(..., "0030", ...)  "Vertical Sync"
//   0x378F02  "Makes some optimizations that can reduce..."  -> addToggle(..., "0023", ...)  "Smooth Fix"
//   0x378F1F  "Shows frames per second while playing."     -> addToggle(..., "0115", ...)  "Show FPS"
//   0x378F3C  "Lets you set a custom target frames per ..."-> addToggle(..., "0116", ...)  "Unlock FPS"
//
// Then, at 0x37A8BF, the same page applies the first one:
//
//   getGameVariable("0030")  ->  CCApplication::sharedApplication()->toggleVerticalSync(value)
//
// and both of those are imports, not GD code:
//
//   libcocos2d.dll!?sharedApplication@CCApplication@cocos2d@@SAPEAV12@XZ
//   libcocos2d.dll!?toggleVerticalSync@CCApplication@cocos2d@@QEAAX_N@Z
//
// So the variable is only STORAGE. GD reads it and hands it to cocos2d at three
// setup points, and nothing re-reads it after that. setGameVariable("0030", ...)
// on its own moves the checkbox and leaves the display exactly as it was - which
// is what every version up to this one did.
//
// PlatformToolbox::toggleVerticalSync is the same cocos2d function, reached
// through GD's own bindings.
inline void applyVerticalSync(bool enable) {
    PlatformToolbox::toggleVerticalSync(enable);
}

namespace {

bool g_enabled = false;
double g_target = 0.0;

// Interval bookkeeping. g_before is the interval we replaced, and it is the
// ONLY thing that is restored - the previous value, never a hardcoded 1/60,
// because the user's own FPS setting and GD's own options both live here.
bool g_held = false;
float g_before = 0.0f;
float g_applied = -1.0f;

// GD's own frame-rate state, and its vsync flag. Both are saved on the first
// frame of a pin and put back on release.
//
// THE POINT OF m_customFPSTarget + updateCustomFPS(). Setting the animation
// interval on its own is NOT how GD runs at a chosen rate - it is the low-level
// half of it. GD keeps the number the user asked for in m_customFPSTarget and
// applies it through updateCustomFPS(); the interval is a result of that. So a
// pin that writes only the interval shows up in nothing GD reads back, and the
// first thing GD does on a path we do not own - options, resize, level load -
// put the rate it remembers back. The user saw exactly that: the counter in GD
// did not move. This is the function the user meant when they said the setting
// had to use what GD itself has.
//
// m_vsyncEnabled sits next to it in the same cluster and means what its name
// says: whether the compositor is allowed to gate presentation. With it on, a
// request for 60 becomes "wait longer" on a 180 Hz panel.
bool g_vsyncSeen = false;
bool g_beforeVsyncVar = false;
bool g_beforeUnlockVar = false;
bool g_verified = false;
float g_beforeCustomFps = 0.0f;

unsigned long long g_capHits = 0;

// The user's own GUI state, saved while a macro holds the display rate. Kept
// apart from g_enabled/g_target on purpose: those two ARE the GUI setting, and
// a macro that wrote into them directly would permanently move the control the
// user set for the next session.
bool g_macroHeld = false;
bool g_savedEnabled = false;
double g_savedTarget = 0.0;

// What the macro last put in force. The GUI mirrors these every frame while a
// macro holds, so a push that DIFFERS from them is a user edit and nothing else
// - that is the whole override detector. Without it, every frame's push would
// look like an edit and the macro's rate would never survive.
bool g_macroEnabled = false;
double g_macroTarget = 0.0;
bool g_userOverride = false;

void applyMacroRate(skkbot2::fps::MacroDisplayRate const& rate) {
    using Kind = skkbot2::fps::MacroDisplayRate::Kind;
    switch (rate.kind) {
        case Kind::Absent:
            // "The macro says nothing" is not "leave it as it is": the run was
            // captured with FPS off, so it must execute with FPS off.
            //
            // The target is left alone (the row still shows the menu's number
            // next to the disabled toggle), but g_macroTarget has to follow it.
            // The GUI mirrors the live target every frame and pushes it back;
            // if g_macroTarget kept an older number, that mirror would look
            // like a hand edit on the very first frame and permanently override
            // the macro.
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
            // Defensive clamp: every writer validates its own range, but a
            // foreign importer can hand us 0.4, and a sub-1 fps pin means a
            // 2.5-second frame - the game would look frozen rather than slow.
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

// True while a render owns the frame. Echo bails out of sync_playback_fps on
// exactly this condition and so must we: the renderer feeds its own dt of
// 1/videoFps and batches six frames per drawScene, so pinning the display
// interval underneath it would change how many drawScene calls happen per
// second and nothing about the output - pure noise at best, and a fight with
// the renderer's own accounting at worst.
bool renderOwnsFrame() {
    return skkbot2::render::Renderer::get().isRecording();
}

float desiredInterval(double tps) {
    if (!g_enabled) {
        return -1.0f;  // not a valid interval: means "do not hold"
    }
    const double fps = (g_target == skkbot2::fps::kFollowTps) ? tps : g_target;
    if (!(fps > 0.0) || !std::isfinite(fps)) {
        return -1.0f;
    }
    return static_cast<float>(1.0 / fps);
}

// Changing the interval leaves GD's own m_extraDelta holding a remainder
// computed against the OLD interval. One anomalous frame is harmless and the
// carry would absorb it, but it costs a step and it shows up in the drift
// meter, so clear it rather than let it be attributed to something else.
void dropExtraDelta() {
    if (auto* play = PlayLayer::get()) {
        play->m_extraDelta = 0.0f;
    }
}

// The menu's own saved slider value. g_target is a live module cell that the
// GUI's every-frame push overwrites with the kFollowTps sentinel whenever the
// toggle is off - so "off at 0.0" in a snapshot line can mean "the menu slider
// is anywhere". Reading the persisted value here makes the begin/end pair
// comparable across sessions: if the number drifts, the corruption is real and
// this line is the evidence; if it does not, 0.0 was just the sentinel.
double menuSavedFps() {
    return geode::Mod::get()->getSavedValue<double>("gui_fps_value", 0.0);
}

}  // namespace

namespace skkbot2::fps {

void restoreVsyncOnStartup() {
    // If FPS is currently disabled (gui_fps_enabled == false), the previous
    // session may have left VSync off. This happens when:
    //   - the game crashes or is force-closed while a pin is active;
    //   - the user disables FPS, but the variable "0030" was already written
    //     to CCGameManager.dat as false and never restored.
    //
    // Called from $on_mod(Loaded) after gui.init() pushes the saved
    // gui_fps_enabled into the module. GameManager is alive by this point.
    auto* gm = GameManager::get();
    if (!gm) return;

    const bool vsyncVar = gm->getGameVariable(kVsyncKey);
    if (!vsyncVar) {
        // VSync was left off by a previous session. Restore it.
        gm->setGameVariable(kVsyncKey, kVsyncOnValue);
        applyVerticalSync(kVsyncOnValue);
        skkbot2::log::info(
            "[TIMING] startup: VSync was off ({}=false) from a previous session "
            "- restored to on. FPS is currently disabled, so nothing should have "
            "turned it off.", kVsyncKey);
    }

    // Also restore SDL pacing if it was left dirty.
    skkbot2::restorePacing();
}

void setEnabled(bool on) {
    // A push that differs from what the macro put in force is a user edit: the
    // GUI mirrors the live state every frame while a macro holds the rate, so
    // the only way the value can change is a click. Record it as an override so
    // the next re-arm (practice respawn, editor playtest entry) does not undo
    // it, and make it the state endMacro() hands back.
    if (g_macroHeld && !g_userOverride && on != g_macroEnabled) {
        g_userOverride = true;
        // Only the toggle changes. The target keeps whatever it had BEFORE the
        // session: the user touched the checkbox, not the slider, and restoring
        // the macro's own number afterwards would silently rewrite the menu
        // setting they never edited.
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
        // BUG (02.11): this branch used to write g_savedEnabled = g_enabled
        // too. But g_enabled at that moment is what the MACRO put in force,
        // not what the menu had saved - a macro holding FollowTps/users-on
        // state turned every slider edit into "the user also flipped the
        // toggle", and endMacro() then handed the menu back ON (the macro's
        // state) instead of the user's. The toggle has exactly one writer:
        // setEnabled(), which snapshots it correctly against g_macroEnabled.
        // A slider grab must only move the number.
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
    // g_verified was set and never cleared anywhere, so the read-back below -
    // the one line this whole feature was built to produce - printed once for
    // the lifetime of the process and then never again. Every later pin (every
    // macro run, every slider move, every rate worth checking) was unverified,
    // silently. It has to be cleared here, next to the state it describes.
    g_verified = false;
    // Same reasoning for the step-budget counter, except the failure is quieter:
    // noteCapHit() is throttled to hits==1 || hits==10 || hits%600==0, so after
    // 600 lifetime hits the hits==1 line never fires again and the first warning
    // of every later session is gone. resetCounters() existed for this and had
    // no caller at all, which is why it is called rather than the field poked.
    resetCounters();
    g_applied = -1.0f;
    if (auto* director = cocos2d::CCDirector::sharedDirector()) {
        if (g_before > 0.0f) {
            director->setAnimationInterval(g_before);
        }
    }
    // GD's frame-rate state goes back exactly as it was found, through the same
    // GD method that took it, so release takes the same path a user's own change
    // would. An interval restored under a panel that is still unlocked, or under
    // a target GD still believes is 240, is a state the user did not choose and
    // cannot see.
    //
    // vsync is the one exception and is NOT restored - see below.
    if (auto* gm = GameManager::get(); gm && g_vsyncSeen) {
        gm->m_customFPSTarget = g_beforeCustomFps;
        gm->setGameVariable(kUnlockKey, g_beforeUnlockVar);
        gm->updateCustomFPS();
        // vsync goes ON here, unconditionally, and this is a deliberate change
        // of policy rather than a bug fix.
        //
        // Restoring the snapshot was faithful to the letter of "put back what
        // was found" and wrong in practice. The user had it off already - the
        // log said so, "vsync checkbox 0030=false->false" - so releasing put it
        // back off, and the pairing the feature promises (fps on -> vsync off,
        // fps off -> vsync on) simply did not hold. Their words: "при выключении
        // fps должен включаться vsync, а он не включается".
        //
        // The snapshot is still logged, because a user who deliberately runs
        // with vsync off should be able to see that this overrode it.
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
        // The user chose a rate by hand. A respawn re-arms the whole execution
        // environment, and re-asserting the macro here would silently snap the
        // display back to a number the user just overrode - the feature would
        // work for one attempt and then "forget".
        return;
    }
    applyMacroRate(rate);
}

void endMacro() {
    if (g_macroHeld) {
        g_macroHeld = false;
        g_userOverride = false;
        // Restore the menu's own state BEFORE dropping the pin, so the frame
        // after this one is governed by the setting the user actually chose.
        // The other order leaves the interval released but the pin immediately
        // re-taken at the macro's rate on the next sync(), because the macro's
        // value is still in g_target - a session that outlives itself by exactly
        // one frame.
        //
        // g_saved* is also where a hand edit lands (setEnabled/setTarget write
        // it), so this one line covers both cases: untouched -> the state from
        // before the session; edited -> the value the user last chose.
        g_enabled = g_savedEnabled;
        g_target = g_savedTarget;
        skkbot2::log::info("[TIMING] macro display rate released (menu back to {} "
                           "at {:.1f}, menu slider saved: {:.1f})",
                           g_enabled ? "on" : "off",
                           g_target == skkbot2::fps::kFollowTps ? 0.0 : g_target,
                           menuSavedFps());
    }
    // Unconditional, and not an early return on !g_macroHeld: a session can hold
    // a pin entirely from the menu setting, with no macro involved. Making this
    // the stop-path call means it has to cover that case too, or stopping a
    // macro would quietly leave the menu's own pin in place.
    release();
}

bool macroHolds() { return g_macroHeld; }
bool macroUserOverride() { return g_userOverride; }

MacroDisplayRate MacroDisplayRate::fromWire(double stored) {
    if (!std::isfinite(stored)) {
        return absent();
    }
    // The one place the sign bit of zero is read. -0.0 is the "fps == tps"
    // marker; every comparison treats it as zero, so this cannot be written as
    // `stored < 0.0` (which is false for -0.0).
    if (std::signbit(stored)) {
        return followTps();
    }
    if (stored == 0.0) {
        return absent();  // +0.0: the macro carries no display rate
    }
    return pinned(stored);
}

double MacroDisplayRate::toWire() const {
    switch (kind) {
        case Kind::FollowTps:
            // copysign readably guarantees -0.0; `return -0.0;` relies on the
            // compiler keeping the sign of a folded constant.
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
    const double live = (g_target == skkbot2::fps::kFollowTps) ? tps : g_target;
    if (!std::isfinite(live) || !(live > 0.0)) {
        return MacroDisplayRate::absent();
    }
    // Equal to the tick rate? Then the macro wants the display pinned to the
    // tick grid, which is exactly what the follow-tps marker says. Storing the
    // number twice would create two sources of truth and a tie to break.
    if (std::abs(live - tps) < 1e-3) {
        return MacroDisplayRate::followTps();
    }
    return MacroDisplayRate::pinned(live);
}

void sync(double tps, double gameSpeed) {
    // No engagement check here on purpose. The caller is advanceNormalScheduler,
    // which only runs once the scheduler has already decided custom timing is
    // in effect for this frame - so "is a session live" is a fact this function
    // is handed, not a question it has to answer with a second guess. Every
    // path that stops a session calls release() instead.
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
    // The early-out below skips only the EXPENSIVE parts. The WRITE is not one
    // of them, and making it conditional is exactly why the setting did not
    // take: Echo's Logic::sync_playback_fps() is called from hooks.cpp:2208 on
    // EVERY frame and does one thing - setAnimationInterval(1/playback_fps).
    // Not once at load. GD re-asserts its own rate on paths nobody hooked, so a
    // one-shot write holds until the next such path and then reads as
    // "sometimes works". GD's own counter not moving was this.
    if (g_held && live == g_applied && live == want) {
        return;  // interval already ours; the write would be a no-op anyway
    }
    if (!g_held) {
        g_before = live;
        g_held = true;
        // First frame of a pin: take the snapshot, then write.
        //
        // The snapshot is taken under `!g_held` for a reason that is not
        // tidiness: without the guard a second pin would save the already-
        // written state and then "restore" it, leaving the user's box off and
        // GD's own target at our number for the rest of the session.
        if (auto* gm = GameManager::get()) {
            g_beforeCustomFps = gm->m_customFPSTarget;
            g_beforeVsyncVar = gm->getGameVariable(kVsyncKey);
            g_beforeUnlockVar = gm->getGameVariable(kUnlockKey);
            g_vsyncSeen = true;
            // Storage first, then the real switch. In that order: the applier
            // takes the value as an argument, so writing the variable first means
            // the two can never disagree if one of them is interrupted.
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
            // GD's own path silently ignores a target at or below 60 and uses
            // 1/60 instead (comiss at 0x14018AC9C). Our per-frame interval write
            // still holds the number, but the two disagree, and a user reading
            // only GD's behaviour would conclude the pin did nothing.
            if (1.0 / want <= kUnlockFloor) {
                skkbot2::log::warn(
                    "[TIMING] {:.1f} fps is at or below {:.0f}, so GD's own "
                    "updateCustomFPS() forces 60; the pin itself still runs at "
                    "{:.1f} but GD will not show that", 1.0 / want, kUnlockFloor,
                    1.0 / want);
            }
        }
        // The gate is not in GD - it is the SDL mod's settings (see
        // gd_option_tracer.cpp). Tick the boxes, say what it will take to make
        // them bite, and print the raw state so the next report is a reading.
        // Reporting alone was the previous behaviour and it was not enough: the
        // user asked for the box to END UP unticked, which is a different
        // request from being told which box to click.
        skkbot2::log::info("[TIMING] SDL pacing: {}", skkbot2::pacingState());
        if (const auto applied = skkbot2::applyPacingForPin(); !applied.empty()) {
            skkbot2::log::warn("[TIMING] {}", applied);
        }
    }
    // GD's own apply path, every frame while held, through the same entry point
    // its own settings screen uses. That is what makes GD's counter and state
    // agree with ours, and - together with the interval write below - what makes
    // the pin survive GD re-asserting its remembered rate on level load, resize
    // and the options screen. Once per frame is not paranoia: the alternative is
    // a setting that works until something else happens, which is the same shape
    // as the bug it replaces.
    if (auto* gm = GameManager::get()) {
        gm->m_customFPSTarget = static_cast<float>(1.0 / want);
        gm->updateCustomFPS();
        // What GD's OWN path produced, read back on the first held frame. This is
        // the line that exposed the missing gate: it printed 0.016667 on a 117
        // fps pin, because with 0116 false updateCustomFPS() returns at its
        // first instruction and leaves the interval alone. Reported against the
        // number we asked for so the two can be compared by eye.
        //
        // m_vsyncEnabled is printed too, not because it is expected to matter -
        // it is not the switch, the SDL mod's settings are not either - but
        // because it costs nothing and it is the field whose name misled this
        // feature twice.
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
        // Only when the interval actually moved. Zeroing m_extraDelta on every
        // frame would be the "make it more precise" change this project has
        // already paid for once: it erases GD's own remainder and the drift
        // comes back as y.
        dropExtraDelta();
    }
    g_applied = want;
}

void noteStepCost(double seconds) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) return;
    if (g_emaStepCost == 0.0) {
        g_emaStepCost = seconds;
    } else {
        // ~10-sample time constant. Step costs are individually jittery
        // (scheduler housekeeping, cache misses), so smoothing is what makes
        // the derived budget stable; the fallback window below covers the
        // samples before the EMA has anything to stand on.
        g_emaStepCost += (seconds - g_emaStepCost) * 0.1;
    }
}

double stepCostMs() {
    return g_emaStepCost * 1000.0;
}

int stepCap(double tps, double gameSpeed) {
    // Budget = frame-time budget / measured cost of one step. Deliberately
    // independent of tps and gameSpeed: neither changes what one step costs,
    // and a fast machine simply never hits the cap (it clamps at kHardCap and
    // the carry decides how many steps actually run). The cap only binds when
    // physics is expensive enough that running every owed step would eat the
    // whole frame - which is exactly when slowdown is physically unavoidable.
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

    // No measurement yet (first frames of a session, or the batch loop has not
    // run): fall back to the configured interval - the original formula, which
    // is correct as a default and gets replaced by the measured cost as soon
    // as the first steps execute.
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
    // gameSpeed is not always >= 1 - a slow-motion setting under 1 needs no
    // more steps than normal, so clamping up at 1 is correct rather than merely
    // convenient.
    double need = std::ceil(tps / static_cast<double>(fps) * speed);
    if (!std::isfinite(need) || need < 1.0) {
        need = 1.0;
    }

    double cap = need * 4.0;
    cap = std::max(cap, static_cast<double>(kFloorCap));
    cap = std::min(cap, static_cast<double>(kHardCap));
    return std::max(1, static_cast<int>(cap));
}

}  // namespace skkbot2::fps