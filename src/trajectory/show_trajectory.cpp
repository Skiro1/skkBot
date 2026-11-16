#include "physics/near_movers.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/HardStreak.hpp>
#include <Geode/binding/OBB2D.hpp>
#include <Geode/cocos/draw_nodes/CCDrawNode.h>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include "logger/logger.hpp"

#include "skkBot.hpp"
#include "trajectory.hpp"

#include <algorithm>
#include <cctype>
#include <string>

using namespace geode::prelude;

namespace skk::trajectory {

namespace {

Settings g_settings;

bool g_simulating = false;
bool g_simDead = false;
float g_dtStep = 1.0f / 240.0f;
bool g_dtStepValid = false;
void runSimulation(PlayLayer* layer);

class TrajectoryDrawNode : public cocos2d::CCDrawNode {
public:
    static TrajectoryDrawNode* create() {
        auto* node = new TrajectoryDrawNode();
        if (node && node->init()) {
            node->autorelease();
            return node;
        }
        CC_SAFE_DELETE(node);
        return nullptr;
    }

    void visit() override {
        if (auto* pl = PlayLayer::get()) {
            runSimulation(pl);
            if (pl->m_objectLayer && getParent()) {
                const auto layerToWorld =
                    pl->m_objectLayer->nodeToWorldTransform();
                const auto worldToParent = getParent()->worldToNodeTransform();
                setAdditionalTransform(
                    cocos2d::CCAffineTransformConcat(layerToWorld,
                                                     worldToParent));
            }
        }
        CCDrawNode::visit();
    }
};

geode::Ref<TrajectoryDrawNode> g_drawNode = nullptr;
geode::Ref<PlayerObject> g_dummyJump = nullptr;
geode::Ref<PlayerObject> g_dummyNormal = nullptr;

float tickDelta(PlayLayer* layer) {
    if (g_dtStepValid && g_dtStep > 0.0f) {
        return g_dtStep;
    }
    double tps = 240.0;
    if (auto* engine = ReplayEngine::get()) {
        tps = engine->runtimeTickRate();
    }
    double warp = 1.0;
    if (layer) {
        warp = std::clamp<double>(layer->m_gameState.m_timeWarp, 0.0, 1.0);
    }
    if (warp <= 0.0) {
        warp = 1.0;
    }
    return static_cast<float>(warp / std::max(1.0, tps));
}

void copyPlayerState(PlayerObject* from, PlayerObject* to) {
    if (!from || !to) return;

    to->m_wasTeleported = from->m_wasTeleported;
    to->m_fixGravityBug = from->m_fixGravityBug;
    to->m_reverseSync = from->m_reverseSync;

    to->m_yVelocityBeforeSlope = from->m_yVelocityBeforeSlope;
    to->m_dashX = from->m_dashX;
    to->m_dashY = from->m_dashY;
    to->m_dashAngle = from->m_dashAngle;
    to->m_dashStartTime = from->m_dashStartTime;
    to->m_slopeStartTime = from->m_slopeStartTime;
    to->m_justPlacedStreak = from->m_justPlacedStreak;

    to->m_lastCollisionBottom = from->m_lastCollisionBottom;
    to->m_lastCollisionTop = from->m_lastCollisionTop;
    to->m_lastCollisionLeft = from->m_lastCollisionLeft;
    to->m_lastCollisionRight = from->m_lastCollisionRight;
    to->m_unk50C = from->m_unk50C;
    to->m_unk510 = from->m_unk510;
    to->m_currentSlope2 = from->m_currentSlope2;
    to->m_preLastGroundObject = from->m_preLastGroundObject;
    to->m_slopeAngle = from->m_slopeAngle;
    to->m_slopeSlidingMaybeRotated = from->m_slopeSlidingMaybeRotated;
    to->m_quickCheckpointMode = from->m_quickCheckpointMode;
    to->m_collidedObject = from->m_collidedObject;
    to->m_lastGroundObject = from->m_lastGroundObject;
    to->m_collidingWithLeft = from->m_collidingWithLeft;
    to->m_collidingWithRight = from->m_collidingWithRight;
    to->m_maybeSavedPlayerFrame = from->m_maybeSavedPlayerFrame;
    to->m_scaleXRelated2 = from->m_scaleXRelated2;
    to->m_groundYVelocity = from->m_groundYVelocity;
    to->m_yVelocityRelated = from->m_yVelocityRelated;
    to->m_scaleXRelated3 = from->m_scaleXRelated3;

    to->m_scaleXRelated4 = from->m_scaleXRelated4;
    to->m_scaleXRelated5 = from->m_scaleXRelated5;
    to->m_isCollidingWithSlope = from->m_isCollidingWithSlope;
    to->m_isBallRotating = from->m_isBallRotating;
    to->m_unk669 = from->m_unk669;
    to->m_currentPotentialSlope = from->m_currentPotentialSlope;
    to->m_currentSlope = from->m_currentSlope;
    to->unk_584 = from->unk_584;
    to->m_collidingWithSlopeId = from->m_collidingWithSlopeId;
    to->m_slopeFlipGravityRelated = from->m_slopeFlipGravityRelated;
    to->m_slopeAngleRadians = from->m_slopeAngleRadians;
    to->m_rotationSpeed = from->m_rotationSpeed;
    to->m_rotateSpeed = from->m_rotateSpeed;
    to->m_isRotating = from->m_isRotating;
    to->m_isBallRotating2 = from->m_isBallRotating2;
    to->m_isHidden = from->m_isHidden;

    to->m_speedMultiplier = from->m_speedMultiplier;
    to->m_yStart = from->m_yStart;
    to->m_gravity = from->m_gravity;
    to->m_trailingParticleLife = from->m_trailingParticleLife;
    to->m_unk648 = from->m_unk648;
    to->m_gameModeChangedTime = from->m_gameModeChangedTime;
    to->m_padRingRelated = from->m_padRingRelated;
    to->m_maybeReducedEffects = from->m_maybeReducedEffects;
    to->m_maybeIsFalling = from->m_maybeIsFalling;
    to->m_shouldTryPlacingCheckpoint = from->m_shouldTryPlacingCheckpoint;
    to->m_playEffects = from->m_playEffects;
    to->m_maybeCanRunIntoBlocks = from->m_maybeCanRunIntoBlocks;
    to->m_hasGroundParticles = from->m_hasGroundParticles;
    to->m_hasShipParticles = from->m_hasShipParticles;
    to->m_isOnGround3 = from->m_isOnGround3;
    to->m_checkpointTimeout = from->m_checkpointTimeout;
    to->m_lastCheckpointTime = from->m_lastCheckpointTime;
    to->m_lastFlipTime = from->m_lastFlipTime;
    to->m_flashTime = from->m_flashTime;
    to->m_flashDuration = from->m_flashDuration;
    to->m_flashDelay = from->m_flashDelay;
    to->m_flashMainColor = from->m_flashMainColor;
    to->m_flashSecondColor = from->m_flashSecondColor;
    to->m_lastSpiderFlipTime = from->m_lastSpiderFlipTime;
    to->m_unkBool5 = from->m_unkBool5;
    to->m_maybeIsVehicleGlowing = from->m_maybeIsVehicleGlowing;
    to->m_accelerationOrSpeed = from->m_accelerationOrSpeed;
    to->m_snapDistance = from->m_snapDistance;
    to->m_usedRingThisStep = from->m_usedRingThisStep;
    to->m_objectSnappedTo = from->m_objectSnappedTo;
    to->m_onFlyCheckpointTries = from->m_onFlyCheckpointTries;
    to->m_maybeSpriteRelated = from->m_maybeSpriteRelated;

    to->m_useLandParticles0 = from->m_useLandParticles0;
    to->m_landParticlesAngle = from->m_landParticlesAngle;
    to->m_landParticleRelatedY = from->m_landParticleRelatedY;
    to->m_playerStreak = from->m_playerStreak;
    to->m_streakStrokeWidth = from->m_streakStrokeWidth;
    to->m_disableStreakTint = from->m_disableStreakTint;
    to->m_alwaysShowStreak = from->m_alwaysShowStreak;
    to->m_shipStreakType = from->m_shipStreakType;
    to->m_slopeRotation = from->m_slopeRotation;
    to->m_currentSlopeYVelocity = from->m_currentSlopeYVelocity;
    to->m_unk3d0 = from->m_unk3d0;
    to->m_blackOrbRelated = from->m_blackOrbRelated;
    to->m_unk3e0 = from->m_unk3e0;
    to->m_unk3e1 = from->m_unk3e1;
    to->m_isAccelerating = from->m_isAccelerating;
    to->m_isCurrentSlopeTop = from->m_isCurrentSlopeTop;
    to->m_collidedTopMinY = from->m_collidedTopMinY;
    to->m_collidedBottomMaxY = from->m_collidedBottomMaxY;
    to->m_collidedLeftMaxX = from->m_collidedLeftMaxX;
    to->m_collidedRightMinX = from->m_collidedRightMinX;
    to->m_fadeOutStreak = from->m_fadeOutStreak;
    to->m_canPlaceCheckpoint = from->m_canPlaceCheckpoint;
    to->m_originalMainColor = from->m_originalMainColor;
    to->m_originalSecondColor = from->m_originalSecondColor;
    to->m_maybeIsColliding = from->m_maybeIsColliding;

    to->m_jumpBuffered = from->m_jumpBuffered;
    to->m_stateRingJump = from->m_stateRingJump;
    to->m_wasJumpBuffered = from->m_wasJumpBuffered;
    to->m_wasRobotJump = from->m_wasRobotJump;
    to->m_stateJumpBuffered = from->m_stateJumpBuffered;
    to->m_stateRingJump2 = from->m_stateRingJump2;
    to->m_touchedRing = from->m_touchedRing;
    to->m_touchedCustomRing = from->m_touchedCustomRing;
    to->m_touchedGravityPortal = from->m_touchedGravityPortal;
    to->m_maybeTouchedBreakableBlock = from->m_maybeTouchedBreakableBlock;
    to->m_jumpRelatedAC2 = from->m_jumpRelatedAC2;
    to->m_touchedPad = from->m_touchedPad;

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
    to->m_isDead = from->m_isDead;
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


cocos2d::ccColor4F parseHexColor(std::string const& hex, cocos2d::ccColor4F fallback) {
    std::string s = hex;
    if (!s.empty() && s.front() == '#') s.erase(s.begin());
    if (s.size() != 6 && s.size() != 8) return fallback;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return fallback;
    }
    auto byte = [&](size_t i) {
        return static_cast<float>(std::stoi(s.substr(i, 2), nullptr, 16)) / 255.0f;
    };
    return ccc4f(byte(0), byte(2), byte(4), s.size() == 8 ? byte(6) : 1.0f);
}

PlayerObject* createVirtualPlayer(PlayLayer* layer) {
    if (!layer) return nullptr;
    auto* dummy = PlayerObject::create(1, 1, layer, layer, true);
    if (dummy) {
        dummy->setPosition({0, 105});
        dummy->setVisible(false);
        dummy->setID("skk-trajectory-dummy"_spr);
        if (layer->m_objectLayer) {
            layer->m_objectLayer->addChild(dummy);
        }
    }
    return dummy;
}

void reset();

void initForLayer(PlayLayer* layer) {
    if (!layer || !layer->m_objectLayer) return;

    reset();

    g_dummyJump = createVirtualPlayer(layer);
    g_dummyNormal = createVirtualPlayer(layer);

    auto* node = TrajectoryDrawNode::create();
    if (node) {
        node->setBlendFunc(ccBlendFunc{GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        node->setZOrder(9999);
        node->setID("skk-trajectory"_spr);
        node->m_bUseArea = false;
        layer->addChild(node, 9999);
        g_drawNode = node;
    }
    skkbot2::log::info(
        "[TRAJ] initForLayer: dummyJump={} dummyNormal={} drawNode={}",
        g_dummyJump != nullptr, g_dummyNormal != nullptr, g_drawNode != nullptr);
}

void clearCollisionLogs(PlayerObject* player) {
    if (!player) return;
    if (player->m_collisionLogTop) player->m_collisionLogTop->removeAllObjects();
    if (player->m_collisionLogBottom) player->m_collisionLogBottom->removeAllObjects();
    if (player->m_collisionLogLeft) player->m_collisionLogLeft->removeAllObjects();
    if (player->m_collisionLogRight) player->m_collisionLogRight->removeAllObjects();
}

void renderHitboxOutline(cocos2d::CCRect const& rect, cocos2d::ccColor4F const& border) {
    if (!g_drawNode) return;
    cocos2d::CCPoint vertices[4] = {
        {rect.getMinX(), rect.getMinY()},
        {rect.getMinX(), rect.getMaxY()},
        {rect.getMaxX(), rect.getMaxY()},
        {rect.getMaxX(), rect.getMinY()},
    };
    g_drawNode->drawPolygon(vertices, 4, {0.f, 0.f, 0.f, 0.f}, 0.25f, border);
}


void simulatePath(PlayLayer* layer, PlayerObject* simPlayer, PlayerObject* sourcePlayer,
                  bool isJumpInput) {
    if (!layer || !simPlayer || !sourcePlayer || !g_drawNode) return;

    simPlayer->copyAttributes(sourcePlayer);
    copyPlayerState(sourcePlayer, simPlayer);

    simPlayer->m_playEffects = false;
    simPlayer->m_hasNoEffects = true;
    simPlayer->m_maybeReducedEffects = g_settings.performance;

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

    simPlayer->setVisible(false);
    g_simDead = false;

    auto* engine = ReplayEngine::get();
    int iterations = std::max(1, engine ? engine->pathLength : g_settings.iterations);
    int stepMultiplier = (iterations > 150) ? 2 : std::max(1, g_settings.stepMultiplier);
    int effectiveIterations = std::max(1, iterations / stepMultiplier);

    cocos2d::ccColor4F lineColor = isJumpInput ? g_settings.jumpColor : g_settings.normalColor;

    float effectiveDt = tickDelta(layer) * static_cast<float>(stepMultiplier);

    for (int i = 0; i < effectiveIterations; ++i) {
        cocos2d::CCPoint currentPos = simPlayer->getPosition();
        clearCollisionLogs(simPlayer);

        if (g_simDead) break;

        simPlayer->update(effectiveDt);

        if (!g_settings.noCollisions && layer->checkCollisions(simPlayer, effectiveDt, false) == 1) {
            g_simDead = true;
        }

        g_drawNode->drawSegment(currentPos, simPlayer->getPosition(), 1.5f, lineColor);
    }

    renderHitboxOutline(simPlayer->getObjectRect(), g_settings.hitboxColor);
    renderHitboxOutline(simPlayer->getObjectRect(0.25f, 0.25f), g_settings.hitboxInnerColor);
}

struct SimScope {
    bool previousEngineFlag = false;

    SimScope() {
        if (auto* engine = ReplayEngine::get()) {
            previousEngineFlag = engine->simulatingPath;
            engine->simulatingPath = true;
        }
        g_simulating = true;
    }

    ~SimScope() {
        g_simulating = false;
        if (auto* engine = ReplayEngine::get()) {
            engine->simulatingPath = previousEngineFlag;
        }
    }

    SimScope(SimScope const&) = delete;
    SimScope& operator=(SimScope const&) = delete;
};

void runSimulation(PlayLayer* layer) {
    if (!g_drawNode) return;

    auto* engine = ReplayEngine::get();
    bool const enabled = engine ? engine->showTrajectory : g_settings.enabled;
    bool const suppressed = !enabled
        || !layer
        || skkbot2::render::Renderer::get().isRecording()
        || !layer->m_started
        || layer->m_isPaused
        || layer->m_hasCompletedLevel
        || layer->m_levelEndAnimationStarted
        || !layer->m_player1
        || layer->m_player1->m_isDead;

    if (suppressed) {
        g_drawNode->clear();
        static int prevEnabled = -1;
        static int supLogged = 0;
        if ((int)enabled != prevEnabled) {
            prevEnabled = enabled;
            supLogged = 0;
            skkbot2::log::info("[TRAJ] enabled -> {} (layer={} started={})",
                enabled ? "true" : "false", layer != nullptr,
                layer ? layer->m_started : false);
        }
        if (enabled && supLogged < 10) {
            supLogged++;
            skkbot2::log::info(
                "[TRAJ] suppressed #{}: layer={} recording={} "
                "started={} paused={} completed={} endAnim={} p1={} dead={}",
                supLogged, layer != nullptr,
                skkbot2::render::Renderer::get().isRecording(),
                layer ? layer->m_started : false,
                layer ? layer->m_isPaused : false,
                layer ? layer->m_hasCompletedLevel : false,
                layer ? layer->m_levelEndAnimationStarted : false,
                layer ? (layer->m_player1 != nullptr) : false,
                layer && layer->m_player1 ? layer->m_player1->m_isDead : false);
        }
        return;
    }

    if (!g_dummyJump || !g_dummyNormal || !g_drawNode) {
        initForLayer(layer);
        if (!g_dummyJump || !g_dummyNormal || !g_drawNode) return;
    }

    if (auto* engine = ReplayEngine::get()) {
        g_settings.tps = static_cast<float>(engine->runtimeTickRate());
    }

    g_drawNode->clear();

    static int s_logCounter = 0;
    if (++s_logCounter % 180 == 1 && layer->m_player1) {
        skkbot2::log::info("[TRAJ] sim pos=({:.1f}, {:.1f}) yVel={:.2f} dart={} ship={} iter={}",
            layer->m_player1->m_position.x, layer->m_player1->m_position.y,
            layer->m_player1->m_yVelocity,
            layer->m_player1->m_isDart, layer->m_player1->m_isShip,
            engine ? engine->pathLength : g_settings.iterations);
    }

    SimScope scope;

    simulatePath(layer, g_dummyJump, layer->m_player1, true);
    simulatePath(layer, g_dummyNormal, layer->m_player1, false);

    if (layer->m_gameState.m_isDualMode && layer->m_player2) {
        simulatePath(layer, g_dummyJump, layer->m_player2, true);
        simulatePath(layer, g_dummyNormal, layer->m_player2, false);
    }
}

void reset() {
    if (g_drawNode) {
        g_drawNode->removeFromParentAndCleanup(true);
        g_drawNode = nullptr;
    }
    if (g_dummyJump) {
        g_dummyJump->removeFromParentAndCleanup(true);
        g_dummyJump = nullptr;
    }
    if (g_dummyNormal) {
        g_dummyNormal->removeFromParentAndCleanup(true);
        g_dummyNormal = nullptr;
    }
    g_simulating = false;
    g_simDead = false;
    g_dtStep = 1.0f / 240.0f;
    g_dtStepValid = false;
}

}

bool isSimulating() {
    return g_simulating;
}

Settings& settings() {
    return g_settings;
}

}


class $modify(SkkTrajectoryPlayLayer, PlayLayer) {
    struct Fields {
        ~Fields() {
            skk::trajectory::reset();
        }
    };

