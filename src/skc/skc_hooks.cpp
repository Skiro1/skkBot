#include "skc_batch_probe.hpp"
#include <chrono>
#include "skc_bot.hpp"
#include "skc_checkpoint.hpp"
#include "skc_frame.hpp"
#include "skc_rng.hpp"
#include "skc_tick_clock.hpp"
#include "core/step_trace.hpp"
#include "core/checkpoint_creation.hpp"
#include "core/seam_phase_probe.hpp"
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
// Drift-probe grid, in frames. Coarse enough to keep the log readable, fine
// enough to name the exact frame where a divergence starts.
constexpr uint32_t kProbeInterval = 240;

// Playback diagnostics for an imported macro that dies early. The failure we
// are chasing (Windy Road.slc: death on frame 367 after only two dispatched
// inputs) happens inside an input-free window of the recording, so the only way
// to see what the player did is a per-frame state line. The window covers the
// first 601 frames (2.5 s at 240 tps) on a 1-in-10 grid, and every single frame
// from 190 to 460 - the stretch around the authored jump at 208 and the death.
constexpr uint32_t kStateTraceFrames = 600;
constexpr uint32_t kDenseFrom = 190;
constexpr uint32_t kDenseTo = 460;
// Every dispatched click is logged until the cursor has moved this far into the
// macro; past a couple of hundred the log stops being evidence and starts being
// noise, and the dispatch path is identical for the rest of the file.
constexpr uint32_t kDispatchLogInputs = 400;

// "100" = jump held, "010" = left, "001" = right.
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

// "<objectID>@(<x>,<y>)" - enough to name a spike (8), a small spike (39), a
// saw (88/89/98) or a portal without dumping every field.
namespace {
// BUG-102 v3: метка канала флипа. GJBaseGameLayer::flipGravity ставит перед
// вызовом члена PlayerObject::flipGravity; хук члена печатает канал:
// gjbl = портал/тоггл через GJBaseGameLayer, other = прямой вызов
// (hardFlipGravity inline / dual-sync / прочее).
thread_local bool g_inGjblFlip = false;
} // namespace

std::string objectTag(GameObject* object) {
    if (!object) return "<none>";
    const auto pos = object->getPosition();
    return std::to_string(object->m_objectID) + "@(" +
           std::to_string(static_cast<int>(pos.x)) + "," +
           std::to_string(static_cast<int>(pos.y)) + ")";
}

// ===========================================================================
// BUG-102: детерминированный флип гравитации для SKC-плейбэка.
// 'Flamewall 2': в записи флип срабатывает на f=1159-1163, а в плейбэке
// ванильная overlap-проверка портала не стреляет вовсе (ud=0 в момент
// смерти, детерминированно, дважды подряд). Причина: внутришаговая OBB-
// проверка срабатывает в том подшаге, где ориентированный бокс уже/ещё
// не накрывает портал — раскладка подшагов зависит от dt (Real Time),
// поэтому точка срабатывания «плавает» и промах возможен.
// SKC не реставрирует физику per-frame (только клики), поэтому промах
// не чинится пост-фактум РЕСТАВРОЙ состояния — только инженерным свипом:
// реплицируем ванильную клаузу грав-портала
// (GJBaseGameLayer::collisionCheckObjects, клаузы InverseGravityPortal /
// NormalGravityPortal / GravityTogglePortal) в семантике GucciBot
// collisions.cpp: rect/circle pre-test + two-way OBB SAT для
// m_shouldUseOuterOb + гейт once-per-contact. Свип живёт между
// подшагами (это вызов processCommands = один шаг физики), поэтому
// точка срабатывания - ФРОНТ контакта, а не вход в прямоугольник.
// Ванильный check либо уже сработал внутри того же шага (тогда
// hasBeenActivatedByPlayer == true и мы пропускаем), либо не сработал
// вовсе (тогда это единственный флип). activatedByPlayer ставит
// ванильный флаг «уже активирован» — двойного флипа не будет и на
// последующих шагах, пока игрок ещё в прямоугольнике.
// ===========================================================================
struct SkcPortalItem {
    GameObject* obj = nullptr;
    CCRect rect{};
    bool inverse = false; // InverseGravityPortal -> ud=true, Normal -> false
    bool toggle = false;  // GravityTogglePortal -> flip = !m_isUpsideDown
    float radius = 0.0f;  // m_objectRadius: круговая коллизия (GucciBot 439-443)
    bool outerOb = false; // m_shouldUseOuterOb: two-way OBB SAT (GucciBot 478-488)
};

// Состояние контакта портала за попытку (аналог canBeActivatedByPlayer /
// once-per-contact из GucciBot world::canActivate): гейт активации
// срабатывает только на фронте контакта, не каждый шаг внутри контакта.
struct SkcPortalRun {
    int lastContactFrame = -100000;
    int activated = 0;    // только НАШИ (play-side) применения флипа
};

static CCArray* g_skcPortalArray = nullptr;
static size_t g_skcPortalCount = 0;
static std::vector<SkcPortalItem> g_skcPortals;
static std::vector<SkcPortalRun> g_skcPortalRuns;
static uint32_t g_skcPortalForceCount = 0;
static int g_skcSweepLastFrame = 0;
// Переиспользуемый игрок-OBB для two-way SAT на интерполированных
// подшагах: игрок живёт каждый кадр в новой точке, живой-box ваниллы
// нельзя использовать для выборки между prev и cur. Мутирует только
// наш temp-объект, не живое состояние игрока.
static OBB2D* g_skcTmpPlayerObb = nullptr;

