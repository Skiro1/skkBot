#include "skc_batch_probe.hpp"
#include <chrono>
#include "skc_bot.hpp"
#include "skc_checkpoint.hpp"
#include "skc_frame.hpp"
#include "skc_rng.hpp"
#include "skc_tick_clock.hpp"
#include "core/step_trace.hpp"
#include "core/rng_lock.hpp"
#include "physics/step_grid_hooks.hpp"

#include "skkBot.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/binding/OBB2D.hpp>

#include "logger/logger.hpp"
#include "core/hotpath.hpp"

#include <cmath>

using namespace geode::prelude;

namespace skk::skc {

namespace {
constexpr uint32_t kProbeInterval = 240;

constexpr uint32_t kStateTraceFrames = 600;
constexpr uint32_t kDenseFrom = 190;
constexpr uint32_t kDenseTo = 460;
constexpr uint32_t kDispatchLogInputs = 400;

std::string heldMask(PlayerObject* player) {
    std::string out(3, '0');
    if (!player) return out;
    for (int button = 1; button <= 3; ++button) {
        auto it = player->m_holdingButtons.find(button);
        if (it != player->m_holdingButtons.end() && it->second) {
            out[static_cast<size_t>(button - 1)] = '1';
        }
    }
    return out;
}

namespace {
thread_local bool g_inGjblFlip = false;
}

std::string objectTag(GameObject* object) {
    if (!object) return "<none>";
    const auto pos = object->getPosition();
    return std::to_string(object->m_objectID) + "@(" +
           std::to_string(static_cast<int>(pos.x)) + "," +
           std::to_string(static_cast<int>(pos.y)) + ")";
}

struct SkcPortalItem {
    GameObject* obj = nullptr;
    CCRect rect{};
    bool inverse = false;
    bool toggle = false;
    float radius = 0.0f;
    bool outerOb = false;
};

struct SkcPortalRun {
    int lastContactFrame = -100000;
    int activated = 0;
};

static CCArray* g_skcPortalArray = nullptr;
static size_t g_skcPortalCount = 0;
static std::vector<SkcPortalItem> g_skcPortals;
static std::vector<SkcPortalRun> g_skcPortalRuns;
static uint32_t g_skcPortalForceCount = 0;
static int g_skcSweepLastFrame = 0;
static OBB2D* g_skcTmpPlayerObb = nullptr;

static void skcResetPortalCache() {
    g_skcPortalArray = nullptr;
    g_skcPortalCount = 0;
    g_skcPortals.clear();
    g_skcPortalRuns.clear();
}

static void skcClearPortalRuns() {
    g_skcPortalRuns.assign(g_skcPortals.size(), {});
}

static void skcRebuildPortalCache(GJBaseGameLayer* layer) {
    auto* objects = layer ? layer->m_objects : nullptr;
    if (!objects) {
        g_skcPortalArray = nullptr;
        g_skcPortalCount = 0;
        g_skcPortals.clear();
        g_skcPortalRuns.clear();
        return;
    }
    const size_t count = static_cast<size_t>(objects->count());
    if (g_skcPortalArray == objects && g_skcPortalCount == count) return;
    g_skcPortalArray = objects;
    g_skcPortalCount = count;
    g_skcPortals.clear();
    for (size_t i = 0; i < count; ++i) {
        auto* obj = dynamic_cast<GameObject*>(objects->objectAtIndex(i));
        if (!obj) continue;
        const auto type = obj->m_objectType;
        if (type != GameObjectType::InverseGravityPortal &&
            type != GameObjectType::NormalGravityPortal &&
            type != GameObjectType::GravityTogglePortal) {
            continue;
        }
        SkcPortalItem item;
        item.obj = obj;
        item.rect = obj->getObjectRect();
        item.toggle = type == GameObjectType::GravityTogglePortal;
        item.inverse = type == GameObjectType::InverseGravityPortal;
        item.radius = obj->m_objectRadius;
        if (!item.toggle) {
            const bool fixRadius = layer->m_levelSettings
                ? layer->m_levelSettings->m_fixRadiusCollision : true;
            item.outerOb = obj->m_shouldUseOuterOb &&
                (!fixRadius || obj->m_objectRadius <= 0.0f);
        }
        g_skcPortals.push_back(item);
    }
    skcClearPortalRuns();
    skkbot2::log::debug(
        "[SKC] gravity-portal cache rebuilt: objects={} portals={} "
        "(BUG-102 v2: gucci-style contact gate)",
        count, g_skcPortals.size());
    for (auto& it : g_skcPortals) {
        skkbot2::log::debug(
            "[SKC] portal {} obj={} toggle={} radius={:a} outerOb={} "
            "rect=({:a},{:a},{}x{})",
            it.toggle ? 2 : (it.inverse ? 1 : 0), objectTag(it.obj),
            it.toggle ? 1 : 0, static_cast<double>(it.radius),
            it.outerOb ? 1 : 0,
            static_cast<double>(it.rect.origin.x),
            static_cast<double>(it.rect.origin.y),
            static_cast<double>(it.rect.size.width),
            static_cast<double>(it.rect.size.height));
    }
}

static bool skcPortalContact(GJBaseGameLayer* layer, PlayerObject* player,
                             const SkcPortalItem& item,
                             const CCPoint& samplePos,
                             const CCRect& livePlayerRect) {
    const bool circle = item.radius > 0.0f;
    CCRect pre;
    if (circle) {
        const CCPoint c = item.obj->getPosition();
        const float r = item.radius;
        pre = CCRect(c.x - r, c.y - r, r * 2.0f, r * 2.0f);
    } else {
        pre = item.rect;
    }
    const float dx = samplePos.x - livePlayerRect.getMidX();
    const float dy = samplePos.y - livePlayerRect.getMidY();
    CCRect pr = livePlayerRect;
    pr.origin.x += dx;
    pr.origin.y += dy;
    if (pr.getMaxX() <= pre.getMinX() || pr.getMinX() >= pre.getMaxX() ||
        pr.getMaxY() <= pre.getMinY() || pr.getMinY() >= pre.getMaxY()) {
        return false;
    }
    if (item.outerOb) {
        OBB2D* box = item.obj->getOrientedBox();
        player->updateOrientedBox();
        const float w = player->m_width * player->m_spriteWidthScale *
            player->m_scaleX;
        const float h = player->m_height * player->m_spriteHeightScale *
            player->m_scaleY;
        const float rot = -player->getRotation();
        const CCPoint center(
            samplePos.x + player->getBoxOffset().x,
            samplePos.y + player->getBoxOffset().y);
        if (!g_skcTmpPlayerObb) {
            g_skcTmpPlayerObb = OBB2D::create(
                center, w, h, rot * 0.017453f);
            g_skcTmpPlayerObb->retain();
        } else {
            g_skcTmpPlayerObb->calculateWithCenter(
                center, w, h, rot * 0.017453f);
        }
        if (!(box->overlaps1Way(g_skcTmpPlayerObb) &&
              g_skcTmpPlayerObb->overlaps1Way(box))) {
            return false;
        }
    }
    return true;
}

static void skcPortalSweep(GJBaseGameLayer* layer, PlayerObject* player,
                           bool prevXValid, float prevX, float prevY) {
    if (!layer || !player || player->m_isDead) return;
    skcRebuildPortalCache(layer);
    if (g_skcPortals.empty()) return;

    const int frame = FrameClock::get().getFrame();
    if (frame < g_skcSweepLastFrame) {
        skcClearPortalRuns();
    }
    g_skcSweepLastFrame = frame;

    const CCPoint pos = player->getPosition();
    const CCRect livePlayerRect = player->getObjectRect();
    CCPoint prevPos(prevX, prevY);
    if (!prevXValid) {
        prevPos.x = pos.x;
        prevPos.y = pos.y;
    }
    for (size_t pi = 0; pi < g_skcPortals.size(); ++pi) {
        auto& item = g_skcPortals[pi];
        if (item.obj->m_isGroupDisabled || item.obj->m_isDisabled) continue;
        if (pi >= g_skcPortalRuns.size()) {
            g_skcPortalRuns.resize(pi + 1, {});
        }
        auto& run = g_skcPortalRuns[pi];
        const CCRect& r = item.rect;
        const float wMinX = std::min(prevPos.x, pos.x);
        const float wMaxX = std::max(prevPos.x, pos.x);
        if (r.getMaxX() < wMinX || r.getMinX() > wMaxX) continue;
        bool contactNow = false;
        int firstSample = -1;
        for (int s = 0; s <= 8; ++s) {
            const float t = static_cast<float>(s) / 8.0f;
            CCPoint sp = prevPos;
            sp.x += (pos.x - prevPos.x) * t;
            sp.y += (pos.y - prevPos.y) * t;
            if (skcPortalContact(layer, player, item, sp, livePlayerRect)) {
                contactNow = true;
                firstSample = s;
                break;
            }
        }
        const bool hadContact =
            frame - run.lastContactFrame <= 1;
        run.lastContactFrame = contactNow ? frame : run.lastContactFrame;
        if (!contactNow || hadContact) continue;
        if (item.obj->hasBeenActivatedByPlayer(player)) {
            continue;
        }
        if (layer->canBeActivatedByPlayer(
                player, static_cast<EffectGameObject*>(item.obj))) {
            continue;
        }
        const float beforeVy = player->m_yVelocity;
        player->m_lastPortalPos = item.obj->getPosition();
        player->m_lastActivatedPortal = item.obj;
        const bool flipVal =
            item.toggle ? !player->m_isUpsideDown : item.inverse;
        layer->flipGravity(
            player, flipVal,
            item.obj->m_hasNoEffects ? true : item.obj->m_isHide);
        item.obj->playShineEffect();
        item.obj->activatedByPlayer(player);
        run.activated++;
        ++g_skcPortalForceCount;
        skkbot2::log::info(
            "[SKC] gravity-portal sweep-apply f={} obj={} kind={} "
            "x=[{:a}..{:a}] sample={}/8 portalRect=({:a},{:a},{}x{}) "
            "vy={:a} post-ud={} (contact-edge flip, gucci-style)",
            frame, objectTag(item.obj),
            item.toggle ? "toggle" : (item.inverse ? "inverse" : "normal"),
            static_cast<double>(wMinX), static_cast<double>(wMaxX),
            firstSample < 0 ? 0 : firstSample,
            static_cast<double>(r.origin.x),
            static_cast<double>(r.origin.y),
            static_cast<double>(r.size.width),
            static_cast<double>(r.size.height),
            static_cast<double>(beforeVy),
            player->m_isUpsideDown ? 1 : 0);
    }
}


void dumpDeathGeometry(GJBaseGameLayer* layer, CCPoint center,
                       GameObject* killer) {
    if (!layer || !layer->m_objects) return;
    std::string nearby;
    int shown = 0;
    const int count = static_cast<int>(layer->m_objects->count());
    for (int i = 0; i < count && shown < 12; ++i) {
        auto* object =
            static_cast<GameObject*>(layer->m_objects->objectAtIndex(i));
        if (!object || object == killer) continue;
        const auto pos = object->getPosition();
        if (std::abs(pos.x - center.x) > 75.0f ||
            std::abs(pos.y - center.y) > 75.0f) {
            continue;
        }
        if (!nearby.empty()) nearby += ' ';
        nearby += objectTag(object);
        ++shown;
    }
    skkbot2::log::info(
        "[SKC] death geometry: killer={} nearby[{}]={} (level has {} objects)",
        objectTag(killer), shown, nearby.empty() ? "<none>" : nearby.c_str(),
        count);
}

void sanitizeSeamQueueAfterReset(GJBaseGameLayer* layer, const char* site) {
    auto& bot = Bot::get();
    if (!layer) return;
    if (!bot.isRecording() && !bot.isPlaying()) return;
    const auto mode = bot.isRecording() ? Bot::SeamQueueMode::Record
                                        : Bot::SeamQueueMode::Play;
    bot.sanitizeSeamQueue(layer->m_queuedButtons,
                          FrameClock::get().getFrame(), mode, site);
}
}

class $modify(SkcPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        auto& bot = Bot::get();

        bot.onLevelEnter(level);

        const bool hadPending = bot.getPendingAction() != PendingAction::None;
        if (hadPending) {
            bot.consumePendingAction(level);
            if (bot.isRecording()) {
                bot.bindLevel(level);
            }
        }

        return PlayLayer::init(level, dontCreateObjects);
    }

