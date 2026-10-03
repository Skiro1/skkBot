// Wave Trail Fix — a self-contained, always-on, visual-only rework of the dart
// (wave) trail. Combines two reference mods into one feature:
//
//   drag-fix  (geode-wave-trail-drag-fix, nytelytee):
//     Vanilla GD only adds streak points from PlayerObject::postCollision, so
//     the trail breaks whenever the player moves vertically without colliding
//     (noclip, spider rings/pads, teleports, mid-air mode swaps). We rebuild
//     the points from PlayerObject::updateRotation instead — it runs every
//     step regardless of collisions, so the trail is complete and straight.
//
//   draw-fix  (geode-wave-trail-draw-fix, nytelytee):
//     Replaces HardStreak::updateStroke with a miter-joined band renderer that
//     supports sub-parts: a colored core plus a white outline. Optional color
//     override and per-part opacity.
//
// The feature is visual only: it never touches player physics, input, or the
// engine's replay state. Works in PlayLayer and in the LevelEditor playtest,
// independent of record / play / noclip.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/HardStreak.hpp>

#include "skkBot.hpp"

using namespace geode::prelude;

namespace skk::wavetrail {

// ---------------------------------------------------------------------------
// Settings access (kept on ReplayEngine so both GUI shells stay in sync).
// ---------------------------------------------------------------------------
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

// HardStreak is only meaningful on the level layer; LevelEditorLayer inherits
// GJBaseGameLayer and the editor playtest uses the same PlayerObject, so the
// drag fix applies there too. The draw fix applies everywhere the streak is
// drawn.
static bool trailContextActive() {
    return GJBaseGameLayer::get() != nullptr;
}

// ---------------------------------------------------------------------------
// Sub-part configuration for the draw fix. A part is one band of the trail,
// described as an offset range [-1..1] relative to the streak's half-width,
// with one or more colored sub-parts (weights).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Geometry helpers (ported from draw-fix utilities).
// ---------------------------------------------------------------------------
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

// Offset a point by distance along the miter of the two adjacent segments.
static CCPoint offsetPoint(std::optional<CCPoint> prev, CCPoint cur,
                           std::optional<CCPoint> next, float distance) {
    if (!prev) {
        const CCPoint normal = (cur - *next).getPerp().normalize();
        return cur + normal * distance;
    }
    if (!next) {
        const CCPoint normal = (cur - *prev).getRPerp().normalize();
        return cur + normal * distance;
    }
    const CCPoint n1 = (cur - *prev).getRPerp().normalize();
    const CCPoint n2 = (cur - *next).getPerp().normalize();
    const CCPoint bisector = (n1 + n2).normalize();
    const float length = distance / std::sqrt(0.5f + 0.5f * n1.dot(n2));
    return cur + bisector * length;
}

// Offset a whole path, returning the offset points and a per-segment label
// (true = offset segment runs the same direction as its source segment).
static std::tuple<std::vector<CCPoint>, std::vector<bool>>
createOffset(std::vector<CCPoint>& path, float offset, float extensionLength) {
    std::vector<CCPoint> points;
    points.reserve(path.size());
    std::vector<bool> labels;
    labels.reserve(path.size() > 0 ? path.size() - 1 : 0);
    for (size_t i = 0; i < path.size(); i++) {
        std::optional<CCPoint> previousPoint;
        CCPoint currentPoint = path[i];
        std::optional<CCPoint> nextPoint;
        if (i != 0) previousPoint = path[i - 1];
        if (i != path.size() - 1) nextPoint = path[i + 1];

        // Extend the end caps slightly to hide artifacts, but never push the
        // trail visibly past the player.
        if (!previousPoint) {
            currentPoint -= (*nextPoint - currentPoint).normalize() * extensionLength;
        } else if (!nextPoint) {
            const float length = previousPoint->getDistance(currentPoint);
            currentPoint += (currentPoint - *previousPoint).normalize() *
                            ((length > extensionLength + 5.0f) ? -5.0f
                                                               : extensionLength - length);
        }

        const CCPoint point = offsetPoint(previousPoint, currentPoint, nextPoint, offset);
        points.push_back(point);
        if (!previousPoint) continue;
        const CCPoint actualDirection = (currentPoint - *previousPoint).normalize();
        CCPoint trailDirection = point - points[points.size() - 2];
        if (trailDirection.isZero()) {
            labels.push_back(true);
            continue;
        }
        trailDirection = trailDirection.normalize();
        labels.push_back(actualDirection.dot(trailDirection) < -1.0f + 0.000001f ? false : true);
    }
    return {points, labels};
}

// Remove self-intersections introduced by the offset on U-turns.
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

// ---------------------------------------------------------------------------
// Trail configuration. The default draws the player-colored core plus a white
// outline only while the streak is non-solid (the vanilla "hollow" look).
// ---------------------------------------------------------------------------
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

} // namespace skk::wavetrail

