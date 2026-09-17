#ifndef _skkBot_hpp
#define _skkBot_hpp

#include "utils.hpp"
#include "lang/localization.hpp"
#include "format/replay.hpp"
#include "format/skk_format.hpp"
#include "core/practice_fix_bridge.hpp"
#include "core/noclip_accuracy.hpp"
#include "core/replay_timing.hpp"
#include "core/rng_lock.hpp"
#include "hacks/autoclicker.hpp"
#include "trajectory/trajectory.hpp"
#include "render/renderer.hpp"

#include <Geode/Bindings.hpp>
#include <Geode/loader/Loader.hpp>

#include <algorithm>
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
    static constexpr int kTrajectoryLengthMin = 1;
    static constexpr int kTrajectoryLengthDefault = 312;
    static constexpr int kTrajectoryLengthSliderMax = 2400;
    static constexpr int kTrajectoryLengthMax = 60000;
    static constexpr int kTrajectoryPredictionStepMax = 250000;

    MacroMode engineMode = MODE_DISABLED;
    ValidationResult validationStatus = RESULT_OK;

    bool dataModified = false;

    // True while an async macro-file save is running on a worker thread: the
    // multi-MB .skk serialization+zstd+disk write happens off the game thread
    // (a synchronous save visibly hitched the game); completion posts back to
    // the main thread, and this flag is only ever touched there.
    bool macroSaveInFlight = false;

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
    bool showTrajectory = false;
    bool trajectoryBothSides = false;
    bool creatingTrajectory = false;

    bool showHitboxes = false;
    bool hitboxOnDeath = false;
    bool noclipHitboxOnDeath = false;
    bool hitboxTrail = false;
    int hitboxTrailLength = 240;

    int noclipTotalFrames = 0;
    int noclipUnsafeFrames = 0;
    int noclipDeathEvents = 0;
    int noclipAccuracyDecimals = 2;
    bool noclipWouldDieThisFrame = false;
    bool noclipDeadLastFrame = false;
    PlayerObject* noclipLastDeathPlayer = nullptr;
    GameObject* noclipLastDeathObject = nullptr;

    // RNG lock is non-configurable by design: always engaged, always the same
    // start seed (100), so every recording and playback share one deterministic
    // randoms base. The old GUI seed field was removed (see docs RNG survey).
    bool rngLocked = true;
    static constexpr unsigned int rngSeedVal = 100;
    uintptr_t capturedRngState = 0;

    int pathLength = kTrajectoryLengthDefault;

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
    std::vector<CheckpointStateBundle> practiceFixQueue;
    int lastTickIndex = 0;
    int respawnTickIndex = -1;
    // Respawn splice staging (flat SKK main timeline, bug BUG-06). Each
    // practice attempt records into captureStaging instead of the main
    // timeline. On death/respawn the segment up to the last checkpoint the
    // attempt TOUCHED while alive is committed into main (that crossing is
    // proven progress); everything above it (the dying tail, incl. air/death
    // frames below newer checkpoints) is dropped and never reaches playback.
    // The retry therefore can never leave a death seam in the line, and a
    // failed section keeps the last clean traversal from an earlier attempt.
    SkMacro* captureStaging = nullptr;
    int captureStagingStartTick = 0;
    int captureStagingTouchFloor = 0;
    bool captureStagingActive() const {
        return engineMode == MODE_CAPTURE && captureStaging && skMode && activeSk && !canUsePersistenceCapture();
    }
    void captureStagingReset(int startTick);
    void captureCommitStagingToFloor(int floorTick);
    void captureFlushStagingAll();
    void captureOnRespawn(int respawnTick);
    void captureOnCheckpointTouched(int tick);
    // Practice-restart RNG boundary (spliced recordings): the checkpoint
    // restore latch {progress, live triple} here. The next captured frame
    // stamps the macro tick and pushes the LIVE triple (not a formula) to
    // activeSk->rngBoundaries. Playback restores entries with
    // nextBoundaryIndex + applyRngBoundaryReseed.
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
    bool levelRestarting = false;
    bool initialRun = false;
    bool pendingPlaybackStart = false;
    // When playback was queued from outside a level (fresh entry) the level is
    // already at the start, so the initialRun reset would cause a needless
    // visible restart. This flag suppresses that single reset.
    bool suppressInitialRunReset = false;
    enum class PendingAction { None, Record, Play };
    PendingAction pendingAction = PendingAction::None;
    std::string pendingActionName;
    bool anchorReconciliation = false;
    bool perFrameReconciliation = false;
    bool playbackExhaustionNotified = false;
    // Countdown budget for the substep-grid probe (processCommands trace in
    // record/play logs). Armed on capture/play start; 0 disables logging.
    int substepProbe = 0;
    // Center tick of a permanent ±24-label substep trace window, armed on the
    // last playback death so both the next PLAY attempt and the next RECORD
    // pass through the same window emit per-substep traces (half/last, dt,
    // levelTime, step) - lets record and playback phase grids be compared
    // line by line around the exact death. -1 disables.
    int probeAroundTick = -1;
    // Frame-boundary input semantics (capture truth): GD consumes touches
    // between frames, effective on the FIRST substep of the next frame, and
    // capture stamps the press with that frame's entry label. Dispatching on
    // mid-frame substep label equality puts clicks one substep late in the
    // fps-grid (the 1,1,2-step pattern). The planner refreshes frameEntryTick
    // at every frame top; the first playback substep of the frame dispatches
    // everything due for that entry label, once.
    int frameEntryTick = -1;
    bool frameDispatchDone = true;
    // Per-anchor fast_rand audit: counts emitted desync warnings (reset with
    // the drift meter at every PLAY setup).
    int rngStreamMismatchReports = 0;
    // Throttle bookkeeping for dash-ring reattachment attempts (per slot).
    int dashReattachLast[2] = { -999, -999 };
    // v4 step-grid clock: when true, tick labels come from completed physics
    // sub-step PAIRS (half+last), the same grid GD's rings/flips/collisions
    // live on - immune to levelTime-vs-step phase drift. Reset per attempt.
    bool stepClock = false;
    int stepPairCount = 0;
    // Periodic phase-grid visibility (every ~10s of sim time) for logs.
    int phaseSampleCounter = 0;
    // Sample window per session: reset at session start so a window never
    // spans restarts/pauses (which used to produce bogus negative deltas).
    double sessionSampleLastLevelTime = 0.0;
    long long sessionSampleStepsWindow = 0;
    // Per-session step-grid audit: total plan calls, total steps planned,
    // how many hit the resume-fade zero-dt window, and the first/last
    // levelTime. Logged in haltExecution as "step-grid epilogue".
    long long sessionPlanCount = 0;
    long long sessionStepSum = 0;
    long long sessionResumeZeroPlans = 0;
    double sessionFirstLevelTime = 0.0;
    double sessionLastLevelTime = 0.0;
    bool sessionLevelTimeInit = false;
    int processSliceSkips = 0;
    // True while the tick planner's quantized dt is inside the engine update
    // (getModifiedDelta must pass it through, not re-quantize).
    bool customTickPlanActive = false;
    // CBS dispatch mix (proves which sub-step consumption branch is running).
    int cbsImmediateCount = 0;
    int cbsQueueNativeCount = 0;
    int cbsWaitCount = 0;
    // Playback phase-drift meter: |arrived - recorded| at force time.
    float playDriftMax = 0.0f;
    int playDriftMaxTick = -1;
    int playDriftLastLogTick = -100000;
    int playDriftLastSampleTick = -100000;
    // Seam-recovery glide (BUG-06): a recording seam (practice death/restart
    // stitched into the per-frame stream) produces a physically-impossible jump
    // with NO portal object nearby. Snapping the player across it in one tick
    // looks like a 'fast lag-freeze' teleport. Instead, while this watermark is
    // active, prePhyPositionPin/reconcilePerFrames move the player toward the
    // recorded route at a capped per-tick speed so the jump reads as a fast
    // natural dash instead of a blink.
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
    // Capture-mode shadow of "we last recorded this slot/button as held".
    // GD clears m_holdingButtons internally (wave flips, gate steps) before the
    // physical release reaches handleButton, so the game flag alone drops the
    // release edge - a recorded wave section then replays as an endless hold.
    // Index: slot*3 + (button-1), slot 0=P1, 1=P2.
    bool recordedHoldShadow[6] = { false, false, false, false, false, false };
    // Last logged game-side held bit (button 1) for P1/P2 during capture, so
    // the hold-probe only fires on real changes. -2 = unset.
    int holdProbeLast[2] = { -2, -2 };
    // After a practice checkpoint restore / restart, GD zeroes player hold
    // maps and then delivers the stale physical releases as ordinary
    // handleButton events. Recording those seam releases writes phantom
    // 'up' edges into the stream (duplicate release right after the rewind
    // tick) which cancel held-orb/ring states early at replay. Releases are
    // dropped until this label; presses always pass.
    int captureReleaseSuppressUntil = 0;
    // Orientation-state change probe (gravity sign / upside-down / spider /
    // rotation bucket) logged in both CAPTURE and PLAYBACK; diffing the two
    // event sequences localizes 'character visually wrong gravity' to the
    // recorded data or to playback restore. Rotation is bucketed to 5 deg to
    // avoid tween spam.
    uint64_t gravProbeLast = 0;
    // BUG-04 study: capped counter of bursty per-frame reconciles (>1 frame
    // consumed by reconcilePerFrames in a single tick) - the smoke signal for
    // 'recorded state applied N ticks late'.
    int perFrameReconciliationCatchUps = 0;
    // BUG-04 study: number of live-vs-recorded gravity/ud mismatches logged
    // by gravDiffProbe (capped so a pathological run can't flood the log).
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
    bool savedClickBetweenSteps = false;
    bool savedLayerClickBetweenSteps = false;
    bool hasSavedLayerClickBetweenSteps = false;
    bool savedClickOnSteps = false;
    bool savedLayerClickOnSteps = false;
    bool hasSavedLayerClickOnSteps = false;
    bool playbackTickRateOverrideActive = false;
    double savedPlaybackTickRate = kBaseTickRate;
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

    // CBS sub-tick phase anchors. GD's PlayerButtonCommand::m_timestamp is the
    // step-aligned game clock (m_timestamp), frozen between processCommands
    // calls, so m_timestamp - tickStartTimestamp is always ~0 and a recorded
    // CBS macro never captured its real within-tick phase. Instead we anchor
    // the LIVE wall clock at every new tick (see processCommands) and stamp
    // the wall clock on each physical input arrival (UILayer keyDown/keyUp /
    // ccTouchBegan/Ended hooks). The delta is the true sub-tick phase.
    double tickStartWallTime = 0.0;       // wall-clock seconds at current tick anchor
    double prevTickStartWallTime = 0.0;   // wall-clock seconds at previous tick anchor
    double prevTickStartTimestamp = 0.0;  // game-clock seconds at previous tick anchor
    double inputArrivalWallTime = -1.0;   // wall-clock seconds of last physical input
    bool hasInputArrivalWallTime = false;

    double cbsPhaseFromInputWall() const {
        if (!hasInputArrivalWallTime) {
            return -1.0;
        }
        // Frame the physical arrival between the previous and the current tick
        // anchors in WALL time. The macro's cbsTimeOffset is a sub-tick phase
        // in GAME seconds within [0, 1/tps); the wall interval between two
        // processCommands calls is ~1/240 (a step), which can be LONGER than
        // the macro tick (1/tps at tps>240), so a raw wall delta would crack
        // the recordAction guard (< 1/safeTps). Normalize to a fraction first.
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
            return -1.0; // arrival after the current tick start lands in this tick's slot
        }
        return clamped * (1.0 / runtimeTickRate());
    }

    int tickOffset = 0;
    bool startPosActive = false;
    std::string startPosWarning;
    bool startPosWarningIsKey = false;

    std::vector<std::string> storedMacros;
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
        storedMacros.clear();
        incompatibleMacros.clear();
        cbsMacros.clear();
        platformerMacros.clear();
        skMacros.clear();
        auto directory = geode::prelude::Mod::get()->getSaveDir() / "replays";
        std::error_code ec;
        if (!std::filesystem::exists(directory, ec) || ec) {
            return;
        }

        std::unordered_set<std::string> usableStems;

        auto addUsable = [&](std::string const& stem) {
            if (usableStems.insert(stem).second) {
                storedMacros.push_back(stem);
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

            // Only .skk macros are supported. Ignore every other extension.
            if (extension != ".skk") {
                continue;
            }

            // Always surface the file in the list so the user can see it even
            // if it fails to parse (e.g. a corrupt or foreign .skk). Metadata
            // like CBS/platformer tags are only set when the file parses.
            addUsable(stem);
            skMacros.insert(stem);

            std::unique_ptr<SkMacro> macro(SkMacro::loadFromPath(path));
            if (!macro) {
                continue;
            }
            if (macro->accuracyMode == AccuracyMode::CBS) {
                cbsMacros.insert(stem);
            }
            if (macro->platformerMode) {
                platformerMacros.insert(stem);
            }
        }

        if (ec) {
            skkbot2::log::warn("Failed while scanning replay directory {}: {}", skk::pathToUtf8(directory), ec.message());
        }
    }

    // Queue Record/Play to start automatically when the next level is entered.
    void queueRecord(std::string const& name) {
        pendingAction = PendingAction::Record;
        pendingActionName = name;
        skkbot2::log::info("RECORD queued for next level entry (name='{}')", name);
    }
    void queuePlayback(std::string const& name) {
        pendingAction = PendingAction::Play;
        pendingActionName = name;
        skkbot2::log::info("PLAY queued for next level entry (name='{}')", name);
    }
    void clearPendingAction() {
        if (pendingAction != PendingAction::None) {
            skkbot2::log::info("queued action cancelled ({})",
                pendingAction == PendingAction::Record ? "RECORD" : "PLAY");
        }
        pendingAction = PendingAction::None;
        pendingActionName.clear();
    }

    // Loads a .skk macro by name from the replays dir into memory (no playback).
    bool loadMacroFromName(std::string const& name) {
        if (name.empty()) return false;
        namespace fs = std::filesystem;
        auto dir = Mod::get()->getSaveDir() / "replays";
        auto file = dir / (name + ".skk");
        if (!fs::exists(file)) return false;
        std::unique_ptr<SkMacro> macro(SkMacro::loadFromPath(file));
        if (!macro) return false;
        delete activeSk;
        activeSk = macro.release();
        skMode = true;
        selectedAccuracyMode = activeSk->accuracyMode;
        enableCBSForLoadedMacroIfNeeded();
        double safeTps = activeSk->framerate > 0.0 ? activeSk->framerate : tickRate;
        practiceFixQueue = skk::bridge::wireToBundles(activeSk->practiceFixCheckpoints, safeTps);
        return true;
    }

    // Consume a queued Record/Play when a level is entered. Called from the
    // PlayLayer init hook. Returns true if a queued action was consumed.
    bool consumePendingAction(GJGameLevel* level) {
        if (pendingAction == PendingAction::None || !level) {
            return false;
        }
        PendingAction act = pendingAction;
        std::string name = pendingActionName;
        clearPendingAction();

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
            return false;
        }
        if (!hasMacroInputs()) {
            return false;
        }
        // Queued from outside the level: the level is already at the start on
        // entry, so skip the initialRun reset when playback begins.
        skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::Playback);
        suppressInitialRunReset = true;
        pendingPlaybackStart = true;
        nextBoundaryIndex = 0;
        pendingRngBoundary = {};
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
        anchorReconciliation = true;
        perFrameReconciliation = true;
        nextBoundaryIndex = 0;
        pendingRngBoundary = {};
        // v4: every new capture runs on the GD physics-step label grid.
        stepClock = true;
        stepPairCount = 0;
        resetTimingTracking();
        resetSessionStepGridStat();
        resetRecordedHoldShadow();
        // A fresh record session must start from a clean staging slate even
        // when Start was pressed without Clear first: leaving the previous
        // session's captureStaging alive would buffer all button presses into
        // staging and only surface them in activeSk after the first death
        // (captureOnRespawn -> captureCommitStagingToFloor), so Record->Play
        // right after a clean no-death run would miss every input.
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
        AccuracyMode captureAccuracyMode = runtimeAccuracyModeFor(selectedAccuracyMode);
        beginReplayAccuracyEnvironment(captureAccuracyMode, false);

        if (skMode) {
            initializeSkMacro(level);
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
        if (auto* playLayer = PlayLayer::get(); playLayer && playLayer->m_levelSettings) {
            activeMacro->platformerMode = playLayer->m_levelSettings->m_platformerMode;
            activeMacro->hasPlatformerModeMetadata = true;
        }
        dataModified = false;
    }

    void initializeSkMacro(GJGameLevel* level) {
        if (activeSk) {
            delete activeSk;
        }
        activeSk = new SkMacro();
        activeSk->accuracyMode = runtimeAccuracyModeFor(selectedAccuracyMode);
        activeSk->usesStepClock = true;  // v4: physics-step label grid
        if (level) {
            activeSk->levelId = level->m_levelID;
            activeSk->levelName = level->m_levelName;
            activeSk->name = level->m_levelName;
        }
        activeSk->framerate = tickRate;
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
        // Boundaries are appended in record order (practice respawns), which is
        // not ascending-by-tick; sort + dedupe so playback reseeds fire when the
        // run crosses each tick (BUG-06 RNG desync fix).
        if (skMode && activeSk && !activeSk->rngBoundaries.empty()) {
            size_t pre = activeSk->rngBoundaries.size();
            normalizeSkRngBoundaries(*activeSk);
            skkbot2::log::info("normalized rngBoundaries: {} -> {} (sorted by tick)",
                pre, activeSk->rngBoundaries.size());
        }
        engineMode = MODE_EXECUTE;
        refreshManualInputIgnoreState();
        // Follow the macro's own clock declaration (legacy files = time grid).
        stepClock = skMode && activeSk ? activeSk->usesStepClock : false;
        stepPairCount = 0;
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
        // Both arm sites run on a FRESH attempt, so the tick>2 initialRun
        // re-sync must NOT reset a second time: the double reset re-fires
        // spawn triggers (the spawn teleport then never re-arms) and shifts
        // RNG/object phase.
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
        beginReplayAccuracyEnvironment(runtimeAccuracyModeFor(activeMacroAccuracyMode()), true);
        // Auto-applied timing profile (idea 4): the macro's declared source TPS
        // drives the runtime clock substep grid regardless of the user's manual
        // tickRate setting; clock provenance follows the file's own declaration.
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
            // Materialization sanity trace: first/last runtime ticks of the
            // stream that will actually play.
            auto* list = activeSkInputs();
            if (list && !list->empty()) {
                skkbot2::log::info("PLAY stream: [0].tick={} [{}].tick={}",
                    (*list)[0].tick, list->size() - 1, list->back().tick);
            }
            // ---- BUG-06 diag #4: dump every input (index -> tick -> position)
            // so the user's pointer "problem at input ~99-100" can be tied to a
            // concrete tick and map position. Also logs a few frames around the
            // interesting inputs.
            if (list && !list->empty()) {
                std::string inDump = (Mod::get()->getSaveDir() / "inputs_dump.txt").string();
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
                // Position of the FIRST ~120 inputs mapped to the recorded route.
                if (!activeSk->perFrames.empty() && list->size() >= 80) {
                    size_t const limit = std::min<size_t>(list->size(), 130);
                    std::ostringstream oss;
                    oss << "BUG-06 input->pos:";
                    for (size_t i = 0; i < limit; ++i) {
                        auto const& in = (*list)[i];
                        // nearest perFrame <= in.tick
                        float px = 0.f, py = 0.f;
                        for (auto const& f : activeSk->perFrames) {
                            if (f.tick > in.tick) break;
                            px = f.p1x; py = f.p1y;
                        }
                        oss << " #" << i << "=" << in.tick << ":("
                            << std::fixed << std::setprecision(1) << px << "," << py << ")";
                        if (i >= 94 && i <= 109) {
                            // also dump 3 frames AROUND the reported spot
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
            // ---- BUG-06 diag #2: one-time dump of EVERY TeleportPortalObject
            // in the level (position + linked exit), to correlate the recorded
            // jump table (perframe_jumps.txt) with real portal placements.
            if (auto* pl = PlayLayer::get()) {
                std::string portalDump = (Mod::get()->getSaveDir() / "portal_objects.txt").string();
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
            // ---- BUG-06 diagnostics: dump every consecutive-frame jump in the
            // recorded per-frame stream to see the REAL seam structure (raw
            // recorded data, before any playback smoothing). Any |dpos|>THRESH
            // between AUDACIOUS frames is exactly the "teleport" players see.
            if (!activeSk->perFrames.empty()) {
                auto const& pfs = activeSk->perFrames;
                int64_t tickSpan = (int64_t)pfs.back().tick - (int64_t)pfs.front().tick;
                int64_t missingTicks = std::max<int64_t>(0, tickSpan - (int64_t)pfs.size());
                const double kPFJumpThreshold = 30.0;
                std::string dumpPath = (Mod::get()->getSaveDir() / "perframe_jumps.txt").string();
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
                    // Exact capture holes: every integer tick in [first..last]
                    // with no frame, as contiguous ranges - the "record side"
                    // of BUG-06 (did the macro lose ticks at the orb arcs?).
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
            if (playLayer->m_isPracticeMode) {
                playLayer->togglePracticeMode(false);
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
        if (!playLayer || fastPlayback) {
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

    void haltExecution() {
        if (engineMode == MODE_CAPTURE) {
            captureVarianceSnapshot();
            if (activeSk) {
                skkbot2::log::info("RECORD session ended: main inputs={} anchors={} frames={} | chain links={} | variance slots={}",
                    activeSk->inputs.size(), activeSk->anchors.size(), activeSk->perFrames.size(),
                    activeSk->persistenceAttempts.size(), activeSk->varianceOverrides.size());
            }
        } else if (engineMode == MODE_EXECUTE && activeSk) {
            skkbot2::log::info("PLAY session ended: consumed {}/{} inputs, maxDrift {:.2f}px@{}, "
                               "slice-skips={}, cbs[imm={},queue={},wait={}]",
                executeIndex, activeSk->inputs.size(), playDriftMax, playDriftMaxTick,
                processSliceSkips, cbsImmediateCount, cbsQueueNativeCount, cbsWaitCount);
        }
        // Step-grid session epilogue: audit plan/step/resume counts.
        if (sessionPlanCount > 0) {
            const double ltDelta = sessionLastLevelTime - sessionFirstLevelTime;
            const long long estTicks = static_cast<long long>(std::round(ltDelta * runtimeTickRate()));
            skkbot2::log::info(
                "step-grid epilogue: plans={} steps={} resumeZero={} | "
                "levelTime {:.4f} -> {:.4f} (+{:.4f}s ~{} ticks) | plan/tick {:.2f}",
                sessionPlanCount, sessionStepSum, sessionResumeZeroPlans,
                sessionFirstLevelTime, sessionLastLevelTime, ltDelta, estTicks,
                estTicks > 0 ? static_cast<double>(sessionPlanCount) / static_cast<double>(estTicks) : 0.0);
        }
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
        phaseSampleCounter = 0;
        sessionSampleLastLevelTime = 0.0;
        sessionSampleStepsWindow = 0;
    }

    bool hasMacro() const {
        return skMode ? (activeSk != nullptr) : (activeMacro != nullptr);
    }

    bool hasMacroInputs() const {
        if (skMode) {
            return activeSk && (!activeSk->inputs.empty() || !activeSk->persistenceAttempts.empty() ||
                                !persistenceCaptureAttempt.inputs.empty());
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
                                       !persistenceCaptureAttempt.inputs.empty() || !persistenceCaptureAttempt.anchors.empty()))
            || (activeMacro && !activeMacro->inputs.empty());
        return dataModified && hasInputs;
    }

    void markActiveMacroDirty() {
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
        // Main-first policy: when the macro owns a clean full run (the main
        // timeline), playing it directly beats re-enacting the death chain
        // (whose only visible effect was replaying a recorded death on run 1).
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

    double activePlaybackTickRate() const {
        double macroTps = activeMacroFramerate();
        double requiredTps = macroTps;
        if (skMode && activeSk && activeSk->loadedFromSKK3()) {
            requiredTps = activeSk->maxSourceTps();
        }
        return skk::replay_timing::playbackRuntimeTps(macroTps, requiredTps);
    }

    static AccuracyMode runtimeAccuracyModeFor(AccuracyMode mode) {
        // Only native accuracy levels exist now: Vanilla and CBS. SIP/CBF are
        // declared incompatible at load time and any accuracy tag they left in
        // old replay files degrades to CBS (best native level).
        return mode == AccuracyMode::CBS ? AccuracyMode::CBS : AccuracyMode::Vanilla;
    }

    static void applyRuntimeAccuracyMode(AccuracyMode mode, bool cosEnabled) {
        mode = runtimeAccuracyModeFor(mode);
        bool enabled = mode == AccuracyMode::CBS;
        // COS (Click On Steps) is an auxiliary vanilla mechanic: it only applies
        // together with Vanilla or CBS accuracy.
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
        selectedAccuracyMode = runtimeAccuracyModeFor(activeMacroAccuracyMode());
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
            // Replay declares its own timing profile (GDH reference): the source
            // TPS wins over the user's manual tickRate. Re-assert idempotently on
            // every arm - if a previous environment reuse left a stale auto rate,
            // the profile of the macro being replayed now is authoritative.
            double declaredTps = activePlaybackTickRate();
            if (!playbackTickRateOverrideActive) {
                savedPlaybackTickRate = tickRate;
                tickRate = declaredTps;
                playbackTickRateOverrideActive = true;
            } else if (tickRate != declaredTps) {
                tickRate = declaredTps;
            }
        }

        applyRuntimeAccuracyMode(mode, cosEnabled);
    }

    void endReplayAccuracyEnvironment() {
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

    double runtimeTickRate() const {
        return std::max(1.0, tickRate);
    }

    double trajectoryPredictionTickRate() const {
        return std::max(runtimeTickRate(), kBaseTickRate);
    }

    static int sanitizeTrajectoryLength(int length) {
        return std::clamp(length, kTrajectoryLengthMin, kTrajectoryLengthMax);
    }

    double trajectoryPredictionDuration() const {
        return static_cast<double>(sanitizeTrajectoryLength(pathLength)) / kBaseTickRate;
    }

    int trajectoryPredictionStepCountForRate(double predictionTickRate) const {
        if (!std::isfinite(predictionTickRate) || predictionTickRate <= 0.0) {
            return 0;
        }

        double stepCount = std::ceil(trajectoryPredictionDuration() * predictionTickRate);
        if (!std::isfinite(stepCount) || stepCount <= 0.0) {
            return 0;
        }

        return static_cast<int>(std::clamp(stepCount, 0.0, static_cast<double>(kTrajectoryPredictionStepMax)));
    }

    int trajectoryPredictionStepCount() const {
        return trajectoryPredictionStepCountForRate(trajectoryPredictionTickRate());
    }

    int trajectoryPredictionStepCount(float simulationDelta) const {
        double predictionTickRate = trajectoryPredictionTickRate();
        if (std::isfinite(simulationDelta) && simulationDelta > 0.0f) {
            predictionTickRate = std::max(kBaseTickRate, 60.0 / static_cast<double>(simulationDelta));
        }

        return trajectoryPredictionStepCountForRate(predictionTickRate);
    }

    float trajectorySimulationDelta() const {
        return static_cast<float>(60.0 / runtimeTickRate());
    }

    double effectiveTimeScale() const {
        return gameSpeed * (kBaseTickRate / runtimeTickRate());
    }

    float fixedSimulationDelta() const {
        return static_cast<float>(1.0 / runtimeTickRate());
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

    // Move the in-flight persistence attempt into the macro. Attempt data is
    // normally finalized by death, but Stop/halt/Save/restart can arrive
    // first; without this flush the segment (and in persistence mode often
    // the ENTIRE recording, since main streams stay empty) would be silently
    // dropped by clearPersistenceRuntimeState/resetPersistenceCaptureAttempt.
    void resetRecordedHoldShadow() {
        for (bool& state : recordedHoldShadow) {
            state = false;
        }
        holdProbeLast[0] = holdProbeLast[1] = -2;
        gravProbeLast = 0;
    }

    void flushPendingPersistenceCaptureAttempt() {
        if (!activeSk || !persistenceCaptureAttempt.hasData()) {
            persistenceCaptureAttempt = {};
            persistenceCaptureDeathRecorded = false;
            return;
        }
        // Deathless survivor runs are the FINAL timeline candidate: promote to
        // MAIN whenever the survivor is at least as long as current main
        // (same wholesale-replace rule as completePersistenceCaptureAttempt).
        // The old 'main must be empty' condition stranded a 20-input survivor
        // in the chain behind a stale 6-input main, and main-first playback
        // then ignored the good run. A survivor never belongs in the chain
        // (a link without death never advances); it stays there only as a
        // fallback when main is already longer (unreachable, harmless).
        if (persistenceCaptureAttempt.deathTick <= 0 &&
            persistenceCaptureAttempt.inputs.size() >= activeSk->inputs.size()) {
            const size_t inCount = persistenceCaptureAttempt.inputs.size();
            const size_t pfCount = persistenceCaptureAttempt.perFrames.size();
            activeSk->inputs = std::move(persistenceCaptureAttempt.inputs);
            activeSk->anchors = std::move(persistenceCaptureAttempt.anchors);
            activeSk->perFrames = std::move(persistenceCaptureAttempt.perFrames);
            skkbot2::log::info("RECORD flush: survivor promoted to MAIN (inputs={} frames={})",
                inCount, pfCount);
        } else {
            activeSk->persistenceAttempts.push_back(std::move(persistenceCaptureAttempt));
            skkbot2::log::info("RECORD flush: segment appended to chain (#{} links)",
                activeSk->persistenceAttempts.size());
        }
        persistenceCaptureAttempt = {};
        persistenceCaptureDeathRecorded = false;
    }

    // Variance session state (GJBaseGameLayer::m_varianceValues, 2000 slots
    // consumed by offset-variance objects and random triggers). Recording:
    // snapshot the non-zero slots while the level is live. Playback: force-
    // prefill those slots at every attempt start, so variance reads match the
    // recorded session regardless of how this game session's RNG history was
    // shaped. Slots that were still lazy at capture time are deliberately not
    // written - the locked per-tick reseed rerolls them to the same value.
    // Does activeSk belong to the level that is currently loading/live?
    // id>0 compares ids; id==0 (editor/unpublished) falls back to name;
    // empty metadata (legacy macros) is treated as matching.
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

    // R1 setup-seed parity (toasty-lite model, bindings only): GD chains
    // global fast_srand AND GJBaseGameLayer::m_randomSeed/m_replayRandSeed
    // through resetLevel to derive per-attempt object rolls (variance index,
    // random triggers). The chain input differs between sessions (menu
    // consumes LCG!) -> blocks land px-off -> deterministic mid-run deaths.
    // Force the layer's seed triple to a constant derived from rngSeedVal on
    // EVERY attempt in BOTH capture and playback (toasty-lite applies after
    // vanilla reset so GD's own derivation is overwritten, fully stateless).
    static constexpr uint64_t forcedAttemptSeed() {
        // splitmix64 of the macro's rng seed base - distinct per rngSeedVal,
        // nonzero, well-distributed into the LCG chain
        uint64_t z = static_cast<uint64_t>(rngSeedVal) + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    uint64_t macroSetupSeed() const {
        // legacy captures stored the observed pre-reset state - keep using it;
        // new recordings get the forced constant (stamped on capture below).
        if (activeSk && activeSk->setupRngSeed != 0) {
            return activeSk->setupRngSeed;
        }
        return forcedAttemptSeed();
    }

    // BEFORE vanilla resetLevel derives attempt state: feed the chain input.
    void pinSetupSeedBeforeReset() {
        if (!activeSk) {
            return;
        }
        if (engineMode == MODE_CAPTURE) {
            if (activeSkMatchesCurrentLevel()) {
                if (activeSk->setupRngSeed == 0) {
                    activeSk->setupRngSeed = forcedAttemptSeed();
                }
                // Snapshot counters BEFORE vanilla init re-reads them; GD does
                // not clear persistent counters at retry, so the map visible
                // here is exactly what this attempt starts with. The last
                // stamped attempt is the survivor that becomes MAIN.
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

    // AFTER vanilla resetLevel: overwrite GD's freshly derived state with the
    // forced constants (toasty-lite ordering - post-reset write beats the
    // session-dependent derivation inside vanilla).
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
        if (!activeSk || !activeSkMatchesCurrentLevel()) {
            // Do not let a foreign level's freshly-rolled variance table
            // overwrite the macro's recorded one (halt from the cross-level
            // init guard must not corrupt the kept macro's data).
            return;
        }
        auto* playLayer = PlayLayer::get();
        if (!playLayer) {
            return;
        }
        activeSk->varianceOverrides.clear();
        size_t index = 0;
        for (const float& value : playLayer->m_varianceValues) {
            if (value != 0.0f && std::isfinite(value) && index < playLayer->m_varianceValues.size()) {
                SkVarianceEntry entry;
                entry.index = static_cast<uint16_t>(index);
                entry.value = value;
                activeSk->varianceOverrides.push_back(entry);
            }
            ++index;
        }
    }

    void applyVarianceOverrides() {
        if (!activeSk || activeSk->varianceOverrides.empty()) {
            return;
        }
        if (!activeSkMatchesCurrentLevel()) {
            // Playing a foreign-level macro already desyncs; do not ALSO
            // spray its variance slots into this level's table.
            return;
        }
        auto* playLayer = PlayLayer::get();
        if (!playLayer) {
            return;
        }
        // m_varianceValues is absolute STATE, not a stream: GD fills it at
        // level load and objects read slots directly (no lazy reroll on read),
        // and it is not refilled per attempt. So playback restores the exact
        // captured table: clear everything, then write the recorded non-zero
        // slots. Slots absent from the snapshot were zero at capture time.
        std::fill(playLayer->m_varianceValues.begin(), playLayer->m_varianceValues.end(), 0.0f);
        for (auto const& entry : activeSk->varianceOverrides) {
            if (entry.index < playLayer->m_varianceValues.size()) {
                playLayer->m_varianceValues[entry.index] = entry.value;
            }
        }
        // Persistent trigger counters: the recorded run may have been played
        // as attempt N of its session (counters already non-zero); the fresh
        // replay session starts them at zero, which moves every
        // "count/count-equals/counter" gated object -> single-point world
        // deaths at identical ticks. Re-apply the captured map per attempt.
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

        if (canUsePersistenceCapture()) {
            activeSk->recordAction(persistenceCaptureAttempt.inputs, tick, button, player2, pressed, offset, cbsTimeOffset);
        } else if (captureStaging) {
            captureStaging->recordAction(tick, button, player2, pressed, offset, cbsTimeOffset);
        } else {
            activeSk->recordAction(tick, button, player2, pressed, offset, cbsTimeOffset);
        }
    }

    // Orientation-state change probe for both record and playback. Fires only
    // when (gravity sign | level-upside-down | spider | 5-deg rotation bucket)
    // changes for either player, so a spider section yields a short event
    // stream that can be diffed rec-vs-play to answer 'recorded wrong or
    // replayed wrong' for the character's gravity facing.
    static uint64_t gravStateKey(PlayerObject* p, uint64_t shift) {
        if (!p) {
            return 1ULL << shift;
        }
        uint64_t g = (p->m_gravity < 0.0) ? 1ULL : 0ULL;
        uint64_t ud = p->m_isUpsideDown ? 2ULL : 0ULL;
        uint64_t sp = p->m_isSpider ? 4ULL : 0ULL;
        // Node scale signs: GD flips some vehicle/spider visuals through
        // scaleX/scaleY rather than gravity; invisible to the g/ud/rot triple.
        uint64_t sx = p->getScaleX() < 0.0 ? 8ULL : 0ULL;
        uint64_t sy = p->getScaleY() < 0.0 ? 16ULL : 0ULL;
        // Dash-ring hold state: the ring object currently driving the
        // 'pressed' pose (null = not on a dash ring) + dashing flag. The
        // reported 'hold animation never plays' is this, not gravity.
        uint64_t dr = p->m_dashRing ? 32ULL : 0ULL;
        uint64_t dash = p->m_isDashing ? 64ULL : 0ULL;
        // Spider sprite is a CHILD node with its own rotation/scale - legs
        // orientation lives there, root scale/rot can match while it is wrong.
        uint64_t spr = 0;
        if (auto* spider = p->m_spiderSprite) {
            const double srot = std::fmod(std::fabs(static_cast<double>(spider->getRotation())), 360.0);
            spr = (static_cast<uint64_t>(static_cast<int>(srot / 15.0)) & 0xFULL) << 8;
            if (spider->getScaleX() < 0.0) spr |= (1ULL << 12);
            if (spider->getScaleY() < 0.0) spr |= (1ULL << 13);
        }
        double rot = std::fmod(std::fabs(static_cast<double>(p->getRotation())), 360.0);
        uint64_t rotb = static_cast<uint64_t>(static_cast<int>(rot / 5.0)) & 0x7FULL; // bits 14-20
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
        // Change-only logging misses channels that move in one session and
        // sit still in the other; a forced digest every 480 ticks (2s) keeps
        // both sides comparable everywhere at trivial log cost.
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
        // Dash-ring witness: when either player holds a ring, log ring vs
        // player positions + touching-ring counts. Decides the reattach
        // mystery: ring physically elsewhere (moving object, unpinned) vs
        // ring present but GD refusing the hold.
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

    // BUG-04 study: diff the LIVE player's gravity/ud against the RECORDED
    // per-frame target for the same effective tick. Unlike gravStateProbe
    // (which logs change-only state snapshots on both sides) this fires ONLY
    // when live disagrees with the recorded frame and stamps the relation:
    //   [raw]      - live BEFORE reconcile: GD's own physics already differ
    //                from the recording (the phantom originated in physics)
    //   [applied]  - live AFTER reconcile: our per-frame application still
    //                disagrees (the phantom is either not being applied or
    //                is swallowed by a later physics step)
    // Historically rare, so a small cap avoids log floods on a bad run.
    void gravDiffProbe(const char* stage, int tick, PlayerObject* p1, PlayerObject* p2) {
        if (gravDiffReports >= kGravDiffReportCap) {
            return;
        }
        auto* frames = activePerFrames();
        if (!frames || frames->empty() || !p1) {
            return;
        }
        // The recorded frame owning `tick`: the last pf.tick <= tick (frames up to
        // `tick` may be missing due to duplicate compression). Grep from the end;
        // the reconcile cursor (perFramePlaybackIndex) lags for the 'raw' stage.
        int pfIdx = -1;
        for (size_t i = frames->size(); i-- > 0;) {
            if (frames->at(i).tick <= tick) {
                pfIdx = static_cast<int>(i);
                break;
            }
        }
        if (pfIdx < 0) {
            return;
        }
        auto const& pf = frames->at(static_cast<size_t>(pfIdx));
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

        gravStateProbe("rec", tick, player1, player2);

        // Hold-bit probe: the game-side held state (button 1) per slot,
        // logged on change. Distinguishes the two remaining suspects of the
        // 'p2 press never activated the trigger' reports: a touch the game
        // never delivered (bit stays 0 across the physical press) vs one it
        // holds stuck at 1 after a lost release (cocos multi-touch glitch).
        // Diagnostic extension: also dump on a 30-tick cadence even without a
        // change, and include Y velocity + residual jump flags so a self-jump
        // that bypasses m_holdingButtons entirely (post-restore jumpBuffered /
        // robot residue) still shows up.
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

        // Start-position state isn't known at beginCapture (it runs before
        // PlayLayer::init builds m_startPosObject). Stamp it on the first
        // captured tick so recordedFromStartPos/startPosX/Y describe the real
        // run start; playback's SP guard depends on it.
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
            // Attempt ticks are run-local (restart at zero); each segment owns
            // a full per-frame stream in that same local coordinate space.
            activeSk->recordPerFrame(persistenceCaptureAttempt.perFrames, tick, player1, player2);
            if (persistenceCaptureAttempt.inputs.empty()) {
                return;
            }
            activeSk->recordAnchor(persistenceCaptureAttempt.anchors, tick, player1, player2, isPlatformer, isDual);
        } else {
            SkMacro* recSk = captureStaging ? captureStaging : activeSk;
            recSk->recordPerFrame(tick, player1, player2);
            // First frame after a practice respawn: verify the seam is sealed.
            // The boundary frame at `tick-1` was rewritten by captureResealBoundaryFrame
            // to the respawn position; if this frame is far from it the splice is
            // still broken (a >30px delta = the BUG-06 seam leaking into the line).
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
                // The boundary stores the LIVE post-restore globals - this is
                // the exact state the segment runs under, not a formula guess.
                // Teleport persists unchanged across a fast_srand respawn,
                // so taking it here (first captured frame) reproduces the
                // capture path precisely. Shake is not locked.
                boundary.primary = liveTriple.primary;
                boundary.shake = 0;
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
            recSk->recordAnchor(tick, player1, player2, isPlatformer, isDual);
        }
    }

    void finalizePersistenceCaptureDeath(int deathTick, bool deathPlayer2) {
        if (!canUsePersistenceCapture() || persistenceCaptureDeathRecorded) {
            return;
        }

        persistenceCaptureAttempt.deathTick = std::max(1, deathTick);
        persistenceCaptureAttempt.deathPlayer2 = deathPlayer2;
        // A death segment without a single input is pure noise: on playback
        // it forces a tick-1 death at the head of the chain ("strange death at
        // the start of play"). Only keep attempts the user actually played.
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
        rng.shakeState = 0; // shake (camera) is not locked
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
    // Juice-fix (GucciBot): a respawn/reset cut of the input stream can slice a
    // press/release pair - the press stays while its release lands beyond the
    // seam (Popped by truncateAfter or suppressed by the post-restore window),
    // so a held key replays as permanently stuck across the stitch. Re-scan the
    // retained tail and append one synthetic release per dangling (p2, button)
    // edge at the seam tick so every pair closes on the boundary.
    static void closeSeamHoldEdges(std::vector<SkInput>& inputs, int boundaryTick);
    void prepareCaptureRestore(int tick);
    // BUG-06 record-side seal: after a practice checkpoint restore, rewrite the
    // recorded boundary frame at `tick` (kept by truncateAfter from the dying
    // attempt) to the post-restore respawn position. Without this the seam
    // frame keeps the corpse tail position (e.g. x=17099) while the retry's
    // first frame (tick+1) is at the spawn position (x=17228) - a ~130px jump
    // recorded straight into the per-frame line. Sealing makes the boundary
    // smooth and leaves one legit checkpoint teleport for Play to snap.
    void captureResealBoundaryFrame(int tick, PlayerObject* p1, PlayerObject* p2, bool isPlatformer, bool isDual);
    // BUG-06 playback-side mapping: the recorded streams are labelled with the
    // capture step clock, but practice respawns re-align against the game-time
    // tick (m_levelTime). Map a game-time respawn tick back to the step-clock
    // tick of the first retry frame (bundle.priorTick + 1) so Play continues
    // from the seamless splice, not the 130px-behind dying tail. Returns -1
    // when no checkpoint bundle matches (level start, no checkpoints).
    int respawnStepTickForGameTick(int gameTick);
    void restoreLockedRngState(uintptr_t rngState);
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
