#pragma once

#include <Geode/Geode.hpp>
#include "game_object_physics_snapshot.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

struct FullPlayerCheckpoint {
    static FullPlayerCheckpoint store(PlayerObject* p);
    void apply(PlayerObject* p) const;

    float p_x = 0.f, p_y = 0.f;
    float p_rotation = 0.f;
    float rotationSpeed = 0.f;
    float shipRotX = 0.f, shipRotY = 0.f;
    float lastPortalX = 0.f, lastPortalY = 0.f;
    float lastGroundedX = 0.f, lastGroundedY = 0.f;

    double yVelocity = 0.0;
    double fallSpeed = 0.0;
    double platformerXVelocity = 0.0;
    double groundYVelocity = 0.0;
    double yVelocityBeforeSlope = 0.0;
    double yVelocityRelated = 0.0;
    double scaleXRelated = 0.0, scaleXRelated2 = 0.0, scaleXRelated3 = 0.0;
    double scaleXRelated4 = 0.0, scaleXRelated5 = 0.0;
    double scaleXRelatedTime = 0.0;
    float playerSpeed = 0.f;
    float vehicleSize = 1.f;
    float platformerVelocityRelated = 0.f;
    float xVelocityRelated = 0.f, xVelocityRelated2 = 0.f;
    float yVelocityRelated3 = 0.f;
    float slopeVelocity = 0.f;

    double gravity = 1.0;
    double speedMultiplier = 1.0;
    double snapDistance = 0.0;
    double accelerationOrSpeed = 0.0;
    double yStart = 0.0, unk_584 = 0.0;
    double unk3d0 = 0.0;
    float gravityMod = 1.f;
    float unk648 = 0.f, unk838 = 0.f;
    float somethingPlayerSpeedTime = 0.f, playerSpeedAC = 0.f;

    float slopeAngle = 0.f;
    float slopeAngleRadians = 0.f;
    double slopeRotation = 0.0;
    double currentSlopeYVelocity = 0.0;
    double slopeStartTime = 0.0;
    double maybeSlopeForce = 0.0;
    double physDeltaRelated = 0.0;
    double changedDirectionsTime = 0.0;
    double slopeEndTime = 0.0;
    double maybeChangedDirectionAngle = 0.0;
    double unkUnused2 = 0.0;

    double dashX = 0.0, dashY = 0.0, dashAngle = 0.0, dashStartTime = 0.0;
    double blackOrbRelated = 0.0;

    double totalTime = 0.0;
    double lastCheckpointTime = 0.0;
    double lastJumpTime = 0.0;
    double lastFlipTime = 0.0;
    double lastSpiderFlipTime = 0.0;
    double gameModeChangedTime = 0.0;
    double lastLandTime = 0.0;
    double flashTime = 0.0;
    float flashDuration = 0.f, flashDelay = 0.f;
    float audioScale = 0.f;
    float unkAngle1 = 0.f;

    int lastCollisionBottom = 0, lastCollisionTop = 0;
    int lastCollisionLeft = 0, lastCollisionRight = 0;
    int prevCollisionBottom = 0, prevCollisionTop = 0;
    int unk50C = 0, unk510 = 0;
    int collidingWithSlopeId = 0;
    double collidedTopMinY = 0.0, collidedBottomMaxY = 0.0;
    double collidedLeftMaxX = 0.0, collidedRightMinX = 0.0;
    int groundObjectMaterial = 0;
    int maybeSavedPlayerFrame = 0;
    int maybeSlidingTime = 0;

    int stateOnGround = 0;
    int stateBoostX = 0, stateBoostY = 0;
    int maybeStateForce2 = 0, stateScale = 0;
    int stateNoAutoJump = 0, stateDartSlide = 0;
    int stateHitHead = 0, stateFlipGravity = 0;
    int stateForce = 0;
    int reverseRelated = 0;
    double maybeReverseSpeed = 0.0, maybeReverseAcceleration = 0.0;
    int onFlyCheckpointTries = 0;
    int playerStreak = 0;
    float streakStrokeWidth = 0.f;
    int shipStreakType = 0;
    float trailingParticleLife = 0.f;
    float landParticlesAngle = 0.f, landParticleRelatedY = 0.f;
    int followRelated = 0;
    int iconRequestID = 0, unkUnused = 0;
    float unkUnused3 = 0.f;
    float fallStartY = 0.f;
    float slopeSlidingMaybeRotated_angle = 0.f;

