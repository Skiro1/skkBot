#include "skkBot.hpp"
#include "core/checkpoint_handler.hpp"
#include "core/editor_playback.hpp"
#include "core/gameplay_layer.hpp"
#include "core/hotpath.hpp"
#include "core/physical_keys.hpp"
#include "core/replay_timing.hpp"
#include "core/step_trace.hpp"
#include "gui/skkbot_gui.hpp"
#include "hacks/autoclicker.hpp"
#include "skc/skc_batch_probe.hpp"
#include "skc/skc_bot.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/UILayer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <sstream>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif


// Every PHYSICAL key press the player makes, logged.
//
// The macro's own clicks never come through here: playback pushes straight into
// m_queuedButtons (physicsbypass.cpp:403, skc_hooks.cpp:426), so anything that
// arrives on UILayer::keyDown is a human by construction. That makes this the
// one place where "was that me or the player?" is a fact rather than a guess -
// and it is the same shape as syncAccuracyFromGame(), which adopts CBS/COS out
// of GD's live fields and logs the moment they move. Here the thing being
// adopted is the player's own hands.
//
// Individual lines are capped, not the counter. During recording a player taps
// constantly and the lines would bury everything else; during playback the cap
// is what makes a stray press visible rather than the hundredth one.
//
// Defined here, at the top of the file, because everything below this point sits
// inside a namespace that would turn these into different symbols than the ones
// skkBot.hpp declares - and the linker failure that caused looks like nothing at
// all ("unresolved external symbol skkbot2::notePlayerInput") while the cause is
// three hundred lines away from where the compiler complains.
namespace skkbot2 {
namespace {
struct PlayerInputTally {
    long long total = 0;
    long long duringPlayback = 0;
    long long duringRecording = 0;
    long long printed = 0;
    long long suppressed = 0;
    static constexpr long long kPrintCap = 40;
};

PlayerInputTally g_playerInput;
}  // namespace

void notePlayerInput(char const* kind, int key) {
    auto& tally = g_playerInput;
    ++tally.total;

    // Both bots' state is asked for rather than one being picked. SKK's state lives
    // in ReplayEngine::engineMode (get() hands back a pointer, as everywhere else
    // in this file); SKC keeps its own enum. The local is not named `skk` -
    // that would shadow the namespace holding SKC.
    auto* skkEngine = ReplayEngine::get();
    const bool skkPlaying = skkEngine && skkEngine->engineMode == MODE_EXECUTE;
    const bool skkRecording = skkEngine && skkEngine->engineMode == MODE_CAPTURE;
    auto& skc = skk::skc::Bot::get();
    const bool skcPlaying = skc.isPlaying();
    const bool skcRecording = skc.isRecording();
    const bool skcIgnoring = skc.getIgnoreInputs();

    if (skkPlaying || skcPlaying) {
        ++tally.duringPlayback;
    }
    if (skkRecording || skcRecording) {
        ++tally.duringRecording;
    }

    const bool active = skkPlaying || skcPlaying || skkRecording || skcRecording;
    const bool playing = skkPlaying || skcPlaying;

    // During playback a press from the human is the interesting case, and the
    // one that can corrupt a run. Say whether it will be swallowed rather than
    // letting the reader assume: Ignore Inputs clears the queue before the
    // macro's commands are pushed (skc_hooks.cpp:413), and with it off the press
    // interleaves and the run drifts. Both are documented behaviour; which one
    // is active decides whether the line matters.
    //
    // Escape is special: this hook does NOT swallow it - UILayer still receives
    // it and RobTop's own code pauses the game with it. Calling that "swallowed"
    // sent the BUG-69 investigation looking for a swallowed-pause defect while
    // the pause menu was on screen the whole time.
    const char* verdict =
        key == 27
            ? "GD pauses the game on Escape - playback is frozen in the pause menu"
            : playing
                ? (skcIgnoring
                       ? "during playback, swallowed by Ignore Inputs"
                       : "during playback, NOT ignored - it interleaves with the macro")
                : (active ? "while a run is open but not playing" : "outside a run");

    if (tally.printed < PlayerInputTally::kPrintCap) {
        ++tally.printed;
        skkbot2::log::info(
            "player-input: {} {} (code {}) - {}; press #{} this session "
            "[total {} / playback {} / recording {}]",
            kind, skkbot2::gui::Gui::keyName(key), key, verdict, tally.printed,
            tally.total, tally.duringPlayback, tally.duringRecording);
    } else {
        ++tally.suppressed;
        if (tally.suppressed == 1) {
            skkbot2::log::info(
                "player-input: further individual lines suppressed; the tally "
                "keeps counting and is printed with the session");
        }
    }
}

void dumpPlayerInputTally(char const* reason) {
    auto& tally = g_playerInput;
    if (tally.total == 0) {
        return;
    }
    skkbot2::log::info(
        "player-input tally ({}): {} presses, {} during playback, {} during "
        "recording, {} lines printed, {} suppressed",
        reason, tally.total, tally.duringPlayback, tally.duringRecording,
        tally.printed, tally.suppressed);
    tally = PlayerInputTally {};
}
}  // namespace skkbot2

namespace {
    // Level-wide portal list cache shared by findTeleportEntryPortal and the
    // pre-warm at playback start (so the first ~40ms build never lands on a
    // seam tick — the PLAY_HOT pin=40ms at tick 539 was the cache build on
    // the start-warp seam). Portals are static for a level; the list is
    // rebuilt only when the m_objects array pointer or its count changes.
    CCArray* gPortalCacheArray = nullptr;
    size_t gPortalCacheCount = 0;
    std::vector<TeleportPortalObject*> gPortalCache;

    // === PLAYBACK JUMP TRACER ====================================================
    // Auto-activates a detailed per-tick trace around any visible "jump" detected
    // at the frame boundary (isLastTick). Logs full state: live pos/vy/vyb/rot
    // before reconcile, recorded target, what reconcile sets, and all visual flags.
    //
    // Root-cause hypothesis from reference-bot study: GDH pins only position
    // (velocity_fix=OFF, never rotation), silicate pins nothing.  Our
    // reconcilePerFrames pins pos+vy+vyb+rot+gravity+dash+ring EVERY tick —
    // this unique over-pin can fight GD's physics and produce a visible hop.
    // ============================================================================
    static int  gFrameLastTick      = -1;    // tick of previous isLastTick
    static float gFrameLastPx       = 0.f;   // visible position after last frame
    static float gFrameLastPy       = 0.f;
    static float gFrameLastRot      = 0.f;
    static double gFrameLastVy      = 0.0;
    static int  gFrameCount         = 0;     // visual frame counter

    static int  gTraceWindowEnd     = -1;    // tick: active trace window ends here
    static constexpr int kTraceWindowLen = 40; // frames to trace after jump detected
    static constexpr float kJumpThresholdPx = 10.0f;  // frame-to-frame visible jump
    // Rendered-drift logging cadence (Этап 0): sample every N ticks at the
    // isLastTick visual boundary so the actual post-reconcile offset is visible
    // even when it stays below any "visible" threshold.
    static constexpr int kRenderDriftReportEveryTicks = 180;

    // Format a single-line P1 snapshot for the tracer.
    static void logP1TraceLine(
        const char* tag, int tick,
        float liveX, float liveY, double liveVy, double liveVyb, float liveRot,
        float wantX, float wantY, double wantVy, double wantVyb, float wantRot,
        float dx, float dy, double dVy, double dVyb, float dRot,
        int gravNeg, int ud, int dash, int ringId)
    {
        skkbot2::log::info(
            "[TRC{}] t={} "
            "live=({:.2f},{:.2f}) vy={:.3f} vyb={:.3f} rot={:.1f} | "
            "rec=({:.2f},{:.2f}) vy={:.3f} vyb={:.3f} rot={:.1f} | "
            "Δ=({:.2f},{:.2f}) Δvy={:.3f} Δvyb={:.3f} Δrot={:.1f} | "
            "g={} u={} d={} r={}",
            tag, tick,
            liveX, liveY, liveVy, liveVyb, liveRot,
            wantX, wantY, wantVy, wantVyb, wantRot,
            dx, dy, dVy, dVyb, dRot,
            gravNeg, ud, dash, ringId);
    }
}  // namespace

static void prePhyWarmPortalCache(GJBaseGameLayer* playLayer) {
    auto* objects = playLayer ? playLayer->m_objects : nullptr;
    if (!objects) {
        gPortalCacheArray = nullptr;
        gPortalCacheCount = 0;
        return;
    }
    if (gPortalCacheArray == objects &&
        gPortalCacheCount == static_cast<size_t>(objects->count())) {
        return;
    }
    gPortalCacheArray = objects;
    gPortalCacheCount = static_cast<size_t>(objects->count());
    gPortalCache.clear();
    gPortalCache.reserve(64);
    for (int i = 0; i < objects->count(); ++i) {
        auto* tp = dynamic_cast<TeleportPortalObject*>(objects->objectAtIndex(i));
        if (tp) {
            gPortalCache.push_back(tp);
        }
    }
    skkbot2::log::info("prePhyPin portal cache rebuilt: objects={} portals={}", objects->count(), gPortalCache.size());
}

void refreshRngState(bool isRestart = false);

namespace {
    // When true, handleButton is being called from per-frame sync (processCommands)
    // for a physical release that GD missed. Suppress normal GD processing but
    // still record the release into the macro stream.
    bool g_perFrameReleaseSync = false;
    // Same as above but for a press event — the physical key went down but GD
    // never fired handleButton (.e.g. key was held through death/respawn).
    bool g_perFramePressSync = false;

    // DIAGNOSTIC: set around the vanilla GJBaseGameLayer::handleButton call
    // inside our hook, so the pushButton/releaseButton probes can tell whether
    // the press originated from GD's own dispatch (real user input passing
    // through vanilla) or from a direct PlayerObject::pushButton invocation
    // elsewhere. Distinguishes 'user self-press inside vanilla' from an
    // external synthesized jump.
    bool g_diagInVanillaHandleButton = false;

    cocos2d::CCPoint g_substepP1Position = { 0.f, 0.f };
    cocos2d::CCPoint g_substepP2Position = { 0.f, 0.f };
    float g_substepRotationDelta = 0.0f;
    bool g_substepP1Split = false;
    bool g_substepP2Split = false;

    namespace tick_util {
        int current(GJBaseGameLayer* layer, ReplayEngine* engine) {
            auto* playLayer = PlayLayer::get();
            // Editor playtest has no PlayLayer: fall back to the explicit layer
            // the drive-loop passes (the hooked GJBaseGameLayer running physics).
            GJBaseGameLayer* active = playLayer ? static_cast<GJBaseGameLayer*>(playLayer) : layer;
            if (!active) return 0;
            if (engine && engine->stepClock) {
                return std::max(0, engine->stepPairCount + 1);
            }
            int tick = static_cast<int>(active->m_gameState.m_levelTime * engine->runtimeTickRate());
            return std::max(0, tick + 1);
        }
    }

    // BUG-79 start trace: where exactly does the macro's FIRST click land
    // relative to the level clock? distance input.tick - effectiveTick is the
    // macro's own lede; anything further out should match the [START-WIN]
    // frozen-window line in physicsbypass.cpp. Once per session, INFO.
    static void logFirstPlaybackInput(ReplayEngine* engine, int inputTick,
                                      int referenceTick, double levelTime,
                                      size_t index, size_t total) {
        if (engine->playbackFirstInputReported) {
            return;
        }
        engine->playbackFirstInputReported = true;
        skkbot2::log::info(
            "[PLAY-START] first macro input dispatched: input.tick={} refTick={} "
            "levelTime={:.4f}s idx={}/{} - positive (inputTick - refTick) is the "
            "macro's lede; compare with [START-WIN] for the attempt-start freeze",
            inputTick, referenceTick, levelTime, index, total);
    }

    static bool resolveFlipState() {
        auto* playLayer = PlayLayer::get();
        if (!playLayer) return GameManager::get()->getGameVariable("0010");
        if (playLayer->m_levelSettings->m_platformerMode) return false;
        return GameManager::get()->getGameVariable("0010");
    }

    class DeferredDispatcher {
        GJBaseGameLayer* layer;
        ReplayEngine* engine;
        int currentTick;
        bool twoPlayers;
    public:
        DeferredDispatcher(GJBaseGameLayer* l, ReplayEngine* e, int tick)
            : layer(l), engine(e), currentTick(tick),
              twoPlayers(l->m_levelSettings->m_twoPlayerMode) {}

        bool tryFireMain(int playerSlot) {
            if (engine->deferredInputTick[playerSlot] != currentTick) return false;
            engine->deferredInputTick[playerSlot] = -1;
            bool player2 = twoPlayers ? (playerSlot == 0) : false;
            bool prevInVanilla = g_diagInVanillaHandleButton;
            g_diagInVanillaHandleButton = true;
            layer->GJBaseGameLayer::handleButton(true, 1, player2);
            g_diagInVanillaHandleButton = prevInVanilla;
            return true;
        }

        bool tryFireMainRelease(int playerSlot) {
            if (engine->deferredReleaseA[playerSlot] != currentTick) return false;
            engine->deferredReleaseA[playerSlot] = -1;
            bool player2 = twoPlayers ? (playerSlot == 0) : false;
            bool prevInVanilla = g_diagInVanillaHandleButton;
            g_diagInVanillaHandleButton = true;
            layer->GJBaseGameLayer::handleButton(false, 1, player2);
            g_diagInVanillaHandleButton = prevInVanilla;
            return true;
        }

        bool tryFireLateralRelease(int playerSlot, int buttonSlot) {
            if (engine->deferredReleaseB[playerSlot][buttonSlot] != currentTick) return false;
            engine->deferredReleaseB[playerSlot][buttonSlot] = -1;
            bool player2 = twoPlayers ? (playerSlot == 0) : false;
            bool prevInVanilla = g_diagInVanillaHandleButton;
            g_diagInVanillaHandleButton = true;
            layer->GJBaseGameLayer::handleButton(false, buttonSlot == 0 ? 2 : 3, player2);
            g_diagInVanillaHandleButton = prevInVanilla;
            return true;
        }
    };

    static bool hasDualPlaybackContext(GJBaseGameLayer* playLayer) {
        return playLayer && (playLayer->m_gameState.m_isDualMode || playLayer->m_levelSettings->m_twoPlayerMode);
    }

    static int deferredSlotForPlayer(bool twoPlayerMode, bool player2) {
        return twoPlayerMode ? static_cast<int>(!player2) : 0;
    }

    static bool keepSecondPlayerSlot(GJBaseGameLayer* layer) {
        return layer && (layer->m_levelSettings->m_twoPlayerMode || layer->m_gameState.m_isDualMode);
    }

    static bool isTrackedHoldButton(int button) {
        return button >= 1 && button <= 3;
    }

    static float clampSubstepOffset(float offset) {
        if (!std::isfinite(offset)) {
            return 0.0f;
        }
        return std::clamp(offset, 0.0f, 1.0f);
    }

    static void clearCollisionLog(PlayerObject* player) {
        if (!player) {
            return;
        }

        player->m_collisionLogTop->removeAllObjects();
        player->m_collisionLogBottom->removeAllObjects();
        player->m_collisionLogLeft->removeAllObjects();
        player->m_collisionLogRight->removeAllObjects();
        player->m_lastCollisionLeft = -1;
        player->m_lastCollisionRight = -1;
        player->m_lastCollisionBottom = -1;
        player->m_lastCollisionTop = -1;
    }

    static bool shouldSplitQueuedSubsteps(PlayerObject* player) {
        if (!player) {
            return false;
        }

        return player->m_isOnGround
            || player->m_touchingRings->count() > 0
            || player->m_isDashing
            || player->m_isDart
            || player->m_isBird
            || player->m_isShip
            || player->m_isSwing;
    }

    static bool shouldBlockDeferredCapture(
        GJBaseGameLayer* layer,
        ReplayEngine* engine,
        int button,
        bool hold,
        bool player2,
        int tick
    ) {
        if (engine->skipTickIndex != -1 && hold) {
            return true;
        }

        if (button != 1) {
            return false;
        }

        int playerIndex = deferredSlotForPlayer(layer->m_levelSettings->m_twoPlayerMode, player2);
        bool delayedInputQueued = engine->deferredInputTick[playerIndex] != -1;
        bool delayedReleaseQueued = engine->deferredReleaseA[playerIndex] != -1;

        if (!delayedInputQueued && engine->skipActionTick != tick && !delayedReleaseQueued) {
            return false;
        }

        bool resumedPlayer2 = keepSecondPlayerSlot(layer) ? player2 : false;
        int resumedSlot = resumedPlayer2 ? 1 : 0;

        if (hold && engine->isCheckpointHoldResumed(resumedSlot, button)) {
            return true;
        }
        if (engine->skipActionTick >= tick) {
            engine->deferredInputTick[playerIndex] = engine->skipActionTick + 1;
        }
        return true;
    }

    static void resetQueuedSubstepVisualState() {
        g_substepP1Split = false;
        g_substepP2Split = false;
        g_substepRotationDelta = 0.0f;
    }
}

void ReplayEngine::dispatchQueuedSubstepInput(PlayLayer* playLayer, QueuedSubstepInput const& input) {
    if (!playLayer) {
        return;
    }

    bool player2 = input.player2;
    if (input.macroInput && resolveFlipState()) {
        player2 = !player2;
    }

    if (input.macroInput) {
        bool previousSubstepMacroDispatch = substepMacroDispatch;
        substepMacroDispatch = true;
        playLayer->GJBaseGameLayer::handleButton(input.pressed, input.button, player2);
        substepMacroDispatch = previousSubstepMacroDispatch;
        if (!simulatingPath) {
            triggerAudio(player2, input.button, input.pressed);
        }
        return;
    }

    auto* ac = Autoclicker::get();
    bool previousAutoclickerInput = ac->isAutoclickerInput;
    float previousCaptureOffset = substepCaptureOffset;
    ac->isAutoclickerInput = true;
    substepCaptureOffset = input.offset;
    playLayer->handleButton(input.pressed, input.button, player2);
    substepCaptureOffset = previousCaptureOffset;
    ac->isAutoclickerInput = previousAutoclickerInput;
}

void ReplayEngine::simulateQueuedSubstepTick(
    PlayLayer* playLayer,
    PlayerObject* player1,
    PlayerObject* player2,
    float stepDelta,
    std::deque<QueuedSubstepSegment>& segments,
    std::function<void(QueuedSubstepInput const&)> dispatchOverride
) {
    if (!playLayer || !player1 || segments.empty()) {
        return;
    }

    bool isDual = player2 && (playLayer->m_gameState.m_isDualMode || playLayer->m_levelSettings->m_twoPlayerMode);

    g_substepP1Position = player1->getPosition();
    g_substepP2Position = player2 ? player2->getPosition() : cocos2d::CCPoint { 0.f, 0.f };
    g_substepP1Split = shouldSplitQueuedSubsteps(player1);
    g_substepP2Split = isDual && shouldSplitQueuedSubsteps(player2);

    substepMidStep = true;

    while (!segments.empty()) {
        auto segment = segments.front();
        segments.pop_front();

        if (segment.hasActionAfter && !segment.endStep && segment.deltaFactor <= 0.0f) {
            if (dispatchOverride) {
                dispatchOverride(segment.actionAfter);
            } else {
                dispatchQueuedSubstepInput(playLayer, segment.actionAfter);
            }
            continue;
        }

        float substepDelta = stepDelta * std::max(0.0f, segment.deltaFactor);
        g_substepRotationDelta = substepDelta;

        if (g_substepP1Split) {
            player1->PlayerObject::update(substepDelta);
            if (!segment.endStep) {
                if (!player1->m_isOnSlope || player1->m_isDart) {
                    playLayer->checkCollisions(player1, 0.0f, true);
                } else {
                    playLayer->checkCollisions(player1, stepDelta, true);
                }
                player1->PlayerObject::updateRotation(substepDelta);
                clearCollisionLog(player1);
            }
        } else if (segment.endStep) {
            player1->PlayerObject::update(stepDelta);
        }

        if (isDual && player2) {
            if (g_substepP2Split) {
                player2->PlayerObject::update(substepDelta);
                if (!segment.endStep) {
                    if (!player2->m_isOnSlope || player2->m_isDart) {
                        playLayer->checkCollisions(player2, 0.0f, true);
                    } else {
                        playLayer->checkCollisions(player2, stepDelta, true);
                    }
                    player2->PlayerObject::updateRotation(substepDelta);
                    clearCollisionLog(player2);
                }
            } else if (segment.endStep) {
                player2->PlayerObject::update(stepDelta);
            }
        }

        if (segment.hasActionAfter) {
            if (dispatchOverride) {
                dispatchOverride(segment.actionAfter);
            } else {
                dispatchQueuedSubstepInput(playLayer, segment.actionAfter);
            }
        }

        if (segment.endStep) {
            break;
        }
    }

    substepMidStep = false;
    substepCaptureOffset = -1.0f;
}