    void resetLevel() {
        auto& bot = Bot::get();
        auto& fix = PracticeFix::get();

        if (m_level) {
            skkbot2::log::info(
                "[SKC] level: '{}' (id {}) platformer={} twoPlayer={} "
                "dual={} length={} playState={}",
                m_level->m_levelName.c_str(),
                static_cast<long long>(m_level->m_levelID),
                m_level->isPlatformer() ? 1 : 0,
                m_level->m_twoPlayerMode ? 1 : 0,
                m_gameState.m_isDualMode ? 1 : 0, m_level->m_levelLength,
                bot.isPlaying() ? "PLAY" : (bot.isRecording() ? "RECORD" : "-"));
        }

        CheckpointObject* injectedCheckpoint = nullptr;
        if (m_isPlatformer && m_checkpointArray &&
            (bot.isRecording() || bot.isPlaying())) {
            auto* arrayTail = static_cast<CheckpointObject*>(
                m_checkpointArray->lastObject());
            injectedCheckpoint = fix.latestPlatformerCheckpoint();
            skkbot2::log::info(
                "[SKC] platformer reset selection: tracked={} latest={} "
                "arrayCount={} arrayTail={} latestTime={:.9f} tailTime={:.9f} "
                "latestProgress={} tailProgress={}",
                fix.getPlatformerCheckpointCount(),
                static_cast<void*>(injectedCheckpoint), m_checkpointArray->count(),
                static_cast<void*>(arrayTail),
                injectedCheckpoint ? static_cast<double>(
                    injectedCheckpoint->m_gameState.m_levelTime) : -1.0,
                arrayTail ? static_cast<double>(
                    arrayTail->m_gameState.m_levelTime) : -1.0,
                injectedCheckpoint
                    ? injectedCheckpoint->m_gameState.m_currentProgress : -1,
                arrayTail ? arrayTail->m_gameState.m_currentProgress : -1);
            if (injectedCheckpoint &&
                m_checkpointArray->lastObject() != injectedCheckpoint) {
                m_checkpointArray->addObject(injectedCheckpoint);
                skkbot2::log::info(
                    "[SKC] setup-checkpoint reset: selected saved object={} "
                    "ahead of GD array tail",
                    static_cast<void*>(injectedCheckpoint));
            } else {
                injectedCheckpoint = nullptr;
            }
        }

        if (bot.ownsTiming()) {
            bot.reapplySessionAccuracy();
        }

        const int tick = m_gameState.m_currentProgress;
        const bool inPractice = m_isPracticeMode != 0;
        const bool platformer = m_isPlatformer;
        const bool checkpointMode = inPractice || platformer;
        const bool hasSnapshots = fix.getSnapshotCount() > 0;
        const bool checkpointPresent =
            m_checkpointArray && m_checkpointArray->count() > 0;
        const bool progressed = platformer ? checkpointPresent : (tick > 1);
        const bool checkpointRespawn = checkpointMode && progressed &&
                                       checkpointPresent;
        const bool stitch = checkpointRespawn && hasSnapshots;

        skkbot2::log::debug(
            "[SKC] resetLevel: inPractice={} platformer={} tick={} "
            "frame={} checkpoints={} progressed={} snapshots={} -> {}",
            inPractice, platformer, tick, FrameClock::get().getFrame(),
            m_checkpointArray ? m_checkpointArray->count() : 0, progressed,
            hasSnapshots,
            stitch ? (platformer ? "platformer checkpoint respawn (stitch)"
                                 : "practice respawn (stitch)")
                   : "attempt from start (wipe)");

        if (!stitch) {
            FrameClock::get().reset();
            fix.clear();

            if (bot.isPlaybackArmed()) {
                bot.startPlayback();
            }
            if (bot.isRecording()) {
                bot.onResetEvent(Bot::ResetReason::AttemptFromStart, 0);
            } else {
                bot.restart();
            }
        } else {
            if (bot.isPlaybackArmed()) {
                bot.startPlayback();
            }
            uint32_t cpFrame = fix.lastCheckpointFrame();
            skkbot2::log::info(
                "[SKC] checkpoint respawn ({}{}) -> frame {} (checkpoints held: "
                "{}, recorded inputs: {}, mode: {})",
                platformer ? "platformer" : "practice",
                hasSnapshots ? "" : ", no snapshot",
                cpFrame, fix.getSnapshotCount(), bot.getInputCount(),
                bot.isRecording() ? "RECORD" :
                (bot.isPlaying() ? "PLAY" : "idle"));
            FrameClock::get().setFrame(cpFrame);

            if (bot.isRecording()) {
                bot.onResetEvent(Bot::ResetReason::PracticeRespawn, cpFrame);
            } else {
                bot.seekTo(cpFrame);
            }
        }

        if ((bot.isRecording() || bot.isPlaying()) && !stitch) {
            const uint64_t pinned = kSkcLayerSeed;
            const uint64_t oldPrimary = skk::captureTripleRng().primary;
            skk::restoreTripleRng({pinned, pinned});
            skkbot2::log::info(
                "[SKC] rng-pin f={} rec={} play={} primary {:#x} -> {:#x} "
                "(teleport тоже {:#x}) - детерминированный m_varianceIndex "
                "для всех объектов попытки (BUG-107)",
                FrameClock::get().getFrame(),
                bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                oldPrimary, pinned, pinned);
        }

        const auto tEntry0 = std::chrono::steady_clock::now();
        PlayLayer::resetLevel();
        const auto tEntry1 = std::chrono::steady_clock::now();
        const double entrySec =
            std::chrono::duration<double>(tEntry1 - tEntry0).count();
        skkbot2::log::info("[SKC] entry perf: vanilla resetLevel took {:.3f} s",
            entrySec);
        if (!stitch) {
            forceLayerSeeds();
            skkbot2::log::info(
                "[SKC] layer seeds forced: m_randomSeed/m_replayRandSeed = "
                "{:#x} after vanilla reset (skc_rng)",
                kSkcLayerSeed);
        }
        bot.reapplySessionAccuracy();
        bot.resumeCaptureAfterReset();
        sanitizeSeamQueueAfterReset(this, "resetLevel");
        if (injectedCheckpoint && m_checkpointArray &&
            m_checkpointArray->count() > 0 &&
            m_checkpointArray->lastObject() == injectedCheckpoint) {
            m_checkpointArray->removeLastObject();
        }
    }

