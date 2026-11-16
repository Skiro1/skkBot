#include "skkBot.hpp"
#include "core/gameplay_layer.hpp"
#include "core/start_position_policy.hpp"
#include "physics/anti_ssb.hpp"
#include "core/checkpoint_handler.hpp"
#include "core/checkpoint_creation.hpp"
#include "core/seam_phase_probe.hpp"
#include "core/step_trace.hpp"
#include "core/broken_object_tracker.hpp"
#include "core/physical_keys.hpp"
#include "core/capture_reconciliation.hpp"
#include "core/paths.hpp"
#include "core/practice_fix_bridge.hpp"
#include "core/playback_align.hpp"
#include "hacks/physicsbypass.hpp"
#include "render/renderer.hpp"
#include "trajectory/trajectory.hpp"
#include "skc/skc_bot.hpp"
#include "skc/skc_checkpoint.hpp"
#include "skc/skc_frame.hpp"

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

// Playback alignment helpers (position matching, tick baselines, coverage
// tolerances) moved to core/playback_align.hpp so the editor playtest path can
// reuse the exact logic that powers the PlayLayer attempt takeovers.
using namespace skk::playback_align;

#include "core/frame_stepper.hpp"

#include <Geode/utils/casts.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <random>
#include <thread>

using namespace geode::prelude;

namespace {
    void logCheckpointRngPhase(char const* phase, CheckpointStateBundle const& state) {
        const auto live = skk::captureTripleRng();
        skkbot2::log::info(
            "[CK-RNG] phase={} tick={} prior={} live=({:X},{:X},{:X}) snap=({:X},{:X},{:X}) wire={}",
            phase, state.tick, state.priorTick, live.primary, live.teleport,
            skk::shakeStateGet(), state.rng.fastRandState, state.rng.teleportState,
            state.rng.shakeState, !state.hasPlayerState);
    }

    void incrementSaturated(int& value) {
        if (value < std::numeric_limits<int>::max()) {
            ++value;
        }
    }

    void triggerNoclipDeathFlash(ReplayEngine* engine) {
        if (!engine || !engine->noclipDeathFlash) {
            return;
        }

        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) {
            return;
        }

        auto* overlay = static_cast<CCLayerColor*>(scene->getChildByTag(39182));
        if (!overlay) {
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            overlay = CCLayerColor::create(ccc4(
                static_cast<int>(engine->noclipDeathColorR * 255),
                static_cast<int>(engine->noclipDeathColorG * 255),
                static_cast<int>(engine->noclipDeathColorB * 255),
                0
            ));
            overlay->setContentSize(winSize);
            overlay->setTag(39182);
            overlay->setZOrder(9999);
            overlay->setPosition(0, 0);
            scene->addChild(overlay);
        }

        overlay->setColor(ccc3(
            static_cast<int>(engine->noclipDeathColorR * 255),
            static_cast<int>(engine->noclipDeathColorG * 255),
            static_cast<int>(engine->noclipDeathColorB * 255)
        ));
        overlay->stopAllActions();
        overlay->setOpacity(100);
        overlay->runAction(CCFadeTo::create(0.25f, 0));
    }

    bool isNoclipAccuracyFrameActive(PlayLayer* playLayer, ReplayEngine* engine) {
        if (!playLayer || !engine || !engine->collisionBypass) {
            return false;
        }
        if (!engine->noclipPlayer1 && !engine->noclipPlayer2) {
            return false;
        }
        if (skk::trajectory::isSimulating()) {
            return false;
        }

        bool playerDead = (playLayer->m_player1 && playLayer->m_player1->m_isDead)
            || (playLayer->m_player2 && playLayer->m_player2->m_isDead);
        return !playerDead && !playLayer->m_hasCompletedLevel && !playLayer->m_levelEndAnimationStarted;
    }

    bool shouldNoclipPlayer(PlayLayer* playLayer, PlayerObject* player, ReplayEngine* engine) {
        if (!playLayer || !player || !engine || !engine->collisionBypass) {
            return false;
        }
        if (player == playLayer->m_player1) {
            return engine->noclipPlayer1;
        }
        if (player == playLayer->m_player2) {
            return engine->noclipPlayer2;
        }
        return false;
    }

    void clearPendingNoclipFrame(ReplayEngine* engine) {
        if (!engine) {
            return;
        }

        engine->noclipWouldDieThisFrame = false;
        engine->noclipDeadLastFrame = false;
        engine->noclipLastDeathPlayer = nullptr;
        engine->noclipLastDeathObject = nullptr;
    }

    void processNoclipAccuracyFrame(PlayLayer* playLayer) {
        auto* engine = ReplayEngine::get();

        if (!isNoclipAccuracyFrameActive(playLayer, engine)) {
            clearPendingNoclipFrame(engine);
            return;
        }

        incrementSaturated(engine->noclipTotalFrames);

        bool unsafeFrame = engine->noclipWouldDieThisFrame;
        if (unsafeFrame) {
            incrementSaturated(engine->noclipUnsafeFrames);
            if (!engine->noclipDeadLastFrame) {
                incrementSaturated(engine->noclipDeathEvents);
                triggerNoclipDeathFlash(engine);
            }
        }

        engine->noclipDeadLastFrame = unsafeFrame;
        engine->noclipWouldDieThisFrame = false;

        auto* deathPlayer = engine->noclipLastDeathPlayer ? engine->noclipLastDeathPlayer : playLayer->m_player1;
        auto* deathObject = engine->noclipLastDeathObject;
        engine->noclipLastDeathPlayer = nullptr;
        engine->noclipLastDeathObject = nullptr;

        if (!unsafeFrame || !engine->collisionLimitActive || engine->collisionThreshold <= 0.0f) {
            return;
        }

        if (engine->noclipAccuracyPercent() < engine->collisionThreshold) {
            bool restoreNoclip = engine->collisionBypass;
            engine->collisionBypass = false;
            engine->clearFrameStepState();
            engine->resetNoclipAccuracyStats();
            playLayer->destroyPlayer(deathPlayer, deathObject);
            engine->collisionBypass = restoreNoclip;
        }
    }
}

static uint32_t computeLockedSeed(uint32_t seedSalt, int currentProgress) {
    std::mt19937 mixer(static_cast<unsigned>(seedSalt + currentProgress));
    std::uniform_int_distribution<uint64_t> dist(10000, 999999999);
    return static_cast<uint32_t>(dist(mixer));
}

static uint32_t computeFallbackSeed() {
    std::random_device device;
    std::mt19937 mixer(device());
    std::uniform_int_distribution<uint64_t> dist(1000, 999999999);
    return static_cast<uint32_t>(dist(mixer));
}

void refreshRngState(bool isLevelStart) {
    auto* engine = ReplayEngine::get();
    auto* playLayer = PlayLayer::get();
    if (!playLayer) {
        return;
    }

    if (engine->rngLocked) {
        const uint64_t seedBase =
            (engine->skMode && engine->activeSk && engine->activeSk->rngSeed != 0)
                ? engine->activeSk->rngSeed
                : engine->rngSeedVal;
        const bool attemptStart = playLayer->m_gameState.m_currentProgress == 0;
        if (!attemptStart && !isLevelStart && !playLayer->m_player1->m_isDead) {
            return;
        }
        uint64_t computedSeed = !playLayer->m_player1->m_isDead
            ? computeLockedSeed(seedBase, playLayer->m_gameState.m_currentProgress)
            : computeFallbackSeed();
        GameToolbox::fast_srand(computedSeed);

        // Triple RNG lock: pin teleport at attempt/level start.
        // Use the same seed derivation so all channels are deterministic.
        if (isLevelStart || attemptStart) {
            uint64_t teleportSeed = skk::lcgAdvance(computedSeed);
            skk::pinTripleSeedAtReset(computedSeed, teleportSeed);

            // Reseed per-object advanced randoms (2068/1912/3607).
            skk::advRandomReseed(computedSeed);

            skkbot2::log::info("rng lock armed: seedBase={} progress={} -> "
                "primary=0x{:X} teleport=0x{:X}",
                seedBase, playLayer->m_gameState.m_currentProgress,
                computedSeed, teleportSeed);

            // Splice boundary fallback (capture): a practice respawn re-arms
            // mid-run. Latch the triple we just seeded here as the default;
            // the checkpoint-restore latch that follows overrides it with the
            // LIVE post-restore globals. If no restore runs (unknown checkpoint)
            // this seed-family snapshot is what the segment actually starts from.
            if (engine->engineMode == MODE_CAPTURE && !attemptStart) {
                auto triple = skk::captureTripleRng();
                engine->pendingRngBoundary.progress = playLayer->m_gameState.m_currentProgress;
                engine->pendingRngBoundary.primary = triple.primary;
                engine->pendingRngBoundary.teleport = triple.teleport;
            }
        }
    }

    if (isLevelStart && engine->engineMode == MODE_CAPTURE) {
        engine->capturedRngState = GameToolbox::getfast_srand();
    }
}

static void resetDeferredInputs(ReplayEngine* engine) {
    engine->deferredReleaseA[0] = -1;
    engine->deferredReleaseA[1] = -1;
    engine->deferredInputTick[0] = -1;
    engine->deferredInputTick[1] = -1;
    engine->lateralInputPending[0] = false;
    engine->lateralInputPending[1] = false;
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex) {
        for (int holdIndex = 0; holdIndex < 2; ++holdIndex) {
            engine->deferredReleaseB[playerIndex][holdIndex] = -1;
        }
    }
}

void ReplayEngine::resetDeferredInputState() {
    skipTickIndex = -1;
    skipActionTick = -1;
    clearCheckpointResumedHolds();
    clearQueuedSubstepState();
    clearQueuedMacroCommands();
    queuedCaptureCommands.clear();
    cbsCaptureProcessingQueue = false;
    resetDeferredInputs(this);
}

void ReplayEngine::buildQueuedSubstepSegments(
    std::vector<QueuedSubstepInput> actions,
    std::deque<QueuedSubstepSegment>& output
) const {
    output.clear();
    if (actions.empty()) {
        return;
    }

    float elapsed = 0.0f;
    for (auto& action : actions) {
        if (!std::isfinite(action.offset)) {
            action.offset = 0.0f;
        }
        action.offset = std::clamp(action.offset, 0.0f, 1.0f);

        QueuedSubstepSegment segment;
        segment.deltaFactor = std::max(0.0f, action.offset - elapsed);
        segment.endStep = false;
        segment.hasActionAfter = true;
        segment.actionAfter = action;
        output.push_back(segment);
        elapsed = std::max(elapsed, action.offset);
    }

    QueuedSubstepSegment finalSegment;
    finalSegment.deltaFactor = std::max(0.0f, 1.0f - elapsed);
    finalSegment.endStep = true;
    output.push_back(finalSegment);
}

void ReplayEngine::armQueuedSubstepTick(std::vector<QueuedSubstepInput> actions) {
    if (actions.empty()) {
        return;
    }

    buildQueuedSubstepSegments(std::move(actions), queuedSubstepSegments);
}

bool ReplayEngine::saveActiveMacro() {
    if (engineMode == MODE_CAPTURE) {
        captureVarianceSnapshot();
    }
    flushPendingPersistenceCaptureAttempt();
    captureFlushStagingAll();
    if ((skMode && activeSk) || (!activeMacro && activeSk)) {
        if (!activeSk || (activeSk->inputs.empty() && activeSk->persistenceAttempts.empty())) {
            dataModified = false;
            return false;
        }

        // Live restore points are authoritative when the level is still open.
        // After a level exit, onQuit already materialized them into the macro
        // and cleared the map (BUG-19); re-capturing then would overwrite the
        // preserved set with an empty one and drop every practice-fix
        // checkpoint recorded this session.
        if (!storedRestorePoints.empty() || activeSk->practiceFixCheckpoints.empty()) {
            activeSk->practiceFixCheckpoints = skk::bridge::captureRestorePointsToWire(
                storedRestorePoints, activeSk->framerate > 0.0 ? activeSk->framerate : tickRate);
        }
        skkbot2::log::info("P1 save: practiceFixCheckpoints written={}", activeSk->practiceFixCheckpoints.size());

        if (macroSaveInFlight) {
            skkbot2::log::warn("P1 save: a macro save is already in flight - duplicate skipped");
            return false;
        }

        // Snapshot the macro on the game thread (cheap vector copies), then
        // serialize + zstd-compress + write on a worker thread so a several-MB
        // .skk never blocks the game loop. Only the immutable write metadata is
        // merged back, on the main thread - never the recorded streams, so a
        // concurrent recording cannot be clobbered by an in-flight snapshot.
        SkMacro snapshot = *activeSk;
        snapshot.author = GJAccountManager::get()->m_username;
        snapshot.recordTimestamp = static_cast<int64_t>(std::time(nullptr));
        snapshot.fileFormat = SkFileFormat::SKK3;
        normalizeSkPersistenceTiming(snapshot);
        double safeFramerate = std::isfinite(snapshot.framerate) && snapshot.framerate > 0.0
            ? snapshot.framerate
            : 240.0;
        snapshot.duration = 0.0;
        for (auto const& input : snapshot.inputs) {
            double inputTime = input.hasAbsoluteTime()
                ? input.timeSeconds
                : static_cast<double>(std::max(0, input.tick)) / safeFramerate;
            if (!input.hasAbsoluteTime() && std::isfinite(input.cbsTimeOffset) && input.cbsTimeOffset >= 0.0) {
                inputTime += input.cbsTimeOffset;
            }
            snapshot.duration = std::max(snapshot.duration, inputTime);
        }

        SkMacro* live = activeSk;
        const auto savedGeneration = macroGeneration;
        const auto savedRevision = macroRevision;
        const auto originalName = activeSk->name;
        const bool savedWhileRecording = engineMode == MODE_CAPTURE;
        macroSaveInFlight = true;
        std::thread worker([this, live, savedGeneration, savedRevision, originalName, savedWhileRecording, snapshot = std::move(snapshot)]() mutable {
            std::error_code ec;
            auto directory = skkbot2::paths::replays();
            if (!std::filesystem::exists(directory, ec)) {
                std::filesystem::create_directories(directory, ec);
            }
            bool ok = false;
            std::string resolvedName = snapshot.name;
            if (!ec) {
                skkbot2::log::info("[SAVE] worker: makeUniqueReplayName ENTRY name={}", snapshot.name);
                skkbot2::Logger::get().flush();
                // Exact name+.skk match gets _n+1, always - see makeUniqueReplayName.
                snapshot.name = ReplayStorage::makeUniqueReplayNameInDirectory(
                    directory, snapshot.name, ".skk");
                snapshot.persistedName = snapshot.name;
                resolvedName = snapshot.name;
                skkbot2::log::info("[SAVE] worker: makeUniqueReplayName EXIT name={}", snapshot.name);
                skkbot2::Logger::get().flush();
                ok = snapshot.saveToPath(directory / skk::stringToPath(snapshot.name + ".skk"));
                skkbot2::log::info("[SAVE] worker saveToPath returned ok={}", (bool)ok);
                skkbot2::Logger::get().flush();
            } else {
                skkbot2::log::error("Could not prepare replay directory '{}': {}", skk::pathToUtf8(directory), ec.message());
            }
            geode::queueInMainThread([this, live, savedGeneration, savedRevision, originalName, savedWhileRecording, ok, resolvedName = std::move(resolvedName), snapshot = std::move(snapshot)]() mutable {
                skkbot2::log::info("[SAVE] main-thread callback ENTRY ok={}", (bool)ok);
                skkbot2::Logger::get().flush();
                macroSaveInFlight = false;
                if (!ok) {
                    skkbot2::log::error("Could not save macro '{}'", resolvedName);
                    Notification::create("Failed to save macro", NotificationIcon::Error)->show();
                    return;
                }
                skkbot2::log::info("[SAVE] main-thread callback: merging metadata into activeSk");
                skkbot2::Logger::get().flush();
                // Only merge scalar write metadata. If activeSk was replaced
                // (Clear/Load) while the save ran, skip - the file is still
                // valid on disk and the new macro owns the name.
                const bool unchanged = skMode && activeSk && activeSk == live &&
                    macroGeneration == savedGeneration && macroRevision == savedRevision &&
                    activeSk->name == originalName && !savedWhileRecording && engineMode != MODE_CAPTURE;
                if (unchanged) {
                    activeSk->name = snapshot.name;
                    activeSk->persistedName = snapshot.persistedName;
                    activeSk->duration = snapshot.duration;
                    activeSk->recordTimestamp = snapshot.recordTimestamp;
                    activeSk->fileFormat = SkFileFormat::SKK3;
                    activeSk->accuracyMode = snapshot.accuracyMode;
                    activeSk->exactCbsTiming = snapshot.exactCbsTiming;
                    dataModified = false;
                }
                skkbot2::log::info("[SAVE] callback: metadataMerged={} generation={}/{} revision={}/{} dirty={}",
                    unchanged, macroGeneration, savedGeneration, macroRevision, savedRevision, dataModified);
                skkbot2::Logger::get().flush();
                reloadMacroList();
                skkbot2::log::info("[SAVE] main-thread callback: notification");
                skkbot2::Logger::get().flush();
                Notification::create(
                    fmt::format("Macro saved: {}", resolvedName),
                    NotificationIcon::Success)->show();
                skkbot2::log::info("[SAVE] main-thread callback EXIT");
                skkbot2::Logger::get().flush();
            });
        });
        worker.detach();
        return true;
    }

    if (activeMacro) {
        if (activeMacro->inputs.empty()) {
            dataModified = false;
            return false;
        }

        bool const useJson = selectedRecordingFormat == RecordingFormat::GDRJson;
        if (!activeMacro->persist(activeMacro->accuracyMode, static_cast<int>(tickRate), useJson)) {
            return false;
        }
        dataModified = false;
        reloadMacroList();
        return true;
    }

    dataModified = false;
    return false;
}