    static void onModify(auto& self) {
        (void) self.setHookPriority("PlayLayer::destroyPlayer", -50);
    }

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;

        m_fields.self();
        skk::trajectory::initForLayer(this);

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (!skk::trajectory::g_drawNode || !skk::trajectory::g_dummyJump || !skk::trajectory::g_dummyNormal) {
            skk::trajectory::initForLayer(this);
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object != m_anticheatSpike && skk::trajectory::g_simulating) {
            if (player == skk::trajectory::g_dummyJump
                || player == skk::trajectory::g_dummyNormal) {
                skk::trajectory::g_simDead = true;
                return;
            }
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (skk::trajectory::g_simulating) return;
        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(SkkTrajectoryBaseGameLayer, GJBaseGameLayer) {
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (skk::trajectory::g_simulating) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, std::vector<GameObject*>* objects,
                               int objectCount, float dt) {
        if (!skk::trajectory::g_simulating) {
            if (objects == &m_calcNonEffectObjects && objectCount > 0 && skk::phys::nearMoversEnabled()) {
                objects = skk::phys::filterNearMovers(player, objects, objectCount, dt);
                objectCount = static_cast<int>(objects->size());
            }
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        if (skk::trajectory::g_settings.noCollisions) return;
        if (!player || !objects || objectCount <= 0) return;

        auto addCollisionObject = [](GameObject* obj, std::vector<GameObject*>& vec,
                                     int& count, int& index) {
            if (count < index) {
                vec.at(count) = obj;
            } else {
                vec.push_back(obj);
                index++;
            }
            count++;
        };

        cocos2d::CCRect const playerRect = player->getObjectRect();

        for (int i = 0; i < objectCount; ++i) {
            GameObject* object = objects->at(i);

            if (!object || object->m_isGroupDisabled || object->m_isDisabled
                || object == m_anticheatSpike) {
                continue;
            }

            auto const type = object->m_objectType;

            if (type == GameObjectType::Solid || type == GameObjectType::Breakable) {
                addCollisionObject(object, m_solidCollisionObjects,
                                   m_solidCollisionObjectsCount, m_solidCollisionObjectsIndex);
                continue;
            }

            if (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard) {
                addCollisionObject(object, m_hazardCollisionObjects,
                                   m_hazardCollisionObjectsCount, m_hazardCollisionObjectsIndex);
                continue;
            }

            if (type == GameObjectType::Slope) {
                cocos2d::CCRect const slopeRect = object->getObjectRect(2.0f, 2.0f);
                if (!playerRect.intersectsRect(slopeRect)) continue;

                bool const needsFix = !m_levelSettings->m_fixRadiusCollision
                    || object->m_objectRadius <= 0.0;
                if (object->m_shouldUseOuterOb && needsFix) {
                    OBB2D* box = object->getOrientedBox();
                    player->updateOrientedBox();

                    OBB2D* playerBox = static_cast<GameObject*>(player)->getOrientedBox();
                    if (!box->overlaps1Way(playerBox)) continue;
                }

                if (!player->m_isSideways) {
                    player->collidedWithSlopeInternal(dt, object, false);
                } else {
                    player->handleRotatedCollisionInternal(
                        dt, object, {0.0f, 0.0f, 0.0f, 0.0f}, false, false, true);
                }
            }
        }
    }

    void flipGravity(PlayerObject* object, bool flip, bool noEffects) {
        if (skk::trajectory::g_simulating) return;
        GJBaseGameLayer::flipGravity(object, flip, noEffects);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* object) {
        if (skk::trajectory::g_simulating) return;
        GJBaseGameLayer::playerTouchedRing(player, object);
    }

    void destroyObject(GameObject* object) {
        if (skk::trajectory::g_simulating) {
            if (object->m_objectType == GameObjectType::Breakable) return;
        }
        GJBaseGameLayer::destroyObject(object);
    }
};

class $modify(SkkTrajectoryPlayerObject, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);

        if (skk::trajectory::g_simulating) return;

        auto* pl = PlayLayer::get();
        if (!pl || (this != pl->m_player1 && this != pl->m_player2)) return;

        if (dt > 0.0f) {
            skk::trajectory::g_dtStep = dt;
            skk::trajectory::g_dtStepValid = true;
        }
    }

    void stopDashing() {
        if (!skk::trajectory::g_simulating) return PlayerObject::stopDashing();

        bool orig = m_playEffects;
        m_playEffects = false;
        PlayerObject::stopDashing();
        m_playEffects = orig;
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (skk::trajectory::g_simulating) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* object, bool skipCheck) {
        if (skk::trajectory::g_simulating) return;
        PlayerObject::ringJump(object, skipCheck);
    }

    void incrementJumps() {
        if (skk::trajectory::g_simulating) return;
        PlayerObject::incrementJumps();
    }
};

class $modify(SkkTrajectoryEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, std::vector<int> const* remapKeys) {
        if (skk::trajectory::g_simulating) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(SkkTrajectoryGameObject, GameObject) {
    void playShineEffect() {
        if (skk::trajectory::g_simulating) return;
        GameObject::playShineEffect();
    }
};

class $modify(SkkTrajectoryHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (skk::trajectory::g_simulating) return;
        HardStreak::addPoint(point);
    }
};

