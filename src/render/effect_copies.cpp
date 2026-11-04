#include "effect_copies.hpp"


#include <Geode/Geode.hpp>
#include <Geode/binding/CCCircleWave.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/CCActionManager.hpp>
#include <Geode/modify/CCNode.hpp>
#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <memory>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace geode::prelude;

using NodePairs = std::vector<std::pair<CCNode*, CCNode*>>;
using AddKey = std::pair<CCNode*, int>;

struct AddedChild {
    CCNode* parent;
    CCNode* child;
    EffectSource source;
};

struct NodeActions {
    CCNode* node;
    std::vector<CCAction*> actions;
};

struct CopyGroup {
    CCNode* parent = nullptr;
    std::vector<CCNode*> members;
    int copies = 0;
    int filled = 0;
    std::vector<CCNode*> targets;
    std::vector<CCPoint> positions;
};

struct EffectMember {
    unsigned long long epoch = 0;
    EffectSource source = EffectSource::None;
    std::vector<NodeActions> actions;
    CopyGroup* group = nullptr;
    int index = 0;
};

struct AddChain {
    CopyGroup* group = nullptr;
    std::vector<CCNode*> singles;
};

static int g_scope_depth = 0;
static EffectSource g_source = EffectSource::None;
static std::vector<AddedChild> g_added;
static std::vector<NodeActions> g_new_actions;
static std::unordered_map<CCNode*, EffectMember> g_members;
static std::vector<std::unique_ptr<CopyGroup>> g_groups;
static std::map<AddKey, AddChain> g_chains;
static unsigned long long g_epoch = 1;
static bool g_drawing_copy = false;

struct IntervalFields : CCActionInterval {
    static bool first_tick(CCActionInterval* action) { return static_cast<IntervalFields*>(action)->m_bFirstTick; }
};

struct SequenceFields : CCSequence {
    static SequenceFields* of(CCAction* action) { return static_cast<SequenceFields*>(action); }
    CCFiniteTimeAction* action_at(int index) { return m_pActions[index]; }
    float split() { return m_split; }
    int last() { return m_last; }
};

struct SpawnFields : CCSpawn {
    static SpawnFields* of(CCAction* action) { return static_cast<SpawnFields*>(action); }
    CCFiniteTimeAction* one() { return m_pOne; }
    CCFiniteTimeAction* two() { return m_pTwo; }
};

struct RepeatFields : CCRepeat {
    static RepeatFields* of(CCAction* action) { return static_cast<RepeatFields*>(action); }
    bool same(RepeatFields* other) { return m_uTimes == other->m_uTimes && m_uTotal == other->m_uTotal && m_fNextDt == other->m_fNextDt && m_bActionInstant == other->m_bActionInstant; }
    CCFiniteTimeAction* inner() { return m_pInnerAction; }
};

struct RepeatForeverFields : CCRepeatForever {
    static RepeatForeverFields* of(CCAction* action) { return static_cast<RepeatForeverFields*>(action); }
    CCActionInterval* inner() { return m_pInnerAction; }
};

struct EaseFields : CCActionEase {
    static EaseFields* of(CCAction* action) { return static_cast<EaseFields*>(action); }
    CCActionInterval* inner() { return m_pInner; }
};

struct EaseRateFields : CCEaseRateAction {
    static EaseRateFields* of(CCAction* action) { return static_cast<EaseRateFields*>(action); }
    float rate() { return m_fRate; }
};

struct ScaleFields : CCScaleTo {
    static ScaleFields* of(CCAction* action) { return static_cast<ScaleFields*>(action); }
    bool same(ScaleFields* other) {
        return m_fScaleX == other->m_fScaleX && m_fScaleY == other->m_fScaleY && m_fStartScaleX == other->m_fStartScaleX && m_fStartScaleY == other->m_fStartScaleY &&
            m_fEndScaleX == other->m_fEndScaleX && m_fEndScaleY == other->m_fEndScaleY && m_fDeltaX == other->m_fDeltaX && m_fDeltaY == other->m_fDeltaY;
    }
};