// ---------------------------------------------------------------------------
// DRAW FIX — HardStreak::updateStroke override.
// ---------------------------------------------------------------------------
class $modify(WTSHardStreak, HardStreak) {
    struct Fields {
        std::vector<skk::wavetrail::Part> configuration;
    };

    // CCDrawNode::drawTriangle is not exported in this SDK build, so the
    // vertices are pushed into the shared buffer directly (same approach as
    // the reference draw-fix mod).
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
        if (!skk::wavetrail::drawEnabled()) {
            HardStreak::updateStroke(dt);
            return;
        }

        // Vanilla draws the streak as a single solid band; we rebuild the same
        // vertex buffer with our sub-part configuration (miter-joined bands).
        this->clear();
        if (!m_drawStreak) return;
        if (m_pointArray->count() == 0) return;
        if (getOpacity() == 0) return;

        const CCPoint position = getPosition();
        std::vector<CCPoint> trailPath;
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

            std::vector<CCPoint> previousPoints, nextPoints, firstPoints, lastPoints;
            std::vector<bool> previousLabels, nextLabels, firstLabels, lastLabels;
            float previousOffset = 0.0f, nextOffset = 0.0f;

            std::tie(firstPoints, firstLabels) =
                skk::wavetrail::createOffset(trailPath, firstOffset, relativeFactor);
            std::tie(lastPoints, lastLabels) =
                skk::wavetrail::createOffset(trailPath, lastOffset, relativeFactor);
            previousPoints = firstPoints;
            previousLabels = firstLabels;
            previousOffset = firstOffset;
            (void)previousLabels;
            (void)nextLabels;
            (void)firstLabels;
            (void)lastLabels;

            skk::wavetrail::fixPoints(trailPath, previousOffset, previousPoints,
                                      previousLabels, lastPoints);

