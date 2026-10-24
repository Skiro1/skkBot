#pragma once

#include <Geode/Geode.hpp>

#include <functional>

class FrameStepper {
public:
    static FrameStepper& get() {
        static FrameStepper instance;
        return instance;
    }

    bool isEnabled() const;
    void setEnabled(bool enabled);
    void toggle();

    int getStepCount() const { return m_stepCount; }
    void setStepCount(int count);

    // `fromMenuButton` selects which input contract applies:
//   false (the keybind) — press owes the configured step count AND holding keeps
//                         repeating, always, regardless of Hold To Step.
//   true  (the menu STEP button) — obeys Hold To Step: off means tap only (a
//                         hold adds nothing), on means hold only (a press steps
//                         nothing at all).
void stepForward(int frames = 0, bool fromMenuButton = false);

    bool isHoldToStepEnabled() const { return m_holdEnabled; }
    void setHoldToStepEnabled(bool enabled);

    // Auto-pause on death: while ON, a death detected while stepping releases
    // the freeze so GD can play the death animation and respawn, then re-freezes
    // automatically. Independent of the master switch so it can be armed once and
    // left alone.
    bool isAutoPauseOnDeathEnabled() const { return m_autoPauseOnDeath; }
    void setAutoPauseOnDeathEnabled(bool enabled);
    void updateAutoPause();

    float getHoldDelay() const { return m_holdDelay; }
    void setHoldDelay(float delay);

    int getHoldSpeed() const { return m_holdSpeed; }
    void setHoldSpeed(int speed);

    bool consumeAdvanceStep() { return false; }

    bool updateScheduler(cocos2d::CCScheduler* scheduler, float dt);

    void onResetLevel();
    void onPlayLayerDestroy();

    void setHoldState(bool active, bool forward);
    void setKeyHoldState(bool held, bool forward);
    bool isKeyHeld() const { return m_keyHeld; }

    // Live "is the mouse button physically down" probe for the menu's STEP
    // hold. The menu is drawn in the imgui swapBuffers hook, i.e. AFTER the
    // scheduler already ran for the frame, so a hold flag written from the GUI
    // is always one frame stale: the core kept stepping once after the button
    // came up ("отпускаю — происходит ещё сдвиг"). The GUI registers a probe
    // that reads the OS state directly, which the scheduler can consult in the
    // same tick it is deciding to step.
    void setHoldProbe(std::function<bool()> probe);

private:
    FrameStepper();
    ~FrameStepper() = default;

    int m_stepCount = 1;
    bool m_holdEnabled = true;
    float m_holdDelay = 0.15f;
    int m_holdSpeed = 1;
    bool m_autoPauseOnDeath = false;
    bool m_autoPaused = false;

    bool m_nextFrame = false;
    // Frames still owed from the initial press (the configured step count minus
    // the one frame that fires immediately). This queue is drained BEFORE a
    // held key is allowed to become a hold-repeat, and is never cancelled, so a
    // tap always delivers the full count from the menu.
    int m_pendingClick = 0;

    // Frames owed by a key press that has NOT been resolved yet. A press and a
    // hold only become distinguishable after m_holdDelay: released inside the
    // window it is a tap (this whole count fires at once), still held it is a
    // hold (discarded, the repeat takes over). Nothing steps during the window,
    // which is what removes the initial lurch when the key is held down.
    int m_keyTapPending = 0;

    bool m_holdActive = false;
    bool m_holdForward = true;
    bool m_keyHeld = false;
    bool m_keyForward = true;
    std::function<bool()> m_holdProbe;
    float m_holdTimer = 0.0f;
    int m_holdTickCounter = 0;
};