class $modify(MacroEngineBaseLayer, GJBaseGameLayer) {
    struct Fields {
        bool macroInput = false;
        // Playback commands that must be visible in m_queuedButtons for the
        // readers that consult the QUEUE instead of the hold map (dash-ring
        // activation). They are collected at dispatch time and re-pushed right
        // before this step's vanilla drain, because the drain also has to drop
        // the user's own clicks - see processQueuedButtons.
        std::vector<PlayerButtonCommand> queueMirror;
        // True while vanilla is draining the queue. The input-only path
        // dispatches AFTER the drain, so a queued copy there would be applied a
        // step late; those events are deliberately not mirrored.
        bool drainingQueue = false;
    };

    bool resolvePlaybackPlayer2(bool player2) {
        auto* engine = ReplayEngine::get();
        // SK inputs are captured AT handleButton and store the raw param the
        // game itself received (probe-proven: with the control-flip setting
        // the param maps to the opposite player object inside handleButton).
        // Replaying that param verbatim reproduces the exact object routing;
        // the legacy UI-level pre-flip is only correct for GDR-era sources.
        if (!engine || !engine->skMode) {
            if (resolveFlipState()) {
                player2 = !player2;
            }
        }

        return player2 && keepSecondPlayerSlot(this);
    }

    bool shouldUseInputOnlySkPlayback() {
        auto* engine = ReplayEngine::get();

        return engine &&
            engine->engineMode == MODE_EXECUTE &&
            engine->skMode &&
            engine->activeSk &&
            !engine->activeSk->inputs.empty() &&
            engine->activeSk->anchors.empty() &&
            ReplayEngine::runtimeAccuracyModeFor(engine->activeSk->accuracyMode) != AccuracyMode::CBS;
    }

    bool shouldUseInputOnlyGDRPlayback() {
        auto* engine = ReplayEngine::get();
        return engine &&
            engine->engineMode == MODE_EXECUTE &&
            !engine->skMode &&
            engine->activeMacro &&
            !engine->activeMacro->inputs.empty() &&
            engine->activeMacro->anchors.empty() &&
            ReplayEngine::runtimeAccuracyModeFor(engine->activeMacro->accuracyMode) != AccuracyMode::CBS;
    }

    void processInputOnly(bool shouldUse, std::function<void(int)> dispatch) {
        if (!shouldUse) return;
        int progress = static_cast<int>(m_gameState.m_currentProgress / 2);
        bool prev = m_fields->macroInput;
        m_fields->macroInput = true;
        dispatch(progress);
        m_fields->macroInput = prev;
    }

    void dispatchImmediatePlaybackAction(int tick, int button, bool down, bool player2,
                                         float stepOffset = 0.0f) {
        auto* engine = ReplayEngine::get();
        if (tick == engine->respawnTickIndex) {
            return;
        }

        const bool rawPlayer2 = player2;
        player2 = resolvePlaybackPlayer2(player2);

        // Determine current accuracy mode
        AccuracyMode accMode = engine->skMode
            ? (engine->activeSk ? engine->activeSk->accuracyMode : engine->selectedAccuracyMode)
            : (engine->activeMacro ? engine->activeMacro->accuracyMode : engine->selectedAccuracyMode);

        // Vanilla / COS / CBS: direct handleButton + queue mirror
        if (static_cast<int>(tick) < gTraceWindowEnd) {
            auto* trcPlayLayer = PlayLayer::get();
            skkbot2::log::info("[TRC IN ] t={} button={} pressed={} p2param={} p2resolved={} posBefore=({:.1f},{:.1f})",
                tick, button, down ? 1 : 0, rawPlayer2 ? 1 : 0, player2 ? 1 : 0,
                trcPlayLayer && trcPlayLayer->m_player1 ? trcPlayLayer->m_player1->getPositionX() : 0.f,
                trcPlayLayer && trcPlayLayer->m_player1 ? trcPlayLayer->m_player1->getPositionY() : 0.f);
        } else {
            skkbot2::log::debug("play input: tick={} button={} pressed={} p2param={} p2resolved={}",
                tick, button, down ? 1 : 0, rawPlayer2 ? 1 : 0, player2 ? 1 : 0);
        }
        m_fields->macroInput = true;
        GJBaseGameLayer::handleButton(down, button, player2);
        m_fields->macroInput = false;
        engine->triggerAudio(player2, button, down);
        // Queue mirror: real touches travel UILayer -> m_queuedButtons ->
        // processQueuedButtons, and some game-side readers (dash-ring
        // activation) consult the QUEUE, not just the hold map. The direct
        // call above sets the map; this copy rides the vanilla queue so
        // queue-readers see the same events as in the recording. Vanilla
        // consumption routes back through our EXECUTE hook and is swallowed
        // there (map already correct), so this is side-effect free.
        //
        // The copy is parked in queueMirror and flushed by processQueuedButtons
        // immediately before the drain: pushing it here instead would let the
        // step's own "drop the user's clicks" pass erase it (that was the
        // bug - the mirror never reached a single reader).
        if (button >= 1 && button <= 3 && !m_fields->drainingQueue) {
            PlayerButtonCommand command = {};
            command.m_button = static_cast<PlayerButton>(button);
            command.m_isPush = down;
            command.m_isPlayer2 = player2;
            command.m_timestamp = m_timestamp;
            m_fields->queueMirror.push_back(command);
        }
    }

    void dispatchQueuedSubstepInput(QueuedSubstepInput const& input) {
        auto* engine = ReplayEngine::get();
        auto* playLayer = PlayLayer::get();
        if (!engine || !playLayer) {
            return;
        }
        engine->dispatchQueuedSubstepInput(playLayer, input);
    }

    void queueAutoclickerInputs() {
        auto* engine = ReplayEngine::get();
        auto* ac = Autoclicker::get();
        if (!engine || !ac->enabled || engine->engineMode == MODE_EXECUTE) {
            return;
        }

        std::vector<QueuedSubstepInput> queued;
        if (ac->isTimedMode()) {
            if (engine->engineMode == MODE_CAPTURE) {
                return;
            }

            auto actions = ac->buildTimedTickActions(engine->runtimeTickRate());
            queued.reserve(actions.size());
            for (auto const& action : actions) {
                queued.push_back({
                    clampSubstepOffset(action.offset),
                    1,
                    action.player2,
                    action.pressed,
                    false
                });
            }
        } else {
            auto actions = ac->processTick();
            if (actions.p1Fire) {
                queued.push_back({ 0.0f, 1, false, actions.p1Press, false });
            }
            if (actions.p2Fire) {
                queued.push_back({ 0.0f, 1, true, actions.p2Press, false });
            }
        }

        engine->armQueuedSubstepTick(std::move(queued));
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto* engine = ReplayEngine::get();
        auto* playLayer = PlayLayer::get();

        // Drive the step clock for a PlayLayer attempt OR an editor playtest
        // (LevelEditorLayer playtest funnels through this base-layer update).
        if (!playLayer && !skk::gameplay::isEditorPlaytest()) {
            skkbot2::step_trace::finish();
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        // Editor playtest: align the freshly spawned player to the recorded
        // route exactly once (loop-guarded by EXECUTE && !startPosActive). Must
        // run BEFORE the step-clock increment so the baseline matches the
        // reset-moment semantics applyReplayResetState relies on.
        if (skk::gameplay::isEditorPlaytest() &&
            engine->engineMode == MODE_EXECUTE && !engine->startPosActive) {
            skk::editor::attemptPlaytestArm(this, engine);
        }

        // Step-grid clock boundary: every processCommands call IS one physics
        // sub-step in a PlayLayer (probe-proven: one call per forced step,
        // half/last are not pair flags, and m_currentStep stays 0 in this
        // batched pipeline). An editor playtest is different: GD funnels the
        // same calls at ~180/s with dt=1/180, so one call is only ~1.33 macro
        // ticks. Advance by dt*tickRate (keeping the fractional remainder) so the
        // playback grid stays locked to the level's game time instead of
        // drifting ~25% slow (the "speedhack-like" slowdown).
        if (engine->stepClock) {
            if (skk::gameplay::isEditorPlaytest()) {
                engine->stepPairFrac += static_cast<double>(dt) * engine->runtimeTickRate();
                int whole = static_cast<int>(engine->stepPairFrac);
                engine->stepPairFrac -= static_cast<double>(whole);
                engine->stepPairCount += whole;
            } else {
                ++engine->stepPairCount;
            }
        }

        // EDITOR PLAYTEST CADENCE PROBE: in the editor the physics timing
        // machinery (SpeedControlScheduler / PhysicsControlLayer / getModifiedDelta)
        // is PlayLayer-gated and stays vanilla, so GD funnels processCommands at
        // ~180/s with dt=1/180 instead of 240/s with dt=1/240. One call is NOT
        // one macro tick there: the step clock now advances by dt*tickRate (see
        // the compensation above). This probe verifies the grid stays locked to
        // the level's game time - a residual cadence mismatch would still show
        // up as a "speedhack-like" slowdown of the whole run.
        if (skk::gameplay::isEditorPlaytest()) {
            static double sLast = 0.0;
            static int sCalls = 0;
            static float sAccumDt = 0.0f;
            static double sAccumLt = 0.0;
            static int sLastTick = 0;
            if (sCalls == 0) {
                sLast = ReplayEngine::wallClockSeconds();
                sLastTick = engine->stepPairCount;
            }
            ++sCalls;
            sAccumDt += dt;
            sAccumLt += m_gameState.m_levelTime;
            if (sCalls >= 240) {
                const double now = ReplayEngine::wallClockSeconds();
                const double wall = now - sLast;
                const double pps = wall > 0.0 ? static_cast<double>(sCalls) / wall : 0.0;
                const int tickDelta = engine->stepPairCount - sLastTick;
                skkbot2::log::info(
                    "EDITOR cadence: {} calls in {:.3f}s = {:.1f} processCommands/s | "
                    "sum(dt)={:.4f}s avgDt={:.7f} | tickDelta={} ({} ticks/s) lt={:.4f}",
                    sCalls, wall, pps, sAccumDt,
                    sAccumDt / static_cast<float>(sCalls),
                    tickDelta, wall > 0.0 ? static_cast<double>(tickDelta) / wall : 0.0,
                    sAccumLt);
                sCalls = 0;
                sAccumDt = 0.0f;
                sAccumLt = 0.0;
            }
        }

        // Substep-grid probe: how GD interleaves half/last processCommands
        // calls against levelTime and m_currentStep during capture and
        // playback. Budget mode (first N calls of a session) plus a permanent
        // window around the last playback death (probeAroundTick) so a REplay
        // and a fresh RECORD of the same span can be diffed line by line.
        const bool activeMode = engine->engineMode == MODE_CAPTURE ||
                                engine->engineMode == MODE_EXECUTE;
        // BUG-102 v3 hotfix: SKC record/play NEVER touch the SKK engineMode -
        // the SKC bot is a sibling engine (State::Record/Playback on its own
        // State), so the 04.11 sessions ran with SKK MODE_DISABLED, the probe
        // never armed and the 11:29/11:35 logs have no probe lines at all.
        const bool skcActive = skk::skc::Bot::get().isRecording() ||
                               skk::skc::Bot::get().isPlaying();
        const bool anyActive = activeMode || skcActive;
        const bool budgetProbe = engine->substepProbe > 0 && activeMode;
        // BUG-102 v3: arm the window probe on an arbitrary tick from saved
        // settings, not only around a playback death. The 04.11 divergences
        // are LIVE at a deterministic tick (rec flips the portal f=417, play
        // f=421) with bit-identical coordinates AND bit-identical levelTime
        // (verified from trace bins), so the probe window must answer WHICH
        // channel differs there: the half/last grouping phase of the feed
        // (vanilla portal contact quantizes against half-ticks) or the
        // moving-object timeline. Set `skc-probe-tick` in the mod's saved
        // values (+= mod.json extras) to the tick of interest.
        if (engine->probeAroundTick < 0 && engine->probeAroundTicks.empty() && anyActive) {
            static bool sProbeTickRead = false;
            if (!sProbeTickRead) {
                sProbeTickRead = true;
                // Щедро: несколько окон сразу - "skc-probe-ticks" = "416,516,1237".
                const std::string list = geode::Mod::get()->getSavedValue<std::string>(
                    "skc-probe-ticks", "");
                std::string cur;
                for (size_t i = 0; i <= list.size(); ++i) {
                    if (i == list.size() || list[i] == ',' || list[i] == ';') {
                        if (!cur.empty()) {
                            try {
                                engine->probeAroundTicks.push_back(std::stoi(cur));
                            } catch (...) {}
                            cur.clear();
                        }
                    } else if (list[i] >= '0' && list[i] <= '9') {
                        cur.push_back(list[i]);
                    }
                }
                const int64_t saved = geode::Mod::get()->getSavedValue<int64_t>(
                    "skc-probe-tick", -1);
                if (saved >= 0) {
                    engine->probeAroundTick = static_cast<int>(saved);
                    engine->probeAroundTicks.push_back(static_cast<int>(saved));
                }
                if (!engine->probeAroundTicks.empty()) {
                    std::string joined;
                    for (int t : engine->probeAroundTicks) {
                        if (!joined.empty()) joined += ",";
                        joined += std::to_string(t);
                    }
                    skkbot2::log::info(
                        "[SKC] window probe armed at ticks [{}] (skc-probe-ticks/skc-probe-tick)",
                        joined);
                }
            }
        }
        bool windowProbe = false;
        int probeTick = 0;
        if ((!engine->probeAroundTicks.empty() || engine->probeAroundTick >= 0) && anyActive) {
            probeTick = tick_util::current(this, engine);
            for (int center : engine->probeAroundTicks) {
                if (std::abs(probeTick - center) <= 24) {
                    windowProbe = true;
                    break;
                }
            }
        }
        if (budgetProbe) {
            --engine->substepProbe;
        }
        // BUG-102 v3: позиция подшага внутри кадра дисплея - счётчик
        // сбрасывается в update-хуке раз в кадр (skc_batch_probe.hpp).
        // Инкремент на КАЖДЫЙ вызов processCommands: вне probe-окна счётчик
        // тоже должен быть жив - gravity-flip строки печатают batch в любой
        // точке уровня, а окно не всегда накрывает флип (1057 вне 476-524).
        skkbot2::skcprobe::noteStep();
        if (budgetProbe || windowProbe) {
            // info: debug lines never reach the tiny flush buffer, and the
            // window comparison (rec session vs play session on the same
            // tick) is exactly what the diverging level needs to answer.
            // Label: SKC keeps its own state enum (SKK engineMode stays
            // MODE_DISABLED in SKC-only sessions), so pick per engine.
            const char* modeLabel =
                skcActive ? (skk::skc::Bot::get().isRecording() ? "rec-skc"
                                                               : "play-skc")
                          : (engine->engineMode == MODE_CAPTURE ? "rec-skk"
                                                                : "play-skk");
            // BUG-102 v3: m_objectID - это ТИП объекта, инстансов 2926
            // в уровне много (сессия 14:35: кэш из portal-see поймал
            // чужой инстанс в (5042,3165) scale 0.76 rot 90, а
            // активный портал оседает в (2492,3150) scale 0.89
            // rot 270). Один раз собираем ВСЕ инстансы и печатаем
            // ближайший к игроку - его rect и есть гейт флипа.
            static std::vector<GameObject*> s_portalInstances;
            static bool s_portalsInit = false;
            if (!s_portalsInit) {
                s_portalsInit = true;
                if (m_objects) {
                    for (int i = 0; i < m_objects->count(); ++i) {
                        auto* obj = static_cast<GameObject*>(m_objects->objectAtIndex(i));
                        if (obj && obj->m_objectID == 2926) s_portalInstances.push_back(obj);
                    }
                }
                skkbot2::log::info("[SKC] portal-2926 instances collected: n={}",
                                   s_portalInstances.size());
            }
            GameObject* probePortal = nullptr;
            {
                const float px = m_player1 ? m_player1->getPositionX() : 0.0f;
                float bestDist = 1e9f;
                for (auto* inst : s_portalInstances) {
                    const float d = std::fabs(inst->getPositionX() - px);
                    if (d < bestDist) { bestDist = d; probePortal = inst; }
                }
            }
            skkbot2::log::info(
                "substep probe{}: mode={} half={} last={} dt={:.7f} lt={:.7f} "
                "ltTicks={:.4f} step={} tick={} batch={} fdt={:.7f} "
                "vy={:.4f} nx={:.3f} ny={:.3f} mx={:.3f} my={:.3f} "
                "gx={:.3f} gy={:.3f} rminy={:.3f} rmaxy={:.3f} "
                "sfg={} shh={} upy={:.3f} pY={:.3f} maxPY={:.3f} minPY={:.3f} "
                "portal={}:px={:.3f} py={:.3f} ps={:.4f} pr={:.4f}",
                windowProbe ? "[win]" : "",
                modeLabel,
                isHalfTick, isLastTick, dt, m_gameState.m_levelTime,
                m_gameState.m_levelTime * engine->runtimeTickRate(), m_currentStep, probeTick,
                skkbot2::skcprobe::g_batchStep, skkbot2::skcprobe::g_frameDt,
                m_player1 ? static_cast<double>(m_player1->m_yVelocity) : -1.0,
                m_player1 ? m_player1->getPosition().x : -1.0,
                m_player1 ? m_player1->getPosition().y : -1.0,
                m_player1 ? m_player1->m_position.x : -1.0,
                m_player1 ? m_player1->m_position.y : -1.0,
                m_player1 ? m_player1->m_positionX : -1.0,
                m_player1 ? m_player1->m_positionY : -1.0,
                m_player1 ? m_player1->getObjectRect().getMinY() : -1.0,
                m_player1 ? m_player1->getObjectRect().getMaxY() : -1.0,
                m_player1 ? m_player1->m_stateFlipGravity : -1,
                m_player1 ? m_player1->m_stateHitHead : -1,
                static_cast<double>(m_gameState.m_unkUint17),
                static_cast<double>(m_gameState.m_portalY),
                static_cast<double>(getMaxPortalY()),
                static_cast<double>(getMinPortalY()),
                probePortal ? probePortal->m_objectID : 0,
                probePortal ? static_cast<double>(probePortal->getPositionX()) : -1.0,
                probePortal ? static_cast<double>(probePortal->getPositionY()) : -1.0,
                probePortal ? static_cast<double>(probePortal->getScale()) : -1.0,
                probePortal ? static_cast<double>(probePortal->getRotation()) : -1.0);
            // Mover timeline probe: hash of every moving object's
            // id/pos/rot. Movers advance with the cocos action manager - if
            // that path runs on raw frame time instead of the fixed grid,
            // record and playback diverge at the SAME player position
            // (drift 0.4px) yet die to an object that was 2px elsewhere.
            // The first tick whose hash differs localizes the channel.
            if (auto* movers = m_objectsToMove) {
                uint64_t moversHash = movers->count();
                long long firstId = -1;
                double fx = 0.0, fy = 0.0;
                for (int i = 0; i < movers->count() && i < 96; ++i) {
                    auto* obj = static_cast<GameObject*>(movers->objectAtIndex(i));
                    if (!obj) continue;
                    const long long id = obj->m_objectID;
                    if (i == 0) {
                        firstId = id;
                        fx = obj->getPositionX();
                        fy = obj->getPositionY();
                    }
                    uint64_t mix = static_cast<uint64_t>(id) ^
                        (static_cast<uint64_t>(std::llround(obj->getPositionX() * 4.0)) << 20) ^
                        (static_cast<uint64_t>(std::llround(obj->getPositionY() * 4.0)) << 40) ^
                        (static_cast<uint64_t>(std::llround(obj->getRotation() * 4.0)) << 56);
                    moversHash = moversHash * 1000003ULL + mix;
                }
                skkbot2::log::info(
                    "movers probe{}: tick={} n={} h={:016X} first={}@({:.2f},{:.2f})",
                    windowProbe ? "[win]" : "", probeTick, movers->count(), moversHash,
                    firstId, fx, fy);
            }
        }

        refreshRngState();

        int tick = tick_util::current(this, engine);
        double processSliceTimestamp = m_timestamp + static_cast<double>(std::max(0.0f, dt));

        // Practice-restart boundary reseeds: spliced recordings re-armed the
        // LCG at each checkpoint respawn (arm family + captured primary). When
        // the live tick crosses a stored boundary, reproduce the same reseed so
        // the segment that follows keeps its recorded RNG world.
        if (engine->engineMode == MODE_EXECUTE && engine->activeSk) {
            auto const& boundaries = engine->activeSk->rngBoundaries;
            while (engine->nextBoundaryIndex < boundaries.size()) {
                auto const& boundary = boundaries[engine->nextBoundaryIndex];
                if (boundary.tick > tick) {
                    break;
                }
                uint32_t seedSalt = (engine->activeSk && engine->activeSk->rngSeed != 0)
                    ? engine->activeSk->rngSeed : engine->rngSeedVal;
                skk::applyRngBoundaryReseed(boundary.primary, boundary.teleport,
                                            seedSalt, boundary.progress);
                skkbot2::log::info("rng boundary applied: tick={} progress={} live(0x{:X},0x{:X})",
                    boundary.tick, boundary.progress,
                    boundary.primary, boundary.teleport);
                ++engine->nextBoundaryIndex;
            }
        }
        if (engine->shouldResetAfterPersistencePlaybackDeath(playLayer)) {
            if (m_levelSettings->m_platformerMode) {
                return playLayer->resetLevelFromStart();
            }
            return playLayer->resetLevel();
        }

        AccuracyMode playbackAccuracy = ReplayEngine::runtimeAccuracyModeFor(engine->activeMacroAccuracyMode());
        bool timedPlayback = engine->engineMode == MODE_EXECUTE && usesTimedAccuracy(playbackAccuracy);
        bool newTick = tick != engine->lastTickIndex;

        if (newTick) {
            engine->prevTickStartTimestamp = engine->tickStartTimestamp;
            engine->prevTickStartWallTime = engine->tickStartWallTime;
            engine->tickStartStep = m_currentStep;
            engine->tickStartTimestamp = m_timestamp;
            engine->tickStartWallTime = ReplayEngine::wallClockSeconds();
            engine->clearQueuedSubstepState();
            queueAutoclickerInputs();
        }

        int stepDelta = std::max(0, m_currentStep - engine->tickStartStep);

        // Reset the playback jump tracer at the start of every attempt so a
        // cross-attempt position delta is never misread as a visible hop.
        if (newTick && tick <= 1) {
            gFrameLastTick = -1;
            gTraceWindowEnd = -1;
        }

        auto computeAndGuard = [&]() -> std::optional<int> {
            if (engine->engineMode != MODE_DISABLED) {
                if (tick > 2 && engine->initialRun && engine->hasMacro()) {
                    engine->initialRun = false;
                    engine->suppressInitialRunReset = false;
                }

                if (skk::replay_timing::shouldSkipRepeatedProcessSlice(
                    timedPlayback,
                    engine->lastTickIndex,
                    tick,
                    engine->hasMacro(),
                    engine->lastStepDelta,
                    stepDelta,
                    engine->lastProcessSliceTimestamp,
                    processSliceTimestamp
                )) {
                    // Repeated-slice suppression is exactly the fps-vs-grid
                    // boundary; make it visible instead of silently dropping.
                    if (++engine->processSliceSkips % 240 == 1) {
                        skkbot2::log::info("process-slice skip #{}: tick={} stepDelta={} ts={:.6f} lastTs={:.6f}",
                            engine->processSliceSkips, tick, stepDelta,
                            processSliceTimestamp, engine->lastProcessSliceTimestamp);
                    }
                    return std::nullopt;
                }
            }
            return tick;
        };

        auto handleRecordingPhase = [&](int t) {
            if (!newTick) return;
            if (engine->hasMacro() && engine->levelRestarting && !m_levelEndAnimationStarted) {
                // Don't call resetLevel() here — GD handles the restart.
                // Calling resetLevel() again creates a death loop.
                return;
            }
            captureTick(t);
        };

        auto handlePlaybackPhase = [&](int t) {
            if (newTick && engine->hasMacro() && engine->levelRestarting && !m_levelEndAnimationStarted) {
                // GD is already handling the reset — don't call resetLevel()
                // again (that creates a death loop: reset → death → reset).
                // Just return and let GD finish its restart sequence.
                return;
            }
            executeTick(t, stepDelta);
        };

        auto tickOpt = computeAndGuard();
        if (!tickOpt) {
            // MODE_DISABLED: xdNako-style per-frame hold sync.
            // After practice checkpoint restore, vanilla GD calls
            // releaseAllButtons() which clears m_holdingButtons AND internal
            // flags (m_jumping, m_hasJustHeld, etc). Direct map writes are
            // insufficient — we must call pushButton/releaseButton through
            // the player's API to properly set all internal state.
            auto syncPhysicalHolds = [&, layer = static_cast<GJBaseGameLayer*>(playLayer)](PlayerObject* player) {
                if (!player) return;
#ifdef GEODE_IS_WINDOWS
                auto& hb = player->m_holdingButtons;

                bool isP2 = (player == m_player2);
                bool blocked = player->m_controlsDisabled;
                auto mode = currentPhysicalHoldMode(layer);

                // Jump: mapping depends on twoPlayer/platformer/dual (see physical_keys.hpp)
                bool jumpHeld = physicalHoldActive(mode, 1, isP2, blocked);
                bool jumpMap = hb.count(1) && hb[1];
                if (jumpHeld && !jumpMap) {
                    player->pushButton(PlayerButton::Jump);
                } else if (!jumpHeld && jumpMap) {
                    player->releaseButton(PlayerButton::Jump);
                }

                // Left
                bool leftHeld = physicalHoldActive(mode, 2, isP2, blocked);
                bool leftMap = hb.count(2) && hb[2];
                if (leftHeld && !leftMap) {
                    player->pushButton(PlayerButton::Left);
                } else if (!leftHeld && leftMap) {
                    player->releaseButton(PlayerButton::Left);
                }

                // Right
                bool rightHeld = physicalHoldActive(mode, 3, isP2, blocked);
                bool rightMap = hb.count(3) && hb[3];
                if (rightHeld && !rightMap) {
                    player->pushButton(PlayerButton::Right);
                } else if (!rightHeld && rightMap) {
                    player->releaseButton(PlayerButton::Right);
                }

                // MODE_CAPTURE: route both press and release through hooked
                // handleButton so the macro stream records them. pushButton/
                // releaseButton only set internal state — they do NOT call
                // handleButton, so the recording layer never sees these events.
                if (engine->engineMode == MODE_CAPTURE && engine->hasMacro()) {
                    bool p2 = handleButtonPlayer2Param(isP2, controlFlipActive(this));
                    if (jumpHeld && !jumpMap) {
                        g_perFramePressSync = true;
                        layer->handleButton(true, 1, p2);
                        g_perFramePressSync = false;
                    } else if (!jumpHeld && jumpMap) {
                        g_perFrameReleaseSync = true;
                        layer->handleButton(false, 1, p2);
                        g_perFrameReleaseSync = false;
                    }
                    if (leftHeld && !leftMap) {
                        g_perFramePressSync = true;
                        layer->handleButton(true, 2, p2);
                        g_perFramePressSync = false;
                    } else if (!leftHeld && leftMap) {
                        g_perFrameReleaseSync = true;
                        layer->handleButton(false, 2, p2);
                        g_perFrameReleaseSync = false;
                    }
                    if (rightHeld && !rightMap) {
                        g_perFramePressSync = true;
                        layer->handleButton(true, 3, p2);
                        g_perFramePressSync = false;
                    } else if (!rightHeld && rightMap) {
                        g_perFrameReleaseSync = true;
                        layer->handleButton(false, 3, p2);
                        g_perFrameReleaseSync = false;
                    }
                }
#endif
            };
            syncPhysicalHolds(m_player1);
            if (m_player2) syncPhysicalHolds(m_player2);

            auto dbgMode = currentPhysicalHoldMode(this);
            const bool dbgP1J = isPhysicalKeyHeld(1, false, dbgMode.twoPlayer, dbgMode.platformer, dbgMode.dual);
            const bool dbgP1L = isPhysicalKeyHeld(2, false, dbgMode.twoPlayer, dbgMode.platformer, dbgMode.dual);
            const bool dbgP1R = isPhysicalKeyHeld(3, false, dbgMode.twoPlayer, dbgMode.platformer, dbgMode.dual);
            const bool dbgP2J = isPhysicalKeyHeld(1, true, dbgMode.twoPlayer, dbgMode.platformer, dbgMode.dual);
            const bool dbgP2L = isPhysicalKeyHeld(2, true, dbgMode.twoPlayer, dbgMode.platformer, dbgMode.dual);
            const bool dbgP2R = isPhysicalKeyHeld(3, true, dbgMode.twoPlayer, dbgMode.platformer, dbgMode.dual);
            if (dbgP1J || dbgP1L || dbgP1R || dbgP2J || dbgP2L || dbgP2R) {
                skkbot2::log::debug("holdSync MODE_DISABLED: twoPlayer={} platformer={} dual={} p1Jump={} p1Left={} p1Right={} p2Jump={} p2Left={} p2Right={}",
                    dbgMode.twoPlayer, dbgMode.platformer, dbgMode.dual,
                    dbgP1J, dbgP1L, dbgP1R, dbgP2J, dbgP2L, dbgP2R);
            }

            skkbot2::step_trace::finish();
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        // BUG-06: phase timing on the playback hot path. Logs whenever the
        // bot's own per-tick work (dispatch + pre-phy pin + GD physics +
        // post-step reconcile/logs) exceeds 5ms of wall time — the user's
        // "пролаг ГД" on every spider-orb press. Proves whether the stall
        // lives in bot CPU work (inputs/pin/reconcile) or is pure GD/jitter.
        std::chrono::steady_clock::time_point tInputsStart, tGdStart, tPostStart;
        int64_t hotInputsUs = 0, hotPinUs = 0, hotGdUs = 0, hotPostUs = 0;

        if (engine->engineMode == MODE_EXECUTE && newTick && tick >= 0) {
            // BUG-06: build the level-wide portal list cache on the very first
            // ticks of the run, NOT on the first seam tick (~40ms of
            // dynamic_cast on 141k objects would otherwise hitch the start-warp
            // seam as a visible "пролаг ГД" on every attempt).
            if (tick <= 8) {
                prePhyWarmPortalCache(this);
            }
            tInputsStart = std::chrono::steady_clock::now();
            dispatchPlaybackInputsPre(tick, stepDelta);
            hotInputsUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tInputsStart).count();
        }

        // NOTE: The old force-reconcile of anchor[0] on every tick was wrong —
        // it applied tick 289's state at tick 0..288, teleporting P2 to the
        // wrong position and causing void death on file-loaded macros.
        // Position pinning is now handled by prePhyPositionPin + reconcilePerFrames.
        // Anchor structural reconciliation (modeOnly) still runs in executeTick
        // via reconcileAnchors when the tick reaches each anchor.

        // Pre-physics position pin: snap to previous tick's end state so
        // collision detection (portals, orbs, pads) evaluates at correct positions.
        // Only on newTick (first sub-step) — subsequent sub-steps must let GD
        // advance the position naturally for correct trigger detection.
        if (newTick && engine->engineMode == MODE_EXECUTE && engine->skMode && engine->perFrameReconciliation) {
            auto tPinStart = std::chrono::steady_clock::now();
            prePhyPositionPin(tick + engine->tickOffset);
            hotPinUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tPinStart).count();
        }

        tGdStart = std::chrono::steady_clock::now();
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        // Compare raw live physics before executeTick can reconcile positions,
        // velocity, or other recorded state. This detector never writes state.
        if (skkbot2::step_trace::enabled()) {
            skkbot2::step_trace::checkDrift(
                this, tick + engine->tickOffset,
                engine->engineMode == MODE_CAPTURE,
                engine->engineMode == MODE_EXECUTE, false);
        }

        engine->lastTickIndex = tick;
        engine->lastStepDelta = stepDelta;
        engine->lastProcessSliceTimestamp = processSliceTimestamp;

        if (engine->engineMode == MODE_DISABLED) {
            // Armed-waiting playback takeover: PLAY was pressed mid-run, so the
            // bot does NOT reset or teleport the player - it waits until the
            // live position is actually covered by the recording and arms in
            // place on that tick.
            if (newTick) {
                engine->tryLivePlaybackTakeover(this);
            }
            return;
        }

        if (engine->engineMode == MODE_EXECUTE && newTick) {
            hotGdUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tGdStart).count();
        }

