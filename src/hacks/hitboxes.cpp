// Hitbox overlay — visual-only debug draw of GD collision boxes.
//
// One CCDrawNode per GJBaseGameLayer, parented to m_objectLayer (Neverhook
// style: getObjectRect() already returns object-layer-space rects, so no
// transform sync is needed — silicate's syncToObjectLayer dance is only
// required when the node hangs off the layer root). Redrawn every frame from
// GJBaseGameLayer::updateVisibility, which both PlayLayer and the editor
// playtest run after GD recomputes the visible section bounds.
//
// Object iteration walks GD's own section grid (m_leftSectionIndex..
// m_rightSectionIndex x m_bottom..m_top with m_sectionSizes) — the same view
// culling GDH / GeodeMenu use, so the overlay stays cheap on big levels.
//
// Reference mods studied (D:\SKKVPN\GEODE_MODS\GDBOTS + GitHub):
//   GDH-main           show_hitboxes.cpp       section walk, dirty-flag guard
//   GeodeMenu          Hitboxes/HitboxNode.cpp classification, slope triangles
//   Neverhook          Hitbox.cpp              m_objectLayer parenting, taxonomy
//   silicate           assist/hitboxes.cpp     zoom-corrected line width
//   ToastyReplay       hacks/hitboxes.cpp      dirty-flag RAII, editor branch
//   Prism-Menu         — no hitbox draw at all (byte-patch only), nothing to copy.
//
// The feature is visual only: it never touches physics, input, or replay state.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/OBB2D.hpp>
#include <Geode/binding/GJGameState.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/cocos/draw_nodes/CCDrawNode.h>

#include "skkBot.hpp"
#include "logger/logger.hpp"

#include <algorithm>

using namespace geode::prelude;

