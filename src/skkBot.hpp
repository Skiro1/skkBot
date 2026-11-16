#ifndef _skkBot_hpp
#define _skkBot_hpp

#include "utils.hpp"
#include "lang/localization.hpp"
#include "format/replay.hpp"
#include "format/GBR/gbr.hpp"
#include "format/skk_format.hpp"
#include "core/practice_fix_bridge.hpp"
#include "core/noclip_accuracy.hpp"
#include "core/replay_timing.hpp"
#include "core/rng_lock.hpp"
#include "core/playback_fps.hpp"
#include "core/gameplay_layer.hpp"
#include "core/paths.hpp"
#include "core/page_heap_debug.hpp"
#include "core/step_profiler.hpp"
#include "hacks/autoclicker.hpp"
#include "hacks/physicsbypass.hpp"
#include "gui/skkbot_gui.hpp"
#include "render/renderer.hpp"

#include <Geode/Bindings.hpp>
#include <Geode/loader/Loader.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/autosave_recorder.hpp"

using namespace geode::prelude;

namespace skk::branding {
    inline constexpr const char* kProductName = "skkBot";
    inline constexpr const char* kEditionLabel = "FREE";

    inline std::string versionText() {
        return std::string("v") + MOD_VERSION;
    }

    inline std::string versionBadge() {
        return versionText() + " " + kEditionLabel;
    }

    inline std::string baseBrand() {
        return std::string(kProductName) + " " + versionText();
    }

    inline std::string fullBrand() {
        return std::string(kProductName) + " " + versionBadge();
    }
}

namespace skk::hitboxes {
    void forceRefresh();
}

namespace skkbot2 {
void notePlayerInput(char const* kind, int key);

void dumpPlayerInputTally(char const* reason);
}

enum MacroMode {
    MODE_DISABLED,
    MODE_CAPTURE,
    MODE_EXECUTE
};

enum ValidationResult {
    RESULT_OK,
    RESULT_KEY_MISSING,
    RESULT_KEY_MALFORMED,
    RESULT_NETWORK_FAILURE,
    RESULT_DEVICE_MISMATCH,
    RESULT_UNEXPECTED,
    RESULT_DEVICE_CONFLICT
};

struct QueuedSubstepInput {
    float offset = 0.0f;
    int button = 1;
    bool player2 = false;
    bool pressed = false;
    bool macroInput = false;
};

struct QueuedSubstepSegment {
    float deltaFactor = 1.0f;
    bool endStep = true;
    bool hasActionAfter = false;
    QueuedSubstepInput actionAfter;
};

class ReplayEngine {
public:
    struct QueuedMacroCommand {
        int button = 0;
        bool down = false;
        bool player2 = false;
        double timestamp = 0.0;
    };

    struct QueuedCaptureCommand {
        int button = 0;
        bool down = false;
        bool player2 = false;
        double timestamp = 0.0;
    };

    static constexpr double kBaseTickRate = 240.0;
    static constexpr int kTrajectoryLengthDefault = 150;
    static constexpr int kTrajectoryLengthMin = 10;
    static constexpr int kTrajectoryLengthMax = 1200;

    MacroMode engineMode = MODE_DISABLED;
    ValidationResult validationStatus = RESULT_OK;

    bool dataModified = false;
    uint64_t macroGeneration = 0;
    uint64_t macroRevision = 0;

    std::atomic<bool> macroSaveInFlight{false};

    bool macroSaveBusy() const { return macroSaveInFlight.load(std::memory_order_acquire); }

    float tickAccumulator = 0.f;

    bool renderingDisabled = false;
    bool bypassIgnored = false;
    bool recentlyInitialized = false;
    bool inputIgnored = false;
    bool tickStepping = false;
    bool pendingStep = false;
    bool singleTickStep = false;
    bool renderInternal = false;
    bool pulseFix = false;
    bool priorTickStepping = false;
    bool stepKeyActive = false;
    bool audioPitchEnabled = true;
    bool noMirrorEffect = false;
    bool noMirrorRecordingOnly = false;
    bool fastPlayback = false;
    bool respawnTimeOverrideEnabled = false;
    int respawnTimeOverrideMs = 1000;
    bool layoutMode = false;
    bool layoutModeRegularBg = false;
    bool layoutModeDefaultArt = false;
    int layoutModeBackgroundR = 160;
    int layoutModeBackgroundG = 160;
    int layoutModeBackgroundB = 160;
    int layoutModeGroundR = 160;
    int layoutModeGroundG = 160;
    int layoutModeGroundB = 160;
    bool disableShaders = false;
    bool noDeathEffect = false;
    bool noEffect = false;
    bool hideEndscreen = false;
    bool hideNewBest = false;
    bool macroInputActive = false;
    bool protectedMode = false;
    bool autoSafeMode = false;
    bool pathPreview = false;
    bool collisionBypass = false;
    bool noclipPlayer1 = true;
    bool noclipPlayer2 = true;
    bool collisionLimitActive = false;
    float collisionThreshold = 0.0f;
    bool noclipDeathFlash = true;
    float noclipFlashAlpha = 0.0f;
    float noclipDeathColorR = 1.0f;
    float noclipDeathColorG = 0.0f;
    float noclipDeathColorB = 0.0f;

    bool safeMode = false;
    bool persistenceMode = false;

    bool waveTrailFix = true;
    bool waveTrailDragFix = true;
    bool waveTrailDrawFix = true;
    bool waveTrailColorOverride = false;
    float waveTrailColorR = 1.0f;
    float waveTrailColorG = 1.0f;
    float waveTrailColorB = 1.0f;
    float waveTrailOpacity = 1.0f;
    float waveTrailOutlineOpacity = 0.65f;
    bool waveTrailEditor = true;

    bool showHitboxes = false;
    bool hitboxFill = false;
    bool hitboxOnDeath = false;
    bool noclipHitboxOnDeath = false;
    bool hitboxTrail = false;
    int hitboxTrailLength = 240;

    bool showTrajectory = false;
    bool trajectoryBothSides = false;
    int pathLength = kTrajectoryLengthDefault;

    int noclipTotalFrames = 0;
    int noclipUnsafeFrames = 0;
    int noclipDeathEvents = 0;
    int noclipAccuracyDecimals = 2;
    bool noclipWouldDieThisFrame = false;
    bool noclipDeadLastFrame = false;
    PlayerObject* noclipLastDeathPlayer = nullptr;
    GameObject* noclipLastDeathObject = nullptr;

    bool rngLocked = true;
    static constexpr unsigned int rngSeedVal = 100;
    uintptr_t capturedRngState = 0;

    double gameSpeed = 1.0;
    double tickRate = 240.0;
    MacroSequence* activeMacro = nullptr;

    SkMacro* activeSk = nullptr;
    bool skMode = true;

    enum class RecordingFormat : uint8_t {
        SKK3 = 0,
        GDRBinary = 1,
        GDRJson = 2,
    };
    RecordingFormat selectedRecordingFormat = RecordingFormat::SKK3;

    void applyRecordingFormatSelection() {
        skMode = (selectedRecordingFormat == RecordingFormat::SKK3);
    }

    std::unordered_map<CheckpointObject*, CheckpointStateBundle> storedRestorePoints;
    std::vector<CheckpointObject*> platformerCheckpointObjects;
    void trackPlatformerCheckpoint(CheckpointObject* checkpoint) {
        if (!checkpoint) return;
        for (auto* saved : platformerCheckpointObjects)
            if (saved == checkpoint) return;
        checkpoint->retain();
        platformerCheckpointObjects.push_back(checkpoint);
    }
    void clearPlatformerCheckpoints() {
        for (auto* checkpoint : platformerCheckpointObjects)
            if (checkpoint) checkpoint->release();
        platformerCheckpointObjects.clear();
    }
    std::vector<CheckpointStateBundle> practiceFixQueue;
    int lastTickIndex = 0;
    int respawnTickIndex = -1;
    SkMacro* captureStaging = nullptr;
    int captureStagingStartTick = 0;
    int captureStagingTouchFloor = 0;
    bool captureStagingActive() const {
        return engineMode == MODE_CAPTURE && captureStaging && skMode && activeSk && !canUsePersistenceCapture();
    }
    void captureStagingReset(int startTick);
    void clearFlatCaptureTimeline();
    void appendCaptureStagingTo(SkMacro& snapshot) const;
    void captureCommitStagingToFloor(int floorTick);
    void captureFlushStagingAll();
    void captureOnRespawn(int respawnTick);
    void captureOnCheckpointTouched(int tick);
    struct PendingRngBoundary {
        int progress = -1;
        uint64_t primary = 0;
        uint64_t teleport = 0;
    };
    PendingRngBoundary pendingRngBoundary;
    size_t nextBoundaryIndex = 0;
    size_t executeIndex = 0;
    size_t playbackAnchorIndex = 0;
    size_t perFramePlaybackIndex = 0;
    bool captureIgnored = false;
    bool captureDeathPending = false;
    uint64_t captureRestoreSerial = 0;
    bool levelRestarting = false;
    bool initialRun = false;
    bool pendingPlaybackStart = false;
    bool suppressInitialRunReset = false;
    enum class PendingAction { None, Record, Play };
    PendingAction pendingAction = PendingAction::None;
    std::string pendingActionName;
    bool anchorReconciliation = false;
    bool perFrameReconciliation = false;
    bool playbackExhaustionNotified = false;
    int substepProbe = 0;
    int probeAroundTick = -1;
    std::vector<int> probeAroundTicks;
    int frameEntryTick = -1;
    bool frameDispatchDone = true;
    int rngStreamMismatchReports = 0;
    int dashReattachLast[2] = { -999, -999 };
    bool stepClock = false;
    int stepPairCount = 0;
    double stepPairFrac = 0.0;

    double sessionSampleLastLevelTime = 0.0;
    long long sessionSampleStepsWindow = 0;
    double sessionSampleWallStart = 0.0;
    double sessionSampleDtSum = 0.0;
    double sampleSpdLast = 0.0;
    double emaFrameTime = 0.0;
    long long lockBacklogDrops = 0;
    double lockBacklogDropped = 0.0;
    long long sessionSchedWalks = 0;
    long long sessionVanillaExits = 0;
    long long sessionPlanCount = 0;
    long long sessionStepSum = 0;
    long long sessionResumeZeroPlans = 0;
    long long batFrozenFlagPlans = 0;
    long long batFrozenFlagSteps = 0;
    long long batFreezeCtrPlans = 0;
    long long batFreezeCtrSteps = 0;
    int startFrozenFrames = 0;
    bool startFrozenReported = false;
    double startFrozenWall = 0.0;
    bool startLiveReported = false;
    bool playbackFirstInputReported = false;
    double sessionFirstLevelTime = 0.0;
    double sessionLastLevelTime = 0.0;
    bool sessionLevelTimeInit = false;
    long long lockPlanCount = 0;
    long long lockWantedSum = 0;
    long long lockStepSum = 0;
    long long lockMaxWanted = 0;
    double lockOffCadenceFraction = 1.0;
    long long lockOffCadenceInWindow = 0;
    long long lockWindowFrames = 0;
    float lockLearnedInterval = 0.0f;
    std::vector<float> lockLearnsWindow;
    int lockAnchorFps = 0;
    int lockAnchorPendingFps = 0;
    int lockAnchorPendingHits = 0;
    double lockDeliveredTotal = 0.0;
    long long lockDeliveredFrames = 0;
    bool lockWarnedSlow = false;
    long long lockZeroFrames = 0;
    double lockDroppedCarry = 0.0;
    double lockMaxDtSkew = 0.0;
    bool lockEverOn = false;
    bool lockLoggedOn = false;
    float sampleRawDtMin = 0.0f;
    float sampleRawDtMax = 0.0f;
    double sampleRawDtSum = 0.0;
    long long  sampleRawFrames = 0;
    long long  sampleVanillaExits = 0;
    double sampleWallGapSum = 0.0;
    float  sampleWallGapMin = -1.0f;
    float  sampleWallGapMax = 0.0f;
    long long samplePhantomCount = 0;
    double samplePhantomDtSum = 0.0;
    long long samplePlansWindow = 0;
    double sampleDwmSum = 0.0;
    double sampleClampSum = 0.0;
    double sampleFedSum = 0.0;
    long long sampleDrives = 0;
    long long sampleDwmRewrites = 0;
    double sampleDebtSum = 0.0;
    long long sampleDebtCount = 0;
    double sampleWantSum = 0.0;
    double sampleFreezeDropped = 0.0;
    double lastDriveWall = 0.0;
    int lockFirstPlanFrame = -1;
    int processSliceSkips = 0;
    bool customTickPlanActive = false;
    int cbsImmediateCount = 0;
    int cbsQueueNativeCount = 0;
    int cbsWaitCount = 0;
    float playDriftMax = 0.0f;
    int playDriftMaxTick = -1;
    int playDriftLastLogTick = -100000;
    int playDriftLastSampleTick = -100000;
    float renderDriftMax = 0.0f;
    int renderDriftMaxTick = -1;
    int renderDriftLastLogTick = -100000;
    int seamGlideUntilTick = -1;
    static constexpr float kSeamGlideStepPx = 26.0f;
    static constexpr float kSeamGlideArmTicks = 40;
    int anchorInterval = 240;
    int skipTickIndex = -1;
    int skipActionTick = -1;
    int deferredReleaseA[2] = { -1, -1 };
    int deferredInputTick[2] = { -1, -1 };
    int deferredReleaseB[2][2] = { { -1, -1 }, { -1, -1 } };
    std::array<std::array<bool, 3>, 2> checkpointResumedHolds = {};
    bool activeButtons[6] = { false, false, false, false, false, false };
    bool priorButtonState[6] = { false, false, false, false, false, false };
    bool recordedHoldShadow[6] = { false, false, false, false, false, false };
    int holdProbeLast[2] = { -2, -2 };
    int captureReleaseSuppressUntil = 0;
    uint64_t gravProbeLast = 0;
    int perFrameReconciliationCatchUps = 0;
    int gravDiffReports = 0;