            totalWeight = part.subparts[0].weight;
            for (size_t i = 1; i < part.subparts.size() + 1; i++) {
                nextOffset = (i == part.subparts.size())
                                 ? lastOffset
                                 : (firstOffset + totalWeight * oneWidth);
                std::tie(nextPoints, nextLabels) =
                    skk::wavetrail::createOffset(trailPath, nextOffset, relativeFactor);
                skk::wavetrail::fixPoints(trailPath, nextOffset, nextPoints, nextLabels,
                                          firstPoints);

                const auto& sub = part.subparts[i - 1];
                const bool applies =
                    (!sub.solidOnly && !sub.nonSolidOnly) ||
                    (sub.solidOnly && m_isSolid) ||
                    (sub.nonSolidOnly && !m_isSolid);
                if (applies) {
                    const ccColor4B color = ccc4(
                        sub.color.value_or(getColor()).r,
                        sub.color.value_or(getColor()).g,
                        sub.color.value_or(getColor()).b,
                        static_cast<uint8_t>(std::clamp(
                            sub.opacity * getOpacity(), 0.0f, 255.0f)));
                    for (size_t j = 0; j + 1 < previousPoints.size(); j++) {
                        if (previousPoints[j] != previousPoints[j + 1]) {
                            drawTriangle(previousPoints[j], previousPoints[j + 1],
                                         nextPoints[j + 1], color);
                        }
                        if (nextPoints[j] != nextPoints[j + 1]) {
                            drawTriangle(nextPoints[j + 1], nextPoints[j],
                                         previousPoints[j], color);
                        }
                    }
                }

                previousOffset = nextOffset;
                previousPoints = nextPoints;
                previousLabels = nextLabels;

                if (i == part.subparts.size()) break;
                totalWeight += part.subparts[i].weight;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// DRAG FIX — rebuild streak points from updateRotation.
// ---------------------------------------------------------------------------
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

    bool preSlopeCollision(float p0, GameObject* p1) {
        const bool value = PlayerObject::preSlopeCollision(p0, p1);
        if (!value) m_fields->collidedSlope = p1;
        return value;
    }

    void update(float deltaFactor) {
        m_fields->currentPos = getRealPosition();
        m_fields->gotHereFromUpdate = true;
        PlayerObject::update(deltaFactor);
        m_fields->nextPosNoCollision = getRealPosition();
        m_fields->collidedSlope = nullptr;
        m_fields->deltaFactor = deltaFactor;
    }

    void updateRotation(float p0) {
        PlayerObject::updateRotation(p0);

        if (!skk::wavetrail::dragEnabled() || !skk::wavetrail::trailContextActive() ||
            !m_fields->gotHereFromUpdate) {
            return;
        }
        m_fields->gotHereFromUpdate = false;

        // Skip the level editor's build mode — during a playtest the editor
        // layer is live, so the drag fix runs there too. m_playbackMode is
        // PlaybackMode::Playing (nonzero) while playtesting.
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

        const CCPoint currentVector = (nextPosition - currentPosition).normalize();
        const CCPoint previousVector = (currentPosition - previousPosition).normalize();
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
            const float intercept =
                (nextPositionNoCollision.y - currentPosition.y) /
                (nextPositionNoCollision.x - currentPosition.x);
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

    // Add a point, dropping the previous one if the three are (almost) colinear.
    inline void addWaveTrailPoint(CCPoint point) {
        if (!m_waveTrail || !m_waveTrail->m_pointArray) return;
        const unsigned objectCount = static_cast<unsigned>(m_waveTrail->m_pointArray->count());
        if (objectCount >= 3) {
            auto* arr = m_waveTrail->m_pointArray;
            const CCPoint point0 =
                static_cast<PointNode*>(arr->objectAtIndex(objectCount - 2))->m_point;
            const CCPoint point1 =
                static_cast<PointNode*>(arr->objectAtIndex(objectCount - 1))->m_point;
            const CCPoint vector01 = (point1 - point0).normalize();
            const CCPoint vector12 = (point - point1).normalize();
            const float cpm = std::abs(vector12.cross(vector01));
            if (cpm <= 0.001f) arr->removeObjectAtIndex(objectCount - 1);
        }
        m_waveTrail->addPoint(point);
    }

    // Spider orb.
    bool pushButton(PlayerButton button) {
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

    // Spider pad.
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

    void placeStreakPoint() {
        if (skk::wavetrail::dragEnabled() && m_isDart && m_gameLayer) return;
        PlayerObject::placeStreakPoint();
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
        if (!skk::wavetrail::dragEnabled() || !state || playerTouchingPortal != m_player2) {
            GJBaseGameLayer::toggleDualMode(portal, state, playerTouchingPortal, p4);
            return;
        }
        // Player 2 becomes player 1 after the original executes, so carry the
        // drag-fix state across the swap.
        WTSPlayerObject::Fields fieldsBackup =
            *static_cast<WTSPlayerObject*>(m_player2)->m_fields.operator->();
        GJBaseGameLayer::toggleDualMode(portal, state, playerTouchingPortal, p4);
        *static_cast<WTSPlayerObject*>(m_player1)->m_fields.operator->() = fieldsBackup;
    }
};