        if (engine->engineMode == MODE_CAPTURE) {
            handleRecordingPhase(*tickOpt);
        } else if (engine->engineMode == MODE_EXECUTE) {
            if (newTick) {
                tPostStart = std::chrono::steady_clock::now();
            }
            handlePlaybackPhase(*tickOpt);
            if (newTick) {
                hotPostUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tPostStart).count();
                auto tEnd = std::chrono::steady_clock::now();
                int64_t totalUs = std::chrono::duration_cast<std::chrono::microseconds>(tEnd - tInputsStart).count();
                if (totalUs > 5000) {
                    skkbot2::log::info(
                        "PLAY_HOT tick={} inputs={}us pin={}us gd={}us post={}us total={}us",
                        tick, hotInputsUs, hotPinUs, hotGdUs, hotPostUs, totalUs);
                }
            }
        }

        // One row per completed processCommands call, after capture/playback
        // reconciliation. The trace frame is the macro clock; batch records
        // the game-step count consumed for this macro tick (zero for any
        // additional substeps belonging to the same tick).
        skkbot2::step_trace::record(this, tick + engine->tickOffset,
                                    newTick ? static_cast<unsigned>(std::max(stepDelta, 0)) : 0u);

        // === PLAYBACK JUMP TRACER: frame boundary detector ==================
        // isLastTick marks the last physics step of a visual (60Hz) frame, i.e.
        // the position the eye actually renders. A large frame-to-frame delta
        // there IS the reported "резкий скачок". On detection we open a detailed
        // per-tick trace window (reconcilePerFrames logs [TRC RECON] lines).
        if (isLastTick && engine->engineMode == MODE_EXECUTE && m_player1) {
            auto* p = m_player1;
            const float px = p->getPositionX();
            const float py = p->getPositionY();
            const float rot = p->getRotation();
            const double vy = p->m_yVelocity;
            const int effTick = tick + engine->tickOffset;
            if (gFrameLastTick >= 0) {
                const float fdx = px - gFrameLastPx;
                const float fdy = py - gFrameLastPy;
                const float fdist = std::sqrt(fdx * fdx + fdy * fdy);
                ++gFrameCount;
                // Horizontal advance at 240tps over 4 substeps is small for
                // normal speeds, but speed-hacked sections legitimately move
                // faster; only vertical hops + sudden big deltas are the tell.
                if (fdist > kJumpThresholdPx) {
                    // Gap to the recording at the visible frame (if a perFrame
                    // exists for the current effective tick).
                    float recX = 0.f, recY = 0.f;
                    bool haveRec = false;
                    if (auto* pf = engine->activePerFrames()) {
                        // nearest perFrame at/before effTick
                        for (size_t i = 0; i < pf->size(); ++i) {
                            if (pf->at(i).tick <= effTick) { recX = pf->at(i).p1x; recY = pf->at(i).p1y; haveRec = true; }
                            else break;
                        }
                    }
                    const float fdxPrev = gFrameLastPx - recX;
                    const float fdyPrev = gFrameLastPy - recY;
                    skkbot2::log::warn(
                        "[FRAME-JUMP] frame#{} effTick={} dist={:.1f}px "
                        "prevFrame=({:.1f},{:.1f}) thisFrame=({:.1f},{:.1f}) | "
                        "rec=({:.1f},{:.1f}) recGapBefore=({:.1f},{:.1f}) | "
                        "rot {:.1f}->{:.1f} vy {:.2f}->{:.2f} "
                        "(opening {} tick trace window)",
                        gFrameCount, effTick, fdist,
                        gFrameLastPx, gFrameLastPy, px, py,
                        recX, recY, fdxPrev, fdyPrev,
                        gFrameLastRot, rot, gFrameLastVy, vy,
                        kTraceWindowLen);
                    gTraceWindowEnd = effTick + kTraceWindowLen;
                }
            }
            gFrameLastTick = effTick;
            gFrameLastPx = px;
            gFrameLastPy = py;
            gFrameLastRot = rot;
            gFrameLastVy = vy;
        }
        // === RENDERED-DRIFT METER (Этап 0) =====================================
        // playDriftMax reads preP1x inside reconcilePerFrames (pre-position-write):
        // it measures how far vanilla physics wandered BEFORE the recorded
        // position is applied. It is not a visible playback residual. This meter
        // samples the actual post-reconcile state at the visual boundary
        // (isLastTick): (live - last applied recorded frame). A nonzero value
        // here is a residual a user could see; phase-saw artifacts do not appear.
        auto* rdriftFrames = engine->activePerFrames();
        if (isLastTick && rdriftFrames && !rdriftFrames->empty() && m_player1) {
            const float rpx = m_player1->getPositionX();
            const float rpy = m_player1->getPositionY();
            const size_t ri = engine->perFramePlaybackIndex;
            if (ri > 0) {
                const auto& rpf = rdriftFrames->at(ri - 1);
                const float rdx = rpx - rpf.p1x;
                const float rdy = rpy - rpf.p1y;
                const float rD = std::max(std::abs(rdx), std::abs(rdy));
                if (rD > engine->renderDriftMax) {
                    engine->renderDriftMax = rD;
                    engine->renderDriftMaxTick = rpf.tick;
                }
                if (rpf.tick - engine->renderDriftLastLogTick >= kRenderDriftReportEveryTicks) {
                    engine->renderDriftLastLogTick = rpf.tick;
                    const double rvy = m_player1->m_yVelocity;
                    const float rrot = m_player1->getRotation();
                    skkbot2::log::info(
                        "RENDER-drift: tick={} pfIdx={}/{} live=({:.1f},{:.1f}) pf=({:.1f},{:.1f}) dx={:.3f} dy={:.3f} "
                        "dvy={:.3f} drot={:.1f} rot={:.1f}->{:.1f} (max {:.3f}@{})",
                        rpf.tick, ri - 1, rdriftFrames->size(),
                        rpx, rpy, rpf.p1x, rpf.p1y,
                        rdx, rdy, rvy - rpf.p1vy, rrot - rpf.p1rot,
                        rpf.p1rot, rrot, engine->renderDriftMax, engine->renderDriftMaxTick);
                }
            }
        }
        // =====================================================================
    }

#ifdef GEODE_IS_MACOS
    void update(float dt) {
        auto* engine = ReplayEngine::get();
        auto* playLayer = PlayLayer::get();
        if (!playLayer || !engine) {
            return GJBaseGameLayer::update(dt);
        }
        if (engine->engineMode == MODE_DISABLED) {
            engine->tryLivePlaybackTakeover(playLayer);
            return GJBaseGameLayer::update(dt);
        }

        refreshRngState();
        int tick = tick_util::current(this, engine);
        bool newTick = tick != engine->lastTickIndex;

        if (engine->shouldResetAfterPersistencePlaybackDeath(playLayer)) {
            if (m_levelSettings->m_platformerMode) return playLayer->resetLevelFromStart();
            return playLayer->resetLevel();
        }

        if (tick > 2 && engine->initialRun && engine->hasMacro() && !m_levelEndAnimationStarted) {
            engine->initialRun = false;
            engine->suppressInitialRunReset = false;
        }

            if (newTick) {
                engine->prevTickStartTimestamp = engine->tickStartTimestamp;
                engine->prevTickStartWallTime = engine->tickStartWallTime;
                engine->tickStartStep = m_currentStep;
                engine->tickStartTimestamp = m_timestamp;
                engine->tickStartWallTime = ReplayEngine::wallClockSeconds();
                engine->clearQueuedSubstepState();
                queueAutoclickerInputs();
                if (engine->hasMacro() && engine->levelRestarting && !m_levelEndAnimationStarted) {
                    // Don't call resetLevel() again — GD handles the restart.
                    return GJBaseGameLayer::update(dt);
                }
            }

        GJBaseGameLayer::update(dt);

        engine->lastTickIndex = tick;
        engine->lastStepDelta = std::max(0, m_currentStep - engine->tickStartStep);
    }