    static constexpr int kGravDiffReportCap = 40;
    bool lateralInputPending[2] = { false, false };
    bool dashCancelIgnored[2] = { false, false };
    bool simulatingPath = false;
    int sessionCounter = 0;
    int lastSaveTick = 0;
    bool substepMidStep = false;
    bool substepMacroDispatch = false;
    float substepCaptureOffset = -1.0f;
    std::deque<QueuedSubstepSegment> queuedSubstepSegments;
    std::deque<QueuedMacroCommand> queuedMacroCommands;
    std::deque<QueuedCaptureCommand> queuedCaptureCommands;
    bool cbsCaptureProcessingQueue = false;
    bool cbsPlaybackProcessingQueue = false;
    bool unsavedGuardActive = false;
    bool replayAccuracyEnvironmentActive = false;
    bool externalAccuracyEnvironmentActive = false;
    bool savedClickBetweenSteps = false;
    bool savedLayerClickBetweenSteps = false;
    bool hasSavedLayerClickBetweenSteps = false;
    bool savedClickOnSteps = false;
    bool savedLayerClickOnSteps = false;
    bool hasSavedLayerClickOnSteps = false;
    bool playbackTickRateOverrideActive = false;
    double savedPlaybackTickRate = kBaseTickRate;
    bool playbackFpsOverrideActive = false;
    SkAttemptSegment persistenceCaptureAttempt;
    bool persistenceCaptureDeathRecorded = false;
    size_t persistencePlaybackAttemptIndex = 0;
    bool persistencePlaybackDeathPending = false;
    int persistenceRenderBaseTick = 0;

    bool frameStepAudioActive = false;
    bool frameStepAudioWasPaused = false;
    bool frameStepMusicSeekPending = false;
    int frameStepPendingMusicTimeMs = 0;

    AccuracyMode selectedAccuracyMode = AccuracyMode::Vanilla;
    bool cosEnabled = false;
    int tickStartStep = 0;
    int lastStepDelta = -1;
    double tickStartTimestamp = 0.0;
    double lastProcessSliceTimestamp = -1.0;
    double macroStartTimestamp = -1.0;

    double tickStartWallTime = 0.0;
    double prevTickStartWallTime = 0.0;
    double prevTickStartTimestamp = 0.0;
    double inputArrivalWallTime = -1.0;
    bool hasInputArrivalWallTime = false;

    double cbsPhaseFromInputWall() const {
        if (!hasInputArrivalWallTime) {
            return -1.0;
        }
        if (prevTickStartWallTime <= 0.0 || tickStartWallTime <= prevTickStartWallTime) {
            return -1.0;
        }
        double localElapsed = inputArrivalWallTime - prevTickStartWallTime;
        double tickSpan = tickStartWallTime - prevTickStartWallTime;
        double fraction = localElapsed / tickSpan;
        if (fraction < 0.0) {
            return -1.0;
        }
        double clamped = std::min(fraction, 1.0);
        if (clamped >= 1.0) {
            return -1.0;
        }
        return clamped * (1.0 / runtimeTickRate());
    }

    int tickOffset = 0;
    int lastDispatchEffectiveTick = -1;
    bool startPosActive = false;
    std::string startPosWarning;
    bool startPosWarningIsKey = false;

    static std::string macroEntryKey(std::string const& stem, std::string const& ext) {
        return stem + " [" + ext + "]";
    }
    static std::string macroEntryStem(std::string const& entry) {
        auto const pos = entry.rfind(" [");
        return pos == std::string::npos ? entry : entry.substr(0, pos);
    }
    static std::string macroEntryExt(std::string const& entry) {
        auto const pos = entry.rfind(" [");
        if (pos == std::string::npos) {
            return {};
        }
        return entry.substr(pos + 2, entry.size() - pos - 3);
    }

    std::vector<std::string> storedMacros;
    uint32_t macroListVersion = 0;
    std::unordered_set<std::string> incompatibleMacros;
    std::unordered_set<std::string> cbsMacros;
    std::unordered_set<std::string> platformerMacros;
    std::unordered_set<std::string> skMacros;
    int hotkey_tickStep = 0x56;
    int hotkey_audioPitch = 0;
    int hotkey_protected = 0;
    int hotkey_pathPreview = 0;
    int hotkey_collision = 0;
    int hotkey_rngLock = 0;
    int hotkey_hitboxes = 0;

    void clearStartPosWarning() {
        startPosWarning.clear();
        startPosWarningIsKey = false;
    }

    void resetNoclipAccuracyStats() {
        noclipTotalFrames = 0;
        noclipUnsafeFrames = 0;
        noclipDeathEvents = 0;
        noclipWouldDieThisFrame = false;
        noclipDeadLastFrame = false;
        noclipLastDeathPlayer = nullptr;
        noclipLastDeathObject = nullptr;
    }

    void markNoclipUnsafeFrame(PlayerObject* player, GameObject* object) {
        noclipWouldDieThisFrame = true;
        if (player) {
            noclipLastDeathPlayer = player;
        }
        if (object) {
            noclipLastDeathObject = object;
        }
    }

    float noclipAccuracyPercent() const {
        return static_cast<float>(skk::noclip::accuracyPercent(noclipTotalFrames, noclipUnsafeFrames));
    }

    void setStartPosWarningKey(std::string_view key) {
        startPosWarning = std::string(key);
        startPosWarningIsKey = true;
    }

    bool hasStartPosWarning() const {
        return !startPosWarning.empty();
    }

    std::string getStartPosWarningText() const {
        if (startPosWarning.empty()) {
            return {};
        }
        if (startPosWarningIsKey) {
            return std::string(skk::lang::tr(startPosWarning));
        }
        return startPosWarning;
    }

    void reloadMacroList() {
        ++macroListVersion;
        storedMacros.clear();
        incompatibleMacros.clear();
        cbsMacros.clear();
        platformerMacros.clear();
        skMacros.clear();
        auto directory = skkbot2::paths::replays();
        std::error_code ec;
        if (!std::filesystem::exists(directory, ec) || ec) {
            return;
        }

        std::unordered_set<std::string> usableStems;

        auto addUsable = [&](std::string const& stem, std::string const& ext) {
            if (usableStems.insert(stem + ext).second) {
                storedMacros.push_back(macroEntryKey(stem, ext));
            }
        };

        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) {
                continue;
            }

            auto path = it->path();
            auto stem = skk::pathToUtf8(path.stem());
            auto extension = skk::pathToUtf8(path.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

            if (extension != ".skk" && extension != ".skc" &&
                extension != ".gdr2" && extension != ".slc" && extension != ".nhm" && extension != ".xe" &&
                extension != ".re4" && extension != ".ttrl" &&
        extension != ".echo" &&
        extension != ".ast" &&
                !skk::gbr::looksLikeGbrByExtension(extension)) {
                continue;
            }

            addUsable(stem, extension);

            if (extension == ".skk") {
                skMacros.insert(stem);
            }
        }

