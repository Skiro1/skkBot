#include "physics21.hpp"
#include "../skkBot.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

namespace skkbot2::physics21 {

struct VanillaFlags {
    bool valid = false;
    bool fixGravityBug = true;
    bool fixRobotJump = true;
    bool fixRadiusCollision = true;
    bool enable22Changes = true;
    bool enableImpulseFix = true;
    bool enablePlayerSqueeze = true;
    bool disablePlayerSqueezeP1 = false;
    bool disablePlayerSqueezeP2 = false;
    bool enable22ChangesP1 = true;
    bool enable22ChangesP2 = true;
    bool fixGravityBugP1 = true;
    bool fixGravityBugP2 = true;
    bool fixRobotJumpP1 = true;
    bool fixRobotJumpP2 = true;
    bool enableImpulseFixP1 = true;
    bool enableImpulseFixP2 = true;
};

static VanillaFlags s_vanillaFlags;
static bool s_wasEnabled = false;

bool isEnabled() {
    auto* engine = ReplayEngine::get();
    return engine && engine->physics21;
}

static inline void apply21PlayerFlags(PlayerObject* player) {
    if (!player) return;
    player->m_fixGravityBug = false;
    player->m_fixRobotJump = false;
    player->m_disablePlayerSqueeze = true;
    player->m_enableImpulseFix = false;
    player->m_enable22Changes = false;
}

static inline void apply21LevelFlags(GJBaseGameLayer* layer) {
    if (!layer) return;
    if (layer->m_levelSettings) {
        layer->m_levelSettings->m_fixGravityBug = false;
        layer->m_levelSettings->m_fixRobotJump = false;
        layer->m_levelSettings->m_fixRadiusCollision = false;
        layer->m_levelSettings->m_enable22Changes = false;
        layer->m_levelSettings->m_enableImpulseFix = false;
        layer->m_levelSettings->m_enablePlayerSqueeze = false;
    }
    layer->m_enable22Changes = false;
    if (layer->m_player1) apply21PlayerFlags(layer->m_player1);
    if (layer->m_player2) apply21PlayerFlags(layer->m_player2);
}

static inline void captureVanillaFlags(GJBaseGameLayer* layer) {
    if (!layer || !layer->m_levelSettings) return;
    if (s_vanillaFlags.valid) return;

    s_vanillaFlags.fixGravityBug = layer->m_levelSettings->m_fixGravityBug;
    s_vanillaFlags.fixRobotJump = layer->m_levelSettings->m_fixRobotJump;
    s_vanillaFlags.fixRadiusCollision = layer->m_levelSettings->m_fixRadiusCollision;
    s_vanillaFlags.enable22Changes = layer->m_levelSettings->m_enable22Changes;
    s_vanillaFlags.enableImpulseFix = layer->m_levelSettings->m_enableImpulseFix;
    s_vanillaFlags.enablePlayerSqueeze = layer->m_levelSettings->m_enablePlayerSqueeze;
    if (layer->m_player1) {
        s_vanillaFlags.disablePlayerSqueezeP1 = layer->m_player1->m_disablePlayerSqueeze;
        s_vanillaFlags.enable22ChangesP1 = layer->m_player1->m_enable22Changes;
        s_vanillaFlags.fixGravityBugP1 = layer->m_player1->m_fixGravityBug;
        s_vanillaFlags.fixRobotJumpP1 = layer->m_player1->m_fixRobotJump;
        s_vanillaFlags.enableImpulseFixP1 = layer->m_player1->m_enableImpulseFix;
    }
    if (layer->m_player2) {
        s_vanillaFlags.disablePlayerSqueezeP2 = layer->m_player2->m_disablePlayerSqueeze;
        s_vanillaFlags.enable22ChangesP2 = layer->m_player2->m_enable22Changes;
        s_vanillaFlags.fixGravityBugP2 = layer->m_player2->m_fixGravityBug;
        s_vanillaFlags.fixRobotJumpP2 = layer->m_player2->m_fixRobotJump;
        s_vanillaFlags.enableImpulseFixP2 = layer->m_player2->m_enableImpulseFix;
    }
    s_vanillaFlags.valid = true;
}

static inline void restoreVanillaFlags(GJBaseGameLayer* layer) {
    if (!layer || !s_vanillaFlags.valid) return;
    if (layer->m_levelSettings) {
        layer->m_levelSettings->m_fixGravityBug = s_vanillaFlags.fixGravityBug;
        layer->m_levelSettings->m_fixRobotJump = s_vanillaFlags.fixRobotJump;
        layer->m_levelSettings->m_fixRadiusCollision = s_vanillaFlags.fixRadiusCollision;
        layer->m_levelSettings->m_enable22Changes = s_vanillaFlags.enable22Changes;
        layer->m_levelSettings->m_enableImpulseFix = s_vanillaFlags.enableImpulseFix;
        layer->m_levelSettings->m_enablePlayerSqueeze = s_vanillaFlags.enablePlayerSqueeze;
    }
    layer->m_enable22Changes = s_vanillaFlags.enable22Changes;
    if (layer->m_player1) {
        layer->m_player1->m_fixGravityBug = s_vanillaFlags.fixGravityBugP1;
        layer->m_player1->m_fixRobotJump = s_vanillaFlags.fixRobotJumpP1;
        layer->m_player1->m_disablePlayerSqueeze = s_vanillaFlags.disablePlayerSqueezeP1;
        layer->m_player1->m_enableImpulseFix = s_vanillaFlags.enableImpulseFixP1;
        layer->m_player1->m_enable22Changes = s_vanillaFlags.enable22ChangesP1;
    }
    if (layer->m_player2) {
        layer->m_player2->m_fixGravityBug = s_vanillaFlags.fixGravityBugP2;
        layer->m_player2->m_fixRobotJump = s_vanillaFlags.fixRobotJumpP2;
        layer->m_player2->m_disablePlayerSqueeze = s_vanillaFlags.disablePlayerSqueezeP2;
        layer->m_player2->m_enableImpulseFix = s_vanillaFlags.enableImpulseFixP2;
        layer->m_player2->m_enable22Changes = s_vanillaFlags.enable22ChangesP2;
    }
}

void syncState(GJBaseGameLayer* layer) {
    if (!layer) return;
    captureVanillaFlags(layer);

    bool enabled = isEnabled();
    if (enabled) {
        apply21LevelFlags(layer);
        s_wasEnabled = true;
    } else if (s_wasEnabled) {
        restoreVanillaFlags(layer);
        s_wasEnabled = false;
    }
}

void onLevelExit() {
    s_vanillaFlags.valid = false;
    s_wasEnabled = false;
}

}

