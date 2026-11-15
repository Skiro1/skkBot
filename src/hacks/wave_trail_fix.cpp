

#include <Geode/Geode.hpp>
#include "../core/hotpath.hpp"
#include "../skc/skc_bot.hpp"
#include "../trajectory/trajectory.hpp"
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/HardStreak.hpp>

#include "skkBot.hpp"

#include <chrono>

using namespace geode::prelude;

namespace skk::wavetrail {

static bool fixEnabled() {
    auto* engine = ReplayEngine::get();
    return engine && engine->waveTrailFix;
}

static bool dragEnabled() {
    auto* engine = ReplayEngine::get();
    return engine && engine->waveTrailFix && engine->waveTrailDragFix;
}

static bool drawEnabled() {
    auto* engine = ReplayEngine::get();
    return engine && engine->waveTrailFix && engine->waveTrailDrawFix;
}

static bool trailContextActive() {
    return GJBaseGameLayer::get() != nullptr;
}

struct WtCounters {
    uint64_t dragAdds = 0;
    uint64_t dragSteps = 0;
    uint64_t skipZero = 0;
    uint64_t skipJump = 0;
    uint64_t placeSuppressed = 0;
    std::chrono::steady_clock::time_point lastPrint{};
    bool timerArmed = false;
};
static WtCounters g_wt;

static void wtResetCounters() { g_wt = {}; }

static const char* wtModeLabel() {
    auto& skcBot = skk::skc::Bot::get();
    if (skcBot.isRecording()) return "SKC-record";
    if (skcBot.isPlaying()) return "SKC-play";
    auto* engine = ReplayEngine::get();
    if (engine && engine->engineMode == MODE_CAPTURE) return "SKK-record";
    if (engine && engine->engineMode == MODE_EXECUTE) return "SKK-play";
    return "vanilla";
}

static void wtLogSession() {
    wtResetCounters();
    skkbot2::log::info(
        "[WT] session: mode={} fix={} drag={} draw={} context={}",
        wtModeLabel(), fixEnabled() ? 1 : 0, dragEnabled() ? 1 : 0,
        drawEnabled() ? 1 : 0, trailContextActive() ? "level" : "no-layer");
}

static void wtMaybePrintStats() {
    if (!g_wt.timerArmed) {
        g_wt.lastPrint = std::chrono::steady_clock::now();
        g_wt.timerArmed = true;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_wt.lastPrint < std::chrono::seconds(10)) return;
    g_wt.lastPrint = now;
    if (g_wt.dragSteps == 0 && g_wt.dragAdds == 0 && g_wt.placeSuppressed == 0)
        return;
    skkbot2::log::info(
        "[WT] stats 10s: mode={} dartSteps={} dragAdds={} skipZero={} "
        "skipJump={} placeSuppressed={}",
        wtModeLabel(), g_wt.dragSteps, g_wt.dragAdds, g_wt.skipZero,
        g_wt.skipJump, g_wt.placeSuppressed);
}

struct Subpart {
    std::optional<ccColor3B> color;
    float weight = 1.0f;
    float opacity = 1.0f;
    bool solidOnly = false;
    bool nonSolidOnly = false;
};

struct Part {
    float start = -1.0f;
    float end = 1.0f;
    float startOffset = 0.0f;
    float endOffset = 0.0f;
    std::optional<float> pulseOverride;
    std::optional<float> sizeOverride;
    std::vector<Subpart> subparts;
};

static std::optional<CCPoint> lineSegmentIntersection(CCPoint s1a, CCPoint s1b,
                                                      CCPoint s2a, CCPoint s2b) {
    if (s1a == s1b || s2a == s2b) return std::nullopt;
    const CCPoint d1 = s1b - s1a;
    const CCPoint d2 = s2b - s2a;
    const float cross = d1.cross(d2);
    if (std::fabs(cross) < 0.000001f) return std::nullopt;
    const CCPoint toS2 = s2a - s1a;
    const float t1 = toS2.cross(d2) / cross;
    const float t2 = toS2.cross(d1) / cross;
    if (t1 < -0.000001f || t1 > 1.0f + 0.000001f ||
        t2 < -0.000001f || t2 > 1.0f + 0.000001f) {
        return std::nullopt;
    }
    return s1a + d1 * t1;
}

static CCPoint offsetPoint(std::optional<CCPoint> prev, CCPoint cur,
                           std::optional<CCPoint> next, float distance) {
    if (!prev) {
        const CCPoint delta = cur - *next;
        if (delta.getLength() <= 0.0001f) return cur;
        const CCPoint normal = delta.getPerp().normalize();
        return cur + normal * distance;
    }
    if (!next) {
        const CCPoint delta = cur - *prev;
        if (delta.getLength() <= 0.0001f) return cur;
        const CCPoint normal = delta.getRPerp().normalize();
        return cur + normal * distance;
    }
    const CCPoint delta1 = cur - *prev;
    const CCPoint delta2 = cur - *next;
    if (delta1.getLength() <= 0.0001f || delta2.getLength() <= 0.0001f) return cur;
    const CCPoint n1 = delta1.getRPerp().normalize();
    const CCPoint n2 = delta2.getPerp().normalize();
    const CCPoint bisectorRaw = n1 + n2;
    if (bisectorRaw.getLength() <= 0.0001f) {
        return cur + n1 * distance;
    }
    const CCPoint bisector = bisectorRaw.normalize();
    const float length = distance / std::sqrt(0.5f + 0.5f * n1.dot(n2));
    return cur + bisector * length;
}

static void createOffset(const std::vector<CCPoint>& path, float offset,
                         float extensionLength,
                         std::vector<CCPoint>& points, std::vector<bool>& labels) {
    points.clear();
    points.reserve(path.size());
    labels.clear();
    labels.reserve(path.size() > 0 ? path.size() - 1 : 0);
    for (size_t i = 0; i < path.size(); i++) {
        std::optional<CCPoint> previousPoint;
        CCPoint currentPoint = path[i];
        std::optional<CCPoint> nextPoint;
        if (i != 0) previousPoint = path[i - 1];
        if (i != path.size() - 1) nextPoint = path[i + 1];

        if (!previousPoint) {
            const CCPoint delta = *nextPoint - currentPoint;
            if (delta.getLength() > 0.0001f)
                currentPoint -= delta.normalize() * extensionLength;
        } else if (!nextPoint) {
            const float length = previousPoint->getDistance(currentPoint);
            const CCPoint delta = currentPoint - *previousPoint;
            if (delta.getLength() > 0.0001f)
                currentPoint += delta.normalize() *
                                ((length > extensionLength + 5.0f) ? -5.0f
                                                                   : extensionLength - length);
        }

        const CCPoint point = offsetPoint(previousPoint, currentPoint, nextPoint, offset);
        points.push_back(point);
        if (!previousPoint) continue;
        const CCPoint deltaDir = currentPoint - *previousPoint;
        const CCPoint actualDirection =
            deltaDir.getLength() > 0.0001f ? deltaDir.normalize() : CCPoint{1.0f, 0.0f};
        CCPoint trailDirection = point - points[points.size() - 2];
        if (trailDirection.isZero()) {
            labels.push_back(true);
            continue;
        }
        const float trailLen = trailDirection.getLength();
        if (trailLen > 0.0001f)
            trailDirection = trailDirection / trailLen;
        labels.push_back(actualDirection.dot(trailDirection) < -1.0f + 0.000001f ? false : true);
    }
}

static void fixPoints(std::vector<CCPoint>& path, float offset,
                      std::vector<CCPoint>& points, std::vector<bool>& labels,
                      std::vector<CCPoint>& otherPoints) {
    for (size_t i = 0; i < labels.size(); i++) {
        CCPoint nextSegmentP1, nextSegmentP2, prevSegmentP1, prevSegmentP2;
        bool nextFinished = false, prevFinished = false;

        if (labels[i]) continue;
        if (i + 1 != labels.size() && !labels[i + 1] &&
            path[i + 1].getDistanceSq(path[i]) > path[i + 2].getDistanceSq(path[i + 1])) {
            continue;
        }

        if (i == 0) {
            nextSegmentP1 = points[0];
            nextSegmentP2 = otherPoints[0];
        } else {
            nextSegmentP1 = points[i - 1];
            nextSegmentP2 = offsetPoint(path[i - 1], path[i], {}, offset);
        }
        if (i + 2 >= points.size()) {
            prevSegmentP1 = points.back();
            prevSegmentP2 = otherPoints.back();
        } else {
            prevSegmentP1 = points[i + 2];
            prevSegmentP2 = offsetPoint({}, path[i + 1], path[i + 2], offset);
        }
        for (size_t j = 2; j < points.size() + 1; j++) {
            if (i + j >= points.size() && !nextFinished) {
                nextFinished = true;
                if (auto is = lineSegmentIntersection(points.back(), otherPoints.back(),
                                                      nextSegmentP1, nextSegmentP2)) {
                    for (size_t k = i; k < points.size(); k++) points[k] = *is;
                    for (size_t k = i; k < labels.size(); k++) labels[k] = true;
                    break;
                }
            }
            if (i + 1 < j && !prevFinished) {
                prevFinished = true;
                if (auto is = lineSegmentIntersection(points[0], otherPoints[0],
                                                      prevSegmentP1, prevSegmentP2)) {
                    for (size_t k = 0; k < i + 2; k++) points[k] = *is;
                    for (size_t k = 0; k < i + 1; k++) labels[k] = true;
                    break;
                }
            }
            if (!nextFinished) {
                if (auto is = lineSegmentIntersection(points[i - 1 + j], points[i + j],
                                                      nextSegmentP1, nextSegmentP2)) {
                    for (size_t k = i; k < i + j; k++) points[k] = *is;
                    for (size_t k = i; k < i + j - 1; k++) labels[k] = true;
                    break;
                }
            }
            if (!prevFinished) {
                if (auto is = lineSegmentIntersection(points[i + 1 - j], points[i + 2 - j],
                                                      prevSegmentP1, prevSegmentP2)) {
                    for (size_t k = i + 2 - j; k < i + 2; k++) points[k] = *is;
                    for (size_t k = i + 2 - j; k < i + 1; k++) labels[k] = true;
                    break;
                }
            }
            if (nextFinished && prevFinished) break;
        }
    }
}

static std::vector<Part> buildConfiguration() {
    auto* engine = ReplayEngine::get();
    std::vector<Part> config;

    Part core;
    core.start = -1.0f;
    core.end = 1.0f;
    Subpart coreSub;
    if (engine && engine->waveTrailColorOverride) {
        coreSub.color = ccc3(
            static_cast<uint8_t>(std::clamp(engine->waveTrailColorR, 0.0f, 1.0f) * 255.0f),
            static_cast<uint8_t>(std::clamp(engine->waveTrailColorG, 0.0f, 1.0f) * 255.0f),
            static_cast<uint8_t>(std::clamp(engine->waveTrailColorB, 0.0f, 1.0f) * 255.0f));
    }
    if (engine) coreSub.opacity = engine->waveTrailOpacity;
    core.subparts.push_back(coreSub);
    config.push_back(core);

    Part outline;
    outline.start = -0.3333333f;
    outline.end = 0.3333333f;
    Subpart outlineSub;
    outlineSub.color = ccc3(255, 255, 255);
    outlineSub.nonSolidOnly = true;
    if (engine) outlineSub.opacity = engine->waveTrailOutlineOpacity;
    outline.subparts.push_back(outlineSub);
    config.push_back(outline);

    return config;
}

}