        if (ec) {
            skkbot2::log::warn("Failed while scanning replay directory {}: {}", skk::pathToUtf8(directory), ec.message());
        }
    }

    void queueRecord(std::string const& name) {
        pendingAction = PendingAction::Record;
        pendingActionName = name;
        skkbot2::entryFreezeReset();
        skkbot2::log::info("RECORD queued for next level entry (name='{}')", name);
    }
    void queuePlayback(std::string const& name) {
        pendingAction = PendingAction::Play;
        pendingActionName = name;
        skkbot2::entryFreezeReset();
        skkbot2::log::info("PLAY queued for next level entry (name='{}')", name);
    }
    void clearPendingAction() {
        if (pendingAction != PendingAction::None) {
            skkbot2::log::info("queued action cancelled ({})",
                pendingAction == PendingAction::Record ? "RECORD" : "PLAY");
        }
        pendingAction = PendingAction::None;
        pendingActionName.clear();
        pendingPlaybackStart = false;
    }

    bool loadMacroFromName(std::string const& name) {
        if (engineMode != MODE_DISABLED) return false;
        if (name.empty()) return false;
        namespace fs = std::filesystem;
        auto dir = skkbot2::paths::replays();
        auto file = dir / skk::stringToPath(name + ".skk");
        if (!fs::exists(file)) return false;
        std::unique_ptr<SkMacro> macro(SkMacro::loadFromPath(file));
        if (!macro) return false;
        delete activeSk;
        activeSk = macro.release();
        ++macroGeneration;
        skMode = true;
        selectedAccuracyMode = activeSk->accuracyMode;
        enableCBSForLoadedMacroIfNeeded();
        double safeTps = activeSk->framerate > 0.0 ? activeSk->framerate : tickRate;
        storedRestorePoints.clear();
        practiceFixQueue = skk::bridge::wireToBundles(activeSk->practiceFixCheckpoints, safeTps);
        return true;
    }

    bool consumePendingAction(GJGameLevel* level) {
        if (pendingAction == PendingAction::None || !level) {
            return false;
        }
        PendingAction act = pendingAction;
        std::string name = pendingActionName;
        pendingAction = PendingAction::None;
        pendingActionName.clear();
        skkbot2::log::info(
            "queued action consumed ({}) level='{}' id={} name='{}'",
            act == PendingAction::Record ? "RECORD" : "PLAY",
            level->m_levelName.c_str(), level->m_levelID, name);

        if (act == PendingAction::Record) {
            skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::Record);
            if (!beginCapture(level)) {
                skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
                engineMode = MODE_DISABLED;
                return false;
            }
            if (!name.empty()) {
                if (activeSk) activeSk->name = name;
                if (activeMacro) activeMacro->name = name;
            }
            return true;
        }

        if (!hasMacroInputs() && !loadMacroFromName(name)) {
            skkbot2::log::error(
                "PLAY queue failed: could not load macro '{}' for level='{}' id={}",
                name, level->m_levelName.c_str(), level->m_levelID);
            return false;
        }
        if (!hasMacroInputs()) {
            skkbot2::log::error("PLAY queue failed: macro '{}' has no inputs", name);
            return false;
        }
        const bool sameLevel = activeSk &&
            (activeSk->levelId > 0
                ? activeSk->levelId == static_cast<int32_t>(level->m_levelID)
                : activeSk->levelName == level->m_levelName.c_str());
        if (sameLevel) {
            const bool actualPlatformer = level->isPlatformer();
            if (activeSk->platformerMode != actualPlatformer) {
                skkbot2::log::warn(
                    "PLAY metadata repaired for matching level '{}': platformer {} -> {}",
                    level->m_levelName.c_str(),
                    activeSk->platformerMode ? "true" : "false",
                    actualPlatformer ? "true" : "false");
                activeSk->platformerMode = actualPlatformer;
            }
        }
        skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::Playback);
        suppressInitialRunReset = true;
        pendingPlaybackStart = true;
        nextBoundaryIndex = 0;
        pendingRngBoundary = {};
        skkbot2::log::info(
            "PLAY queued macro armed: name='{}' inputs={} anchors={} frames={} tps={:.3f}",
            activeSk ? activeSk->name : name,
            activeSk ? activeSk->inputs.size() : 0,
            activeSk ? activeSk->anchors.size() : 0,
            activeSk ? activeSk->perFrames.size() : 0,
            activeSk ? activeSk->framerate : tickRate);
        return true;
    }

    bool beginCapture(GJGameLevel* level) {
        if (Autoclicker::get()->enabled && Autoclicker::get()->isTimedMode()) {
            FLAlertLayer::create(
                nullptr,
                "skkBot",
                "OK",
                nullptr,
                "Timed autoclicker cannot be recorded in normal replay mode. Disable timed autoclicker before recording."
            )->show();
            return false;
        }

        pendingPlaybackStart = false;
        engineMode = MODE_CAPTURE;
        ++sessionCounter;
        storedRestorePoints.clear();
        clearPlatformerCheckpoints();
        skkbot2::debug::profilerStart("skk-record");
        anchorReconciliation = true;
        perFrameReconciliation = true;
        nextBoundaryIndex = 0;
        pendingRngBoundary = {};
        stepClock = true;
        stepPairCount = 0;
        stepPairFrac = 0.0;
        resetTimingTracking();
        resetSessionStepGridStat();
        resetRecordedHoldShadow();
        if (captureStaging) {
            delete captureStaging;
            captureStaging = nullptr;
        }
        captureStagingStartTick = 0;
        captureStagingTouchFloor = 0;
        substepProbe = 96;
        skkbot2::log::info(
            "RECORD start: level='{}' id={} persistence={} accuracy={} tps={} seed={} noclip={}",
            level ? level->m_levelName.c_str() : "?",
            level ? static_cast<long long>(level->m_levelID) : 0LL,
            persistenceMode,
            static_cast<int>(selectedAccuracyMode),
            runtimeTickRate(),
            rngSeedVal,
            collisionBypass);
        clearPersistenceRuntimeState();
        clearQueuedSubstepState();
        skkbot2::debug::setFullPageHeap(true);
        skkbot2::log::info("[BC] after setFullPageHeap");
        skkbot2::Logger::get().flush();
        AccuracyMode captureAccuracyMode = AccuracyMode::Vanilla;
        beginReplayAccuracyEnvironment(captureAccuracyMode, false);
        skkbot2::log::info("[BC] after beginReplayAccuracyEnvironment");
        skkbot2::Logger::get().flush();

        if (skMode) {
            initializeSkMacro(level);
            skkbot2::log::info("[BC] after initializeSkMacro");
            skkbot2::Logger::get().flush();
            if (activeSk) {
                activeSk->accuracyMode = captureAccuracyMode;
            }
            if (activeSk) {
                if (auto* playLayer = PlayLayer::get(); playLayer && playLayer->m_startPosObject) {
                    activeSk->recordedFromStartPos = true;
                    activeSk->startPosX = playLayer->m_startPosObject->getPositionX();
                    activeSk->startPosY = playLayer->m_startPosObject->getPositionY();
                }
            }
        } else {
            initializeMacro(level);
            if (activeMacro) {
                activeMacro->accuracyMode = captureAccuracyMode;
            }
            if (activeMacro) {
                if (auto* playLayer = PlayLayer::get(); playLayer && playLayer->m_startPosObject) {
                    activeMacro->recordedFromStartPos = true;
                    activeMacro->startPosX = playLayer->m_startPosObject->getPositionX();
                    activeMacro->startPosY = playLayer->m_startPosObject->getPositionY();
                }
            }
        }

        return true;
    }

    void initializeMacro(GJGameLevel* level) {
        if (activeMacro) {
            delete activeMacro;
        }
        activeMacro = new MacroSequence();
        activeMacro->accuracyMode = runtimeAccuracyModeFor(selectedAccuracyMode);
        if (level) {
            activeMacro->levelInfo.id = level->m_levelID;
            activeMacro->levelInfo.name = level->m_levelName;
            activeMacro->name = level->m_levelName;
        }
        activeMacro->framerate = tickRate;
        activeMacro->displayFps =
            skkbot2::fps::captureForStorage(tickRate).toWire();
        auto const& gui = skkbot2::gui::get();
        activeMacro->lockDelta = gui.lockDelta && !gui.realTime;
        activeMacro->realTime = gui.realTime && !gui.lockDelta;
        if (auto* playLayer = PlayLayer::get(); playLayer && playLayer->m_levelSettings) {
            activeMacro->platformerMode = playLayer->m_levelSettings->m_platformerMode;
            activeMacro->hasPlatformerModeMetadata = true;
        }
        dataModified = false;
    }

    void initializeSkMacro(GJGameLevel* level) {
        ++macroGeneration;
        if (activeSk) {
            delete activeSk;
        }
        activeSk = new SkMacro();
        activeSk->accuracyMode = runtimeAccuracyModeFor(selectedAccuracyMode);
        activeSk->usesStepClock = true;
        if (level) {
            activeSk->levelId = level->m_levelID;
            activeSk->levelName = level->m_levelName;
            activeSk->name = level->m_levelName;
        }
        activeSk->framerate = tickRate;
        activeSk->displayFps = skkbot2::fps::captureForStorage(tickRate).toWire();
        auto const& gui = skkbot2::gui::get();
        activeSk->lockDelta = gui.lockDelta && !gui.realTime;
        activeSk->realTime = gui.realTime && !gui.lockDelta;
        if (auto* playLayer = PlayLayer::get(); playLayer && playLayer->m_levelSettings) {
            activeSk->platformerMode = playLayer->m_levelSettings->m_platformerMode;
            activeSk->twoPlayerMode = playLayer->m_levelSettings->m_twoPlayerMode;
        }
        activeSk->rngLocked = rngLocked;
        activeSk->rngSeed = rngSeedVal;
        activeSk->persistenceAttempts.clear();
        dataModified = false;
    }

    bool validateExecutionRequest() {
        bool hasSk = skMode && activeSk && (!activeSk->inputs.empty() || !activeSk->persistenceAttempts.empty() ||
                                             !persistenceCaptureAttempt.inputs.empty());
        bool hasGDR = !skMode && activeMacro && !activeMacro->inputs.empty();
        if (!hasSk && !hasGDR) {
            return false;
        }

        if (!validatePlaybackPlatformerMode()) {
            return false;
        }

        return true;
    }

    bool armExecutionState() {
        if (!validateExecutionRequest()) {
            pendingPlaybackStart = false;
            return false;
        }

        pendingPlaybackStart = false;
        if (skMode && activeSk && !activeSk->rngBoundaries.empty()) {
            size_t pre = activeSk->rngBoundaries.size();
            normalizeSkRngBoundaries(*activeSk);
            skkbot2::log::info("normalized rngBoundaries: {} -> {} (sorted by tick)",
                pre, activeSk->rngBoundaries.size());
        }
        engineMode = MODE_EXECUTE;
        skkbot2::debug::setFullPageHeap(true);
        refreshManualInputIgnoreState();
        stepClock = skMode && activeSk ? activeSk->usesStepClock : false;
        stepPairCount = 0;
        stepPairFrac = 0.0;
        executeIndex = 0;
        playbackAnchorIndex = 0;
        perFramePlaybackIndex = 0;
        playbackExhaustionNotified = false;
        resetPlayDriftMeter();
        processSliceSkips = 0;
        cbsImmediateCount = 0;
        cbsQueueNativeCount = 0;
        cbsWaitCount = 0;
        clearPersistenceRuntimeState();
        initialRun = true;
        suppressInitialRunReset = true;
        respawnTickIndex = -1;
        tickOffset = 0;
        startPosActive = false;
        clearStartPosWarning();
        anchorReconciliation = true;
        perFrameReconciliation = true;
        resetTimingTracking();
        resetSessionStepGridStat();
        substepProbe = 96;
        clearQueuedSubstepState();
        beginReplayAccuracyEnvironment(AccuracyMode::Vanilla, true);
        skkbot2::log::info(
            "PLAY timing profile: hintTps={} appliedTickRate={} clock={} accuracy={}",
            (skMode && activeSk) ? activeSk->framerate
                : (activeMacro ? activeMacro->framerate : kBaseTickRate),
            runtimeTickRate(),
            stepClock ? "stepGrid" : "gameTime",
            static_cast<int>(activeMacroAccuracyMode()));

        if (skMode && activeSk && activeSk->loadedFromSKK3()) {
            activeSk->materializeSKK3RuntimeTicks(runtimeTickRate());
        }

        if (!skMode && activeMacro) {
            anchorInterval = activeMacro->savedAnchorInterval > 0
                ? activeMacro->savedAnchorInterval
                : static_cast<int>(activeMacroFramerate());
        }

        if (skMode && activeSk) {
            size_t chainInputsTotal = 0;
            for (auto const& link : activeSk->persistenceAttempts) {
                chainInputsTotal += link.inputs.size();
            }
            skkbot2::log::info(
                "PLAY start: macro='{}' mainInputs={} chainInputs={} chainLinks={} mainFrames={} runtimeTps={} hintTps={}",
                activeSk->name, activeSk->inputs.size(), chainInputsTotal,
                activeSk->persistenceAttempts.size(), activeSk->perFrames.size(),
                runtimeTickRate(), activeSk->framerate);
            auto* list = activeSkInputs();
            if (list && !list->empty()) {
                skkbot2::log::info("PLAY stream: [0].tick={} [{}].tick={}",
                    (*list)[0].tick, list->size() - 1, list->back().tick);
            }
            if (list && !list->empty()) {
                auto inDump = skkbot2::paths::file("inputs_dump.txt");
                std::ofstream idump(inDump, std::ios::trunc);
                if (idump.is_open()) {
                    idump << "inputs dump: macro='" << activeSk->name << "' total=" << list->size() << "\n";
                    idump << "idx | tick | btn | pressed | p2 | offset | timeSec\n";
                    for (size_t i = 0; i < list->size(); ++i) {
                        auto const& in = (*list)[i];
                        bool const isPress = in.isPressed();
                        idump << i << " | " << in.tick << " | " << (int)in.actionType
                              << " | " << (isPress ? 1 : 0) << " | " << (in.isPlayer2() ? 1 : 0)
                              << " | " << std::fixed << std::setprecision(3) << in.stepOffset
                              << " | " << std::setprecision(6) << in.timeSeconds << "\n";
                    }
                    idump.close();
                    skkbot2::log::info("BUG-06 inputs dump written: {}", inDump);
                }
                if (!activeSk->perFrames.empty() && list->size() >= 80) {
                    size_t const limit = std::min<size_t>(list->size(), 130);
                    std::ostringstream oss;
                    oss << "BUG-06 input->pos:";
                    for (size_t i = 0; i < limit; ++i) {
                        auto const& in = (*list)[i];
                        float px = 0.f, py = 0.f;
                        for (auto const& f : activeSk->perFrames) {
                            if (f.tick > in.tick) break;
                            px = f.p1x; py = f.p1y;
                        }
                        oss << " #" << i << "=" << in.tick << ":("
                            << std::fixed << std::setprecision(1) << px << "," << py << ")";
                        if (i >= 94 && i <= 109) {
                            oss << "[f:";
                            for (auto const& f : activeSk->perFrames) {
                                if (std::abs((int)f.tick - (int)in.tick) <= 2) {
                                    oss << f.tick << "=(" << std::setprecision(1) << f.p1x << ","
                                        << f.p1y << ") ";
                                }
                            }
                            oss << "]";
                        }
                    }
                    skkbot2::log::info("{}", oss.str());
                }
            }
            if (auto* pl = PlayLayer::get()) {
                auto portalDump = skkbot2::paths::file("portal_objects.txt");
                std::ofstream pd(portalDump, std::ios::trunc);
                if (pd.is_open()) {
                    pd << "portal_objects dump: macro='" << activeSk->name << "'\n";
                    int pcount = 0;
                    if (pl->m_objects) {
                        for (int i = 0; i < pl->m_objects->count(); ++i) {
                            auto* obj = pl->m_objects->objectAtIndex(i);
                            auto* tp = dynamic_cast<TeleportPortalObject*>(obj);
                            if (!tp) continue;
                            GameObject* go = static_cast<GameObject*>(tp);
                            ++pcount;
                            pd << "portal#" << pcount
                               << " id=" << (go ? go->m_objectID : -1)
                               << " pos=(" << std::fixed << std::setprecision(1)
                               << go->getPositionX() << "," << go->getPositionY() << ")"
                               << " orange=" << (tp->m_orangePortal ? "yes" : "no");
                            if (tp->m_orangePortal) {
                                pd << " orangePos=(" << tp->m_orangePortal->getPositionX()
                                   << "," << tp->m_orangePortal->getPositionY() << ")";
                            }
                            pd << "\n";
                        }
                    }
                    pd << "total TeleportPortalObject=" << pcount << "\n";
                    pd.close();
                    skkbot2::log::info("BUG-06 portal dump written: {} ({} portals)",
                        portalDump, pcount);
                }
            }
            if (!activeSk->perFrames.empty()) {
                auto const& pfs = activeSk->perFrames;
                int64_t tickSpan = (int64_t)pfs.back().tick - (int64_t)pfs.front().tick;
                int64_t missingTicks = std::max<int64_t>(0, tickSpan - (int64_t)pfs.size());
                const double kPFJumpThreshold = 30.0;
                auto dumpPath = skkbot2::paths::file("perframe_jumps.txt");
                std::ofstream dump(dumpPath, std::ios::trunc);
                if (dump.is_open()) {
                    dump << "BUG-06 perframe_jumps dump: macro='" << activeSk->name << "'\n";
                    dump << "frames=" << pfs.size() << " tickRange=[" << pfs.front().tick << ".."
                         << pfs.back().tick << "] tickSpan=" << tickSpan
                         << " missingTicks=" << missingTicks
                         << " avgFramesPerTick=" << (tickSpan > 0 ? (double)pfs.size() / (double)tickSpan : 0.0)
                         << "\n";
                    dump << "idx hairline tick  dx       dy       dist     mode vy        prev=(x,y)              next=(x,y)\n";
                    for (size_t i = 1; i < pfs.size(); ++i) {
                        auto const& a = pfs[i - 1];
                        auto const& b = pfs[i];
                        if (!a.hasPlayer2 && b.hasPlayer2) continue;
                        double const dx = (double)b.p1x - (double)a.p1x;
                        double const dy = (double)b.p1y - (double)a.p1y;
                        double const dist = std::sqrt(dx * dx + dy * dy);
                        if (dist >= kPFJumpThreshold || (int64_t)b.tick - (int64_t)a.tick != 1) {
                            dump << "idx " << (i - 1) << "->" << i
                                 << " tick " << a.tick << "->" << b.tick
                                 << " dt=" << ((int64_t)b.tick - (int64_t)a.tick)
                                 << " dx=" << std::fixed << std::setprecision(1) << dx
                                 << " dy=" << dy
                                 << " dist=" << dist
                                 << " mode=" << (int)((a.p1vis >> 2) & 0x07)
                                 << " vy=" << std::setprecision(2) << a.p1vy
                                 << " prev=(" << a.p1x << "," << a.p1y << ")"
<< " next=(" << b.p1x << "," << b.p1y << ")"
                                 << "\n";
                        }
                    }
                    {
                        std::vector<int> present;
                        present.reserve(pfs.size());
                        for (auto const& f : pfs) present.push_back((int)f.tick);
                        std::sort(present.begin(), present.end());
                        present.erase(std::unique(present.begin(), present.end()), present.end());
                        dump << "capture holes (" << (tickSpan - (int64_t)present.size()) << " total):\n";
                        if (!present.empty()) {
                            for (size_t i = 1; i < present.size(); ++i) {
                                if ((int64_t)present[i] > (int64_t)present[i - 1] + 1) {
                                    int const lo = present[i - 1] + 1;
                                    int const hi = present[i] - 1;
                                    dump << "  missing ticks [" << lo << ".." << hi << "]"
                                         << " gap=" << (present[i] - present[i - 1] - 1) << "\n";
                                }
                            }
                        }
                        dump << "capture holes done\n";
                    }
                    dump.close();
                    skkbot2::log::info("BUG-06 perframe jump dump written: {} ({} records)",
                        dumpPath, pfs.size());
                } else {
                    skkbot2::log::info("BUG-06 perframe jump dump FAILED to open: {}", dumpPath);
                }
            }
        }

        return true;
    }

    bool beginExecutionImmediate() {
        if (!armExecutionState()) {
            engineMode = MODE_DISABLED;
            return false;
        }

        if (auto* playLayer = PlayLayer::get()) {
            if (auto* endLayer = playLayer->getChildByType<EndLevelLayer>(0)) {
                endLayer->removeFromParentAndCleanup(true);
            }
            if (playLayer->m_levelSettings && playLayer->m_levelSettings->m_platformerMode) {
                playLayer->resetLevelFromStart();
            } else {
                playLayer->resetLevel();
            }
        }
        return engineMode == MODE_EXECUTE;
    }

    bool requestExecutionStart() {
        if (!validateExecutionRequest()) {
            pendingPlaybackStart = false;
            skkbot2::log::info("PLAY start refused: macro has no playable data for the current level");
            return false;
        }

        auto* playLayer = PlayLayer::get();
        if (fastPlayback) {
            return beginExecutionImmediate();
        }
        if (!playLayer) {
            if (skk::gameplay::isEditorPlaytest()) {
                if (!armExecutionState()) {
                    engineMode = MODE_DISABLED;
                    return false;
                }
                applyVarianceOverrides();
                parkPlaybackPending();
                return true;
            }
            return beginExecutionImmediate();
        }

        bool runActive = !playLayer->m_isPracticeMode &&
            !playLayer->m_levelEndAnimationStarted &&
            playLayer->m_player1 &&
            !playLayer->m_player1->m_isDead;

        if (!runActive) {
            return beginExecutionImmediate();
        }

        clearStartPosWarning();
        pendingPlaybackStart = true;
        skkbot2::log::info("PLAY armed: takes over on the next attempt (no level restart)");
        return true;
    }

    bool armPendingPlaybackStart(PlayLayer* playLayer) {
        if (!pendingPlaybackStart || engineMode == MODE_EXECUTE || !playLayer) {
            return false;
        }

        if (auto* endLayer = playLayer->getChildByType<EndLevelLayer>(0)) {
            endLayer->removeFromParentAndCleanup(true);
        }

        return armExecutionState();
    }

    bool tryLivePlaybackTakeover(GJBaseGameLayer* playLayer);

    bool coversPlaybackPosition(float refX, float refY,
                                float xTolerance, float yTolerance);

    void parkPlaybackPending();

    void logTimingEpilogues() {
        if (sessionPlanCount > 0) {
            const double ltDelta = sessionLastLevelTime - sessionFirstLevelTime;
            const long long estTicks = static_cast<long long>(sessionStepSum);
            skkbot2::log::info(
                "step-grid epilogue: plans={} steps={} resumeZero={} | "
                "levelTime {:.4f} -> {:.4f} (+{:.4f}s = {} steps) | plan/step {:.2f}",
                sessionPlanCount, sessionStepSum, sessionResumeZeroPlans,
                sessionFirstLevelTime, sessionLastLevelTime, ltDelta, estTicks,
                estTicks > 0 ? static_cast<double>(sessionPlanCount) / static_cast<double>(estTicks) : 0.0);
        }
        if (lockPlanCount > 0) {
            skkbot2::log::info(
                "lock-delta epilogue: engaged at frame {}, {} frames planned, "
                "{} steps over {} wanted (avg {:.3f}/frame), worst frame wanted "
                "{}, {} frames ran nothing, backlog refused {}x ({:.2f}s), walks "
                "{} (vanilla exits {}), measured-dt minus "
                "interval summed {:.4f}s, worst single frame {:.4f}s",
                lockFirstPlanFrame, lockPlanCount, lockStepSum, lockWantedSum,
                static_cast<double>(lockWantedSum) / static_cast<double>(lockPlanCount),
                lockMaxWanted, lockZeroFrames,
                lockBacklogDrops, lockBacklogDropped,
                sessionSchedWalks, sessionVanillaExits,
                lockDroppedCarry, lockMaxDtSkew);
            lockLoggedOn = false;
        }
    }

    void haltExecution() {
        skkbot2::debug::profilerStopAndReport();
        if (engineMode == MODE_CAPTURE) {
            captureFlushStagingAll();
            captureVarianceSnapshot();
            flushPendingPersistenceCaptureAttempt();
            ::skkbot2::autosave::flushRecording();
        }
        if (engineMode == MODE_CAPTURE) {
            if (activeSk) {
                skkbot2::log::info("RECORD session ended: main inputs={} anchors={} frames={} | chain links={} | variance slots={}",
                    activeSk->inputs.size(), activeSk->anchors.size(), activeSk->perFrames.size(),
                    activeSk->persistenceAttempts.size(), activeSk->varianceOverrides.size());
            }
            auto& gui = ::skkbot2::gui::get();
            if (gui.macroName[0] == '\0') {
                const std::string autoName = ::skkbot2::autosave::skkAutosaveName();
                if (!autoName.empty()) {
                    std::snprintf(gui.macroName, sizeof(gui.macroName), "%s", autoName.c_str());
                    skkbot2::log::info("RECORD stop: empty macro name auto-filled from autosave ('{}')", autoName);
                }
            }
        } else if (engineMode == MODE_EXECUTE && activeSk) {
            skkbot2::log::info("PLAY session ended: consumed {}/{} inputs, maxPreReconcileDelta {:.2f}px@{}, maxRenderResidual {:.3f}px@{}, "
                               "slice-skips={}, cbs[imm={},queue={},wait={}]",
                executeIndex, activeSk->inputs.size(), playDriftMax, playDriftMaxTick,
                renderDriftMax, renderDriftMaxTick,
                processSliceSkips, cbsImmediateCount, cbsQueueNativeCount, cbsWaitCount);
        }
        logTimingEpilogues();
        skkbot2::dumpPlayerInputTally("run ended");
        resetPlayDriftMeter();
        resetSessionStepGridStat();
        pendingPlaybackStart = false;
        initialRun = false;
        levelRestarting = false;
        anchorReconciliation = false;
        perFrameReconciliation = false;
        macroInputActive = false;
        respawnTickIndex = -1;
        tickAccumulator = 0.0f;
        ::skkbot2::fps::release();
        resetTimingTracking();
        resetDeferredInputState();
        if (auto* playLayer = PlayLayer::get()) {
            if (playLayer->m_player1) {
                playLayer->m_player1->releaseAllButtons();
            }
            if (playLayer->m_player2) {
                playLayer->m_player2->releaseAllButtons();
            }
        }
        Autoclicker::get()->reset();
        endReplayAccuracyEnvironment();
        skkbot2::debug::setFullPageHeap(false);
        executeIndex = 0;
        playbackAnchorIndex = 0;
        perFramePlaybackIndex = 0;
        playbackExhaustionNotified = false;
        tickOffset = 0;
        startPosActive = false;
        engineMode = MODE_DISABLED;
        flushPendingPersistenceCaptureAttempt();
        clearPersistenceRuntimeState();
        for (bool& state : activeButtons) {
            state = false;
        }
        for (bool& state : priorButtonState) {
            state = false;
        }
        resetRecordedHoldShadow();
        refreshManualInputIgnoreState();
    }

    void resetPlayDriftMeter() {
        playDriftMax = 0.0f;
        playDriftMaxTick = -1;
        playDriftLastLogTick = -100000;
        renderDriftMax = 0.0f;
        renderDriftMaxTick = -1;
        renderDriftLastLogTick = -100000;
        playDriftLastSampleTick = -100000;
        seamGlideUntilTick = -1;
        rngStreamMismatchReports = 0;
        gravProbeLast = 0;
        perFrameReconciliationCatchUps = 0;
        gravDiffReports = 0;
        dashReattachLast[0] = dashReattachLast[1] = -999;
    }

    void resetSessionStepGridStat() {
        sessionPlanCount = 0;
        sessionStepSum = 0;
        sessionResumeZeroPlans = 0;
        sessionFirstLevelTime = 0.0;
        sessionLastLevelTime = 0.0;
        sessionLevelTimeInit = false;
        sessionSampleLastLevelTime = 0.0;
        sessionSampleStepsWindow = 0;
        sessionSampleWallStart = 0.0;
        sessionSampleDtSum = 0.0;
        sampleSpdLast = 0.0;
        emaFrameTime = 0.0;
        lockBacklogDrops = 0;
        lockBacklogDropped = 0.0;
        sessionSchedWalks = 0;
        sessionVanillaExits = 0;
        sampleRawDtMin = 0.0f;
        sampleRawDtMax = 0.0f;
        sampleRawDtSum = 0.0;
        sampleRawFrames = 0;
        sampleWallGapSum = 0.0;
        sampleWallGapMin = -1.0f;
        sampleWallGapMax = 0.0f;
        samplePhantomCount = 0;
        samplePhantomDtSum = 0.0;
        sampleDebtSum = 0.0;
        sampleDebtCount = 0;
        sampleWantSum = 0.0;
        sampleFreezeDropped = 0.0;
        lastDriveWall = 0.0;
        sampleVanillaExits = 0;
        lockPlanCount = 0;
        lockWantedSum = 0;
        lockStepSum = 0;
        lockMaxWanted = 0;
        batFrozenFlagPlans = 0;
        batFrozenFlagSteps = 0;
        batFreezeCtrPlans = 0;
        batFreezeCtrSteps = 0;
        startFrozenFrames = 0;
        startFrozenReported = false;
        startFrozenWall = 0.0;
        startLiveReported = false;
        playbackFirstInputReported = false;
        lockOffCadenceFraction = 1.0;
        lockOffCadenceInWindow = 0;
        lockWindowFrames = 0;
        lockDeliveredTotal = 0.0;
        lockDeliveredFrames = 0;
        lockWarnedSlow = false;
        lockZeroFrames = 0;
        lockDroppedCarry = 0.0;
        lockMaxDtSkew = 0.0;
        lockEverOn = false;
        lockLoggedOn = false;
        lockFirstPlanFrame = -1;
    }

    bool hasMacro() const {
        return skMode ? (activeSk != nullptr) : (activeMacro != nullptr);
    }

    bool hasMacroInputs() const {
        if (skMode) {
            return activeSk && (!activeSk->inputs.empty() || !activeSk->persistenceAttempts.empty() ||
                                !persistenceCaptureAttempt.inputs.empty() ||
                                (captureStaging && !captureStaging->inputs.empty()));
        }
        return activeMacro && !activeMacro->inputs.empty();
    }

    void setPersistenceMode(bool enabled) {
        persistenceMode = enabled;
        if (auto* mod = Mod::get()) {
            mod->setSavedValue("eng_persistence_mode", persistenceMode);
        }
    }

    void disablePersistenceModeSafeguard() {
        if (!persistenceMode) {
            return;
        }
        setPersistenceMode(false);
    }

    bool canUsePersistenceCapture() const {
        return persistenceMode && engineMode == MODE_CAPTURE && skMode && activeSk;
    }

    bool hasUnsavedActiveMacro() const {
        bool hasInputs = (activeSk && (!activeSk->inputs.empty() || !activeSk->persistenceAttempts.empty() ||
                                       !persistenceCaptureAttempt.inputs.empty() || !persistenceCaptureAttempt.anchors.empty() ||
                                       (captureStaging && !captureStaging->inputs.empty())))
            || (activeMacro && !activeMacro->inputs.empty());
        return dataModified && hasInputs;
    }

    void markActiveMacroDirty() {
        ++macroRevision;
        dataModified = true;
    }

    void clearActiveMacroDirty() {
        dataModified = false;
    }

    bool saveActiveMacro();
    void discardActiveMacro();
    void runWithUnsavedMacroGuard(std::function<void()> continueAction, std::string message = {});

    bool hasAnchorData() const {
        auto const* anchors = activeAnchors();
        return anchors && !anchors->empty();
    }

    bool manualInputIgnoredActive() const {
        if (playbackExhaustionNotified) return false;
        return engineMode == MODE_EXECUTE;
    }

    void refreshManualInputIgnoreState() {
        inputIgnored = manualInputIgnoredActive();
    }

    std::string getMacroName() const {
        if (skMode && activeSk) return activeSk->name;
        if (!skMode && activeMacro) return activeMacro->name;
        return "";
    }

    AccuracyMode activeMacroAccuracyMode() const {
        if (skMode) {
            return activeSk ? activeSk->accuracyMode : AccuracyMode::Vanilla;
        }
        return activeMacro ? activeMacro->accuracyMode : AccuracyMode::Vanilla;
    }

    bool activeMacroPlatformerMode() const {
        if (skMode) {
            return activeSk ? activeSk->platformerMode : false;
        }
        return activeMacro ? activeMacro->platformerMode : false;
    }

    SkAttemptSegment* activePersistencePlaybackAttempt() {
        if (engineMode != MODE_EXECUTE || !skMode || !activeSk) {
            return nullptr;
        }
        if (!activeSk->inputs.empty()) {
            return nullptr;
        }
        if (persistencePlaybackAttemptIndex >= activeSk->persistenceAttempts.size()) {
            return nullptr;
        }
        return &activeSk->persistenceAttempts[persistencePlaybackAttemptIndex];
    }

    SkAttemptSegment const* activePersistencePlaybackAttempt() const {
        if (engineMode != MODE_EXECUTE || !skMode || !activeSk) {
            return nullptr;
        }
        if (!activeSk->inputs.empty()) {
            return nullptr;
        }
        if (persistencePlaybackAttemptIndex >= activeSk->persistenceAttempts.size()) {
            return nullptr;
        }
        return &activeSk->persistenceAttempts[persistencePlaybackAttemptIndex];
    }

    bool isPersistencePlaybackActive() const {
        return engineMode == MODE_EXECUTE && skMode && activeSk && !activeSk->persistenceAttempts.empty();
    }

    bool isPersistencePlaybackFailureAttempt() const {
        return activePersistencePlaybackAttempt() != nullptr;
    }

    std::vector<SkInput>* activeSkInputs() {
        if (!activeSk) {
            return nullptr;
        }
        if (auto* attempt = activePersistencePlaybackAttempt()) {
            return &attempt->inputs;
        }
        return &activeSk->inputs;
    }

    std::vector<SkInput> const* activeSkInputs() const {
        if (!activeSk) {
            return nullptr;
        }
        if (auto const* attempt = activePersistencePlaybackAttempt()) {
            return &attempt->inputs;
        }
        return &activeSk->inputs;
    }

    bool currentLevelPlatformerMode() const {
        auto* playLayer = PlayLayer::get();
        return playLayer && playLayer->m_levelSettings && playLayer->m_levelSettings->m_platformerMode;
    }

    bool validatePlaybackPlatformerMode() {
        auto* playLayer = PlayLayer::get();
        if (!playLayer || !playLayer->m_levelSettings) {
            return true;
        }

        bool macroPlatformer = activeMacroPlatformerMode();
        bool levelPlatformer = currentLevelPlatformerMode();
        if (macroPlatformer == levelPlatformer) {
            return true;
        }

        setStartPosWarningKey(macroPlatformer
            ? "This macro was recorded in platformer mode. Open a platformer level to play it."
            : "This macro was recorded in classic mode. Open a classic level to play it.");
        return false;
    }

    double activeMacroFramerate() const {
        double framerate = kBaseTickRate;
        if (skMode && activeSk) {
            framerate = activeSk->framerate;
        } else if (!skMode && activeMacro) {
            framerate = activeMacro->framerate;
        }

        if (!std::isfinite(framerate) || framerate <= 0.0) {
            return kBaseTickRate;
        }
        return std::max(1.0, framerate);
    }

    skkbot2::fps::MacroDisplayRate activeMacroDisplayRate() const {
        double fps = 0.0;
        if (skMode && activeSk) {
            fps = activeSk->displayFps;
        } else if (!skMode && activeMacro) {
            fps = activeMacro->displayFps;
        }
        return skkbot2::fps::MacroDisplayRate::fromWire(fps);
    }

    double activePlaybackTickRate() const {
        double macroTps = activeMacroFramerate();
        double requiredTps = macroTps;
        if (skMode && activeSk && activeSk->loadedFromSKK3()) {
            requiredTps = activeSk->maxSourceTps();
        }
        return skk::replay_timing::playbackRuntimeTps(macroTps, requiredTps);
    }

    static AccuracyMode runtimeAccuracyModeFor(AccuracyMode mode) {
        return mode == AccuracyMode::CBS ? AccuracyMode::CBS : AccuracyMode::Vanilla;
    }