#endif

    void dispatchDeferredActions(int tick) {
        auto* engine = ReplayEngine::get();

        if (engine->skipTickIndex != -1 && engine->skipTickIndex < tick) {
            engine->skipTickIndex = -1;
        }

        if (engine->skipActionTick != -1 && tick > engine->skipActionTick) {
            engine->skipActionTick = -1;
        }

        DeferredDispatcher dispatcher(this, engine, tick);
        for (int slot = 0; slot < 2; ++slot) {
            dispatcher.tryFireMain(slot);
            dispatcher.tryFireMainRelease(slot);
            if (m_levelSettings->m_platformerMode) {
                dispatcher.tryFireLateralRelease(slot, 0);
                dispatcher.tryFireLateralRelease(slot, 1);
            }
        }
    }

    void captureTick(int tick) {
        auto* engine = ReplayEngine::get();
        auto* playLayer = PlayLayer::get();
        if (!engine->hasMacro() || !playLayer) {
            return;
        }

        // Do not sample trajectory/anchors while either player is in a death
        // animation. The next reset decides whether the preceding live segment
        // is retained (practice checkpoint) or wiped (fresh attempt).
        if (engine->captureDeathPending ||
            (m_player1 && m_player1->m_isDead) || (m_player2 && m_player2->m_isDead)) {
            return;
        }

        dispatchDeferredActions(tick);

        if (engine->skMode) {
            if (!engine->activeSk) {
                return;
            }

            engine->recordSkAnchor(
                tick,
                m_player1,
                m_player2,
                m_levelSettings->m_platformerMode,
                hasDualPlaybackContext(playLayer)
            );
            return;
        }

        if (!engine->anchorReconciliation || !engine->activeMacro || engine->activeMacro->inputs.empty()) {
            return;
        }

        engine->activeMacro->recordAnchor(
            AnchorReconciler::captureAnchor(
                tick,
                playLayer,
                m_player1,
                m_player2,
                engine->captureRngState()
            )
        );
    }

    void dispatchSkInputs(int tick, int effectiveTick, int stepDelta) {
        auto* engine = ReplayEngine::get();
        auto* inputListPtr = engine->activeSkInputs();
        if (!inputListPtr) {
            return;
        }
        auto& inputList = *inputListPtr;
        AccuracyMode accuracyMode = engine->activeSk->accuracyMode;

        static int diagCount = 0;
        ++diagCount;

        while (engine->executeIndex < inputList.size()) {
            auto const& input = inputList[engine->executeIndex];

            if (diagCount <= 30 || diagCount % 240 == 0) {
                skkbot2::log::debug("dispatchSk: effectiveTick={} idx={}/{} input.tick={} input.stepOffset={:.4f} input.flags=0x{:02X} acc={} stepDelta={}",
                    effectiveTick, engine->executeIndex, inputList.size(),
                    input.tick, input.stepOffset, input.flags,
                    static_cast<int>(accuracyMode), stepDelta);
            }

            if (input.tick > effectiveTick) {
                break;
            }

            if (accuracyMode == AccuracyMode::CBS &&
                input.tick == effectiveTick &&
                static_cast<int>(input.stepOffset) > stepDelta) {
                break;
            }

            bool player2 = input.isPlayer2();
            logFirstPlaybackInput(engine, input.tick, effectiveTick,
                                  m_gameState.m_levelTime,
                                  static_cast<size_t>(engine->executeIndex),
                                  inputList.size());
            dispatchImmediatePlaybackAction(tick, input.actionType, input.isPressed(), player2, input.stepOffset);
            ++engine->executeIndex;
        }
    }

    void dispatchInputOnlySkInputs(int progress) {
        auto* engine = ReplayEngine::get();
        auto* inputListPtr = engine->activeSkInputs();
        if (!inputListPtr) {
            return;
        }

        auto& inputList = *inputListPtr;

        while (engine->executeIndex < inputList.size()) {
            auto const& input = inputList[engine->executeIndex];
            if (input.tick > progress) {
                break;
            }

            bool player2 = input.isPlayer2();
            logFirstPlaybackInput(engine, input.tick, progress,
                                  m_gameState.m_levelTime,
                                  static_cast<size_t>(engine->executeIndex),
                                  inputList.size());
            dispatchImmediatePlaybackAction(input.tick, input.actionType, input.isPressed(), player2, input.stepOffset);
            ++engine->executeIndex;
        }
    }

    void dispatchGDRInputsOnly(int progress) {
        auto* engine = ReplayEngine::get();
        auto& inputList = engine->activeMacro->inputs;

        while (engine->executeIndex < inputList.size()) {
            auto const& input = inputList[engine->executeIndex];
            int inputTick = static_cast<int>(input.frame);
            if (inputTick > progress) break;
            logFirstPlaybackInput(engine, inputTick, progress,
                                  m_gameState.m_levelTime,
                                  static_cast<size_t>(engine->executeIndex),
                                  inputList.size());
            dispatchImmediatePlaybackAction(inputTick, input.button, input.down, input.player2, input.stepOffset);
            ++engine->executeIndex;
        }
    }

    void dispatchGDRInputs(int tick, int effectiveTick, int stepDelta) {
        auto* engine = ReplayEngine::get();
        auto& inputList = engine->activeMacro->inputs;
        AccuracyMode accuracyMode = engine->activeMacro->accuracyMode;

        while (engine->executeIndex < inputList.size()) {
            auto const& input = inputList[engine->executeIndex];
            int inputTick = static_cast<int>(input.frame);
            if (inputTick > effectiveTick) {
                break;
            }

            if (accuracyMode == AccuracyMode::CBS &&
                inputTick == effectiveTick &&
                static_cast<int>(input.stepOffset) > stepDelta) {
                break;
            }

            logFirstPlaybackInput(engine, inputTick, effectiveTick,
                                  m_gameState.m_levelTime,
                                  static_cast<size_t>(engine->executeIndex),
                                  inputList.size());
            dispatchImmediatePlaybackAction(tick, input.button, input.down, input.player2, input.stepOffset);
            ++engine->executeIndex;
        }
    }

    void reconcileAnchors(int effectiveTick) {
        auto* engine = ReplayEngine::get();
        GJBaseGameLayer* playLayer = this;
        auto* anchors = engine->activeAnchors();
        if (!engine->anchorReconciliation || !playLayer || !anchors) {
            return;
        }

        // GDH-aligned reconstruction (field study 02.09): GDH's per-frame
        // handle_update pins ONLY position (+optional vy) and lets GD derive
        // gravity/rotation/vehicle/dash/spider from replayed inputs. Our old
        // code force-restored the FULL state every tick (force = tick match),
        // which fights GD's sub-step state machines - the dash-ring hold
        // animation never completes and the spider-sprite legs render flipped
        // while the root rotation matches. Full restore now fires only on real
        // drift / mode change (needsReconciliation); the per-frame position+vy
        // pin (reconcilePerFrames) still guarantees exact-walk determinism.
        // With a live per-frame stream (EXACT-walk), anchors only reconcile on
        // STRUCTURAL divergence (modeOnly); sparse files keep pos-drift sync.
        const bool modeOnlySync = [&] {
            if (!engine->perFrameReconciliation || !engine->skMode) {
                return false;
            }
            if (auto* pf = engine->activePerFrames(); pf) {
                return !pf->empty();
            }
            return false;
        }();

        static int anchorDiagCount = 0;

        while (engine->playbackAnchorIndex < anchors->size()) {
            auto const& anchor = anchors->at(engine->playbackAnchorIndex);
            if (anchor.tick > effectiveTick) {
                break;
            }

            bool isFirstAnchor = (engine->playbackAnchorIndex == 0);
            bool useForce = isFirstAnchor;

            if (anchorDiagCount < 10 || effectiveTick < 260 || anchorDiagCount % 240 == 0) {
                ++anchorDiagCount;
                skkbot2::log::debug("reconcileAnchor: tick={} anchorIdx={}/{} anchor.tick={} modeOnly={} force={} p2ud={}",
                    effectiveTick, engine->playbackAnchorIndex, anchors->size(),
                    anchor.tick, modeOnlySync, useForce,
                    anchor.hasPlayer2);
            }

            AnchorReconciler::reconcile(playLayer, m_player1, m_player2, anchor, useForce, useForce ? false : modeOnlySync);
            ++engine->playbackAnchorIndex;
        }
    }

    // Re-arm the dash-ring hold on playback: recording proved the only
    // surviving tick-boundary divergence (probe dr=1 vs 0) is GD's
    // startDashing() edge never re-fires in the replayed session, so the
    // ring press animation (and the pointer state feeding it) stays off.
    // The ring is found by its objectID - the world is the same objects -
    // and only when the player currently has NO ring (never re-trigger an
    // in-flight dash).
    static void reattachDashRing(GJBaseGameLayer* playLayer, PlayerObject* player, int32_t ringId,
                                 int tick, int& lastAttemptTick, float recVy, float recVyb) {
        static_cast<void>(lastAttemptTick);
        if (!playLayer || !player || ringId == 0) {
            return;
        }
        // Don't reattach if already attached (GD will detach it at the end of this tick)
        if (player->m_dashRing) {
            return;
        }
        // Use per-frame ring ID for rising edge detection (exact activation tick
        // from recording) instead of unreliable touch detection. The per-frame
        // tells us the exact tick the ring was activated in the recording.
        static int s_lastAttachedRingId[2] = { 0, 0 };
        int slot = (player == playLayer->m_player2) ? 1 : 0;
        bool risingEdge = (ringId != 0 && s_lastAttachedRingId[slot] != ringId);
        s_lastAttachedRingId[slot] = ringId;

        auto attachRing = [&](DashRingObject* obj, const char* via) {
            // Call startDashing only on the rising edge (exact activation tick
            // from per-frame). Subsequent ticks just keep the pointer so GD
            // continues the dash lifecycle naturally.
            if (risingEdge) {
                player->startDashing(obj);
                player->m_yVelocity = recVy;
                player->m_yVelocityBeforeSlope = recVyb;
            } else {
                player->m_dashRing = obj;
                player->m_isDashing = true;
            }
            skkbot2::log::debug("DASH_RING attached: id={} tick={} via={} edge={} player_pos=({:.1f},{:.1f}) ring_pos=({:.1f},{:.1f})",
                ringId, tick, via, risingEdge ? 1 : 0, player->getPositionX(), player->getPositionY(),
                obj->getPositionX(), obj->getPositionY());
        };
        // Direct state set instead of startDashing(): GD's startDashing()
        // triggers endDash() on the next tick, creating a contested state
        // where we fight with GD's dash lifecycle (broken dash physics).
        // By setting m_dashRing directly, GD sees an active dash and
        // processes it normally without calling endDash(). Backup only -
        // with coherent physics the game attaches the ring itself.
        // 2) Nearest match with the ID (never the far twin): the cache is
        // validated by world identity (rebuilds free the objects) and by
        // proximity (a cached twin 2000px+ away is the wrong twin, or the
        // player moved on to another twin sharing the ID).
        static int cacheRingId = 0;
        static DashRingObject* cacheObj = nullptr;
        static void* cacheWorld = nullptr;
        static int cacheWorldCount = -1;
        auto* objects = playLayer->m_objects;
        if (!objects) {
            return;
        }
        const float ppx = player->getPositionX();
        const float ppy = player->getPositionY();
        auto distanceOf = [&](DashRingObject* obj) {
            const float dx = obj->getPositionX() - ppx;
            const float dy = obj->getPositionY() - ppy;
            return std::sqrt(dx * dx + dy * dy);
        };
        if (cacheObj && (cacheRingId != ringId || cacheWorld != static_cast<void*>(objects) ||
                         cacheWorldCount != objects->count() || distanceOf(cacheObj) > 2000.0f)) {
            cacheObj = nullptr;
        }
        if (!cacheObj) {
            cacheRingId = ringId;
            cacheWorld = static_cast<void*>(objects);
            cacheWorldCount = objects->count();
            float bestDist = 1e30f;
            for (int i = 0; i < objects->count(); ++i) {
                auto* node = objects->objectAtIndex(i);
                auto* obj = dynamic_cast<DashRingObject*>(node);
                if (obj && obj->m_objectID == ringId) {
                    const float d = distanceOf(obj);
                    if (d < bestDist) {
                        bestDist = d;
                        cacheObj = obj;
                    }
                }
            }
            if (cacheObj) {
                skkbot2::log::debug("DASH_RING: found ring id={} at tick={} dist={:.1f}",
                    ringId, tick, bestDist);
            } else {
                static int missLogged = 0;
                if (missLogged < 8) {
                    ++missLogged;
                    int dashes = 0;
                    int firstAny = -1;
                    for (int i = 0; i < objects->count(); ++i) {
                        auto* node = objects->objectAtIndex(i);
                        if (firstAny < 0) {
                            if (auto* go = dynamic_cast<GameObject*>(node)) {
                                firstAny = go->m_objectID;
                            }
                        }
                        if (dynamic_cast<DashRingObject*>(node)) {
                            ++dashes;
                        }
                    }
                    skkbot2::log::warn("DASH_RING NOT found: id={} tick={} (world_objects={}, dash_rings_present={}, firstId={})",
                        ringId, tick, objects->count(), dashes, firstAny);
                }
                return;
            }
        }

        if (cacheObj && player) {
            attachRing(cacheObj, "scan");
        }
    }

    // Pre-physics position pin: snap player to the PREVIOUS tick's per-frame
    // position BEFORE GD runs collision detection. This ensures gravity portals,
    // orb touch zones, and pad triggers fire at the exact recorded positions.
    //
    // Per-frame records POST-physics state, so per-frame[tick N-1] = state at
    // the START of tick N. Pinning to it before GD's processCommands gives
    // collision detection the correct input position.
    //
    // Post-step reconcilePerFrames still runs after physics as a safety net.
    // Locate the TeleportPortalObject that actually represents the recorded
    // portal jump. Three independent acceptance criteria:
    //   1. a portal object literally sits at the recorded ENTRY position
    //      (<=300px) - the recorded pre-jump frame may be wherever the player
    //      stood, but the portal zone object usually sits right there; and
    //   2. a portal object's linked destination (m_orangePortal) lands near the
    //      recorded EXIT position (<=600px) - the exact recorded landing spot.
    //   3. a portal object (WITHOUT a linked m_orangePortal) sits AT the
    //      recorded EXIT position (<=600px). These are "destination-standing"
    //      teleports (observed id=3022 recv objects): the portal object IS the
    //      landing target, linked only by group, not by m_orangePortal. GD
    //      fires them live, so treating the jump as a real portal (instead of
    //      an objectless seam) keeps the pre-pin from fighting the native
    //      relocation. Without this criterion prePhyPin reported "NOT a portal"
    //      for Summer Blitz @512 (4748px warp) and dropped the one tick that
    //      re-appears as the persistent -1.05px/0.212vy drift in TRCRECON.
    // If NO portal object matches any criterion, the jump is NOT a real GD
    // portal: it is either fast legit movement at speed-hack speeds (>42px/tick
    // cap) or a RECORDING SEAM (practice death/respawn stitched into one
    // per-frame stream -> physically impossible 2-tick jumps, e.g. DF2 @8446
    // 136.9px, @8944 85.1px, nearest real portal >5k px away). Popping the
    // player at such a jump was THE visible 'teleport bug'. Callers treat
    // nullptr as "not a portal" and fall back to the normal pin.
    // Passing a found entry to GD's own GJBaseGameLayer::teleportPlayer
    // reproduces the portal exactly like a real run: GD resolves the exit via
    // m_orangePortal/group, applies y-offsets/effects and flags the player as
    // teleported (ToastyReplay/silicate approach).
    static TeleportPortalObject* findTeleportEntryPortal(
        GJBaseGameLayer* playLayer, float ex, float ey, float tx, float ty) {
        // Level-wide portal list cache: dynamic_cast on ALL objects (this
        // level is 141 THOUSAND objects) per seam-window tick was a ~22ms
        // synchronous hitch exactly where the recorded teleport/orb fires
        // (PLAY_HOT showed pin=~22ms on every seam tick — the "пролаг ГД").
        // A portal window spans 2 ticks (ahead + behind) and both callers run
        // this search with DIFFERENT (entry,exit) pairs, so a per-pair memo
        // never hit. Portals are static for a level: build the list ONCE per
        // array, then each seam window walks only the SMALL portal list
        // (microseconds).
        prePhyWarmPortalCache(playLayer);
        auto* objects = playLayer->m_objects;
        auto const& sPortalCache = gPortalCache;
        TeleportPortalObject* nearest = nullptr;
        float nearestEntDist = 1e30f;
        TeleportPortalObject* bestOrange = nullptr;
        float bestExitDist = 1e30f;
        TeleportPortalObject* bestDestStand = nullptr;
        float bestDestStandDist = 1e30f;
        int portalCount = static_cast<int>(sPortalCache.size());
        for (auto* tp : sPortalCache) {
            float const mx = tp->getPositionX() - ex;
            float const my = tp->getPositionY() - ey;
            float const dEnt = mx * mx + my * my;
            if (dEnt < nearestEntDist) {
                nearestEntDist = dEnt;
                nearest = tp;
            }
            if (tp->m_orangePortal) {
                float const dx = tp->m_orangePortal->getPositionX() - tx;
                float const dy = tp->m_orangePortal->getPositionY() - ty;
                float const dExit = dx * dx + dy * dy;
                if (dExit < bestExitDist) {
                    bestExitDist = dExit;
                    bestOrange = tp;
                }
            } else {
                float const dx = tp->getPositionX() - tx;
                float const dy = tp->getPositionY() - ty;
                float const dExit = dx * dx + dy * dy;
                if (dExit < bestDestStandDist) {
                    bestDestStandDist = dExit;
                    bestDestStand = tp;
                }
            }
        }
        if (!nearest) {
            skkbot2::log::info(
                "prePhyPin no TeleportPortalObject in level: count={}",
                objects->count());
            return nullptr;
        }
        float const nearestDist = std::sqrt(nearestEntDist);
        float const exitDist = std::sqrt(bestExitDist);
        if (nearestDist <= 300.0f) {
            skkbot2::log::info(
                "prePhyPin findEntryPortal: obj=({:.1f},{:.1f}) orange={} dEnt={:.1f}px bestExit={:.1f}px (entry match)",
                nearest->getPositionX(), nearest->getPositionY(),
                nearest->m_orangePortal ? "yes" : "no", nearestDist,
                exitDist);
            return nearest;
        }
        if (bestOrange && exitDist <= 600.0f) {
            skkbot2::log::info(
                "prePhyPin findEntryPortal: obj=({:.1f},{:.1f}) orange=({:.1f},{:.1f}) dEnt={:.1f}px exit={:.1f}px (exit match)",
                bestOrange->getPositionX(), bestOrange->getPositionY(),
                bestOrange->m_orangePortal->getPositionX(), bestOrange->m_orangePortal->getPositionY(),
                nearestDist, exitDist);
            return bestOrange;
        }
        // Criterion 3 (BUG-04/BUG-06 SB @512): a portal WITHOUT m_orangePortal
        // standing AT the recorded exit - the object IS the landing target
        // (group-linked destination, e.g. id=3022). Treating it as a real
        // portal lets GD's native teleportPlayer run; the no-orange fallback
        // in the caller force-pins to the recorded exit otherwise.
        float const destStandDist = std::sqrt(bestDestStandDist);
        if (bestDestStand && destStandDist <= 600.0f) {
            skkbot2::log::info(
                "prePhyPin findEntryPortal: obj=({:.1f},{:.1f}) orange=no dEnt={:.1f}px destStand={:.1f}px (destination-stand exit match)",
                bestDestStand->getPositionX(), bestDestStand->getPositionY(),
                nearestDist, destStandDist);
            return bestDestStand;
        }
        // BUG-06 diag #3: no portal object near this recorded jump. Dump the
        // objects that DO sit here — the seam is likely a REAL level teleport
        // (spawn-clone portal, touch-trigger, or other mechanism) that GD
        // fires live but that dynamic_cast<TeleportPortalObject*> on m_objects
        // cannot see. The nearest-object IDs/positions reveal the mechanism.
        // Run it at most ONCE per session: the all-objects scan + sort + giant
        // log on the per-tick hot path caused a ~40ms main-thread hitch right
        // at the seam tick (the "GD пролагивает" symptom on every orb press).
        {
            static bool sObjectsNearLogged = false;
            if (!sObjectsNearLogged) {
                sObjectsNearLogged = true;
                struct ObjProbe { float d2; int id; float px; float py; };
                std::vector<ObjProbe> nearby;
                nearby.reserve(64);
                for (int i = 0; i < objects->count(); ++i) {
                    auto* go = dynamic_cast<GameObject*>(objects->objectAtIndex(i));
                    if (!go) continue;
                    float const mx = go->getPositionX() - ex;
                    float const my = go->getPositionY() - ey;
                    float const d2 = mx * mx + my * my;
                    if (d2 > 2000.0f * 2000.0f) continue;
                    nearby.push_back({ d2, go->m_objectID, go->getPositionX(), go->getPositionY() });
                }
                std::sort(nearby.begin(), nearby.end(), [](ObjProbe const& a, ObjProbe const& b) {
                    return a.d2 < b.d2;
                });
                if (nearby.size() > 25) nearby.resize(25);
                std::ostringstream oss;
                oss << "prePhyPin objectsNear entry=(" << std::fixed << std::setprecision(1)
                    << ex << "," << ey << ") exit=(" << tx << "," << ty << ") count=" << nearby.size()
                    << " portalCount=" << portalCount;
                for (auto const& o : nearby) {
                    oss << " | id=" << o.id << " @" << std::setprecision(1) << o.px << "," << o.py
                        << " d=" << std::setprecision(0) << std::sqrt(o.d2);
                }
                skkbot2::log::info("{}", oss.str());
            }
        }
        skkbot2::log::info(
            "prePhyPin NOT a portal (no matching object): jump=({:.1f},{:.1f})->({:.1f},{:.1f}) portalCount={} nearest portal dEnt={:.1f}px at ({:.1f},{:.1f}) dExit={:.1f}px via ({:.1f},{:.1f}) destStand dExit={:.1f}px via ({:.1f},{:.1f})",
            ex, ey, tx, ty, portalCount,
            nearestDist, nearest->getPositionX(), nearest->getPositionY(),
            exitDist,
            bestOrange ? bestOrange->m_orangePortal->getPositionX() : tx,
            bestOrange ? bestOrange->m_orangePortal->getPositionY() : ty,
            destStandDist,
            bestDestStand ? bestDestStand->getPositionX() : tx,
            bestDestStand ? bestDestStand->getPositionY() : ty);
        return nullptr;
    }

    void prePhyPositionPin(int effectiveTick) {
        auto* engine = ReplayEngine::get();
        GJBaseGameLayer* playLayer = this;
        auto* perFrames = engine->activePerFrames();
        if (!engine->perFrameReconciliation || !playLayer || !perFrames || !engine->skMode) {
            return;
        }
        if (perFrames->empty()) return;
        if (effectiveTick <= 0) return;

        int prevTick = effectiveTick - 1;
        const SkPerFrame* pfPrev = nullptr;
        if (engine->perFramePlaybackIndex > 0 && engine->perFramePlaybackIndex <= perFrames->size()) {
            auto const& c = perFrames->at(engine->perFramePlaybackIndex - 1);
            if (c.tick == prevTick) pfPrev = &c;
        }
        if (!pfPrev) {
            for (size_t i = (engine->perFramePlaybackIndex > 0 ? engine->perFramePlaybackIndex - 1 : 0); ; --i) {
                auto const& e = perFrames->at(i);
                if (e.tick <= prevTick) { pfPrev = &e; break; }
                if (i == 0) break;
            }
        }
        if (!pfPrev) {
            // No perFrame at or before prevTick — find the nearest
            // perFrame at or before effectiveTick. This covers early ticks
            // where perFrame data starts later (e.g. first perFrame at tick 2
            // while we're pinning tick 2 — prevTick=1 has no data, but the
            // perFrame at tick 2 is the correct pre-physics state).
            for (size_t i = 0; i < perFrames->size(); ++i) {
                auto const& e = perFrames->at(i);
                if (e.tick <= effectiveTick) { pfPrev = &e; }
                else break;
            }
        }
        if (!pfPrev) {
            return;
        }

        // BUG-06: detect a RECORDED teleport-portal jump around the current
        // frame. At a portal the perFrame data jumps by hundreds/thousands of
        // px between consecutive frames. GD fires the same portal during live
        // physics, but the trigger tick can drift +/-1 vs the recording because
        // the pre-pin only restores position+vy, not the full physics state.
        // Consequences without this guard:
        //   - portal fired ONE tick early live: the player already stands at the
        //     exit, but pre-pin snaps them BACK to the recorded entry position
        //     (3838px backward tear, the worst case on DF2 tick 1382).
        //   - portal not fired yet live (player at the entry): pre-pin snaps them
        //     straight to the recorded exit, teleporting through the portal it
        //     never entered (DF2 tick 6073).
        // So whenever a recorded portal jump is detected in the +/-1 frame
        // window, we skip the pre-physics pin for this tick and let GD's own
        // portal mechanics do the relocation; reconcilePerFrames absorbs the
        // result after physics. If the live portal fails to fire, the post-step
        // reconcile still pins to the recorded exit (only one tick later, and it
        // shows up as a big "playback drift" for diagnosis). Splice/checkpoint
        // jumps are unaffected: their per-frame streams do not contain a
        // consecutive-frame jump here.
        {
            // Portal jumps here are NOT always huge: vertical teleport portals
            // move the player only ~100-200px (observed awd.skk tick 6317:
            // 180px), well below the old 300px threshold -> the pin snapped the
            // player into the recorded exit before GD's own portal fired (or
            // back to the entry after it already fired). Maximum LEGITIMATE
            // consecutive-frame movement is ~42px/tick (4x speed hack), so every
            // jump above that is a teleport/stitch. 30px catches the small
            // vertical portals (recorded 36/43px insta-jumps on awd) without
            // false-positive on any sane recording (legit single-tick movement
            // is <=~3px; even 4x speed hack is <=~42px/tick but then the stream
            // is smooth because every tick is recorded, so no jump exists).
            constexpr float kPortalJumpPx = 30.0f;
            enum class PortalKind { None, Ahead, Behind, LiveGap };
            PortalKind portalKind = PortalKind::None;
            size_t const n = perFrames->size();
            size_t const idx = engine->perFramePlaybackIndex;
            // Endpoint frames of a portal window: pEntry = pre-teleport frame,
            // pExit = post-teleport frame. Used to decide whether the live
            // player is still ON the entry side (pin to entry so physics
            // reproduces the teleport) or already PAST it (skip, don't tear
            // the player back through the portal).
            const SkPerFrame* pEntry = nullptr;
            const SkPerFrame* pExit = nullptr;
            // Jump AHEAD: pfPrev -> next unconsumed frame.
            if (idx < n) {
                auto const& pfNext = perFrames->at(idx);
                if (pfNext.tick > pfPrev->tick) {
                    float const jx = pfNext.p1x - pfPrev->p1x;
                    float const jy = pfNext.p1y - pfPrev->p1y;
                    if (jx * jx + jy * jy > kPortalJumpPx * kPortalJumpPx) {
                        portalKind = PortalKind::Ahead;
                        pEntry = pfPrev;
                        pExit = &pfNext;
                        skkbot2::log::info(
                            "prePhyPin portal window (ahead): effTick={} jump {:.1f}px pfPrev=({:.1f},{:.1f})@{} pfNext=({:.1f},{:.1f})@{}",
                            effectiveTick, std::sqrt(jx * jx + jy * jy),
                            pfPrev->p1x, pfPrev->p1y, pfPrev->tick,
                            pfNext.p1x, pfNext.p1y, pfNext.tick);
                    }
                }
            }
            // Jump BEHIND: frame before pfPrev already jumped INTO the exit
            // (recorded portal fired during the previous physics tick; live
            // may still be on the entry side). Let GD run the portal now.
            if (portalKind == PortalKind::None && idx >= 2) {
                auto const& pfPrev2 = perFrames->at(idx - 2);
                float const jx = pfPrev->p1x - pfPrev2.p1x;
                float const jy = pfPrev->p1y - pfPrev2.p1y;
                if (jx * jx + jy * jy > kPortalJumpPx * kPortalJumpPx) {
                    portalKind = PortalKind::Behind;
                    pEntry = &pfPrev2;
                    pExit = pfPrev;
                    skkbot2::log::info(
                        "prePhyPin portal window (behind): effTick={} jump {:.1f}px pfPrev2=({:.1f},{:.1f})@{} pfPrev=({:.1f},{:.1f})@{}",
                        effectiveTick, std::sqrt(jx * jx + jy * jy),
                        pfPrev2.p1x, pfPrev2.p1y, pfPrev2.tick,
                        pfPrev->p1x, pfPrev->p1y, pfPrev->tick);
                }
            }
            // Live player far from pin target: the recorded data is sparse
            // (perFrame not every tick), so perFrame gaps can MISS a portal
            // jump that already happened in live GD physics. If the live
            // player is >threshold away from pfPrev, pinning back would cause
            // a visible teleport; skip and let reconcilePerFrames pull to
            // the correct post-physics position instead.
            if (portalKind == PortalKind::None && playLayer->m_player1) {
                float const dx = playLayer->m_player1->getPositionX() - pfPrev->p1x;
                float const dy = playLayer->m_player1->getPositionY() - pfPrev->p1y;
                if (dx * dx + dy * dy > kPortalJumpPx * kPortalJumpPx) {
                    portalKind = PortalKind::LiveGap;
                    skkbot2::log::info(
                        "prePhyPin portal window (live-player-gap): effTick={} dist {:.1f}px cur=({:.1f},{:.1f}) pin=({:.1f},{:.1f})",
                        effectiveTick, std::sqrt(dx * dx + dy * dy),
                        playLayer->m_player1->getPositionX(), playLayer->m_player1->getPositionY(),
                        pfPrev->p1x, pfPrev->p1y);
                }
            }
            if (portalKind != PortalKind::None && pEntry && pExit) {
                // BUG-06 gate: only treat a big recorded jump as a REAL portal
                // if the level actually has a TeleportPortalObject there. Fast
                // legit movement at speed-hack speeds and recording seams
                // (practice death/respawn stitched into one stream) produce
                // 60-160px jumps with NO portal object nearby. These are NOT
                // discrete-dash gaps anymore: perframe_jumps.txt proved the
                // clean awd recording has an INSTANT recorded relocation at
                // exactly these spots (start warp 4745px, vertical 36-148px),
                // and the exit lands frame-perfect on real portal heights.
                // GD live does not fire them (no portal object -> no fire), so
                // gliding at 26px/tick over ~5 frames reads as lag/freeze to
                // the eye (the reported symptom). Reproduce the jump EXACTLY
                // as recorded: one instant relocation, same as a real portal.
                // Backward/rewind exits (respawn seams) never snap backward.
                if (!findTeleportEntryPortal(
                        playLayer, pEntry->p1x, pEntry->p1y, pExit->p1x, pExit->p1y)) {
                    if (playLayer->m_player1) {
                        float const sx = playLayer->m_player1->getPositionX();
                        float const sy = playLayer->m_player1->getPositionY();
                        float const jx = pExit->p1x - sx;
                        float const jy = pExit->p1y - sy;
                        float const jumpLen = std::sqrt(jx * jx + jy * jy);
                        // BUG-06: visual-state witness so a clean (GD-fired)
                        // seam can be compared field-by-field against a bot-snap
                        // with the SAME record jump — rotation/sprite/vel/ground
                        // mismatches are what read as a 'sharp dirty teleport'.
                        auto seamVisual = [&](const char* tag) {
                            auto* p = playLayer->m_player1;
                            if (!p) return;
                            float srot = -999.0f, ssx = 0.0f, ssy = 0.0f;
                            if (p->m_spiderSprite) {
                                srot = p->m_spiderSprite->getRotation();
                                ssx = p->m_spiderSprite->getScaleX();
                                ssy = p->m_spiderSprite->getScaleY();
                            }
                            skkbot2::log::info(
                                "seamVisual[{}]: effTick={} pos=({:.1f},{:.1f}) rot={:.1f} vy={:.1f} "
                                "spiderRot={:.1f} spiderScale=({:.2f},{:.2f}) upDown={} dashing={} onGr={}",
                                tag, effectiveTick, p->getPositionX(), p->getPositionY(),
                                p->getRotation(), p->m_yVelocity, srot, ssx, ssy,
                                p->m_isUpsideDown ? 1 : 0, p->m_isDashing ? 1 : 0,
                                p->m_isOnGround ? 1 : 0);
                        };
                        // Rewind seam guard: if the live player is already at /
                        // past the recorded EXIT side, the stream just stitched a
                        // respawn that lands BEHIND us; pinning to the exit would
                        // drag the player backward across the whole level. Skip
                        // the whole pre-phy pin this tick (reconcile then lets
                        // the route catch up on its own).
                        float const din  = (sx - pEntry->p1x) * (sx - pEntry->p1x) +
                                           (sy - pEntry->p1y) * (sy - pEntry->p1y);
                        float const dout = (sx - pExit->p1x)  * (sx - pExit->p1x) +
                                           (sy - pExit->p1y)  * (sy - pExit->p1y);
                        if (dout < din) {
                            engine->seamGlideUntilTick = -1;
                            seamVisual("skip_past_exit");
                            skkbot2::log::info(
                                "prePhyPin seam (live past exit, skip): effTick={} cur=({:.1f},{:.1f}) exit=({:.1f},{:.1f})@{}",
                                effectiveTick, sx, sy, pExit->p1x, pExit->p1y, pExit->tick);
                            portalKind = PortalKind::None;
                            return;
                        }
                        // Never snap BACKWARD for an objectless seam: that is a
                        // death/respawn rewind (record stitched the checkpoint
                        // behind us), not a level teleport. Dragging the player
                        // backward across the level was THE visible tear bug.
                        // Real portals are handled above (object found). All
                        // legit objectless teleports in a clean recording move
                        // FORWARD (start warp 4745px, vertical drops 36-148px,
                        // x stays == or advances). Skip and let reconcile pull
                        // to the next in-order frame.
                        if (pExit->p1x < pEntry->p1x - 40.0f) {
                            engine->seamGlideUntilTick = -1;
                            seamVisual("skip_backward");
                            skkbot2::log::info(
                                "prePhyPin seam (backward rewind, skip): effTick={} cur=({:.1f},{:.1f}) exit=({:.1f},{:.1f})@{}",
                                effectiveTick, sx, sy, pExit->p1x, pExit->p1y, pExit->tick);
                            portalKind = PortalKind::None;
                            return;
                        }
                        engine->seamGlideUntilTick = -1;
                        // BUG-06: input-driven instant relocation (spider hold,
                        // teleport-orb, dash-orb). The clean recording has a
                        // consecutive-frame jump that is NOT a level portal (no
                        // TeleportPortalObject nearby), but the frame data PROVES
                        // a recorded press lands on the same window (awd 8525/8570/
                        // 8934/9104/6278 map 1:1 to the jump ticks in inputs_dump).
                        // During live play GD fires these natively when the press
                        // is dispatched — dispatchPlaybackInputsPre runs right
                        // before this pin — and the native displacement equals the
                        // recorded jump. The old objectless instant seam snap
                        // (setPosition to exit + m_wasTeleported) fought that
                        // native relocation: a hard position write landed BEFORE
                        // GD's own orb/spider moved the player, producing the
                        // visible double-hop/flash the user reads as "рывок ГД".
                        // So for a press-driven jump we skip the pre-phy pin like
                        // a portal window: let GD run its mechanic this tick and
                        // reconcilePerFrames absorbs the result (live lands at the
                        // recorded exit, then pinned exactly). Only truly
                        // press-less objectless jumps (start warp 4711px, plain
                        // vertical drops) keep the instant snap — GD never fires
                        // those on its own.
                        if (auto* inputs = engine->activeSkInputs()) {
                            bool pressDriven = false;
                            int const lo = std::min(pEntry->tick, pExit->tick) - 1;
                            int const hi = std::max(pEntry->tick, pExit->tick) + 1;
                            for (auto const& in : *inputs) {
                                if (!in.isPressed()) continue;
                                if (in.tick >= lo && in.tick <= hi) {
                                    pressDriven = true;
                                    break;
                                }
                            }
                            if (pressDriven) {
                                seamVisual("native_relocation_skip");
                                skkbot2::log::info(
                                    "prePhyPin input-driven relocation (skip, GD fires): effTick={} jump={:.1f}px entry=({:.1f},{:.1f})@{} exit=({:.1f},{:.1f})@{}",
                                    effectiveTick, jumpLen,
                                    pEntry->p1x, pEntry->p1y, pEntry->tick,
                                    pExit->p1x, pExit->p1y, pExit->tick);
                                portalKind = PortalKind::None;
                                return;
                            }
                        }
                        // Objectless seam = a recorded death/respawn stitch (or
                        // plain recorded teleport gap) with no portal object.
                        // GD NEVER fires these live (no object to collide), so
                        // there is nothing to wait for and no native effect to
                        // preserve: the extra "free 1 tick, let GD fire" window
                        // only stalled the player at the entry (reads as lag) and
                        // always ended in the same snap. Reproduce the recorded
                        // relocation EXACTLY as the reference bots do (per-frame
                        // position pin, one clean tick): write the player to the
                        // recorded exit frame immediately and mark it as GD's own
                        // teleport so the engine renders the standard flash/slide
                        // (same visual cue as the clean original respawn) instead
                        // of a raw snap that reads as a lag hitch.
                        playLayer->m_player1->setPosition({ pExit->p1x, pExit->p1y });
                        playLayer->m_player1->m_yVelocity = pExit->p1vy;
                        playLayer->m_player1->m_yVelocityBeforeSlope = pExit->p1vyb;
                        playLayer->m_player1->setRotation(pExit->p1rot);
                        playLayer->m_player1->m_wasTeleported = true;
                        seamVisual("snap");
                        skkbot2::log::info(
                            "prePhyPin instant seam snap: effTick={} cur=({:.1f},{:.1f})->exit=({:.1f},{:.1f})@{} jump={:.1f}px (reproduced as recorded)",
                            effectiveTick, sx, sy, pExit->p1x, pExit->p1y, pExit->tick, jumpLen);
                        portalKind = PortalKind::None;
                        return;
                    }
                    portalKind = PortalKind::None;
                }
            }
            if (portalKind != PortalKind::None) {
                bool skipPin = true;
                if (playLayer->m_player1 && pEntry && pExit) {
                    // Decide by the live player's position. If already at/past
                    // the EXIT endpoint, GD fired the portal naturally (or is a
                    // plain live-gap divergence) -> skip, do NOT tear the player
                    // back through the portal (BUG-06 DF2 tear). If still at the
                    // ENTRY side -> reproduce the portal ourselves below.
                    float const cx = playLayer->m_player1->getPositionX();
                    float const cy = playLayer->m_player1->getPositionY();
                    float const dIn  = (cx - pEntry->p1x) * (cx - pEntry->p1x) +
                                       (cy - pEntry->p1y) * (cy - pEntry->p1y);
                    float const dOut = (cx - pExit->p1x)  * (cx - pExit->p1x) +
                                       (cy - pExit->p1y)  * (cy - pExit->p1y);
                    skipPin = (dOut < dIn);
                }
                if (skipPin) {
                    // Player already at the recorded exit (or a plain live-gap
                    // divergence) -> let GD finish the relocation and let
                    // reconcilePerFrames absorb the result after physics.
                    skkbot2::log::info("prePhyPin skip (portal window): effTick={} cur=({:.1f},{:.1f})",
                        effectiveTick,
                        playLayer->m_player1 ? playLayer->m_player1->getPositionX() : 0.0f,
                        playLayer->m_player1 ? playLayer->m_player1->getPositionY() : 0.0f);
                    return;
                }
                // Player is still on the ENTRY side at the recorded portal tick.
                // Just pinning to the entry does NOT fire GD's portal: the pin
                // resets the player to the zone edge every tick, the zone-overlap
                // check (run at the START of each physics step) never sees the
                // player inside, and GD never teleports (awd.skk 8935: stuck at
                // the entry for ticks, then a delayed 146px snap to the exit).
                // So fire the portal OURSELVES at this exact tick via GD's own
                // teleportPlayer(entryPortal) -> correct exit, state and effects.
                // If the entry object cannot be found, or the GD call does not
                // take effect, fall back to a direct jump to the recorded exit
                // (single in-place portal pop, marked m_wasTeleported).
                static const SkPerFrame* sFiredEntry = nullptr;
                static int sFiredTick = -1;
                bool const firedAlready =
                    (sFiredEntry == pEntry) && (effectiveTick - sFiredTick <= 1);
                auto forceExit = [&]() {
                    if (playLayer->m_player1) {
                        playLayer->m_player1->setPosition({ pExit->p1x, pExit->p1y });
                        playLayer->m_player1->m_yVelocity = pExit->p1vy;
                        playLayer->m_player1->m_yVelocityBeforeSlope = pExit->p1vyb;
                        playLayer->m_player1->m_wasTeleported = true;
                    }
                    if (pExit->hasPlayer2 && playLayer->m_player2) {
                        playLayer->m_player2->setPosition({ pExit->p2x, pExit->p2y });
                        playLayer->m_player2->m_yVelocity = pExit->p2vy;
                        playLayer->m_player2->m_yVelocityBeforeSlope = pExit->p2vyb;
                        playLayer->m_player2->m_wasTeleported = true;
                    }
                };
                if (firedAlready) {
                    skkbot2::log::info(
                        "prePhyPin force portal exit (gd fire no-op): effTick={} cur=({:.1f},{:.1f}) exit=({:.1f},{:.1f})@{}",
                        effectiveTick,
                        playLayer->m_player1 ? playLayer->m_player1->getPositionX() : 0.0f,
                        playLayer->m_player1 ? playLayer->m_player1->getPositionY() : 0.0f,
                        pExit->p1x, pExit->p1y, pExit->tick);
                    forceExit();
                    return;
                }
                TeleportPortalObject* entryPortal = findTeleportEntryPortal(
                    playLayer, pEntry->p1x, pEntry->p1y, pExit->p1x, pExit->p1y);
                // Only let GD resolve the destination when the portal carries a
                // linked m_orangePortal. A portal without one (target handled by
                // a group) makes GD pick a RANDOM group portal as destination;
                // for exact-record replay that would land who-knows-where, so a
                // deterministic jump to the recorded exit is safer there.
                if (entryPortal && entryPortal->m_orangePortal && playLayer->m_player1) {
                    sFiredEntry = pEntry;
                    sFiredTick = effectiveTick;
                    skkbot2::log::info(
                        "prePhyPin fire portal: effTick={} entryObj=({:.1f},{:.1f}) orange=({:.1f},{:.1f}) exit=({:.1f},{:.1f})@{}",
                        effectiveTick,
                        entryPortal->getPositionX(), entryPortal->getPositionY(),
                        entryPortal->m_orangePortal->getPositionX(), entryPortal->m_orangePortal->getPositionY(),
                        pExit->p1x, pExit->p1y, pExit->tick);
                    playLayer->GJBaseGameLayer::teleportPlayer(entryPortal, playLayer->m_player1);
                    if (pExit->hasPlayer2 && playLayer->m_player2) {
                        playLayer->GJBaseGameLayer::teleportPlayer(entryPortal, playLayer->m_player2);
                    }
                    return;
                }
                sFiredEntry = pEntry;
                sFiredTick = effectiveTick;
                skkbot2::log::info(
                    "prePhyPin force portal exit (entry no-orange): effTick={} cur=({:.1f},{:.1f}) exit=({:.1f},{:.1f})@{}",
                    effectiveTick,
                    playLayer->m_player1 ? playLayer->m_player1->getPositionX() : 0.0f,
                    playLayer->m_player1 ? playLayer->m_player1->getPositionY() : 0.0f,
                    pExit->p1x, pExit->p1y, pExit->tick);
                forceExit();
                return;
            }
        }

        // BUG-06 diagnostics: this pin is the ONLY position write on the
        // playback path without drift logging. If the per-frame pointer ever
        // snaps to a "wrong" record (stale index after practice restore,
        // splice, or a bad fallback scan below), the player jumps across the
        // map silently. Log any spike above a generous threshold so the next
        // report gives us the pin tick and indices.
        auto pinSpike = [&](PlayerObject* p, float pnx, float pny, int pinTick, const char* slot) {
            if (!p || playLayer->m_player1 == nullptr) {
                return;
            }
            const float dx = p->getPositionX() - pnx;
            const float dy = p->getPositionY() - pny;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < 100.0f) {
                return;
            }
            static int sSpikeCount = 0;
            if (++sSpikeCount <= 6 || sSpikeCount % 120 == 0) {
                skkbot2::log::warn(
                    "TELEPORT-SPIKE?: prePhyPin slot={} effTick={} pinTick={} cur=({:.1f},{:.1f}) pin=({:.1f},{:.1f}) dist={:.1f} pfIdx={}/{}",
                    slot, effectiveTick, pinTick,
                    p->getPositionX(), p->getPositionY(), pnx, pny, dist,
                    engine->perFramePlaybackIndex, perFrames->size());
            }
        };

        if (playLayer->m_player1) {
            // BUG-06: adopt the reference-bot playback model (GDH/Toasty pin
            // the player to the recorded frame EVERY tick, velocity-fix on).
            // Zero drift means level triggers (orbs/pads/cuts) fire exactly
            // like the recording and the whole level runs smooth instead of
            // "lagging". Active seam windows already returned early above
            // (defer/snap/fire/skip + rewind), so reaching here means this
            // tick maps to a clean frame: pin unconditionally. Only a >60px
            // offset (an unclassified record gap) is left to reconcilePerFrames
            // with capped forward steps.
            float const cx = playLayer->m_player1->getPositionX();
            float const cy = playLayer->m_player1->getPositionY();
            float const dx = pfPrev->p1x - cx;
            float const dy = pfPrev->p1y - cy;
            float const d = std::sqrt(dx * dx + dy * dy);
            const double liveVyBeforePin = playLayer->m_player1->m_yVelocity;
            const double liveVybBeforePin = playLayer->m_player1->m_yVelocityBeforeSlope;
            const float liveRotBeforePin = playLayer->m_player1->getRotation();
            if (d < 60.0f) {
                pinSpike(playLayer->m_player1, pfPrev->p1x, pfPrev->p1y, pfPrev->tick, "p1");
                playLayer->m_player1->setPosition({ pfPrev->p1x, pfPrev->p1y });
                playLayer->m_player1->m_yVelocity = pfPrev->p1vy;
                playLayer->m_player1->m_yVelocityBeforeSlope = pfPrev->p1vyb;
                // Rotation must be pinned BEFORE physics too (not only in
                // reconcilePerFrames). GD's slope/look-at math reads the live
                // rotation attached to the node; if only reconcile sets it, the
                // per-tick physics roll in a rotating ship/ball starts from the
                // stale live rotation and adds a growing Δrot on long spins.
                playLayer->m_player1->setRotation(pfPrev->p1rot);
                if (effectiveTick < gTraceWindowEnd) {
                    logP1TraceLine(
                        "PRE", effectiveTick,
                        cx, cy, liveVyBeforePin, liveVybBeforePin, liveRotBeforePin,
                        pfPrev->p1x, pfPrev->p1y, pfPrev->p1vy, pfPrev->p1vyb, pfPrev->p1rot,
                        cx - pfPrev->p1x, cy - pfPrev->p1y,
                        liveVyBeforePin - pfPrev->p1vy, liveVybBeforePin - pfPrev->p1vyb,
                        liveRotBeforePin - pfPrev->p1rot,
                        (pfPrev->p1vis & kVisualGravityNegative) ? 1 : 0,
                        (pfPrev->p1vis & kVisualUpsideDown) ? 1 : 0,
                        (pfPrev->p1vis & kVisualDashing) ? 1 : 0,
                        pfPrev->p1RingId);
                }
            } else {
                skkbot2::log::info(
                    "prePhyPin no-pin (reconcile owns convergence): effTick={} cur=({:.1f},{:.1f}) want=({:.1f},{:.1f}) rem={:.1f}px",
                    effectiveTick, cx, cy, pfPrev->p1x, pfPrev->p1y, d);
            }
        }
        if (pfPrev->hasPlayer2 && playLayer->m_player2) {
            // BUG-21: a recorded (0,0) P2 frame is the DORMANT sentinel from
            // before the P2 liveness gate existed (or a macro from before
            // today). Pinning a live, dual-activated P2 to it teleports it
            // across the level. GD's own liveness signal is parenting, so
            // skip the pin for dormant frames.
            if (pfPrev->p2x == 0.0f && pfPrev->p2y == 0.0f
                && playLayer->m_player2->getParent() != nullptr) {
                skkbot2::log::debug(
                    "prePhyPin p2 dormant-skip: effTick={} pfIdx={}/{} live=({:.1f},{:.1f})",
                    effectiveTick, engine->perFramePlaybackIndex, perFrames->size(),
                    playLayer->m_player2->getPositionX(), playLayer->m_player2->getPositionY());
            } else {
                pinSpike(playLayer->m_player2, pfPrev->p2x, pfPrev->p2y, pfPrev->tick, "p2");
                playLayer->m_player2->setPosition({ pfPrev->p2x, pfPrev->p2y });
                playLayer->m_player2->m_yVelocity = pfPrev->p2vy;
                playLayer->m_player2->m_yVelocityBeforeSlope = pfPrev->p2vyb;
            }
        }
    }

    void reconcilePerFrames(int effectiveTick) {
        auto* engine = ReplayEngine::get();
        GJBaseGameLayer* playLayer = this;
        auto* perFrames = engine->activePerFrames();
        if (!engine->perFrameReconciliation || !playLayer || !perFrames || !engine->skMode) {
            return;
        }

        static int pfDiagCount = 0;
        int framesAppliedThisTick = 0;

        while (engine->perFramePlaybackIndex < perFrames->size()) {
            auto const& pf = perFrames->at(engine->perFramePlaybackIndex);
            if (pf.tick > effectiveTick) {
                break;
            }

            if (pfDiagCount < 5 || (effectiveTick < 260 && pfDiagCount < 30) || pfDiagCount % 1000 == 0) {
                float curP1x = playLayer->m_player1 ? playLayer->m_player1->getPositionX() : 0;
                float curP1y = playLayer->m_player1 ? playLayer->m_player1->getPositionY() : 0;
                ++pfDiagCount;
                skkbot2::log::debug("reconcilePF: tick={} pfIdx={}/{} pf.tick={} cur=({:.1f},{:.1f}) want=({:.1f},{:.1f}) drift=({:.1f},{:.1f})",
                    effectiveTick, engine->perFramePlaybackIndex, perFrames->size(),
                    pf.tick, curP1x, curP1y, pf.p1x, pf.p1y,
                    curP1x - pf.p1x, curP1y - pf.p1y);
            }

            const float preP1x = playLayer->m_player1 ? playLayer->m_player1->getPositionX() : pf.p1x;
            const float preP1y = playLayer->m_player1 ? playLayer->m_player1->getPositionY() : pf.p1y;
            const double liveVyBeforePin = playLayer->m_player1 ? playLayer->m_player1->m_yVelocity : 0.0;
            const double liveVybBeforePin = playLayer->m_player1 ? playLayer->m_player1->m_yVelocityBeforeSlope : 0.0;
            const float liveRotBeforePin = playLayer->m_player1 ? playLayer->m_player1->getRotation() : 0.f;
            const float dx0 = preP1x - pf.p1x;
            const float dy0 = preP1y - pf.p1y;
            // Keep in sync with prePhyPositionPin's portal threshold (60px):
            // anything above is a portal teleport / stitch gap that GD's own
            // physics must resolve (pre-pin was already skipped that tick; a
            // 180px reconcile snap right after would recreate the teleport).
            constexpr float kReconcileSkipPx = 60.0f;
            if (dx0 * dx0 + dy0 * dy0 > kReconcileSkipPx * kReconcileSkipPx) {
                // Big live->recorded gap. If the recorded frame is AHEAD,
                // converge with a capped step (never a single-tick snap) at
                // 26px/tick — this covers both armed seam glides and medium
                // divergences that never reached the portal window. If the
                // route is BEHIND (respawn/rewind seam: stream jumped back to
                // start/checkpoint or the merge put it behind us), skip and
                // let the route catch up on its own — never drag backwards.
                if (playLayer->m_player1 && pf.p1x > preP1x) {
                    float const dx1 = pf.p1x - preP1x;
                    float const dy1 = pf.p1y - preP1y;
                    float const d1 = std::sqrt(dx1 * dx1 + dy1 * dy1);
                    float const nx = preP1x + dx1 / d1 * ReplayEngine::kSeamGlideStepPx;
                    float const ny = preP1y + dy1 / d1 * ReplayEngine::kSeamGlideStepPx;
                    playLayer->m_player1->setPosition({ nx, ny });
                    playLayer->m_player1->m_yVelocity = pf.p1vy;
                    playLayer->m_player1->m_yVelocityBeforeSlope = pf.p1vyb;
                    skkbot2::log::info(
                        "reconcilePF glide step: tick={} pfIdx={}/{} cur=({:.1f},{:.1f})->({:.1f},{:.1f}) want=({:.1f},{:.1f}) dist={:.1f}",
                        effectiveTick, engine->perFramePlaybackIndex, perFrames->size(),
                        preP1x, preP1y, nx, ny, pf.p1x, pf.p1y, d1);
                    ++engine->perFramePlaybackIndex;
                    ++framesAppliedThisTick;
                    continue;
                }
                skkbot2::log::info(
                    "reconcilePF SKIP (route behind/live-player-gap): tick={} pfIdx={}/{} pf.tick={} cur=({:.1f},{:.1f}) want=({:.1f},{:.1f}) dist={:.1f}",
                    effectiveTick, engine->perFramePlaybackIndex, perFrames->size(),
                    pf.tick, preP1x, preP1y, pf.p1x, pf.p1y, std::sqrt(dx0 * dx0 + dy0 * dy0));
                ++engine->perFramePlaybackIndex;
                ++framesAppliedThisTick;
                continue;
            }

            if (playLayer->m_player1) {
                // Never instant-snap across a gap on the normal path either:
                // sub-60px seams (they never reached the portal window) and any
                // leftover below the skip threshold used to be pinned in ONE
                // tick, which is exactly the visible teleport-blip (52px@8541,
                // 56px@9103). Always converge with capped forward steps; only
                // pin exactly when the route is already within the cap. Never
                // claw BACKWARD to a behind route (rewind seams close on their
                // own).
                bool leavePosition = false;
                float pinX = pf.p1x;
                float pinY = pf.p1y;
                {
                    float const dx1 = pf.p1x - preP1x;
                    float const dy1 = pf.p1y - preP1y;
                    float const d1 = std::sqrt(dx1 * dx1 + dy1 * dy1);
                    if (pf.p1x > preP1x) {
                        if (d1 > ReplayEngine::kSeamGlideStepPx) {
                            pinX = preP1x + dx1 / d1 * ReplayEngine::kSeamGlideStepPx;
                            pinY = preP1y + dy1 / d1 * ReplayEngine::kSeamGlideStepPx;
                        } else {
                            engine->seamGlideUntilTick = -1;
                        }
                    } else if (d1 <= ReplayEngine::kSeamGlideStepPx) {
                        // Small backward gap (live slightly AHEAD of the route,
                        // typically <= one tick's advance, ~2px). Claw it back
                        // now: leaving it lets a one-substep lead turn into a
                        // lingering forward offset on the rendered frame. Only
                        // genuine rewind seams (large backward distance) are
                        // exempt below.
                        engine->seamGlideUntilTick = -1;
                    } else {
                        leavePosition = true;
                    }
                }
                if (!leavePosition) {
                    playLayer->m_player1->setPosition({ pinX, pinY });
                }
                // === PLAYBACK JUMP TRACER (auto window) ==========================
                // Log the LIVE post-physics / pre-reconcile state vs the recorded
                // target, so the reported "hop" can be attributed to either the
                // physics itself (live already drifted) or the reconcile pin.
                if (effectiveTick < gTraceWindowEnd) {
                    logP1TraceLine(
                        "RECON", effectiveTick,
                        preP1x, preP1y, liveVyBeforePin, liveVybBeforePin, liveRotBeforePin,
                        pf.p1x, pf.p1y, pf.p1vy, pf.p1vyb, pf.p1rot,
                        preP1x - pf.p1x, preP1y - pf.p1y,
                        liveVyBeforePin - pf.p1vy, liveVybBeforePin - pf.p1vyb,
                        liveRotBeforePin - pf.p1rot,
                        (pf.p1vis & kVisualGravityNegative) ? 1 : 0,
                        (pf.p1vis & kVisualUpsideDown) ? 1 : 0,
                        (pf.p1vis & kVisualDashing) ? 1 : 0,
                        pf.p1RingId);
                }
                // ==================================================================
                playLayer->m_player1->m_yVelocity = pf.p1vy;
                playLayer->m_player1->m_yVelocityBeforeSlope = pf.p1vyb;
                playLayer->m_player1->setRotation(pf.p1rot);
                // Gravity/ud state correction from p1vis (TLPD repair):
                // GD's orb/portals sometimes fail to flip during playback due
                // to substep timing. Force the recorded state to keep visuals
                // deterministic. This is NOT full state restore — only gravity
                // sign and upsideDown flag which are pure OUTPUT of collision
                // detection and don't feed back into substep state machines.
                {
                    const bool wantGravNeg = (pf.p1vis & kVisualGravityNegative) != 0;
                    const bool wantUD      = (pf.p1vis & kVisualUpsideDown) != 0;
                    const bool curGravNeg  = playLayer->m_player1->m_gravity < 0.0;
                    const bool curUD       = playLayer->m_player1->m_isUpsideDown;
                    // BUG-04 study: log every forced gravity/ud application and
                    // every disagreeing recorded frame. `lag` = how many ticks
                    // behind the recorded target the live player sat when the
                    // reconcile had to force the state - proves the hypothesis
                    // that state lands one (or several) ticks LATE.
                    if (curGravNeg != wantGravNeg || curUD != wantUD) {
                        const int lag = effectiveTick - pf.tick;
                        // Always INFO, and on BOTH bits disagreeing. The old gate
                        // (wantUD != wantGravNeg) only fired when the two bits
                        // disagreed with each other, which a normal gravity portal
                        // never does — both flip together — so the exact case we are
                        // chasing (a UFO's gravity flip not landing) logged nothing.
                        if (std::abs(lag) <= 8) {
                            skkbot2::log::info(
                                "RECON ud: tick={} pfTick={} lag={} p1 cur(g={} ud={}) -> want(g={} ud={}) p1vis=0x{:02x}",
                                effectiveTick, pf.tick, lag,
                                curGravNeg ? 1 : 0, curUD ? 1 : 0,
                                wantGravNeg ? 1 : 0, wantUD ? 1 : 0,
                                static_cast<int>(pf.p1vis));
                        }
                    }
                    if (curGravNeg != wantGravNeg) {
                        double mag = std::abs(playLayer->m_player1->m_gravity);
                        if (mag < 0.001) mag = 1.0;
                        playLayer->m_player1->m_gravity = wantGravNeg ? -mag : mag;
                        if (playLayer->m_player1->m_isRobot) {
                            playLayer->m_player1->m_wasRobotJump = false;
                        }
                    }
                    if (curUD != wantUD) {
                        playLayer->m_player1->m_isUpsideDown = wantUD;
                        playLayer->m_player1->m_stateFlipGravity = wantUD ? 1 : 0;
                    }
                }
                if (pf.p1RingId != 0) {
                    reattachDashRing(playLayer, playLayer->m_player1, pf.p1RingId, pf.tick,
                                     engine->dashReattachLast[0], pf.p1vy, pf.p1vyb);
                }
                // Dash state correction from p1vis bit7: force m_isDashing
                // to match the recording. This drives the dash orb hold
                // animation and dash visual effects that GD may miss due to
                // substep timing differences.
                {
                    const bool wantDash = (pf.p1vis & kVisualDashing) != 0;
                    if (playLayer->m_player1->m_isDashing != wantDash) {
                        playLayer->m_player1->m_isDashing = wantDash;
                    }
                }
            }
            if (pf.hasPlayer2 && playLayer->m_player2) {
                // BUG-21: skip the dormant (0,0) sentinel — same guard as the
                // pre-phy pin above, so a dual-activated live P2 is never
                // yanked to the origin by a pre-gate recorded frame.
                if (pf.p2x == 0.0f && pf.p2y == 0.0f
                    && playLayer->m_player2->getParent() != nullptr) {
                    // Still reconcile the gravity/ud flags below? No — a
                    // dormant frame's vis bits are stale too. Skip entirely.
                } else {
                    playLayer->m_player2->setPosition({ pf.p2x, pf.p2y });
                    playLayer->m_player2->m_yVelocity = pf.p2vy;
                    playLayer->m_player2->m_yVelocityBeforeSlope = pf.p2vyb;
                    playLayer->m_player2->setRotation(pf.p2rot);
                }
                // P2 gravity/ud correction (mirrors P1 logic above).
                {
                    const bool wantGravNeg = (pf.p2vis & kVisualGravityNegative) != 0;
                    const bool wantUD      = (pf.p2vis & kVisualUpsideDown) != 0;
                    const bool curGravNeg  = playLayer->m_player2->m_gravity < 0.0;
                    const bool curUD       = playLayer->m_player2->m_isUpsideDown;
                    if (curGravNeg != wantGravNeg || curUD != wantUD) {
                        const int lag = effectiveTick - pf.tick;
                        if (wantUD != wantGravNeg) {
                            skkbot2::log::info(
                                "RECON ud[BUG-04]: tick={} pfTick={} lag={} p2 cur(g={} ud={}) -> want(g={} ud={}) p2vis=0x{:02x}",
                                effectiveTick, pf.tick, lag,
                                curGravNeg ? 1 : 0, curUD ? 1 : 0,
                                wantGravNeg ? 1 : 0, wantUD ? 1 : 0,
                                static_cast<int>(pf.p2vis));
                        } else {
                            skkbot2::log::debug(
                                "RECON ud[BUG-04]: tick={} pfTick={} lag={} p2 cur(g={} ud={}) -> want(g={} ud={}) p2vis=0x{:02x}",
                                effectiveTick, pf.tick, lag,
                                curGravNeg ? 1 : 0, curUD ? 1 : 0,
                                wantGravNeg ? 1 : 0, wantUD ? 1 : 0,
                                static_cast<int>(pf.p2vis));
                        }
                    }
                    if (curGravNeg != wantGravNeg) {
                        double mag = std::abs(playLayer->m_player2->m_gravity);
                        if (mag < 0.001) mag = 1.0;
                        playLayer->m_player2->m_gravity = wantGravNeg ? -mag : mag;
                        if (playLayer->m_player2->m_isRobot) {
                            playLayer->m_player2->m_wasRobotJump = false;
                        }
                    }
                    if (curUD != wantUD) {
                        playLayer->m_player2->m_isUpsideDown = wantUD;
                        playLayer->m_player2->m_stateFlipGravity = wantUD ? 1 : 0;
                    }
                }
                if (pf.p2RingId != 0) {
                    reattachDashRing(playLayer, playLayer->m_player2, pf.p2RingId, pf.tick,
                                     engine->dashReattachLast[1], pf.p2vy, pf.p2vyb);
                }
                // P2 dash state correction (mirrors P1 logic above).
                {
                    const bool wantDash = (pf.p2vis & kVisualDashing) != 0;
                    if (playLayer->m_player2->m_isDashing != wantDash) {
                        playLayer->m_player2->m_isDashing = wantDash;
                    }
                }
            }

            const float dx = preP1x - pf.p1x;
            const float dy = preP1y - pf.p1y;
            const float drift = std::max(std::abs(dx), std::abs(dy));
            if (drift > engine->playDriftMax) {
                engine->playDriftMax = drift;
                engine->playDriftMaxTick = pf.tick;
            }
            if (drift > 1.2f && pf.tick - engine->playDriftLastLogTick >= 240) {
                engine->playDriftLastLogTick = pf.tick;
                skkbot2::log::info(
                    "pre-reconcile physics delta: tick={} dx={:.3f} dy={:.3f} "
                    "(max {:.3f}px@{}; position is reconciled afterward)",
                    pf.tick, dx, dy, engine->playDriftMax, engine->playDriftMaxTick);
            }

            ++engine->perFramePlaybackIndex;
            ++framesAppliedThisTick;
        }

        // BUG-04 study: >1 per-frame consumed in one tick = the recorded state
        // is being applied in a burst N ticks late. That delay is exactly the
        // 'state lands behind the live physics' hypothesis; log bursts.
        if (framesAppliedThisTick > 1 && engine->perFrameReconciliationCatchUps < 20) {
            ++engine->perFrameReconciliationCatchUps;
            skkbot2::log::info(
                "RECON catch-up[{}]: tick={} applied={} frames in one tick (pfIdx={}/{})",
                engine->perFrameReconciliationCatchUps,
                effectiveTick, framesAppliedThisTick,
                engine->perFramePlaybackIndex, perFrames->size());
        }
    }

    // Click dispatch BEFORE the vanilla sub-step (capture consumed the click
    // of tick T during step T; dispatching after the step delayed every
    // playback click by one sub-step - ~2px at max speed, the residual
    // 'dies near the end' phase error). CBS already queues pre-step.
    void dispatchPlaybackInputsPre(int tick, int stepDelta) {
        auto* engine = ReplayEngine::get();
        if (m_levelEndAnimationStarted || !engine->hasMacro()) {
            return;
        }
        if (m_player1->m_isDead) {
            return;
        }

        static int dispatchCount = 0;
        if (++dispatchCount <= 5 || dispatchCount % 240 == 0) {
            auto* sk = engine->activeSk;
            auto* list = sk ? engine->activeSkInputs() : nullptr;
            int first0 = list && list->size() > 0 ? (*list)[0].tick : -1;
            int first1 = list && list->size() > 1 ? (*list)[1].tick : -1;
            int first2 = list && list->size() > 2 ? (*list)[2].tick : -1;
            skkbot2::log::debug("dispatchPre: tick={} effectiveTick={} idx={} inputs={} sk={} anchors={} offset={} first=[{},{},{}]",
                tick, tick + engine->tickOffset, engine->executeIndex,
                sk ? static_cast<int>(sk->inputs.size()) : 0,
                engine->skMode ? 1 : 0,
                sk ? static_cast<int>(sk->anchors.size()) : 0,
                engine->tickOffset, first0, first1, first2);
        }

        int effectiveTick = tick + engine->tickOffset;
        engine->lastDispatchEffectiveTick = effectiveTick;

        // Input dispatch only (frame-boundary).
        if (!shouldUseInputOnlySkPlayback() && !shouldUseInputOnlyGDRPlayback()) {
            m_fields->macroInput = true;
            if (engine->skMode && engine->activeSk) {
                dispatchSkInputs(tick, effectiveTick, stepDelta);
            } else if (engine->activeMacro) {
                dispatchGDRInputs(tick, effectiveTick, stepDelta);
            }
            m_fields->macroInput = false;
        }

        engine->respawnTickIndex = -1;
    }

    void executeTick(int tick, int stepDelta) {
        auto* engine = ReplayEngine::get();
        if (m_levelEndAnimationStarted || !engine->hasMacro()) {
            return;
        }

        if (m_player1->m_isDead) {
            m_player1->releaseAllButtons();
            if (m_player2) m_player2->releaseAllButtons();
            return;
        }

        // After all inputs are exhausted, stop reconciliation and anchors
        // so the game continues normally with user control.
        if (engine->playbackExhaustionNotified) {
            return;
        }

        int effectiveTick = tick + engine->tickOffset;

        reconcileAnchors(effectiveTick);
        // BUG-04 study: probe the raw live state BEFORE reconcile, so a stray
        // GD gravity/ud flip that our application LATER corrects shows up as
        // playRaw(first) vs play(after) on the same tick - the 'state applied
        // late' fingerprint.
        engine->gravStateProbe("playRaw", effectiveTick, m_player1, m_player2);
        engine->gravDiffProbe("raw", effectiveTick, m_player1, m_player2);
        reconcilePerFrames(effectiveTick);
        engine->gravStateProbe("play", effectiveTick, m_player1, m_player2);
        engine->gravDiffProbe("applied", effectiveTick, m_player1, m_player2);
        {
            // Play-side hold probe (mirrors the record-side one in
            // recordSkAnchor): proves, tick by tick, that dispatched inputs
            // actually land on the intended player object.
            auto heldBit = [](PlayerObject* p) -> int {
                if (!p) return -1;
                const auto& hb = p->m_holdingButtons;
                auto it = hb.find(1);
                return it == hb.end() ? 0 : (it->second ? 1 : 0);
            };
            const int hp1 = heldBit(m_player1);
            const int hp2 = heldBit(m_player2);
            if (hp1 != engine->holdProbeLast[0] || hp2 != engine->holdProbeLast[1]) {
                engine->holdProbeLast[0] = hp1;
                engine->holdProbeLast[1] = hp2;
                skkbot2::log::debug("hold probe[play]: tick={} p1={} p2={}", effectiveTick, hp1, hp2);
            }
        }

        // RNG stream lock + audit: anchors carry the level fast_rand state
        // captured at the same post-substep point in recording. Field proof
        // (02.09): one extra setup draw in either session shifted the whole
        // stream by one - every random pick then returned the NEXT recorded
        // value and randomized objects landed in alien positions. Pinning
        // the state to the recorded value at every anchor tick makes the
        // stream equal the recording by construction (GDH's mechanism),
        // whatever the fps/particle noise consumed in between. First
        // mismatches are logged with the detected lag (how many recorded
        // states back the live value sits) to prove the lock bites.
        // Triple RNG: also restore teleport from anchor.
        if (engine->skMode) {
            if (auto* anchors = engine->activeAnchors(); anchors && engine->playbackAnchorIndex > 0) {
                const size_t ai = engine->playbackAnchorIndex - 1;
                auto const& auditAnchor = anchors->at(ai);
                if (auditAnchor.tick == effectiveTick && auditAnchor.rng.fastRandState != 0) {
                    const uintptr_t liveRng = GameToolbox::getfast_srand();
                    if (liveRng != auditAnchor.rng.fastRandState) {
                        if (engine->rngStreamMismatchReports < 6) {
                            ++engine->rngStreamMismatchReports;
                            int lag = -1;
                            for (int back = 1; back <= 3 && ai >= static_cast<size_t>(back); ++back) {
                                if (anchors->at(ai - back).rng.fastRandState == liveRng) {
                                    lag = back;
                                    break;
                                }
                            }
                            skkbot2::log::warn("rng stream relocked: tick={} live=0x{:X} recorded=0x{:X} lag={}",
                                effectiveTick,
                                static_cast<unsigned long long>(liveRng),
                                static_cast<unsigned long long>(auditAnchor.rng.fastRandState),
                                lag);
                        }
                        GameToolbox::fast_srand(auditAnchor.rng.fastRandState);
                    }
                    // Triple RNG: restore teleport from anchor.
                    if (auditAnchor.rng.teleportState != 0) {
                        *skk::gdTeleportRng() = auditAnchor.rng.teleportState;
                    }
                    // And shake, now that it is locked for both SKK and SKC.
                    // Without this the chain drifted across the whole replay
                    // while primary and teleport were re-pinned every tick.
                    if (auditAnchor.rng.shakeState != 0) {
                        skk::shakeStateSet(auditAnchor.rng.shakeState);
                    }
                }
            }
        }

        // One-shot notice when the segment's inputs are exhausted while the
        // player is still alive: every later death is "macro ended", not a
        // desync - the log line tells the two apart.
        // Unblock user input so the player is not frozen in-place.
        if (!engine->playbackExhaustionNotified) {
            bool exhausted = false;
            int totalInputs = 0;
            if (engine->skMode) {
                if (auto* list = engine->activeSkInputs(); list && !list->empty() &&
                    engine->executeIndex >= list->size()) {
                    exhausted = true;
                    totalInputs = static_cast<int>(list->size());
                }
            } else if (engine->activeMacro) {
                auto& inputList = engine->activeMacro->inputs;
                if (!inputList.empty() && engine->executeIndex >= inputList.size()) {
                    exhausted = true;
                    totalInputs = static_cast<int>(inputList.size());
                }
            }
            if (exhausted) {
                engine->playbackExhaustionNotified = true;
                skkbot2::log::warn("PLAY: all {} inputs of the active segment dispatched (tick={}); "
                                   "switching to MODE_DISABLED to hand control to vanilla GD",
                                   totalInputs, effectiveTick);
                m_player1->releaseAllButtons();
                if (m_player2) m_player2->releaseAllButtons();
                engine->inputIgnored = false;
                engine->engineMode = MODE_DISABLED;
            }
        }

        if (engine->shouldTriggerPersistencePlaybackDeath(effectiveTick)) {
            skkbot2::log::info("PLAY: forced chain death at tick={} (recorded attempt death)", effectiveTick);
            if (auto* playLayer = PlayLayer::get()) {
                if (auto* deathPlayer = engine->persistencePlaybackDeathPlayer(playLayer)) {
                    engine->markPersistencePlaybackDeathPending();
                    playLayer->destroyPlayer(deathPlayer, nullptr);
                }
            }
        }
    }

