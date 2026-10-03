// Trajectory overlay — predicts player flight paths matching GDH default behavior and appearance.
// Based on GDH (src/hacks/level/show_trajectory.cpp).

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/binding/HardStreak.hpp>
#include <Geode/binding/GJGameState.hpp>
#include <Geode/cocos/draw_nodes/CCDrawNode.h>
#include "skkBot.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace skk::trajectory {
namespace {

// ---------------------------------------------------------------------------
// GDH Default Styling & Constants
// ---------------------------------------------------------------------------
// Jump Trajectory:   #E900FFFF (Magenta / purple-pink)
// Normal Trajectory: #00FFFFFF (Cyan)
// Outer Hitbox:      #FF0000FF (Red)
// Inner Hitbox:      #003FFFFF (Blue)
const ccColor4F kJumpColor        = ccc4f(0.913725f, 0.0f, 1.0f, 1.0f);
const ccColor4F kNormalColor      = ccc4f(0.0f, 1.0f, 1.0f, 1.0f);
const ccColor4F kPlayerColor      = ccc4f(1.0f, 0.0f, 0.0f, 1.0f);
const ccColor4F kPlayerInnerColor = ccc4f(0.0f, 0.247058f, 1.0f, 1.0f);

constexpr float kLineWidth  = 0.65f;
constexpr int   kIterations = 150;

// ---------------------------------------------------------------------------
// Global Simulation State
// ---------------------------------------------------------------------------
bool g_inSim = false;
bool g_simDead = false;
float g_dtStep = 0.25f;

CCDrawNode* g_drawNode = nullptr;
PlayerObject* g_dummyJump = nullptr;
PlayerObject* g_dummyNormal = nullptr;

// Retained containers for ghost players to avoid memory corruption / aliasing
CCArray* g_ghostRings = nullptr;
CCDictionary* g_ghostLogTop = nullptr;
CCDictionary* g_ghostLogBottom = nullptr;
CCDictionary* g_ghostLogLeft = nullptr;
CCDictionary* g_ghostLogRight = nullptr;

void initGhostContainers() {
    if (!g_ghostRings) {
        g_ghostRings = CCArray::create();
        g_ghostRings->retain();
    }
    if (!g_ghostLogTop) {
        g_ghostLogTop = CCDictionary::create();
        g_ghostLogTop->retain();
    }
    if (!g_ghostLogBottom) {
        g_ghostLogBottom = CCDictionary::create();
        g_ghostLogBottom->retain();
    }
    if (!g_ghostLogLeft) {
        g_ghostLogLeft = CCDictionary::create();
        g_ghostLogLeft->retain();
    }
    if (!g_ghostLogRight) {
        g_ghostLogRight = CCDictionary::create();
        g_ghostLogRight->retain();
    }
}

void bindGhostContainers(PlayerObject* p) {
    if (!p) return;
    initGhostContainers();
    p->m_touchingRings = g_ghostRings;
    p->m_collisionLogTop = g_ghostLogTop;
    p->m_collisionLogBottom = g_ghostLogBottom;
    p->m_collisionLogLeft = g_ghostLogLeft;
    p->m_collisionLogRight = g_ghostLogRight;
}

void clearCollisionLogs(PlayerObject* p) {
    if (!p) return;
    bindGhostContainers(p);
    if (p->m_collisionLogTop) p->m_collisionLogTop->removeAllObjects();
    if (p->m_collisionLogBottom) p->m_collisionLogBottom->removeAllObjects();
    if (p->m_collisionLogLeft) p->m_collisionLogLeft->removeAllObjects();
    if (p->m_collisionLogRight) p->m_collisionLogRight->removeAllObjects();
    if (p->m_touchingRings) p->m_touchingRings->removeAllObjects();
}

bool isFake(const PlayerObject* p) {
    return p && (p == g_dummyJump || p == g_dummyNormal);
}

// Render-frame gate to avoid redundant traces during sub-step physics ticks
unsigned int g_lastRenderFrame = 0;
bool alreadyTracedThisRenderFrame() {
    const unsigned int frame = CCDirector::sharedDirector()->getTotalFrames();
    if (frame == g_lastRenderFrame) return true;
    g_lastRenderFrame = frame;
    return false;
}

// ---------------------------------------------------------------------------
// GDH Hitbox Rendering
// ---------------------------------------------------------------------------
void renderHitboxOutline(CCDrawNode* drawNode, const cocos2d::CCRect& rect, const cocos2d::ccColor4F& border) {
    if (!drawNode) return;
    cocos2d::CCPoint vertices[4] = {
        {rect.getMinX(), rect.getMinY()},
        {rect.getMinX(), rect.getMaxY()},
        {rect.getMaxX(), rect.getMaxY()},
        {rect.getMaxX(), rect.getMinY()}
    };
    drawNode->drawPolygon(vertices, 4, {0.f, 0.f, 0.f, 0.f}, 0.25f, border);
}

// ---------------------------------------------------------------------------
// Player State Cloning (GDH PracticeFix::copyPlayerState)
// ---------------------------------------------------------------------------
void copyPlayerState(PlayerObject* from, PlayerObject* to) {
    if (!from || !to) return;
    to->copyAttributes(from);

    to->m_yVelocity = from->m_yVelocity;
    to->m_fallSpeed = from->m_fallSpeed;
    to->m_isOnSlope = from->m_isOnSlope;
    to->m_wasOnSlope = from->m_wasOnSlope;
    to->m_slopeVelocity = from->m_slopeVelocity;
    to->m_maybeUpsideDownSlope = from->m_maybeUpsideDownSlope;
    to->m_isShip = from->m_isShip;
    to->m_isBird = from->m_isBird;
    to->m_isBall = from->m_isBall;
    to->m_isDart = from->m_isDart;
    to->m_isRobot = from->m_isRobot;
    to->m_isSpider = from->m_isSpider;
    to->m_isUpsideDown = from->m_isUpsideDown;
    to->m_isDead = false;
    to->m_isOnGround = from->m_isOnGround;
    to->m_isGoingLeft = from->m_isGoingLeft;
    to->m_isSideways = from->m_isSideways;
    to->m_isSwing = from->m_isSwing;

    to->m_reverseRelated = from->m_reverseRelated;
    to->m_maybeReverseSpeed = from->m_maybeReverseSpeed;
    to->m_maybeReverseAcceleration = from->m_maybeReverseAcceleration;
    to->m_xVelocityRelated2 = from->m_xVelocityRelated2;
    to->m_isDashing = from->m_isDashing;
    to->m_dashFireFrame = from->m_dashFireFrame;
    to->m_groundObjectMaterial = from->m_groundObjectMaterial;
    to->m_vehicleSize = from->m_vehicleSize;
    to->m_playerSpeed = from->m_playerSpeed;
    to->m_speedMultiplier = from->m_speedMultiplier;
    to->m_accelerationOrSpeed = from->m_accelerationOrSpeed;
    to->m_rotationSpeed = from->m_rotationSpeed;
    to->m_rotateSpeed = from->m_rotateSpeed;
    to->m_shipRotation = from->m_shipRotation;
    to->m_lastPortalPos = from->m_lastPortalPos;
    to->m_unkUnused3 = from->m_unkUnused3;
    to->m_isOnGround2 = from->m_isOnGround2;
    to->m_lastLandTime = from->m_lastLandTime;
    to->m_platformerVelocityRelated = from->m_platformerVelocityRelated;
    to->m_maybeIsBoosted = from->m_maybeIsBoosted;
    to->m_scaleXRelatedTime = from->m_scaleXRelatedTime;
    to->m_decreaseBoostSlide = from->m_decreaseBoostSlide;
    to->m_unkA29 = from->m_unkA29;
    to->m_isLocked = from->m_isLocked;
    to->m_controlsDisabled = from->m_controlsDisabled;
    to->m_lastGroundedPos = from->m_lastGroundedPos;

    to->m_lastActivatedPortal = from->m_lastActivatedPortal;
    to->m_hasEverJumped = from->m_hasEverJumped;
    to->m_hasEverHitRing = from->m_hasEverHitRing;
    to->m_playerColor1 = from->m_playerColor1;
    to->m_playerColor2 = from->m_playerColor2;

    to->m_position = from->m_position;
    to->setPosition(from->getPosition());
    to->setRotation(from->getRotation());
    to->setScale(from->getScale());
    to->setScaleX(from->getScaleX());
    to->setScaleY(from->getScaleY());

    to->m_isSecondPlayer = from->m_isSecondPlayer;
    to->m_unkA99 = from->m_unkA99;
    to->m_totalTime = from->m_totalTime;
    to->m_isBeingSpawnedByDualPortal = from->m_isBeingSpawnedByDualPortal;
    to->m_audioScale = from->m_audioScale;
    to->m_unkAngle1 = from->m_unkAngle1;
    to->m_yVelocityRelated3 = from->m_yVelocityRelated3;
    to->m_defaultMiniIcon = from->m_defaultMiniIcon;
    to->m_swapColors = from->m_swapColors;
    to->m_switchDashFireColor = from->m_switchDashFireColor;
    to->m_followRelated = from->m_followRelated;
    to->m_unk838 = from->m_unk838;

    to->m_stateOnGround = from->m_stateOnGround;
    to->m_stateUnk = from->m_stateUnk;
    to->m_stateNoStickX = from->m_stateNoStickX;
    to->m_stateNoStickY = from->m_stateNoStickY;
    to->m_stateUnk2 = from->m_stateUnk2;
    to->m_stateBoostX = from->m_stateBoostX;
    to->m_stateBoostY = from->m_stateBoostY;
    to->m_maybeStateForce2 = from->m_maybeStateForce2;
    to->m_stateScale = from->m_stateScale;
    to->m_platformerXVelocity = from->m_platformerXVelocity;
    to->m_holdingRight = from->m_holdingRight;
    to->m_holdingLeft = from->m_holdingLeft;
    to->m_leftPressedFirst = from->m_leftPressedFirst;
    to->m_scaleXRelated = from->m_scaleXRelated;
    to->m_maybeHasStopped = from->m_maybeHasStopped;
    to->m_xVelocityRelated = from->m_xVelocityRelated;
    to->m_maybeGoingCorrectSlopeDirection = from->m_maybeGoingCorrectSlopeDirection;
    to->m_isSliding = from->m_isSliding;
    to->m_maybeSlopeForce = from->m_maybeSlopeForce;
    to->m_isOnIce = from->m_isOnIce;
    to->m_physDeltaRelated = from->m_physDeltaRelated;
    to->m_isOnGround4 = from->m_isOnGround4;
    to->m_maybeSlidingTime = from->m_maybeSlidingTime;
    to->m_maybeSlidingStartTime = from->m_maybeSlidingStartTime;
    to->m_changedDirectionsTime = from->m_changedDirectionsTime;
    to->m_slopeEndTime = from->m_slopeEndTime;
    to->m_isMoving = from->m_isMoving;
    to->m_platformerMovingLeft = from->m_platformerMovingLeft;
    to->m_platformerMovingRight = from->m_platformerMovingRight;
    to->m_isSlidingRight = from->m_isSlidingRight;
    to->m_maybeChangedDirectionAngle = from->m_maybeChangedDirectionAngle;
    to->m_unkUnused2 = from->m_unkUnused2;
    to->m_isPlatformer = from->m_isPlatformer;

    to->m_stateNoAutoJump = from->m_stateNoAutoJump;
    to->m_stateDartSlide = from->m_stateDartSlide;
    to->m_stateHitHead = from->m_stateHitHead;
    to->m_stateFlipGravity = from->m_stateFlipGravity;
    to->m_gravityMod = from->m_gravityMod;
    to->m_stateForce = from->m_stateForce;
    to->m_stateForceVector = from->m_stateForceVector;
    to->m_affectedByForces = from->m_affectedByForces;
    to->m_somethingPlayerSpeedTime = from->m_somethingPlayerSpeedTime;
    to->m_playerSpeedAC = from->m_playerSpeedAC;
    to->m_fixRobotJump = from->m_fixRobotJump;
    to->m_inputsLocked = from->m_inputsLocked;
    to->m_gv0123 = from->m_gv0123;
    to->m_iconRequestID = from->m_iconRequestID;

    to->m_unkUnused = from->m_unkUnused;
    to->m_isOutOfBounds = from->m_isOutOfBounds;
    to->m_fallStartY = from->m_fallStartY;
    to->m_disablePlayerSqueeze = from->m_disablePlayerSqueeze;
    to->m_robotAnimation1Enabled = from->m_robotAnimation1Enabled;
    to->m_robotAnimation2Enabled = from->m_robotAnimation2Enabled;
    to->m_spiderAnimationEnabled = from->m_spiderAnimationEnabled;
    to->m_ignoreDamage = from->m_ignoreDamage;
    to->m_enable22Changes = from->m_enable22Changes;
}

// ---------------------------------------------------------------------------
// Simulation Path (GDH TrajectoryManager::simulatePath)
// ---------------------------------------------------------------------------
void simulatePath(PlayLayer* layer, PlayerObject* simPlayer, PlayerObject* sourcePlayer,
                  bool isJumpInput, const ccColor4F& lineColor) {
    if (!layer || !simPlayer || !sourcePlayer || !g_drawNode) return;

    copyPlayerState(sourcePlayer, simPlayer);
    bindGhostContainers(simPlayer);
    clearCollisionLogs(simPlayer);

    simPlayer->m_playEffects = false;
    simPlayer->m_hasNoEffects = true;
    simPlayer->m_maybeReducedEffects = true;

    simPlayer->m_gravityMod = sourcePlayer->m_gravityMod;
    simPlayer->m_isSideways = sourcePlayer->m_isSideways;

    simPlayer->m_isDashing = sourcePlayer->m_isDashing;
    simPlayer->m_dashX = sourcePlayer->m_dashX;
    simPlayer->m_dashY = sourcePlayer->m_dashY;
    simPlayer->m_dashAngle = sourcePlayer->m_dashAngle;

    simPlayer->m_isOnGround = sourcePlayer->m_isOnGround;

    if (isJumpInput) {
        if (sourcePlayer->m_isSpider) {
            if (sourcePlayer->m_isOnGround || sourcePlayer->m_jumpBuffered) {
                simPlayer->spiderTestJump(false);
            }
        } else {
            simPlayer->pushButton(PlayerButton::Jump);
        }
    } else {
        simPlayer->releaseButton(PlayerButton::Jump);
        simPlayer->m_jumpBuffered = false;
    }

    if (layer->m_levelSettings && layer->m_levelSettings->m_platformerMode) {
        const bool left = sourcePlayer->m_isGoingLeft;
        simPlayer->releaseButton(PlayerButton::Right);
        simPlayer->releaseButton(PlayerButton::Left);
        simPlayer->pushButton(left ? PlayerButton::Left : PlayerButton::Right);
    }

    simPlayer->setVisible(false);
    g_simDead = false;

    float effectiveDt = g_dtStep;
    if (!(effectiveDt > 0.0f) || !std::isfinite(effectiveDt)) {
        effectiveDt = 0.25f;
    }

    for (int i = 0; i < kIterations; ++i) {
        cocos2d::CCPoint currentPos = simPlayer->getPosition();
        clearCollisionLogs(simPlayer);

        if (g_simDead) break;

        simPlayer->update(effectiveDt);

        if (layer->checkCollisions(simPlayer, effectiveDt, false) == 1) {
            g_simDead = true;
        }

        g_drawNode->drawSegment(currentPos, simPlayer->getPosition(), kLineWidth, lineColor);
    }

    renderHitboxOutline(g_drawNode, simPlayer->getObjectRect(), kPlayerColor);
    renderHitboxOutline(g_drawNode, simPlayer->getObjectRect(0.25f, 0.25f), kPlayerInnerColor);

    simPlayer->setPosition({-99999.f, -99999.f});
    simPlayer->setVisible(false);
}

// ---------------------------------------------------------------------------
// Main Simulation Runner (GDH TrajectoryManager::runTrajectorySimulation)
// ---------------------------------------------------------------------------
void runTrajectorySimulation(PlayLayer* layer) {
    if (!layer || !g_drawNode) return;

    auto* engine = ReplayEngine::get();
    if (!engine || !engine->showTrajectory) {
        g_drawNode->clear();
        return;
    }

    if (!layer->m_player1 || !layer->m_player1->isRunning()) {
        g_drawNode->clear();
        return;
    }

    if (alreadyTracedThisRenderFrame()) {
        return;
    }

    if (!g_dummyJump || !g_dummyNormal) return;

    g_inSim = true;
    g_drawNode->clear();

    if (layer->m_player1) {
        simulatePath(layer, g_dummyJump, layer->m_player1, true, kJumpColor);
        simulatePath(layer, g_dummyNormal, layer->m_player1, false, kNormalColor);
    }

    const bool p2Active = layer->m_player2 && layer->m_player2->isRunning() &&
                          (layer->m_gameState.m_isDualMode ||
                           (layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode));
    if (p2Active) {
        simulatePath(layer, g_dummyJump, layer->m_player2, true, kJumpColor);
        simulatePath(layer, g_dummyNormal, layer->m_player2, false, kNormalColor);
    }

    g_inSim = false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
class $modify(ShowTrajectoryPlayerObject, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);
        if (!g_inSim && !isFake(this)) {
            if (auto pl = PlayLayer::get()) {
                float timeWarp = pl->m_gameState.m_timeWarp;
                g_dtStep = (timeWarp > 0.001f) ? (dt / timeWarp) : dt;
            }
        }
    }