    void fullReset() {
        auto& bot = Bot::get();
        skkbot2::log::debug(
            "[SKC] fullReset: clock->0, wiping {} snapshot(s), inputs={}, "
            "mode={}", PracticeFix::get().getSnapshotCount(),
            bot.getInputCount(), bot.isRecording() ? "RECORD"
                : (bot.isPlaying() ? "PLAY" : "idle"));
        FrameClock::get().reset();
        PracticeFix::get().clear();
        if (bot.isPlaybackArmed()) {
            bot.startPlayback();
        } else if (bot.isRecording()) {
            bot.onResetEvent(Bot::ResetReason::AttemptFromStart, 0);
        } else {
            bot.restart();
        }
        PlayLayer::fullReset();
        bot.reapplySessionAccuracy();
        bot.resumeCaptureAfterReset();
        sanitizeSeamQueueAfterReset(this, "fullReset");
    }

    void resetLevelFromStart() {
        auto& bot = Bot::get();
        FrameClock::get().reset();
        PracticeFix::get().clear();
        if (bot.isPlaybackArmed()) {
            bot.startPlayback();
        } else if (bot.isRecording()) {
            bot.onResetEvent(Bot::ResetReason::AttemptFromStart, 0);
        } else {
            bot.restart();
        }
        PlayLayer::resetLevelFromStart();
        bot.reapplySessionAccuracy();
        bot.resumeCaptureAfterReset();
        sanitizeSeamQueueAfterReset(this, "resetLevelFromStart");
    }

    void delayedResetLevel() {
        auto& bot = Bot::get();
        const bool wipe = !bot.isEnabled() || bot.isPlaybackArmed();
        skkbot2::log::debug(
            "[SKC] delayedResetLevel: inPractice={} tick={} snapshots={} "
            "inputs={} mode={} -> {}",
            m_isPracticeMode != 0, m_gameState.m_currentProgress,
            PracticeFix::get().getSnapshotCount(), bot.getInputCount(),
            bot.isRecording() ? "RECORD" : (bot.isPlaying() ? "PLAY" : "idle"),
            wipe ? "wipe clock+snapshots" : "KEEP clock+snapshots");
        if (wipe) {
            FrameClock::get().reset();
            PracticeFix::get().clear();
        }
        if (!bot.isEnabled() && !bot.isPlaybackArmed()) {
            bot.restart();
        }
        PlayLayer::delayedResetLevel();
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        PlayLayer::playEndAnimationToPos(position);

        auto& bot = Bot::get();
        if (!bot.isRecording() && !bot.isPlaying()) return;

        if (m_player1) m_player1->stopAllActions();
        if (m_player2) m_player2->stopAllActions();

        skkbot2::log::info("[SKC] end-anim canonized f={} mode={}",
            FrameClock::get().getFrame(),
            bot.isRecording() ? "RECORD" : "PLAY");
        levelComplete();
    }

    void levelComplete() {
        auto& bot = Bot::get();
        const bool wasPlaying = bot.isPlaying();
        const bool wasRecording = bot.isRecording();

        PlayLayer::levelComplete();

        if (wasPlaying || wasRecording) {
            bot.stop();
            if (wasRecording) {
                skkbot2::Logger::get().setSessionMode(
                    skkbot2::Logger::SessionMode::None);
            }
            skkbot2::log::info(
                "[SKC] level complete - session ended (mode={})",
                wasPlaying ? "PLAY" : "RECORD");
            if (wasPlaying) {
                skk::skc::journalPlaybackCounters("playback complete");
            }
        }
    }

    void onQuit() {
        auto& bot = Bot::get();
        if (bot.isEnabled() || bot.isPlaybackArmed()) {
            bot.onLevelExit();
        }
        skcResetPortalCache();
        PracticeFix::get().clear();
        FrameClock::get().reset();
        PlayLayer::onQuit();
    }