struct FadeFields : CCFadeTo {
    static FadeFields* of(CCAction* action) { return static_cast<FadeFields*>(action); }
    bool same(FadeFields* other) { return m_toOpacity == other->m_toOpacity && m_fromOpacity == other->m_fromOpacity; }
};

struct TintFields : CCTintTo {
    static TintFields* of(CCAction* action) { return static_cast<TintFields*>(action); }
    bool same(TintFields* other) {
        return m_to.r == other->m_to.r && m_to.g == other->m_to.g && m_to.b == other->m_to.b && m_from.r == other->m_from.r && m_from.g == other->m_from.g && m_from.b == other->m_from.b;
    }
};

struct MoveFields : CCMoveTo {
    static MoveFields* of(CCAction* action) { return static_cast<MoveFields*>(action); }
    bool same_by(MoveFields* other) {
        return m_positionDelta.equals(other->m_positionDelta) && m_startPosition.equals(other->m_startPosition) && m_previousPosition.equals(other->m_previousPosition);
    }
    bool same_to(MoveFields* other) { return same_by(other) && m_endPosition.equals(other->m_endPosition); }
};

struct TweenFields : CCActionTween {
    static TweenFields* of(CCAction* action) { return static_cast<TweenFields*>(action); }
    bool same(TweenFields* other) { return m_strKey == other->m_strKey && m_fFrom == other->m_fFrom && m_fTo == other->m_fTo && m_fDelta == other->m_fDelta; }
};

struct CallFields : CCCallFunc {
    static CallFields* of(CCAction* action) { return static_cast<CallFields*>(action); }
    CCObject* selector_target() { return m_pSelectorTarget; }
    bool same_call(CallFields* other) { return m_nScriptHandler == other->m_nScriptHandler && std::memcmp(&m_pCallFunc, &other->m_pCallFunc, sizeof(m_pCallFunc)) == 0; }
};

struct AnimateFields : CCAnimate {
    static AnimateFields* of(CCAction* action) { return static_cast<AnimateFields*>(action); }
    bool same(AnimateFields* other) {
        if (m_nNextFrame != other->m_nNextFrame || m_pOrigFrame != other->m_pOrigFrame || m_uExecutedLoops != other->m_uExecutedLoops)
            return false;
        if (!m_pSplitTimes || !other->m_pSplitTimes) return m_pSplitTimes == other->m_pSplitTimes;
        return *m_pSplitTimes == *other->m_pSplitTimes;
    }
};

struct NodeFields : CCNode {
    static NodeFields* of(CCNode* node) { return static_cast<NodeFields*>(node); }
    bool plain() { return !m_pCamera && !m_pGrid && !m_pUserObject && !m_pUserData; }
};

template <class T>
static bool same_type(T* first, T* second) {
    return first && second && typeid(*first) == typeid(*second);
}

static bool mapped(CCObject* first, CCObject* second, const NodePairs& pairs) {
    if (first == second) return true;
    for (const auto& [mine, theirs] : pairs) {
        if (mine == first) return theirs == second;
    }
    return false;
}

static bool same_animation(CCAnimation* first, CCAnimation* second) {
    if (!first || !second) return first == second;
    if (first->getDelayPerUnit() != second->getDelayPerUnit() || first->getLoops() != second->getLoops() || first->getRestoreOriginalFrame() != second->getRestoreOriginalFrame() ||
        first->getTotalDelayUnits() != second->getTotalDelayUnits())
        return false;
    auto* first_frames = first->getFrames();
    auto* second_frames = second->getFrames();
    unsigned count = first_frames ? first_frames->count() : 0;
    if (count != (second_frames ? second_frames->count() : 0)) return false;
    for (unsigned at = 0; at < count; ++at) {
        auto* mine = static_cast<CCAnimationFrame*>(first_frames->objectAtIndex(at));
        auto* theirs = static_cast<CCAnimationFrame*>(second_frames->objectAtIndex(at));
        if (mine->getSpriteFrame() != theirs->getSpriteFrame() || mine->getDelayUnits() != theirs->getDelayUnits() || mine->getUserInfo() || theirs->getUserInfo())
            return false;
    }
    return true;
}