class $modify(WTSHardStreak, HardStreak) {
    struct Fields {
        std::vector<skk::wavetrail::Part> configuration;
        std::vector<CCPoint> trailPath;
        std::vector<CCPoint> firstPts, lastPts, prevPts, nextPts;
        std::vector<bool> firstLbl, lastLbl, prevLbl, nextLbl;
        bool meshValid = false;
        bool sDrawStreak = false;
        bool sSolid = false;
        bool sFlipped = false;
        float sWave = -1.0f;
        float sPulse = -1.0f;
        float sOpacity = -1.0f;
        ccColor3B sColor{0, 0, 0};
        CCPoint sPosition;
        size_t sCount = 0;
        CCPoint sCurrent;
        bool haveSnapshot = false;
    };

    void drawTriangle(CCPoint const& p1, CCPoint const& p2, CCPoint const& p3,
                      ccColor4B color) {
        if (m_nBufferCount + 3u >= m_uBufferCapacity) {
            m_uBufferCapacity += std::max(m_uBufferCapacity, 3u);
            m_pBuffer = static_cast<ccV2F_C4B_T2F*>(
                realloc(m_pBuffer, m_uBufferCapacity * sizeof(ccV2F_C4B_T2F)));
        }
        m_pBuffer[m_nBufferCount++] = {{p1.x, p1.y}, color, {0.0f, 0.0f}};
        m_pBuffer[m_nBufferCount++] = {{p2.x, p2.y}, color, {0.0f, 0.0f}};
        m_pBuffer[m_nBufferCount++] = {{p3.x, p3.y}, color, {0.0f, 0.0f}};
        m_bDirty = true;
    }