bool syncAccuracyFromGame() {
    if (replayAccuracyEnvironmentActive || externalAccuracyEnvironmentActive) {
        return false;
    }

    bool cbs = false;
    bool cos = false;
    const char* source = "none";

    if (auto* layer = skk::gameplay::activeLayer()) {
        cbs = layer->m_clickBetweenSteps;
        cos = layer->m_clickOnSteps;
        source = "layer";
    } else if (auto* gameManager = GameManager::get()) {
        cbs = gameManager->getGameVariable("0177");
        cos = gameManager->getGameVariable("0176");
        source = "gamevar";
    } else {
        return false;
    }

    bool changed = false;
    AccuracyMode fromGame = cbs ? AccuracyMode::CBS : AccuracyMode::Vanilla;

    if (selectedAccuracyMode != fromGame) {
        selectedAccuracyMode = fromGame;
        Mod::get()->setSavedValue("gui_accuracy", static_cast<int>(fromGame));
        changed = true;
        skkbot2::Logger::get().info(
            "skkBot2: accuracy picked up from GD [{}] -> {}", source,
            fromGame == AccuracyMode::CBS ? "CBS" : "Vanilla");
    }

    if (cosEnabled != cos) {
        cosEnabled = cos;
        Mod::get()->setSavedValue("gui_cos", cos);
        changed = true;
        skkbot2::Logger::get().info("skkBot2: COS picked up from GD [{}] -> {}",
                                    source, cos ? "on" : "off");
    }

    return changed;
}

    static void applyRuntimeAccuracyMode(AccuracyMode mode, bool cosEnabled) {
        mode = runtimeAccuracyModeFor(mode);
        bool enabled = mode == AccuracyMode::CBS;
        bool cosOn = cosEnabled &&
            (mode == AccuracyMode::Vanilla || mode == AccuracyMode::CBS);
        if (auto* gameManager = GameManager::get()) {
            gameManager->setGameVariable("0177", enabled);
            gameManager->setGameVariable("0176", cosOn);
        }
        if (auto* playLayer = PlayLayer::get()) {
            playLayer->m_clickBetweenSteps = enabled;
            playLayer->m_clickOnSteps = cosOn;
        }
    }

    void enableCBSForLoadedMacroIfNeeded() {
        selectedAccuracyMode = AccuracyMode::Vanilla;
    }

    void beginReplayAccuracyEnvironment(AccuracyMode mode, bool overridePlaybackTickRate) {
        if (!replayAccuracyEnvironmentActive) {
            if (auto* gameManager = GameManager::get()) {
                savedClickBetweenSteps = gameManager->getGameVariable("0177");
                savedClickOnSteps = gameManager->getGameVariable("0176");
            }
            if (auto* playLayer = PlayLayer::get()) {
                savedLayerClickBetweenSteps = playLayer->m_clickBetweenSteps;
                hasSavedLayerClickBetweenSteps = true;
                savedLayerClickOnSteps = playLayer->m_clickOnSteps;
                hasSavedLayerClickOnSteps = true;
            } else {
                hasSavedLayerClickBetweenSteps = false;
                hasSavedLayerClickOnSteps = false;
            }
            replayAccuracyEnvironmentActive = true;
        }

        if (overridePlaybackTickRate) {
            double declaredTps = activePlaybackTickRate();
            if (!playbackTickRateOverrideActive) {
                savedPlaybackTickRate = tickRate;
                tickRate = declaredTps;
                playbackTickRateOverrideActive = true;
            } else if (tickRate != declaredTps) {
                tickRate = declaredTps;
            }

            const auto declaredFps = activeMacroDisplayRate();
            if (!playbackFpsOverrideActive) {
                if (skkbot2::fps::beginMacro(declaredFps)) {
                    playbackFpsOverrideActive = true;
                }
            } else {
                skkbot2::fps::updateMacro(declaredFps);
            }

            auto& gui = skkbot2::gui::get();
            const bool macroLockDelta =
                (skMode && activeSk) ? activeSk->lockDelta
                                     : (activeMacro ? activeMacro->lockDelta : true);
            const bool macroRealTime =
                (skMode && activeSk) ? activeSk->realTime
                                     : (activeMacro ? activeMacro->realTime : false);
            const bool newLockDelta = macroLockDelta && !macroRealTime;
            const bool newRealTime = macroRealTime && !macroLockDelta;
            if (gui.lockDelta != newLockDelta || gui.realTime != newRealTime) {
                gui.lockDelta = newLockDelta;
                gui.realTime = newRealTime;
                Mod::get()->setSavedValue("gui_timing_lock_delta", newLockDelta);
                Mod::get()->setSavedValue("gui_timing_real_time", newRealTime);
                skkbot2::log::info(
                    "[SKK] playback timing mode restored from macro: {}",
                    newRealTime ? "Real Time" : "Lock Delta");
            }
            skkbot2::log::info(
                "[SKK] playback timing mode: {} (from the macro, menu follows)",
                gui.realTime && !gui.lockDelta ? "Real Time" : "Lock Delta");
            skkbot2::debug::profilerStart("skk-play");
            dropCarry();
        }

        applyRuntimeAccuracyMode(AccuracyMode::Vanilla, false);
        if (mode != AccuracyMode::Vanilla || cosEnabled) {
            skkbot2::log::info(
                "[SKK] accuracy frozen to vanilla for the session (requested mode={}, cos={})",
                mode == AccuracyMode::CBS ? "CBS" : "Vanilla", cosEnabled ? "on" : "off");
        }
    }

    void endReplayAccuracyEnvironment() {
        if (playbackFpsOverrideActive) {
            skkbot2::fps::endMacro();
            playbackFpsOverrideActive = false;
        }
        if (playbackTickRateOverrideActive) {
            tickRate = savedPlaybackTickRate;
            playbackTickRateOverrideActive = false;
        }

        if (!replayAccuracyEnvironmentActive) {
            return;
        }

        if (auto* gameManager = GameManager::get()) {
            gameManager->setGameVariable("0177", savedClickBetweenSteps);
            gameManager->setGameVariable("0176", savedClickOnSteps);
        }
        if (hasSavedLayerClickBetweenSteps) {
            if (auto* playLayer = PlayLayer::get()) {
                playLayer->m_clickBetweenSteps = savedLayerClickBetweenSteps;
            }
        }
        if (hasSavedLayerClickOnSteps) {
            if (auto* playLayer = PlayLayer::get()) {
                playLayer->m_clickOnSteps = savedLayerClickOnSteps;
            }
        }
        hasSavedLayerClickBetweenSteps = false;
        hasSavedLayerClickOnSteps = false;
        replayAccuracyEnvironmentActive = false;
    }

    void dropPlaybackTickRateOverride() {
        if (playbackTickRateOverrideActive) {
            tickRate = savedPlaybackTickRate;
            playbackTickRateOverrideActive = false;
        }
    }

    double runtimeTickRate() const {
        return std::max(1.0, tickRate);
    }

    double effectiveTimeScale() const {
        return gameSpeed * (kBaseTickRate / runtimeTickRate());
    }

    double fixedSimulationDelta() const {
        return 1.0 / runtimeTickRate();
    }

    double fixedPhysicsDelta(double timeWarp = 1.0) const {
        if (!std::isfinite(timeWarp)) {
            return 0.0;
        }
        const double warp = std::clamp(timeWarp, 0.0, 1.0);
        const float physicsStep = static_cast<float>(fixedSimulationDelta() * warp);
        return static_cast<double>(physicsStep);
    }

    static double wallClockSeconds() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void resetTimingTracking() {
        tickStartStep = 0;
        lastStepDelta = -1;
        tickStartTimestamp = 0.0;
        lastProcessSliceTimestamp = -1.0;
        macroStartTimestamp = -1.0;
        respawnTickIndex = -1;
        queuedMacroCommands.clear();
        queuedCaptureCommands.clear();
        cbsCaptureProcessingQueue = false;
        cbsPlaybackProcessingQueue = false;
        tickStartWallTime = 0.0;
        prevTickStartWallTime = 0.0;
        prevTickStartTimestamp = 0.0;
        inputArrivalWallTime = -1.0;
        hasInputArrivalWallTime = false;
    }

    void clearPersistenceRuntimeState() {
        persistenceCaptureAttempt = {};
        persistenceCaptureDeathRecorded = false;
        persistencePlaybackAttemptIndex = 0;
        persistencePlaybackDeathPending = false;
        persistenceRenderBaseTick = 0;
    }

    void resetPersistenceCaptureAttempt() {
        persistenceCaptureAttempt = {};
        persistenceCaptureDeathRecorded = false;
    }

    void resetRecordedHoldShadow() {
        for (bool& state : recordedHoldShadow) {
            state = false;
        }
        holdProbeLast[0] = holdProbeLast[1] = -2;
        gravProbeLast = 0;
    }

    static void mergePersistenceCaptureAttempt(SkMacro& snapshot, SkAttemptSegment attempt) {
        if (!attempt.hasData()) return;
        if (attempt.deathTick <= 0 && attempt.inputs.size() >= snapshot.inputs.size()) {
            snapshot.inputs = std::move(attempt.inputs);
            snapshot.anchors = std::move(attempt.anchors);
            snapshot.perFrames = std::move(attempt.perFrames);
        } else {
            snapshot.persistenceAttempts.push_back(std::move(attempt));
        }
    }

    void appendPendingPersistenceTo(SkMacro& snapshot) const {
        if (canUsePersistenceCapture()) {
            mergePersistenceCaptureAttempt(snapshot, persistenceCaptureAttempt);
        }
    }

    void flushPendingPersistenceCaptureAttempt() {
        if (!activeSk || !persistenceCaptureAttempt.hasData()) {
            persistenceCaptureAttempt = {};
            persistenceCaptureDeathRecorded = false;
            return;
        }
        const bool promote = persistenceCaptureAttempt.deathTick <= 0 &&
            persistenceCaptureAttempt.inputs.size() >= activeSk->inputs.size();
        const size_t inCount = persistenceCaptureAttempt.inputs.size();
        const size_t pfCount = persistenceCaptureAttempt.perFrames.size();
        mergePersistenceCaptureAttempt(*activeSk, std::move(persistenceCaptureAttempt));
        if (promote) {
            skkbot2::log::info("RECORD flush: survivor promoted to MAIN (inputs={} frames={})",
                inCount, pfCount);
        } else {
            skkbot2::log::info("RECORD flush: segment appended to chain (#{} links)",
                activeSk->persistenceAttempts.size());
        }
        persistenceCaptureAttempt = {};
        persistenceCaptureDeathRecorded = false;
    }

    bool activeSkMatchesCurrentLevel() const {
        if (!activeSk) {
            return false;
        }
        auto* playLayer = PlayLayer::get();
        if (!playLayer || !playLayer->m_level) {
            return false;
        }
        auto* level = playLayer->m_level;
        if (activeSk->levelId > 0) {
            return activeSk->levelId == static_cast<int32_t>(level->m_levelID);
        }
        return activeSk->levelName.empty() || activeSk->levelName == level->m_levelName.c_str();
    }

    static constexpr uint64_t forcedAttemptSeed() {
        uint64_t z = static_cast<uint64_t>(rngSeedVal) + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    uint64_t macroSetupSeed() const {
        if (activeSk && activeSk->setupRngSeed != 0) {
            return activeSk->setupRngSeed;
        }
        return forcedAttemptSeed();
    }

    void pinSetupSeedBeforeReset() {
        if (!activeSk) {
            return;
        }
        if (engineMode == MODE_CAPTURE) {
            if (activeSkMatchesCurrentLevel()) {
                if (activeSk->setupRngSeed == 0) {
                    activeSk->setupRngSeed = forcedAttemptSeed();
                }
                if (auto* playLayer = PlayLayer::get()) {
                    if (auto* em = playLayer->m_effectManager) {
                        activeSk->persistentItems.assign(
                            em->m_persistentItemCountMap.begin(), em->m_persistentItemCountMap.end());
                        std::sort(activeSk->persistentItems.begin(), activeSk->persistentItems.end());
                    }
                }
                GameToolbox::fast_srand(activeSk->setupRngSeed);
            }
            return;
        }
        if ((engineMode == MODE_EXECUTE || pendingPlaybackStart) &&
            activeSk->setupRngSeed != 0 && activeSkMatchesCurrentLevel()) {
            GameToolbox::fast_srand(activeSk->setupRngSeed);
        }
    }

    void forceLayerSeeds() {
        if (!activeSk || !activeSkMatchesCurrentLevel()) {
            return;
        }
        auto* playLayer = PlayLayer::get();
        if (!playLayer) {
            return;
        }
        const uint64_t seed = macroSetupSeed();
        playLayer->m_randomSeed = seed;
        playLayer->m_replayRandSeed = seed;
        GameToolbox::fast_srand(seed);
    }

    void captureVarianceSnapshot() {
        if (activeSk) captureVarianceSnapshot(*activeSk);
    }

    void captureVarianceSnapshot(SkMacro& snapshot) const {
        if (!activeSk || !activeSkMatchesCurrentLevel()) {
            return;
        }
        auto* playLayer = PlayLayer::get();
        if (!playLayer) {
            return;
        }
        snapshot.varianceOverrides.clear();
        size_t index = 0;
        for (const float& value : playLayer->m_varianceValues) {
            if (value != 0.0f && std::isfinite(value) && index < playLayer->m_varianceValues.size()) {
                SkVarianceEntry entry;
                entry.index = static_cast<uint16_t>(index);
                entry.value = value;
                snapshot.varianceOverrides.push_back(entry);
            }
            ++index;
        }
    }

    void applyVarianceOverrides() {
        if (!activeSk || activeSk->varianceOverrides.empty()) {
            return;
        }
        if (!activeSkMatchesCurrentLevel()) {
            return;
        }
        auto* playLayer = PlayLayer::get();
        if (!playLayer) {
            return;
        }
        std::fill(playLayer->m_varianceValues.begin(), playLayer->m_varianceValues.end(), 0.0f);
        for (auto const& entry : activeSk->varianceOverrides) {
            if (entry.index < playLayer->m_varianceValues.size()) {
                playLayer->m_varianceValues[entry.index] = entry.value;
            }
        }
        if (auto* em = playLayer->m_effectManager) {
            em->m_persistentItemCountMap.clear();
            for (auto const& item : activeSk->persistentItems) {
                em->m_persistentItemCountMap[item.first] = item.second;
            }
        }
        static int applyLogCounter = 0;
        if (++applyLogCounter % 4 == 1) {
            skkbot2::log::info("world-state applied: variance={} items={} setupSeed=0x{:X}",
                activeSk->varianceOverrides.size(), activeSk->persistentItems.size(),
                activeSk->setupRngSeed);
        }
    }

    void recordSkAction(int tick, int button, bool player2, bool pressed, float offset, double cbsTimeOffset) {
        if (!activeSk) {
            return;
        }

        if (captureDeathPending) {
            skkbot2::log::debug(
                "capture drop [death-window]: tick={} button={} p2={} down={}",
                tick, button, player2, pressed);
            return;
        }

        if (auto* layer = PlayLayer::get()) {
            if ((layer->m_player1 && layer->m_player1->m_isDead) ||
                (layer->m_player2 && layer->m_player2->m_isDead)) {
                skkbot2::log::debug(
                    "capture drop [dead-player]: tick={} button={} p2={} down={} "
                    "(death chains are not recorded)",
                    tick, button, player2, pressed);
                return;
            }
        }

        if (canUsePersistenceCapture()) {
            activeSk->recordAction(persistenceCaptureAttempt.inputs, tick, button, player2, pressed, offset, cbsTimeOffset);
        } else if (captureStaging) {
            captureStaging->recordAction(tick, button, player2, pressed, offset, cbsTimeOffset);
        } else {
            activeSk->recordAction(tick, button, player2, pressed, offset, cbsTimeOffset);
        }
    }

    static uint64_t gravStateKey(PlayerObject* p, uint64_t shift) {
        if (!p) {
            return 1ULL << shift;
        }
        uint64_t g = (p->m_gravity < 0.0) ? 1ULL : 0ULL;
        uint64_t ud = p->m_isUpsideDown ? 2ULL : 0ULL;
        uint64_t sp = p->m_isSpider ? 4ULL : 0ULL;
        uint64_t sx = p->getScaleX() < 0.0 ? 8ULL : 0ULL;
        uint64_t sy = p->getScaleY() < 0.0 ? 16ULL : 0ULL;
        uint64_t dr = p->m_dashRing ? 32ULL : 0ULL;
        uint64_t dash = p->m_isDashing ? 64ULL : 0ULL;
        uint64_t spr = 0;
        if (auto* spider = p->m_spiderSprite) {
            const double srot = std::fmod(std::fabs(static_cast<double>(spider->getRotation())), 360.0);
            spr = (static_cast<uint64_t>(static_cast<int>(srot / 15.0)) & 0xFULL) << 8;
            if (spider->getScaleX() < 0.0) spr |= (1ULL << 12);
            if (spider->getScaleY() < 0.0) spr |= (1ULL << 13);
        }
        double rot = std::fmod(std::fabs(static_cast<double>(p->getRotation())), 360.0);
        uint64_t rotb = static_cast<uint64_t>(static_cast<int>(rot / 5.0)) & 0x7FULL;
        return (g | ud | sp | sx | sy | dr | dash | spr | (rotb << 14)) << shift;
    }

    void gravStateProbe(const char* mode, int tick, PlayerObject* p1, PlayerObject* p2) {
        uint64_t camKey = 0;
        double cam = 0.0;
        if (auto* pl = PlayLayer::get()) {
            cam = static_cast<double>(pl->m_gameState.m_levelFlipping);
            const uint64_t camBucket = static_cast<uint64_t>(static_cast<int>(cam + 90.0) / 5) & 0x3FULL;
            camKey = camBucket << 48;
        }
        const uint64_t key = gravStateKey(p1, 0) | gravStateKey(p2, 24) | camKey;
        const bool periodic = (tick % 480) == 0;
        if (key == gravProbeLast && !periodic) {
            return;
        }
        gravProbeLast = key;
        skkbot2::log::debug(
            "grav probe[{}]: tick={} cam={:.1f} p1(g={} ud={} sp={} sx={} sy={} dr={} dash={} spr(rot={:.0f} sx={} sy={}) rot={:.1f}) "
            "p2(g={} ud={} sp={} sx={} sy={} dr={} dash={} spr(rot={:.0f} sx={} sy={}) rot={:.1f})",
            mode, tick, cam,
            p1 ? (p1->m_gravity < 0.0 ? "-" : "+") : "x",
            p1 && p1->m_isUpsideDown ? 1 : 0,
            p1 && p1->m_isSpider ? 1 : 0,
            p1 && p1->getScaleX() < 0.0 ? -1 : 1,
            p1 && p1->getScaleY() < 0.0 ? -1 : 1,
            p1 && p1->m_dashRing ? 1 : 0,
            p1 && p1->m_isDashing ? 1 : 0,
            p1 && p1->m_spiderSprite ? static_cast<double>(p1->m_spiderSprite->getRotation()) : -999.0,
            p1 && p1->m_spiderSprite && p1->m_spiderSprite->getScaleX() < 0.0 ? -1 : 1,
            p1 && p1->m_spiderSprite && p1->m_spiderSprite->getScaleY() < 0.0 ? -1 : 1,
            p1 ? static_cast<double>(p1->getRotation()) : 0.0,
            p2 ? (p2->m_gravity < 0.0 ? "-" : "+") : "x",
            p2 && p2->m_isUpsideDown ? 1 : 0,
            p2 && p2->m_isSpider ? 1 : 0,
            p2 && p2->getScaleX() < 0.0 ? -1 : 1,
            p2 && p2->getScaleY() < 0.0 ? -1 : 1,
            p2 && p2->m_dashRing ? 1 : 0,
            p2 && p2->m_isDashing ? 1 : 0,
            p2 && p2->m_spiderSprite ? static_cast<double>(p2->m_spiderSprite->getRotation()) : -999.0,
            p2 && p2->m_spiderSprite && p2->m_spiderSprite->getScaleX() < 0.0 ? -1 : 1,
            p2 && p2->m_spiderSprite && p2->m_spiderSprite->getScaleY() < 0.0 ? -1 : 1,
            p2 ? static_cast<double>(p2->getRotation()) : 0.0);
        {
            auto ringWitness = [&](PlayerObject* p, const char* tag) {
                if (!p) return;
                auto* ring = p->m_dashRing;
                if (!ring) return;
                const auto* touched = p->m_touchingRings;
                skkbot2::log::debug(
                    "ring witness[{}]: tick={} {} ring id={} at({:.2f},{:.2f}) player=({:.2f},{:.2f}) touchingRings={}",
                    mode, tick, tag,
                    static_cast<GameObject*>(ring)->m_objectID,
                    ring->getPositionX(), ring->getPositionY(),
                    p->getPositionX(), p->getPositionY(),
                    touched ? static_cast<int>(touched->count()) : -1);
            };
            ringWitness(p1, "p1");
            ringWitness(p2, "p2");
        }
    }

    void gravDiffProbe(const char* stage, int tick, PlayerObject* p1, PlayerObject* p2) {
        if (gravDiffReports >= kGravDiffReportCap) {
            return;
        }
        auto* frames = activePerFrames();
        if (!frames || frames->empty() || !p1) {
            return;
        }
        auto it = std::upper_bound(
            frames->begin(), frames->end(), tick,
            [](int t, const typename std::remove_reference_t<decltype(
                   frames->front())>& pf) { return t < pf.tick; });
        if (it == frames->begin()) {
            return;
        }
        --it;
        auto const& pf = *it;
        auto probePlayer = [&](PlayerObject* p, uint8_t vis, const char* tag) {
            if (!p) {
                return;
            }
            const bool recG = (vis & kVisualGravityNegative) != 0;
            const bool recUD = (vis & kVisualUpsideDown) != 0;
            const bool liveG = p->m_gravity < 0.0;
            const bool liveUD = p->m_isUpsideDown;
            if (liveG == recG && liveUD == recUD) {
                return;
            }
            if (++gravDiffReports > kGravDiffReportCap) {
                return;
            }
            skkbot2::log::info(
                "grav diff[{}]: tick={} pfTick={} {} live(g={} ud={}) vs recorded(g={} ud={}) p1vis=0x{:02x}",
                stage, tick, pf.tick, tag, liveG ? 1 : 0, liveUD ? 1 : 0, recG ? 1 : 0, recUD ? 1 : 0,
                static_cast<int>(vis));
        };
        probePlayer(p1, pf.p1vis, "p1");
        if (p2 && pf.hasPlayer2) {
            probePlayer(p2, pf.p2vis, "p2");
        }
    }

    void recordSkAnchor(int tick, PlayerObject* player1, PlayerObject* player2, bool isPlatformer, bool isDual) {
        if (!activeSk) {
            return;
        }

        if (captureDeathPending || (player1 && player1->m_isDead) ||
            (player2 && player2->m_isDead)) {
            return;
        }

        gravStateProbe("rec", tick, player1, player2);

        {
            auto heldBit = [](PlayerObject* p, int b) -> int {
                if (!p) return -1;
                const auto& hb = p->m_holdingButtons;
                auto it = hb.find(b);
                return it == hb.end() ? 0 : (it->second ? 1 : 0);
            };
            const int hp1 = heldBit(player1, 1);
            const int hp2 = heldBit(player2, 1);
            bool periodic = (tick % 30) == 0;
            bool needDump = (hp1 != holdProbeLast[0] || hp2 != holdProbeLast[1]) || periodic;
            if (needDump) {
                if (!periodic) {
                    holdProbeLast[0] = hp1;
                    holdProbeLast[1] = hp2;
                }
                skkbot2::log::debug(
                    "hold probe: tick={} p1={}={}{}{} p2={}={}{}{} yv1={:.0f} yv2={:.0f} jb1={} jb2={} rj1={} rj2={}",
                    tick,
                    hp1,
                    heldBit(player1, 2), heldBit(player1, 3),
                    (player1 && player1->m_holdingRight) ? "R" : "",
                    hp2,
                    heldBit(player2, 2), heldBit(player2, 3),
                    (player2 && player2->m_holdingRight) ? "R" : "",
                    player1 ? player1->m_yVelocity : -999.0,
                    player2 ? player2->m_yVelocity : -999.0,
                    player1 && player1->m_jumpBuffered ? 1 : 0,
                    player2 && player2->m_jumpBuffered ? 1 : 0,
                    player1 && player1->m_wasRobotJump ? 1 : 0,
                    player2 && player2->m_wasRobotJump ? 1 : 0
                );
            }
        }

        if (!activeSk->startPosStamped) {
            activeSk->startPosStamped = true;
            if (auto* playLayer = PlayLayer::get(); playLayer && playLayer->m_startPosObject) {
                activeSk->recordedFromStartPos = true;
                activeSk->startPosX = playLayer->m_startPosObject->getPositionX();
                activeSk->startPosY = playLayer->m_startPosObject->getPositionY();
                skkbot2::log::info("RECORD: start position detected at ({:.0f},{:.0f}) - macro tagged SP",
                    activeSk->startPosX, activeSk->startPosY);
            }
        }

        if (canUsePersistenceCapture()) {
            activeSk->recordPerFrame(persistenceCaptureAttempt.perFrames, tick, player1, player2);
            if (persistenceCaptureAttempt.inputs.empty()) {
                return;
            }
            activeSk->recordAnchor(persistenceCaptureAttempt.anchors, tick, player1, player2, isPlatformer, isDual);
        } else {
            SkMacro* recSk = captureStaging ? captureStaging : activeSk;
            recSk->recordPerFrame(tick, player1, player2);
            if (pendingRngBoundary.progress > 0) {
                float const gap = [&]() -> float {
                    if (recSk->perFrames.size() < 2) return 0.0f;
                    auto const& cur = recSk->perFrames.back();
                    auto const& prev = recSk->perFrames[recSk->perFrames.size() - 2];
                    if (cur.tick != prev.tick + 1) return 0.0f;
                    float dx = cur.p1x - prev.p1x;
                    float dy = cur.p1y - prev.p1y;
                    return std::sqrt(dx * dx + dy * dy);
                }();
                skkbot2::log::info("RECORD respawn first frame: tick={} seam->sealed gap={:.2f}px "
                    "cur=({:.3f},{:.3f})",
                    tick, gap,
                    recSk->perFrames.empty() ? -1.0 : recSk->perFrames.back().p1x,
                    recSk->perFrames.empty() ? -1.0 : recSk->perFrames.back().p1y);
            }
            if (pendingRngBoundary.progress > 0) {
                SkRngBoundary boundary;
                boundary.tick = tick;
                boundary.progress = pendingRngBoundary.progress;
                auto liveTriple = skk::captureTripleRng();
                boundary.primary = liveTriple.primary;
                boundary.shake = skk::shakeStateGet();
                boundary.teleport = liveTriple.teleport;
                activeSk->rngBoundaries.push_back(boundary);
                skkbot2::log::info("RECORD rng boundary: tick={} progress={} "
                    "live(0x{:X},0x{:X})",
                    boundary.tick, boundary.progress,
                    boundary.primary, boundary.teleport);
                pendingRngBoundary = {};
                markActiveMacroDirty();
            }
            if (recSk->inputs.empty()) {
                return;
            }

            if (player1 && recSk->perFrames.size() >= 1) {
                auto const& back = recSk->perFrames.back();
                const bool nowUD = (back.p1vis & kVisualUpsideDown) != 0;
                const bool nowG = (back.p1vis & kVisualGravityNegative) != 0;
                const bool prevUD = recSk->perFrames.size() >= 2
                    ? (recSk->perFrames[recSk->perFrames.size() - 2].p1vis & kVisualUpsideDown) != 0
                    : nowUD;
                const bool prevG = recSk->perFrames.size() >= 2
                    ? (recSk->perFrames[recSk->perFrames.size() - 2].p1vis & kVisualGravityNegative) != 0
                    : nowG;
                if (nowUD != prevUD || nowG != prevG) {
                    const size_t from = recSk->perFrames.size() > 4 ? recSk->perFrames.size() - 4 : 0;
                    std::string win;
                    for (size_t i = from; i < recSk->perFrames.size(); ++i) {
                        auto const& f = recSk->perFrames[i];
                        win += fmt::format(" [t={} g={} ud={}]", f.tick,
                            (f.p1vis & kVisualGravityNegative) ? 1 : 0,
                            (f.p1vis & kVisualUpsideDown) ? 1 : 0);
                    }
                    skkbot2::log::info(
                        "REC grav flip: tick={} live(g={} ud={}) mode={} p1vis=0x{:02x} window:{}",
                        tick, player1->m_gravity < 0.0 ? 1 : 0,
                        player1->m_isUpsideDown ? 1 : 0,
                        (back.p1vis & kVisualModeMask) >> kVisualModeShift,
                        static_cast<int>(back.p1vis), win);
                }
            }

            recSk->recordAnchor(tick, player1, player2, isPlatformer, isDual);
        }
    }

    void finalizePersistenceCaptureDeath(int deathTick, bool deathPlayer2) {
        if (!canUsePersistenceCapture() || persistenceCaptureDeathRecorded) {
            return;
        }

        persistenceCaptureAttempt.deathTick = std::max(1, deathTick);
        persistenceCaptureAttempt.deathPlayer2 = deathPlayer2;
        if (!persistenceCaptureAttempt.inputs.empty() && persistenceCaptureAttempt.hasData()) {
            activeSk->persistenceAttempts.push_back(std::move(persistenceCaptureAttempt));
            markActiveMacroDirty();
            skkbot2::log::info("RECORD attempt #{} finalized: deathTick={} p2={} inputs={}",
                activeSk->persistenceAttempts.size(), deathTick, deathPlayer2,
                activeSk->persistenceAttempts.back().inputs.size());
        }
        persistenceCaptureAttempt = {};
        persistenceCaptureDeathRecorded = true;
    }

    void completePersistenceCaptureAttempt() {
        if (!canUsePersistenceCapture()) {
            return;
        }

        activeSk->inputs = std::move(persistenceCaptureAttempt.inputs);
        activeSk->anchors = std::move(persistenceCaptureAttempt.anchors);
        activeSk->perFrames = std::move(persistenceCaptureAttempt.perFrames);
        persistenceCaptureAttempt = {};
        persistenceCaptureDeathRecorded = false;
        markActiveMacroDirty();
        disablePersistenceModeSafeguard();
    }

    void markPersistencePlaybackDeathPending() {
        if (isPersistencePlaybackFailureAttempt()) {
            persistencePlaybackDeathPending = true;
        }
    }

    void advancePersistencePlaybackAfterReset() {
        if (!isPersistencePlaybackActive() || !persistencePlaybackDeathPending) {
            return;
        }

        if (auto const* attempt = activePersistencePlaybackAttempt()) {
            persistenceRenderBaseTick += std::max(1, attempt->deathTick);
        }
        ++persistencePlaybackAttemptIndex;
        persistencePlaybackDeathPending = false;
        executeIndex = 0;
        playbackAnchorIndex = 0;
        perFramePlaybackIndex = 0;
        playbackExhaustionNotified = false;
        resetPlayDriftMeter();
        skkbot2::log::info("PLAY advance: attempt {}/{} ({})",
            persistencePlaybackAttemptIndex + 1, activeSk->persistenceAttempts.size(),
            persistencePlaybackAttemptIndex < activeSk->persistenceAttempts.size()
                ? "chain link" : "main timeline");
        resetTimingTracking();
    }

    bool shouldTriggerPersistencePlaybackDeath(int effectiveTick) const {
        auto const* attempt = activePersistencePlaybackAttempt();
        return attempt
            && attempt->deathTick > 0
            && effectiveTick >= attempt->deathTick
            && !persistencePlaybackDeathPending;
    }

    bool shouldResetAfterPersistencePlaybackDeath(PlayLayer* playLayer) const {
        if (!playLayer || !isPersistencePlaybackFailureAttempt() || !persistencePlaybackDeathPending) {
            return false;
        }

        bool p1Dead = playLayer->m_player1 && playLayer->m_player1->m_isDead;
        bool p2Dead = playLayer->m_player2 && playLayer->m_player2->m_isDead;
        return p1Dead || p2Dead;
    }

    PlayerObject* persistencePlaybackDeathPlayer(PlayLayer* playLayer) const {
        if (!playLayer) {
            return nullptr;
        }

        auto const* attempt = activePersistencePlaybackAttempt();
        if (attempt && attempt->deathPlayer2 && playLayer->m_player2) {
            return playLayer->m_player2;
        }
        return playLayer->m_player1;
    }

    int persistenceFailedAttemptsTotalTicks() const {
        if (!activeSk) {
            return 0;
        }

        int total = 0;
        for (auto const& attempt : activeSk->persistenceAttempts) {
            total += std::max(1, attempt.deathTick);
        }
        return total;
    }

    int activeMacroEndTick() const {
        if (skMode && activeSk && !activeSk->inputs.empty()) {
            return activeSk->inputs.back().tick;
        }
        if (!skMode && activeMacro && !activeMacro->inputs.empty()) {
            return static_cast<int>(activeMacro->inputs.back().frame);
        }
        return 0;
    }

    int persistencePlaybackTotalTicks() const {
        if (!activeSk) {
            return 0;
        }
        return persistenceFailedAttemptsTotalTicks() + activeMacroEndTick();
    }

    int renderLocalTick() const {
        return std::max(0, lastTickIndex + tickOffset);
    }

    int renderTimelineTick() const {
        if (isPersistencePlaybackActive()) {
            return persistenceRenderBaseTick + renderLocalTick();
        }
        return lastTickIndex;
    }

    std::vector<PlaybackAnchor>* activeAnchors() {
        if (!skMode && !usesTimedAccuracy(activeMacroAccuracyMode())) {
            return nullptr;
        }
        if (skMode) {
            if (auto* attempt = activePersistencePlaybackAttempt()) {
                return &attempt->anchors;
            }
            return activeSk ? &activeSk->anchors : nullptr;
        }
        return activeMacro ? &activeMacro->anchors : nullptr;
    }

    std::vector<SkPerFrame>* activePerFrames() {
        if (!skMode) {
            return nullptr;
        }
        if (auto* attempt = activePersistencePlaybackAttempt()) {
            return &attempt->perFrames;
        }
        return activeSk ? &activeSk->perFrames : nullptr;
    }

    std::vector<PlaybackAnchor> const* activeAnchors() const {
        if (!skMode && !usesTimedAccuracy(activeMacroAccuracyMode())) {
            return nullptr;
        }
        if (skMode) {
            if (auto const* attempt = activePersistencePlaybackAttempt()) {
                return &attempt->anchors;
            }
            return activeSk ? &activeSk->anchors : nullptr;
        }
        return activeMacro ? &activeMacro->anchors : nullptr;
    }

    AnchorRngState captureRngState() const {
        AnchorRngState rng;
        rng.fastRandState = GameToolbox::getfast_srand();
        rng.locked = rngLocked;
        rng.seed = rngSeedVal;
        rng.shakeState = skk::shakeStateGet();
        rng.teleportState = *skk::gdTeleportRng();
        return rng;
    }

    void clearCheckpointResumedHolds() {
        for (auto& playerHolds : checkpointResumedHolds) {
            playerHolds.fill(false);
        }
    }

    void clearQueuedSubstepState() {
        queuedSubstepSegments.clear();
        substepMidStep = false;
        substepMacroDispatch = false;
        substepCaptureOffset = -1.0f;
    }

    void queueMacroCommand(int button, bool down, bool player2, double timestamp) {
        queuedMacroCommands.push_back({ button, down, player2, timestamp });
    }

    void clearQueuedMacroCommands() {
        queuedMacroCommands.clear();
    }

    void queueCbsCaptureCommand(int button, bool down, bool player2, double timestamp) {
        queuedCaptureCommands.push_back({ button, down, player2, timestamp });
    }

    std::optional<QueuedCaptureCommand> popCbsCaptureCommand(int button, bool down, bool player2) {
        if (!cbsCaptureProcessingQueue) {
            return std::nullopt;
        }

        auto it = std::find_if(
            queuedCaptureCommands.begin(),
            queuedCaptureCommands.end(),
            [&](QueuedCaptureCommand const& command) {
                return command.button == button
                    && command.down == down
                    && command.player2 == player2;
            }
        );
        if (it == queuedCaptureCommands.end()) {
            return std::nullopt;
        }

        auto result = *it;
        queuedCaptureCommands.erase(it);
        return result;
    }

    bool hasQueuedSubstepTick() const {
        return !queuedSubstepSegments.empty();
    }

    bool isCheckpointHoldResumed(int playerSlot, int button) const {
        if (playerSlot < 0 || playerSlot >= static_cast<int>(checkpointResumedHolds.size())) {
            return false;
        }
        if (button < 1 || button > 3) {
            return false;
        }
        return checkpointResumedHolds[playerSlot][static_cast<size_t>(button - 1)];
    }

    void setCheckpointHoldResumed(int playerSlot, int button, bool resumed) {
        if (playerSlot < 0 || playerSlot >= static_cast<int>(checkpointResumedHolds.size())) {
            return;
        }
        if (button < 1 || button > 3) {
            return;
        }
        checkpointResumedHolds[playerSlot][static_cast<size_t>(button - 1)] = resumed;
    }

    static ReplayEngine* get() {
        static auto* singleton = new ReplayEngine();
        return singleton;
    }

    void triggerAudio(bool secondPlayer, int actionType, bool pressed);

    void resetDeferredInputState();
    void buildQueuedSubstepSegments(
        std::vector<QueuedSubstepInput> actions,
        std::deque<QueuedSubstepSegment>& output
    ) const;
    void dispatchQueuedSubstepInput(PlayLayer* playLayer, QueuedSubstepInput const& input);
    void simulateQueuedSubstepTick(
        PlayLayer* playLayer,
        PlayerObject* player1,
        PlayerObject* player2,
        float stepDelta,
        std::deque<QueuedSubstepSegment>& segments,
        std::function<void(QueuedSubstepInput const&)> dispatchOverride = {}
    );
    void armQueuedSubstepTick(std::vector<QueuedSubstepInput> actions);
    void truncateRecordedDataAfter(int tick);
    static void closeSeamHoldEdges(std::vector<SkInput>& inputs, int boundaryTick);
    void prepareCaptureRestore(int tick);
    void finishCaptureRestore(PlayLayer* playLayer, int tick);
    int reconcileCaptureRestore(PlayLayer* playLayer, int tick);
    void captureResealBoundaryFrame(int tick, PlayerObject* p1, PlayerObject* p2, bool isPlatformer, bool isDual);
    int respawnStepTickForGameTick(int gameTick);
    void restoreLockedRngState(uintptr_t rngState);
    void restoreLockedShakeState(uint64_t shakeState);
    void pruneStoredRestorePoints(PlayLayer* playLayer, int restoredTick);
    void clearFrameStepState();
    int computeFrameStepMusicTimeMS(PlayLayer* playLayer) const;
    void requestFrameStepMusicSync(PlayLayer* playLayer);
    void syncFrameStepAudio(FMODAudioEngine* audio);
    void setFrameStepEnabled(bool enabled, PlayLayer* playLayer);
    void bridgeUserHoldsToPlayer(PlayLayer* playLayer);

    void processHotkeys();
};

#endif