static bool same_action(CCAction* first, CCAction* second, const NodePairs& pairs);

static bool same_inner(CCAction* first, CCAction* second, const NodePairs& pairs) {
    if (!first || !second) return first == second;
    return same_action(first, second, pairs);
}

static bool same_action(CCAction* first, CCAction* second, const NodePairs& pairs) {
    if (!same_type(first, second)) return false;
    if (first->getTag() != second->getTag()) return false;
    if (!mapped(first->getOriginalTarget(), second->getOriginalTarget(), pairs) || !mapped(first->getTarget(), second->getTarget(), pairs))
        return false;
    if (auto* timed = dynamic_cast<CCFiniteTimeAction*>(first)) {
        if (timed->getDuration() != static_cast<CCFiniteTimeAction*>(second)->getDuration())
            return false;
    }
    if (auto* interval = dynamic_cast<CCActionInterval*>(first)) {
        auto* other = static_cast<CCActionInterval*>(second);
        if (interval->getElapsed() != other->getElapsed() || IntervalFields::first_tick(interval) != IntervalFields::first_tick(other))
            return false;
    }
    const auto& type = typeid(*first);
    if (type == typeid(CCSequence)) {
        auto* mine = SequenceFields::of(first);
        auto* theirs = SequenceFields::of(second);
        return mine->split() == theirs->split() && mine->last() == theirs->last() && same_inner(mine->action_at(0), theirs->action_at(0), pairs) &&
            same_inner(mine->action_at(1), theirs->action_at(1), pairs);
    }
    if (type == typeid(CCSpawn)) {
        auto* mine = SpawnFields::of(first);
        auto* theirs = SpawnFields::of(second);
        return same_inner(mine->one(), theirs->one(), pairs) && same_inner(mine->two(), theirs->two(), pairs);
    }
    if (type == typeid(CCRepeat))
        return RepeatFields::of(first)->same(RepeatFields::of(second)) && same_inner(RepeatFields::of(first)->inner(), RepeatFields::of(second)->inner(), pairs);
    if (type == typeid(CCRepeatForever))
        return same_inner(RepeatForeverFields::of(first)->inner(), RepeatForeverFields::of(second)->inner(), pairs);
    if (type == typeid(CCEaseIn) || type == typeid(CCEaseOut) || type == typeid(CCEaseInOut))
        return EaseRateFields::of(first)->rate() == EaseRateFields::of(second)->rate() && same_inner(EaseFields::of(first)->inner(), EaseFields::of(second)->inner(), pairs);
    if (type == typeid(CCEaseExponentialIn) || type == typeid(CCEaseExponentialOut) || type == typeid(CCEaseExponentialInOut) || type == typeid(CCEaseSineIn) ||
        type == typeid(CCEaseSineOut) || type == typeid(CCEaseSineInOut))
        return same_inner(EaseFields::of(first)->inner(), EaseFields::of(second)->inner(), pairs);
    if (type == typeid(CCScaleTo) || type == typeid(CCScaleBy))
        return ScaleFields::of(first)->same(ScaleFields::of(second));
    if (type == typeid(CCFadeTo)) return FadeFields::of(first)->same(FadeFields::of(second));
    if (type == typeid(CCFadeIn) || type == typeid(CCFadeOut) || type == typeid(CCDelayTime))
        return true;
    if (type == typeid(CCTintTo)) return TintFields::of(first)->same(TintFields::of(second));
    if (type == typeid(CCMoveBy)) return MoveFields::of(first)->same_by(MoveFields::of(second));
    if (type == typeid(CCMoveTo)) return MoveFields::of(first)->same_to(MoveFields::of(second));
    if (type == typeid(CCActionTween)) return TweenFields::of(first)->same(TweenFields::of(second));
    if (type == typeid(CCCallFunc))
        return mapped(CallFields::of(first)->selector_target(), CallFields::of(second)->selector_target(), pairs) && CallFields::of(first)->same_call(CallFields::of(second));
    if (type == typeid(CCAnimate))
        return same_animation(AnimateFields::of(first)->getAnimation(), AnimateFields::of(second)->getAnimation()) && AnimateFields::of(first)->same(AnimateFields::of(second));
    return false;
}