    void updateStroke(float dt) {
        skkbot2::hot::Timed skkbot2Timing(skkbot2::hot::StreakUpdateStroke);
        if (!skk::wavetrail::drawEnabled()) {
            HardStreak::updateStroke(dt);
            return;
        }

        const ccColor3B colorNow = getColor();
        if (m_fields->meshValid && m_fields->sDrawStreak == m_drawStreak
            && m_fields->sCount == m_pointArray->count()
            && m_fields->sCurrent == m_currentPoint
            && m_fields->sWave == m_waveSize && m_fields->sPulse == m_pulseSize
            && m_fields->sOpacity == getOpacity()
            && m_fields->sSolid == m_isSolid && m_fields->sFlipped == m_isFlipped
            && m_fields->sColor.r == colorNow.r && m_fields->sColor.g == colorNow.g
            && m_fields->sColor.b == colorNow.b
            && m_fields->sPosition == getPosition()) {
            return;
        }

        this->clear();

        if (!m_drawStreak || m_pointArray->count() == 0 || getOpacity() == 0) {
            return;
        }

        const CCPoint position = getPosition();

        auto& trailPath = m_fields->trailPath;
        trailPath.clear();
        trailPath.reserve(m_pointArray->count());
        for (unsigned int i = 0; i < m_pointArray->count(); i++) {
            auto* node = static_cast<PointNode*>(m_pointArray->objectAtIndex(i));
            if (!node) break;
            const CCPoint p = node->m_point - position;
            if (!trailPath.empty() && trailPath.back() == p) continue;
            trailPath.push_back(p);
        }
        if (trailPath.empty() || trailPath.back() != (m_currentPoint - position)) {
            trailPath.push_back(m_currentPoint - position);
        }

        if (m_fields->configuration.empty()) {
            m_fields->configuration = skk::wavetrail::buildConfiguration();
        }

        auto& firstPts = m_fields->firstPts;
        auto& lastPts = m_fields->lastPts;
        auto& prevPts = m_fields->prevPts;
        auto& nextPts = m_fields->nextPts;
        auto& prevLbl = m_fields->prevLbl;

        for (auto& part : m_fields->configuration) {
            const float waveSize = part.sizeOverride.value_or(m_waveSize);
            const float pulseSize = part.pulseOverride.value_or(m_pulseSize);
            const float relativeFactor = 3.0f * waveSize * pulseSize;

            const float totalWidth =
                (part.endOffset + part.end * relativeFactor) -
                (part.startOffset + part.start * relativeFactor);
            float totalWeight = 0.0f;
            for (auto& sp : part.subparts) totalWeight += sp.weight;
            const float oneWidth = totalWidth / totalWeight;

            const float firstOffset = m_isFlipped ? (part.endOffset + part.end * relativeFactor)
                                                  : (part.startOffset + part.start * relativeFactor);
            const float lastOffset = m_isFlipped ? (part.startOffset + part.start * relativeFactor)
                                                 : (part.endOffset + part.end * relativeFactor);

            skk::wavetrail::createOffset(trailPath, firstOffset, relativeFactor, firstPts, m_fields->firstLbl);
            skk::wavetrail::createOffset(trailPath, lastOffset, relativeFactor, lastPts, m_fields->lastLbl);
            prevPts = firstPts;
            prevLbl = m_fields->firstLbl;

            skk::wavetrail::fixPoints(trailPath, firstOffset, prevPts, prevLbl, lastPts);

            totalWeight = part.subparts[0].weight;
            for (size_t i = 1; i < part.subparts.size() + 1; i++) {
                const float nextOffset = (i == part.subparts.size())
                                             ? lastOffset
                                             : (firstOffset + totalWeight * oneWidth);
                skk::wavetrail::createOffset(trailPath, nextOffset, relativeFactor, nextPts, m_fields->nextLbl);
                skk::wavetrail::fixPoints(trailPath, nextOffset, nextPts, m_fields->nextLbl, firstPts);

                const auto& sub = part.subparts[i - 1];
                const bool applies =
                    (!sub.solidOnly && !sub.nonSolidOnly) ||
                    (sub.solidOnly && m_isSolid) ||
                    (sub.nonSolidOnly && !m_isSolid);
                if (applies) {
                    const ccColor4B drawColor = ccc4(
                        sub.color.value_or(getColor()).r,
                        sub.color.value_or(getColor()).g,
                        sub.color.value_or(getColor()).b,
                        static_cast<uint8_t>(std::clamp(
                            sub.opacity * getOpacity(), 0.0f, 255.0f)));
                    for (size_t j = 0; j + 1 < prevPts.size(); j++) {
                        if (prevPts[j] != prevPts[j + 1]) {
                            drawTriangle(prevPts[j], prevPts[j + 1], nextPts[j + 1], drawColor);
                        }
                        if (nextPts[j] != nextPts[j + 1]) {
                            drawTriangle(nextPts[j + 1], nextPts[j], prevPts[j], drawColor);
                        }
                    }
                }

                prevPts = nextPts;
                prevLbl = m_fields->nextLbl;

                if (i == part.subparts.size()) break;
                totalWeight += part.subparts[i].weight;
            }
        }

        m_fields->meshValid = true;
        m_fields->haveSnapshot = true;
        m_fields->sDrawStreak = m_drawStreak;
        m_fields->sSolid = m_isSolid;
        m_fields->sFlipped = m_isFlipped;
        m_fields->sWave = m_waveSize;
        m_fields->sPulse = m_pulseSize;
        m_fields->sOpacity = getOpacity();
        m_fields->sColor = colorNow;
        m_fields->sPosition = position;
        m_fields->sCount = m_pointArray->count();
        m_fields->sCurrent = m_currentPoint;
    }

};