    void stopDashing() {
        if (!g_inSim) return PlayerObject::stopDashing();
        bool orig = m_playEffects;
        m_playEffects = false;
        PlayerObject::stopDashing();
        m_playEffects = orig;
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (g_inSim) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* object, bool skipCheck) {
        if (g_inSim) return;
        PlayerObject::ringJump(object, skipCheck);
    }

    void incrementJumps() {
        if (g_inSim) return;
        PlayerObject::incrementJumps();
    }

    void setVisible(bool visible) {
        if (isFake(this)) {
            if (m_iconSprite) m_iconSprite->setVisible(false);
            if (m_iconSpriteSecondary) m_iconSpriteSecondary->setVisible(false);
            if (m_iconSpriteWhitener) m_iconSpriteWhitener->setVisible(false);
            if (m_iconGlow) m_iconGlow->setVisible(false);
            if (m_vehicleSprite) m_vehicleSprite->setVisible(false);
            if (m_vehicleSpriteSecondary) m_vehicleSpriteSecondary->setVisible(false);
            if (m_birdVehicle) m_birdVehicle->setVisible(false);
            if (m_spiderSprite) m_spiderSprite->setVisible(false);
            if (m_robotSprite) m_robotSprite->setVisible(false);
            if (m_robotBatchNode) m_robotBatchNode->setVisible(false);
            if (m_spiderBatchNode) m_spiderBatchNode->setVisible(false);
            return;
        }
        PlayerObject::setVisible(visible);
    }

