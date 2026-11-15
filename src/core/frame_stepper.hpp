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

void stepForward(int frames = 0, bool fromMenuButton = false);

    bool isHoldToStepEnabled() const { return m_holdEnabled; }
    void setHoldToStepEnabled(bool enabled);

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
    int m_pendingClick = 0;

    int m_keyTapPending = 0;

    bool m_holdActive = false;
    bool m_holdForward = true;
    bool m_keyHeld = false;
    bool m_keyForward = true;
    std::function<bool()> m_holdProbe;
    float m_holdTimer = 0.0f;
    int m_holdTickCounter = 0;
};