void ReplayEngine::discardActiveMacro() {
    ++macroGeneration;
    endReplayAccuracyEnvironment();

    if (activeSk) {
        delete activeSk;
        activeSk = nullptr;
    }
    if (captureStaging) {
        delete captureStaging;
        captureStaging = nullptr;
    }
    if (activeMacro) {
        delete activeMacro;
        activeMacro = nullptr;
    }

    dataModified = false;
    pendingPlaybackStart = false;
    executeIndex = 0;
    playbackAnchorIndex = 0;
    perFramePlaybackIndex = 0;
    playbackExhaustionNotified = false;
    clearPersistenceRuntimeState();
    storedRestorePoints.clear();
    skkbot2::log::debug("P1 discard: clearing practiceFixQueue (was {})", practiceFixQueue.size());
    practiceFixQueue.clear();
    clearFrameStepState();
    clearStartPosWarning();
    resetDeferredInputState();
    resetTimingTracking();
}

void ReplayEngine::closeSeamHoldEdges(std::vector<SkInput>& inputs, int boundaryTick) {
    if (inputs.empty()) {
        return;
    }
    // Сканируем сохранённый хвост: press открывает пару (raw p2, button),
    // release закрывает её. Вызывается ТОЛЬКО после snapshot-restore: открытая
    // пара закрывается на boundary лишь если snapshot не продолжает это удержание.
    bool open[2][3] = {};
    for (SkInput const& input : inputs) {
        if (input.tick > boundaryTick) {
            break;
        }
        int slot = input.isPlayer2() ? 1 : 0;
        int button = static_cast<int>(input.actionType) - 1;
        if (button < 0 || button > 2) {
            continue;
        }
        open[slot][button] = input.isPressed();
    }
    int appended = 0;
    int preserved = 0;
    auto* layer = PlayLayer::get();
    const bool flip = controlFlipActive(layer);
    const bool hasSecondPlayer = layer && layer->m_levelSettings &&
        (layer->m_levelSettings->m_twoPlayerMode || layer->m_gameState.m_isDualMode);
    for (int slot = 0; slot < 2; ++slot) {
        for (int button = 0; button < 3; ++button) {
            if (!open[slot][button]) {
                continue;
            }

            // В SKInput хранится raw-параметр handleButton. При control flip он
            // адресует противоположный PlayerObject, поэтому snapshot берём из
            // того же объекта, которому playback передаст этот raw-параметр.
            bool snapshotP2 = slot == 1;
            if (hasSecondPlayer) {
                snapshotP2 = handleButtonPlayer2Param(snapshotP2, flip);
            }
            auto* player = layer ? (snapshotP2 ? layer->m_player2 : layer->m_player1) : nullptr;
            bool snapshotHeld = false;
            if (player) {
                const int logicalButton = button + 1;
                const auto heldIt = player->m_holdingButtons.find(logicalButton);
                snapshotHeld = heldIt != player->m_holdingButtons.end() && heldIt->second;
            }
            if (snapshotHeld) {
                ++preserved;
                continue;
            }

            SkInput release;
            release.tick = boundaryTick;
            release.actionType = static_cast<uint8_t>(button + 1);
            release.setPlayer2(slot == 1);
            release.setPressed(false);
            release.stepOffset = 0.0f;
            release.cbsTimeOffset = -1.0;
            release.timeSeconds = -1.0;  // tick-derived time on serialize
            release.swiftPairAnchor = false;
            inputs.push_back(release);
            ++appended;
        }
    }
    if (appended > 0 || preserved > 0) {
        skkbot2::log::info(
            "[SKK] CAPTURE seam-close: boundary={} appended={} preservedSnapshotHolds={}",
            boundaryTick, appended, preserved);
    }
}

void ReplayEngine::truncateRecordedDataAfter(int tick) {
    skkbot2::log::info("[CK] truncateRecordedDataAfter ENTRY tick={}", tick);
    skkbot2::Logger::get().flush();
    if (skMode && activeSk) {
        if (canUsePersistenceCapture()) {
            while (!persistenceCaptureAttempt.inputs.empty() && persistenceCaptureAttempt.inputs.back().tick >= tick) {
                persistenceCaptureAttempt.inputs.pop_back();
            }
            while (!persistenceCaptureAttempt.anchors.empty() && persistenceCaptureAttempt.anchors.back().tick > tick) {
                persistenceCaptureAttempt.anchors.pop_back();
            }
            while (!persistenceCaptureAttempt.perFrames.empty() && persistenceCaptureAttempt.perFrames.back().tick > tick) {
                persistenceCaptureAttempt.perFrames.pop_back();
            }
            skkbot2::log::debug("CAPTURE truncate(persistence): to tick={} inputs={} anchors={} frames={}",
                tick,
                persistenceCaptureAttempt.inputs.size(), persistenceCaptureAttempt.anchors.size(),
                persistenceCaptureAttempt.perFrames.size());
            skkbot2::log::info("[CK] truncateRecordedDataAfter: persistence path done tick={}", tick);
            skkbot2::Logger::get().flush();
            return;
        }

        activeSk->truncateAfter(tick);
        skkbot2::step_trace::trim(tick);
        skkbot2::log::debug("CAPTURE truncate: to tick={} inputs={} anchors={} frames={} "
            "backFrame=({:.3f},{:.3f} {:.3f},{:.3f}) backAnchorTick={}",
            tick,
            activeSk->inputs.size(), activeSk->anchors.size(), activeSk->perFrames.size(),
            activeSk->perFrames.empty() ? -1.0 : activeSk->perFrames.back().p1x,
            activeSk->perFrames.empty() ? -1.0 : activeSk->perFrames.back().p1y,
            activeSk->perFrames.empty() ? -1.0 : activeSk->perFrames.back().p2x,
            activeSk->perFrames.empty() ? -1.0 : activeSk->perFrames.back().p2y,
            activeSk->anchors.empty() ? -1 : activeSk->anchors.back().tick);
        skkbot2::log::info("[CK] truncateRecordedDataAfter: non-persistence path done tick={}", tick);
        skkbot2::Logger::get().flush();
        return;
    }

    if (activeMacro) {
        activeMacro->truncateAfter(tick);
    }
    skkbot2::log::info("[CK] truncateRecordedDataAfter EXIT tick={}", tick);
    skkbot2::Logger::get().flush();
}

void ReplayEngine::prepareCaptureRestore(int tick) {
    skkbot2::log::debug("CAPTURE restore-prepare: tick={} stepPairCount={} lastTickIndex={}",
        tick, stepPairCount, lastTickIndex);
    resetDeferredInputState();
    resetTimingTracking();
    // CBS offset baseline: resetTimingTracking zeroes tickStartTimestamp.  The
    // first raw press right after a checkpoint restore would then compute
    // cbsTimeOffset = m_timestamp - 0.0 = the ENTIRE accumulated game clock
    // (e.g. 121617s), corrupting one input per death (timeSeconds non-monotonic
    // => on-disk 'unsorted inputs', playback freezes on the giant tick).
    // Anchor the baseline to the live clock so the offset stays a small
    // within-tick phase (the normal update() path re-asserts it every tick).
    if (auto* playLayer = PlayLayer::get()) {
        tickStartTimestamp = playLayer->m_timestamp;
    }
    // Truncation/respawn staging is a RECORD-only concern: it trims the
    // growing capture stream to the commit boundary. Replaying must never cut
    // the source macro (potentially deleting everything past the restore point
    // of the very stream being replayed). MODE_EXECUTE still needs the timing
    // reset + skip label below.
    if (engineMode == MODE_CAPTURE) {
        truncateRecordedDataAfter(tick);
        captureOnRespawn(tick);
    }
    skipActionTick = tick + 1;
    tickAccumulator = 0.0f;
}

// BUG-99 (SKK): сверка snapshot-удержаний с физикой делается ПОСЛЕ полного
// восстановления чекпоинта. closeSeamHoldEdges знает про snapshot и не рвёт
// удержание, которое тот продолжает; расхождение с физической клавиатурой
// становится ровно одним boundary-переходом в активном capture-буфере.
// Плейбэк сюда не попадает: он остаётся macro-driven.
int ReplayEngine::reconcileCaptureRestore(PlayLayer* playLayer, int tick) {
    if (engineMode != MODE_CAPTURE || !skMode || !activeSk || !playLayer ||
        !playLayer->m_player1) {
        return 0;
    }

    resetRecordedHoldShadow();
    const auto mode = currentPhysicalHoldMode(playLayer);
    const bool flip = controlFlipActive(playLayer);
    const bool hasSecondPlayer =
        mode.twoPlayer || mode.dual;
    int transitions = 0;
    for (int slot = 0; slot < 2; ++slot) {
        auto* player = slot == 0 ? playLayer->m_player1 : playLayer->m_player2;
        if (!player || (slot == 1 && !hasSecondPlayer)) {
            continue;
        }
        for (int button = 1; button <= 3; ++button) {
            const auto heldIt = player->m_holdingButtons.find(button);
            const bool snapshotHeld =
                heldIt != player->m_holdingButtons.end() && heldIt->second;
            const bool physicalHeld = physicalHoldActive(
                mode, button, slot == 1, player->m_controlsDisabled);

            // SKK хранит raw-параметр handleButton; при control flip он идёт
            // противоположному объекту. В тень пишем слот, под которым событие
            // реально попало бы в обычный capture-пайплайн.
            const bool rawP2 = handleButtonPlayer2Param(slot == 1, flip);
            const bool storedP2 = hasSecondPlayer ? rawP2 : false;
            const int shadowIndex =
                (storedP2 ? 1 : 0) * 3 + (button - 1);
            recordedHoldShadow[shadowIndex] = physicalHeld;
            if (snapshotHeld == physicalHeld) {
                continue;
            }

            // Qualified вызов снова входит в Geode capture-hook. Scoped-гейт
            // оставляет vanilla-эффект, но не даёт hook записать вторую команду;
            // единственный boundary-input добавляется ниже напрямую.
            const double vyBefore = player->m_yVelocity;
            const bool groundBefore = player->m_isOnGround;
            const bool bufferedBefore = player->m_jumpBuffered;
            const auto& targetInputs = canUsePersistenceCapture()
                ? persistenceCaptureAttempt.inputs
                : captureStaging ? captureStaging->inputs : activeSk->inputs;
            const size_t inputsBefore = targetInputs.size();
            {
                skkbot2::capture_reconciliation::Scope internalCall;
                playLayer->GJBaseGameLayer::handleButton(physicalHeld, button, rawP2);
            }
            const size_t hookAppended = targetInputs.size() >= inputsBefore
                ? targetInputs.size() - inputsBefore : 0;
            if (hookAppended != 0) {
                skkbot2::log::warn(
                    "[SKK-RESTORE-EDGE] повторная capture-запись tick={} p={} button={} hookAppended={}",
                    tick, slot + 1, button, hookAppended);
            }
            recordSkAction(tick, button, storedP2, physicalHeld, 0.0f, -1.0);
            const size_t totalAppended = targetInputs.size() >= inputsBefore
                ? targetInputs.size() - inputsBefore : 0;
            ++transitions;
            if (skkbot2::step_trace::enabled()) {
                const auto after = player->m_holdingButtons.find(button);
                skkbot2::log::info(
                    "[SKK-RESTORE-EDGE] tick={} p={} button={} edge={} rawP2={} storedP2={} "
                    "active={} dual={} twoPlayer={} held={}->{} vy={:a}->{:a} ground={}->{} buffered={}->{} "
                    "hookAppended={} totalAppended={}",
                    tick, slot + 1, button, physicalHeld ? "press" : "release", rawP2, storedP2,
                    player->getParent() != nullptr, mode.dual, mode.twoPlayer, snapshotHeld,
                    after != player->m_holdingButtons.end() && after->second,
                    vyBefore, static_cast<double>(player->m_yVelocity), groundBefore, player->m_isOnGround,
                    bufferedBefore, player->m_jumpBuffered, hookAppended, totalAppended);
            }
            skkbot2::log::info(
                "[SKK] capture restore reconciliation tick={} p2={} rawP2={} button={} "
                "snapshot={} physical={} recorded={} route={}",
                tick, slot == 1, rawP2, button, snapshotHeld, physicalHeld,
                physicalHeld ? "press" : "release",
                canUsePersistenceCapture() ? "persistence"
                    : captureStagingActive() ? "staging" : "main");
        }
    }
    if (transitions == 0) {
        skkbot2::log::info(
            "[SKK] capture restore reconciliation tick={}: snapshot holds match physical keys",
            tick);
    }
    return transitions;
}

