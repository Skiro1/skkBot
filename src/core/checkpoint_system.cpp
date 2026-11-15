#include "skkBot.hpp"
#include "core/gameplay_layer.hpp"
#include "core/start_position_policy.hpp"
#include "physics/anti_ssb.hpp"
#include "core/checkpoint_handler.hpp"
#include "core/broken_object_tracker.hpp"
#include "core/physical_keys.hpp"
#include "core/paths.hpp"
#include "core/practice_fix_bridge.hpp"
#include "core/playback_align.hpp"
#include "hacks/physicsbypass.hpp"
#include "render/renderer.hpp"
#include "trajectory/trajectory.hpp"
#include "skc/skc_bot.hpp"

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

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

        if (isLevelStart || attemptStart) {
            uint64_t teleportSeed = skk::lcgAdvance(computedSeed);
            skk::pinTripleSeedAtReset(computedSeed, teleportSeed);

            skk::advRandomReseed(computedSeed);

            skkbot2::log::info("rng lock armed: seedBase={} progress={} -> "
                "primary=0x{:X} teleport=0x{:X}",
                seedBase, playLayer->m_gameState.m_currentProgress,
                computedSeed, teleportSeed);

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

        if (!storedRestorePoints.empty() || activeSk->practiceFixCheckpoints.empty()) {
            activeSk->practiceFixCheckpoints = skk::bridge::captureRestorePointsToWire(
                storedRestorePoints, activeSk->framerate > 0.0 ? activeSk->framerate : tickRate);
        }
        skkbot2::log::info("P1 save: practiceFixCheckpoints written={}", activeSk->practiceFixCheckpoints.size());

        if (macroSaveInFlight) {
            skkbot2::log::warn("P1 save: a macro save is already in flight - duplicate skipped");
            return false;
        }

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
        macroSaveInFlight = true;
        std::thread worker([this, live, snapshot = std::move(snapshot)]() mutable {
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
            geode::queueInMainThread([this, live, ok, resolvedName = std::move(resolvedName), snapshot = std::move(snapshot)]() mutable {
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
                if (skMode && activeSk && activeSk == live) {
                    activeSk->name = snapshot.name;
                    activeSk->persistedName = snapshot.persistedName;
                    activeSk->duration = snapshot.duration;
                    activeSk->recordTimestamp = snapshot.recordTimestamp;
                    activeSk->fileFormat = SkFileFormat::SKK3;
                    activeSk->accuracyMode = snapshot.accuracyMode;
                    activeSk->exactCbsTiming = snapshot.exactCbsTiming;
                }
                skkbot2::log::info("[SAVE] main-thread callback: dataModified=false, reloadMacroList");
                skkbot2::Logger::get().flush();
                dataModified = false;
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
            release.timeSeconds = -1.0;
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
    if (auto* playLayer = PlayLayer::get()) {
        tickStartTimestamp = playLayer->m_timestamp;
    }
    if (engineMode == MODE_CAPTURE) {
        truncateRecordedDataAfter(tick);
        captureOnRespawn(tick);
    }
    skipActionTick = tick + 1;
    tickAccumulator = 0.0f;
}

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

            const bool rawP2 = handleButtonPlayer2Param(slot == 1, flip);
            const bool storedP2 = hasSecondPlayer ? rawP2 : false;
            const int shadowIndex =
                (storedP2 ? 1 : 0) * 3 + (button - 1);
            recordedHoldShadow[shadowIndex] = physicalHeld;
            if (snapshotHeld == physicalHeld) {
                continue;
            }

            playLayer->GJBaseGameLayer::handleButton(physicalHeld, button, rawP2);
            recordSkAction(tick, button, storedP2, physicalHeld, 0.0f, -1.0);
            ++transitions;
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
}

void ReplayEngine::captureResealBoundaryFrame(
    int tick, PlayerObject* p1, PlayerObject* p2, bool isPlatformer, bool isDual) {
    if (!skMode || !activeSk || canUsePersistenceCapture()) {
        skkbot2::log::debug("CAPTURE reseal: skip (skMode={} activeSk={} persistence={}) tick={}",
            skMode ? 1 : 0, activeSk ? 1 : 0, canUsePersistenceCapture() ? 1 : 0, tick);
        return;
    }

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
    SkPerFrame fresh;
    fresh.tick = tick;
    if (p1) {
        auto pos = p1->getPosition();
        fresh.p1x = pos.x;
        fresh.p1y = pos.y;
        fresh.p1vy = p1->m_yVelocity;
        fresh.p1vyb = p1->m_yVelocityBeforeSlope;
        fresh.p1rot = p1->getRotation();
        fresh.p1vis = visualFlagsOf(p1);
        if (p1->m_dashRing) {
            fresh.p1RingId = static_cast<GameObject*>(p1->m_dashRing)->m_objectID;
        }
    }
    if (p2) {
        fresh.hasPlayer2 = true;
        auto pos = p2->getPosition();
        fresh.p2x = pos.x;
        fresh.p2y = pos.y;
        fresh.p2vy = p2->m_yVelocity;
        fresh.p2vyb = p2->m_yVelocityBeforeSlope;
        fresh.p2rot = p2->getRotation();
        fresh.p2vis = visualFlagsOf(p2);
    }
    frames.back() = std::move(fresh);

    float const dx = fresh.p1x - old.p1x;
    float const dy = fresh.p1y - old.p1y;
    float const dist = std::sqrt(dx * dx + dy * dy);
    skkbot2::log::info("CAPTURE reseal: boundary tick={} p1 old=({:.3f},{:.3f}) -> new=({:.3f},{:.3f}) "
        "d={:.2f}px frames={} rngLocked={}",
        tick, old.p1x, old.p1y, fresh.p1x, fresh.p1y, dist, frames.size(), rngLocked ? 1 : 0);

    activeSk->recordAnchor(tick, p1, p2, isPlatformer, isDual);
    if (!activeSk->anchors.empty() && activeSk->anchors.back().tick == tick) {
        skkbot2::log::debug("CAPTURE reseal: boundary anchor rewritten tick={} (anchors={})",
            tick, activeSk->anchors.size());
    } else {
        skkbot2::log::warn("CAPTURE reseal: boundary anchor NOT at tail (back.tick={}, anchors={})",
            activeSk->anchors.empty() ? -1 : activeSk->anchors.back().tick, activeSk->anchors.size());
    }
}

int ReplayEngine::respawnStepTickForGameTick(int gameTick) {
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
void spliceStagingBucket(std::vector<T>& main, std::vector<T>& staging, int floor) {
    for (auto const& item : staging) {
        if (item.tick >= floor) break;
        if (!main.empty() && item.tick <= main.back().tick) continue;
        main.push_back(item);
    }
}
}

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

void ReplayEngine::captureCommitStagingToFloor(int floorTick) {
    if (!captureStaging || !activeSk) {
        return;
    }
    spliceStagingBucket(activeSk->inputs, captureStaging->inputs, floorTick);
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
    spliceStagingBucket(activeSk->inputs, captureStaging->inputs, kNoFloor);
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

void ReplayEngine::restoreLockedShakeState(uint64_t shakeState) {
    if (shakeState != 0) {
        skk::shakeStateSet(shakeState);
    }
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
        skkbot2::log::debug("PLAY takeover wait: live=({:.0f},{:.0f}) no recorded coverage within {}/{}px (x/y)",
            refX, refY, kPlaybackTightTolerance, kPlaybackTakeoverYTolerance);
        return false;
    }

    if (!armExecutionState()) {
        return false;
    }
    applyVarianceOverrides();

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
        skk::advRandomClear();
        engine->clearPlatformerCheckpoints();
        bool ret = PlayLayer::init(level, dontCreateObjects);
        if (auto* engine = ReplayEngine::get()) {
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
            } else if (skk::skc::Bot::get().ownsTiming()) {
            } else {
                ReplayEngine::applyRuntimeAccuracyMode(engine->selectedAccuracyMode,
                                                       engine->cosEnabled);
            }
        }
        return ret;
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        const bool setupCheckpoint = m_activatedCheckpoint != nullptr;
        PlayLayer::storeCheckpoint(checkpoint);

        auto* engine = ReplayEngine::get();
        if (checkpoint && m_isPlatformer && setupCheckpoint && engine &&
            (engine->engineMode == MODE_CAPTURE || engine->engineMode == MODE_EXECUTE)) {
            engine->trackPlatformerCheckpoint(checkpoint);
        }
        if (!checkpoint || !engine || engine->engineMode != MODE_CAPTURE ||
            !engine->skMode || !engine->activeSk || skk::trajectory::isSimulating() ||
            engine->captureDeathPending ||
            (m_player1 && m_player1->m_isDead) ||
            (m_player2 && m_player2->m_isDead)) {
            return;
        }

        const int tick = static_cast<int>(
            m_gameState.m_levelTime * engine->runtimeTickRate()) + 1;
        auto bundle = CheckpointStateManager::capture(
            tick, engine->lastTickIndex, this, engine->captureRngState());
        const bool hasFull = bundle.hasFullState;
        engine->storedRestorePoints[checkpoint] = std::move(bundle);

        engine->captureOnCheckpointTouched(tick);
        skkbot2::log::info(
            "CAPTURE checkpoint stored: tick={} platformer={} activatedSetup={} "
            "restorePoints={} hasFull={}",
            tick, m_isPlatformer, setupCheckpoint,
            engine->storedRestorePoints.size(), hasFull);
    }

    void applyReplayResetState(bool inPractice, int tick,
                               bool platformerCheckpointMode = false) {
        auto* engine = ReplayEngine::get();

        const bool checkpointMode = inPractice || platformerCheckpointMode;

        const bool checkpointPresent =
            m_checkpointArray && m_checkpointArray->count() > 0;
        const bool progressed = platformerCheckpointMode ? checkpointPresent
                                                         : (tick > 1);
        const bool wipeCapture = !checkpointMode || !progressed ||
                                 engine->storedRestorePoints.empty();
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

        refreshRngState(true);
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

            int recFirstTick = 0;
            float recFirstX = 0.f, recFirstY = 0.f;
            bool midRun = false;
            if (engine->skMode && engine->activeSk) {
                recFirstTick = static_cast<int>(engine->activeSk->recordFirstTick());
                midRun = engine->activeSk->startedMidRun();
                recFirstX = engine->activeSk->recordStartX();
                recFirstY = engine->activeSk->recordStartY();
            }

            const bool haveRef = m_startPosObject != nullptr || m_player1 != nullptr;
            float refX = m_startPosObject
                ? m_startPosObject->getPositionX()
                : (m_player1 ? m_player1->getPositionX() : 0.f);
            float refY = m_startPosObject
                ? m_startPosObject->getPositionY()
                : (m_player1 ? m_player1->getPositionY() : 0.f);

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
                if (!midRun) {
                    engine->tickOffset = 0;
                    engine->startPosActive = true;
                    if (anchors && !anchors->empty()) {
                        engine->playbackAnchorIndex = findFirstAnchorAtTick(*anchors, 0);
                    } else {
                        engine->playbackAnchorIndex = 0;
                    }
                    if (engine->skMode) {
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
                    engine->tickOffset = 0;
                    engine->startPosActive = false;
                    engine->clearStartPosWarning();
                } else {
                    int targetTick = matchStartPosTargetTick(engine, *anchors, refX, refY);
                    if (targetTick < 0) {
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

                const bool positionAligned = alignByPosition(refX, refY);
                if (!positionAligned && midRun) {
                    skkbot2::log::warn("PLAY aborted mid-run macro: data starts at tick {} p1=({:.0f},{:.0f}) but the "
                                       "player's position ({:.0f},{:.0f}) has no recorded coverage - start playback "
                                       "from the position where recording began or re-record from the level start",
                        recFirstTick, recFirstX, recFirstY, refX, refY);
                    engine->setStartPosWarningKey("Macro was recorded mid-run. Start playback from the position where recording began, or re-record from the level start.");
                    engine->haltExecution();
                }

                if (!positionAligned && engine->skMode && !engine->activePersistencePlaybackAttempt()) {
                    int respawnTick = computePlaybackTick(this);
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
            if (!checkpointMode || !progressed || engine->storedRestorePoints.empty()) {
                engine->activeSk->inputs.clear();
                engine->activeSk->anchors.clear();
                engine->activeSk->perFrames.clear();
                engine->storedRestorePoints.clear();
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
        engine->stepPairCount = 0;
        engine->stepPairFrac = 0.0;
        engine->captureIgnored = true;
        CheckpointObject* injectedPlatformerCheckpoint = nullptr;
        if (m_isPlatformer && m_checkpointArray &&
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
        m_queuedButtons.clear();
        PlayLayer::resetLevel();
        if (injectedPlatformerCheckpoint && m_checkpointArray &&
            m_checkpointArray->count() > 0 &&
            m_checkpointArray->lastObject() == injectedPlatformerCheckpoint) {
            m_checkpointArray->removeLastObject();
        }
        m_queuedButtons.clear();
        resetSimulationTimingState();
        engine->captureReleaseSuppressUntil = 8;
        engine->resetRecordedHoldShadow();
        engine->captureDeathPending = false;
        engine->captureIgnored = false;

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
        engine->stepPairCount = 0;
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

        BrokenObjectTracker::get().clear();

        applyReplayResetState(m_isPracticeMode, m_gameState.m_currentProgress,
                              false);
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        auto* engine = ReplayEngine::get();
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
            BrokenObjectTracker::get().reDisableBroken();
                engine->stepPairCount = std::max(0, static_cast<int>(
                    m_gameState.m_levelTime * engine->runtimeTickRate()));
                engine->stepPairFrac = 0.0;
                engine->captureReleaseSuppressUntil = engine->stepPairCount + 9;
                engine->captureIgnored = false;
                resetSimulationTimingState();
                return;
            }

            engine->prepareCaptureRestore(it->second.priorTick);
            skkbot2::log::info("[CK] after prepareCaptureRestore priorTick={}", it->second.priorTick);
            skkbot2::Logger::get().flush();
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

            if (engine->rngLocked && it->second.rng.teleportState != 0) {
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
            skkbot2::Logger::get().flush();
            PlayLayer::loadFromCheckpoint(checkpoint);
            skkbot2::log::info("[CK] after PlayLayer::loadFromCheckpoint");
            skkbot2::Logger::get().flush();
            m_queuedButtons.clear();
            CheckpointStateManager::restore(this, it->second);
            skkbot2::log::info("[CK] after CheckpointStateManager::restore");
            skkbot2::Logger::get().flush();

            {
                const bool rTwoPlayer = m_levelSettings ? m_levelSettings->m_twoPlayerMode : false;
                const bool rPlatformer = m_isPlatformer;
                const bool rDual = m_gameState.m_isDualMode || rTwoPlayer;
                engine->captureResealBoundaryFrame(it->second.priorTick, m_player1, m_player2, rPlatformer, rDual);
            }

            engine->finishCaptureRestore(this, it->second.priorTick);

            engine->stepPairCount = std::max(0, it->second.priorTick - 1);
            engine->stepPairFrac = 0.0;
            engine->lastTickIndex = it->second.priorTick;
            engine->captureReleaseSuppressUntil = it->second.priorTick + 8;
            engine->captureIgnored = false;

            resetSimulationTimingState();
            return;
        }

        if (engine->engineMode == MODE_EXECUTE && !engine->practiceFixQueue.empty()) {
            const double currentTickF = m_gameState.m_levelTime * engine->runtimeTickRate();
            const int currentTick = static_cast<int>(std::llround(currentTickF));

            int bestIdx = -1;
            int bestDist = std::numeric_limits<int>::max();
            for (int i = 0; i < static_cast<int>(engine->practiceFixQueue.size()); ++i) {
                int dist = std::abs(engine->practiceFixQueue[i].tick - currentTick);
                if (dist < bestDist) {
                    bestDist = dist;
                    bestIdx = i;
                }
            }

            skkbot2::log::debug("P1 consume: MODE_EXECUTE loadFromCheckpoint currentTick={} queueSize={} bestIdx={} bestDist={}",
                currentTick, engine->practiceFixQueue.size(), bestIdx, bestDist);

            if (bestIdx >= 0 && bestDist <= 2) {
                auto bundle = std::move(engine->practiceFixQueue[bestIdx]);
                engine->practiceFixQueue.erase(engine->practiceFixQueue.begin() + bestIdx);
                engine->storedRestorePoints[checkpoint] = std::move(bundle);
                auto& ref = engine->storedRestorePoints[checkpoint];
                skkbot2::log::info("P1 consume: RESTORE tick={} priorTick={} broken={} variance={} queueRemaining={}",
                    ref.tick, ref.priorTick, ref.brokenObjects.size(), ref.variance.size(),
                    engine->practiceFixQueue.size());

                engine->prepareCaptureRestore(ref.priorTick);
                engine->captureIgnored = true;
                engine->stepPairCount = std::max(0, ref.priorTick - 1);
                engine->stepPairFrac = 0.0;
engine->lastTickIndex = ref.priorTick;
 engine->restoreLockedRngState(ref.rng.fastRandState);
 engine->restoreLockedShakeState(ref.rng.shakeState);

                m_queuedButtons.clear();
                PlayLayer::loadFromCheckpoint(checkpoint);
                m_queuedButtons.clear();
                CheckpointStateManager::restore(this, ref);

                engine->stepPairCount = std::max(0, ref.priorTick - 1);
                engine->stepPairFrac = 0.0;
                engine->lastTickIndex = ref.priorTick;
                engine->captureReleaseSuppressUntil = ref.priorTick + 8;
                engine->captureIgnored = false;
                resetSimulationTimingState();
                return;
            }
            skkbot2::log::warn("P1 consume: no match (bestDist={}), falling back to vanilla", bestDist);
        } else if (engine->engineMode == MODE_EXECUTE) {
            skkbot2::log::debug("P1 consume: queue empty in MODE_EXECUTE, vanilla load");
        }

        PlayLayer::loadFromCheckpoint(checkpoint);
        m_queuedButtons.clear();


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

        const bool commandIndexFix = engine &&
            skk::ssb::progressFixNeeded(engine->runtimeTickRate());
        const uint32_t savedCommandIndex = m_gameState.m_commandIndex;
        if (commandIndexFix) {
            m_gameState.m_commandIndex = static_cast<uint32_t>(
                std::llround(m_gameState.m_levelTime * 480.0));
        }

        PlayLayer::levelComplete();

        if (commandIndexFix) {
            m_gameState.m_commandIndex = savedCommandIndex;
        }

        if (wasExecuting) {
            engine->disablePersistenceModeSafeguard();
            engine->haltExecution();
        } else if (wasCapturing || wasCapturingPersistence) {
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
        bool editorPlaytest = skk::gameplay::asPlayLayer() && skk::gameplay::asEditor();
        auto quitLevel = [this, engine, editorPlaytest]() {
            engine->clearFrameStepState();
            engine->resetNoclipAccuracyStats();
            engine->pendingPlaybackStart = false;
            if (engine->engineMode == MODE_CAPTURE) {
                if (!editorPlaytest) {
                    skkbot2::log::info("RECORD: level exited - session halted, macro KEPT in memory (unsaved)");
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

        const int tick = static_cast<int>(
            layer->m_gameState.m_levelTime * engine->runtimeTickRate()) + 1;
        skkbot2::log::info(
            "CAPTURE checkpointActivated observed: tick={} object={} platformer=true",
            tick, object ? object->m_objectID : -1);
    }
};


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