class $modify(WTSPlayerObject, PlayerObject) {
    struct Fields {
        bool gotHereFromUpdate = false;
        bool forceAdd = true;
        bool spiderPadTriggered = false;
        bool forceAddSpiderPad = false;
        bool forceAddSpiderRing = false;
        bool justTeleported = false;
        bool teleportedPreviouslySpiderRing = false;
        bool transitionToCollision = false;
        float portalTargetLine = 0.0f;
        CCPoint previousPos{-12, -12};
        CCPoint currentPos{-12, -12};
        CCPoint nextPosNoCollision{-12, -12};
        GameObject* collidedSlope = nullptr;
        float deltaFactor = 0.0f;
    };

    void resetObject() {
        WTSPlayerObject::Fields defaultFields;
        *m_fields.operator->() = defaultFields;
        if (m_waveTrail) m_waveTrail->reset();
        PlayerObject::resetObject();
    }

    void resetStreak() {
        PlayerObject::resetStreak();
    }

    bool preSlopeCollision(float p0, GameObject* p1) {
        const bool value = PlayerObject::preSlopeCollision(p0, p1);
        if (!value) m_fields->collidedSlope = p1;
        return value;
    }

    void updateJump(float dt) {
        PlayerObject::updateJump(dt);
    }

    void setYVelocity(double velocity, int type) {
        PlayerObject::setYVelocity(velocity, type);
    }

void update(float deltaFactor) {
          if (skk::trajectory::isSimulating()) {
              PlayerObject::update(deltaFactor);
              return;
          }
          skkbot2::hot::Timed skkbot2Count(skkbot2::hot::WtsPlayerUpdate);
          m_fields->currentPos = getRealPosition();
        m_fields->gotHereFromUpdate = true;
        PlayerObject::update(deltaFactor);
        m_fields->nextPosNoCollision = getRealPosition();
        m_fields->collidedSlope = nullptr;
        m_fields->deltaFactor = deltaFactor;
    }