static bool same_color(const ccColor3B& first, const ccColor3B& second) {
    return first.r == second.r && first.g == second.g && first.b == second.b;
}

static bool same_sprite(CCSprite* first, CCSprite* second) {
    if (first->getBatchNode() || second->getBatchNode()) return false;
    if (first->getTexture() != second->getTexture() || !first->getTextureRect().equals(second->getTextureRect()) || first->isTextureRectRotated() != second->isTextureRectRotated())
        return false;
    if (first->isFlipX() != second->isFlipX() || first->isFlipY() != second->isFlipY() || !first->getOffsetPosition().equals(second->getOffsetPosition()))
        return false;
    auto first_blend = first->getBlendFunc();
    auto second_blend = second->getBlendFunc();
    if (first_blend.src != second_blend.src || first_blend.dst != second_blend.dst) return false;
    if (first->getOpacity() != second->getOpacity() || first->getDisplayedOpacity() != second->getDisplayedOpacity() || !same_color(first->getColor(), second->getColor()) ||
        !same_color(first->getDisplayedColor(), second->getDisplayedColor()))
        return false;
    if (first->isCascadeOpacityEnabled() != second->isCascadeOpacityEnabled() || first->isCascadeColorEnabled() != second->isCascadeColorEnabled() ||
        first->isOpacityModifyRGB() != second->isOpacityModifyRGB())
        return false;
    auto first_quad = first->getQuad();
    auto second_quad = second->getQuad();
    return std::memcmp(&first_quad, &second_quad, sizeof(first_quad)) == 0;
}

static bool same_node(CCNode* first, CCNode* second, NodePairs& pairs) {
    if (!same_type(first, second)) return false;
    const auto& type = typeid(*first);
    bool wave = type == typeid(CCCircleWave);
    if (!wave && type != typeid(CCSprite) && type != typeid(CCNode)) return false;
    if (!NodeFields::of(first)->plain() || !NodeFields::of(second)->plain()) return false;
    if (first->getZOrder() != second->getZOrder()) return false;
    if (first->getTag() != second->getTag()) return false;
    if (first->isVisible() != second->isVisible()) return false;
    if (first->isRunning() != second->isRunning()) return false;
    if (!first->getPosition().equals(second->getPosition())) return false;
    if (!first->getAnchorPoint().equals(second->getAnchorPoint())) return false;
    if (!first->getContentSize().equals(second->getContentSize())) return false;
    if (first->getScaleX() != second->getScaleX() || first->getScaleY() != second->getScaleY())
        return false;
    if (first->getRotationX() != second->getRotationX() || first->getRotationY() != second->getRotationY())
        return false;
    if (first->getSkewX() != second->getSkewX() || first->getSkewY() != second->getSkewY())
        return false;
    if (first->getVertexZ() != second->getVertexZ()) return false;
    if (first->isIgnoreAnchorPointForPosition() != second->isIgnoreAnchorPointForPosition())
        return false;
    if (first->getShaderProgram() != second->getShaderProgram()) return false;
    if (first->getGLServerState() != second->getGLServerState()) return false;
    if (wave && !same_wave(first, second)) return false;
    if (type == typeid(CCSprite) && !same_sprite(static_cast<CCSprite*>(first), static_cast<CCSprite*>(second)))
        return false;
    pairs.push_back({ first, second });
    auto* first_children = first->getChildren();
    auto* second_children = second->getChildren();
    unsigned count = first_children ? first_children->count() : 0;
    if (count != (second_children ? second_children->count() : 0)) return false;
    for (unsigned at = 0; at < count; ++at) {
        if (!same_node(static_cast<CCNode*>(first_children->objectAtIndex(at)), static_cast<CCNode*>(second_children->objectAtIndex(at)), pairs))
            return false;
    }
    return true;
}