void ReplayEngine::finishCaptureRestore(PlayLayer* playLayer, int tick) {
    if (engineMode != MODE_CAPTURE || !skMode || !activeSk) {
        return;
    }
    // Смерть уже завершена: reconcile пишет в обычный capture-маршрут, а не в
    // приостановленную death-цепочку.
    captureDeathPending = false;

    auto& retainedInputs = canUsePersistenceCapture()
        ? persistenceCaptureAttempt.inputs
        : activeSk->inputs;
    const size_t beforeClose = retainedInputs.size();
    closeSeamHoldEdges(retainedInputs, tick);
    const int transitions = reconcileCaptureRestore(playLayer, tick);
    if (retainedInputs.size() != beforeClose || transitions > 0) {
        markActiveMacroDirty();
    }
    ++captureRestoreSerial;
}

void ReplayEngine::captureResealBoundaryFrame(
    int tick, PlayerObject* p1, PlayerObject* p2, bool isPlatformer, bool isDual) {
    if (!skMode || !activeSk || canUsePersistenceCapture()) {
        skkbot2::log::debug("CAPTURE reseal: skip (skMode={} activeSk={} persistence={}) tick={}",
            skMode ? 1 : 0, activeSk ? 1 : 0, canUsePersistenceCapture() ? 1 : 0, tick);
        return;
    }

    // The boundary per-frame (kept by truncateAfter from the dying attempt) must
    // describe the POST-restore state, i.e. where the retry actually continues
    // from. Without the seal the recorded line holds the corpse tail position at
    // `tick` while the retry's first frame (tick+1) sits at the checkpoint spawn
    // position - a ~130px seam straight into the macro (BUG-06).
    auto& frames = activeSk->perFrames;
    if (frames.empty()) {
        skkbot2::log::debug("CAPTURE reseal: skip tick={} (no per-frames)", tick);
        return;
    }
    if (frames.back().tick != tick) {
        skkbot2::log::debug("CAPTURE reseal: skip tick={} (boundary frame not at tail, back.tick={})",
            tick, frames.back().tick);
        return;
    }

    SkPerFrame const old = frames.back();
    // Тот же writer, что у обычного capture: dormant P2 не становится живым,
    // dash-ring ID обоих игроков и visual bits не расходятся на шве.
    activeSk->recordPerFrame(tick, p1, p2);
    auto const& fresh = frames.back();

    float const dx = fresh.p1x - old.p1x;
    float const dy = fresh.p1y - old.p1y;
    float const dist = std::sqrt(dx * dx + dy * dy);
    skkbot2::log::info("CAPTURE reseal: boundary tick={} p1 old=({:.3f},{:.3f}) -> new=({:.3f},{:.3f}) "
        "d={:.2f}px frames={} rngLocked={} vy={:.9f}->{:.9f} rot={:.6f}->{:.6f} "
        "p2Active={}->{} rings=({},{})->({},{}) phase=post-reconcile",
        tick, old.p1x, old.p1y, fresh.p1x, fresh.p1y, dist, frames.size(), rngLocked ? 1 : 0,
        old.p1vy, fresh.p1vy, old.p1rot, fresh.p1rot, old.hasPlayer2, fresh.hasPlayer2,
        old.p1RingId, old.p2RingId, fresh.p1RingId, fresh.p2RingId);

    // Rewrite the boundary anchor too: playback reconcile seeds from anchors, a
    // stale one (corpse position) there would re-open the seam at tick+1 even
    // with a clean per-frame line. recordAnchor replaces the tail anchor when it
    // shares the tick and appends a monotonic-ordered one otherwise.
    activeSk->recordAnchor(tick, p1, p2, isPlatformer, isDual);
    skkbot2::step_trace::seal(PlayLayer::get(), tick);
    if (!activeSk->anchors.empty() && activeSk->anchors.back().tick == tick) {
        skkbot2::log::debug("CAPTURE reseal: boundary anchor rewritten tick={} (anchors={})",
            tick, activeSk->anchors.size());
    } else {
        skkbot2::log::warn("CAPTURE reseal: boundary anchor NOT at tail (back.tick={}, anchors={})",
            activeSk->anchors.empty() ? -1 : activeSk->anchors.back().tick, activeSk->anchors.size());
    }
}

int ReplayEngine::respawnStepTickForGameTick(int gameTick) {
    // The checkpoint bundles carry the game-time label (`tick`) and the
    // step-clock label (`priorTick` = lastTickIndex on touch). Practice respawn
    // re-aligns the player at the game-time tick; find the bundle whose game
    // label is closest and translate to the first retry frame of the recorded
    // stream (priorTick + 1). Callers fall back to the raw game tick when
    // nothing matches (level start / no checkpoints).
    const CheckpointStateBundle* best = nullptr;
    int bestDist = std::numeric_limits<int>::max();
    auto consider = [&](CheckpointStateBundle const& b) {
        int dist = std::abs(b.tick - gameTick);
        if (dist < bestDist) {
            bestDist = dist;
            best = &b;
        }
    };
    for (auto const& b : practiceFixQueue) {
        consider(b);
    }
    for (auto const& [cp, b] : storedRestorePoints) {
        (void)cp;
        consider(b);
    }
    if (!best || bestDist > 3) {
        return -1;
    }
    int step = best->priorTick + 1;
    skkbot2::log::info("PLAY game->step realign: gameTick={} bundleTick={} priorTick={} -> step={} (drift={})",
        gameTick, best->tick, best->priorTick, step, best->priorTick - best->tick);
    return step;
}

namespace {
template <typename T>
void spliceStagingBucket(std::vector<T>& main, std::vector<T>& staging, int floor,
                         bool appendAtBoundary = false) {
    // Фиксируем границу ДО переноса, иначе теряем соседние команды одного тика.
    // Префикс потребляем: повторный commit не должен создавать дубликаты.
    // Для inputs равный тик допустим (seam-close + reconciliation), а state
    // boundary-кадр уже запечатан в main и должен остаться единственным.
    const bool hadMain = !main.empty();
    const int coveredTick = hadMain ? main.back().tick : 0;
    auto end = staging.begin();
    for (; end != staging.end() && end->tick < floor; ++end) {
        auto const& item = *end;
        if (hadMain && (item.tick < coveredTick ||
            (!appendAtBoundary && item.tick == coveredTick))) continue;
        main.push_back(item);
    }
    staging.erase(staging.begin(), end);
}
}  // namespace

void ReplayEngine::captureStagingReset(int startTick) {
    if (skMode && activeSk && !canUsePersistenceCapture()) {
        if (!captureStaging) {
            captureStaging = new SkMacro();
            captureStaging->framerate = tickRate;
        }
    } else if (captureStaging) {
        delete captureStaging;
        captureStaging = nullptr;
    }
    if (captureStaging) {
        captureStaging->inputs.clear();
        captureStaging->anchors.clear();
        captureStaging->perFrames.clear();
    }
    captureStagingStartTick = startTick;
    captureStagingTouchFloor = startTick;
}

void ReplayEngine::appendCaptureStagingTo(SkMacro& snapshot) const {
    if (!captureStaging || canUsePersistenceCapture()) return;
    // Autosave не коммитит живой dying-tail: переносим только в копию.
    auto staging = *captureStaging;
    constexpr int kNoFloor = std::numeric_limits<int>::max();
    spliceStagingBucket(snapshot.inputs, staging.inputs, kNoFloor, true);
    spliceStagingBucket(snapshot.anchors, staging.anchors, kNoFloor);
    spliceStagingBucket(snapshot.perFrames, staging.perFrames, kNoFloor);
}

void ReplayEngine::clearFlatCaptureTimeline() {
    if (!activeSk) return;
    skkbot2::step_trace::wipe();
    skkbot2::log::info(
        "[SKK] очистка записи с начала: inputs={} staging={} rng={} wireCP={}",
        activeSk->inputs.size(), captureStaging ? captureStaging->inputs.size() : 0,
        activeSk->rngBoundaries.size(), activeSk->practiceFixCheckpoints.size());
    activeSk->inputs.clear();
    activeSk->anchors.clear();
    activeSk->perFrames.clear();
    activeSk->rngBoundaries.clear();
    activeSk->practiceFixCheckpoints.clear();
    storedRestorePoints.clear();
    pendingRngBoundary = {};
    nextBoundaryIndex = 0;
    // Не включаем staging там, где запись уже шла напрямую в main.
    if (captureStaging) captureStagingReset(0);
    captureStagingStartTick = 0;
    captureStagingTouchFloor = 0;
    markActiveMacroDirty();
}

void ReplayEngine::captureCommitStagingToFloor(int floorTick) {
    if (!captureStaging || !activeSk) {
        return;
    }
    spliceStagingBucket(activeSk->inputs, captureStaging->inputs, floorTick, true);
    spliceStagingBucket(activeSk->anchors, captureStaging->anchors, floorTick);
    spliceStagingBucket(activeSk->perFrames, captureStaging->perFrames, floorTick);
    skkbot2::log::debug("CAPTURE staging: committed to floor={} (inputs {} anchors {} frames {}) "
        "main now N={} A={} F={}",
        floorTick,
        captureStaging->inputs.size(), captureStaging->anchors.size(), captureStaging->perFrames.size(),
        activeSk->inputs.size(), activeSk->anchors.size(), activeSk->perFrames.size());
}

void ReplayEngine::captureFlushStagingAll() {
    if (!captureStaging || !activeSk) {
        return;
    }
    constexpr int kNoFloor = std::numeric_limits<int>::max();
    spliceStagingBucket(activeSk->inputs, captureStaging->inputs, kNoFloor, true);
    spliceStagingBucket(activeSk->anchors, captureStaging->anchors, kNoFloor);
    spliceStagingBucket(activeSk->perFrames, captureStaging->perFrames, kNoFloor);
    skkbot2::log::debug("CAPTURE staging: FLUSH all (inputs {} anchors {} frames {})",
        captureStaging->inputs.size(), captureStaging->anchors.size(), captureStaging->perFrames.size());
    captureStaging->inputs.clear();
    captureStaging->anchors.clear();
    captureStaging->perFrames.clear();
}

void ReplayEngine::captureOnRespawn(int respawnTick) {
    skkbot2::log::info("[CK] captureOnRespawn ENTRY respawnTick={}", respawnTick);
    skkbot2::Logger::get().flush();
    if (!captureStagingActive()) {
        skkbot2::log::info("[CK] captureOnRespawn: staging not active, return");
        skkbot2::Logger::get().flush();
        return;
    }
    // The dying attempt proved only up to the last checkpoint it touched while
    // alive; its own tail frames above that (the death/air segment the retry
    // jumps over) are dropped and never pollute the committed line.
    int const floor = std::max(captureStagingTouchFloor, captureStagingStartTick);
    skkbot2::log::info("[CK] captureOnRespawn: floor={} stagingStartTick={} touchFloor={}",
        floor, captureStagingStartTick, captureStagingTouchFloor);
    skkbot2::Logger::get().flush();
    captureCommitStagingToFloor(floor);
    skkbot2::log::info("[CK] captureOnRespawn: after captureCommitStagingToFloor");
    skkbot2::Logger::get().flush();
    truncateRecordedDataAfter(respawnTick);
    skkbot2::log::info("[CK] captureOnRespawn: after truncateRecordedDataAfter");
    skkbot2::Logger::get().flush();
    captureStagingReset(respawnTick);
    skkbot2::log::info("[CK] captureOnRespawn: EXIT respawnTick={}", respawnTick);
    skkbot2::Logger::get().flush();
}

void ReplayEngine::captureOnCheckpointTouched(int tick) {
    if (!captureStagingActive()) {
        return;
    }
    if (tick > captureStagingTouchFloor) {
        captureStagingTouchFloor = tick;
        skkbot2::log::debug("CAPTURE staging: checkpoint touched tick={} floor={}",
            tick, captureStagingTouchFloor);
    }
}

void ReplayEngine::restoreLockedRngState(uintptr_t rngState) {
    if (rngLocked) {
        GameToolbox::fast_srand(rngState);
    }
}

// Rewind the shake LCG as well. It used to be skipped ("camera shake is not
// locked"), which made every practice restore leave the chain wherever the
// failed attempt ended - the one locked stream with no restore path at all.
void ReplayEngine::restoreLockedShakeState(uint64_t shakeState) {
    // Ноль — допустимое состояние LCG, а не признак отсутствующего снимка.
    skk::shakeStateSet(shakeState);
}

void ReplayEngine::pruneStoredRestorePoints(PlayLayer* playLayer, int restoredTick) {
    if (!playLayer || !playLayer->m_checkpointArray) {
        storedRestorePoints.clear();
        return;
    }

    while (playLayer->m_checkpointArray->count() > 0) {
        auto* checkpoint = static_cast<CheckpointObject*>(playLayer->m_checkpointArray->lastObject());
        auto it = storedRestorePoints.find(checkpoint);
        if (it == storedRestorePoints.end()) {
            playLayer->removeCheckpoint(false);
            continue;
        }

        if (it->second.tick <= restoredTick) {
            break;
        }

        storedRestorePoints.erase(it);
        playLayer->removeCheckpoint(false);
    }

    std::unordered_set<CheckpointObject*> liveCheckpoints;
    liveCheckpoints.reserve(static_cast<size_t>(playLayer->m_checkpointArray->count()));
    for (unsigned int index = 0; index < playLayer->m_checkpointArray->count(); ++index) {
        auto* checkpoint = static_cast<CheckpointObject*>(playLayer->m_checkpointArray->objectAtIndex(index));
        liveCheckpoints.insert(checkpoint);
    }

    for (auto it = storedRestorePoints.begin(); it != storedRestorePoints.end();) {
        if (!liveCheckpoints.contains(it->first) || it->second.tick > restoredTick) {
            it = storedRestorePoints.erase(it);
        } else {
            ++it;
        }
    }
}

namespace {
    enum class UnsavedMacroChoice {
        Save,
        Discard,
        Cancel,
    };

    static std::string trString(std::string_view key) {
        return std::string(skk::lang::tr(key));
    }