    void updateRotation(float p0) {
        PlayerObject::updateRotation(p0);

        if (skk::trajectory::isSimulating()) return;

        if (!skk::wavetrail::dragEnabled() || !skk::wavetrail::trailContextActive() ||
            !m_fields->gotHereFromUpdate) {
            return;
        }
        m_fields->gotHereFromUpdate = false;
        ++skk::wavetrail::g_wt.dragSteps;
        skk::wavetrail::wtMaybePrintStats();

        const CCPoint pos = getRealPosition();

        if (LevelEditorLayer::get()) {
            const bool playtesting =
                m_gameLayer && static_cast<int>(m_gameLayer->m_playbackMode) != 0;
            if (!playtesting) return;
        }

        if (!m_isDart || m_isHidden) {
            m_fields->previousPos = m_fields->currentPos;
            m_fields->justTeleported = false;
            m_fields->teleportedPreviouslySpiderRing = false;
            m_fields->forceAddSpiderRing = false;
            m_fields->forceAddSpiderPad = false;
            m_fields->spiderPadTriggered = false;
            m_fields->forceAdd = true;
            return;
        }

        const CCPoint previousPosition = m_fields->previousPos;
        CCPoint currentPosition = m_fields->currentPos;
        CCPoint nextPositionNoCollision = m_fields->nextPosNoCollision;
        CCPoint nextPosition = getRealPosition();

        if (m_wasTeleported && !m_fields->justTeleported) {
            addWaveTrailPoint(nextPosition);
            return;
        } else if (m_fields->justTeleported) {
            m_fields->justTeleported = false;
            m_fields->teleportedPreviouslySpiderRing = m_fields->forceAddSpiderRing;
            m_fields->forceAddSpiderRing = false;
            m_fields->forceAddSpiderPad = false;
            m_fields->spiderPadTriggered = false;
            m_fields->forceAdd = false;
            m_fields->portalTargetLine = m_isSideways ? previousPosition.y : previousPosition.x;
            addWaveTrailPoint(previousPosition);
            return;
        } else if (m_fields->forceAdd && !m_fields->forceAddSpiderRing &&
                   !m_fields->forceAddSpiderPad) {
            m_fields->forceAdd = false;
            addWaveTrailPoint(currentPosition);
            m_fields->previousPos = currentPosition;
            return;
        } else if (m_fields->spiderPadTriggered) {
            m_fields->spiderPadTriggered = false;
            m_fields->forceAddSpiderPad = true;
            m_fields->previousPos = currentPosition;
            return;
        } else if (m_fields->forceAddSpiderRing || m_fields->forceAddSpiderPad) {
            m_fields->forceAddSpiderRing = false;
            m_fields->forceAddSpiderPad = false;
            m_fields->forceAdd = false;
            m_fields->transitionToCollision = false;
            m_fields->teleportedPreviouslySpiderRing = false;
            const CCPoint pointToAdd = m_isSideways
                                           ? CCPoint{nextPosition.x, previousPosition.y}
                                           : CCPoint{previousPosition.x, nextPosition.y};
            addWaveTrailPoint(pointToAdd);
            m_fields->previousPos = pointToAdd;
            return;
        } else if (m_fields->teleportedPreviouslySpiderRing) {
            m_fields->teleportedPreviouslySpiderRing = false;
            const CCPoint pointToAdd =
                m_isSideways ? CCPoint{nextPosition.x, m_fields->portalTargetLine}
                             : CCPoint{m_fields->portalTargetLine, nextPosition.y};
            addWaveTrailPoint(pointToAdd);
            m_fields->previousPos = pointToAdd;
            return;
        }

        const CCPoint currentDelta = nextPosition - currentPosition;
        const CCPoint previousDelta = currentPosition - previousPosition;
        const float currentLen = currentDelta.getLength();
        const float previousLen = previousDelta.getLength();
        if (currentLen <= 0.0001f || previousLen <= 0.0001f) {
            ++skk::wavetrail::g_wt.skipZero;
            skkbot2::log::warn("[WT] zero-delta: cL={:.6f} pL={:.6f} pos=({:.2f},{:.2f})", currentLen, previousLen,
                currentPosition.x, currentPosition.y);
            skkbot2::log::warn("[WT] zero-delta: prev=({:.2f},{:.2f}) next=({:.2f},{:.2f})",
                previousPosition.x, previousPosition.y, nextPosition.x, nextPosition.y);
            skkbot2::Logger::get().flush();
            m_fields->previousPos = currentPosition;
            return;
        }
        if (currentLen > 150.0f || previousLen > 150.0f) {
            ++skk::wavetrail::g_wt.skipJump;
            skkbot2::log::warn("[WT] jump-guard: cL={:.1f} pL={:.1f} pos=({:.2f},{:.2f}) prev=({:.2f},{:.2f})",
                currentLen, previousLen, currentPosition.x, currentPosition.y,
                previousPosition.x, previousPosition.y);
            skkbot2::Logger::get().flush();
            m_fields->previousPos = currentPosition;
            return;
        }
        const CCPoint currentVector = currentDelta / currentLen;
        const CCPoint previousVector = previousDelta / previousLen;
        const float crossProductMagnitude = std::abs(currentVector.cross(previousVector));

        const float errorMargin = 0.004f / std::max(m_fields->deltaFactor, 0.0001f);

        if (crossProductMagnitude <= errorMargin) return;

        if (nextPosition != nextPositionNoCollision && !m_fields->transitionToCollision) {
            float objectBoundMin = std::numeric_limits<float>::lowest();
            float objectBoundMax = std::numeric_limits<float>::max();
            float hitboxSizeProbably = 30.0f;
            GameObject* object = m_collidedObject ? m_collidedObject : m_fields->collidedSlope;
            if (object) {
                objectBoundMin = object->getObjectRect().getMinX();
                objectBoundMax = object->getObjectRect().getMaxX();
            }

            if (m_isSideways) {
                if (object) {
                    objectBoundMin = object->getObjectRect().getMinY();
                    objectBoundMax = object->getObjectRect().getMaxY();
                }
                std::swap(nextPositionNoCollision.x, nextPositionNoCollision.y);
                std::swap(nextPosition.x, nextPosition.y);
                std::swap(currentPosition.x, currentPosition.y);
            }
            const float dx = nextPositionNoCollision.x - currentPosition.x;
            if (std::fabs(dx) <= 0.0001f) {
                skkbot2::log::warn(
                    "[WT] dx-zero guard: dx={:.6f} pos=({:.2f},{:.2f}) nextNoCol=({:.2f},{:.2f})",
                    dx, currentPosition.x, currentPosition.y,
                    nextPositionNoCollision.x, nextPositionNoCollision.y);
                skkbot2::Logger::get().flush();
                addWaveTrailPoint(currentPosition);
                m_fields->previousPos = currentPosition;
                m_fields->transitionToCollision = true;
                return;
            }
            const float intercept =
                (nextPositionNoCollision.y - currentPosition.y) / dx;
            float desiredValue =
                (nextPosition.y - currentPosition.y + intercept * currentPosition.x) / intercept;
            if ((desiredValue < currentPosition.x && !m_isGoingLeft) ||
                (desiredValue > currentPosition.x && m_isGoingLeft) ||
                (currentPosition.x + hitboxSizeProbably / 2 < objectBoundMin && !m_isGoingLeft) ||
                (currentPosition.x - hitboxSizeProbably / 2 > objectBoundMax && m_isGoingLeft)) {
                if (m_isSideways) {
                    addWaveTrailPoint({currentPosition.y, currentPosition.x});
                } else {
                    addWaveTrailPoint(currentPosition);
                }
                desiredValue = currentPosition.x;
            }
            CCPoint intersectionPoint{desiredValue, nextPosition.y};
            addWaveTrailPoint(intersectionPoint);
            m_fields->previousPos = intersectionPoint;
            m_fields->transitionToCollision = true;
        } else {
            if (nextPosition == nextPositionNoCollision) m_fields->transitionToCollision = false;
            addWaveTrailPoint(currentPosition);
            m_fields->previousPos = currentPosition;
        }
    }