static const std::vector<CCAction*>* actions_in(const std::vector<NodeActions>& list, CCNode* node) {
    for (const auto& entry : list) {
        if (entry.node == node) return &entry.actions;
    }
    return nullptr;
}

static bool same_actions(const std::vector<NodeActions>& recorded, const NodePairs& pairs) {
    static const std::vector<CCAction*> none;
    for (const auto& [mine, theirs] : pairs) {
        const auto* first = actions_in(recorded, mine);
        const auto* second = actions_in(g_new_actions, theirs);
        if (!first) first = &none;
        if (!second) second = &none;
        if (first->size() != second->size()) return false;
        if (mine->numberOfRunningActions() != first->size() || theirs->numberOfRunningActions() != second->size())
            return false;
        for (size_t at = 0; at < first->size(); ++at) {
            if (!same_action((*first)[at], (*second)[at], pairs)) return false;
        }
    }
    return true;
}

static void collect_actions(CCNode* node, std::vector<NodeActions>& out) {
    if (const auto* actions = actions_in(g_new_actions, node)) out.push_back({ node, *actions });
    auto* children = node->getChildren();
    unsigned count = children ? children->count() : 0;
    for (unsigned at = 0; at < count; ++at) collect_actions(static_cast<CCNode*>(children->objectAtIndex(at)), out);
}

static bool monotone_factor(GLint factor) {
    return factor != GL_ONE_MINUS_DST_ALPHA && factor != GL_ONE_MINUS_DST_COLOR && factor != GL_SRC_ALPHA_SATURATE && factor != GL_DST_ALPHA;
}

static bool tree_blends_monotone(CCNode* node) {
    if (auto* sprite = typeinfo_cast<CCSprite*>(node)) {
        auto blend = sprite->getBlendFunc();
        if (!monotone_factor(blend.src) || !monotone_factor(blend.dst)) return false;
    }
    auto* children = node->getChildren();
    unsigned count = children ? children->count() : 0;
    for (unsigned at = 0; at < count; ++at) {
        if (!tree_blends_monotone(static_cast<CCNode*>(children->objectAtIndex(at)))) return false;
    }
    return true;
}

static bool g_holding_gl_state = false;
static bool g_held_blend_known = false, g_held_blend_monotone = false;
static bool g_held_limit_known = false;
static int g_held_limit = 0;

void hold_copy_gl_state(bool hold) {
    g_holding_gl_state = hold;
    g_held_blend_known = false;
    g_held_limit_known = false;
}

static bool read_blend_monotone() {
    GLint src_rgb = 0, dst_rgb = 0, src_alpha = 0, dst_alpha = 0, equation = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation);
    return equation == GL_FUNC_ADD && monotone_factor(src_rgb) && monotone_factor(dst_rgb) && monotone_factor(src_alpha) && monotone_factor(dst_alpha);
}

static bool current_blend_monotone() {
    if (!g_holding_gl_state) return read_blend_monotone();
    if (!g_held_blend_known) g_held_blend_monotone = read_blend_monotone();
    g_held_blend_known = true;
    return g_held_blend_monotone;
}

static int read_copy_limit() {
    const int largest_counted_bits = 30;
    GLint red = 0, green = 0, blue = 0, alpha = 0;
    glGetIntegerv(GL_RED_BITS, &red);
    glGetIntegerv(GL_GREEN_BITS, &green);
    glGetIntegerv(GL_BLUE_BITS, &blue);
    glGetIntegerv(GL_ALPHA_BITS, &alpha);
    int bits = std::max({ red, green, blue, alpha });
    if (bits <= 0 || bits > largest_counted_bits) return INT_MAX;
    return (1 << bits) - 1;
}

static int copy_limit() {
    if (!g_holding_gl_state) return read_copy_limit();
    if (!g_held_limit_known) g_held_limit = read_copy_limit();
    g_held_limit_known = true;
    return g_held_limit;
}

