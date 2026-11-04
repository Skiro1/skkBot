#pragma once
#include <vector>

namespace cocos2d {
class CCArray;
class CCNode;
}

enum class EffectSource : int {
    None,
    RingJump,
    RingCircle,
    DashStart,
    DashStop,
};

enum class EffectBatchRole : int {
    Plain,
    Foreign,
    SpriteMember,
};

struct EffectCopyScope {
    explicit EffectCopyScope(EffectSource source);
    ~EffectCopyScope();
    EffectCopyScope(const EffectCopyScope&) = delete;
    EffectCopyScope& operator=(const EffectCopyScope&) = delete;
    EffectSource outer_source;
};

void follow_wave_copies(cocos2d::CCNode* wave);
void follow_listed_wave_copies(cocos2d::CCArray* waves);
EffectBatchRole effect_batch_role(cocos2d::CCNode* node);
bool sprite_copy_order(cocos2d::CCNode* node, std::vector<cocos2d::CCNode*>& order);
void hold_copy_gl_state(bool hold);
bool same_wave(cocos2d::CCNode* first, cocos2d::CCNode* second);
void forget_effect_copies();