class $modify(Legacy21PlayerObject, PlayerObject) {
    void update(float dt) {
        if (m_gameLayer) {
            skkbot2::physics21::syncState(m_gameLayer);
        } else if (skkbot2::physics21::isEnabled()) {
            skkbot2::physics21::apply21PlayerFlags(this);
        }
        PlayerObject::update(dt);
    }

    void resetObject() {
        PlayerObject::resetObject();
        if (m_gameLayer) {
            skkbot2::physics21::syncState(m_gameLayer);
        } else if (skkbot2::physics21::isEnabled()) {
            skkbot2::physics21::apply21PlayerFlags(this);
        }
    }

    bool playerIsFallingBugged() {
        if (skkbot2::physics21::isEnabled()) {
            float adjustedGravity = m_gravity;
            if (m_isSecondPlayer) {
                adjustedGravity = -adjustedGravity;
            }

            if (m_isUpsideDown) {
                return m_yVelocity > (m_gravity * 2.0f);
            }

            return m_yVelocity < (double)(adjustedGravity * 2.0f);
        }
        return PlayerObject::playerIsFallingBugged();
    }

    void updateShipRotation(float dt) {
        if (!skkbot2::physics21::isEnabled()) {
            PlayerObject::updateShipRotation(dt);
            return;
        }

        if (m_isDart) {
            PlayerObject::updateShipRotation(dt);
            return;
        }

        if (m_isOnSlope || m_isDashing) return;

        cocos2d::CCPoint currentPos = getPosition();
        float deltaX = currentPos.x - m_shipRotation.x;
        float deltaY = -(currentPos.y - m_shipRotation.y);

        float distanceSq = deltaX * deltaX + deltaY * deltaY;
        if (distanceSq < dt * 1.2f) return;

        float targetAngle = atan2f(deltaY, deltaX);
        float currentRotation = getRotation();

        if (m_isBird) {
            targetAngle *= -0.4f;
        }

        if (m_isCollidingWithSlope && isFlying()) {
            targetAngle = 0.0f;
        }

        float currentRadians = currentRotation * 0.0174532925f;
        float speed = 0.15f;

        if (m_isBird) {
            if (m_isGoingLeft == m_isUpsideDown) {
                if (targetAngle > -0.1f) targetAngle = -0.1f;
            } else {
                if (targetAngle < 0.1f) targetAngle = 0.1f;
            }
            speed = 0.07f;
        } else if (m_isDart) {
            speed = (m_vehicleSize == 1.0f) ? 0.25f : 0.4f;
        }

        if (m_isCollidingWithSlope && isFlying()) {
            speed *= 0.25f;
        }

        float step = dt * speed;
        if (dt <= step) step = dt;

        float cosCur = std::cos(currentRadians);
        float sinCur = std::sin(currentRadians);
        float cosTgt = std::cos(targetAngle);
        float sinTgt = std::sin(targetAngle);

        float dot = sinCur * sinTgt + cosCur * cosTgt;
        if (dot < 0.0f) {
            dot = -dot;
            sinTgt = -sinTgt;
            cosTgt = -cosTgt;
        }

        float wCur = 0.0f;
        float wTgt = 0.0f;
        if (1.0f - dot <= 0.0001f) {
            wTgt = step;
            wCur = 1.0f - step;
        } else {
            float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
            float invSinTheta = 1.0f / std::sin(theta);
            wCur = std::sin((1.0f - step) * theta) * invSinTheta;
            wTgt = std::sin(step * theta) * invSinTheta;
        }

        float newAngle = std::atan2(sinCur * wCur + sinTgt * wTgt, cosCur * wCur + cosTgt * wTgt);
        setRotation(newAngle * 57.2957795f);
    }