    inline void addWaveTrailPoint(CCPoint point) {
        if (!m_waveTrail || !m_waveTrail->m_pointArray) return;
        ++skk::wavetrail::g_wt.dragAdds;
        if (std::isnan(point.x) || std::isnan(point.y) ||
            std::isinf(point.x) || std::isinf(point.y)) {
            skkbot2::log::warn("[WT] addP: NaN/inf point=({:.2f},{:.2f}) — skipped",
                point.x, point.y);
            skkbot2::Logger::get().flush();
            return;
        }
        unsigned objectCount = static_cast<unsigned>(m_waveTrail->m_pointArray->count());
        const unsigned kMaxTrailPoints = 2048u;
        while (objectCount >= kMaxTrailPoints) {
            m_waveTrail->m_pointArray->removeObjectAtIndex(0);
            --objectCount;
        }
        if (objectCount >= 3) {
            auto* arr = m_waveTrail->m_pointArray;
            const CCPoint point0 =
                static_cast<PointNode*>(arr->objectAtIndex(objectCount - 2))->m_point;
            const CCPoint point1 =
                static_cast<PointNode*>(arr->objectAtIndex(objectCount - 1))->m_point;
            const CCPoint delta01 = point1 - point0;
            const CCPoint delta12 = point - point1;
            const float len01 = delta01.getLength();
            const float len12 = delta12.getLength();
            if (len01 <= 0.0001f || len12 <= 0.0001f) {
                m_waveTrail->addPoint(point);
                return;
            }
            const CCPoint vector01 = delta01 / len01;
            const CCPoint vector12 = delta12 / len12;
            const float cpm = std::abs(vector12.cross(vector01));
            if (cpm <= 0.001f) {
                if (auto* last =
                        static_cast<PointNode*>(arr->objectAtIndex(objectCount - 1))) {
                    last->m_point = point;
                }
                return;
            }
        }
        m_waveTrail->addPoint(point);
    }