    void visit() {
        if (isFake(this)) return;
        PlayerObject::visit();
    }

    void draw() {
        if (isFake(this)) return;
        PlayerObject::draw();
    }

    void setOpacity(unsigned char opacity) {
        if (isFake(this)) opacity = 0;
        PlayerObject::setOpacity(opacity);
    }

    void addAllParticles() {
        if (!g_inSim) PlayerObject::addAllParticles();
    }
    void playSpawnEffect() {
        if (!g_inSim && !isFake(this)) PlayerObject::playSpawnEffect();
    }
    void playDeathEffect() {
        if (!g_inSim && !isFake(this)) PlayerObject::playDeathEffect();
    }
    void playerDestroyed(bool noEffects) {
        if (!g_inSim && !isFake(this)) PlayerObject::playerDestroyed(noEffects);
    }
    void spawnScaleCircle() {
        if (!g_inSim && !isFake(this)) PlayerObject::spawnScaleCircle();
    }
    void playBumpEffect(int objectType, GameObject* player) {
        if (!g_inSim && !isFake(this)) PlayerObject::playBumpEffect(objectType, player);
    }
    void bumpPlayer(float bumpMod, int objectType, bool noEffects, GameObject* object) {
        if (g_inSim || isFake(this)) noEffects = true;
        PlayerObject::bumpPlayer(bumpMod, objectType, noEffects, object);
    }
    void propellPlayer(float yVelocity, bool noEffects, int objectType) {
        if (g_inSim || isFake(this)) noEffects = true;
        PlayerObject::propellPlayer(yVelocity, noEffects, objectType);
    }
    void flipGravity(bool flip, bool noEffects) {
        if (g_inSim || isFake(this)) noEffects = true;
        PlayerObject::flipGravity(flip, noEffects);
    }
};