#ifdef GEODE_IS_MACOS
    void macSubstepDispatch() {
        auto* engine = ReplayEngine::get();
        if (!engine || engine->engineMode != MODE_EXECUTE || !engine->hasMacro()) {
            return;
        }
        int tick = tick_util::current(this, engine);
        executeTick(tick, 0);
        processInputOnly(shouldUseInputOnlySkPlayback(), [&](int p) { dispatchInputOnlySkInputs(p); });
        processInputOnly(shouldUseInputOnlyGDRPlayback(), [&](int p) { dispatchGDRInputsOnly(p); });
    }
#endif

    void processQueuedButtons(float dt, bool clearInputQueue) {
        auto* engine = ReplayEngine::get();
        AccuracyMode captureAccuracy = engine->skMode
            ? (engine->activeSk ? engine->activeSk->accuracyMode : engine->selectedAccuracyMode)
            : (engine->activeMacro ? engine->activeMacro->accuracyMode : engine->selectedAccuracyMode);
        bool capturingCBS = engine->engineMode == MODE_CAPTURE &&
            ReplayEngine::runtimeAccuracyModeFor(captureAccuracy) == AccuracyMode::CBS;

        engine->queuedCaptureCommands.clear();
        engine->cbsCaptureProcessingQueue = false;
        if (capturingCBS && !engine->captureIgnored && !engine->simulatingPath) {
            for (auto const& command : m_queuedButtons) {
                int button = static_cast<int>(command.m_button);
                if (isTrackedHoldButton(button)) {
                    engine->queueCbsCaptureCommand(button, command.m_isPush, command.m_isPlayer2, command.m_timestamp);
                }
            }
            engine->cbsCaptureProcessingQueue = true;
        }

        bool previousMacroInput = m_fields->macroInput;
#ifdef GEODE_IS_MACOS
        if (!engine->simulatingPath && !engine->substepMidStep) {
            macSubstepDispatch();
        }
#endif
        // Ignore Inputs: the player's own clicks must not interleave with the
        // macro. Clearing is still the cheapest way to drop them (the drain
        // would swallow them at handleButton anyway, but the queue itself is
        // read by dash-ring code), and it used to also erase the playback
        // mirror pushed at dispatch time. Re-push that mirror right here, after
        // the clear and before the drain, so queue-readers finally see the
        // macro's own events on the tick they belong to.
        const bool dropUserInput =
            engine->engineMode == MODE_EXECUTE && engine->manualInputIgnoredActive();
        if (dropUserInput) {
            m_queuedButtons.clear();
            for (auto const& command : m_fields->queueMirror) {
                m_queuedButtons.push_back(command);
            }
        }
        m_fields->queueMirror.clear();
        const bool previousDraining = m_fields->drainingQueue;
        m_fields->drainingQueue = true;
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        m_fields->drainingQueue = previousDraining;

        processInputOnly(shouldUseInputOnlySkPlayback(), [&](int p) { dispatchInputOnlySkInputs(p); });
        processInputOnly(shouldUseInputOnlyGDRPlayback(), [&](int p) { dispatchGDRInputsOnly(p); });

        m_fields->macroInput = previousMacroInput;
#ifdef GEODE_IS_MACOS
        if (engine->engineMode == MODE_CAPTURE && !engine->simulatingPath && !engine->substepMidStep) {
            captureTick(tick_util::current(this, engine));
        }
#endif
        engine->cbsCaptureProcessingQueue = false;
        engine->queuedCaptureCommands.clear();

        engine->clearQueuedMacroCommands();
    }

    void handleButton(bool hold, int button, bool player2) {
        if (!skk::gameplay::isGameplayActive()) {
            return GJBaseGameLayer::handleButton(hold, button, player2);
        }

        auto* engine = ReplayEngine::get();
        bool macroInput = m_fields->macroInput || engine->substepMacroDispatch;

        if (button == 1 && !macroInput && !Autoclicker::get()->isAutoclickerInput) {
            Autoclicker::get()->trackUserInput(hold, player2);
        }

        // Per-frame sync release: called from processCommands when a physical
        // key was released but GD didn't fire handleButton (no edge transition).
        // We only need to record the release into the macro stream — skip all
        // normal GD processing since the per-frame sync already called
        // releaseButton to clear m_holdingButtons and internal state.
        if (g_perFrameReleaseSync && !hold && engine->engineMode == MODE_CAPTURE && engine->hasMacro()) {
            bool recordPlayer2 = player2;
            if (!keepSecondPlayerSlot(this)) {
                recordPlayer2 = false;
            }
            int recordSlot = recordPlayer2 ? 1 : 0;
            PlayerObject* targetPlayer = recordPlayer2 ? m_player2 : m_player1;
            bool trackHoldTransition = isTrackedHoldButton(button) && targetPlayer;

            auto holdBitOf = [button](PlayerObject* p) -> int {
                if (!p) return 0;
                const auto& hb = p->m_holdingButtons;
                auto it = hb.find(button);
                return it == hb.end() ? 0 : (it->second ? 1 : 0);
            };
            int p1Before = holdBitOf(m_player1);
            int p2Before = holdBitOf(m_player2);
            bool wasHeld = trackHoldTransition ? (targetPlayer ? holdBitOf(targetPlayer) : 0) : false;
            const int paramShadowIndex = recordSlot * 3 + (button - 1);
            const bool shadowHeld = trackHoldTransition && engine->recordedHoldShadow[paramShadowIndex];

            int effSlot = recordSlot;
            int p1After = holdBitOf(m_player1);
            int p2After = holdBitOf(m_player2);
            if (trackHoldTransition && p2After != p2Before && !(p1After != p1Before)) {
                effSlot = 1;
            } else if (trackHoldTransition && p1After != p1Before && !(p2After != p2Before)) {
                effSlot = 0;
            }
            const int effBefore = effSlot == 1 ? p2Before : p1Before;
            const bool effChanged = (effSlot == 1 ? (p2After != p2Before) : (p1After != p1Before));

            bool realTransition = (effChanged ? effBefore == 1 : wasHeld) || shadowHeld;
            if (trackHoldTransition) {
                engine->recordedHoldShadow[paramShadowIndex] = hold;
            }
            if (realTransition && !engine->captureIgnored && !engine->simulatingPath && !m_player1->m_isDead) {
                int recordTick = tick_util::current(this, engine);
                float basePhase = static_cast<float>(std::max(0, m_currentStep - engine->tickStartStep));
                AccuracyMode accuracyMode = engine->skMode
                    ? (engine->activeSk ? engine->activeSk->accuracyMode : engine->selectedAccuracyMode)
                    : (engine->activeMacro ? engine->activeMacro->accuracyMode : engine->selectedAccuracyMode);
                float stepOffset = usesStepBasedAccuracy(accuracyMode) ? basePhase : 0.0f;
                double cbsTimeOffset = -1.0;

                if (usesTimedAccuracy(accuracyMode)) {
                    double wallPhase = engine->cbsPhaseFromInputWall();
                    if (wallPhase >= 0.0) {
                        cbsTimeOffset = wallPhase;
                    }
                }

                if (engine->skMode && engine->activeSk) {
                    if (!usesTimedAccuracy(engine->activeSk->accuracyMode)) {
                        stepOffset = 0.0f;
                        cbsTimeOffset = -1.0;
                    }
                    engine->recordSkAction(recordTick, button, recordPlayer2, hold, stepOffset, cbsTimeOffset);
                } else if (engine->activeMacro) {
                    if (!usesTimedAccuracy(engine->activeMacro->accuracyMode)) {
                        stepOffset = 0.0f;
                    }
                    engine->activeMacro->recordAction(recordTick, button, recordPlayer2, hold, stepOffset);
                }
                skkbot2::log::debug("capture input: tick={} button={} p2={} pressed={} via=releasesync",
                    recordTick, button, recordPlayer2, hold);
                engine->markActiveMacroDirty();
                engine->macroInputActive = true;
                engine->triggerAudio(recordPlayer2, button, hold);
            }
            return;
        }

        // Per-frame sync press: same as release sync but for hold=true. The
        // physical key went down but GD never fired handleButton (e.g. held
        // through death, or GD's edge detection missed the transition). The
        // per-frame sync already called pushButton to set internal state —
        // we just need to record the press into the macro stream.
        if (g_perFramePressSync && hold && engine->engineMode == MODE_CAPTURE && engine->hasMacro()) {
            bool recordPlayer2 = player2;
            if (!keepSecondPlayerSlot(this)) {
                recordPlayer2 = false;
            }
            int recordSlot = recordPlayer2 ? 1 : 0;
            PlayerObject* targetPlayer = recordPlayer2 ? m_player2 : m_player1;
            bool trackHoldTransition = isTrackedHoldButton(button) && targetPlayer;

            auto holdBitOf = [button](PlayerObject* p) -> int {
                if (!p) return 0;
                const auto& hb = p->m_holdingButtons;
                auto it = hb.find(button);
                return it == hb.end() ? 0 : (it->second ? 1 : 0);
            };
            int p1Before = holdBitOf(m_player1);
            int p2Before = holdBitOf(m_player2);
            bool wasHeld = trackHoldTransition ? (targetPlayer ? holdBitOf(targetPlayer) : 0) : false;
            const int paramShadowIndex = recordSlot * 3 + (button - 1);
            const bool shadowHeld = trackHoldTransition && engine->recordedHoldShadow[paramShadowIndex];

            int effSlot = recordSlot;
            int p1After = holdBitOf(m_player1);
            int p2After = holdBitOf(m_player2);
            if (trackHoldTransition && p2After != p2Before && !(p1After != p1Before)) {
                effSlot = 1;
            } else if (trackHoldTransition && p1After != p1Before && !(p2After != p2Before)) {
                effSlot = 0;
            }
            const int effBefore = effSlot == 1 ? p2Before : p1Before;
            const bool effChanged = (effSlot == 1 ? (p2After != p2Before) : (p1After != p1Before));

            bool realTransition = effChanged ? (effBefore == 0) : !wasHeld;
            if (trackHoldTransition) {
                engine->recordedHoldShadow[paramShadowIndex] = hold;
            }
            if (realTransition && !engine->captureIgnored && !engine->simulatingPath && !m_player1->m_isDead) {
                int recordTick = tick_util::current(this, engine);
                float basePhase = static_cast<float>(std::max(0, m_currentStep - engine->tickStartStep));
                AccuracyMode accuracyMode = engine->skMode
                    ? (engine->activeSk ? engine->activeSk->accuracyMode : engine->selectedAccuracyMode)
                    : (engine->activeMacro ? engine->activeMacro->accuracyMode : engine->selectedAccuracyMode);
                float stepOffset = usesStepBasedAccuracy(accuracyMode) ? basePhase : 0.0f;
                double cbsTimeOffset = -1.0;

                if (usesTimedAccuracy(accuracyMode)) {
                    double wallPhase = engine->cbsPhaseFromInputWall();
                    if (wallPhase >= 0.0) {
                        cbsTimeOffset = wallPhase;
                    }
                }

                if (engine->skMode && engine->activeSk) {
                    if (!usesTimedAccuracy(engine->activeSk->accuracyMode)) {
                        stepOffset = 0.0f;
                        cbsTimeOffset = -1.0;
                    }
                    engine->recordSkAction(recordTick, button, recordPlayer2, hold, stepOffset, cbsTimeOffset);
                } else if (engine->activeMacro) {
                    if (!usesTimedAccuracy(engine->activeMacro->accuracyMode)) {
                        stepOffset = 0.0f;
                    }
                    engine->activeMacro->recordAction(recordTick, button, recordPlayer2, hold, stepOffset);
                }
                skkbot2::log::debug("capture input: tick={} button={} p2={} pressed={} via=presssync",
                    recordTick, button, recordPlayer2, hold);
                engine->markActiveMacroDirty();
                engine->macroInputActive = true;
                engine->triggerAudio(recordPlayer2, button, hold);
            }
            return;
        }

        if (engine->engineMode == MODE_DISABLED) {
            const bool prevInVanilla = g_diagInVanillaHandleButton;
            g_diagInVanillaHandleButton = true;
            GJBaseGameLayer::handleButton(hold, button, player2);
            g_diagInVanillaHandleButton = prevInVanilla;
            engine->triggerAudio(player2, button, hold);
            return;
        }

        if (engine->engineMode == MODE_EXECUTE) {
            if (engine->manualInputIgnoredActive() && !macroInput) {
                return;
            }
            const bool prevInVanilla = g_diagInVanillaHandleButton;
            g_diagInVanillaHandleButton = true;
            GJBaseGameLayer::handleButton(hold, button, player2);
            g_diagInVanillaHandleButton = prevInVanilla;
            if (!macroInput) {
                engine->triggerAudio(player2, button, hold);
            }
            return;
        }

        if (shouldBlockDeferredCapture(this, engine, button, hold, player2, tick_util::current(this, engine))) {
            return;
        }

        if (engine->engineMode != MODE_CAPTURE || !engine->hasMacro()) {
            const bool prevInVanilla = g_diagInVanillaHandleButton;
            g_diagInVanillaHandleButton = true;
            GJBaseGameLayer::handleButton(hold, button, player2);
            g_diagInVanillaHandleButton = prevInVanilla;
            engine->triggerAudio(player2, button, hold);
            return;
        }

        bool recordPlayer2 = player2;
        if (!keepSecondPlayerSlot(this)) {
            recordPlayer2 = false;
        }

        int recordSlot = recordPlayer2 ? 1 : 0;
        PlayerObject* targetPlayer = recordPlayer2 ? m_player2 : m_player1;
        bool trackHoldTransition = isTrackedHoldButton(button) && targetPlayer;
        // Game-side control flip makes handleButton apply its player2 PARAM to
        // the OPPOSITE player object. Reading held state from the param object
        // misjudged real presses as duplicates ('capture drop' at the flipped
        // era deaths) and orphaned the releases. Diff BOTH players' hold maps
        // around the vanilla call and judge the transition on the object that
        // actually changed; the stored input keeps the raw param so playback
        // reproduces GD's own mapping.
        auto holdBitOf = [button](PlayerObject* p) -> int {
            if (!p) return 0;
            const auto& hb = p->m_holdingButtons;
            auto it = hb.find(button);
            return it == hb.end() ? 0 : (it->second ? 1 : 0);
        };
        int p1Before = holdBitOf(m_player1);
        int p2Before = holdBitOf(m_player2);
        bool wasHeld = trackHoldTransition ? (targetPlayer ? holdBitOf(targetPlayer) : 0) : false;
        // Shadow of what we last recorded as held for this slot/button (see
        // ReplayEngine::recordedHoldShadow). Game's m_holdingButtons can be
        // cleared internally before the physical release reaches us (wave
        // flips, gate steps), and relying on it alone silently drops releases.
        const int paramShadowIndex = recordSlot * 3 + (button - 1);
        const bool shadowHeld = trackHoldTransition && engine->recordedHoldShadow[paramShadowIndex];
        AccuracyMode accuracyMode = engine->skMode
            ? (engine->activeSk ? engine->activeSk->accuracyMode : engine->selectedAccuracyMode)
            : (engine->activeMacro ? engine->activeMacro->accuracyMode : engine->selectedAccuracyMode);
        bool recordingCBS = ReplayEngine::runtimeAccuracyModeFor(accuracyMode) == AccuracyMode::CBS;
        auto exactCbsCommand = recordingCBS
            ? engine->popCbsCaptureCommand(button, hold, player2)
            : std::optional<ReplayEngine::QueuedCaptureCommand> {};

        // Raw event tap (capture): every handleButton the game really got,
        // with the PRE-call hold bit of its target slot. The week-long 'p2
        // press did not activate the trigger' class needs to be placed in
        // exactly one of two boxes: (a) GD never delivered the touch (this
        // line absent => cocos/dual touch ate it - not a capture bug), or
        // (b) the event arrived but capture filtered/recorded it wrong.
        if (engine->engineMode == MODE_CAPTURE && button <= 2) {
            skkbot2::log::debug(
                "raw btn: tick={} down={} button={} p2={} slot={} preHold={} dual={} dead={} ignore={}",
                tick_util::current(this, engine), hold, button, player2, recordSlot,
                trackHoldTransition ? (wasHeld ? 1 : 0) : -1,
                keepSecondPlayerSlot(this) ? 1 : 0,
                m_player1 && m_player1->m_isDead ? 1 : 0,
                engine->captureIgnored ? 1 : 0);
        }

        // Phantom-input suppression window (post-death / post-restore flush):
        // phantom PRESSES are dropped BEFORE vanilla so the respawned player
        // never jumps on its own. A human press within ~33ms of a restart is
        // not physically meaningful; GD's map stays 0 either way.
        const int eventTick = tick_util::current(this, engine);

        const bool prevInVanilla = g_diagInVanillaHandleButton;
        g_diagInVanillaHandleButton = true;
        GJBaseGameLayer::handleButton(hold, button, player2);
        g_diagInVanillaHandleButton = prevInVanilla;

        int p1After = holdBitOf(m_player1);
        int p2After = holdBitOf(m_player2);
        const bool p1Changed = p1After != p1Before;
        const bool p2Changed = p2After != p2Before;
        // Effective object = whose map actually changed under GD's internal
        // control flip (probe-proven: every event lands on the opposite
        // object when game var 0010 is on). Judge the transition there.
        int effSlot = recordSlot;
        if (trackHoldTransition && p2Changed && !p1Changed) {
            effSlot = 1;
        } else if (trackHoldTransition && p1Changed && !p2Changed) {
            effSlot = 0;
        }
        const int effBefore = effSlot == 1 ? p2Before : p1Before;
        const bool effChanged = (effSlot == 1 ? p2Changed : p1Changed);

        // Checkpoint-restore seam: GD flushes stale buffered presses AND
        // the stale physical releases right after loadFromCheckpoint. The
        // suppression window drops ONLY state no-op releases: by the time the
        // flush arrives the restore has already wiped the hold maps
        // (clearRestoredHolds) - a flush release finds the map at 0 and
        // nothing recorded, so effBefore==0 && !shadowHeld. Such releases are
        // exactly what the no-transition gate would reject anyway.
        // BUG-103 class (fixed in SKC, live evidence release@1681 walked
        // exactly onto the window edge): a GENUINE quick release inside the
        // window - the tap whose press was recorded at the boundary - used to
        // be dropped WITHOUT updating recordedHoldShadow, clipping the tap
        // tail and leaving the shadow stale for the wave-flip flush paths.
        // A release that actually releases a recorded hold (effBefore==1 or
        // shadowHeld) passes the window and goes through the normal edge
        // logic below.
        if (!hold && engine->captureReleaseSuppressUntil >= eventTick) {
            if (effBefore == 1 || shadowHeld) {
                skkbot2::log::debug(
                    "capture [restore-window] genuine release PASSES: tick={} button={} p2={} "
                    "effSlot={} effBefore={} shadowHeld={}",
                    eventTick, button, player2, effSlot, effBefore, shadowHeld ? 1 : 0);
            } else {
                skkbot2::log::debug("capture drop [restore-window]: tick={} button={} p2={} down=false effSlot={} (post-restore release, not recorded)",
                    eventTick, button, player2, effSlot);
                return;
            }
        }

        if (trackHoldTransition && !hold && wasHeld && engine->isCheckpointHoldResumed(recordSlot, button)) {
            engine->setCheckpointHoldResumed(recordSlot, button, false);
        }

        bool realTransition;
        if (!trackHoldTransition) {
            realTransition = true;
        } else if (hold) {
            realTransition = effChanged ? effBefore == 0 : !wasHeld;
        } else {
            // Release: record it when EITHER the effective object actually
            // released (or still shows held) OR we recorded a press that has
            // not been released yet (game can clear holds on wave flips).
            realTransition = (effChanged ? effBefore == 1 : wasHeld) || shadowHeld;
        }
        if (trackHoldTransition) {
            engine->recordedHoldShadow[paramShadowIndex] = hold;
        }
        if (!realTransition) {
            skkbot2::log::debug("capture drop: tick={} button={} p2={} down={} wasHeld={} shadowHeld={} "
                                "effSlot={} effBefore={} p1={}→{} p2={}→{} (NOT recorded)",
                tick_util::current(this, engine), button, player2, hold, wasHeld ? 1 : 0, shadowHeld ? 1 : 0,
                effSlot, effBefore, p1Before, p1After, p2Before, p2After);
            return;
        }

        if (!engine->captureIgnored && !engine->simulatingPath && !m_player1->m_isDead) {
            int recordTick = tick_util::current(this, engine);
            float stepOffset = 0.0f;
            double cbsTimeOffset = -1.0;
            float basePhase = static_cast<float>(std::max(0, m_currentStep - engine->tickStartStep));

            if (usesStepBasedAccuracy(accuracyMode)) {
                stepOffset = basePhase;
            }
            if (exactCbsCommand || recordingCBS) {
                // CBS sub-tick phase. GD's PlayerButtonCommand timestamps are
                // step-aligned (command.m_timestamp == m_timestamp, frozen
                // between processCommands calls), so exactCbsCommand->timestamp
                // - tickStartTimestamp is structurally ~0 and a recorded CBS
                // macro lost its real within-tick phase. Use the WALL-clock
                // arrival stamp from the UILayer hooks instead — it reflects
                // where inside the tick the key/touch physically landed.
                double wallPhase = engine->cbsPhaseFromInputWall();
                if (wallPhase >= 0.0) {
                    cbsTimeOffset = wallPhase;
                    skkbot2::log::debug("capture CBS wall-phase: tick={} button={} p2={} down={} phase={:.6f}",
                        recordTick, button, recordPlayer2, hold, wallPhase);
                }
                if (cbsTimeOffset < 0.0 && exactCbsCommand) {
                    double offset = exactCbsCommand->timestamp - engine->tickStartTimestamp;
                    if (std::isfinite(offset) && offset >= 0.0) {
                        cbsTimeOffset = offset;
                    }
                } else if (cbsTimeOffset < 0.0 && recordingCBS) {
                    double offset = m_timestamp - engine->tickStartTimestamp;
                    if (std::isfinite(offset) && offset >= 0.0) {
                        cbsTimeOffset = offset;
                    }
                }
            }

            if (engine->skMode && engine->activeSk) {
                if (!usesTimedAccuracy(engine->activeSk->accuracyMode)) {
                    stepOffset = 0.0f;
                    cbsTimeOffset = -1.0;
                }
                engine->recordSkAction(recordTick, button, recordPlayer2, hold, stepOffset, cbsTimeOffset);
                skkbot2::log::debug("capture input: tick={} button={} p2={} pressed={} persistence={} shadow={} step={:.2f} cbs={:.6f} via=raw",
                    recordTick, button, recordPlayer2, hold,
                    engine->canUsePersistenceCapture() ? 1 : 0, shadowHeld ? 1 : 0,
                    stepOffset, cbsTimeOffset);
            } else if (engine->activeMacro) {
                if (!usesTimedAccuracy(engine->activeMacro->accuracyMode)) {
                    stepOffset = 0.0f;
                }
                engine->activeMacro->recordAction(recordTick, button, recordPlayer2, hold, stepOffset);
            }

            engine->markActiveMacroDirty();
            engine->macroInputActive = true;
            engine->triggerAudio(recordPlayer2, button, hold);
        }
    }
};