    void updateRotation(float dt) {
        PlayerObject::updateRotation(dt);

        if (!skkbot2::physics21::isEnabled()) return;

        if (!m_isShip && !m_isBird && !m_isDart && !m_isSwing && !m_isRobot && !m_isSpider) {
            if (!m_isDashing && (m_isOnGround || m_isOnGround2)) {
                float r = getRotation();
                float snapped = std::round(r / 90.0f) * 90.0f;
                if (std::abs(r - snapped) > 0.001f) {
                    setRotation(snapped);
                }
            }
        }
    }

    void updateRotation(float dt, float rotation) {
        PlayerObject::updateRotation(dt, rotation);

        if (!skkbot2::physics21::isEnabled()) return;

        if (!m_isShip && !m_isBird && !m_isDart && !m_isSwing && !m_isRobot && !m_isSpider) {
            if (!m_isDashing && (m_isOnGround || m_isOnGround2)) {
                float r = getRotation();
                float snapped = std::round(r / 90.0f) * 90.0f;
                if (std::abs(r - snapped) > 0.001f) {
                    setRotation(snapped);
                }
            }
        }
    }

    void runNormalRotation(bool notNormalMode, float speed) {
        if (!skkbot2::physics21::isEnabled()) {
            PlayerObject::runNormalRotation(notNormalMode, speed);
            return;
        }

        bool isBall = !isFlying() && !m_isRobot && !m_isSpider && m_isBall;
        bool isCube = !isFlying() && !m_isRobot && !m_isSpider && !m_isBall;

        if (notNormalMode || (isCube && !m_isDashing) || (isBall && !m_isDashing)) {
            m_isRotating = true;
            float duration = (m_vehicleSize != 1.0f) ? (20.0f / 60.0f) : (26.0f / 60.0f);
            int rotatedMod = m_isSideways ? -1 : 1;
            m_rotationSpeed = (180.0f * flipMod() * reverseMod() * rotatedMod * m_gravityMod * speed) / duration;
        }
    }

    void setYVelocity(double velocity, int type) {
        if (skkbot2::physics21::isEnabled()) {
            m_yVelocity = velocity;
            return;
        }
        PlayerObject::setYVelocity(velocity, type);
    }

    void flipGravity(bool upsideDown, bool noEffects) {
        const bool flips = m_isUpsideDown != upsideDown;
        PlayerObject::flipGravity(upsideDown, noEffects);
        if (flips && skkbot2::physics21::isEnabled()) {
            m_yVelocity *= (1.75 / 0.5);
        }
    }

    void ringJump(RingObject* ring, bool unk) {
        if (skkbot2::physics21::isEnabled()) {
            const float yStart = m_yStart;
            m_yStart = 11.180032f;
            PlayerObject::ringJump(ring, unk);
            m_yStart = yStart;
            return;
        }
        PlayerObject::ringJump(ring, unk);
    }

    void bumpPlayer(float force, int type, bool effect, GameObject* object) {
        if (skkbot2::physics21::isEnabled()) {
            m_touchedPad = true;
        }
        PlayerObject::bumpPlayer(force, type, effect, object);
    }
};

class $modify(Legacy21PlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        skkbot2::physics21::onLevelExit();
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        skkbot2::physics21::syncState(this);
        return true;
    }

    void resetLevel() {
        if (!skkbot2::physics21::isEnabled()) {
            skkbot2::physics21::syncState(this);
        }
        PlayLayer::resetLevel();
        skkbot2::physics21::syncState(this);
    }

    void onQuit() {
        skkbot2::physics21::onLevelExit();
        PlayLayer::onQuit();
    }
};

class $modify(Legacy21GameLayer, GJBaseGameLayer) {
    void flipGravity(PlayerObject* player, bool upsideDown, bool unk) {
        const bool flips = player && player->m_isUpsideDown != upsideDown;
        GJBaseGameLayer::flipGravity(player, upsideDown, unk);
        if (flips && skkbot2::physics21::isEnabled() && player) {
            player->m_yVelocity *= (1.75 / 0.5);
        }
    }

    void checkRepellPlayer() {
        if (skkbot2::physics21::isEnabled()) {
            return;
        }
        GJBaseGameLayer::checkRepellPlayer();
    }
};