inline void applyPortalHint(PlayerObject* player, int portalId) {
    if (!player) return;
    switch (portalId) {
        case 99:                                         // Regular size
            player->togglePlayerScale(false, true);
            player->m_vehicleSize = 1.0f;
            player->setScale(1.0f);
            break;
        case 101:                                        // Mini size
            player->togglePlayerScale(true, true);
            player->m_vehicleSize = 0.6f;
            player->setScale(0.6f);
            break;
        case 10: player->m_isUpsideDown = false; break;  // Normal gravity
        case 11: player->m_isUpsideDown = true; break;   // Inverse gravity
        default: break;
    }
}

class $modify(ShowTrajectoryGJBaseGameLayer, GJBaseGameLayer) {
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (g_inSim) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!g_inSim) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        if (!player || !objects || objectCount <= 0) return;

        auto addCollisionObject = [](GameObject* obj, gd::vector<GameObject*>& vec, int& count, int& index) {
            if (count < index) {
                vec.at(count) = obj;
            } else {
                vec.push_back(obj);
                index++;
            }
            count++;
        };

        const CCRect playerRect = player->getObjectRect();

        for (int i = 0; i < objectCount; ++i) {
            GameObject* object = objects->at(i);

            if (!object || object->m_isGroupDisabled || object->m_isDisabled || object == m_anticheatSpike) {
                continue;
            }

            const int objId = object->m_objectID;
            if (objId == 99 || objId == 101 || objId == 10 || objId == 11) {
                if (playerRect.intersectsRect(object->getObjectRect())) {
                    applyPortalHint(player, objId);
                }
                continue;
            }

            const auto type = object->m_objectType;

            if (type == GameObjectType::YellowJumpPad) {
                if (playerRect.intersectsRect(object->getObjectRect())) {
                    player->bumpPlayer(1.0f, static_cast<int>(type), true, object);
                }
                continue;
            }
            if (type == GameObjectType::PinkJumpPad) {
                if (playerRect.intersectsRect(object->getObjectRect())) {
                    player->bumpPlayer(0.7f, static_cast<int>(type), true, object);
                }
                continue;
            }
            if (type == GameObjectType::RedJumpPad) {
                if (playerRect.intersectsRect(object->getObjectRect())) {
                    player->bumpPlayer(1.3f, static_cast<int>(type), true, object);
                }
                continue;
            }
            if (type == GameObjectType::GravityPad) {
                if (playerRect.intersectsRect(object->getObjectRect())) {
                    player->flipGravity(!player->m_isUpsideDown, true);
                }
                continue;
            }

            if (type == GameObjectType::Solid || type == GameObjectType::Breakable) {
                addCollisionObject(object, m_solidCollisionObjects, m_solidCollisionObjectsCount, m_solidCollisionObjectsIndex);
                continue;
            }

            if (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard) {
                addCollisionObject(object, m_hazardCollisionObjects, m_hazardCollisionObjectsCount, m_hazardCollisionObjectsIndex);
                continue;
            }

            if (type == GameObjectType::Slope) {
                const cocos2d::CCRect slopeRect = object->getObjectRect(2.0f, 2.0f);
                if (!playerRect.intersectsRect(slopeRect)) continue;

                const bool needsFix = !m_levelSettings || !m_levelSettings->m_fixRadiusCollision || object->m_objectRadius <= 0.0;
                if (object->m_shouldUseOuterOb && needsFix) {
                    OBB2D* box = object->getOrientedBox();
                    player->updateOrientedBox();

                    OBB2D* playerBox = static_cast<GameObject*>(player)->getOrientedBox();
                    if (!box || !playerBox || !box->overlaps1Way(playerBox)) continue;
                }

                if (!player->m_isSideways) {
                    player->collidedWithSlopeInternal(dt, object, false);
                } else {
                    player->handleRotatedCollisionInternal(dt, object, {0.0f, 0.0f, 0.0f, 0.0f}, false, false, true);
                }
            }
        }
    }

    void flipGravity(PlayerObject* object, bool flip, bool noEffects) {
        if (g_inSim) return;
        GJBaseGameLayer::flipGravity(object, flip, noEffects);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* object) {
        if (g_inSim) return;
        GJBaseGameLayer::playerTouchedRing(player, object);
    }

    void destroyObject(GameObject* object) {
        if (g_inSim) {
            if (object && object->m_objectType == GameObjectType::Breakable) return;
        }
        GJBaseGameLayer::destroyObject(object);
    }

    void shakeCamera(float duration, float strength, float interval) {
        if (g_inSim) return;
        GJBaseGameLayer::shakeCamera(duration, strength, interval);
    }

    void checkSpawnObjects() {
        if (g_inSim) return;
        GJBaseGameLayer::checkSpawnObjects();
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (g_inSim) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void toggleDualMode(GameObject* object, bool dual, PlayerObject* player, bool noEffects) {
        if (g_inSim || isFake(player)) return;
        GJBaseGameLayer::toggleDualMode(object, dual, player, noEffects);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (g_inSim || isFake(player)) return;
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }

    void checkCollisionBlocks(EffectGameObject* object, gd::vector<EffectGameObject*>* blocks, int count) {
        if (g_inSim) return;
        GJBaseGameLayer::checkCollisionBlocks(object, blocks, count);
    }

    void createPlayerCollisionBlock() {
        if (g_inSim) return;
        GJBaseGameLayer::createPlayerCollisionBlock();
    }

    void updateCollisionBlocks() {
        if (g_inSim) return;
        GJBaseGameLayer::updateCollisionBlocks();
    }

    void updatePlayerCollisionBlocks() {
        if (g_inSim) return;
        GJBaseGameLayer::updatePlayerCollisionBlocks();
    }

    void updateDualGround(PlayerObject* object, int mode, bool instant, float duration) {
        if (g_inSim || isFake(object)) return;
        GJBaseGameLayer::updateDualGround(object, mode, instant, duration);
    }
};

