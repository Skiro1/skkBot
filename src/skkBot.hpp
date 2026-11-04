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
#include "hacks/autoclicker.hpp"
#include "hacks/physicsbypass.hpp"
#include "gui/skkbot_gui.hpp"
#include "render/renderer.hpp"

#include <Geode/Bindings.hpp>
#include <Geode/loader/Loader.hpp>

#include <algorithm>
#include <atomic>
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

// Physical player input, observed at the only place it can be observed with
// certainty: UILayer::keyDown / keyUp / ccTouch*. Playback injects straight into
// m_queuedButtons and never goes through here, so anything reaching these
// functions was pressed by a person. Implemented in replay_engine.cpp next to
// the hook that fires it.
namespace skkbot2 {
// One physical press. `kind` is prose ("key down"), `key` is the raw
// cocos2d::enumKeyCodes value; key 0 means a mouse touch rather than a key.
void notePlayerInput(char const* kind, int key);

// Session tally, then reset. Called when a run ends so the cap on individual
// lines does not cost the totals.
void dumpPlayerInputTally(char const* reason);
}  // namespace skkbot2

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

    // True while an async macro-file save is running on a worker thread: the
    // multi-MB .skk serialization+zstd+disk write happens off the game thread
    // (a synchronous save visibly hitched the game); completion posts back to
    // the main thread, and this flag is only ever touched there.
    std::atomic<bool> macroSaveInFlight{false};

    // True while the async save worker is still compressing+writing. The GUI
    // reads this to keep the RECORD button from being re-pressed mid-save (a
    // several-MB macro takes long enough that a double-click would otherwise
    // queue a second capture over the one already being flushed).
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
    // Strips the level to its gameplay layout (blueprint): decorations + colour
    // triggers hidden, authored colours/glow removed, background/ground/line
    // repainted to a flat palette. Purely visual, no gameplay effect. Backing
    // field for hacks/layout_mode.cpp; see that file for the reference ports
    // (GDH "Layout Mode", EclipseMenu, Neverhook).
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

    // Safe Mode (existing field): suppresses GD's progress accounting so a run
    // cannot bank score, jumps or attempts. Single master toggle Р В Р вЂ Р В РІР‚С™Р Р†Р вЂљРЎСљ it always
    // freezes attempts AND jumps. The per-effect switches that used to back this
    // were removed at the user's request (the extra two settings are gone from
    // the GUI); both effects stayed on, which is also what GDH "Safe Mode"
    // (hacks/core/safe_mode.cpp) and EclipseMenu "Safe Mode"
    // (hacks/Global/SafeMode.cpp) do. Backing field for hacks/safe_mode.cpp.
    bool safeMode = false;
    bool persistenceMode = false;

    // WAVE TRAIL FIX (drag-fix + draw-fix, always-on, visual only):
    // waveTrailFix is the master switch. dragFix rebuilds the streak geometry
    // every update (vanilla only adds points on collision, so noclipping or
    // fast vertical snaps leave gaps / stray lines); drawFix replaces the
    // vanilla stroke renderer with a miter-joined band that can be tinted and
    // given an opacity, and adds an outline sub-part.
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

    // RNG lock is non-configurable by design: always engaged, always the same
    // start seed (100), so every recording and playback share one deterministic
    // randoms base. The old GUI seed field was removed (see docs RNG survey).
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
    // GD's platformer setup-checkpoint lives behind m_activatedCheckpoint and
    // is not always the CheckpointObject left at the end of m_checkpointArray.
    // Keep the exact CheckpointObject made for that activation, like GucciBot's
    // m_platformerCheckpoints, so resetLevel can make GD restore that object.
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
    // True death pauses clean-line capture until GD completes the attempt reset.
    bool captureDeathPending = false;
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
    // Center tick of a permanent Р В РІР‚в„ўР вЂ™Р’В±24-label substep trace window, armed on the
    // last playback death so both the next PLAY attempt and the next RECORD
    // pass through the same window emit per-substep traces (half/last, dt,
    // levelTime, step) - lets record and playback phase grids be compared
    // line by line around the exact death. -1 disables.
    int probeAroundTick = -1;
    // BUG-102 v3: список центров окон (tick ± 24) из saved-значения
    // "skc-probe-ticks" (строка "416,516,1237") - несколько расхождений
    // за один прогон, без переконфигурации между сессиями. Плюс одиночный
    // легаси "skc-probe-tick" и death-tick из checkpoint_system.
    std::vector<int> probeAroundTicks;
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
    // Editor-playtest cadence compensation: GD funnels processCommands at
    // ~180/s with dt=1/180 there (not 240/s @ dt=1/240 like a PlayLayer),
    // so one call is NOT one macro tick. The step clock advances by
    // dt*tickRate instead of +1 to keep the playback grid locked to the
    // level's game time; this fraction holds the sub-tick remainder.
    double stepPairFrac = 0.0;
    // Periodic phase-grid visibility was plan-count based; replaced by the
    // wall-clock step-grid sample window (sessionSampleWallStart/DtSum).

    // Sample window per session: reset at session start so a window never
    // spans restarts/pauses (which used to produce bogus negative deltas).
    double sessionSampleLastLevelTime = 0.0;
    long long sessionSampleStepsWindow = 0;
    // The step-grid sample's wall-clock window: started on the first plan
    // after a reset, the sample fires every 2.0 s of WALL time and sums the
    // delivered dt stream (sessionSampleDtSum) so the log can compare
    // "sim time fed" against "wall time passed" directly.
    double sessionSampleWallStart = 0.0;
    double sessionSampleDtSum = 0.0;
    // BUG-97 instrument: the speedhack value seen in the last sample window.
    // A dynamic speed change DURING an SKC record is captured here and
    // reported once per change; the speed track work must make record ==
    // play under any speed sequence. Reset at session start.
    double sampleSpdLast = 0.0;
    // Smoothed measured frame time (seconds per drawn frame), the stall cap's
    // cadence source when the display pin is not being substituted. Updated
    // ONLY from non-stall frames: a stall frame's dt is the batch's own
    // serving time, and feeding it back re-creates BUG-66's self-reinforcing
    // budget loop (budget from delivered fps).
    double emaFrameTime = 0.0;
    // Stall accounting: how often the backlog past the cap was refused and
    // how much level time that dropped, plus how many scheduler walks never
    // became plans (custom-timing-off exits) - the first instrumented run
    // showed 1310 walks against 401 plans and the split was unreadable.
    long long lockBacklogDrops = 0;
    double lockBacklogDropped = 0.0;
    long long sessionSchedWalks = 0;
    long long sessionVanillaExits = 0;
    // Per-session step-grid audit: total plan calls, total steps planned,
    // how many hit the resume-fade zero-dt window, and the first/last
    // levelTime. Logged in haltExecution as "step-grid epilogue".
    long long sessionPlanCount = 0;
    long long sessionStepSum = 0;
    long long sessionResumeZeroPlans = 0;
    // Freeze-path audit (the deficit that is INSIDE update): plans whose
    // update() GD ran on the freeze path - zero physics, zero levelTime, the
    // whole batch lost. Two gates, read raw at plan time:
    //   12888 (0x3258) - the pause-family flag; 0 freezes every such frame;
    //   12964 (0x32A4) - the post-resume grace counter; freeze requires >= 2
    //     because this update's own getModifiedDelta call decrements it
    //     before the freeze check sees it.
    long long batFrozenFlagPlans = 0;
    long long batFrozenFlagSteps = 0;
    long long batFreezeCtrPlans = 0;
    long long batFreezeCtrSteps = 0;
    // BUG-79 start-window trace (INFO, once per session): the user reports
    // "the song starts while the level is somehow still waiting" - i.e. the
    // same pathology BUG-70 measured (1.36 s of frozen frames at attempt
    // start: scene/audio/triggers alive, physics and levelTime dead). These
    // fields record WHEN the frozen gate engaged, how many frames it ate,
    // and the wall time between first freeze and first live physics plan,
    // so the next run ties the symptom to a number.
    int startFrozenFrames = 0;      // gate-frozen frames this session
    bool startFrozenReported = false;  // gate-engage INFO emitted
    double startFrozenWall = 0.0;   // wall (hot::now) of the first frozen frame
    bool startLiveReported = false; // first-live-plan INFO emitted
    bool playbackFirstInputReported = false; // first [PLAY-START] emitted
    double sessionFirstLevelTime = 0.0;
    double sessionLastLevelTime = 0.0;
    bool sessionLevelTimeInit = false;
    // Lock delta audit.
    //
    // Always on, so this is not a session setting but a measurement of the frame
    // clock: how far the measured dt strays from the interval that replaces it,
    // and what the planner decides as a result. lockDroppedCarry is the SIGNED
    // sum of that difference - positive means real frames were longer than the
    // pinned interval, negative means shorter.
    long long lockPlanCount = 0;
    long long lockWantedSum = 0;
    long long lockStepSum = 0;
    long long lockMaxWanted = 0;
    // How many frames the step cap actually bit on. Steady state and a stall
    // backlog look identical in every other counter, and this is the number that
    // says whether the cap is a guard or a tax: it should be 0 in a healthy run
    // and non-zero on exactly the frames where the display fell behind.
    // Delivered vs requested display rate. The bug this project shipped on
    // 19.10.2026 was advancing the clock by 1/360 while the machine delivered
    // 1/240; the log printed the requested rate and never the delivered one, so
    // a third of the speed looked like a judgement call rather than a fact.
    //
    // Starts at 1.0, not 0.0: it is the fraction of frames that MISSED the
    // interval, and `displayHolds` is `fraction < 0.25`. At 0.0 the verdict is
    // therefore "the display is holding the pin" before a single frame has been
    // measured - a test that cannot fail for its first window. Starting at 1.0
    // means the honest measured frame time is used until evidence arrives, and
    // evidence is the only thing that can turn the substitution on.
    double lockOffCadenceFraction = 1.0;
    long long lockOffCadenceInWindow = 0;
    long long lockWindowFrames = 0;
    // BUG-95: the substitution constant is the display's ACTUAL cadence -
    // learned from the window of measured frame dts and snapped to an
    // integer fps. Same monitor for record and play -> the same snapped
    // constant, so the step grid repeats across sessions even when
    // getAnimationInterval lies about it (probed: promised 1/240 while the
    // V-Sync'd display delivered 1/180, which left the interval-gated
    // substitution permanently off and the grid undeterministic).
    //
    // BUG-95a: the first take (median, re-anchor on any 2% drift) flapped
    // the anchor 180 <-> 158 <-> 97 fps every few seconds on hitchy frames
    // and the level audibly sped up and slowed down. Two disciplines on
    // top of the learn: the window statistic is the 25th percentile (the
    // fast frame cluster, immune to hitches that drag the median into the
    // slow cluster), and the anchor moves only after a candidate differs
    // by >10% across two consecutive windows (hysteresis).
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
    // BUG-94 probes: per step-grid sample window, the raw dt the scheduler
    // hook actually received per drawn frame, and how many entries fell back
    // to the vanilla path. The 22:48 record sample showed walks and plans in
    // a split that looked impossible for a per-frame hook - these counters
    // must split that apart in one run instead of guesses.
    float sampleRawDtMin = 0.0f;
    float sampleRawDtMax = 0.0f;
    double sampleRawDtSum = 0.0;
    long long  sampleRawFrames = 0;
    long long  sampleVanillaExits = 0;
    // BUG-96 probes: the drive stream's dt was caught summing to MORE than
    // the wall time that actually passed (dtSum 4.1s in a 2.0s wall window -
    // impossible for an honest wall-fed stream). For every drive we also
    // store the REAL wall gap since the previous drive; comparing the two
    // distributions names the phantom drives (dt > wall gap) or proves the
    // stream honest once and for all.
    double sampleWallGapSum = 0.0;
    float  sampleWallGapMin = -1.0f;
    float  sampleWallGapMax = 0.0f;
    long long samplePhantomCount = 0;
    double samplePhantomDtSum = 0.0;
    // Pacing-deficit stage 2: raw drive frames are HONEST (rawSum == wallGap
    // sum), but the dt the planner received (dtSum) is ~17ms/2s short. Split
    // the seam inside one window: how many plans the window carried vs how
    // many raw frames drove it, and the rawSum itself next to dtSum - the
    // difference names the exact loss class (undelivered plans, substituted
    // constant, or per-frame walk count skew).
    long long samplePlansWindow = 0;
    // Pacing-deficit stage 3 (RT): the feed passes through three stages
    // before the planner sees it - raw dt (GD), the DWM-tick smoothing
    // (refresh_frame_time) and the wall clamp. Each stage can eat time, and
    // the totals name WHICH one does:
    //   sampleRawDtSum - what GD reported (already counted per drive)
    //   sampleDwmSum   - what the display-tick smoothing returned
    //   sampleClampSum - what the wall clamp let through
    //   sampleFedSum   - what the planner received (the dtSum the log shows)
    double sampleDwmSum = 0.0;
    double sampleClampSum = 0.0;
    double sampleFedSum = 0.0;
    // How many drives the window carried (one per drawn frame) and how many
    // of them the DWM smoothing actually rewrote (a rewritten drive is a
    // candidate for the loss).
    long long sampleDrives = 0;
    long long sampleDwmRewrites = 0;
    // BUG-96 stage-3 audit: does the tempo-debt feed actually reach the
    // planner? sampleDebtSum/count = what advanceNormalScheduler fed into
    // g_extraDelta; sampleWantSum = what the planner ASKED for
    // (wantedSteps * timestep); sampleFreezeDropped = debt destroyed by the
    // frozen gate (zeroing the carry on pause-family frames). If feed is
    // large but want is not, the loss sits between the two.
    double sampleDebtSum = 0.0;
    long long sampleDebtCount = 0;
    double sampleWantSum = 0.0;
    double sampleFreezeDropped = 0.0;
    double lastDriveWall = 0.0;
    // Frame number at which lock delta was first seen doing something, so a report
    // can say where in the macro it engaged rather than only that it did.
    int lockFirstPlanFrame = -1;
    int processSliceSkips = 0;
    // True while the tick planner's quantized dt is inside the engine update
    // (getModifiedDelta must pass it through, not re-quantize).
    bool customTickPlanActive = false;
    // CBS dispatch mix (proves which sub-step consumption branch is running).
    int cbsImmediateCount = 0;
    int cbsQueueNativeCount = 0;
    int cbsWaitCount = 0;
    // Vanilla-physics delta measured before the recorded per-frame position is
    // applied. Diagnostic only; this is not rendered/post-reconcile drift.
    float playDriftMax = 0.0f;
    int playDriftMaxTick = -1;
    int playDriftLastLogTick = -100000;
    int playDriftLastSampleTick = -100000;
    // Rendered-drift meter (VISIBLE residual): sampled at isLastTick AFTER the
    // whole pin+physics+reconcile cycle, i.e. what the eye actually renders
    // against the recorded frame for the same effective tick. Unlike
    // playDriftMax (which reads pre-reconcile preP1x and always shows the
    // Р В РІР‚в„ўР вЂ™Р’В±1.95px phase saw), this excludes the measurement artifact.
    float renderDriftMax = 0.0f;
    int renderDriftMaxTick = -1;
    int renderDriftLastLogTick = -100000;
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
    // SKC owns a separate per-macro accuracy environment. Do not let the
    // settings UI adopt that temporary runtime mode as the user's preference.
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
    // The effective tick of the most recent pre-step dispatch. The trajectory
    // ghost mirrors the bot's own grid, not a clock it re-derives: every real
    // physics step reads actions with `tick <= effectiveTick`, and the window
    // advances by exactly one per step, so the ghost's i-th step must ask for
    // `lastDispatchEffectiveTick + 1 + i`. Deriving this from FrameClock or
    // levelTime instead put every macro press one or two ticks off.
    int lastDispatchEffectiveTick = -1;
    bool startPosActive = false;
    std::string startPosWarning;
    bool startPosWarningIsKey = false;

    // Macro-list keys carry their format, because a stem can legitimately exist
    // twice: the SAME name saved once as SKK (.skk) and once as SKC (.sc).
    // Keying on the bare stem deduped them into one row, so the SKK copy became
    // invisible whenever a SKC macro of the same name was present - and the
    // delete path removed BOTH files. Key is now "<stem> [<ext>]"; these two
    // split it back apart at every boundary (display, load, info, delete).
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
    // Bumped by every reloadMacroList() disk scan so shells can cheaply re-sync
    // their cached macro list once an async scan (e.g. after a save lands on
    // the worker thread) has refreshed the engine's storedMacros.
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
            // Keyed on stem AND extension: DF2.skc and DF2.skk are two distinct
            // macros and both must be reachable.
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

            // .skk/.skc are native. .gdr2 and .slc are shared formats read from other bots
            // and converted into SKC on load - their macros become playable here,
            // but we never write either (our output stays lossless .skc).
            if (extension != ".skk" && extension != ".skc" &&
                extension != ".gdr2" && extension != ".slc" && extension != ".nhm" && extension != ".xe" &&
                extension != ".re4" && extension != ".ttrl" &&
        extension != ".echo" &&
        extension != ".ast" &&
                !skk::gbr::looksLikeGbrByExtension(extension)) {
                continue;
            }

            // Always surface the file in the list so the user can see it even
            // if it fails to parse (e.g. a corrupt or foreign .skk/.skc). Metadata
            // like CBS/platformer tags are only set when the file parses.
            addUsable(stem, extension);

            if (extension == ".skk") {
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
        }

        if (ec) {
            skkbot2::log::warn("Failed while scanning replay directory {}: {}", skk::pathToUtf8(directory), ec.message());
        }
    }

    // Queue Record/Play to start automatically when the next level is entered.
    void queueRecord(std::string const& name) {
        pendingAction = PendingAction::Record;
        pendingActionName = name;
        // BUG-80: a fresh PlayLayer can reuse the dead one's heap address;
        // disarm the [ENTRY-FREEZE] watcher here so the queued entry rearms.
        skkbot2::entryFreezeReset();
        skkbot2::log::info("RECORD queued for next level entry (name='{}')", name);
    }
    void queuePlayback(std::string const& name) {
        pendingAction = PendingAction::Play;
        pendingActionName = name;
        // Same disarm reason as queueRecord above.
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
        // Disarm the queued playback START too. Cancelling the pending action
        // used to leave this latched, and every "is playback busy?" test that
        // consults it then stayed true forever Р В Р вЂ Р В РІР‚С™Р Р†Р вЂљРЎСљ which is what kept the Frame
        // Stepper master toggle greyed out after a cancelled PLAY.
        pendingPlaybackStart = false;
    }

    // Loads a .skk macro by name from the replays dir into memory (no playback).
    bool loadMacroFromName(std::string const& name) {
        if (engineMode != MODE_DISABLED) return false;
        if (name.empty()) return false;
        namespace fs = std::filesystem;
        auto dir = skkbot2::paths::replays();
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
        // Consuming is not cancellation. Clear only the queue fields here;
        // clearPendingAction() is the user-cancel path and also disarms a
        // pending start. Reusing it made successful level-entry consumption
        // indistinguishable from a cancelled PLAY in the log.
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
        // Queued PLAY can load a macro before PlayLayer::init has populated
        // m_levelSettings. Older queued recordings therefore may carry the
        // default classic-mode bit even though they belong to this exact
        // platformer level. Level identity is the guard: never infer mode from
        // a different level or silently make cross-level macros compatible.
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
        // Queued from outside the level: the level is already at the start on
        // entry, so skip the initialRun reset when playback begins.
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
        anchorReconciliation = true;
        perFrameReconciliation = true;
        nextBoundaryIndex = 0;
        pendingRngBoundary = {};
        // v4: every new capture runs on the GD physics-step label grid.
        stepClock = true;
        stepPairCount = 0;
        stepPairFrac = 0.0;
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
        // Same capture, same rule as the tick rate one line above: what the
        // menu is running right now, stored only if it says something the
        // framerate does not already say.
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
            // ---- BUG-06 diagnostics: dump every consecutive-frame jump in the
            // recorded per-frame stream to see the REAL seam structure (raw
            // recorded data, before any playback smoothing). Any |dpos|>THRESH
            // between AUDACIOUS frames is exactly the "teleport" players see.
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
            // Do not force disable practice mode
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
                // PLAY pressed while ALREADY inside an editor playtest: arm in
                // place, then park so the LIVE position drives the takeover
                // (the player is mid-flight, not at a spawn - re-aligning to a
                // spawn would fake a backwards teleport).
                if (!armExecutionState()) {
                    engineMode = MODE_DISABLED;
                    return false;
                }
                applyVarianceOverrides();
                parkPlaybackPending();
                return true;
            }
            // Editor build mode (or menu): arm now; the playtest-start hook
            // performs the spawn alignment when physics begins.
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

    // Live-playback takeover (PLAY pressed while the player is mid-run): the
    // engine is sitting in the armed-waiting state (pendingPlaybackStart true,
    // MODE_DISABLED). Each tick we check whether the recording actually covers
    // the player's CURRENT spot (X AND Y proximity, so the takeover is seamless);
    // on the first covered tick we arm MODE_EXECUTE IN PLACE - no resetLevel,
    // no realignment to the macro's first frames, no backwards teleport.
    bool tryLivePlaybackTakeover(GJBaseGameLayer* playLayer);

    // True when the recording has a frame/anchor within the given X/Y
    // tolerances of (refX, refY). Used to refuse starting playback from a spot
    // the macro never covered, instead of tight->loose aligning a raw spawn to
    // a mid-run macro's first frames (the ~900px PLAY teleport).
    bool coversPlaybackPosition(float refX, float refY,
                                float xTolerance, float yTolerance);

    // Transition from a just-armed MODE_EXECUTE state back into the
    // armed-waiting state (pendingPlaybackStart true, MODE_DISABLED) when the
    // reset spawn turned out to be uncovered by the recording. Keeps the macro
    // loaded so the live takeover can arm once the player reaches covered ground.
    void parkPlaybackPending();

    // Step-grid and lock-delta session epilogues. Extracted from
    // haltExecution so SKC's stop path (Bot::stop) prints the SAME audits:
    // SKC sessions plan ticks through the same machinery and the numbers were
    // previously lost whenever a run ended outside SKK's own stop funnel.
    void logTimingEpilogues() {
        // Step-grid session epilogue: audit plan/step/resume counts.
        if (sessionPlanCount > 0) {
            const double ltDelta = sessionLastLevelTime - sessionFirstLevelTime;
            // The tick count is the session's ACTUAL served steps, not
            // ltDelta * runtimeTickRate(): runtimeTickRate is the menu's tick
            // rate (240 by default), while SKC playback runs at the macro's
            // tps (e.g. 3414) through the physics bypass - the old estimate
            // printed ~1813 ticks for what was really 25805 steps.
            const long long estTicks = static_cast<long long>(sessionStepSum);
            skkbot2::log::info(
                "step-grid epilogue: plans={} steps={} resumeZero={} | "
                "levelTime {:.4f} -> {:.4f} (+{:.4f}s = {} steps) | plan/step {:.2f}",
                sessionPlanCount, sessionStepSum, sessionResumeZeroPlans,
                sessionFirstLevelTime, sessionLastLevelTime, ltDelta, estTicks,
                estTicks > 0 ? static_cast<double>(sessionPlanCount) / static_cast<double>(estTicks) : 0.0);
        }
        // Lock delta session epilogue.
        //
        // Always on, so unlike the setting it replaced there is no "was it
        // switched off" case to guard - every run produces one of these. The
        // numbers to read are lockMaxDtSkew (how far the wall clock strayed from
        // the smoothed grid, i.e. how much smoothing work was needed) and the
        // steps-per-frame average against the tps/fps ratio it should equal.
        // Cadence truth lives in the step-grid samples: dtSum/wall is the
        // real-time fidelity number (100% = the level was fed real time).
        // (The old "display delivered X fps, Y% of frames late" fields are gone:
        // they were never updated since the wall-clock rework and always printed
        // their initial values 0.0 fps / 100% - noise that read as slow motion.)
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
        // The user's autosave contract (02.11): a rescue snapshot at EVERY
        // record-off moment, not only when the GUI stop button runs
        // stopRecording() (level complete, macro swap, editor exit, hotkey
        // oddities all funnel here without it). Still record-only: the call
        // is a no-op outside MODE_CAPTURE, and skkSnapshot bumps nothing
        // when the capture has zero inputs.
        if (engineMode == MODE_CAPTURE) {
            ::skkbot2::autosave::flushRecording();
        }
        if (engineMode == MODE_CAPTURE) {
            captureVarianceSnapshot();
            if (activeSk) {
                skkbot2::log::info("RECORD session ended: main inputs={} anchors={} frames={} | chain links={} | variance slots={}",
                    activeSk->inputs.size(), activeSk->anchors.size(), activeSk->perFrames.size(),
                    activeSk->persistenceAttempts.size(), activeSk->varianceOverrides.size());
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
        // Hand back a pinned display rate. haltExecution is the one place
        // every SKK stop funnels through - GUI stop, macro swapped, editor
        // exit - so it is where the pin has to be released, or it survives the
        // session that set it and slows down ordinary play.
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

    // The display rate the ACTIVE macro declares. The wire value is decoded by
    // MacroDisplayRate::fromWire, which is the only place the sign of zero is
    // read: +0.0 = the macro carries nothing (FPS was off when it was
    // recorded), -0.0 = fps == tps, n = pinned. All three engage on playback;
    // the middle one used to be indistinguishable from the first and did
    // nothing, so a macro recorded with fps == tps played on whatever the menu
    // had at the time.
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
        // Only native accuracy levels exist now: Vanilla and CBS. SIP/CBF are
        // declared incompatible at load time and any accuracy tag they left in
        // old replay files degrades to CBS (best native level).
        return mode == AccuracyMode::CBS ? AccuracyMode::CBS : AccuracyMode::Vanilla;
    }

    // Adopt GD's live CBS / COS state so a change made in GD's own options Р В Р вЂ Р В РІР‚С™Р Р†Р вЂљРЎСљ or by
// another mod Р В Р вЂ Р В РІР‚С™Р Р†Р вЂљРЎСљ shows up in our menu instead of a stale value.
//
// Source of truth, in order:
//   1. GJBaseGameLayer::m_clickBetweenSteps / m_clickOnSteps Р В Р вЂ Р В РІР‚С™Р Р†Р вЂљРЎСљ the fields GD
//      itself gates on (2.2081 GJBaseGameLayer.cpp:1180 reads
//      m_clickBetweenSteps). Authoritative while a level is on screen.
//   2. Game vars 0177 / 0176 Р В Р вЂ Р В РІР‚С™Р Р†Р вЂљРЎСљ our own persisted state, used when no game layer
//      exists (main menu / option screens). GD's own code never touches these
//      two keys, so they must not be treated as a mirror of GD's settings.
//
// An adopted value is mirrored into our saved values, otherwise the next launch
// resurrects the old ones from gui_accuracy / gui_cos. Returns true when
// something actually changed.
//
// Skipped while a replay owns the accuracy environment: the replay applies its
// own mode there and the user's live choice is restored by
// endReplayAccuracyEnvironment(), not by the menu.
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

            // The display rate, on the same contract: taken from the macro,
            // re-asserted idempotently on every arm, handed back in
            // endReplayAccuracyEnvironment().
            //
            // Inside this guard because the flag is really "this is PLAYBACK,
            // not capture" - it is false on the recording call. That distinction
            // is the whole reason: at record time the macro's number was just
            // read OUT of the menu setting a line earlier, so applying it back
            // would be circular. It would also freeze the FPS row for the length
            // of a recording and log a pin that was never a change. At play time
            // the number comes from the FILE and applying it is the entire
            // point of storing it.
            const auto declaredFps = activeMacroDisplayRate();
            if (!playbackFpsOverrideActive) {
                if (skkbot2::fps::beginMacro(declaredFps)) {
                    playbackFpsOverrideActive = true;
                }
            } else {
                skkbot2::fps::updateMacro(declaredFps);
            }

            // Timing mode: the USER's GUI toggles drive SKK playback directly
            // (Echo parity, per the 02.11 BUG-96 stage-5 contract). The macro's
            // own lockDelta/realTime flags were stamped here on record and are
            // honoured only as metadata - the advanceNormalScheduler /
            // applyCustomTickPlan decision words read gui.lockDelta/gui.realTime
            // for BOTH engines ("SKC and SKK alike"). Re-overwriting the GUI
            // from the header made any pre-play choice invisible: the mode
            // always snapped back to whatever the file stored. skcBot still
            // restores the SKC header's mode (skc_bot.cpp), unchanged.
            auto& gui = skkbot2::gui::get();
            skkbot2::log::info(
                "[SKK] playback timing mode: {} (the GUI toggle is the driver)",
                gui.realTime && !gui.lockDelta ? "Real Time" : "Lock Delta");
            dropCarry();
        }

        applyRuntimeAccuracyMode(mode, cosEnabled);
    }

    void endReplayAccuracyEnvironment() {
        if (playbackFpsOverrideActive) {
            // Before the tick rate is handed back, not after: endMacro() puts
            // the menu's own FPS state back AND drops the pin, so leaving it to
            // run after the tick rate is restored would leave one frame where
            // the display is back at its old rate and the budget still sized
            // for the macro's.
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

    // Hand the tick rate back to the menu's own setting WITHOUT ending the
    // replay's accuracy environment Р В Р вЂ Р В РІР‚С™Р Р†Р вЂљРЎСљ the TPS toggle in the GUI uses this when
    // the user turns it off mid-playback, so the clock returns to whatever the
    // menu declared at arm time (the "Р В Р Р‹Р Р†Р вЂљРЎв„ўР В Р’В Р РЋРІР‚СћР В Р Р‹Р Р†Р вЂљРЎв„ў TPS Р В Р Р‹Р Р†Р вЂљР Р‹Р В Р Р‹Р Р†Р вЂљРЎв„ўР В Р’В Р РЋРІР‚Сћ Р В Р’В Р вЂ™Р’В±Р В Р Р‹Р Р†Р вЂљРІвЂћвЂ“Р В Р’В Р вЂ™Р’В» Р В Р’В Р В РІР‚В  Р В Р’В Р РЋР’ВР В Р’В Р вЂ™Р’ВµР В Р’В Р В РІР‚В¦Р В Р Р‹Р В РІР‚в„–" behaviour) while
    // CBS / click-between-steps stay engaged for the running replay.
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
        // Nominal period for scheduler/carry accounting. Keep this in double;
        // the actual GD physics quantum is rounded once by fixedPhysicsDelta.
        return 1.0 / runtimeTickRate();
    }

    double fixedPhysicsDelta(double timeWarp = 1.0) const {
        // GD's per-step physics delta is ultimately a float. Match the
        // reference's rounding order: calculate (1 / TPS) * timeWarp in
        // double, round that single-step quantum to float exactly once, then
        // promote the exact float value back to double for planning and the
        // step-grid hooks. Rounding 1/TPS first and applying timeWarp later
        // can differ by one float ULP per step and accumulate over a level.
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

        if (captureDeathPending) {
            skkbot2::log::debug(
                "capture drop [death-window]: tick={} button={} p2={} down={}",
                tick, button, player2, pressed);
            return;
        }

        // Never put death-animation input into a macro. In particular,
        // platformer callbacks can arrive between destroyPlayer and resetLevel.
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

        // Per-frame state and anchors are the trajectory stream. A dead frame
        // is not a legal point on the clean route and must never be serialized.
        if (captureDeathPending || (player1 && player1->m_isDead) ||
            (player2 && player2->m_isDead)) {
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
                // capture path precisely. Shake is captured the same way now
                // that it is locked for both SKK and SKC.
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

            // Gravity-flip forensic window (record side).
            //
            // A UFO that touches a gravity portal during RECORD looked correct but
            // the macro replayed it frozen at that instant, so either the sampling
            // point lags a tick behind the portal effect or playback fails to apply
            // it. Both are visible only if we dump the recorded bits around the
            // transition: if the flip first appears one tick AFTER the live player
            // already changed, the record is the culprit; if the recorded flip is on
            // time, playback is. captureTick() runs on the step-grid hook
            // (stepCount@0x237DCE), which fires BEFORE GD's collision pass, so the
            // lag is the prime suspect.
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
        // Shake is locked for BOTH SKK and SKC now, so it must be captured
        // like any other locked stream - otherwise a checkpoint taken
        // mid-attempt stores 0 and the practice restore cannot rewind it.
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