class $modify(MacroEnginePlayerObject, PlayerObject) {
    void update(float stepDelta) {
        skkbot2::hot::Timed skkbot2Count(skkbot2::hot::MacroPlayerUpdate);
        auto* playLayer = PlayLayer::get();
        auto* engine = ReplayEngine::get();

        if (!playLayer || !engine || engine->simulatingPath || !engine->hasQueuedSubstepTick()) {
            if (!engine || !engine->substepMidStep) {
                resetQueuedSubstepVisualState();
            }
            return PlayerObject::update(stepDelta);
        }

        if (engine->substepMidStep || this != playLayer->m_player1) {
            return PlayerObject::update(stepDelta);
        }

        engine->simulateQueuedSubstepTick(
            playLayer,
            playLayer->m_player1,
            playLayer->m_player2,
            stepDelta,
            engine->queuedSubstepSegments
        );
    }

    void updateRotation(float t) {
        auto* playLayer = PlayLayer::get();
        auto* engine = ReplayEngine::get();

        if (playLayer && engine && !engine->substepMidStep && this == playLayer->m_player1 && g_substepP1Split) {
            PlayerObject::updateRotation(g_substepRotationDelta);
            g_substepP1Split = false;
            return;
        }

        if (playLayer && engine && !engine->substepMidStep && playLayer->m_player2 && this == playLayer->m_player2 && g_substepP2Split) {
            PlayerObject::updateRotation(g_substepRotationDelta);
            g_substepP2Split = false;
            return;
        }

        PlayerObject::updateRotation(t);
    }