class $modify(ShowTrajectoryEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, const gd::vector<int>* remapKeys) {
        if (g_inSim) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
    void triggerActivated(float value) {
        if (!g_inSim) EffectGameObject::triggerActivated(value);
    }
    void playTriggerEffect() {
        if (!g_inSim) EffectGameObject::playTriggerEffect();
    }
};

class $modify(ShowTrajectoryGameObject, GameObject) {
    void playShineEffect() {
        if (!g_inSim) GameObject::playShineEffect();
    }
    void activateObject() {
        if (!g_inSim) GameObject::activateObject();
    }
};

class $modify(ShowTrajectoryHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (!g_inSim) HardStreak::addPoint(point);
    }
};

class $modify(ShowTrajectoryPlayLayer, PlayLayer) {
    struct Fields {
        ~Fields() {
            if (g_dummyJump) {
                g_dummyJump->release();
                g_dummyJump = nullptr;
            }
            if (g_dummyNormal) {
                g_dummyNormal->release();
                g_dummyNormal = nullptr;
            }
            g_drawNode = nullptr;
            g_inSim = false;
            g_simDead = false;
        }
    };

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto* engine = ReplayEngine::get();
        if (!engine || !engine->showTrajectory) {
            if (g_drawNode) g_drawNode->clear();
            return;
        }

