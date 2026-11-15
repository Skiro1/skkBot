#pragma once

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>

inline bool isPhysicalKeyHeld(int button, bool isP2, bool twoPlayer, bool platformer, bool dual) {
#ifdef GEODE_IS_WINDOWS
    if (isP2 && !dual) {
        return false;
    }
    auto down = [](int vk) -> bool {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    const bool mouse = down(VK_LBUTTON);
    const bool space = down(VK_SPACE);
    const bool keyW = down('W');
    const bool up = down(VK_UP);
    const bool leftArrow = down(VK_LEFT);
    const bool rightArrow = down(VK_RIGHT);
    const bool keyA = down('A');
    const bool keyD = down('D');
    if (button == 1) {
        if (!twoPlayer) {
            return mouse || space || keyW || up;
        }
        if (!dual) {
            return mouse || space || keyW;
        }
        if (isP2) {
            return platformer ? up : (mouse || up);
        }
        return space || keyW;
    }
    if (button == 2) {
        if (!platformer) {
            return false;
        }
        if (!twoPlayer) {
            return keyA || leftArrow;
        }
        if (isP2) {
            return leftArrow;
        }
        return keyA;
    }
    if (button == 3) {
        if (!platformer) {
            return false;
        }
        if (!twoPlayer) {
            return keyD || rightArrow;
        }
        if (isP2) {
            return rightArrow;
        }
        return keyD;
    }
#endif
    return false;
}

inline bool controlFlipActive(GJBaseGameLayer* layer) {
    if (!layer || layer->m_isPlatformer) {
        return false;
    }
    return GameManager::get()->getGameVariable("0010");
}

inline bool handleButtonPlayer2Param(bool isP2, bool flip) {
    return flip ? isP2 : !isP2;
}

struct PhysicalHoldMode {
    bool twoPlayer;
    bool platformer;
    bool dual;
};

inline PhysicalHoldMode currentPhysicalHoldMode(GJBaseGameLayer* layer) {
    if (!layer) {
        return { false, false, false };
    }
    return {
        layer->m_levelSettings ? layer->m_levelSettings->m_twoPlayerMode : false,
        layer->m_isPlatformer,
        layer->m_gameState.m_isDualMode,
    };
}

inline bool physicalHoldActive(PhysicalHoldMode const& mode, int button, bool isP2, bool blocked) {
    if (blocked) {
        return false;
    }
    return isPhysicalKeyHeld(button, isP2, mode.twoPlayer, mode.platformer, mode.dual);
}