    CheckpointObject* markCheckpoint() {
        auto& bot = Bot::get();
        if (bot.isRecording() && m_player1 && m_player2 &&
            (m_player1->m_isDead || m_player2->m_isDead)) {
            return nullptr;
        }
        return PlayLayer::markCheckpoint();
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        auto& bot = Bot::get();
        const bool setupCheckpoint = m_isPlatformer &&
                                     m_activatedCheckpoint != nullptr;
        if (setupCheckpoint) {
            skkbot2::log::info(
                "[SKC] setup store BEFORE base: object={} activated={} "
                "arrayCount={} frame={} player=({:.6f},{:.6f}) levelTime={:.9f}",
                static_cast<void*>(checkpoint),
                static_cast<void*>(m_activatedCheckpoint),
                m_checkpointArray ? m_checkpointArray->count() : 0,
                FrameClock::get().getFrame(),
                m_player1 ? static_cast<double>(m_player1->getPositionX()) : 0.0,
                m_player1 ? static_cast<double>(m_player1->getPositionY()) : 0.0,
                static_cast<double>(m_gameState.m_levelTime));
        }
        PlayLayer::storeCheckpoint(checkpoint);

        if (checkpoint && setupCheckpoint &&
            (bot.isRecording() || bot.isPlaying())) {
            PracticeFix::get().trackPlatformerCheckpoint(checkpoint);
            skkbot2::log::info(
                "[SKC] setup store AFTER base: object={} activated={} "
                "arrayCount={} arrayTail={} frame={} cpTime={:.9f} "
                "cpProgress={} player=({:.6f},{:.6f}) trackedObjects={}",
                static_cast<void*>(checkpoint),
                static_cast<void*>(m_activatedCheckpoint),
                m_checkpointArray ? m_checkpointArray->count() : 0,
                m_checkpointArray
                    ? static_cast<void*>(m_checkpointArray->lastObject()) : nullptr,
                FrameClock::get().getFrame(),
                static_cast<double>(checkpoint->m_gameState.m_levelTime),
                checkpoint->m_gameState.m_currentProgress,
                m_player1 ? static_cast<double>(m_player1->getPositionX()) : 0.0,
                m_player1 ? static_cast<double>(m_player1->getPositionY()) : 0.0,
                PracticeFix::get().getPlatformerCheckpointCount());
        }

        if (bot.isRecording() && checkpoint && !bot.isDeathCapturePaused() &&
            (!m_player1 || !m_player1->m_isDead) &&
            (!m_player2 || !m_player2->m_isDead)) {
            const uint32_t frame = FrameClock::get().getFrame();
            PracticeFix::get().capture(frame, this, checkpoint);
            const CCPoint pos = m_player1 ? m_player1->getPosition() : CCPoint{0, 0};
            skkbot2::log::debug(
                "[SKC] checkpoint stored at frame {} (progress={}) pos=({:.3f},{:.3f}) "
                "rot={:.2f} - now {} snapshot(s)", frame,
                checkpoint->m_gameState.m_currentProgress / 2,
                static_cast<double>(pos.x), static_cast<double>(pos.y),
                m_player1 ? static_cast<double>(m_player1->getRotation()) : 0.0,
                PracticeFix::get().getSnapshotCount());
        }
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        auto& bot = Bot::get();

        const auto logLoad = [&](const char* phase) {
            skkbot2::log::info(
                "[SKC] platformer load {}: arg={} trackedLatest={} "
                "arrayCount={} arrayTail={} argTime={:.9f} argProgress={} "
                "player=({:.6f},{:.6f}) vy={:.9f} frame={} mode={}",
                phase, static_cast<void*>(checkpoint),
                static_cast<void*>(PracticeFix::get().latestPlatformerCheckpoint()),
                m_checkpointArray ? m_checkpointArray->count() : 0,
                m_checkpointArray
                    ? static_cast<void*>(m_checkpointArray->lastObject()) : nullptr,
                checkpoint ? static_cast<double>(
                    checkpoint->m_gameState.m_levelTime) : -1.0,
                checkpoint ? checkpoint->m_gameState.m_currentProgress : -1,
                m_player1 ? static_cast<double>(m_player1->getPositionX()) : 0.0,
                m_player1 ? static_cast<double>(m_player1->getPositionY()) : 0.0,
                m_player1 ? m_player1->m_yVelocity : 0.0,
                FrameClock::get().getFrame(),
                bot.isRecording() ? "RECORD" : bot.isPlaying() ? "PLAY" : "idle");
        };
        if (m_isPlatformer) logLoad("BEFORE base");

        PlayLayer::loadFromCheckpoint(checkpoint);
        if (m_isPlatformer) logLoad("AFTER base");

        if (bot.isEnabled()) {
            if (auto appliedFrame = PracticeFix::get().apply(this, checkpoint)) {
                const CCPoint rpos =
                    m_player1 ? m_player1->getPosition() : CCPoint{0, 0};
                skkbot2::log::debug(
                    "[SKC] checkpoint restored to frame {} pos=({:.3f},{:.3f}) "
                    "rot={:.2f} (recording={}, playing={})", *appliedFrame,
                    static_cast<double>(rpos.x), static_cast<double>(rpos.y),
                    m_player1 ? static_cast<double>(m_player1->getRotation()) : 0.0,
                    bot.isRecording(), bot.isPlaying());
                if (bot.isRecording()) {
                    bot.onResetEvent(Bot::ResetReason::CheckpointRestore,
                                     *appliedFrame);
                    FrameClock::get().setFrame(*appliedFrame);
                    bot.finishCaptureRestore(*appliedFrame);
                    bot.seekTo(*appliedFrame);
                } else {
                    bot.seekTo(*appliedFrame);
                }
            } else if (bot.isPlaying() && checkpoint) {
                skkbot2::log::info(
                    "[SKC] checkpoint restore during playback without a session "
                    "snapshot: no frame guessing - playback continues only from "
                    "the level start");
            }
        }

        sanitizeSeamQueueAfterReset(this, "loadFromCheckpoint");
    }


    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& bot = Bot::get();

        const bool playerOne = player == m_player1;

        const bool wasDead = player && player->m_isDead;
        PlayLayer::destroyPlayer(player, object);
        const bool diedNow = player && !wasDead && player->m_isDead;
        if (bot.isRecording() && playerOne && diedNow) {
            bot.noteAttemptReset(FrameClock::get().getFrame());
        } else if (bot.isPlaying() && playerOne && diedNow && bot.getInputIndex() > 0) {
            const auto pos = player ? player->getPosition() : CCPoint{0, 0};
            skkbot2::log::info(
                "[SKC] playback: player died on frame {} after consuming "
                "{}/{} inputs - pos=({:.3f},{:.3f}) levelTime={:.4f} "
                "onGround={} held={} g={:.1f} ud={} spd={:.2f} "
                "next=input#{}@frame{} - SKC has no death "
                "chain, the macro will replay from frame 0",
                FrameClock::get().getFrame(), bot.getInputIndex(),
                bot.getInputCount(), static_cast<double>(pos.x),
                static_cast<double>(pos.y),
                static_cast<double>(m_gameState.m_levelTime),
                (player && player->m_isOnGround) ? 1 : 0, heldMask(player),
                player ? player->m_gravity : 0.0,
                (player && player->m_isUpsideDown) ? 1 : 0,
                player ? static_cast<double>(player->m_playerSpeed) : 0.0,
                bot.getInputIndex(),
                bot.peekNextInputFrame());
            dumpDeathGeometry(this, pos, object);
            skk::skc::journalPlaybackCounters("playback death");
        }

    }


    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused);

        auto& bot = Bot::get();
        if (!bot.isRecording())
            return;

        const bool swap = GameManager::get()->getGameVariable("0010");
        const uint32_t frame = FrameClock::get().getFrame() + 1;
        for (int button = 1; button <= 3; ++button) {
            bot.recordInput(frame, static_cast<uint8_t>(button), swap, false);
        }
    }
};

class $modify(SkcBaseGameLayer, GJBaseGameLayer) {
    struct Fields {
        bool macroDispatch = false;
        float probePrevRotSpeed = 0.0f;
        float probePrevRot = 0.0f;
        bool probePrevRotValid = false;
        bool probePrevOnSlope = false;
        bool probePrevLocked = false;
        bool probePrevDash = false;
        uint32_t probeDigestIdx = 0;
        bool probePrevUpsideDown = false;
        bool probePrevContactP1 = false;
        bool probePrevContactP2 = false;
        int probeContactRunP1 = 0;
        int probeContactRunP2 = 0;
        uint32_t probePrevStep = 0;
        bool probePrevStepValid = false;
        bool portalPrevXValid = false;
        float portalPrevX = 0.0f;
        float portalPrevY = 0.0f;
        GameObject* portalTracked = nullptr;
        bool portalMovePrevValid = false;
        float portalMovePrevX = 0.0f;
        float portalMovePrevY = 0.0f;
        float portalMovePrevS = -1.0f;
        std::vector<GameObject*> portalInstances;
        bool portalInstancesInit = false;
        bool portalObbPrevValid = false;
        bool portalObbPrevRectOO = false;
        bool portalObbPrevObbOO = false;
    };

