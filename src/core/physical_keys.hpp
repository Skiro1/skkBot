#pragma once

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>

// Vanilla GD physical-key mapping (button, isP2) -> whether the physical keys
// that GD maps to that button/player are currently held, per mode.
//
// Mode flags come straight from GD state:
//   twoPlayer  = m_levelSettings->m_twoPlayerMode
//   platformer = m_isPlatformer
//   dual       = m_gameState.m_isDualMode
//
// The mapping (gathered empirically):
//   - no two-player mode: p1 and p2 get the same buttons/shared keys
//   - p2 only exists while in dual mode
//   - no platformer: move buttons 2/3 (left/right) don't exist
//   - no-2p dual:         both share jump=Mouse/Space/W/Up, move=A+Left/D+Right
//   - 2p no-dual:         only p1; jump=Mouse/Space/W (Up arrow NOT active),
//                         move (platformer) = A/D only (arrows NOT active)
//   - 2p dual no-plat:    p1: Space/W ; p2: Mouse/Up
//   - 2p dual platformer: p1: Space/W + A/D ; p2: Up + Left/Right (Mouse dead)
//
// A Settings trigger can disable p1/p2/both; the caller must additionally gate
// on player->m_controlsDisabled.
inline bool isPhysicalKeyHeld(int button, bool isP2, bool twoPlayer, bool platformer, bool dual) {
#ifdef GEODE_IS_WINDOWS
    if (isP2 && !dual) {
        return false;  // player 2 only spawns with dual
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

// GD's handleButton 3rd param semantics (empirically + probe-proven):
//   flip OFF (game var 0010 = 0): param true -> Player1, param false -> Player2
//   flip ON  (game var 0010 = 1): param acts on the OPPOSITE player object
// (binding calls it isPlayer1, which matches the flip-off case).
// To route a synthesized press to logical player isP2:
//   param = flip ? isP2 : !isP2
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