namespace skk::hitboxes {
namespace {

// ---------------------------------------------------------------------------
// Palette (ToastyReplay / GeodeMenu defaults — outline + faint fill).
// ---------------------------------------------------------------------------
constexpr float kBorderWidth = 0.35f;
constexpr float kFillAlpha   = 0.20f;
constexpr unsigned int kCircleSegments = 28;

const ccColor4F kSolid     = ccc4f(0.05f, 0.20f, 0.95f, 1.0f);  // blue
const ccColor4F kHazard    = ccc4f(0.95f, 0.10f, 0.10f, 1.0f);  // red
const ccColor4F kPassable  = ccc4f(0.10f, 0.85f, 0.95f, 1.0f);  // cyan
const ccColor4F kInteract  = ccc4f(0.20f, 0.85f, 0.30f, 1.0f);  // green
const ccColor4F kPortal    = ccc4f(0.90f, 0.10f, 0.90f, 1.0f);  // magenta
const ccColor4F kPlayer    = ccc4f(0.95f, 0.10f, 0.10f, 1.0f);  // red
const ccColor4F kPlayerRot = ccc4f(0.53f, 0.03f, 0.03f, 1.0f);  // dark red
const ccColor4F kPlayerIn  = ccc4f(0.05f, 0.20f, 0.95f, 1.0f);  // blue

bool engineEnabled() {
    auto* engine = ReplayEngine::get();
    return engine && engine->showHitboxes;
}

// Filled vs outline-only is a separate sub-option (off by default — outline
// boxes are the readable baseline, like Toasty / Neverhook / silicate).
float fillAlpha() {
    auto* engine = ReplayEngine::get();
    return (engine && engine->hitboxFill) ? kFillAlpha : 0.0f;
}

// Visible when the user toggled it on, or when GD's own on-death hitbox flag
// (m_hitboxesOnDeath) fires while a player is dead — mirrors GeodeMenu's
// shouldHitboxesBeVisible so practice-mode deaths still show the killer box.
bool shouldBeVisible(GJBaseGameLayer* gl) {
    if (engineEnabled()) return true;
    if (gl->m_hitboxesOnDeath) {
        if (gl->m_player1 && gl->m_player1->m_isDead) return true;
        if (gl->m_player2 && gl->m_player2->m_isDead) return true;
    }
    return false;
}

enum class Kind { Skip, Solid, Hazard, Passable, Interact, Portal };

// Neverhook's taxonomy, extended with GeodeMenu's interactable list.
Kind classify(GameObject* o) {
    const auto t = o->m_objectType;

    if (t == GameObjectType::Hazard || t == GameObjectType::AnimatedHazard)
        return Kind::Hazard;

    if (o->m_isPassable || t == GameObjectType::Breakable)
        return Kind::Passable;

    if (t == GameObjectType::Solid || t == GameObjectType::Slope)
        return Kind::Solid;

    // Vehicle + gravity + size + mirror portals get their own color.
    // Size portals (RegularSizePortal=17 / MiniSizePortal=18) and mirror
    // portals (InverseMirrorPortal=14 / NormalMirrorPortal=15) were missing
    // here, so they fell through to Kind::Skip and drew nothing.
    if (t == GameObjectType::CubePortal || t == GameObjectType::ShipPortal ||
        t == GameObjectType::BallPortal || t == GameObjectType::UfoPortal ||
        t == GameObjectType::WavePortal || t == GameObjectType::RobotPortal ||
        t == GameObjectType::SpiderPortal || t == GameObjectType::SwingPortal ||
        t == GameObjectType::DualPortal || t == GameObjectType::SoloPortal ||
        t == GameObjectType::InverseGravityPortal ||
        t == GameObjectType::NormalGravityPortal ||
        t == GameObjectType::GravityTogglePortal ||
        t == GameObjectType::RegularSizePortal ||
        t == GameObjectType::MiniSizePortal ||
        t == GameObjectType::InverseMirrorPortal ||
        t == GameObjectType::NormalMirrorPortal)
        return Kind::Portal;

    // Orbs, pads, rings, coins, speed portals (Modifier).
    if (t == GameObjectType::SecretCoin || t == GameObjectType::UserCoin ||
        t == GameObjectType::Collectible ||
        t == GameObjectType::YellowJumpPad || t == GameObjectType::PinkJumpPad ||
        t == GameObjectType::GravityPad || t == GameObjectType::RedJumpPad ||
        t == GameObjectType::SpiderPad ||
        t == GameObjectType::YellowJumpRing || t == GameObjectType::PinkJumpRing ||
        t == GameObjectType::GravityRing || t == GameObjectType::GreenRing ||
        t == GameObjectType::DropRing || t == GameObjectType::RedJumpRing ||
        t == GameObjectType::CustomRing || t == GameObjectType::DashRing ||
        t == GameObjectType::GravityDashRing || t == GameObjectType::SpiderOrb ||
        t == GameObjectType::TeleportOrb || t == GameObjectType::TeleportPortal ||
        t == GameObjectType::Modifier)
        return Kind::Interact;

    return Kind::Skip;
}

const ccColor4F& colorFor(Kind k) {
    switch (k) {
        case Kind::Solid:      return kSolid;
        case Kind::Hazard:     return kHazard;
        case Kind::Passable:   return kPassable;
        case Kind::Interact: return kInteract;
        case Kind::Portal:     return kPortal;
        default:               return kSolid;
    }
}

// getObjectRect() recomputes when dirty and can leave m_isObjectRectDirty /
// m_boxOffsetCalculated flipped (GD caches rect state off them). Every bot
// guards this — a stale flip makes GD's own collision rect cache go wrong.
// Neverhook's fast path: read the cached m_objectRect directly when clean.
CCRect objectRect(GameObject* o) {
    if (!o->m_isObjectRectDirty) return o->m_objectRect;
    const bool saveDirty = o->m_isObjectRectDirty;
    const bool saveBox = o->m_boxOffsetCalculated;
    const CCRect r = o->getObjectRect();
    o->m_isObjectRectDirty = saveDirty;
    o->m_boxOffsetCalculated = saveBox;
    return r;
}

bool shouldDrawObject(GameObject* o) {
    if (o->m_isDecoration) return false;
    if (!o->m_isActivated) return false;
    if (o->m_isGroupDisabled) return false;
    return classify(o) != Kind::Skip;
}

void drawPolygonFromRect(CCDrawNode* node, const CCRect& r, const ccColor4F& col) {
    CCPoint verts[4] = {
        ccp(r.getMinX(), r.getMinY()),
        ccp(r.getMaxX(), r.getMinY()),
        ccp(r.getMaxX(), r.getMaxY()),
        ccp(r.getMinX(), r.getMaxY()),
    };
    node->drawPolygon(verts, 4, ccc4f(col.r, col.g, col.b, fillAlpha()),
                      kBorderWidth, col, cocos2d::BorderAlignment::Inside);
}

void drawRotatedPolygonFromRect(CCDrawNode* node, const CCRect& r, float rotation, const ccColor4F& col) {
    if (std::abs(rotation) <= 0.01f) {
        drawPolygonFromRect(node, r, col);
        return;
    }
    CCPoint verts[4] = {
        ccp(r.getMinX(), r.getMinY()),
        ccp(r.getMaxX(), r.getMinY()),
        ccp(r.getMaxX(), r.getMaxY()),
        ccp(r.getMinX(), r.getMaxY()),
    };
    const CCPoint center = ccp(r.getMidX(), r.getMidY());
    const float rad = -CC_DEGREES_TO_RADIANS(rotation);
    for (auto& v : verts) {
        v = v.rotateByAngle(center, rad);
    }
    node->drawPolygon(verts, 4, ccc4f(col.r, col.g, col.b, fillAlpha()),
                      kBorderWidth, col, cocos2d::BorderAlignment::Inside);
}

void drawObjectHitbox(CCDrawNode* node, GameObject* o, bool isEditor) {
    const Kind k = classify(o);
    if (k == Kind::Skip) return;
    const ccColor4F col = colorFor(k);

    // Spikes/hazards collide as a circle: m_objectRadius scaled by the bigger
    // axis (GDH / GeodeMenu / Neverhook all do this).
    if (o->m_objectRadius != 0.0f) {
        const float radius = o->m_objectRadius *
                             std::max(o->m_scaleX, o->m_scaleY);
        node->drawCircle(ccp(o->m_positionX, o->m_positionY), radius,
                         ccc4f(0.0f, 0.0f, 0.0f, 0.0f), kBorderWidth, col,
                         kCircleSegments);
        return;
    }

    const CCRect r = objectRect(o);

    // Slopes collide as a triangle, picked by m_slopeDirection (GeodeMenu's
    // geometry — GDH / Eclipse use the identical switch).
    if (o->m_objectType == GameObjectType::Slope) {
        CCPoint verts[3] = {
            ccp(r.getMaxX(), r.getMinY()),  // br
            ccp(r.getMinX(), r.getMaxY()),  // tl
            ccp(r.getMinX(), r.getMinY()),  // bl
        };
        switch (o->m_slopeDirection) {
            case 0: case 7:
                verts[1] = ccp(r.getMaxX(), r.getMaxY());
                break;
            case 3: case 6:
                verts[0] = ccp(r.getMaxX(), r.getMaxY());
                break;
            case 1: case 5:
                verts[2] = ccp(r.getMinX(), r.getMaxY());
                verts[1] = ccp(r.getMaxX(), r.getMaxY());
                verts[0] = ccp(r.getMaxX(), r.getMinY());
                break;
            default:
                break;
        }
        node->drawPolygon(verts, 3, ccc4f(col.r, col.g, col.b, fillAlpha()),
                          kBorderWidth, col, cocos2d::BorderAlignment::Inside);
        return;
    }

    // Rotated objects use the oriented box. In play GD keeps m_orientedBox
    // populated; in the editor the cached box can be stale so recompute
    // (ToastyReplay / GDH branch the same way).
    if (o->m_shouldUseOuterOb ||
        (o->getRotation() != 0.0f && (static_cast<int>(o->getRotation()) % 90) != 0)) {
        OBB2D* ob = isEditor ? o->getOrientedBox() : o->m_orientedBox;
        if (ob) {
            node->drawPolygon(ob->m_corners.data(), 4,
                              ccc4f(col.r, col.g, col.b, fillAlpha()),
                              kBorderWidth, col, cocos2d::BorderAlignment::Inside);
            return;
        }
    }

    drawPolygonFromRect(node, r, col);
}

void drawPlayerHitbox(CCDrawNode* node, PlayerObject* p) {
    if (!p) return;

    if (p->m_isDart) {
        // Wave (dart): GD's exact hitbox is 10x10 for normal, 6x6 for mini.
        // Rotated box for slopes/rotated hazards + AABB for straight hazards.
        // Wave has no inner 0.3x box in GD physics.
        const float wSize = (p->m_vehicleSize != 1.0f) ? 6.0f : 10.0f;
        const CCPoint center = p->getPosition();
        const CCRect waveRect(center.x - wSize * 0.5f, center.y - wSize * 0.5f, wSize, wSize);
        drawRotatedPolygonFromRect(node, waveRect, p->getRotation(), kPlayerRot);
        drawPolygonFromRect(node, waveRect, kPlayer);
        return;
    }

    if (p->m_isOrientedBoxDirty) {
        p->updateOrientedBox();
    }

    // Rotated box (dart / slope rotation) first so the outer rect draws on top.
    if (p->m_orientedBox) {
        node->drawPolygon(p->m_orientedBox->m_corners.data(), 4,
                          ccc4f(kPlayerRot.r, kPlayerRot.g, kPlayerRot.b, fillAlpha()),
                          kBorderWidth, kPlayerRot, cocos2d::BorderAlignment::Inside);
    }

    drawPolygonFromRect(node, p->getObjectRect(), kPlayer);

    // Inner box — the actual landing/collision core GD checks.
    drawPolygonFromRect(node, p->getObjectRect(0.3f, 0.3f), kPlayerIn);
}

// GD's own visible-section grid walk (GDH show_hitboxes.cpp:206-223,
// GeodeMenu HitboxNode::forEachObject). Skips the collision blocks GD keeps
// in the section arrays and the anticheat spike.
void forEachVisibleObject(GJBaseGameLayer* gl,
                          const std::function<void(GameObject*)>& cb) {
    if (!cb || gl->m_sections.empty() || gl->m_sectionSizes.empty()) return;
    const int colCount = std::min(static_cast<int>(gl->m_sections.size()),
                                  static_cast<int>(gl->m_sectionSizes.size()));
    const int leftIdx = std::max(0, gl->m_leftSectionIndex);
    const int rightIdx = std::min(gl->m_rightSectionIndex, colCount - 1);

    for (int i = leftIdx; i <= rightIdx; ++i) {
        auto* col = gl->m_sections[i];
        auto* sizeRow = gl->m_sectionSizes[i];
        if (!col || !sizeRow) continue;
        const int rowCount = std::min(static_cast<int>(col->size()),
                                      static_cast<int>(sizeRow->size()));
        const int bottomIdx = std::max(0, gl->m_bottomSectionIndex);
        const int topIdx = std::min(gl->m_topSectionIndex, rowCount - 1);

        for (int j = bottomIdx; j <= topIdx; ++j) {
            auto* section = col->at(j);
            if (!section) continue;
            const int n = std::min(sizeRow->at(j), static_cast<int>(section->size()));
            for (int m = 0; m < n; ++m) {
                auto* obj = section->at(m);
                if (!obj) continue;
                if (obj == gl->m_player1CollisionBlock) continue;
                if (obj == gl->m_player2CollisionBlock) continue;
                if (obj == gl->m_anticheatSpike) continue;
                cb(obj);
            }
        }
    }
}

void redraw(GJBaseGameLayer* gl, CCDrawNode* node) {
    node->clear();
    if (!node->isVisible()) return;

    const bool isEditor = gl->m_isEditor;
    forEachVisibleObject(gl, [node, isEditor](GameObject* o) {
        if (shouldDrawObject(o)) drawObjectHitbox(node, o, isEditor);
    });
    drawPlayerHitbox(node, gl->m_player1);
    // P2 only when it is actually running — a dormant P2 sits at (0,0)
    // detached from the layer (see BUG-21), drawing a stray box at the origin.
    if (gl->m_player2 && gl->m_player2->isRunning())
        drawPlayerHitbox(node, gl->m_player2);
}

}  // namespace

// ---------------------------------------------------------------------------
// Hook entry: rebuild the overlay every time GD recomputes section visibility.
// The node is created lazily (the object layer does not exist until the level
// setup finishes) and re-attached if it lost its parent.
//
// GJBaseGameLayer::updateVisibility is inline (empty) and thus not hookable in
// this bindings build, so the tick is attached to both concrete layers that
// implement it — PlayLayer and the editor playtest (LevelEditorLayer), which
// is also how GDH / EclipseMenu drive their overlays.
// ---------------------------------------------------------------------------
CCDrawNode* getOrCreateNode(GJBaseGameLayer* gl, Ref<CCDrawNode>& nodeRef, const char* id) {
    if (!gl) return nullptr;
    CCNode* desiredParent = gl->m_objectLayer ? static_cast<CCNode*>(gl->m_objectLayer) : static_cast<CCNode*>(gl);

    if (nodeRef) {
        if (nodeRef->getParent() != desiredParent) {
            if (nodeRef->getParent()) {
                nodeRef->removeFromParentAndCleanup(false);
            }
            desiredParent->addChild(nodeRef, 9999);
        }
        return nodeRef;
    }

    auto* n = CCDrawNode::create();
    n->setBlendFunc(ccBlendFunc{GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    n->m_bUseArea = false;
    n->setID(id);
    desiredParent->addChild(n, 9999);
    nodeRef = n;
    return n;
}

void updateHitboxes(GJBaseGameLayer* gl, CCDrawNode* node) {
    if (!node) return;
    bool visible = shouldBeVisible(gl);
    static bool s_lastVisible = false;
    if (visible != s_lastVisible) {
        s_lastVisible = visible;
        skkbot2::log::info("[HITBOX] overlay visibility changed -> {}", visible ? "ENABLED" : "DISABLED");
    }
    node->setVisible(visible);
    if (!visible) {
        node->clear();
        return;
    }
    redraw(gl, node);
}

class $modify(HitboxPlayLayer, PlayLayer) {
    struct Fields {
        Ref<CCDrawNode> node = nullptr;
    };

    void createObjectsFromSetupFinished() {
        PlayLayer::createObjectsFromSetupFinished();
        auto* n = getOrCreateNode(this, m_fields->node, "skk-hitboxes"_spr);
        updateHitboxes(this, n);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto* n = getOrCreateNode(this, m_fields->node, "skk-hitboxes"_spr);
        updateHitboxes(this, n);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto* n = getOrCreateNode(this, m_fields->node, "skk-hitboxes"_spr);
        updateHitboxes(this, n);
    }

    void updateVisibility(float dt) {
        PlayLayer::updateVisibility(dt);
        auto* n = getOrCreateNode(this, m_fields->node, "skk-hitboxes"_spr);
        updateHitboxes(this, n);
    }
};

class $modify(HitboxEditorLayer, LevelEditorLayer) {
    struct Fields {
        Ref<CCDrawNode> node = nullptr;
    };

    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        auto* n = getOrCreateNode(this, m_fields->node, "skk-hitboxes-editor"_spr);
        updateHitboxes(this, n);
        return true;
    }

    void postUpdate(float dt) {
        LevelEditorLayer::postUpdate(dt);
        auto* n = getOrCreateNode(this, m_fields->node, "skk-hitboxes-editor"_spr);
        updateHitboxes(this, n);
    }

    void updateVisibility(float dt) {
        LevelEditorLayer::updateVisibility(dt);
        auto* n = getOrCreateNode(this, m_fields->node, "skk-hitboxes-editor"_spr);
        updateHitboxes(this, n);
    }
};

void forceRefresh() {
    if (auto* pl = PlayLayer::get()) {
        auto* modifyPl = static_cast<HitboxPlayLayer*>(pl);
        if (modifyPl) {
            auto* n = getOrCreateNode(pl, modifyPl->m_fields->node, "skk-hitboxes"_spr);
            updateHitboxes(pl, n);
        }
    } else if (auto* el = LevelEditorLayer::get()) {
        auto* modifyEl = static_cast<HitboxEditorLayer*>(el);
        if (modifyEl) {
            auto* n = getOrCreateNode(el, modifyEl->m_fields->node, "skk-hitboxes-editor"_spr);
            updateHitboxes(el, n);
        }
    }
}

}  // namespace skk::hitboxes