    void logContactProbe(
        skk::skc::Bot& bot, PlayerObject* p, int playerIdx, int runTicks,
        bool hasContact, bool wasContact) {
        (void)wasContact;
        skkbot2::log::info(
            "[SKC] contact-{} f={} rec={} play={} p={} run={:d} vy={:a} "
            "y={:a} x={:a} rot={:a} rotSpeed={:a} ground={} slope={} "
            "wasSlope={} slopeRot={:a} slopeAngle={:a} slopeVel={:a} "
            "curSlopeYVel={:a} cwId={} lastColBottom={:d} "
            "lastColTop={:d} locked={} dash={} gmod={:a} step={}",
            hasContact ? "attach" : "detach",
            FrameClock::get().getFrame(),
            bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
            playerIdx, runTicks,
            static_cast<double>(p->m_yVelocity),
            static_cast<double>(p->getPositionY()),
            static_cast<double>(p->getPositionX()),
            static_cast<double>(p->getRotation()),
            static_cast<double>(p->m_rotationSpeed),
            p->m_isOnGround ? 1 : 0,
            p->m_isOnSlope ? 1 : 0,
            p->m_wasOnSlope ? 1 : 0,
            static_cast<double>(p->m_slopeRotation),
            static_cast<double>(p->m_slopeAngle),
            static_cast<double>(p->m_slopeVelocity),
            static_cast<double>(p->m_currentSlopeYVelocity),
            p->m_collidingWithSlopeId,
            p->m_lastCollisionBottom,
            p->m_lastCollisionTop,
            p->m_isLocked ? 1 : 0,
            p->m_isDashing ? 1 : 0,
            static_cast<double>(p->m_gravityMod),
            m_currentStep);
    }

    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::processCommands", 100);
        (void)self.setHookPriority("GJBaseGameLayer::processQueuedButtons", 100);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", 100);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        const auto& skcBot = Bot::get();
        const bool skcSession =
            skcBot.isRecording() || skcBot.isPlaying();
        if (skcSession) {
            static bool sFlagFixLogged = false;
            if (!sFlagFixLogged) {
                sFlagFixLogged = true;
                skkbot2::log::info(
                    "[SKC] canonical half/last flags active: vanilla "
                    "was half={} last={} -> forced (false, true) on every "
                    "processCommands call (BUG-102 v3)",
                    isHalfTick, isLastTick);
            }
            isHalfTick = false;
            isLastTick = true;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        FrameClock::get().tick();
        skkbot2::skc::advanceTickDt(static_cast<double>(dt));
        FrameClock::get().noteCall(dt, isHalfTick);
        auto& bot = Bot::get();

        if (skcSession) {
            if (bot.isRecording()) {
                journalRecordTick(bot.consumeRngTypedInput());
            } else if (bot.isPlaying()) {
                journalPlaybackTick();
            }
        }

        if (bot.isPlaying() && m_player1) {
            skcPortalSweep(this, m_player1,
                           m_fields->portalPrevXValid, m_fields->portalPrevX,
                           m_fields->portalPrevY);
        }

        if ((bot.isRecording() || bot.isPlaying()) && m_player1) {
            const float rotSpeed = m_player1->m_rotationSpeed;
            if (rotSpeed != 0.0f &&
                m_fields->probePrevRotSpeed == 0.0f) {
                const CCPoint pos = m_player1->getPosition();
                skkbot2::log::info(
                    "[SKC] spin-start f={} rec={} play={} rot={:a} "
                    "rotSpeed={:a} slopeRot={:a} unkAngle1={:a} x={:a} y={:a} "
                    "vy={:a} ground={} slope={} wasSlope={} ud={} step={} "
                    "extraDelta={:a}",
                    FrameClock::get().getFrame(),
                    bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                    static_cast<double>(m_player1->getRotation()),
                    static_cast<double>(rotSpeed),
                    static_cast<double>(m_player1->m_slopeRotation),
                    static_cast<double>(m_player1->m_unkAngle1),
                    static_cast<double>(pos.x), static_cast<double>(pos.y),
                    static_cast<double>(m_player1->m_yVelocity),
                    m_player1->m_isOnGround ? 1 : 0,
                    m_player1->m_isOnSlope ? 1 : 0,
                    m_player1->m_wasOnSlope ? 1 : 0,
                    m_player1->m_isUpsideDown ? 1 : 0,
                    m_currentStep,
                    static_cast<double>(m_extraDelta));
            }
            m_fields->probePrevRotSpeed = rotSpeed;

            if (m_player1->m_isUpsideDown != m_fields->probePrevUpsideDown) {
                const CCPoint pos = m_player1->getPosition();
                const CCPoint physPos = m_player1->m_position;
                const CCPoint nodePos = m_player1->getPosition();
                const CCRect playerRect = m_player1->getObjectRect();
                skkbot2::log::info(
                    "[SKC] gravity-flip f={} rec={} play={} ud={} "
                    "rot={:a} vy={:a} x={:a} y={:a} totalTime={:a} "
                    "batch={} fdt={:.7f} between={} "
                    "nodeDX={:a} nodeDY={:a} maxPortalY={:a} "
                    "nx={:.3f} ny={:.3f} mx={:.3f} my={:.3f} "
                    "gx={:.3f} gy={:.3f} rminy={:.3f} rmaxy={:.3f}",
                    FrameClock::get().getFrame(),
                    bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                    m_player1->m_isUpsideDown ? 1 : 0,
                    static_cast<double>(m_player1->getRotation()),
                    static_cast<double>(m_player1->m_yVelocity),
                    static_cast<double>(pos.x), static_cast<double>(pos.y),
                    static_cast<double>(m_player1->m_totalTime),
                    skkbot2::skcprobe::g_batchStep,
                    skkbot2::skcprobe::g_frameDt,
                    m_isBetweenSteps ? 1 : 0,
                    static_cast<double>(pos.x - physPos.x),
                    static_cast<double>(pos.y - physPos.y),
                    static_cast<double>(m_gameState.m_unkUint17),
                    static_cast<double>(nodePos.x), static_cast<double>(nodePos.y),
                    static_cast<double>(physPos.x), static_cast<double>(physPos.y),
                    m_player1->m_positionX, m_player1->m_positionY,
                    static_cast<double>(playerRect.getMinY()),
                    static_cast<double>(playerRect.getMaxY()));
                m_fields->probePrevUpsideDown = m_player1->m_isUpsideDown;
            }
        }

        if ((bot.isRecording() || bot.isPlaying()) && m_player1) {
            const uint32_t probeFrame = FrameClock::get().getFrame();
            const float probeRot = m_player1->getRotation();
            const bool probeOnSlope = m_player1->m_isOnSlope;

            if (m_fields->probePrevRotValid) {
                const float drot = probeRot - m_fields->probePrevRot;
                if (drot > 45.0f || drot < -45.0f) {
                    skkbot2::log::info(
                        "[SKC] rot-jump f={} rec={} play={} rot={:a} "
                        "prevRot={:a} dRot={:a} rotSpeed={:a} slopeRot={:a} "
                        "slope={} wasSlope={} ud={} step={} "
                        "extraDelta={:a}",
                        probeFrame,
                        bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                        static_cast<double>(probeRot),
                        static_cast<double>(m_fields->probePrevRot),
                        static_cast<double>(drot),
                        static_cast<double>(m_player1->m_rotationSpeed),
                        static_cast<double>(m_player1->m_slopeRotation),
                        probeOnSlope ? 1 : 0,
                        m_player1->m_wasOnSlope ? 1 : 0,
                        m_player1->m_isUpsideDown ? 1 : 0,
                        m_currentStep,
                        static_cast<double>(m_extraDelta));
                }
            }
            m_fields->probePrevRot = probeRot;
            m_fields->probePrevRotValid = true;

            if (probeOnSlope != m_fields->probePrevOnSlope) {
                skkbot2::log::info(
                    "[SKC] slope-{} f={} rec={} play={} rot={:a} rotSpeed={:a} "
                    "slopeRot={:a} slopeAngle={:a} slopeVel={:a} cwId={} "
                    "ground={} ud={} y={:a} vy={:a} totalTime={:a}",
                    probeOnSlope ? "attach" : "detach", probeFrame,
                    bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                    static_cast<double>(probeRot),
                    static_cast<double>(m_player1->m_rotationSpeed),
                    static_cast<double>(m_player1->m_slopeRotation),
                    static_cast<double>(m_player1->m_slopeAngle),
                    static_cast<double>(m_player1->m_slopeVelocity),
                    m_player1->m_collidingWithSlopeId,
                    m_player1->m_isOnGround ? 1 : 0,
                    m_player1->m_isUpsideDown ? 1 : 0,
                    static_cast<double>(m_player1->getPositionY()),
                    static_cast<double>(m_player1->m_yVelocity),
                    static_cast<double>(m_player1->m_totalTime));
                m_fields->probePrevOnSlope = probeOnSlope;
            }

            const bool probeLocked = m_player1->m_isLocked;
            const bool probeDash = m_player1->m_isDashing;            if (probeLocked != m_fields->probePrevLocked ||
                probeDash != m_fields->probePrevDash) {
                skkbot2::log::info(
                    "[SKC] state-toggle f={} rec={} play={} rot={:a} "
                    "locked={} dash={} rotSpeed={:a} slopeRot={:a}",
                    probeFrame,
                    bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                    static_cast<double>(probeRot),
                    probeLocked ? 1 : 0, probeDash ? 1 : 0,
                    static_cast<double>(m_player1->m_rotationSpeed),
                    static_cast<double>(m_player1->m_slopeRotation));
                m_fields->probePrevLocked = probeLocked;
                m_fields->probePrevDash = probeDash;
            }

            auto contactProbe = [&](PlayerObject* p, bool& prevContact,
                                    int& run) {
                if (!p) return;
                const bool contact = p->m_isOnGround || p->m_isOnSlope;
                if (contact != prevContact) {
                    logContactProbe(
                        Bot::get(), p, p == m_player1 ? 1 : 2, run,
                        contact, prevContact);
                    run = 0;
                } else {
                    ++run;
                }
                prevContact = contact;
            };
            contactProbe(m_player1, m_fields->probePrevContactP1,
                         m_fields->probeContactRunP1);
            contactProbe(m_player2, m_fields->probePrevContactP2,
                         m_fields->probeContactRunP2);

            const uint32_t prevStep = m_fields->probePrevStep;
            m_fields->probePrevStep = m_currentStep;
            if (m_fields->probePrevStepValid) {
                const int dStep = int(m_currentStep) - int(prevStep);
                if (dStep > 1) {
                    skkbot2::log::info(
                        "[SKC] step-jump f={} rec={} play={} "
                        "prevStep={} step={} dStep={:d} extraDelta={:a}",
                        probeFrame,
                        bot.isRecording() ? 1 : 0,
                        bot.isPlaying() ? 1 : 0,
                        prevStep, m_currentStep, dStep,
                        static_cast<double>(m_extraDelta));
                }
            }
            m_fields->probePrevStepValid = true;

            if (probeFrame / 100 != m_fields->probeDigestIdx) {
                m_fields->probeDigestIdx = probeFrame / 100;
                skkbot2::log::debug(
                    "[SKC] digest f={} rec={} play={} rot={:a} rotSpeed={:a} "
                    "slopeRot={:a} slopeAngle={:a} slopeVel={:a} "
                    "curSlopeYVel={:a} cwId={} onSlope={} wasSlope={} "
                    "locked={} dash={} rotating={} ballRot2={} "
                    "jumpBuf={} lastCB={} ud={} step={}",
                    probeFrame,
                    bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                    static_cast<double>(probeRot),
                    static_cast<double>(m_player1->m_rotationSpeed),
                    static_cast<double>(m_player1->m_slopeRotation),
                    static_cast<double>(m_player1->m_slopeAngle),
                    static_cast<double>(m_player1->m_slopeVelocity),
                    static_cast<double>(m_player1->m_currentSlopeYVelocity),
                    m_player1->m_collidingWithSlopeId,
                    probeOnSlope ? 1 : 0,
                    m_player1->m_wasOnSlope ? 1 : 0,
                    m_player1->m_isLocked ? 1 : 0,
                    probeDash ? 1 : 0,
                    m_player1->m_isRotating ? 1 : 0,
                    m_player1->m_isBallRotating2 ? 1 : 0,
                    m_player1->m_jumpBuffered ? 1 : 0,
                    m_player1->m_lastCollisionBottom,
                    m_player1->m_isUpsideDown ? 1 : 0,
                    m_currentStep);
            }
        }

        if (skkbot2::step_trace::enabled()) {
            skkbot2::step_trace::checkDrift(
                this, static_cast<int>(FrameClock::get().getFrame()),
                bot.isRecording(), bot.isPlaying(), true);
            skkbot2::step_trace::recordSkc(
                this, static_cast<int>(FrameClock::get().getFrame()), 1,
                bot.isRecording(), bot.isPlaying());
        }
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        const auto& bot = Bot::get();
        if ((bot.isRecording() || bot.isPlaying()) && object && player) {
            const auto type = object->m_objectType;
            if (type == GameObjectType::InverseGravityPortal ||
                type == GameObjectType::NormalGravityPortal ||
                type == GameObjectType::GravityTogglePortal) {
                const CCPoint nodePos = player->getPosition();
                const CCPoint physPos = player->m_position;
                const CCRect pr = player->getObjectRect();
                const CCRect orct = object->getObjectRect();
                if (object->m_objectID == 2926) {
                    skkbot2::skcprobe::g_probePortal = object;
                    skkbot2::skcprobe::g_probePortalId = 2926;
                }
                skkbot2::log::info(
                    "[SKC] portal-see f={} rec={} play={} type={} obj={} "
                    "batch={} between={} "
                    "nx={:.7f} ny={:.7f} mx={:.7f} my={:.7f} "
                    "prY={:.7f}..{:.7f} prX={:.7f}..{:.7f} "
                    "orY={:.7f}..{:.7f} orX={:.7f}..{:.7f}",
                    FrameClock::get().getFrame(),
                    bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                    type == GameObjectType::InverseGravityPortal ? 1
                        : (type == GameObjectType::NormalGravityPortal ? 0 : 2),
                    object->m_objectID,
                    skkbot2::skcprobe::g_batchStep,
                    m_isBetweenSteps ? 1 : 0,
                    static_cast<double>(nodePos.x),
                    static_cast<double>(nodePos.y),
                    static_cast<double>(physPos.x),
                    static_cast<double>(physPos.y),
                    static_cast<double>(pr.getMinY()),
                    static_cast<double>(pr.getMaxY()),
                    static_cast<double>(pr.getMinX()),
                    static_cast<double>(pr.getMaxX()),
                    static_cast<double>(orct.getMinY()),
                    static_cast<double>(orct.getMaxY()),
                    static_cast<double>(orct.getMinX()),
                    static_cast<double>(orct.getMaxX()));
            }
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        const auto& bot = Bot::get();
        if ((bot.isRecording() || bot.isPlaying()) && player) {
            auto* portal = player->m_lastActivatedPortal;
            const CCPoint nodePos = player->getPosition();
            const CCPoint physPos = player->m_position;
            const CCRect pr = player->getObjectRect();
            CCRect orct{};
            bool hasPortal = portal != nullptr;
            if (hasPortal) orct = portal->getObjectRect();
            skkbot2::log::info(
                "[SKC] flip-apply f={} rec={} play={} flip={} noFx={} "
                "udBefore={} vy={:a} batch={} between={} "
                "nx={:.7f} ny={:.7f} mx={:.7f} my={:.7f} "
                "prY={:.7f}..{:.7f} portal={} "
                "orY={:.7f}..{:.7f} orX={:.7f}..{:.7f}",
                FrameClock::get().getFrame(),
                bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                flip ? 1 : 0, noEffects ? 1 : 0,
                player->m_isUpsideDown ? 1 : 0,
                static_cast<double>(player->m_yVelocity),
                skkbot2::skcprobe::g_batchStep,
                m_isBetweenSteps ? 1 : 0,
                static_cast<double>(nodePos.x),
                static_cast<double>(nodePos.y),
                static_cast<double>(physPos.x),
                static_cast<double>(physPos.y),
                static_cast<double>(pr.getMinY()),
                static_cast<double>(pr.getMaxY()),
                hasPortal ? portal->m_objectID : -1,
                static_cast<double>(orct.getMinY()),
                static_cast<double>(orct.getMaxY()),
                static_cast<double>(orct.getMinX()),
                static_cast<double>(orct.getMaxX()));
        }
        g_inGjblFlip = true;
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
        g_inGjblFlip = false;
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        auto& bot = Bot::get();
        const bool playing = bot.isPlaying();

        if ((playing || bot.isRecording()) && m_player1) {
            const uint32_t f = FrameClock::get().getFrame();
            const bool dense = f >= kDenseFrom && f <= kDenseTo;
            const bool grid = playing && f <= kStateTraceFrames && f % 10 == 0;
            if (playing && (dense || grid)) {
                const auto pos = m_player1->getPosition();
                skkbot2::log::info(
                    "[SKC] state{} f={} x={:.3f} y={:.3f} rot={:.2f} vy={:.4f} "
                    "t={:.4f} ground={} buf={} dead={} held={} idx={} "
                    "g={:.1f} ud={} spd={:.2f}",
                    dense ? "" : "*", f, static_cast<double>(pos.x),
                    static_cast<double>(pos.y),
                    static_cast<double>(m_player1->getRotation()),
                    m_player1->m_yVelocity,
                    static_cast<double>(m_gameState.m_levelTime),
                    m_player1->m_isOnGround ? 1 : 0,
                    m_player1->m_jumpBuffered ? 1 : 0,
                    m_player1->m_isDead ? 1 : 0, heldMask(m_player1),
                    bot.getInputIndex(), m_player1->m_gravity,
                    m_player1->m_isUpsideDown ? 1 : 0,
                    static_cast<double>(m_player1->m_playerSpeed));
            }
        }

        if (playing) {
            if (bot.getIgnoreInputs()) {
                m_queuedButtons.clear();
            }

            const uint32_t frame = FrameClock::get().getFrame();
            uint32_t dispatched = 0;
            while (auto opt = bot.poll(frame)) {
                PlayerButtonCommand command = {};
                command.m_button = static_cast<PlayerButton>(opt->button);
                command.m_isPush = opt->pressed;
                command.m_isPlayer2 = opt->player2;
                command.m_step = 0;
                command.m_timestamp = 0.0;
                m_queuedButtons.push_back(command);
                ++dispatched;

                if (bot.getInputIndex() <= kDispatchLogInputs) {
                    const auto pos =
                        m_player1 ? m_player1->getPosition() : CCPoint{0, 0};
                    skkbot2::log::info(
                        "[SKC] dispatch f={} btn={} down={} p2={} idx={} "
                        "x={:.3f} y={:.3f} ground={} held={}",
                        frame, opt->button, opt->pressed ? 1 : 0,
                        opt->player2 ? 1 : 0, bot.getInputIndex(),
                        static_cast<double>(pos.x), static_cast<double>(pos.y),
                        (m_player1 && m_player1->m_isOnGround) ? 1 : 0,
                        heldMask(m_player1));
                }
            }
            (void)dispatched;
        }

        if ((playing || bot.isRecording()) && m_player1 &&
            FrameClock::get().getFrame() % (kProbeInterval * 10) == 0) {
            const auto pos = m_player1->getPosition();
            skkbot2::log::info(
                "[SKC] probe{} f={} x={:.3f} y={:.3f} rot={:.2f} spd={:.4f} vx={:.4f} "
                "ud={} sfg={} upy={:.3f} maxPY={:.3f} minPY={:.3f}",
                playing ? "P" : "R", FrameClock::get().getFrame(),
                static_cast<double>(pos.x), static_cast<double>(pos.y),
                static_cast<double>(m_player1->getRotation()),
                static_cast<double>(m_player1->m_playerSpeed),
                static_cast<double>(m_player1->m_xVelocityRelated),
                m_player1->m_isUpsideDown ? 1 : 0,
                m_player1->m_stateFlipGravity,
                static_cast<double>(m_gameState.m_unkUint17),
                static_cast<double>(getMaxPortalY()),
                static_cast<double>(getMinPortalY()));
        }

        m_fields->macroDispatch = playing;

        if ((playing || bot.isRecording()) && m_player1) {
            if (!m_fields->portalInstancesInit) {
                m_fields->portalInstancesInit = true;
                if (m_objects) {
                    for (int i = 0; i < m_objects->count(); ++i) {
                        auto* obj = static_cast<GameObject*>(m_objects->objectAtIndex(i));
                        if (obj && obj->m_objectID == 2926)
                            m_fields->portalInstances.push_back(obj);
                    }
                }
                skkbot2::log::info("[SKC] portal-2926 tracked instances: n={}",
                                   m_fields->portalInstances.size());
            }
            const float px = m_player1->getPositionX();
            auto* tracked = m_fields->portalTracked;
            if (!tracked || std::fabs(tracked->getPositionX() - px) > 800.0f) {
                GameObject* best = nullptr;
                float bestDist = 1e9f;
                for (auto* inst : m_fields->portalInstances) {
                    if (!inst) continue;
                    const float d = std::fabs(inst->getPositionX() - px);
                    if (d < bestDist) { bestDist = d; best = inst; }
                }
                if (best != tracked) {
                    m_fields->portalTracked = best;
                    m_fields->portalMovePrevValid = false;
                    if (best) {
                        skkbot2::log::info(
                            "[SKC] portal-track f={} rec={} play={} switch to "
                            "ptr={:p} at ({:.7f},{:.7f}) s={:.7f} r={:.4f} dist={:.1f}",
                            FrameClock::get().getFrame(),
                            bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                            static_cast<void*>(best),
                            static_cast<double>(best->getPositionX()),
                            static_cast<double>(best->getPositionY()),
                            static_cast<double>(best->getScale()),
                            static_cast<double>(best->getRotation()),
                            static_cast<double>(bestDist));
                    }
                }
            }
            tracked = m_fields->portalTracked;
            if (tracked) {
                const float cx = tracked->getPositionX();
                const float cy = tracked->getPositionY();
                const float cs = tracked->getScale();
                if (!m_fields->portalMovePrevValid ||
                    std::fabs(cx - m_fields->portalMovePrevX) > 0.01f ||
                    std::fabs(cy - m_fields->portalMovePrevY) > 0.01f ||
                    std::fabs(cs - m_fields->portalMovePrevS) > 0.0005f) {
                    skkbot2::log::info(
                        "[SKC] portal-move f={} rec={} play={} batch={} "
                        "({:.7f},{:.7f}) s={:.7f} -> ({:.7f},{:.7f}) s={:.7f} r={:.4f}",
                        FrameClock::get().getFrame(),
                        bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                        skkbot2::skcprobe::g_batchStep,
                        m_fields->portalMovePrevValid
                            ? static_cast<double>(m_fields->portalMovePrevX) : -1.0,
                        m_fields->portalMovePrevValid
                            ? static_cast<double>(m_fields->portalMovePrevY) : -1.0,
                        m_fields->portalMovePrevS,
                        static_cast<double>(cx), static_cast<double>(cy),
                        static_cast<double>(cs),
                        static_cast<double>(tracked->getRotation()));
                    m_fields->portalMovePrevX = cx;
                    m_fields->portalMovePrevY = cy;
                    m_fields->portalMovePrevS = cs;
                    m_fields->portalMovePrevValid = true;
                }
            }

            GameObject* nearPortal = nullptr;
            float nearDist = 1e9f;
            for (auto* inst : m_fields->portalInstances) {
                if (!inst) continue;
                const float d = std::fabs(inst->getPositionX() - px);
                if (d < nearDist) { nearDist = d; nearPortal = inst; }
            }
            if (nearPortal && nearDist < 400.0f) {
                const CCRect pr = m_player1->getObjectRect();
                const CCRect orct = nearPortal->getObjectRect();
                const bool rectOO = pr.intersectsRect(orct);
                bool obbA = false;
                bool obbB = false;
                double slackA0 = 0.0, slackA1 = 0.0, slackB0 = 0.0, slackB1 = 0.0;
                auto* obox = nearPortal->m_orientedBox;
                auto* pbox = m_player1->m_orientedBox;
                if (obox && pbox) {
                    auto axisPass = [](OBB2D* self, OBB2D* other,
                                       double& s0, double& s1) {
                        bool pass = true;
                        double worst = 1e30;
                        for (int axis = 0; axis < 2; ++axis) {
                            const float ex = self->m_edges[axis].x;
                            const float ey = self->m_edges[axis].y;
                            const double origin = self->m_projections[axis];
                            float low = 0.0f, high = 0.0f;
                            for (int i = 0; i < 4; ++i) {
                                const float v = other->m_corners[i].y * ey +
                                                other->m_corners[i].x * ex;
                                if (i == 0) { low = high = v; }
                                else if (v < low) low = v;
                                else if (v > high) high = v;
                            }
                            const double sLow = origin + 1.0 - low;
                            const double sHigh = high - origin;
                            if (axis == 0) { s0 = std::min(sLow, sHigh); }
                            else { s1 = std::min(sLow, sHigh); }
                            worst = std::min(worst, std::min(sLow, sHigh));
                            if (sLow < 0.0 || sHigh < 0.0) pass = false;
                        }
                        return pass ? worst : -std::fabs(worst);
                    };
                    obbA = axisPass(obox, pbox, slackA0, slackA1) >= 0.0;
                    obbB = axisPass(pbox, obox, slackB0, slackB1) >= 0.0;
                }
                const bool obbOO = obbA && obbB;
                if (!m_fields->portalObbPrevValid ||
                    rectOO != m_fields->portalObbPrevRectOO ||
                    obbOO != m_fields->portalObbPrevObbOO) {
                    skkbot2::log::info(
                        "[SKC] portal-obb f={} rec={} play={} batch={} "
                        "obj={:p} dist={:.1f} rectOO={} obbOO={} "
                        "obbA={} obbB={} outer={} radius={:.7f} "
                        "slackA=({:.7f},{:.7f}) slackB=({:.7f},{:.7f}) "
                        "prY={:.7f}..{:.7f} orY={:.7f}..{:.7f} "
                        "protRot={:.4f} playerRot={:.4f}",
                        FrameClock::get().getFrame(),
                        bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                        skkbot2::skcprobe::g_batchStep,
                        static_cast<void*>(nearPortal),
                        static_cast<double>(nearDist),
                        rectOO ? 1 : 0, obbOO ? 1 : 0,
                        obbA ? 1 : 0, obbB ? 1 : 0,
                        nearPortal->m_shouldUseOuterOb ? 1 : 0,
                        static_cast<double>(nearPortal->m_objectRadius),
                        slackA0, slackA1, slackB0, slackB1,
                        static_cast<double>(pr.getMinY()),
                        static_cast<double>(pr.getMaxY()),
                        static_cast<double>(orct.getMinY()),
                        static_cast<double>(orct.getMaxY()),
                        static_cast<double>(nearPortal->getRotation()),
                        static_cast<double>(m_player1->getRotation()));
                    m_fields->portalObbPrevValid = true;
                    m_fields->portalObbPrevRectOO = rectOO;
                    m_fields->portalObbPrevObbOO = obbOO;
                }
            }
        }

        if (m_player1) {
            if (m_player1->m_isDead) {
                m_fields->portalPrevXValid = false;
            } else {
                const CCPoint pos = m_player1->getPosition();
                m_fields->portalPrevXValid = true;
                m_fields->portalPrevX = pos.x;
                m_fields->portalPrevY = pos.y;
            }
        } else {
            m_fields->portalPrevXValid = false;
        }

        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        m_fields->macroDispatch = false;

        if (!playing)
            return;

        if (bot.isPlaybackExhausted() && m_player1 && !m_player1->m_isDead) {
            bot.onPlaybackExhausted();
        } else if (bot.isPlaybackExhausted()) {
            skkbot2::log::info("[SKC] playback: macro fully dispatched at frame {} "
                               "(waited for the player to respawn)",
                               FrameClock::get().getFrame());
        }
    }

    void handleButton(bool down, int button, bool player2) {
        auto& bot = Bot::get();
        const bool fromMacro = m_fields->macroDispatch;

        if (!fromMacro && bot.isPlaying()) {
            const auto pos = m_player1 ? m_player1->getPosition() : CCPoint{0, 0};
            skkbot2::log::warn(
                "[SKC] NON-MACRO click during playback: f={} down={} btn={} "
                "p2={} ignoreInputs={} x={:.3f} y={:.3f} queue={}",
                FrameClock::get().getFrame(), down ? 1 : 0, button,
                player2 ? 1 : 0, bot.getIgnoreInputs() ? 1 : 0,
                static_cast<double>(pos.x), static_cast<double>(pos.y),
                m_queuedButtons.size());
        }

        if (!fromMacro && bot.isPlaying() && bot.getIgnoreInputs())
            return;

const bool keepSecond = m_levelSettings && m_levelSettings->m_twoPlayerMode;
        const bool recordPlayer2 = keepSecond && !player2;


        GJBaseGameLayer::handleButton(down, button, player2);

        if (!fromMacro && bot.isRecording()) {
            if (recordPlayer2) {
                skkbot2::log::warn(
                    "[SKC] record P2 press: frame={} down={} btn={} rawPlayer2={} "
                    "twoPlayerMode={} isDualMode={} keepSecond={}",
                    FrameClock::get().getFrame(), down, button, player2,
                    m_levelSettings ? m_levelSettings->m_twoPlayerMode : false,
                    m_gameState.m_isDualMode, keepSecond);
            }
            bot.recordInput(
                FrameClock::get().getFrame(),
                static_cast<uint8_t>(button),
                recordPlayer2,
                down
            );
        }
    }

    int checkCollisions(PlayerObject* object, float dt, bool ignoreDamage) {

        return GJBaseGameLayer::checkCollisions(object, dt, ignoreDamage);
    }
};

class $modify(SkcPlayerObject, PlayerObject) {
    void setPosition(cocos2d::CCPoint const& pos) {
        if (skk::isPlayerArtDeferred()) {
            m_isObjectRectDirty = true;
            m_isOrientedBoxDirty = true;
            GameObject::setPosition(pos);
            if (m_waveTrail) {
                if (m_fadeOutStreak && m_isDart) {
                    m_waveTrail->m_currentPoint = getPosition();
                }
                m_waveTrail->setPosition(getPosition());
            }
            return;
        }
        PlayerObject::setPosition(pos);
    }