        if (!g_drawNode && m_objectLayer) {
            auto* parent = m_debugDrawNode ? m_debugDrawNode->getParent() : m_objectLayer;
            if (!parent) parent = m_objectLayer;

            g_drawNode = CCDrawNode::create();
            g_drawNode->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
            g_drawNode->setZOrder(9999);
            g_drawNode->setID("trajectory-node"_spr);
            g_drawNode->m_bUseArea = false;
            parent->addChild(g_drawNode);
        }

        if (!g_dummyJump) {
            initGhostContainers();
            g_dummyJump = PlayerObject::create(1, 1, this, this, true);
            if (g_dummyJump) {
                g_dummyJump->retain();
                g_dummyJump->setPosition({-99999.f, -99999.f});
                g_dummyJump->setVisible(false);
                g_dummyJump->m_playEffects = false;
                bindGhostContainers(g_dummyJump);
            }
        }
        if (!g_dummyNormal) {
            initGhostContainers();
            g_dummyNormal = PlayerObject::create(1, 1, this, this, true);
            if (g_dummyNormal) {
                g_dummyNormal->retain();
                g_dummyNormal->setPosition({-99999.f, -99999.f});
                g_dummyNormal->setVisible(false);
                g_dummyNormal->m_playEffects = false;
                bindGhostContainers(g_dummyNormal);
            }
        }

        if (g_drawNode) {
            g_drawNode->setColor(ccColor3B{255, 255, 255});
            g_drawNode->setOpacity(255);
        }

        runTrajectorySimulation(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object != m_anticheatSpike && g_inSim) {
            if (isFake(player)) {
                g_simDead = true;
                if (player) player->m_isDead = true;
                return;
            }
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (g_inSim) return;
        PlayLayer::playEndAnimationToPos(position);
    }
};

}  // namespace skk::trajectory