static bool group_alive(const CopyGroup& group) {
    for (auto* member : group.members) {
        if (member->getParent() == group.parent) return true;
    }
    return false;
}

static void release_group(CopyGroup& group) {
    for (auto* target : group.targets) {
        if (target) target->release();
    }
    for (auto* member : group.members) {
        auto found = g_members.find(member);
        if (found == g_members.end()) continue;
        g_members.erase(found);
        member->release();
    }
}

static void next_epoch() {
    ++g_epoch;
    g_chains.clear();
    for (auto entry = g_members.begin(); entry != g_members.end();) {
        entry->second.actions.clear();
        if (entry->second.group) {
            ++entry;
            continue;
        }
        entry->first->release();
        entry = g_members.erase(entry);
    }
    for (auto group_entry = g_groups.begin(); group_entry != g_groups.end();) {
        if (group_alive(**group_entry)) {
            ++group_entry;
            continue;
        }
        release_group(**group_entry);
        group_entry = g_groups.erase(group_entry);
    }
}

static void register_member(CCNode* node, EffectSource source) {
    auto [found, fresh] = g_members.try_emplace(node);
    if (fresh) node->retain();
    auto& member = found->second;
    member.epoch = g_epoch;
    member.source = source;
    member.actions.clear();
    collect_actions(node, member.actions);
}

static bool matches(CCNode* candidate, CCNode* child, EffectSource source) {
    auto found = g_members.find(candidate);
    if (found == g_members.end() || found->second.epoch != g_epoch || found->second.source != source)
        return false;
    NodePairs pairs;
    return same_node(candidate, child, pairs) && same_actions(found->second.actions, pairs);
}

static CCArray* wave_list() {
    auto* play = PlayLayer::get();
    return play ? play->m_circleWaveArray : nullptr;
}

static int index_from_end(CCArray* list, CCObject* object) {
    if (!list) return -1;
    for (int at = (int)list->count() - 1; at >= 0; --at) {
        if (list->objectAtIndex(at) == object) return at;
    }
    return -1;
}

static void detach_copy(CCNode* parent, CCNode* child) {
    if (typeid(*child) == typeid(CCCircleWave)) {
        auto* waves = wave_list();
        int at = index_from_end(waves, child);
        if (at >= 0) waves->removeObjectAtIndex(at, true);
    }
    if (parent->isRunning()) {
        child->onExitTransitionDidStart();
        child->onExit();
    }
    child->cleanup();
    child->setParent(nullptr);
    auto* children = parent->getChildren();
    if (parent->getUseChildIndex()) {
        children->removeObjectAtIndexChild(child->m_uIndexInArray, true);
        return;
    }
    int at = index_from_end(children, child);
    if (at >= 0) children->removeObjectAtIndex(at, true);
}

static CopyGroup* make_group(CCNode* parent, std::vector<CCNode*> members) {
    auto group = std::make_unique<CopyGroup>();
    group->parent = parent;
    group->members = std::move(members);
    for (int at = 0; at < (int)group->members.size(); ++at) {
        auto& member = g_members[group->members[at]];
        member.group = group.get();
        member.index = at;
    }
    g_groups.push_back(std::move(group));
    return g_groups.back().get();
}

static int next_member(const CopyGroup& group) {
    return group.copies == 0 || group.filled == (int)group.members.size() ? 0 : group.filled;
}

static void absorb(CopyGroup& group, CCNode* parent, CCNode* child) {
    int width = (int)group.members.size();
    if (group.copies == 0 || group.filled == width) {
        ++group.copies;
        group.filled = 0;
        group.targets.resize((size_t)group.copies * width, nullptr);
        group.positions.resize((size_t)group.copies * width);
    }
    size_t at = (size_t)(group.copies - 1) * width + group.filled;
    if (typeid(*child) == typeid(CCCircleWave)) {
        auto* wave = static_cast<CCCircleWave*>(child);
        if (wave->m_target) wave->m_target->retain();
        group.targets[at] = wave->m_target;
        group.positions[at] = wave->m_position;
    }
    ++group.filled;
    detach_copy(parent, child);
}