    bool pushButton(PlayerButton button) {
        if (skk::trajectory::isSimulating()) {
            return PlayerObject::pushButton(button);
        }
        const int TOGGLE_RING = 1594;
        const int TELEPORT_RING = 3027;
        const int SPIDER_RING = 3004;
        if (!skk::wavetrail::dragEnabled() || !m_isDart || !m_gameLayer ||
            LevelEditorLayer::get()) {
            return PlayerObject::pushButton(button);
        }
        bool willTriggerSpiderRing = false;

        for (unsigned i = 0; i < m_touchingRings->count(); i++) {
            auto* ring = static_cast<RingObject*>(m_touchingRings->objectAtIndex(i));
            switch (ring->m_objectID) {
                case TOGGLE_RING:
                    if (ring->m_claimTouch) return PlayerObject::pushButton(button);
                    [[fallthrough]];
                case TELEPORT_RING:
                    continue;
                case SPIDER_RING:
                    willTriggerSpiderRing = true;
                    [[fallthrough]];
                default:
                    if (!willTriggerSpiderRing) return PlayerObject::pushButton(button);
                    break;
            }
        }
        if (willTriggerSpiderRing) {
            if (m_waveTrail) m_waveTrail->addPoint(m_fields->currentPos);
            m_fields->previousPos = m_fields->currentPos;
            m_fields->forceAddSpiderRing = true;
        }
        return PlayerObject::pushButton(button);
    }