    uint64_t flags1 = 0;
    uint64_t flags2 = 0;

    double rotateSpeed = 0.0;
    int dashFireFrame = 0;
    bool holdingRight = false;
    bool holdingLeft = false;
    bool leftPressedFirst = false;
    bool wasOnSlope = false;
    bool isOnSlope = false;
    bool isOnGround2 = false;
    bool inputsLocked = false;
    bool maybeUpsideDownSlope = false;
    double maybeSlidingStartTime = 0.0;
    cocos2d::CCPoint stateForceVector = { 0.f, 0.f };
    geode::SeedValueRSV jumpRelatedAC2;
    unsigned char stateJumpBuffered = 0;
    unsigned char stateUnk = 0;
    unsigned char stateNoStickX = 0;
    unsigned char stateNoStickY = 0;
    unsigned char stateUnk2 = 0;
    bool ringJumpRelated = false;
    bool unkA29 = false;
    bool unkA99 = false;

    std::unordered_map<int, GameObject*> potentialSlopeMap;
    std::map<int, bool> jumpPadRelated;
    std::unordered_set<int> ringRelatedSet;
    std::unordered_set<int> touchedRings;
    std::unordered_map<int, GJPointDouble> rotateObjectsRelated;
    std::vector<cocos2d::CCObject*> touchingRings;
    std::string currentRobotAnimation;

    std::vector<std::pair<int, GameObject*>> collisionLogTop;
    std::vector<std::pair<int, GameObject*>> collisionLogBottom;
    std::vector<std::pair<int, GameObject*>> collisionLogLeft;
    std::vector<std::pair<int, GameObject*>> collisionLogRight;
    int collisionLogTopAlias = 0;

    std::array<bool, 3> clickReleased = { true, true, true };

    cocos2d::ccColor3B playerColor1 = { 0, 0, 0 };
    cocos2d::ccColor3B playerColor2 = { 0, 0, 0 };
    cocos2d::ccColor3B originalMainColor = { 0, 0, 0 };
    cocos2d::ccColor3B originalSecondColor = { 0, 0, 0 };

    std::vector<cocos2d::CCPoint> waveTrailPoints;
    cocos2d::CCPoint waveTrailCurrentPoint = { 0.f, 0.f };
    float waveTrailWaveSize = 0.f;
    float waveTrailPulseSize = 0.f;
    bool waveTrailIsSolid = false;
    bool waveTrailIsFlipped = false;
    bool waveTrailDrawStreak = false;
};

struct FullCheckpointBundle {
    FullPlayerCheckpoint player1;
    FullPlayerCheckpoint player2;
    bool hasDual = false;

    std::unordered_map<int, int> persistentItemMap;
    std::array<float, 2000> varianceValues{};
    GameObject* player1CollisionBlock = nullptr;
    GameObject* player2CollisionBlock = nullptr;
    double extraDelta = 0.0;
    int unk32d0 = 0;
    int unk32ec = 0;
    int currentStep = 0;
    float unk3380 = 0.f;
    skk::GameObjectPhysicsSnapshotMap gameObjectPhysics;

    uint64_t seedState = 0;
    uint64_t shakeRandomState = 0;
    uint64_t teleportRandomState = 0;

    std::vector<GameObject*> brokenObjects;

    int lastActivatedPortalUID = -1;
    int dashRingUID = -1;
    int collidedObjectUID = -1;
    int lastGroundObjectUID = -1;
    int preLastGroundObjectUID = -1;
    int currentSlopeUID = -1;
    int currentSlope2UID = -1;
    int currentPotentialSlopeUID = -1;
    int collidingWithLeftUID = -1;
    int collidingWithRightUID = -1;
    int objectSnappedToUID = -1;
    int maybeLastGroundObjectUID = -1;

    std::vector<int> calcNonEffectObjectUIDs;
    int calcNonEffectObjectsSize = 0;
};