static bool copies_allowed() {
    return true;
}

static bool added_inside(CCNode* parent) {
    for (const auto& added : g_added) {
        if (added.child == parent) return true;
    }
    return false;
}

static void settle(const AddedChild& added) {
    const size_t longest_pattern = 8;
    auto* parent = added.parent;
    auto* child = added.child;
    if (added_inside(parent) || child->getParent() != parent) return;
    auto& chain = g_chains[{ parent, child->getZOrder() }];
    if (chain.group) {
        auto* group = chain.group;
        if (matches(group->members[next_member(*group)], child, added.source)) {
            absorb(*group, parent, child);
            return;
        }
        chain.group = nullptr;
    }
    int count = (int)chain.singles.size();
    for (int back = 1; back <= count; ++back) {
        if (!matches(chain.singles[count - back], child, added.source)) continue;
        auto* group = make_group(parent, std::vector<CCNode*>(chain.singles.end() - back, chain.singles.end()));
        chain.singles.clear();
        chain.group = group;
        absorb(*group, parent, child);
        return;
    }
    register_member(child, added.source);
    chain.singles.push_back(child);
    if (chain.singles.size() > longest_pattern) chain.singles.erase(chain.singles.begin());
}

static void settle_added() {
    if (copies_allowed()) {
        for (const auto& added : g_added) settle(added);
    }
    g_added.clear();
    g_new_actions.clear();
}

EffectCopyScope::EffectCopyScope(EffectSource source) : outer_source(g_source) {
    ++g_scope_depth;
    g_source = source;
}

EffectCopyScope::~EffectCopyScope() {
    g_source = outer_source;
    if (--g_scope_depth == 0) settle_added();
}

static int row_width(const CopyGroup& group, int row) {
    return row == group.copies - 1 ? group.filled : (int)group.members.size();
}

static bool same_rows(const CopyGroup& group, int first, int second) {
    size_t width = group.members.size();
    for (size_t at = 0; at < width; ++at) {
        if (!group.positions[first * width + at].equals(group.positions[second * width + at]))
            return false;
    }
    return true;
}

static int repeat_limit(const CopyGroup& group) {
    bool checked_blend = false;
    for (auto* member : group.members) {
        if (typeid(*member) != typeid(CCCircleWave)) {
            if (!tree_blends_monotone(member)) return INT_MAX;
            continue;
        }
        if (static_cast<CCCircleWave*>(member)->m_blendAdditive || checked_blend) continue;
        if (!current_blend_monotone()) return INT_MAX;
        checked_blend = true;
    }
    return copy_limit();
}

static void draw_row(const CopyGroup& group, int row, int width) {
    size_t base = (size_t)row * group.members.size();
    for (int at = 0; at < width; ++at) {
        auto* member = group.members[at];
        if (member->getParent() != group.parent) continue;
        bool wave = typeid(*member) == typeid(CCCircleWave);
        auto* circle = static_cast<CCCircleWave*>(member);
        CCPoint own = wave ? circle->m_position : CCPointZero;
        if (wave) circle->m_position = group.positions[base + at];
        g_drawing_copy = true;
        member->visit();
        g_drawing_copy = false;
        if (wave) circle->m_position = own;
    }
}

static void draw_copies(const CopyGroup& group) {
    int limit = repeat_limit(group);
    int width = (int)group.members.size();
    int full_rows = group.filled == width ? group.copies : group.copies - 1;
    int row = 0;
    while (row < full_rows) {
        int run = 1;
        while (row + run < full_rows && same_rows(group, row, row + run)) ++run;
        int times = std::min(run, limit);
        for (int time = 0; time < times; ++time) draw_row(group, row, width);
        row += run;
    }
    if (full_rows < group.copies) draw_row(group, full_rows, row_width(group, full_rows));
}

static void follow_column(CopyGroup& group, int column) {
    size_t width = group.members.size();
    for (int row = 0; row < group.copies; ++row) {
        if (column >= row_width(group, row)) continue;
        size_t at = row * width + column;
        if (group.targets[at]) group.positions[at] = group.targets[at]->getPosition();
    }
}