    // DIAGNOSTIC probes on the raw player API. The infinite-hold report shows
    // clean m_holdingButtons after a practice respawn yet the character keeps
    // jumping by itself. m_holdingButtons is only mutated by GD's dispatch:
    // real input => vanilla handleButton (g_diagInVanillaHandleButton true),
    // synthesized replay => our layer->handleButton (also true), everything
    // else (autoclicker, direct pushButton, post-restore residue) shows up as
    // direct=false. Logging who pressed, which player, and the resulting hold
    // map tells us if the self-jump originates OUTSIDE the button system.
    void reportButtonCall(const char* kind, int button) {
        // Level gate FIRST: the lines below build tick stamps and dig through
        // two m_holdingButtons maps per call, at up to ~14k calls a second in
        // a dual playback. wouldLog(Debug) is one integer compare; everything
        // past it is wasted when the flood is not being written.
        if (!skkbot2::Logger::get().wouldLog(skkbot2::Logger::Debug)) {
            return;
        }
        auto* playLayer = PlayLayer::get();
        if (!playLayer) {
                return;
        }
        auto* engine = ReplayEngine::get();
        const bool isP1 = this == playLayer->m_player1;
        const bool isP2 = playLayer->m_player2 && this == playLayer->m_player2;
            int tick = 0;
        if (engine) {
            if (engine->stepClock) {
                tick = std::max(0, engine->stepPairCount + 1);
            } else {
                tick = std::max(0, static_cast<int>(playLayer->m_gameState.m_levelTime * engine->runtimeTickRate()) + 1);
            }
        }
        int engineMode = engine ? static_cast<int>(engine->engineMode) : -1;
        int holdP1 = -1;
        int holdP2 = -1;
        if (playLayer->m_player1) {
            const auto& hb = playLayer->m_player1->m_holdingButtons;
            auto it = hb.find(1);
            holdP1 = it == hb.end() ? 0 : (it->second ? 1 : 0);
        }
        if (playLayer->m_player2) {
            const auto& hb = playLayer->m_player2->m_holdingButtons;
            auto it = hb.find(1);
            holdP2 = it == hb.end() ? 0 : (it->second ? 1 : 0);
        }
        // Hot path diag debug log disabled to prevent 7 FPS disk stalls
        (void)kind; (void)tick; (void)button; (void)isP1; (void)isP2; (void)holdP1; (void)holdP2;
    }