static void skcResetPortalCache() {
    // BUG-109: кэш держится по идентичности CCArray + count. После выхода из
    // уровня массив уничтожается, а новая куча может выделить новый CCArray
    // по ТОМУ ЖЕ адресу - проверка "тот же массив" ложно сработает и кэш
    // отдаст висячие указатели на объекты прошлого уровня. Сбрасываем
    // принудительно при выходе из уровня (SkcPlayLayer::onQuit).
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

// Контакт изображает совмещённый тест ваниллы (collisionCheckObjects
// до клаузных гейтов): rect/circle pre-test + two-way OBB SAT для
// m_shouldUseOuterOb. samplePos - интерполированная позиция подшага
// между prev и cur; outerOb-тест переносит живой player OBB в samplePos
// через временный OBB2D (габариты игрока не меняются между подшагами).
static bool skcPortalContact(GJBaseGameLayer* layer, PlayerObject* player,
                             const SkcPortalItem& item,
                             const CCPoint& samplePos,
                             const CCRect& livePlayerRect) {
    const bool circle = item.radius > 0.0f;
    CCRect pre;
    if (circle) {
        // Круговая коллизия: bbox круга вокруг центра портала; для
        // interior-подшагов точный playerCircleCollision недоступен
        // (он читает живую позицию), поэтому консервативный bbox.
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
        // GucciBot collisions.cpp:478-488 / ванилла 0x214bb0-0x214bbf:
        // односторонний SAT пропускает частичные перекрытия, проверяем
        // ОБА набора осей.
        OBB2D* box = item.obj->getOrientedBox();
        player->updateOrientedBox();
        // Габариты живого box восстанавливаем от его углов к центру
        // samplePos: ширина/высота и поворот - из GameObject-полей игрока.
        const float w = player->m_width * player->m_spriteWidthScale *
            player->m_scaleX;
        const float h = player->m_height * player->m_spriteHeightScale *
            player->m_scaleY;
        const float rot = -player->getRotation();
        const CCPoint center(
            samplePos.x + player->getBoxOffset().x,
            samplePos.y + player->getBoxOffset().y);
        if (!g_skcTmpPlayerObb) {
            // OBB2D::create(center, width, height, rotationAngle) - углы в
            // радианах (ваниль передаёт deg * 0.017453f).
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

// Свип v2 (BUG-102, GucciBot-семантика): окно [prev..cur] разбивается
// на S интерполированных подшагов; контакт портала тестируется так же,
// как ванильный collisionCheckObjects (rect/circle + two-way OBB SAT).
// Гейт once-per-contact: флип применяется на ФРОНТЕ контакта (первый
// кадр окна, где появился контакт, при контакте после паузы) и только
// если ваниль на этом же шаге свой флип уже НЕ прижала
// (hasBeenActivatedByPlayer) или не прижала журнал нашей попытки.
// Так точка срабатывания ложится в полосу допуска записи (±2 подшага),
// а не на вход в прямоугольник, как это делал свип v1.
static void skcPortalSweep(GJBaseGameLayer* layer, PlayerObject* player,
                           bool prevXValid, float prevX, float prevY) {
    if (!layer || !player || player->m_isDead) return;
    skcRebuildPortalCache(layer);
    if (g_skcPortals.empty()) return;

    const int frame = FrameClock::get().getFrame();
    if (frame < g_skcSweepLastFrame) {
        // Новый аттемпт (frame ушёл назад): контакт/активации сброс.
        skcClearPortalRuns();
    }
    g_skcSweepLastFrame = frame;

    const CCPoint pos = player->getPosition();
    const CCRect livePlayerRect = player->getObjectRect();
    CCPoint prevPos(prevX, prevY);
    if (!prevXValid) {
        // Нет предыдущего шага: проверяем только текущую позицию
        // (эквивалент window нулевой ширины).
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
        // грубый reject по X окна (все типы порталов имеют rect)
        if (r.getMaxX() < wMinX || r.getMinX() > wMaxX) continue;
        // контакт ставится по фактической, не по грубой
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
        // ФРОНТ контакта: первый шаг, в котором игрок оказался внутри.
        if (item.obj->hasBeenActivatedByPlayer(player)) {
            // ваниль на этом же шаге флипнула сама - cвип не нужен
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


// What killed the player, plus the objects around it. "died on the ground at
// x=476" does not say whether that was a spike on the floor, a saw, or the
// side of a block; the recording survived the same frame, so the geometry at
// the death point is what separates "wrong trajectory" from "wrong level".
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

// Every reset seam runs the same queue sanitation. GD leaves the dead attempt's
// presses and releases in m_queuedButtons and drains them on the first step
// after the respawn, so they land on the NEW attempt: without this a phantom
// press is recorded into the stitched suffix, and a stale release can cancel a
// hold the macro is about to resume. Policy (three Xenon rules in play mode,
// whole-queue drop in record mode) lives in Bot::sanitizeSeamQueue.
void sanitizeSeamQueueAfterReset(GJBaseGameLayer* layer, const char* site) {
    auto& bot = Bot::get();
    if (!layer) return;
    if (!bot.isRecording() && !bot.isPlaying()) return;
    const auto mode = bot.isRecording() ? Bot::SeamQueueMode::Record
                                        : Bot::SeamQueueMode::Play;
    bot.sanitizeSeamQueue(layer->m_queuedButtons,
                          FrameClock::get().getFrame(), mode, site);
}
} // namespace

// ---------------------------------------------------------------------------
// PlayLayer — attempt lifecycle.
//
// resetLevel covers both a death respawn and a manual restart; GD calls it
// either way, and `m_checkpointArray->count()` distinguishes them. A full
// restart rewinds the clock and (when not playing back) clears the macro; a
// respawn from a checkpoint keeps the recorded past and only rewinds the
// cursor.
// ---------------------------------------------------------------------------
class $modify(SkcPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        auto& bot = Bot::get();
        // Новые объекты уровня не наследуют диагностические окна старого.
        skkbot2::seam_probe::skcCenters.clear();
        skkbot2::seam_probe::skkCenters.clear();
        // Новый PlayLayer — новое время жизни объектов, даже при обходе onQuit.
        // Сброс до queued RECORD/PLAY и до ванильного init/reset.
        skcResetPortalCache();
        PracticeFix::get().clear();
        FrameClock::get().reset();

        // A session bound to a different level must not survive into this one
        // (same rule as the SKK PlayLayer::init hook's sameLevel check).
        bot.onLevelEnter(level);

        // A queued RECORD/PLAY from outside the level fires now, exactly like
        // ReplayEngine::consumePendingAction in the SKK PlayLayer::init hook.
        // init runs before the level's first physics step, so the clock starts
        // at 0 and the macro plays from the very beginning.
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

        // Which level is actually open. The PLAY path that runs from
        // PlayLayer::init cannot see the layer (it does not exist yet), so this
        // is the first point where the live level is readable. An imported
        // macro carries no level identity, and a macro replayed in the wrong
        // level dies exactly like a broken replay.
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

        // Match GucciBot's platformer checkpoint path: GD's setup-checkpoint
        // may leave a synthetic object in the array. Put the exact object
        // associated with m_activatedCheckpoint last so vanilla reset restores
        // that live checkpoint, then remove only our temporary array entry.
        CheckpointObject* injectedCheckpoint = nullptr;
        if (m_isPlatformer && m_checkpointArray && m_checkpointArray->count() == 0 &&
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

        // GD re-reads these flags during reset; apply before reset so the game
        // sees the macro's mode, and again afterward in case reset copied stale
        // values into the live layer.
        if (bot.ownsTiming()) {
            bot.reapplySessionAccuracy();
        }

        // SKK's wipe rule is `!checkpointMode || tick <= 1 ||
        // restorePoints.empty()` (checkpoint_system.cpp:1622-1647). SKC used to
        // key off "no checkpoint array" alone, which missed the `tick <= 1`
        // half.
        //
        // checkpointMode is NOT the same as practice mode. A platformer level
        // places checkpoints through the editor "setup checkpoint" object even
        // with m_isPracticeMode == false; GD respawns from the last
        // CheckpointObject on death, so wiping the timeline here would drop the
        // checkpoint the player actually respawned at. Classic normal mode
        // still wipes (GD restarts from the top there).
        const int tick = m_gameState.m_currentProgress;
        const bool inPractice = m_isPracticeMode != 0;
        const bool platformer = m_isPlatformer;
        const bool checkpointMode = inPractice || platformer;
        const bool hasSnapshots = fix.getSnapshotCount() > 0;
        // GD's resetLevel() restores the last CheckpointObject iff
        // m_checkpointArray is non-empty. In platformer that is the
        // authoritative sign of a checkpoint respawn: m_currentProgress is an
        // x coordinate there, and the editor "setup checkpoint" can sit at
        // x == 0, so neither `tick > 1` nor a frame-clock guess is reliable.
        // Classic keeps the progress guard (a normal classic run has no
        // checkpoint array and a real practice respawn is at progress > 1).
        const bool checkpointPresent =
            m_checkpointArray && m_checkpointArray->count() > 0;
        const bool progressed = platformer ? checkpointPresent : (tick > 1);
        // A checkpoint respawn exists whenever GD will restore a checkpoint.
        // Stitching is RECORD-ONLY (пользователь 10.11): SKC-плейбэк больше не
        // пытается "предугадать" фрейм чекпоинта по levelTime - макрос
        // воспроизводится только с начала уровня, попытка без снапшотов
        // означает рестарт с кадра 0.
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

            // An armed PLAY takes over on the next attempt without a teleport:
            // the level just restarted, so start the macro from frame 0.
            if (bot.isPlaybackArmed()) {
                bot.startPlayback();
            }
            // Deletion policy lives in ONE place now. An idle or playing bot
            // still must not lose a macro it merely loaded - onResetEvent
            // decides that, and the reason says why.
            if (bot.isRecording()) {
                bot.onResetEvent(Bot::ResetReason::AttemptFromStart, 0);
            } else {
                bot.restart();
            }
        } else {
            // Respawn from a checkpoint (practice mode OR a platformer auto
            // checkpoint): the clock rewinds to the frame the checkpoint was
            // captured at and the cursor follows.
            if (bot.isPlaybackArmed()) {
                bot.startPlayback();
            }
            auto* target = m_checkpointArray
                ? static_cast<CheckpointObject*>(m_checkpointArray->lastObject()) : nullptr;
            const auto targetFrame = fix.frameFor(target);
            skkbot2::log::info(
                "[SKC] checkpoint respawn ({}{}) -> frame {} (checkpoints held: "
                "{}, recorded inputs: {}, mode: {})",
                platformer ? "platformer" : "practice",
                hasSnapshots ? "" : ", no snapshot",
                targetFrame.value_or(0), fix.getSnapshotCount(), bot.getInputCount(),
                bot.isRecording() ? "RECORD" :
                (bot.isPlaying() ? "PLAY" : "idle"));
            // BUG-142: никаких предварительных trim/seek по back() снимков.
            // Реальный аргумент loadFromCheckpoint выбирает GD после удаления
            // последнего CP; только его снимок задаёт границу восстановления.
        }

        // BUG-107: пин primary/teleport RNG на фиксированный сид перед
        // ванильным телом resetLevel, только для attempt-from-start (wipe)
        // в SKC-сессии. Ванильный resetLevel прогоняет ВСЕ объекты через
        // GameObject::resetObject, который на каждом вызове шагает
        // глобальный LCG (seed = 214013*old + 2531011) и присваивает
        // m_varianceIndex - фазу idle-анимаций объекта (пульс/блинк/подлёт
        // порталов). История draws до f=0 в record- и play-мирах разная
        // (вход в уровень, переходы, UI), поэтому фазы анимаций стартуют
        // в разных фазах: пульс 300 vs 444, стартовая позиция портала
        // 2492 vs 2495, latched m_shouldUseOuterOb 0 vs 1 - и контакт с
        // порталом квантуется ±1 тик даже при пине display rate. Один и
        // тот же сид перед циклом resetObject делает назначение variance
        // бит-в-бит в обоих мирах. На stitch (practice respawn) пина НЕТ:
        // loadFromCheckpoint не ресторит RNG, и вмешательство сломало бы
        // подтверждённую бит-в-бит паритетность пост-стич флипов.
        if ((bot.isRecording() || bot.isPlaying()) && !stitch) {
            // Один сид на пин и сиды слоёв: kSkcLayerSeed из skc_rng.hpp
            // (forceLayerSeeds после ванили ставит слои той же константой).
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

        // Entry perf (BUG-127): measure the vanilla body — the user reports a
        // ~11 s level entry. The log decides whether the wait is inside
        // PlayLayer::resetLevel (vanilla level parse/init) or elsewhere.
        const auto tEntry0 = std::chrono::steady_clock::now();
        PlayLayer::resetLevel();
        const auto tEntry1 = std::chrono::steady_clock::now();
        const double entrySec =
            std::chrono::duration<double>(tEntry1 - tEntry0).count();
        skkbot2::log::info("[SKC] entry perf: vanilla resetLevel took {:.3f} s",
            entrySec);
        // skc_rng: сиды слоёв (m_randomSeed/m_replayRandSeed) в константу
        // обоих миров сразу после ванили — SKK-порт forceLayerSeeds без
        // activeSk. Только attempt-from-start: у стича ваниль вообще не
        // перекатывает попытку, и вмешательство сломало бы пост-стич паритет.
        if (!stitch && (bot.isRecording() || bot.isPlaying())) {
            forceLayerSeeds();
            skkbot2::log::info(
                "[SKC] layer seeds forced: m_randomSeed/m_replayRandSeed = "
                "{:#x} after vanilla reset (skc_rng)",
                kSkcLayerSeed);
        }
        bot.reapplySessionAccuracy();
        // Keep the death gate armed throughout GD's reset cleanup. onResetEvent
        // runs before the base reset in this hook; clearing it there let
        // reset-time release callbacks leak the death tail back into the macro.
        bot.resumeCaptureAfterReset();
        // The dead attempt's input queue survives the reset and would be
        // drained on the first step of the new one - drop it now, while the
        // clock already names the resume frame and the cursor is aligned.
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

    // A platformer FULL RESTART (the restart button / a new attempt from the
    // top) comes through here instead of resetLevel; death still goes through
    // resetLevel -> loadFromCheckpoint (the auto-checkpoint). Our own GUI calls
    // this directly for platformer playback - without a hook the frame clock and
    // the cursor would survive into the new attempt.
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
        // SKK deliberately does NOT touch the recorded stream here
        // (checkpoint_system.cpp:2007-2014) - delayedResetLevel is only a
        // simulation-timing reset.
        //
        // SKC used to zero the FrameClock and wipe every practice snapshot
        // here. In practice mode GD fires this MID-RUN, so a live recording
        // suddenly had clock=0 while its action stream still held events at
        // frame ~445. Every subsequent input was then rejected as
        // out-of-order, and the next checkpoint snapshot came from a clock
        // that no longer matched the level - the macro ended up a shredded
        // mix of two timelines. Neither the clock nor the snapshots may move
        // unless the same SKK wipe rule (resetLevel) says so.
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

    // BUG-135: ванильный финиш - wall-clock анимация. playEndAnimationToPos
    // вешает на ноду игрока CCSequence(CCEaseIn(CCBezierTo(1.0s),1.2),
    // CCCallFunc(levelComplete)) и CCEaseIn(CCRotateBy(360,1.0s),1.5)
    // (2.2091 sub_14050A2A0). Они тикают от scheduler dt дисплея, поэтому
    // record и playback миры расходятся на фазе джиттера: первое отличие
    // трейса на следующем кадре после лока (1 ULP y), к концу 1-секундного
    // полёта - до 0.6px x / 0.34 rot. Геймплей и входы бит-точны.
    // Канонизация: после ванильного тела гасим wall-clock действия вместе
    // с запланированным колбэком (CCSequence живёт на ноде игрока - в 2.2091
    // runAction(v13,...), v13 = m_player1) и завершаем уровень немедленно
    // через наш levelComplete-хук. Входы давно закончились, полёт - только
    // косметика; цена - пропущенный 1-секундный подлёт в SKC-сессиях.
    // Ванильные/SKK-прохождения не трогаем.
    void playEndAnimationToPos(cocos2d::CCPoint position) {
        PlayLayer::playEndAnimationToPos(position);

        finishSkcEndAnimation("classic");
    }

    // End-trigger использует отдельный Bezier-путь даже в classic-уровне.
    // GD 2.2091: playPlatformerEndAnimationToPos, CCSequence на P1,
    // тот же levelComplete callback; при instant=true он уже вызван ванилью.
    void playPlatformerEndAnimationToPos(cocos2d::CCPoint position, bool instant) {
        PlayLayer::playPlatformerEndAnimationToPos(position, instant);
        finishSkcEndAnimation(instant ? "trigger-instant" : "trigger-bezier");
    }

    void finishSkcEndAnimation(char const* path) {
        auto& bot = Bot::get();
        if (!bot.isRecording() && !bot.isPlaying()) return;

        // Колбэк levelComplete сидит внутри CCSequence на ноде игрока
        // (только P1), rotate - на P1/P2: stopAllActions убивает оба вида,
        // поэтому наш levelComplete() ниже - единственный вызов.
        if (m_player1) m_player1->stopAllActions();
        if (m_player2) m_player2->stopAllActions();

        skkbot2::log::info("[SKC] end-anim canonized f={} mode={} path={}",
            FrameClock::get().getFrame(),
            bot.isRecording() ? "RECORD" : "PLAY", path);
        levelComplete();
    }

    // Completion ends the session in BOTH modes, same as SKK's levelComplete:
    // a finished RECORD must not stay armed past the win screen (the next
    // retry's restart-clear would silently wipe the finished timeline), and a
    // finished PLAY must hand control back.
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
        // BUG-109: кэш порталов держит сырые указатели на объекты уровня -
        // сбрасываем до того, как уровень уничтожится.
        skcResetPortalCache();
        PracticeFix::get().clear();
        FrameClock::get().reset();
        PlayLayer::onQuit();
    }

    // --- practice ---------------------------------------------------------

    CheckpointObject* markCheckpoint() {
        auto& bot = Bot::get();
        // A checkpoint placed on a corpse captures a dead player; restoring to
        // it later respawns straight into the death animation. Skip it, same as
        // EclipseMenu and silicate do.
        if (bot.isRecording() && m_player1 && m_player2 &&
            (m_player1->m_isDead || m_player2->m_isDead)) {
            return nullptr;
        }
        return PlayLayer::markCheckpoint();
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        auto& bot = Bot::get();
        // GD clears m_activatedCheckpoint while storing the checkpoint on this
        // path. Snapshot the association before calling the base implementation
        // (GucciBot does the same check before its custom store path).
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

        // BUG-141: store регистрирует снимок create; pending CP может быть
        // старше live-состояния на сотни тиков. Не смешивать два времени.
        // BUG-156: pending CP может попасть в массив уже после смерти.
        // Пригодность проверена в create; состояние тела при store неважно.
        if (bot.isRecording() && checkpoint) {
            auto& created = static_cast<SkkCheckpointCreation*>(checkpoint)->m_fields->skc;
            if (!created || created->generation != PracticeFix::get().generation()) {
                skkbot2::log::warn(
                    "[SKC] checkpoint STORE: object={} нет снимка текущей записи; "
                    "поздний live-capture запрещён", static_cast<void*>(checkpoint));
                return;
            }
            const uint32_t frame = created->frame;
            const auto captureX = created->state.fullP1.p_x;
            const auto captureY = created->state.fullP1.p_y;
            const auto captureRot = created->state.fullP1.p_rotation;
            PracticeFix::get().capture(frame, this, checkpoint, std::move(created->state));
            created.reset();
            skkbot2::log::info(
                "[SKC] checkpoint STORE: object={} createFrame={} storeFrame={} "
                "cpTime={:.9f} liveTime={:.9f} captureX={:.6f} liveX={:.6f} "
                "deathPaused={} deadP1={} deadP2={}",
                static_cast<void*>(checkpoint), frame, FrameClock::get().getFrame(),
                checkpoint->m_gameState.m_levelTime, m_gameState.m_levelTime,
                captureX, m_player1 ? m_player1->getPositionX() : 0.f,
                bot.isDeathCapturePaused(), m_player1 && m_player1->m_isDead,
                m_player2 && m_player2->m_isDead);
            skkbot2::log::debug(
                "[SKC] checkpoint stored at frame {} (progress={}) pos=({:.3f},{:.3f}) "
                "rot={:.2f} - now {} snapshot(s)", frame,
                checkpoint->m_gameState.m_currentProgress / 2,
                static_cast<double>(captureX), static_cast<double>(captureY),
                static_cast<double>(captureRot),
                PracticeFix::get().getSnapshotCount());
        }
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        const auto probeFrame = PracticeFix::get().frameFor(checkpoint);
        if (probeFrame)
            skkbot2::seam_probe::arm(static_cast<int>(*probeFrame));
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
        skkbot2::seam_probe::log("skc-after-vanilla-load", m_player1, 0.f,
            probeFrame ? static_cast<int>(*probeFrame) : -1);
        if (m_isPlatformer) logLoad("AFTER base");
        // BUG-123 (12.11): armCheckpointMusicResync снят вместе с фиксом.

        // Re-pin what vanilla restore dropped. Must run AFTER the original so
        // we overwrite, not get overwritten. apply() reports the FrameClock
        // frame of the matched snapshot, which is also the correct trim bound:
        // the future the player just undid must not stay in the macro, and
        // loading an OLD checkpoint has to cut back to that checkpoint, not to
        // the most recent one.
        if (bot.isEnabled()) {
            if (auto appliedFrame = PracticeFix::get().apply(this, checkpoint)) {
                skkbot2::seam_probe::log("skc-after-bridge", m_player1, 0.f,
                    static_cast<int>(*appliedFrame));
                const CCPoint rpos =
                    m_player1 ? m_player1->getPosition() : CCPoint{0, 0};
                skkbot2::log::debug(
                    "[SKC] checkpoint restored to frame {} pos=({:.3f},{:.3f}) "
                    "rot={:.2f} (recording={}, playing={})", *appliedFrame,
                    static_cast<double>(rpos.x), static_cast<double>(rpos.y),
                    m_player1 ? static_cast<double>(m_player1->getRotation()) : 0.0,
                    bot.isRecording(), bot.isPlaying());
                // Stitch at the matched snapshot's frame - NOT
                // lastCheckpointFrame(): loading an OLD checkpoint must cut
                // back to THAT checkpoint, not to the most recent one.
                //
                // On a practice DEATH this also fires right after the
                // resetLevel branch did the same thing; the second pass finds
                // nothing left to cut, which is why the "dropped N" count is
                // logged by whichever ran first.
                if (bot.isRecording()) {
                    bot.onResetEvent(Bot::ResetReason::CheckpointRestore,
                                     *appliedFrame);
                    // Vanilla restore is exact, so the retry continues on the
                    // same frame grid - no correction. See onResetEvent.
                    FrameClock::get().setFrame(*appliedFrame);
                    // GD flushes buffered presses and stale releases right
                    // after the restore; keep them out of the macro stream.
                    bot.finishCaptureRestore(*appliedFrame);
                    skkbot2::seam_probe::log("skc-after-reconcile", m_player1);
                    bot.seekTo(*appliedFrame);
                } else {
                    bot.seekTo(*appliedFrame);
                }
            } else if (bot.isRecording() && checkpoint) {
                skkbot2::log::warn(
                    "[SKC] checkpoint RESTORE MISSING: object={} frame={} "
                    "cpTime={:.9f} cpProgress={} snapshots={} arrayCount={} "
                    "(trim/rewind не выполнены)",
                    static_cast<void*>(checkpoint), FrameClock::get().getFrame(),
                    checkpoint->m_gameState.m_levelTime,
                    checkpoint->m_gameState.m_currentProgress,
                    PracticeFix::get().getSnapshotCount(),
                    m_checkpointArray ? m_checkpointArray->count() : 0);
            } else if (bot.isPlaying() && checkpoint) {
                // Пользователь 10.11: никакого предугадывания фрейма для
                // SKC-плейбэка. Макрос воспроизводится только с начала
                // уровня; восстановление из чекпоинта без снапшота просто
                // оставляет курсор на месте (рестарт попытки перезапустит
                // макрос с кадра 0).
                skkbot2::log::info(
                    "[SKC] checkpoint restore during playback without a session "
                    "snapshot: no frame guessing - playback continues only from "
                    "the level start");
            }
        }

        // A checkpoint can also be restored without resetLevel (our own GUI
        // checkpoint buttons). The cursor and the clock are aligned by now, so
        // the same seam sanitation applies.
        sanitizeSeamQueueAfterReset(this, "loadFromCheckpoint");
    }

    // --- death ------------------------------------------------------------

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& bot = Bot::get();

        // SKK uses this hook for its persistence death-chain, which SKC does not
        // have (the format stores no physics). SKC only needs to explain the
        // restart: without a chain the macro simply replays from frame 0 on the
        // next attempt, so a silent death looks like "the bot forgot to click".
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

    // --- pause ------------------------------------------------------------

    void pauseGame(bool unfocused) {
        const bool wasPaused = m_isPaused;
        PlayLayer::pauseGame(unfocused);

        auto& bot = Bot::get();
        if (!bot.isRecording() || wasPaused || !m_isPaused)
            return;

        // GD 2.2091 pauseGame отпускает ОБОИХ игроков напрямую, минуя
        // handleButton. Здесь уже слоты макроса, не raw-флаг/настройка swap:
        // no-transition гейт оставит только реально открытые удержания.
        const uint32_t frame = FrameClock::get().getFrame() + 1;
        const size_t before = bot.getInputCount();
        for (int slot = 0; slot < 2; ++slot) {
            for (int button = 1; button <= 3; ++button) {
                bot.recordInput(frame, static_cast<uint8_t>(button), slot == 1, false);
            }
        }
        skkbot2::log::info("[SKC] пауза: frame={} записано релизов={} (оба слота)",
            frame, bot.getInputCount() - before);
    }
};

// ---------------------------------------------------------------------------
// GJBaseGameLayer — the clock, the recording tap and the playback inject.
// ---------------------------------------------------------------------------
class $modify(SkcBaseGameLayer, GJBaseGameLayer) {
    struct Fields {
        // True while vanilla processQueuedButtons is draining OUR commands, so
        // the handleButton gate lets the macro's own clicks through instead of
        // swallowing them as if they were human input.
        bool macroDispatch = false;
        // Spin-start / gravity-flip probe state (seam-divergence diagnostics:
        // a wrong air-spin base shows up later as a quantized portal event
        // firing one physics step early).
        float probePrevRotSpeed = 0.0f;
        float probePrevRot = 0.0f;
        bool probePrevRotValid = false;
        bool probePrevOnSlope = false;
        bool probePrevLocked = false;
        bool probePrevDash = false;
        uint32_t probeDigestIdx = 0;
        bool probePrevUpsideDown = false;
        // BUG-92 probe: contact-decision surveillance. The 4th round showed a
        // +0.223 vy impulse at f=21307 ONLY in the playback world while the
        // recorded world (attempt A) stayed bit-equal through f=21306 - a
        // one-tick-off CONTACT decision near y~530 with NO slope-attach /
        // state-toggle / gravity-flip firing. This probe watches the
        // contact state per player and dumps the full hidden contact family
        // plus m_currentStep when the contact surface changes.
        bool probePrevContactP1 = false;
        bool probePrevContactP2 = false;
        int probeContactRunP1 = 0;
        int probeContactRunP2 = 0;
        uint32_t probePrevStep = 0;
        bool probePrevStepValid = false;
        // BUG-102 gravity-portal sweep: prev-подшаг X игрока (для свипа
        // [prevX..curX]) и журнал строгих применений флипа за попытку.
        bool portalPrevXValid = false;
        float portalPrevX = 0.0f;
        float portalPrevY = 0.0f;
        // BUG-102 v3: трекинг анимации движущегося портала 2926. Сессии
        // 14:19/14:35/14:46: rect портала на одном тике РАЗНЫЙ в мирах
        // (rec летит к цели, play уже осел + пульс скейла) - флип квантуется
        // наездом rect движущегося портала. Переход-детектор называет тики
        // старта/конца анимации в каждом мире без привязки к окнам.
        GameObject* portalTracked = nullptr;
        bool portalMovePrevValid = false;
        float portalMovePrevX = 0.0f;
        float portalMovePrevY = 0.0f;
        float portalMovePrevS = -1.0f;
        // BUG-109: инстансы портала 2926 собираются НА УРОВЕНЬ (раньше жили в
        // function-local static и переживали выход из уровня - висячие
        // указатели на уничтоженные объекты; в краш-сессии 15:40 в векторе
        // оказался нулевой элемент, чтение по 0xD8).
        std::vector<GameObject*> portalInstances;
        bool portalInstancesInit = false;
        // BUG-107: SAT-зонд статичного портала - предыдущее состояние
        // (rectOverlap, obbOverlap) для переход-детектора.
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

    // processCommands is called exactly once per physics step, which makes it
    // the cheapest reliable tick source. The SKK step clock uses the same
    // signal; using it here keeps both bots on the same grid.
    //
    // It also drives the clock the playback cursor is compared against:
    // vanilla calls processQueuedButtons from INSIDE this function (GD 2.2081
    // RVA 0x239C60 -> 0x239D51), so dispatching there happens at exactly the
    // same point inside the step where a human press is applied.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        // BUG-102 v3 ROOT CAUSE (window-probe verdict, 04.11 session
        // 11:48/11:53): the deterministic dt grid is bit-identical between
        // record and playback (probe: dt/lt/ltTicks/step all equal), the
        // movers channel is empty (n=0 both worlds), and the ONLY differing
        // channel was the half/last grouping phase. GD derives isLastTick
        // from its per-display-frame step batching, which rides on wall
        // clock: the record world happened to run one substep per display
        // frame (last=true on EVERY call), the playback world ran
        // 3+3+2 substeps across ~90fps display frames (last on a repeating
        // 3+3+2 schedule). Portal contact quantizes against the last-phase,
        // so the same portal flipped at rec f=417 vs play f=421 - a 1..4
        // frame deterministic-but-wall-clock-dependent shift, worse with
        // more portals until the playback died at f=9952.
        //
        // Fix, SKC-side only: canonicalize the flags for SKC sessions -
        // half=false, last=true on every step - exactly the grouping the
        // record world is observed to run under. Both worlds then execute
        // the identical physics semantics regardless of display cadence.
        // No SKC format change; SKK sessions (SKC State::Disabled) keep
        // the vanilla GD flags untouched.
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
        // BUG-90: cumulative physics time for the dash-spin rotation clock
        // (skc_dash_determinism.cpp). Advanced after the vanilla body, so a
        // mid-body read is the total through the previous step.
        skkbot2::skc::advanceTickDt(static_cast<double>(dt));
        // BUG-70 probe: this call IS one physics iteration; record how many
        // iterations were half vs full and how much dt they carried in sum.
        FrameClock::get().noteCall(dt, isHalfTick);
        auto& bot = Bot::get();

        // skc_rng: якорь журнала RNG. Точка захвата и точка релока — одна и
        // та же (сразу после тела шага), так что плейбэк восстанавливает
        // записанные состояния ровно там же, где запись их сняла. Всё, что
        // GD нарисует из этих цепочек внутри СЛЕДУЮЩЕГО шага, совпадёт.
        if (skcSession) {
            if (bot.isRecording()) {
                journalRecordTick(bot.consumeRngTypedInput());
            } else if (bot.isPlaying()) {
                journalPlaybackTick();
            }
        }

        // BUG-102 gravity-portal sweep: детерминированный флип, после
        // ванильного тела шага и только на SKC playback — recording
        // сам флип не прикладывает. Свип живёт от prevX до текущего X
        // и срабатывает ТОЛЬКО если ваниль портал в этом шаге не
        // активировала (gate hasBeenActivatedByPlayer).
        if (bot.isPlaying() && m_player1) {
            skcPortalSweep(this, m_player1,
                           m_fields->portalPrevXValid, m_fields->portalPrevX,
                           m_fields->portalPrevY);
        }

        // Seam-divergence probe (BUG-87): spin-start and gravity-flip events of
        // a live session, full float precision. A spin whose base rot differs
        // between the recording world and the playback world fires quantized
        // portal events one physics step apart (The Yandere 2 Furnace 2177:
        // vy -15.000 -> -7.500 one step early -> death). The log lines pair up
        // 1:1 between record and play, so the recording of them names the wrong
        // field with the seam-bridge log.
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
                // BUG-102 v3: батч-контекст решения о портале. nodeDX/nodeDY -
                // разница позиции ноды (её читают коллизии) и физической
                // m_position; если нода отстаёт внутри батча, контакт портала
                // квантуется границей кадра дисплея.
                const CCPoint physPos = m_player1->m_position;
                const CCPoint nodePos = m_player1->getPosition();
                const CCRect playerRect = m_player1->getObjectRect();
                // Щедро (BUG-102 v3): каждый флип несёт ВСЕ источники позиции
                // - канал квантования виден в самой строке, окно не нужно.
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

        // BUG-90 probes: the third trace round shows a CONSTANT rotation-base
        // offset (-4.2524 deg) born at frame 2103 exactly at a lattice-style
        // jump, inside a continuous non-seam stretch, invisible in x/y/vy for
        // 723 frames. So the event-level and periodic digests below name the
        // first diverging hidden slope/rotation field directly: every line
        // pairs 1:1 between the record world (rec=1) and the playback world
        // (play=1), and the first pair whose payloads differ IS the bug.
        if ((bot.isRecording() || bot.isPlaying()) && m_player1) {
            const uint32_t probeFrame = FrameClock::get().getFrame();
            const float probeRot = m_player1->getRotation();
            const bool probeOnSlope = m_player1->m_isOnSlope;

            // (1) giant per-tick rotation jump (lattice / portal-set / unlock):
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

            // (2) slope attach / detach events:
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

            // (3) lock / dash toggles:
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

            // (3b) BUG-92 contact surveillance: the one-tick-off vy
            // impulse at f=21307 carried NO probe event at all (no
            // slope-attach, no state-toggle, no gravity-flip), so the
            // contact-decision codepath is silent. Watch both players'
            // ground/slope contact state and dump the full hidden
            // contact family whenever it changes - the payload pairs
            // 1:1 between the record and the playback worlds, and the
            // first diverging pair names the wrong hidden field. Also
            // log m_currentStep jumps per frame so the 34-step deviation
            // at f=21300 (rec 42552 vs play 42518, identical physics)
            // becomes visible tick by tick.
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

            // (4) periodic hidden-state digest (debug): the first diverging
            // line between the two worlds names the wrong field.
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

    // Playback goes through the game's OWN input queue, exactly like
    // silicate does (silicate-main/src/hooks/GJBaseGameLayer.cpp:535-540:
    // poll -> queueButton -> THEN the base processQueuedButtons drains it).
    //
    // This is the single biggest accuracy difference between SKC and the
    // reference bots. A human press travels
    // UILayer -> m_queuedButtons -> processQueuedButtons -> handleButton.
    // Calling handleButton directly - which is what SKC used to do - skips the
    // queue, so anything that INSPECTS m_queuedButtons instead of the hold map
    // (dash-ring activation, orbs reacting to queued input) behaves differently
    // during playback than it did during recording, and the run drifts.
    //
    // silicate is input-only too and pins no physics: this canonical path IS
    // its accuracy mechanism.
    // BUG-102 v3: инструмент РЕШЕНИЯ о портале. canBeActivatedByPlayer
    // вызывается ванилью ТОЛЬКО когда rect-куллинг уже пропустил объект,
    // значит строка здесь = "ванилья увидела overlap грав-портала в этом
    // подшаге". Парность строк rec/play по тикам называет, какой источник
    // позиции двигает решение: если в play строка появляется на 5 тиков
    // позже при бит-идентичной ноде - ванилья читает не ноду.
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

    // BUG-102 v3: контекст применённого флипа - какой портал, чей rect,
    // все источники позиции игрока в момент решения.
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

        // Diagnostic: what the player is doing on the frames the macro is NOT
        // clicking. An input-only replay that dies where the recording survived
        // can only be explained by state (on ground / airborne / holding), so
        // print it per frame in the window that matters. Runs before dispatch
        // and before vanilla physics, so every line is "state entering step f".
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
            // Human input must not interleave with the macro. Silicate does the
            // same (GJBaseGameLayer.cpp:498-500) before pushing its own. Skipped
            // when the user turned "Ignore Inputs" off - that toggle exists to
            // let them interleave deliberately.
            if (bot.getIgnoreInputs()) {
                m_queuedButtons.clear();
            }

            const uint32_t frame = FrameClock::get().getFrame();
            uint32_t dispatched = 0;
            while (auto opt = bot.poll(frame)) {
                // No duplicate suppression here. Silicate (the reference this
                // path was taken from) queues EVERY matching action, duplicate
                // presses included; collapsing them changed what GD's queue sees
                // on the very frames the file marks as repeated presses. The
                // queue is the game's own contract - hand it the file verbatim.
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

        // Drift probe. An input-only replay is exact only if the physics ends
        // up in the same state every run; this prints the live position on a
        // fixed frame grid so attempts can be diffed. It runs during RECORDING
        // too - comparing a recording's probe line against the playback of the
        // same frame is the only direct test of "does playback match what was
        // recorded", and playback-vs-playback only ever proves determinism.
        //
        // 10 frames, not 120. At the coarser grid the residual y difference
        // (1-4 px with dx exactly 0, so timing was already perfect) first shows
        // up somewhere between two probes and the causing frame is
        // unrecoverable. Velocity is included for the same reason: y alone does
        // not say whether a divergence is still growing or has settled.
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

        // Always drain vanilla's queue, playing or not - this is the game's own
        // input pump and skipping it would kill all input during a recording.
        m_fields->macroDispatch = playing;

        // BUG-102 v3: переход-детектор анимации портала 2926. Один раз
        // собираем все инстансы, липко трекаем ближайший к игроку (в радиусе
        // 800px по X) и логируем КАЖДОЕ изменение позиции/скейла - тики
        // старта и конца анимации видны в обоих мирах напрямую.
        if ((playing || bot.isRecording()) && m_player1) {
            // BUG-109: сбор пер-уровневый (Fields), не per-process static:
            // после выхода из уровня старые указатели висячие.
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
                    if (!inst) continue;  // BUG-109: null-гвард от порчи/дуал-инстансов
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

            // BUG-107: SAT-зонд статичного портала. Остаточный ±1 флип
            // (738/739, 1049/1050, 1114/1113 в сессии 15:32) происходит у
            // СТАТИЧНОГО портала при бит-в-бит позициях игрока - значит,
            // решением контакта управляет не анимация. Зонд каждый подшаг
            // берёт ближайший инстанс 2926 и воспроизводит решение ваниль-
            // ного collisionCheckObjects: rect-пересечение + OBB-путь
            // (m_shouldUseOuterOb -> overlaps1Way в обе стороны, семантика
            // GD: ось проваливается если low > origin + 1.0 || high <
            // origin; проекции - double, dot - float). Логируем переход
            // состояния (rectOO, obbOO) - кадр перехода в rec/play прямо
            // называет, какой вход SAT квантуется по-разному.
            GameObject* nearPortal = nullptr;
            float nearDist = 1e9f;
            for (auto* inst : m_fields->portalInstances) {
                if (!inst) continue;  // BUG-109: null-гвард
                const float d = std::fabs(inst->getPositionX() - px);
                if (d < nearDist) { nearDist = d; nearPortal = inst; }
            }
            if (nearPortal && nearDist < 400.0f) {
                const CCRect pr = m_player1->getObjectRect();
                const CCRect orct = nearPortal->getObjectRect();
                const bool rectOO = pr.intersectsRect(orct);
                bool obbA = false; // portal.overlaps1Way(player)
                bool obbB = false; // player.overlaps1Way(portal)
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
                                // порядок dot как в GD: y*ey + x*ex
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

        // BUG-102: хвост свип-окна — запоминаем X и Y конца шага (и в
        // playback, и в recording). После смерти prevX invalid — свип
        // начнёт с точки рестарта, а не с коры.
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
        // BUG-70 attribution: the click storm hypothesis. The heavy section's
        // wall time tracks clicks/tick almost linearly (~160us/click); this
        // probe isolates the vanilla drain (handleButton chain included) from
        // everything else in the step.

        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        m_fields->macroDispatch = false;

        if (!playing)
            return;

        // All inputs dispatched and the player is still alive: hand control
        // back to vanilla GD (SKK's playbackExhaustionNotified rule), instead
        // of freezing the player with a held button.
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
        // The macro's clicks reach us through m_queuedButtons, so while vanilla
        // drains them this flag is up and they must pass. Human input cannot
        // appear here during playback: processQueuedButtons clears the queue
        // before pushing the macro's commands.
        const bool fromMacro = m_fields->macroDispatch;

        // Diagnostic: any click that reaches handleButton while the macro is
        // playing but did NOT come from the macro itself. During playback the
        // queue is cleared and refilled with the macro's own commands, so this
        // must never fire - if it does, something (another mod, a leftover
        // human press, GD's own synthetic reset push) is injecting input.
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

        // GD's handleButton third parameter is NOT "is player 2" - it is the
        // raw flag and it is INVERTED relative to its name. SKK's
        // deferredSlotForPlayer makes that explicit (replay_engine.cpp:200-202):
        //     return twoPlayerMode ? static_cast<int>(!player2) : 0;
        // and EclipseMenu does the same thing (Bot.cpp:532, `!player1`).
        //
        // SKC read the flag straight, so on a two-player level it filed the
        // second player's clicks under P1: measured 586/586 clicks recorded
        // with p2=false and zero with p2=true. Worse, the pairing groups by
        // (button, player2), so both players' presses landed in one channel and
        // every tap became an orphan.
        //
        // Slot 1 only when the level actually HAS a second player; a solo run
        // must stay all-P1 regardless of what the flag says.
        // Routing, and ONLY m_twoPlayerMode decides it - never m_isDualMode.
//
// Measured on Artemis 13, a level with dual and no second player: 70 clicks
// arrived as rawPlayer2=false with twoPlayerMode=false and isDualMode=true,
// and every one of them was filed as P2. Those clicks jumped during recording
// and do nothing on playback, so the macro lost them and died.
//
// This is exactly the shape of a bug we had already fixed once and then
// reintroduced from the wrong helper. SKK has TWO functions that look like the
// same question:
//     deferredSlotForPlayer  replay_engine.cpp:200 - routing, m_twoPlayerMode
//     keepSecondPlayerSlot   replay_engine.cpp:204 - something else, and this
//                                                      one DOES include dual
// SKC took the inversion from the first and the condition from the second, so
// dual switched the second channel on without a second player existing. On a
// real two-player level m_isDualMode adds nothing, which is why this went
// unnoticed until a dual level recorded a solo run.
const bool keepSecond = m_levelSettings && m_levelSettings->m_twoPlayerMode;
        const bool recordPlayer2 = keepSecond && !player2;

        // BUG-70 attribution: the whole vanilla click chain (drain dispatch,
        // player button work) - the click-storm hypothesis says the heavy
        // section's per-step cost is clicks/tick x ~160us.

        GJBaseGameLayer::handleButton(down, button, player2);

        if (!fromMacro && bot.isRecording()) {
            // Every click filed to the second channel, with every input to the
            // decision. A dual-mode level turns keepSecond on without having a
            // second player, so this is where a solo run quietly grows a P2
            // stream - and a P2 press is not the same physics as a P1 press, so
            // the macro stops reproducing the moment one shows up.
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

    // BUG-70 attribution: the other half of the per-step cost. checkCollisions
    // runs once per player per step inside the physics loop; its cost scales
    // with nearby object density, which is the alternative explanation for
    // the heavy section (the click storm is the first one). 2.2081 binding:
    // int checkCollisions(PlayerObject*, float, bool) = 0x2137f0.
    int checkCollisions(PlayerObject* object, float dt, bool ignoreDamage) {

        return GJBaseGameLayer::checkCollisions(object, dt, ignoreDamage);
    }
};

class $modify(SkcPlayerObject, PlayerObject) {
    void update(float dt) {
        skkbot2::seam_probe::log("player-update-before", this, dt);
        PlayerObject::update(dt);
        skkbot2::seam_probe::log("player-update-after", this, dt);
    }

    void updateJump(float dt) {
        skkbot2::seam_probe::log("jump-before", this, dt);
        PlayerObject::updateJump(dt);
        skkbot2::seam_probe::log("jump-after", this, dt);
    }

    bool pushButton(PlayerButton button) {
        skkbot2::seam_probe::log("push-before", this, 0.f, -1, static_cast<int>(button));
        const bool result = PlayerObject::pushButton(button);
        skkbot2::seam_probe::log("push-after", this, 0.f, -1, static_cast<int>(button));
        return result;
    }

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

    // BUG-102 v3: РЕШАЮЩИЙ инструмент. Флип на f=416 не прошёл ни через
    // canBeActivatedByPlayer (portal-see молчит), ни через
    // GJBaseGameLayer::flipGravity (flip-apply молчит до f=1649), ни через
    // didHitHead (head-hit молчит до f=2972). Остаётся прямой вызов члена
    // PlayerObject::flipGravity - в него упираются и inline hardFlipGravity,
    // и dual-sync. Хук ловит КАЖДЫЙ флип и печатает канал (gjbl/other),
    // отложенные состояния и все источники позиции.
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

    // BUG-102 v3: флип на f=416 НЕ проходит через GJBaseGameLayer::flipGravity
    // (хук flip-apply молчит до f=1649). Единственный оставшийся путь -
    // didHitHead -> hardFlipGravity: отложенное состояние m_stateFlipGravity
    // (ставится касанием объекта 2866) + удар головой. Хук называет момент
    // решения и все источники позиции в нём.
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

// ---------------------------------------------------------------------------
// Editor playtest. RobTop does not reset m_currentProgress when entering a
// playtest, which would leave the clock running from the build mode; reset it
// so a recorded macro starts at frame 0 like in PlayLayer.
// ---------------------------------------------------------------------------
class $modify(SkcEditorUI, EditorUI) {
    void onPlaytest(CCObject* sender) {
        auto& bot = Bot::get();
        // An armed PLAY (queued from the editor's build mode) takes over when
        // the playtest physics begin, mirroring SKK's editor arming.
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

// --- session bookkeeping ----------------------------------------------------

} // namespace skk::skc