void follow_wave_copies(CCNode* wave) {
    if (g_groups.empty()) return;
    auto found = g_members.find(wave);
    if (found != g_members.end() && found->second.group)
        follow_column(*found->second.group, found->second.index);
}

void follow_listed_wave_copies(CCArray* waves) {
    if (g_groups.empty() || !waves) return;
    for (auto& group : g_groups) {
        for (int at = 0; at < (int)group->members.size(); ++at) {
            auto* member = group->members[at];
            if (typeid(*member) == typeid(CCCircleWave) && waves->containsObject(member))
                follow_column(*group, at);
        }
    }
}

static bool sprite_group(const CopyGroup& group) {
    for (auto* member : group.members) {
        if (typeid(*member) != typeid(CCSprite)) return false;
    }
    return true;
}

EffectBatchRole effect_batch_role(CCNode* node) {
    if (g_groups.empty()) return EffectBatchRole::Plain;
    auto found = g_members.find(node);
    if (found == g_members.end() || !found->second.group) return EffectBatchRole::Plain;
    return sprite_group(*found->second.group) ? EffectBatchRole::SpriteMember : EffectBatchRole::Foreign;
}

bool sprite_copy_order(CCNode* node, std::vector<CCNode*>& order) {
    order.clear();
    auto found = g_members.find(node);
    if (found == g_members.end() || !found->second.group) return false;
    auto& group = *found->second.group;
    int width = (int)group.members.size();
    for (int at = found->second.index + 1; at < width; ++at) {
        if (group.members[at]->getParent() == group.parent) return false;
    }
    int full_rows = group.filled == width ? group.copies : group.copies - 1;
    int times = std::min(full_rows, repeat_limit(group));
    for (int time = 0; time < times; ++time) {
        for (auto* member : group.members) {
            if (member->getParent() == group.parent) order.push_back(member);
        }
    }
    if (full_rows < group.copies) {
        for (int at = 0; at < group.filled; ++at) {
            if (group.members[at]->getParent() == group.parent) order.push_back(group.members[at]);
        }
    }
    return true;
}

void forget_effect_copies() {
    for (auto& group : g_groups) {
        for (auto* target : group->targets) {
            if (target) target->release();
        }
    }
    g_groups.clear();
    for (auto& [node, member] : g_members) node->release();
    g_members.clear();
    g_chains.clear();
    g_added.clear();
    g_new_actions.clear();
}

class $modify(EffectCopyActions, CCActionManager) {
    void update(float dt) {
        if (this == CCDirector::sharedDirector()->getActionManager()) next_epoch();
        CCActionManager::update(dt);
    }

    void addAction(CCAction* action, CCNode* target, bool paused) {
        CCActionManager::addAction(action, target, paused);
        if (g_scope_depth <= 0 || !action || !target) return;
        for (auto& entry : g_new_actions) {
            if (entry.node == target) {
                entry.actions.push_back(action);
                return;
            }
        }
        g_new_actions.push_back({ target, { action } });
    }
};

class $modify(EffectCopyNode, CCNode) {
    void addChild(CCNode* child, int z, int tag) {
        CCNode::addChild(child, z, tag);
        if (!child) return;
        if (g_scope_depth > 0) g_added.push_back({ this, child, g_source });
        else if (!g_chains.empty()) g_chains.erase({ this, z });
    }

    void visit() {
        if (g_drawing_copy || g_groups.empty()) {
            CCNode::visit();
            return;
        }
        CCNode::visit();
        auto found = g_members.find(this);
        if (found == g_members.end() || !found->second.group) return;
        auto& group = *found->second.group;
        for (int at = found->second.index + 1; at < (int)group.members.size(); ++at) {
            if (group.members[at]->getParent() == group.parent) return;
        }
        draw_copies(group);
    }
};

bool same_wave(cocos2d::CCNode* first, cocos2d::CCNode* second) { return first == second; }