    bool pushButton(PlayerButton button) {
        if (auto* playLayer = PlayLayer::get(); playLayer && playLayer->m_gameState.m_isDualMode) {
            reportButtonCall("push", static_cast<int>(button));
        }
        return PlayerObject::pushButton(button);
    }

    bool releaseButton(PlayerButton button) {
        if (auto* playLayer = PlayLayer::get(); playLayer && playLayer->m_gameState.m_isDualMode) {
            reportButtonCall("release", static_cast<int>(button));
        }
        return PlayerObject::releaseButton(button);
    }

};

// Stamps the WALL clock on every physical input arrival. GD's own
// PlayerButtonCommand timestamps are step-aligned (m_timestamp is frozen
// between processCommands calls), so the only true source of a sub-tick phase
// is the moment the raw key/touch actually lands on UILayer. These hooks fire
// BEFORE the queued button is dispatched to handleButton (the capture path
// then reads ReplayEngine::cbsPhaseFromInputWall()).
class $modify(MacroUiInputWall, UILayer) {
    void keyDown(cocos2d::enumKeyCodes key, double timestamp) {
        if (auto* engine = ReplayEngine::get()) {
            engine->inputArrivalWallTime = ReplayEngine::wallClockSeconds();
            engine->hasInputArrivalWallTime = true;
        }
        skkbot2::notePlayerInput("key down", static_cast<int>(key));
        UILayer::keyDown(key, timestamp);
    }

    void keyUp(cocos2d::enumKeyCodes key, double timestamp) {
        if (auto* engine = ReplayEngine::get()) {
            engine->inputArrivalWallTime = ReplayEngine::wallClockSeconds();
            engine->hasInputArrivalWallTime = true;
        }
        skkbot2::notePlayerInput("key up", static_cast<int>(key));
        UILayer::keyUp(key, timestamp);
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) {
        if (auto* engine = ReplayEngine::get()) {
            engine->inputArrivalWallTime = ReplayEngine::wallClockSeconds();
            engine->hasInputArrivalWallTime = true;
        }
        skkbot2::notePlayerInput("touch down", 0);
        return UILayer::ccTouchBegan(touch, event);
    }

    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) {
        if (auto* engine = ReplayEngine::get()) {
            engine->inputArrivalWallTime = ReplayEngine::wallClockSeconds();
            engine->hasInputArrivalWallTime = true;
        }
        skkbot2::notePlayerInput("touch up", 0);
        UILayer::ccTouchEnded(touch, event);
    }
};