    static std::vector<std::string> wrapPopupMessage(std::string message) {
        std::vector<std::string> lines;
        constexpr size_t maxLineLength = 32;
        constexpr size_t maxLines = 3;

        while (message.size() > maxLineLength && lines.size() + 1 < maxLines) {
            auto split = message.rfind(' ', maxLineLength);
            if (split == std::string::npos || split < maxLineLength / 2) {
                split = message.find(' ', maxLineLength);
            }
            if (split == std::string::npos) {
                break;
            }

            lines.push_back(message.substr(0, split));
            message.erase(0, split + 1);
        }

        if (!message.empty()) {
            lines.push_back(std::move(message));
        }
        return lines;
    }

    static bool canUsePusabBody(std::string const& message) {
        return std::all_of(message.begin(), message.end(), [](unsigned char ch) {
            return ch < 128;
        });
    }

    class UnsavedMacroPopup : public Popup {
    protected:
        std::function<void(UnsavedMacroChoice)> m_onChoice;
        bool m_resolved = false;

        bool init(std::string message, std::function<void(UnsavedMacroChoice)> onChoice) {
            if (!Popup::init(350.f, 185.f, "square01_001.png")) {
                return false;
            }

            m_onChoice = std::move(onChoice);
            this->setTitle(trString("Unsaved Macro"));

            auto lines = wrapPopupMessage(message);
            auto const* bodyFont = canUsePusabBody(message) ? "bigFont.fnt" : "chatFont.fnt";
            auto const lineSpacing = canUsePusabBody(message) ? 22.0f : 20.0f;
            auto const lineScale = canUsePusabBody(message) ? 0.45f : 0.82f;
            auto const minLineScale = canUsePusabBody(message) ? 0.34f : 0.68f;
            auto const firstLineY = 22.0f + (static_cast<float>(lines.size()) - 1.0f) * lineSpacing * 0.5f;

            for (size_t i = 0; i < lines.size(); ++i) {
                auto* label = CCLabelBMFont::create(lines[i].c_str(), bodyFont);
                label->setAnchorPoint(ccp(0.5f, 0.5f));
                label->setColor(ccc3(235, 245, 255));
                label->limitLabelWidth(m_size.width - 72.0f, lineScale, minLineScale);
                m_mainLayer->addChildAtPosition(
                    label,
                    Anchor::Center,
                    ccp(0.0f, firstLineY - static_cast<float>(i) * lineSpacing)
                );
            }

            auto makeButton = [&](char const* key, char const* background, cocos2d::SEL_MenuHandler callback) {
                auto* sprite = ButtonSprite::create(trString(key).c_str(), "goldFont.fnt", background, 0.8f);
                sprite->setScale(0.78f);
                return CCMenuItemSpriteExtra::create(sprite, this, callback);
            };

            auto* saveBtn = makeButton("Save", "GJ_button_01.png", menu_selector(UnsavedMacroPopup::onSave));
            auto* discardBtn = makeButton("Discard", "GJ_button_05.png", menu_selector(UnsavedMacroPopup::onDiscard));
            auto* cancelBtn = makeButton("Cancel", "GJ_button_04.png", menu_selector(UnsavedMacroPopup::onCancel));

            m_buttonMenu->addChildAtPosition(saveBtn, Anchor::Bottom, ccp(-106.0f, 30.0f));
            m_buttonMenu->addChildAtPosition(discardBtn, Anchor::Bottom, ccp(0.0f, 30.0f));
            m_buttonMenu->addChildAtPosition(cancelBtn, Anchor::Bottom, ccp(106.0f, 30.0f));
            return true;
        }

        void resolve(UnsavedMacroChoice choice) {
            if (m_resolved) {
                return;
            }
            m_resolved = true;
            if (m_onChoice) {
                m_onChoice(choice);
            }
            Popup::onClose(nullptr);
        }

        void onSave(CCObject*) {
            resolve(UnsavedMacroChoice::Save);
        }

        void onDiscard(CCObject*) {
            resolve(UnsavedMacroChoice::Discard);
        }

        void onCancel(CCObject*) {
            resolve(UnsavedMacroChoice::Cancel);
        }

        void onClose(CCObject* sender) override {
            if (!m_resolved && m_onChoice) {
                m_onChoice(UnsavedMacroChoice::Cancel);
            }
            m_resolved = true;
            Popup::onClose(sender);
        }

    public:
        static UnsavedMacroPopup* create(std::string message, std::function<void(UnsavedMacroChoice)> onChoice) {
            auto* popup = new UnsavedMacroPopup();
            if (popup->init(std::move(message), std::move(onChoice))) {
                popup->autorelease();
                return popup;
            }
            delete popup;
            return nullptr;
        }
    };
}

bool ReplayEngine::tryLivePlaybackTakeover(GJBaseGameLayer* playLayer) {
    if (!playLayer ||
        !pendingPlaybackStart ||
        engineMode != MODE_DISABLED ||
        !skMode || !activeSk ||
        activePersistencePlaybackAttempt()) {
        return false;
    }
    auto* p = playLayer->m_player1;
    if (!p || p->m_isDead || playLayer->m_levelEndAnimationStarted) {
        return false;
    }

    const float refX = p->getPositionX();
    const float refY = p->getPositionY();

    int targetTick = -1;
    if (auto* frames = activePerFrames(); frames && !frames->empty()) {
        targetTick = matchRecordedTickByPositionXY(
            *frames, refX, refY, kPlaybackTightTolerance, kPlaybackTakeoverYTolerance);
    }
    if (targetTick < 0) {
        if (auto const* anchors = activeAnchors(); anchors && !anchors->empty()) {
            targetTick = matchRecordedAnchorTickByPositionXY(
                *anchors, refX, refY, kPlaybackTightTolerance, kPlaybackTakeoverYTolerance);
        }
    }
    if (targetTick < 0) {
        // The player has not reached a spot the macro covers yet: keep waiting.
        skkbot2::log::debug("PLAY takeover wait: live=({:.0f},{:.0f}) no recorded coverage within {}/{}px (x/y)",
            refX, refY, kPlaybackTightTolerance, kPlaybackTakeoverYTolerance);
        return false;
    }

    // Found coverage: arm the engine exactly there. Unlike the reset path this
    // does NOT resetLevel - the live run keeps its current attempt (no teleport).
    if (!armExecutionState()) {
        return false;
    }
    // The reset path re-asserts the recorded variance table under MODE_EXECUTE;
    // a takeover arms from a live run, so do the same here.
    applyVarianceOverrides();

    // armExecutionState сбросил count/frac. В editor следующий шаг может дать
    // 0/1/несколько macro-тиков, поэтому его baseline заранее не угадываем.
    // attemptPlaytestArm выставит offset и курсоры после реального инкремента,
    // до dispatch/pin этого шага. Здесь физика уже закончилась, dispatch нет.
    if (skk::gameplay::isEditorPlaytest()) {
        startPosActive = false;
        skkbot2::log::info("PLAY live takeover: editor armed at ({:.0f},{:.0f}); alignment deferred to next step", refX, refY);
        return true;
    }

    const int baselineTick = playbackBaselineTick(playLayer, this);
    tickOffset = targetTick - baselineTick;
    startPosActive = true;
    if (auto* a = activeAnchors(); a && !a->empty()) {
        playbackAnchorIndex = findFirstAnchorAtTick(*a, targetTick);
    }
    if (auto* f = activePerFrames(); f && !f->empty()) {
        perFramePlaybackIndex = findFirstPerFrameAtTick(*f, targetTick);
    }
    if (auto* il = activeSkInputs()) {
        executeIndex = findFirstSkInputAtTick(*il, targetTick);
    } else if (activeMacro) {
        executeIndex = findFirstInputAtTick(activeMacro->inputs, targetTick);
    }
    clearStartPosWarning();
    skkbot2::log::info("PLAY live takeover: live=({:.0f},{:.0f}) -> targetTick={} baseline={} offset={}",
        refX, refY, targetTick, baselineTick, tickOffset);
    return true;
}

bool ReplayEngine::coversPlaybackPosition(float refX, float refY,
                                          float xTolerance, float yTolerance) {
    if (!skMode || !activeSk) {
        // Legacy macro path keeps the classic tick-0 playback; there are no
        // position streams to test coverage against.
        return true;
    }
    if (auto const* frames = activePerFrames(); frames && !frames->empty()) {
        if (matchRecordedTickByPositionXY(*frames, refX, refY, xTolerance, yTolerance) >= 0) {
            return true;
        }
    }
    if (auto const* anchors = activeAnchors(); anchors && !anchors->empty()) {
        if (matchRecordedAnchorTickByPositionXY(*anchors, refX, refY, xTolerance, yTolerance) >= 0) {
            return true;
        }
    }
    return false;
}

