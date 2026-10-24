// Layout Mode — strips a level down to its gameplay layout ("blueprint mode"):
// decorations and colour triggers disappear, every object loses its authored
// colours and glow, and the background / ground / line / middleground are
// repainted to a flat palette so only the geometry and hitboxes read.
//
// NOT an interface scale. There is no such feature under this name anywhere in
// the reference set; the UI-scale knobs there are called "Menu Scale"
// (Neverhook framework_gui.cpp:460-466), "UI Scale" (silicate
// manager.cpp:2582-2586), "interface.ui-scale" (EclipseMenu main.cpp:166) and
// "Text size" (GucciBot shell.cpp:610). Those are a different feature.
//
// Reference implementations, all three ported from the same GDH original
// (EclipseMenu credits it: LayoutMode.cpp:13-14 "thanks toby for permission!"):
//   GDH          src/hacks/level/layout_mode.cpp:7  GUI "Layout Mode",
//                "Removes all decoration and colour from levels", 3 hooks:
//                  PlayLayer::addObject            :72-91
//                  GameObject::addGlow             :99-115
//                  GJBaseGameLayer::updateColor    :123-142
//   EclipseMenu  src/hacks/Level/LayoutMode.cpp:18-45 same hooks + per-colour
//                overrides ("level.layoutmode.override-*" / ".background" etc.)
//   Neverhook    src/hooks/LayoutMode.cpp:20-73 same three hooks, hardcoded
//                colours at :62-67
//
// Deviations from GDH, all deliberate:
//
// 1. NO 3 382-entry deco id table and NO object id table. GDH hides known
//    decoration ids by brute-force lookup. This build classifies by the
//    semantic flags GD already maintains — m_objectType == Decoration and
//    m_isNoTouch — which is the rule GDH's own addGlow hook applies
//    (layout_mode.cpp:111-114). That covers the same objects without shipping a
//    hardcoded table that would go stale the moment GD adds a decoration, and
//    avoids lifting a 3 000-line list from GPL-uncoded reference source.
//
// 2. Colour triggers are hidden by their object TYPE (EffectGameObject is not
//    used — they are plain GameObjects with id 899/900 in GDH's table, i.e.
//    pure data), so this build drops them from addObject instead. Only the two
//    well-known ids are listed; GDH's comment at layout_mode.cpp:899 names 899
//    as "color trigger", 900 is its "pulse/color" sibling.
//
// 3. Colours are applied only while the mode is ON. GDH hooks addObject /
//    addGlow unconditionally and gates through its hack-enabled check inside
//    the handler; this build checks the flag first and touches nothing on the
//    normal path, so leaving the mode off costs a single bool test per object.
//
// 4. updateColor does NOT swallow the base call, and does not return early for
//    unknown ids — it overwrites the colour and then calls through. GDH's
//    version returns without calling the base when PlayLayer is null, which
//    skips GD's own colour bookkeeping; calling through unconditionally is
//    safe and keeps that state intact.
//
// The mode is purely visual: it never touches physics, input or replay state.
// Objects keep their collision boxes, so gameplay is unchanged and the level
// plays exactly as before with a different palette.

#include <Geode/Geode.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "logger/logger.hpp"
#include "skkBot.hpp"

using namespace geode::prelude;

namespace skk::layout_mode {

// Flat "blueprint" palette. Same values as GDH layout_mode.cpp:133-139 and
// EclipseMenu LayoutMode.cpp:49-53.
constexpr int kBackground = 1000;
constexpr int kGround     = 1001;
constexpr int kGroundAlt  = 1009;
constexpr int kLine       = 1002;
constexpr int kMiddle     = 1013;
constexpr int kMiddleAlt  = 1014;

const ccColor3B kBackgroundColor{40, 125, 255};
const ccColor3B kGroundColor{0, 102, 255};
const ccColor3B kLineColor{255, 255, 255};
const ccColor3B kMiddleColor{40, 125, 255};

// Colour-trigger object ids. 899 is named "color trigger" in GDH's own table
// comment (layout_mode.cpp:899); 900 is the paired colour/pulse trigger.
bool isColorTrigger(int objectId) {
    return objectId == 899 || objectId == 900;
}

bool on() {
    auto* engine = ReplayEngine::get();
    return engine && engine->layoutMode;
}

}  // namespace skk::layout_mode

// Strips authored colour/glow from every object and drops the ones that only
// exist to paint the level.
class $modify(LayoutModePlayLayer, PlayLayer) {
    void addObject(GameObject* obj) {
        if (!skk::layout_mode::on() || !obj) {
            PlayLayer::addObject(obj);
            return;
        }

        // Colour triggers are dropped entirely: letting them through would
        // overwrite the flat palette we are about to establish.
        if (skk::layout_mode::isColorTrigger(obj->m_objectID)) {
            obj->m_isHide = true;
            return;
        }

        obj->m_isHide = false;
        obj->m_hasNoGlow = true;
        obj->m_activeMainColorID = -1;
        obj->m_activeDetailColorID = -1;
        obj->m_detailUsesHSV = false;
        obj->m_baseUsesHSV = false;
        obj->setOpacity(255);

        PlayLayer::addObject(obj);
    }
};

// Decorations carry their glow through addGlow, so hiding only in addObject
// would leave them visible. Same classification GDH uses at layout_mode.cpp:111.
class $modify(LayoutModeGameObject, GameObject) {
    void addGlow(gd::string frame) {
        GameObject::addGlow(frame);

        if (!skk::layout_mode::on()) {
            return;
        }

        const bool decorative = m_objectType == GameObjectType::Decoration ||
                                m_isNoTouch ||
                                skk::layout_mode::isColorTrigger(m_objectID);
        m_isHide = decorative;
    }
};

// Repaints the level background / ground / line / middleground.
class $modify(LayoutModeGJBaseGameLayer, GJBaseGameLayer) {
    void updateColor(cocos2d::ccColor3B& color, float fadeTime, int colorID,
                     bool blending, float opacity, cocos2d::ccHSVValue& copyHSV,
                     int colorIDToCopy, bool copyOpacity,
                     EffectGameObject* callerObject, int unk1, int unk2) {
        if (skk::layout_mode::on()) {
            switch (colorID) {
                case skk::layout_mode::kBackground:
                case skk::layout_mode::kMiddle:
                case skk::layout_mode::kMiddleAlt:
                    color = skk::layout_mode::kMiddleColor;
                    break;
                case skk::layout_mode::kGround:
                case skk::layout_mode::kGroundAlt:
                    color = skk::layout_mode::kGroundColor;
                    break;
                case skk::layout_mode::kLine:
                    color = skk::layout_mode::kLineColor;
                    break;
                default:
                    break;
            }
        }

        // Always call through: skipping it would bypass GD's colour bookkeeping
        // and leave the layer's internal colour state stale.
        GJBaseGameLayer::updateColor(color, fadeTime, colorID, blending, opacity,
                                     copyHSV, colorIDToCopy, copyOpacity,
                                     callerObject, unk1, unk2);
    }
};