    void spiderTestJump(bool p0) {
        if (!skk::wavetrail::dragEnabled() || !m_isDart || !m_gameLayer ||
            LevelEditorLayer::get() || m_fields->justTeleported) {
            return PlayerObject::spiderTestJump(p0);
        }
        if (m_waveTrail) m_waveTrail->addPoint(m_fields->currentPos);
        m_fields->previousPos = m_fields->currentPos;
        if (!m_fields->forceAddSpiderRing) m_fields->spiderPadTriggered = true;
        PlayerObject::spiderTestJump(p0);
    }

    void doReversePlayer(bool state) {
        if (skk::wavetrail::dragEnabled()) m_fields->forceAdd = true;
        PlayerObject::doReversePlayer(state);
    }

    void placeStreakPoint(cocos2d::CCPoint pos) {
        if (skk::wavetrail::dragEnabled() && m_isDart && m_gameLayer) {
            ++skk::wavetrail::g_wt.placeSuppressed;
            return;
        }
        PlayerObject::placeStreakPoint(pos);
    }

    void toggleVisibility(bool state) {
        const bool needsPoint = m_isHidden;
        PlayerObject::toggleVisibility(state);
        if (skk::wavetrail::dragEnabled() && state && m_isDart && needsPoint) {
            m_fields->forceAdd = true;
        }
    }
};

class $modify(WTSPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useEditorLayer) {
        const bool ok = PlayLayer::init(level, useEditorLayer);
        if (ok) skk::wavetrail::wtLogSession();
        return ok;
    }

    void checkForEnd() {
        PlayLayer::checkForEnd();
    }

    void playEndAnimationToPos(CCPoint pos) {
        if (skk::wavetrail::dragEnabled()) {
            if (m_player1 && m_player1->m_isDart) {
                m_player1->m_waveTrail->addPoint(m_player1->getRealPosition());
            }
            if (m_player2 && m_player2->m_isDart) {
                m_player2->m_waveTrail->addPoint(m_player2->getRealPosition());
            }
        }
        PlayLayer::playEndAnimationToPos(pos);
    }
};

class $modify(WTSBaseGameLayer, GJBaseGameLayer) {
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void teleportPlayer(TeleportPortalObject* portal, PlayerObject* player) {
        GJBaseGameLayer::teleportPlayer(portal, player);
        if (!skk::wavetrail::dragEnabled()) return;
        if (!player) player = m_player1;
        if (LevelEditorLayer::get() || !player->m_isDart) return;
        static_cast<WTSPlayerObject*>(player)->m_fields->previousPos =
            player->getRealPosition();
        static_cast<WTSPlayerObject*>(player)->m_fields->justTeleported = true;
    }

    void toggleDualMode(GameObject* portal, bool state, PlayerObject* playerTouchingPortal,
                        bool p4) {
        if (!skk::wavetrail::dragEnabled() || !state || !playerTouchingPortal
            || !m_player1 || !m_player2 || playerTouchingPortal != m_player2) {
            GJBaseGameLayer::toggleDualMode(portal, state, playerTouchingPortal, p4);
            return;
        }
        WTSPlayerObject::Fields fieldsBackup =
            *static_cast<WTSPlayerObject*>(m_player2)->m_fields.operator->();
        GJBaseGameLayer::toggleDualMode(portal, state, playerTouchingPortal, p4);
        if (m_player1 && m_player1 != playerTouchingPortal) {
            *static_cast<WTSPlayerObject*>(m_player1)->m_fields.operator->() = fieldsBackup;
        }
    }
};