void ReplayEngine::parkPlaybackPending() {
    // A just-armed MODE_EXECUTE turned out to have an uncovered reset spawn.
    // Drop back into the armed-waiting state (same shape requestExecutionStart
    // produces) so the live takeover can arm when the player reaches the
    // macro-covered route. No "PLAY session ended" epilogue - nothing ended.
    pendingPlaybackStart = true;
    initialRun = false;
    levelRestarting = false;
    anchorReconciliation = false;
    perFrameReconciliation = false;
    macroInputActive = false;
    respawnTickIndex = -1;
    tickAccumulator = 0.0f;
    resetPlayDriftMeter();
    resetSessionStepGridStat();
    tickOffset = 0;
    startPosActive = false;
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

void ReplayEngine::runWithUnsavedMacroGuard(std::function<void()> continueAction, std::string message) {
    if (!continueAction) {
        return;
    }

    if (!hasUnsavedActiveMacro()) {
        continueAction();
        return;
    }

    if (unsavedGuardActive) {
        return;
    }

    if (message.empty()) {
        message = std::string(skk::lang::tr("You have an unsaved macro. Save before continuing?"));
    }

    auto* popup = UnsavedMacroPopup::create(
        std::move(message),
        [this, continueAction = std::move(continueAction)](UnsavedMacroChoice choice) mutable {
            unsavedGuardActive = false;
            if (choice == UnsavedMacroChoice::Cancel) {
                return;
            }
            if (choice == UnsavedMacroChoice::Save) {
                saveActiveMacro();
            }
            continueAction();
        }
    );

    if (!popup) {
        skkbot2::log::warn("Failed to create unsaved macro popup");
        return;
    }

    unsavedGuardActive = true;
    popup->show();
}

class $modify(BrokenObjectHook, GJBaseGameLayer) {
    void destroyObject(GameObject* obj) {
        GJBaseGameLayer::destroyObject(obj);

        auto* engine = ReplayEngine::get();
        if (!engine || engine->engineMode != MODE_CAPTURE
            || skk::trajectory::isSimulating()) return;

        BrokenObjectTracker::get().registerBroken(obj);
    }
};

class $modify(MacroPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        auto* engine = ReplayEngine::get();
        if (engine->pendingAction != ReplayEngine::PendingAction::None) {
            skkbot2::log::info(
                "PLAY init hook reached: queued={} level='{}' id={} platformer={}",
                engine->pendingAction == ReplayEngine::PendingAction::Play
                    ? "PLAY" : "RECORD",
                level ? level->m_levelName.c_str() : "<null>",
                level ? level->m_levelID : 0,
                m_isPlatformer ? "true" : "false");
        }
        // Runtime state from the previous scene survives into a newly inited
        // level ONLY when it belongs to this same level: pause-menu Restart and
        // re-entering the level recreate the PlayLayer, and recording/playing
        // must continue (resetLevel during init rewinds indices anyway). State
        // tied to a different level (or an unknown one) is stale - halt it so
        // an old macro can never auto-run or keep recording in the wrong level.
        bool sameLevel = engine->activeSk && level &&
            (engine->activeSk->levelId > 0
                ? engine->activeSk->levelId == static_cast<int32_t>(level->m_levelID)
                : engine->activeSk->levelName == level->m_levelName.c_str());
        bool keepRuntime = sameLevel &&
            (engine->engineMode != MODE_DISABLED || engine->pendingPlaybackStart);
        if (!keepRuntime &&
            (engine->engineMode != MODE_DISABLED || engine->pendingPlaybackStart)) {
            engine->haltExecution();
        }
        engine->resetNoclipAccuracyStats();
        engine->consumePendingAction(level);
        if (engine->engineMode == MODE_CAPTURE) {
            // Only (re)initialize a macro that does not belong to this level
            // yet; re-initializing on every PlayLayer::init would delete the
            // in-progress macro (incl. persistence attempts) on each restart.
            bool needsInit = engine->skMode
                ? (!engine->activeSk || !level ||
                   (engine->activeSk->levelId > 0
                       ? engine->activeSk->levelId != static_cast<int32_t>(level->m_levelID)
                       : engine->activeSk->levelName != level->m_levelName.c_str()))
                : (!engine->activeMacro || !level ||
                   engine->activeMacro->levelInfo.id != static_cast<decltype(engine->activeMacro->levelInfo.id)>(level->m_levelID));
            if (needsInit) {
                if (engine->skMode) {
                    engine->initializeSkMacro(level);
                } else {
                    engine->initializeMacro(level);
                }
            }
        }
        // A fresh PlayLayer means the previous level's GameObjects (and their
        // $modify Fields) are gone. Without clearing here the per-object RNG
        // registry (g_advRandom) keeps dangling statePtr and the first
        // applyReplayResetState -> advRandomReseed on the new objects writes
        // into freed memory (AV / heap corruption). customObjectSetup running
        // inside PlayLayer::init re-registers the fresh object fields; focus
        // respawns in practice never recreate the PlayLayer and keep reusing
        // the same object instances, so the registry still survives attempts.
        skk::advRandomClear();
        // CheckpointObject pointers belong to one PlayLayer instance. Even an
        // editor playtest that preserves the macro/runtime must not carry its
        // retained platformer objects into the newly-created layer.
        engine->storedRestorePoints.clear();
        engine->clearPlatformerCheckpoints();
        bool ret = PlayLayer::init(level, dontCreateObjects);
        if (auto* engine = ReplayEngine::get()) {
            // beginCapture/initializeSkMacro can run from consumePendingAction
            // before the base PlayLayer::init creates m_levelSettings. Stamp
            // mode metadata now, while the actual initialized layer is live.
            if (ret && engine->engineMode == MODE_CAPTURE && engine->skMode &&
                engine->activeSk && engine->activeSk->levelId ==
                    static_cast<int32_t>(level ? level->m_levelID : 0) &&
                m_levelSettings) {
                engine->activeSk->platformerMode = m_levelSettings->m_platformerMode;
                engine->activeSk->twoPlayerMode = m_levelSettings->m_twoPlayerMode;
                skkbot2::log::info(
                    "RECORD mode metadata stamped after PlayLayer init: platformer={} twoPlayer={}",
                    engine->activeSk->platformerMode ? "true" : "false",
                    engine->activeSk->twoPlayerMode ? "true" : "false");
            }
            if (engine->replayAccuracyEnvironmentActive) {
                // The replay owns the accuracy environment — it re-applies its
                // own mode on every arm.
            } else if (skk::skc::Bot::get().ownsTiming()) {
                // An SKC session owns the clock. SKC labels every click with a
                // count of processCommands calls and its format carries no
                // sub-step offset, so CBS would split those steps in half and
                // make the recorded stream not line up with the physics that
                // produced it. SkcPlayLayer::resetLevel forces vanilla accuracy
                // for exactly that reason - and this hook used to undo it a few
                // frames later, on the same reset, by pushing the menu's
                // selected mode back. The two modules were fighting over one
                // setting with no rule for who wins; the session that owns the
                // clock is the one whose answer has to hold.
            } else {
                // GD re-initialises m_clickBetweenSteps / m_clickOnSteps from
                // its own source inside PlayLayer::init, which would wipe the
                // mode the user picked in our menu. Re-apply our value instead
                // of adopting the post-init one: syncAccuracyFromGame() is the
                // menu's job and would here silently undo the user's choice.
                // A change the user made in GD's own options while inside a
                // level is already mirrored by the menu (it adopts straight from
                // the layer field), so this re-apply is idempotent for it.
                ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                       engine->cosEnabled);
            }
        }
        return ret;
    }

    void syncBotCheckpoints() {
        skk::skc::PracticeFix::get().syncStoredCheckpoints(this);
        auto* engine = ReplayEngine::get();
        if (!engine) return;
        const auto present = [this](CheckpointObject* cp) {
            return m_checkpointArray && cp && m_checkpointArray->containsObject(cp);
        };
        std::erase_if(engine->storedRestorePoints,
            [&](auto const& entry) { return !present(entry.first); });
        std::erase_if(engine->platformerCheckpointObjects, [&](auto* cp) {
            if (present(cp)) return false;
            cp->release();
            return true;
        });
    }

    void removeCheckpoint(bool first) {
        const auto before = m_checkpointArray ? m_checkpointArray->count() : 0;
        PlayLayer::removeCheckpoint(first);
        syncBotCheckpoints();
        skkbot2::log::info(
            "[CK] checkpoint REMOVE: first={} count={}->{} target={} "
            "(ввод не обрезан)", first, before,
            m_checkpointArray ? m_checkpointArray->count() : 0,
            m_checkpointArray ? static_cast<void*>(m_checkpointArray->lastObject()) : nullptr);
    }

    void removeAllCheckpoints() {
        PlayLayer::removeAllCheckpoints();
        syncBotCheckpoints();
        skkbot2::log::info("[CK] checkpoint REMOVE ALL: count={} (ввод не обрезан)",
            m_checkpointArray ? m_checkpointArray->count() : 0);
    }

    CheckpointObject* createCheckpoint() {
        auto* checkpoint = PlayLayer::createCheckpoint();
        if (!checkpoint || skk::trajectory::isSimulating() ||
            (m_player1 && m_player1->m_isDead) ||
            (m_player2 && m_player2->m_isDead)) return checkpoint;

        auto* engine = ReplayEngine::get();
        auto& bot = skk::skc::Bot::get();
        auto* saved = static_cast<SkkCheckpointCreation*>(checkpoint);
        // После vanilla create поля CP уже заполнены, а live-состояние ещё
        // относится к тому же шагу. Store может прийти через сотни тиков.
        if (bot.isRecording() && !bot.isDeathCapturePaused()) {
            const auto frame = skk::skc::FrameClock::get().getFrame();
            skkbot2::seam_probe::arm(static_cast<int>(frame));
            skkbot2::seam_probe::log("create-after-vanilla", m_player1);
            skkbot2::seam_probe::log("create-after-vanilla", m_player2);
            saved->m_fields->skc = SkkCheckpointCreation::Snapshot{
                skk::skc::PracticeFix::get().generation(), frame,
                CheckpointStateManager::capture(static_cast<int>(frame), -1,
                    this, engine ? engine->captureRngState() : AnchorRngState{})};
            skkbot2::log::info(
                "[SKC] checkpoint CREATE: object={} frame={} cpTime={:.9f} "
                "liveTime={:.9f} cpProgress={} liveProgress={} cpX={:.6f} liveX={:.6f}",
                static_cast<void*>(checkpoint), frame,
                checkpoint->m_gameState.m_levelTime, m_gameState.m_levelTime,
                checkpoint->m_gameState.m_currentProgress, m_gameState.m_currentProgress,
                checkpoint->m_player1Checkpoint ? checkpoint->m_player1Checkpoint->m_position.x : 0.f,
                m_player1 ? m_player1->getPositionX() : 0.f);
        }
        if (engine && engine->engineMode == MODE_CAPTURE && engine->skMode &&
            engine->activeSk && !engine->captureDeathPending) {
            const int tick = static_cast<int>(
                m_gameState.m_levelTime * engine->runtimeTickRate()) + 1;
            skkbot2::seam_probe::arm(engine->stepPairCount);
            skkbot2::seam_probe::log("create-after-vanilla", m_player1);
            skkbot2::seam_probe::log("create-after-vanilla", m_player2);
            saved->m_fields->skk = SkkCheckpointCreation::Snapshot{
                static_cast<uint64_t>(engine->sessionCounter), static_cast<uint32_t>(tick),
                CheckpointStateManager::capture(tick, engine->lastTickIndex,
                    this, engine->captureRngState())};
            skkbot2::log::info(
                "CAPTURE checkpoint CREATE: object={} tick={} cpTime={:.9f} "
                "liveTime={:.9f} cpX={:.6f} liveX={:.6f}",
                static_cast<void*>(checkpoint), tick,
                checkpoint->m_gameState.m_levelTime, m_gameState.m_levelTime,
                checkpoint->m_player1Checkpoint ? checkpoint->m_player1Checkpoint->m_position.x : 0.f,
                m_player1 ? m_player1->getPositionX() : 0.f);
        }
        return checkpoint;
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        const bool setupCheckpoint = m_activatedCheckpoint != nullptr;
        PlayLayer::storeCheckpoint(checkpoint);
        syncBotCheckpoints(); // Учитывает и внутреннее удаление старых CP в GD.

        auto* engine = ReplayEngine::get();
        if (checkpoint && m_isPlatformer && setupCheckpoint && engine &&
            (engine->engineMode == MODE_CAPTURE || engine->engineMode == MODE_EXECUTE)) {
            engine->trackPlatformerCheckpoint(checkpoint);
        }
        // BUG-156: поздний store регистрирует живой create-снимок даже после
        // смерти. Запрет corpse-capture остаётся только в createCheckpoint.
        if (!checkpoint || !engine || engine->engineMode != MODE_CAPTURE ||
            !engine->skMode || !engine->activeSk || skk::trajectory::isSimulating()) {
            return;
        }

        auto& created = static_cast<SkkCheckpointCreation*>(checkpoint)->m_fields->skk;
        if (!created || created->generation != static_cast<uint64_t>(engine->sessionCounter)) {
            skkbot2::log::warn(
                "CAPTURE checkpoint STORE: object={} нет снимка текущей записи; "
                "поздний live-capture запрещён", static_cast<void*>(checkpoint));
            return;
        }
        const int tick = static_cast<int>(created->frame);
        auto bundle = std::move(created->state);
        created.reset();
        const bool hasFull = bundle.hasFullState;
        const int captureTick = bundle.priorTick;
        engine->storedRestorePoints[checkpoint] = std::move(bundle);

        // GucciBot's platformer path keys setup-checkpoints here, while
        // m_activatedCheckpoint is still set. Do not rely on
        // GJBaseGameLayer::checkpointActivated: GD 2.2081 does not route this
        // setup-checkpoint path through that callback. storeCheckpoint is the
        // live checkpoint boundary for both ordinary and setup checkpoints.
        // Staging живёт на step-clock, tick bundle — метка времени игры.
        // Floor исключающий: +1 сохраняет boundary-state для reseal; inputs
        // ровно на boundary затем удалит включающий truncateAfter.
        engine->captureOnCheckpointTouched(captureTick + 1);
        skkbot2::log::info(
            "CAPTURE checkpoint stored: tick={} platformer={} activatedSetup={} "
            "restorePoints={} hasFull={} cpTime={:.9f} storeTime={:.9f} stepTick={} "
            "deathPending={} deadP1={} deadP2={}",
            tick, m_isPlatformer, setupCheckpoint,
            engine->storedRestorePoints.size(), hasFull,
            checkpoint->m_gameState.m_levelTime, m_gameState.m_levelTime, captureTick,
            engine->captureDeathPending, m_player1 && m_player1->m_isDead,
            m_player2 && m_player2->m_isDead);
    }

    void applyReplayResetState(bool inPractice, int tick,
                               bool platformerCheckpointMode = false) {
        auto* engine = ReplayEngine::get();

        // A platformer level uses checkpoints even when m_isPracticeMode is
        // false: the editor "setup checkpoint" object / PlayerObject's
        // platformer auto-checkpoint path calls PlayLayer::markCheckpoint() and
        // GD respawns from the last CheckpointObject on death. Treating that
        // reset as a from-start restart wiped the recorded timeline and every
        // restore point, so the retry continued without the checkpoint the
        // player actually respawned at. Practice mode and platformer both keep
        // the checkpoint chain; only a real from-start reset drops it.
        const bool checkpointMode = inPractice || platformerCheckpointMode;

        // `tick` is m_gameState.m_currentProgress, which is an x coordinate in
        // a platformer and is still ~0 at the editor "setup checkpoint" (the
        // player has not moved yet). The old `tick <= 1` wipe guard therefore
        // deleted the snapshot of the very checkpoint the player respawned at.
        // In platformer the authoritative sign of a checkpoint respawn is the
        // checkpoint array GD is about to restore from: resetLevel() calls
        // loadFromCheckpoint(lastObject()) whenever it is non-empty. Classic
        // keeps the progress guard (there is no x-based checkpoint array in a
        // normal classic run).
        const bool checkpointPresent =
            m_checkpointArray && m_checkpointArray->count() > 0;
        const bool progressed = platformerCheckpointMode ? checkpointPresent
                                                         : (tick > 1);
        const bool wipeCapture = !checkpointMode || !progressed ||
                                 engine->storedRestorePoints.empty();
        // Snapshot-map availability is a capture concern, not the lifetime of
        // GD's platformer checkpoint object. During playback there is normally
        // no storedRestorePoints map, but the exact setup CheckpointObject must
        // remain tracked across every retry. Drop objects only on a true
        // from-start reset, not merely because this session has no capture map.
        if (!checkpointMode || !progressed) {
            engine->clearPlatformerCheckpoints();
        }

        if (engine->engineMode == MODE_CAPTURE) {
            skkbot2::log::info(
                "CAPTURE reset: practice={} platformer={} tick={} levelTick={} "
                "checkpoints={} restorePoints={} -> {}",
                inPractice, platformerCheckpointMode, tick,
                computePlaybackTick(this),
                m_checkpointArray ? m_checkpointArray->count() : 0,
                engine->storedRestorePoints.size(),
                (!checkpointMode || !progressed || engine->storedRestorePoints.empty())
                    ? "WIPE" : "KEEP");
        }

        // Per-object RNG (2068/1912/3607): re-seed FIRST while the registry
        // still holds the level instance's object pointers. These states are
        // NOT serialized into the macro, so the ONLY way a fresh attempt
        // reproduces the recorded section is a deterministic reset to
        // computedSeed ^ (uid*2137) on every level start/respawn.
        //
        // CRITICAL: the registry must SURVIVE across attempts. Objects are
        // registered once at level load (customObjectSetup) and GD reuses the
        // same GameObject* instances across practice respawns - clearing the
        // registry here dropped every pointer after the first attempt, turning
        // advRandomReseed into a no-op on later attempts. Per-object LCG states
        // then leaked from the previous attempt -> the same random section
        // looked different attempt-to-attempt (attempt 1 ok, attempt 2 dead,
        // attempt 3 ok). The registry is torn down on level exit (onQuit),
        // where the GameObject* actually become invalid.
        refreshRngState(true);
        // Post-vanilla (toasty-lite ordering): force the layer seed triple so
        // runtime object rolls that read m_randomSeed/m_replayRandSeed match
        // across record & replay sessions regardless of prior attempts. The
        // pre-vanilla pin in resetLevel already set the chain input for setup.
        if (engine->engineMode == MODE_CAPTURE || engine->engineMode == MODE_EXECUTE) {
            engine->forceLayerSeeds();
        }

        engine->executeIndex = 0;
        engine->playbackAnchorIndex = 0;
        engine->perFramePlaybackIndex = 0;
        engine->playbackExhaustionNotified = false;
        engine->resetPlayDriftMeter();
        engine->levelRestarting = false;
        engine->resetTimingTracking();
        engine->respawnTickIndex = -1;
        engine->nextBoundaryIndex = 0;
        engine->tickAccumulator = 0.0f;
        // PLAY was armed while the player was already in a live run (or queued
        // outside the level): take over NOW only if the freshly reset spawn is
        // actually covered by the recording. Otherwise keep waiting - the
        // per-tick live takeover arms the moment the player reaches a covered
        // spot. Never realign a raw spawn to a mid-run macro's first frames.
        if (engine->pendingPlaybackStart && engine->engineMode != MODE_EXECUTE) {
            float spawnX = m_startPosObject ? m_startPosObject->getPositionX()
                                            : (m_player1 ? m_player1->getPositionX() : 0.f);
            float spawnY = m_startPosObject ? m_startPosObject->getPositionY()
                                            : (m_player1 ? m_player1->getPositionY() : 0.f);
            const bool spawnCovered = engine->coversPlaybackPosition(
                spawnX, spawnY, kPlaybackTightTolerance, kPlaybackTakeoverYTolerance);
            if (spawnCovered || !engine->skMode) {
                const bool armed = engine->armPendingPlaybackStart(this);
                skkbot2::log::info(
                    "PLAY reset spawn decision: covered={} armed={} mode={} "
                    "spawn=({:.1f},{:.1f})",
                    spawnCovered ? "true" : "false",
                    armed ? "true" : "false",
                    static_cast<int>(engine->engineMode), spawnX, spawnY);
            } else {
                skkbot2::log::info(
                    "PLAY armed-wait: respawn=({:.0f},{:.0f}) not covered by macro - waiting for live coverage "
                    "(no reset align, no teleport)",
                    spawnX, spawnY);
                engine->clearStartPosWarning();
            }
        } else {
            engine->armPendingPlaybackStart(this);
        }
        if (engine->engineMode == MODE_CAPTURE) {
            bool fromStartPosition = skk::start_position::shouldRecordFromStartPosition(
                m_startPosObject != nullptr,
                getCurrentPercentInt()
            );
            auto applyStartPosition = [&](auto* macro) {
                if (!macro) return;
                macro->recordedFromStartPos = fromStartPosition;
                macro->startPosX = fromStartPosition ? m_startPosObject->getPositionX() : 0.0f;
                macro->startPosY = fromStartPosition ? m_startPosObject->getPositionY() : 0.0f;
            };
            if (engine->skMode) {
                applyStartPosition(engine->activeSk);
            } else {
                applyStartPosition(engine->activeMacro);
            }
        }
        if (engine->engineMode == MODE_EXECUTE) {
            // Fresh attempt: re-assert recorded variance slots before any
            // object reads them this run (lazy GD variance stays locked-in
            // otherwise to whatever this session rolled).
            engine->applyVarianceOverrides();
            engine->advancePersistencePlaybackAfterReset();
        }

        auto clearPlayerHolds = [&]() {
            if (!m_player1 || !m_player2) {
                return;
            }

            engine->resetDeferredInputState();
            m_player1->m_holdingRight = false;
            m_player1->m_holdingLeft = false;
            m_player2->m_holdingRight = false;
            m_player2->m_holdingLeft = false;
            if (m_levelSettings->m_platformerMode) {
                m_player1->m_holdingButtons[2] = false;
                m_player1->m_holdingButtons[3] = false;
                m_player2->m_holdingButtons[2] = false;
                m_player2->m_holdingButtons[3] = false;
            }
        };

        if (engine->engineMode == MODE_EXECUTE) {
            auto const* anchors = engine->activeAnchors();
            bool recordedFromStartPos = engine->skMode
                ? (engine->activeSk && engine->activeSk->recordedFromStartPos)
                : (engine->activeMacro && engine->activeMacro->recordedFromStartPos);

            float recStartX = engine->skMode
                ? (engine->activeSk ? engine->activeSk->startPosX : 0.f)
                : (engine->activeMacro ? engine->activeMacro->startPosX : 0.f);
            float recStartY = engine->skMode
                ? (engine->activeSk ? engine->activeSk->startPosY : 0.f)
                : (engine->activeMacro ? engine->activeMacro->startPosY : 0.f);
            bool recordedAtLevelStart = recordedFromStartPos &&
                skk::start_position::isAtLevelStart(recStartX, m_levelLength);

            bool startPosMatch = false;
            if (m_startPosObject && recordedFromStartPos) {
                float dx = m_startPosObject->getPositionX() - recStartX;
                float dy = m_startPosObject->getPositionY() - recStartY;
                startPosMatch = std::sqrt(dx * dx + dy * dy) < 50.0f;
            }

            // Non-standard-start support: the recording knows WHERE its timeline
            // began. A macro whose first captured frame sits beyond this tick was
            // recorded MID-RUN (the bot was armed while the player was already in
            // the level) - its data does not cover the prefix a tick-0 timeline
            // assumes, so playback must align by POSITION instead. Everything is
            // derived from the saved streams (no wire-format change).
            int recFirstTick = 0;
            float recFirstX = 0.f, recFirstY = 0.f;
            bool midRun = false;
            if (engine->skMode && engine->activeSk) {
                recFirstTick = static_cast<int>(engine->activeSk->recordFirstTick());
                midRun = engine->activeSk->startedMidRun();
                recFirstX = engine->activeSk->recordStartX();
                recFirstY = engine->activeSk->recordStartY();
            }

            // Where the character stands right now: the start-pos object the
            // player spawns at (if the level has one), else the live player
            // position itself - the bot "understands" where the player is.
            const bool haveRef = m_startPosObject != nullptr || m_player1 != nullptr;
            float refX = m_startPosObject
                ? m_startPosObject->getPositionX()
                : (m_player1 ? m_player1->getPositionX() : 0.f);
            float refY = m_startPosObject
                ? m_startPosObject->getPositionY()
                : (m_player1 ? m_player1->getPositionY() : 0.f);

            // Coverage gate: refuse to start playback from a spawn the recording
            // never covered. Previously a mid-run macro (data begins at tick ~2,
            // far ahead of the world spawn) would tight->loose align a raw spawn
            // X to its first frames - a ~900px catch-up that jarringly teleported
            // the player backwards. If the spawn IS covered, proceed (alignment
            // happens right there, seamless). Without coverage, park back into
            // the armed-waiting state; the live takeover arms when the player
            // reaches a covered position.
            if (engine->skMode && !engine->activePersistencePlaybackAttempt() &&
                !engine->coversPlaybackPosition(refX, refY, kPlaybackTightTolerance, kPlaybackTakeoverYTolerance)) {
                skkbot2::log::info(
                    "PLAY waiting: spawn=({:.0f},{:.0f}) not covered by macro - holding armed state (no teleport)",
                    refX, refY);
                engine->parkPlaybackPending();
                return;
            }

            skkbot2::log::info("PLAY attempt: practice={} platformer={} progressPct={} levelTick={} ref=({:.0f},{:.0f}) "
                    "startPos={} recStartPos={} match={} midRun={} recOrigin=[tick={} ({:.0f},{:.0f})] anchors={}",
                inPractice, platformerCheckpointMode, tick, computePlaybackTick(this),
                refX, refY,
                m_startPosObject != nullptr, recordedFromStartPos, startPosMatch,
                midRun, recFirstTick, recFirstX, recFirstY,
                anchors ? static_cast<unsigned long long>(anchors->size()) : 0);

            constexpr float kAlignTight = 120.0f;
            constexpr float kAlignLoose = 500.0f;

            // Position-aware timeline alignment: find the recorded frame whose p1
            // X best matches the player's spawn and re-anchor the playback there
            // (tickOffset + per-frame/anchor/input cursors). Skips persistence
            // chain playback (its segments use run-local ticks) and legacy macros
            // (no position streams). Returns false when the recording has no
            // coverage near the reference X.
            auto alignByPosition = [&](float alignRefX, float alignRefY) -> bool {
                if (!engine->skMode || engine->activePersistencePlaybackAttempt() || !haveRef) {
                    return false;
                }
                int targetTick = -1;
                if (auto* framesStream = engine->activePerFrames();
                    framesStream && !framesStream->empty()) {
                    targetTick = matchRecordedTickByPosition(*framesStream, alignRefX, kAlignTight);
                    if (targetTick < 0) {
                        targetTick = matchRecordedTickByPosition(*framesStream, alignRefX, kAlignLoose);
                    }
                }
                if (targetTick < 0) {
                    if (auto const* anchorStream = engine->activeAnchors();
                        anchorStream && !anchorStream->empty()) {
                        targetTick = matchRecordedAnchorTickByPosition(*anchorStream, alignRefX, kAlignTight);
                        if (targetTick < 0) {
                            targetTick = matchRecordedAnchorTickByPosition(*anchorStream, alignRefX, kAlignLoose);
                        }
                    }
                }
                if (targetTick < 0) {
                    return false;
                }

                const int baselineTick = playbackBaselineTick(this, engine);
                engine->tickOffset = targetTick - baselineTick;
                engine->startPosActive = true;
                if (auto* a = engine->activeAnchors(); a && !a->empty()) {
                    engine->playbackAnchorIndex = findFirstAnchorAtTick(*a, targetTick);
                }
                if (auto* f = engine->activePerFrames(); f && !f->empty()) {
                    engine->perFramePlaybackIndex = findFirstPerFrameAtTick(*f, targetTick);
                }
                if (auto* il = engine->activeSkInputs()) {
                    engine->executeIndex = findFirstSkInputAtTick(*il, targetTick);
                } else if (engine->activeMacro) {
                    engine->executeIndex = findFirstInputAtTick(engine->activeMacro->inputs, targetTick);
                }
                skkbot2::log::info("PLAY position-aligned: ref=({:.0f},{:.0f}) -> targetTick={} baseline={} offset={}",
                    alignRefX, alignRefY, targetTick, baselineTick, engine->tickOffset);
                engine->clearStartPosWarning();
                return true;
            };

            if (m_startPosObject && startPosMatch) {
                // The player uses the exact SP the macro was recorded from.
                // When the recording's data begins at tick 0 the world spawn IS
                // the macro start: classic tick-0 alignment. A MID-RUN recording
                // begins several frames AFTER that spawn, so tick-0 would replay
                // the missing prefix blind - align by position instead.
                if (!midRun) {
                    engine->tickOffset = 0;
                    engine->startPosActive = true;
                    if (anchors && !anchors->empty()) {
                        engine->playbackAnchorIndex = findFirstAnchorAtTick(*anchors, 0);
                    } else {
                        engine->playbackAnchorIndex = 0;
                    }
                    if (engine->skMode) {
                        // Seek in the stream that will ACTUALLY play (chain link
                        // during persistence playback, main otherwise), not
                        // blindly in main's array.
                        if (auto* frames = engine->activePerFrames()) {
                            engine->perFramePlaybackIndex = findFirstPerFrameAtTick(*frames, 0);
                        }
                    }
                    if (engine->skMode && engine->activeSk) {
                        engine->executeIndex = 0;
                    } else if (engine->activeMacro) {
                        engine->executeIndex = 0;
                    }
                    engine->clearStartPosWarning();
                } else if (!alignByPosition(refX, refY)) {
                    skkbot2::log::warn("PLAY aborted: macro recorded from start position ({:.0f},{:.0f}) but data begins "
                                       "MID-RUN at tick {} p1=({:.0f},{:.0f}) and the player's spawn ({:.0f},{:.0f}) has no "
                                       "recorded coverage - start from the spot where recording began or re-record "
                                       "from the level start",
                        recStartX, recStartY, recFirstTick, recFirstX, recFirstY, refX, refY);
                    engine->setStartPosWarningKey("Macro was recorded mid-run. Start playback from the position where recording began, or re-record from the level start.");
                    engine->haltExecution();
                }
            } else if (m_startPosObject && anchors && !anchors->empty()) {
                if (engine->activePersistencePlaybackAttempt()) {
                    // Chain segments use run-local ticks; their own reset path
                    // restores cursors. Skip position alignment entirely.
                    engine->tickOffset = 0;
                    engine->startPosActive = false;
                    engine->clearStartPosWarning();
                } else {
                    int targetTick = matchStartPosTargetTick(engine, *anchors, refX, refY);
                    if (targetTick < 0) {
                        // A start pos the recording never covered within the
                        // tight window: park back into the armed wait - the live
                        // takeover arms the moment the player reaches a covered
                        // spot (no teleport, per spec).
                        skkbot2::log::info(
                            "PLAY startpos wait: spawn=({:.0f},{:.0f}) not on recorded route (within {}/{}) - "
                            "holding armed state until live coverage",
                            refX, refY, kPlaybackTightTolerance, kPlaybackTakeoverYTolerance);
                        engine->parkPlaybackPending();
                        return;
                    }
                    const int baselineTick = playbackBaselineTick(this, engine);
                    engine->tickOffset = targetTick - baselineTick;
                    engine->startPosActive = true;
                    engine->playbackAnchorIndex = findFirstAnchorAtTick(*anchors, targetTick);
                    if (engine->skMode) {
                        if (auto* frames = engine->activePerFrames()) {
                            engine->perFramePlaybackIndex = findFirstPerFrameAtTick(*frames, targetTick);
                        }
                    }
                    if (engine->skMode && engine->activeSk) {
                        if (auto* inputList = engine->activeSkInputs()) {
                            engine->executeIndex = findFirstSkInputAtTick(*inputList, targetTick);
                        } else {
                            engine->executeIndex = 0;
                        }
                    } else if (engine->activeMacro) {
                        engine->executeIndex = findFirstInputAtTick(engine->activeMacro->inputs, targetTick);
                    }
                    engine->clearStartPosWarning();
                    skkbot2::log::info("PLAY startpos-aligned: spawn=({:.0f},{:.0f}) -> targetTick={} baseline={} offset={}",
                        refX, refY, targetTick, baselineTick, engine->tickOffset);
                }
            } else if (!m_startPosObject && recordedFromStartPos && !recordedAtLevelStart) {
                skkbot2::log::warn("PLAY aborted: macro recorded from start position ({:.0f},{:.0f}) but the "
                                    "session has none - playback from raw spawn would diverge at tick 1; "
                                    "set the same start position or re-record from level start",
                    engine->skMode && engine->activeSk ? engine->activeSk->startPosX : 0.f,
                    engine->skMode && engine->activeSk ? engine->activeSk->startPosY : 0.f);
                engine->setStartPosWarningKey("Macro was recorded from a start position. Use the same or later start position.");
                engine->haltExecution();
            } else if (m_startPosObject && (!anchors || anchors->empty())) {
                engine->setStartPosWarningKey("Macro lacks anchor data for start position playback. Re-record to enable.");
                engine->tickOffset = 0;
                engine->startPosActive = false;
            } else {
                engine->tickOffset = 0;
                engine->startPosActive = false;
                engine->clearStartPosWarning();

                // Generalized non-standard-start support: re-anchor the timeline
                // to wherever the character actually stands. This is what makes a
                // MID-RUN recording playable (record with the bot already in the
                // level, then start playback from the same spot) and lets a
                // from-start macro be played from any later position.
                const bool positionAligned = alignByPosition(refX, refY);
                if (!positionAligned && midRun) {
                    skkbot2::log::warn("PLAY aborted mid-run macro: data starts at tick {} p1=({:.0f},{:.0f}) but the "
                                       "player's position ({:.0f},{:.0f}) has no recorded coverage - start playback "
                                       "from the position where recording began or re-record from the level start",
                        recFirstTick, recFirstX, recFirstY, refX, refY);
                    engine->setStartPosWarningKey("Macro was recorded mid-run. Start playback from the position where recording began, or re-record from the level start.");
                    engine->haltExecution();
                }

                // Playback continuity across a practice checkpoint respawn (or
                // a PLAY armed while the player is already past the start):
                // recorded perFrame/input/anchor streams start at the level
                // start (tick 0) even when the recording kept data across a
                // practice death (stream stitched: [0..respawnTick-1] from
                // attempt A, [respawnTick..] from attempt B). Keeping the
                // indices at 0 would re-consume stale attempt-A/level-start
                // data and pin the player far from the live respawn position
                // (massive drift, portal section teleports). Seek every
                // playback cursor to the first record at/after the current
                // checkpoint levelTime tick and continue from there. Skipped
                // when position alignment already placed the cursors.
                if (!positionAligned && engine->skMode && !engine->activePersistencePlaybackAttempt()) {
                    int respawnTick = computePlaybackTick(this);
                    // BUG-06: `computePlaybackTick` returns the game-time label
                    // (m_levelTime*tickRate), but the recorded streams are
                    // labelled with the capture step clock which drifts from it
                    // after every practice respawn (m_levelTime resets, step
                    // clock does not). Seeking by the raw game tick lands on the
                    // DYING tail of the previous segment (~130px behind the
                    // checkpoint) and re-opens the seam at the next Play respawn.
                    // Translate through the checkpoint bundle the player just
                    // respawned from: priorTick+1 = the first retry frame.
                    if (engine->skMode) {
                        int stepRespawn = engine->respawnStepTickForGameTick(respawnTick);
                        if (stepRespawn > 1) {
                            respawnTick = stepRespawn;
                        }
                    }
                    if (respawnTick > 1) {
                        size_t pfiNew = 0;
                        long long ipiNew = -1, aniNew = -1;
                        if (auto* frames = engine->activePerFrames()) {
                            pfiNew = findFirstPerFrameAtTick(*frames, respawnTick);
                            engine->perFramePlaybackIndex = pfiNew;
                        }
                        if (auto* inputList = engine->activeSkInputs()) {
                            ipiNew = static_cast<long long>(findFirstSkInputAtTick(*inputList, respawnTick));
                            engine->executeIndex = static_cast<size_t>(ipiNew);
                        }
                        if (auto const* anchors = engine->activeAnchors()) {
                            if (!anchors->empty()) {
                                aniNew = static_cast<long long>(findFirstAnchorAtTick(*anchors, respawnTick));
                                engine->playbackAnchorIndex = static_cast<size_t>(aniNew);
                            }
                        }
                        skkbot2::log::info(
                            "PLAY checkpoint-realign: respawnTick={} practice={} progressPct={} -> pfi={} ipi={} ani={}",
                            respawnTick, inPractice, tick, pfiNew, ipiNew, aniNew);
                    }
                }
            }
        }

        if (engine->engineMode == MODE_CAPTURE && engine->skMode && engine->activeSk && engine->persistenceMode) {
            if (!checkpointMode || !progressed || engine->storedRestorePoints.empty()) {
                engine->flushPendingPersistenceCaptureAttempt();
                engine->resetPersistenceCaptureAttempt();
                engine->storedRestorePoints.clear();
                engine->resetRecordedHoldShadow();
                clearPlayerHolds();
            }
        } else if (engine->engineMode == MODE_CAPTURE && engine->skMode && engine->activeSk) {
            if (wipeCapture) {
                engine->clearFlatCaptureTimeline();
                engine->resetRecordedHoldShadow();
                clearPlayerHolds();
            }
        } else if (engine->engineMode == MODE_CAPTURE && !engine->skMode && engine->activeMacro) {
            if (!checkpointMode || !progressed || engine->storedRestorePoints.empty()) {
                engine->activeMacro->inputs.clear();
                engine->activeMacro->anchors.clear();
                engine->storedRestorePoints.clear();
                engine->resetRecordedHoldShadow();
                clearPlayerHolds();
            }
        }
    }

    void updateAttempts() {
        PlayLayer::updateAttempts();
    }

    void resetLevel() {
        FrameStepper::get().onResetLevel();
        auto* engine = ReplayEngine::get();
        engine->clearFrameStepState();
        engine->resetNoclipAccuracyStats();
        const auto restoreSerial = engine->captureRestoreSerial;
        engine->stepPairCount = 0;  // step clock restarts with the attempt
        engine->stepPairFrac = 0.0;
        engine->captureIgnored = true;
        CheckpointObject* injectedPlatformerCheckpoint = nullptr;
        if (m_isPlatformer && m_checkpointArray && m_checkpointArray->count() == 0 &&
            !engine->platformerCheckpointObjects.empty()) {
            injectedPlatformerCheckpoint = engine->platformerCheckpointObjects.back();
            if (m_checkpointArray->lastObject() != injectedPlatformerCheckpoint) {
                m_checkpointArray->addObject(injectedPlatformerCheckpoint);
                skkbot2::log::info(
                    "CAPTURE/PLAY setup-checkpoint reset: selected saved object={} "
                    "ahead of GD array tail",
                    static_cast<void*>(injectedPlatformerCheckpoint));
            } else {
                injectedPlatformerCheckpoint = nullptr;
            }
        }

        engine->pinSetupSeedBeforeReset();
        resetSimulationTimingState();
        // Death/restart seam: GD keeps buffered presses from the death
        // animation; the flush on the first new-attempt frame would make
        // the respawned player jump on its own. Clear the queue and arm the
        // phantom-suppression window (labels restart at 1 -> +8).
        m_queuedButtons.clear();
        PlayLayer::resetLevel();
        if (injectedPlatformerCheckpoint && m_checkpointArray &&
            m_checkpointArray->count() > 0 &&
            m_checkpointArray->lastObject() == injectedPlatformerCheckpoint) {
            m_checkpointArray->removeLastObject();
        }
        m_queuedButtons.clear();
        resetSimulationTimingState();
        if (engine->captureRestoreSerial == restoreSerial) {
            engine->captureReleaseSuppressUntil = 8;
            engine->resetRecordedHoldShadow();
        } else {
            skkbot2::log::info(
                "[SKK] reset сохранил post-restore hold-тень: tick={} window={}",
                engine->lastTickIndex, engine->captureReleaseSuppressUntil);
        }
        engine->captureDeathPending = false;
        engine->captureIgnored = false;

        // Инструмент ЛД-RESET (проверка трекер-пункта «m_extraDelta = 0 на
        // каждом resetLevel»): GD обнуляет m_extraDelta сама внутри
        // GJBaseGameLayer::resetLevelVariables() (вызывается из
        // PlayLayer::resetLevel), но practice-путь восстанавливает
        // чекпоинт ПОСЛЕ этого. Логируем фактическое значение сразу после
        // базового resetLevel — если здесь когда-нибудь != 0, первый кадр
        // попытки получит ненулевой остаток (референс Xenia Bot.cpp:524).
        skkbot2::log::info(
            "RESET-EXTRA-DELTA: after resetLevel m_extraDelta={:.6f} "
            "practice={} progress={} platformer={}",
            static_cast<double>(m_extraDelta), m_isPracticeMode,
            m_gameState.m_currentProgress, m_isPlatformer);

        applyReplayResetState(m_isPracticeMode, m_gameState.m_currentProgress,
                              m_isPlatformer);
    }

    void resetLevelFromStart() {
        FrameStepper::get().onResetLevel();
        auto* engine = ReplayEngine::get();
        engine->clearFrameStepState();
        engine->resetNoclipAccuracyStats();
        engine->stepPairCount = 0;  // step clock restarts with the attempt
        engine->stepPairFrac = 0.0;
        engine->captureIgnored = true;
        engine->clearPlatformerCheckpoints();
        engine->pinSetupSeedBeforeReset();
        resetSimulationTimingState();
        m_queuedButtons.clear();
        PlayLayer::resetLevelFromStart();
        m_queuedButtons.clear();
        resetSimulationTimingState();
        skkbot2::log::info(
            "RESET-EXTRA-DELTA: after resetLevelFromStart m_extraDelta={:.6f} "
            "practice={} progress={} platformer={}",
            static_cast<double>(m_extraDelta), m_isPracticeMode,
            m_gameState.m_currentProgress, m_isPlatformer);
        engine->captureReleaseSuppressUntil = 8;
        engine->resetRecordedHoldShadow();
        engine->captureDeathPending = false;
        engine->captureIgnored = false;

        // Full restart: clear broken objects tracking
        BrokenObjectTracker::get().clear();

        // resetLevelFromStart means "back to tick 0", even in a platformer
        // level, so the checkpoint chain must not survive this one.
        applyReplayResetState(m_isPracticeMode, m_gameState.m_currentProgress,
                              false);
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        auto* engine = ReplayEngine::get();
        // Restore/restart seam: GD zeroes hold maps and the stale physical
        // releases arrive as ordinary events afterwards. Suppress releases
        // for a short window (presses always pass) and drop the hold shadow -
        // prevents phantom 'up' edges that cancel held orbs/rings at replay.
        engine->captureReleaseSuppressUntil = static_cast<int>(
            m_gameState.m_levelTime * engine->runtimeTickRate()) + 1 + 8;
        engine->resetRecordedHoldShadow();

        if (!checkpoint) {
            engine->clearFrameStepState();
            engine->resetNoclipAccuracyStats();
            resetSimulationTimingState();
            return PlayLayer::loadFromCheckpoint(checkpoint);
        }

        resetSimulationTimingState();
        engine->clearFrameStepState();
        engine->resetNoclipAccuracyStats();
        engine->resetTimingTracking();
        engine->respawnTickIndex = -1;
        engine->tickAccumulator = 0.0f;
        if (engine->engineMode == MODE_CAPTURE) {
            auto it = engine->storedRestorePoints.find(checkpoint);
            if (it == engine->storedRestorePoints.end()) {
            engine->captureIgnored = true;
            m_queuedButtons.clear();
            PlayLayer::loadFromCheckpoint(checkpoint);
            m_queuedButtons.clear();
            // Unknown checkpoint: re-disable broken objects after vanilla load
            BrokenObjectTracker::get().reDisableBroken();
                // Unknown checkpoint: rebuild the step clock from the
                // restored level time (1 call == 1 sub-step by definition).
                engine->stepPairCount = std::max(0, static_cast<int>(
                    m_gameState.m_levelTime * engine->runtimeTickRate()));
                engine->stepPairFrac = 0.0;
                engine->captureReleaseSuppressUntil = engine->stepPairCount + 9;
                engine->captureIgnored = false;
                resetSimulationTimingState();
                return;
            }

            // BUG-06 root: `tick` is the game-time label (m_levelTime*tickRate)
            // which GD resets on every practice respawn, while the recorded
            // perFrame/input/anchor streams are labelled with the capture step
            // clock that NEVER resets - the two drift apart after the first
            // practice death (test: touch at step 8551 stored as tick 8485).
            // Cutting the stream at game-time `tick` drops the checkpoint-touch
            // frame and every frame up to the death, re-opening the 130px seam.
            // The boundary the stream must keep is `priorTick` (= lastTickIndex
            // on touch = step-clock tick of the touch frame).
            engine->prepareCaptureRestore(it->second.priorTick);
            skkbot2::log::info("[CK] after prepareCaptureRestore priorTick={}", it->second.priorTick);
            skkbot2::Logger::get().flush();
            // Wrap the whole restore in capture-ignorance: GD flushes its
            // m_queuedButtons during load (phantom presses recorded as macro
            // inputs = 'player jumps on respawn'), and the restore's own
            // processCommands calls would advance the step counter past the
            // checkpoint label. Re-assert the label AFTER vanilla finished.
            engine->captureIgnored = true;
            engine->stepPairCount = std::max(0, it->second.priorTick - 1);
            engine->stepPairFrac = 0.0;
            engine->lastTickIndex = it->second.priorTick;
            skkbot2::log::info("[CK] stepPairCount={} lastTickIndex={}", engine->stepPairCount, engine->lastTickIndex);
            skkbot2::Logger::get().flush();
            engine->restoreLockedRngState(it->second.rng.fastRandState);
            skkbot2::log::info("[CK] after restoreLockedRngState");
            skkbot2::Logger::get().flush();
            engine->restoreLockedShakeState(it->second.rng.shakeState);
            skkbot2::log::info("[CK] after restoreLockedShakeState");
            skkbot2::Logger::get().flush();

            // Practice-restart boundary (capture): re-apply the checkpoint's
            // captured teleport AFTER the restore so the segment continues under
            // exactly the state the recording observed. Overrides the arm
            // fallback latch; playback reproduces the same globals verbatim via
            // applyRngBoundaryReseed.
            if (engine->rngLocked) {
                skk::restoreTripleRng({ it->second.rng.fastRandState,
                    it->second.rng.teleportState });
                engine->pendingRngBoundary.progress = it->second.tick;
                engine->pendingRngBoundary.primary = it->second.rng.fastRandState;
                engine->pendingRngBoundary.teleport = it->second.rng.teleportState;
                skkbot2::log::debug("rng boundary: restore-latched triple (0x{:X},0x{:X})",
                    engine->pendingRngBoundary.primary,
                    engine->pendingRngBoundary.teleport);
            }

            m_queuedButtons.clear();
            skkbot2::log::info("[CK] before PlayLayer::loadFromCheckpoint");
            logCheckpointRngPhase("capture-before-vanilla", it->second);
            skkbot2::Logger::get().flush();
            PlayLayer::loadFromCheckpoint(checkpoint);
            skkbot2::log::info("[CK] after PlayLayer::loadFromCheckpoint");
            skkbot2::seam_probe::log("skk-after-vanilla-load", m_player1, 0.f, it->second.priorTick);
            skkbot2::Logger::get().flush();
            m_queuedButtons.clear();
            CheckpointStateManager::restore(this, it->second);
            skkbot2::seam_probe::log("skk-after-bundle", m_player1, 0.f, it->second.priorTick);
            logCheckpointRngPhase("capture-after-restore", it->second);
            skkbot2::log::info("[CK] after CheckpointStateManager::restore");
            skkbot2::Logger::get().flush();
            // BUG-123 (12.11): armCheckpointMusicResync снят вместе с фиксом.

            // Сначала reconciliation: оригинальный handleButton/pushButton может
            // сразу выполнить updateJump(0), изменив vy/ground/jump-buffer.
            // Boundary state и anchor обязаны описывать итоговое состояние, а не
            // snapshot до boundary-input. Плейбэк физическую клавиатуру не сверяет.
            engine->finishCaptureRestore(this, it->second.priorTick);
            skkbot2::seam_probe::log("skk-after-reconcile", m_player1, 0.f, it->second.priorTick);

            // Сохраняем финальную границу после bundle + boundary-input.
            {
                const bool rTwoPlayer = m_levelSettings ? m_levelSettings->m_twoPlayerMode : false;
                const bool rPlatformer = m_isPlatformer;
                const bool rDual = m_gameState.m_isDualMode || rTwoPlayer;
                engine->captureResealBoundaryFrame(it->second.priorTick, m_player1, m_player2, rPlatformer, rDual);
            }

            skkbot2::seam_probe::log("skk-after-reseal", m_player1, 0.f, it->second.priorTick);
            skkbot2::seam_probe::log("skk-after-reseal", m_player2, 0.f, it->second.priorTick);

            engine->stepPairCount = std::max(0, it->second.priorTick - 1);
            engine->stepPairFrac = 0.0;
            engine->lastTickIndex = it->second.priorTick;
            engine->captureReleaseSuppressUntil = it->second.priorTick + 8;
            engine->captureIgnored = false;

            resetSimulationTimingState();
            return;
        }

        // MODE_EXECUTE: цель задаёт аргумент GD, а не время умирающей попытки.
        if (engine->engineMode == MODE_EXECUTE) {
            const int targetTick = static_cast<int>(
                checkpoint->m_gameState.m_levelTime * engine->runtimeTickRate()) + 1;
            auto saved = engine->storedRestorePoints.find(checkpoint);
            const bool cached = saved != engine->storedRestorePoints.end();
            int bestDist = std::numeric_limits<int>::max();
            if (!cached) {
                int bestIdx = -1;
                for (int i = 0; i < static_cast<int>(engine->practiceFixQueue.size()); ++i) {
                    const int dist = std::abs(engine->practiceFixQueue[i].tick - targetTick);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestIdx = i;
                    }
                }
                if (bestIdx >= 0 && bestDist <= 2) {
                    auto bundle = std::move(engine->practiceFixQueue[bestIdx]);
                    engine->practiceFixQueue.erase(engine->practiceFixQueue.begin() + bestIdx);
                    saved = engine->storedRestorePoints.emplace(checkpoint, std::move(bundle)).first;
                }
            }

            if (saved != engine->storedRestorePoints.end()) {
                auto& ref = saved->second;
                skk::bridge::resolveBrokenObjects(ref, this);
                skkbot2::log::info("P1 consume: RESTORE object={} targetTick={} cached={} tick={} priorTick={} broken={} variance={} queueRemaining={}",
                    static_cast<void*>(checkpoint), targetTick, cached, ref.tick, ref.priorTick,
                    ref.brokenObjects.size(), ref.variance.size(), engine->practiceFixQueue.size());

                engine->prepareCaptureRestore(ref.priorTick);
                engine->captureIgnored = true;
                engine->stepPairCount = std::max(0, ref.priorTick - 1);
                engine->stepPairFrac = 0.0;
                engine->lastTickIndex = ref.priorTick;
                engine->restoreLockedRngState(ref.rng.fastRandState);
                engine->restoreLockedShakeState(ref.rng.shakeState);
                if (engine->rngLocked) {
                    skk::restoreTripleRng({ref.rng.fastRandState, ref.rng.teleportState});
                }

                m_queuedButtons.clear();
                logCheckpointRngPhase("play-before-vanilla", ref);
                PlayLayer::loadFromCheckpoint(checkpoint);
                m_queuedButtons.clear();
                CheckpointStateManager::restore(this, ref);
                logCheckpointRngPhase("play-after-restore", ref);
                // BUG-123 (12.11): armCheckpointMusicResync снят вместе с фиксом.

                // Playback follows the macro's stitched edge stream; vanilla
                // checkpoint loading has already applied the serialized input
                // latch via CheckpointStateManager::restore.
                engine->stepPairCount = std::max(0, ref.priorTick - 1);
                engine->stepPairFrac = 0.0;
                engine->lastTickIndex = ref.priorTick;
                engine->captureReleaseSuppressUntil = ref.priorTick + 8;
                engine->captureIgnored = false;
                resetSimulationTimingState();
                return;
            }
            skkbot2::log::warn("P1 consume: нет снимка object={} targetTick={} queue={} bestDist={}, vanilla restore",
                static_cast<void*>(checkpoint), targetTick, engine->practiceFixQueue.size(), bestDist);
        }

        PlayLayer::loadFromCheckpoint(checkpoint);
        m_queuedButtons.clear();
        // BUG-123 (12.11): armCheckpointMusicResync снят вместе с фиксом.

        // The vanilla checkpoint owns the captured jump-buffer mirror here.

        // Re-disable broken objects after vanilla loadFromCheckpoint
        // (GD re-enables breakables on load; our tracker keeps them destroyed)
        BrokenObjectTracker::get().reDisableBroken();
        resetSimulationTimingState();
    }

    void delayedResetLevel() {
        auto* engine = ReplayEngine::get();
        engine->clearFrameStepState();
        engine->resetNoclipAccuracyStats();
        resetSimulationTimingState();
        PlayLayer::delayedResetLevel();
        resetSimulationTimingState();
    }

    // Anti-SSB: the vanilla progress bar / death percent / completion stats
    // assume the native 240 tick grid (progress = timestamp-derived position
    // count, doubled in 2.208+). At runtime TPS != 240 feed them the
    // time-derived percentage instead (Eclipse/GDH/ToastyLite approach),
    // restoring the original value right after the vanilla call.
    int32_t ssbProgressFixBegin() {
        auto* engine = ReplayEngine::get();
        const int32_t saved = m_gameState.m_currentProgress;
        if (m_level && m_level->m_timestamp > 0 && engine &&
            skk::ssb::progressFixNeeded(engine->runtimeTickRate())) {
            const double progress = skk::ssb::timeProgressPercent(this);
            m_gameState.m_currentProgress =
                static_cast<int32_t>(static_cast<double>(m_level->m_timestamp) * progress * 2.0 / 100.0);
        }
        return saved;
    }

    void ssbProgressFixEnd(int32_t saved) {
        m_gameState.m_currentProgress = saved;
    }

    void updateProgressbar() {
        const int32_t saved = ssbProgressFixBegin();
        PlayLayer::updateProgressbar();
        ssbProgressFixEnd(saved);
    }

    void levelComplete() {
        auto* engine = ReplayEngine::get();
        bool wasExecuting = engine->engineMode == MODE_EXECUTE;
        bool wasCapturing = engine->engineMode == MODE_CAPTURE;
        bool wasCapturingPersistence = engine->canUsePersistenceCapture();
        engine->clearFrameStepState();

        if (wasCapturingPersistence) {
            engine->completePersistenceCaptureAttempt();
        }

        // levelComplete() finalizes attempt stats from m_commandIndex (a raw
        // 240-grid tick count); at a foreign runtime TPS re-derive it from
        // level time so the completion point is truthful (Eclipse/GDH parity).
        const bool commandIndexFix = engine &&
            skk::ssb::progressFixNeeded(engine->runtimeTickRate());
        const uint32_t savedCommandIndex = m_gameState.m_commandIndex;
        if (commandIndexFix) {
            m_gameState.m_commandIndex = static_cast<uint32_t>(
                std::llround(m_gameState.m_levelTime * 480.0));  // x2: 2.208 doubling
        }

        PlayLayer::levelComplete();

        if (commandIndexFix) {
            m_gameState.m_commandIndex = savedCommandIndex;
        }

        if (wasExecuting) {
            engine->disablePersistenceModeSafeguard();
            engine->haltExecution();
        } else if (wasCapturing || wasCapturingPersistence) {
            // Completion ends the recording session in BOTH modes. The
            // persistence-only branch (an upstream Toasty leak) neither halted
            // nor saved, so MODE_CAPTURE stayed armed past the win screen and
            // the next retry's restart-clear silently wiped the finished
            // timeline — the Auto Save Mod that used to sit here was the same
            // code commit 13774ee removed, and has now been deleted outright at
            // the user's request. Only the HALT is kept: saving the macro here
            // is what the SAVE button is for, and halting is an independent
            // correctness fix that must not regress with it.
            engine->haltExecution();
            skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto* engine = ReplayEngine::get();
        int deathTick = computePlaybackTick(this);
        bool deathPlayer2 = player && player == m_player2;
        bool persistenceDeath = engine->canUsePersistenceCapture() || engine->isPersistencePlaybackFailureAttempt();
        if (engine->canUsePersistenceCapture()) {
            engine->finalizePersistenceCaptureDeath(deathTick, deathPlayer2);
        } else if (engine->isPersistencePlaybackFailureAttempt()) {
            engine->markPersistencePlaybackDeathPending();
        }

        if (shouldNoclipPlayer(this, player, engine) && !persistenceDeath) {
            if (object == m_anticheatSpike) {
                engine->clearFrameStepState();
                const int32_t savedProgress = ssbProgressFixBegin();
                PlayLayer::destroyPlayer(player, object);
                ssbProgressFixEnd(savedProgress);
                return;
            }

            engine->markNoclipUnsafeFrame(player, object);
            return;
        }

        engine->resetNoclipAccuracyStats();
        engine->clearFrameStepState();
        const bool preDead = player && player->m_isDead;
        const int32_t savedProgress = ssbProgressFixBegin();
        PlayLayer::destroyPlayer(player, object);
        ssbProgressFixEnd(savedProgress);

        // True-death forensic: only when the game ACTUALLY killed the player.
        // resetLevel cleanup calls destroyPlayer too, but those never stick -
        // logging them produced the tick-1..17 barrage of false positives.
        if (!preDead && player && player->m_isDead &&
            engine->engineMode == MODE_CAPTURE) {
            engine->captureDeathPending = true;
            skkbot2::log::info(
                "CAPTURE death latched: player{} tick={} - input/trajectory capture "
                "paused until reset; death chain is not recorded",
                deathPlayer2 ? 2 : 1, deathTick);
        }
        if (!preDead && player && player->m_isDead &&
            engine->engineMode == MODE_EXECUTE && !persistenceDeath) {
            const SkPerFrame* rec = nullptr;
            if (auto* frames = engine->activePerFrames()) {
                auto it = std::upper_bound(frames->begin(), frames->end(), deathTick,
                    [](int t, const SkPerFrame& f) { return t < f.tick; });
                if (it != frames->begin()) {
                    --it;
                    rec = &*it;
                }
            }
            engine->probeAroundTick = deathTick;
            skkbot2::log::warn(
                "playback death: tick={} idx={} nextTick={} victimP2={} | killer id={} type={} at({:.1f},{:.1f}) | "
                "live p1=({:.1f},{:.1f}) | "
                "live p2=({:.1f},{:.1f}) | rec@{} p1=({:.1f},{:.1f}) p2=({:.1f},{:.1f})",
                deathTick, engine->executeIndex,
                [&] { auto* l = engine->activeSkInputs();
                     return (l && engine->executeIndex < l->size()) ? (*l)[engine->executeIndex].tick : -1; }(),
                deathPlayer2,
                object ? static_cast<long long>(object->m_objectID) : -1LL,
                object ? static_cast<int>(object->m_objectType) : -1,
                object ? object->getPositionX() : 0.f,
                object ? object->getPositionY() : 0.f,
                m_player1 ? m_player1->getPositionX() : -1.f,
                m_player1 ? m_player1->getPositionY() : -1.f,
                m_player2 ? m_player2->getPositionX() : -1.f,
                m_player2 ? m_player2->getPositionY() : -1.f,
                rec ? rec->tick : -1,
                rec ? rec->p1x : 0.f, rec ? rec->p1y : 0.f,
                rec ? rec->p2x : 0.f, rec ? rec->p2y : 0.f);
        }
    }

    void onQuit() {
        auto* engine = ReplayEngine::get();
        // Editor playtest exit is a PlayLayer quit too, but the session is not
        // over: the user will press playtest again, so the recording must stay
        // armed and nothing may be discarded (persistence attempts accumulate
        // across playtests). During playtest both layers exist at once, which
        // is the only reliable signal on this codebase (no playtest flag is
        // maintained elsewhere). Toasty's engine shares this quit path and
        // wipes the macro on every playtest stop.
        bool editorPlaytest = skk::gameplay::asPlayLayer() && skk::gameplay::asEditor();
        auto quitLevel = [this, engine, editorPlaytest]() {
            engine->clearFrameStepState();
            engine->resetNoclipAccuracyStats();
            engine->pendingPlaybackStart = false;
            if (engine->engineMode == MODE_CAPTURE) {
                if (!editorPlaytest) {
                    // Exiting ends the recording session, but the macro stays
                    // in memory by design: halt flushes the survivor run into
                    // main/chain and releases capture state; nothing is written
                    // to disk, and the macro remains saveable/playable from
                    // the menu (and re-entering the same level can continue).
                    skkbot2::log::info("RECORD: level exited - session halted, macro KEPT in memory (unsaved)");
                    // Halt before closing the session: the RECORD-ended summary
                    // belongs in record.log.
                    engine->haltExecution();
                    skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
                }
            } else if (engine->engineMode == MODE_EXECUTE) {
                skkbot2::log::info("PLAY: level exited - playback halted");
                engine->haltExecution();
                skkbot2::Logger::get().setSessionMode(skkbot2::Logger::SessionMode::None);
            }
            if (!editorPlaytest) {
                engine->disablePersistenceModeSafeguard();
            }
            skk::advRandomClear();
            // The level exit discards the live checkpoint map, but the macro
            // stays in memory and remains saveable from the menu. The save
            // path (saveActiveMacro) reads storedRestorePoints, which would
            // otherwise be gone by the time the user clicks Save in the GUI,
            // silently dropping every practice-fix checkpoint recorded this
            // session (BUG-19). Materialize them into the macro now so the
            // pending save still sees them.
            if (engine->engineMode == MODE_CAPTURE && !editorPlaytest
                && engine->skMode && engine->activeSk
                && !engine->storedRestorePoints.empty()) {
                engine->activeSk->practiceFixCheckpoints = skk::bridge::captureRestorePointsToWire(
                    engine->storedRestorePoints,
                    engine->activeSk->framerate > 0.0 ? engine->activeSk->framerate : engine->tickRate);
                skkbot2::log::info(
                    "P1 quit: preserved practiceFixCheckpoints into macro (count={})",
                    engine->activeSk->practiceFixCheckpoints.size());
            }
            engine->storedRestorePoints.clear();
            engine->clearPlatformerCheckpoints();
            engine->practiceFixQueue.clear();
            BrokenObjectTracker::get().clear();
            FrameStepper::get().onPlayLayerDestroy();
            PlayLayer::onQuit();
        };

        quitLevel();
    }
};

