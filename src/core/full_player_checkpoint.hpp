#pragma once

#include <Geode/Geode.hpp>
#include <vector>

using namespace geode::prelude;

struct FullPlayerCheckpoint {
    static FullPlayerCheckpoint store(PlayerObject* p);
    void apply(PlayerObject* p) const;

    // --- Position & Rotation ---
    float p_x = 0.f, p_y = 0.f;
    float p_rotation = 0.f;
    float rotationSpeed = 0.f;
    float shipRotX = 0.f, shipRotY = 0.f;
    float lastPortalX = 0.f, lastPortalY = 0.f;
    float lastGroundedX = 0.f, lastGroundedY = 0.f;

    // --- Velocity ---
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

    // --- Physics ---
    double gravity = 1.0;
    double speedMultiplier = 1.0;
    double snapDistance = 0.0;
    double accelerationOrSpeed = 0.0;
    double yStart = 0.0, unk_584 = 0.0;
    double unk3d0 = 0.0;
    float gravityMod = 1.f;
    float unk648 = 0.f, unk838 = 0.f;
    float somethingPlayerSpeedTime = 0.f, playerSpeedAC = 0.f;

    // --- Slope ---
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

    // --- Dash ---
    double dashX = 0.0, dashY = 0.0, dashAngle = 0.0, dashStartTime = 0.0;
    double blackOrbRelated = 0.0;

    // --- Timers ---
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

    // --- Collision ---
    int lastCollisionBottom = 0, lastCollisionTop = 0;
    int lastCollisionLeft = 0, lastCollisionRight = 0;
    int unk50C = 0, unk510 = 0;
    int collidingWithSlopeId = 0;
    double collidedTopMinY = 0.0, collidedBottomMaxY = 0.0;
    double collidedLeftMaxX = 0.0, collidedRightMinX = 0.0;
    int groundObjectMaterial = 0;
    int maybeSavedPlayerFrame = 0;
    int maybeSlidingTime = 0;

    // --- State ints ---
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

    // --- Flags (packed as bitmasks) ---
    uint64_t flags1 = 0;   // wasTeleported..inputsLocked
    uint64_t flags2 = 0;   // isShip..enable22Changes

    // --- Timers (extended v2) ---
    double rotateSpeed = 0.0;
    int dashFireFrame = 0;
    bool holdingRight = false;
    bool holdingLeft = false;
    bool leftPressedFirst = false;
    bool wasOnSlope = false;
    bool maybeUpsideDownSlope = false;
    unsigned char stateJumpBuffered = 0;
    unsigned char stateUnk = 0;
    unsigned char stateNoStickX = 0;
    unsigned char stateNoStickY = 0;
    unsigned char stateUnk2 = 0;
    bool ringJumpRelated = false;
    bool unkA29 = false;
    bool unkA99 = false;

    // --- Click release state (GDH pattern) ---
    std::array<bool, 3> clickReleased = { true, true, true };

    // --- Color ---
    cocos2d::ccColor3B playerColor1 = { 0, 0, 0 };
    cocos2d::ccColor3B playerColor2 = { 0, 0, 0 };
    cocos2d::ccColor3B originalMainColor = { 0, 0, 0 };
    cocos2d::ccColor3B originalSecondColor = { 0, 0, 0 };

    // --- Wave trail ---
    std::vector<cocos2d::CCPoint> waveTrailPoints;
};

struct FullCheckpointBundle {
    FullPlayerCheckpoint player1;
    FullPlayerCheckpoint player2;
    bool hasDual = false;

    // Layer-level state (silicate pattern)
    std::unordered_map<int, int> persistentItemMap;
    std::array<float, 2000> varianceValues{};

    // RNG state
    uint64_t seedState = 0;
    uint64_t shakeRandomState = 0;
    uint64_t teleportRandomState = 0;

    // Broken objects
    std::vector<GameObject*> brokenObjects;

    // Object pointers (session-only, not serialized)
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

    // calcNonEffectObjects (session-only)
    std::vector<int> calcNonEffectObjectUIDs;
    int calcNonEffectObjectsSize = 0;
};