    void flipGravity(bool flip, bool noEffects) {
        const auto& bot = Bot::get();
        if (bot.isRecording() || bot.isPlaying()) {
            const CCPoint nodePos = getPosition();
            const CCPoint physPos = m_position;
            skkbot2::log::info(
                "[SKC] po-flip f={} rec={} play={} flip={} noFx={} via={} "
                "udBefore={} sfg={} shh={} vy={:a} batch={} between={} "
                "nx={:.7f} ny={:.7f} mx={:.7f} my={:.7f}",
                skk::skc::FrameClock::get().getFrame(),
                bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                flip ? 1 : 0, noEffects ? 1 : 0,
                g_inGjblFlip ? "gjbl" : "other",
                m_isUpsideDown ? 1 : 0,
                m_stateFlipGravity, m_stateHitHead,
                static_cast<double>(m_yVelocity),
                skkbot2::skcprobe::g_batchStep,
                m_gameLayer ? (m_gameLayer->m_isBetweenSteps ? 1 : 0) : -1,
                static_cast<double>(nodePos.x),
                static_cast<double>(nodePos.y),
                static_cast<double>(physPos.x),
                static_cast<double>(physPos.y));
        }
        PlayerObject::flipGravity(flip, noEffects);
    }

    void didHitHead() {
        const auto& bot = Bot::get();
        if (bot.isRecording() || bot.isPlaying()) {
            const CCPoint nodePos = getPosition();
            const CCPoint physPos = m_position;
            const CCRect pr = getObjectRect();
            skkbot2::log::info(
                "[SKC] head-hit f={} rec={} play={} sfg={} shh={} ud={} "
                "vy={:a} batch={} between={} "
                "nx={:.7f} ny={:.7f} mx={:.7f} my={:.7f} "
                "prY={:.7f}..{:.7f} ground={} ground2={} boosted={}",
                skk::skc::FrameClock::get().getFrame(),
                bot.isRecording() ? 1 : 0, bot.isPlaying() ? 1 : 0,
                m_stateFlipGravity, m_stateHitHead,
                m_isUpsideDown ? 1 : 0,
                static_cast<double>(m_yVelocity),
                skkbot2::skcprobe::g_batchStep,
                m_gameLayer ? (m_gameLayer->m_isBetweenSteps ? 1 : 0) : -1,
                static_cast<double>(nodePos.x),
                static_cast<double>(nodePos.y),
                static_cast<double>(physPos.x),
                static_cast<double>(physPos.y),
                static_cast<double>(pr.getMinY()),
                static_cast<double>(pr.getMaxY()),
                m_isOnGround ? 1 : 0,
                m_isOnGround2 ? 1 : 0,
                m_maybeIsBoosted ? 1 : 0);
        }
        PlayerObject::didHitHead();
    }
};

class $modify(SkcEditorUI, EditorUI) {
    void onPlaytest(CCObject* sender) {
        auto& bot = Bot::get();
        if (auto* editor = LevelEditorLayer::get()) {
            if (editor->m_playbackMode == PlaybackMode::Not) {
                if (bot.isPlaybackArmed()) {
                    bot.startPlayback();
                }
                bot.restart();
                FrameClock::get().reset();
                editor->m_gameState.m_currentProgress = 0;
            }
        }
        EditorUI::onPlaytest(sender);
    }
};


}