class $modify(SetupCheckpointCapture, GJBaseGameLayer) {
    void checkpointActivated(CheckpointGameObject* object) {
        GJBaseGameLayer::checkpointActivated(object);

        auto* engine = ReplayEngine::get();
        auto* layer = PlayLayer::get();
        if (!layer || !engine || engine->engineMode != MODE_CAPTURE ||
            !engine->skMode || !engine->activeSk || !layer->m_isPlatformer ||
            engine->captureDeathPending ||
            (layer->m_player1 && layer->m_player1->m_isDead) ||
            (layer->m_player2 && layer->m_player2->m_isDead)) {
            return;
        }

        // This is the actual live touch of a platformer setup-checkpoint.
        // Do not infer it from CheckpointObject creation, which may happen as
        // GD prepares the later death-respawn.
        const int tick = static_cast<int>(
            layer->m_gameState.m_levelTime * engine->runtimeTickRate()) + 1;
        skkbot2::log::info(
            "CAPTURE checkpointActivated observed: tick={} object={} platformer=true",
            tick, object ? object->m_objectID : -1);
    }
};

// NOTE: The former RecordingGuardPauseLayer ($modify of PauseLayer::goEdit/onQuit)
// only existed to raise the unsaved-macro popup, which the user asked to remove.

class $modify(NoclipBaseLayer, GJBaseGameLayer) {
#ifdef GEODE_IS_MACOS
    void update(float dt) {
        GJBaseGameLayer::update(dt);
        processNoclipAccuracyFrame(PlayLayer::get());
    }
#else
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        processNoclipAccuracyFrame(PlayLayer::get());
    }
#endif
};
