#include <Geode/Geode.hpp>
#include "moving_objects.hpp"
#if defined(GEODE_IS_WINDOWS) && defined(ECHO_THREADED_MOVE)
#include "skkBot.hpp"
#include "effect_copies.hpp"
#include <Geode/modify/CCNode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <windows.h>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;
using MovingObjects::read;

#define NODE_Z_ORDER_OFFSET 0x2c
#define NODE_VERTEX_Z_OFFSET 0x48
#define NODE_CAMERA_OFFSET 0xc0
#define NODE_GRID_OFFSET 0xc8
#define RGBA_REAL_OPACITY_OFFSET 0x149
#define SPRITE_QUAD_OFFSET 0x1cc
#define SPRITE_SKIP_DRAW_OFFSET 0x22f

struct BatchEntry {
    CCSprite* sprite;
    bool child;
    bool drawn;
};

struct EntryMatrices {
    kmMat4 view;
    kmMat4 view_projection;
};

struct BatchGroup {
    CCSprite* sprite;
    unsigned first_entry, entry_count;
    int copy_of;
};

struct BatchStep {
    CCNode* foreign;
    bool layer_draw;
    unsigned entry_from, entry_to;
};

static CCNode* g_batched_layer = nullptr;
static std::vector<BatchEntry> g_entries;
static std::vector<BatchGroup> g_groups;
static std::vector<BatchStep> g_steps;
static std::vector<EntryMatrices> g_matrices;
static std::vector<CCNode*> g_copy_order;
static std::unordered_map<CCNode*, int> g_source_groups;
static kmMat4 g_projection;
static kmMat4 g_layer_view;

static void* sprite_vtable() {
    static void* table = [] {
        auto* cocos = GetModuleHandleA("libcocos2d.dll");
        return cocos ? (void*)GetProcAddress(cocos, "??_7CCSprite@cocos2d@@6BCCNode@1@@") : nullptr;
    }();
    return table;
}

static CCGLProgram* sprite_program() {
    return CCShaderCache::sharedShaderCache()->programForKey(kCCShader_PositionTextureColor);
}

static bool plain_sprite(CCNode* node, CCGLProgram* program) {
    if (!node || read<void*>(node, 0) != sprite_vtable()) return false;
    if (read<void*>(node, NODE_CAMERA_OFFSET) || read<void*>(node, NODE_GRID_OFFSET)) return false;
    auto* sprite = static_cast<CCSprite*>(node);
    return !sprite->getBatchNode() && sprite->getShaderProgram() == program && sprite->getTexture();
}

static bool sprite_drawn(CCSprite* sprite) {
    return read<unsigned char>(sprite, SPRITE_SKIP_DRAW_OFFSET) == 0 && read<unsigned char>(sprite, RGBA_REAL_OPACITY_OFFSET) != 0;
}

static int z_order(CCNode* node) {
    return read<int>(node, NODE_Z_ORDER_OFFSET);
}

static bool gather_group(CCSprite* sprite, CCGLProgram* program) {
    const int max_sprite_children = 8;
    auto* children = sprite->getChildren();
    unsigned count = children ? children->count() : 0;
    if (count > max_sprite_children) return false;
    if (count > 0) {
        sprite->sortAllChildren();
        for (unsigned at = 0; at < count; ++at) {
            auto* child = static_cast<CCNode*>(children->objectAtIndex(at));
            if (!plain_sprite(child, program) || (child->getChildren() && child->getChildren()->count() > 0))
                return false;
        }
    }
    BatchGroup group{ sprite, (unsigned)g_entries.size(), 0, -1 };
    auto add = [&](CCSprite* node, bool child) {
        if (!node->isVisible()) return;
        g_entries.push_back({ node, child, sprite_drawn(node) });
    };
    for (unsigned at = 0; at < count; ++at) {
        auto* child = static_cast<CCSprite*>(children->objectAtIndex(at));
        if (z_order(child) < 0) add(child, true);
    }
    add(sprite, false);
    for (unsigned at = 0; at < count; ++at) {
        auto* child = static_cast<CCSprite*>(children->objectAtIndex(at));
        if (z_order(child) >= 0) add(child, true);
    }
    group.entry_count = (unsigned)g_entries.size() - group.first_entry;
    g_source_groups[sprite] = (int)g_groups.size();
    g_groups.push_back(group);
    return true;
}

static int group_of(CCNode* sprite) {
    auto found = g_source_groups.find(sprite);
    return found == g_source_groups.end() ? -1 : found->second;
}

static bool gather_copies(CCNode* last_member) {
    if (!sprite_copy_order(last_member, g_copy_order)) return true;
    for (auto* member : g_copy_order) {
        if (!member->isVisible()) continue;
        int source = group_of(member);
        if (source < 0) return false;
        BatchGroup copy = g_groups[source];
        unsigned first = (unsigned)g_entries.size();
        for (unsigned at = 0; at < copy.entry_count; ++at) g_entries.push_back(g_entries[copy.first_entry + at]);
        g_groups.push_back({ copy.sprite, first, copy.entry_count, source });
    }
    return true;
}

static bool gather_child(CCNode* child, CCGLProgram* program) {
    auto role = effect_batch_role(child);
    if (role == EffectBatchRole::Foreign || !plain_sprite(child, program)) return false;
    size_t entries_before = g_entries.size(), groups_before = g_groups.size();
    if (!gather_group(static_cast<CCSprite*>(child), program)) return false;
    if (role != EffectBatchRole::SpriteMember || gather_copies(child)) return true;
    g_entries.resize(entries_before);
    g_groups.resize(groups_before);
    g_source_groups.erase(child);
    return false;
}

static void gather(CCNode* layer) {
    g_entries.clear();
    g_groups.clear();
    g_steps.clear();
    g_source_groups.clear();
    auto* program = sprite_program();
    auto* children = layer->getChildren();
    unsigned count = children ? children->count() : 0;
    bool drew_layer = false;
    for (unsigned at = 0; at <= count; ++at) {
        CCNode* child = at < count ? static_cast<CCNode*>(children->objectAtIndex(at)) : nullptr;
        if (!drew_layer && (!child || z_order(child) >= 0)) {
            drew_layer = true;
            g_steps.push_back({ nullptr, true, 0, 0 });
        }
        if (!child) break;
        if (!child->isVisible()) continue;
        unsigned before = (unsigned)g_entries.size();
        if (gather_child(child, program)) {
            if (!g_steps.empty() && !g_steps.back().foreign && !g_steps.back().layer_draw)
                g_steps.back().entry_to = (unsigned)g_entries.size();
            else
                g_steps.push_back({ nullptr, false, before, (unsigned)g_entries.size() });
            continue;
        }
        g_steps.push_back({ child, false, 0, 0 });
    }
}

static void node_view(CCNode* node, const kmMat4& parent_view, kmMat4& view) {
    node->nodeToParentTransform();
    CCAffineTransform transform = node->getTransformTemp();
    kmMat4 local;
    std::memset(local.mat, 0, sizeof(local.mat));
    local.mat[0] = transform.a;
    local.mat[1] = transform.b;
    local.mat[4] = transform.c;
    local.mat[5] = transform.d;
    local.mat[10] = 1.f;
    local.mat[12] = transform.tx;
    local.mat[13] = transform.ty;
    local.mat[14] = read<float>(node, NODE_VERTEX_Z_OFFSET);
    local.mat[15] = 1.f;
    kmMat4Multiply(&view, &parent_view, &local);
}

static void place_group(const BatchGroup& group) {
    kmMat4 sprite_view;
    node_view(group.sprite, g_layer_view, sprite_view);
    for (unsigned at = group.first_entry; at < group.first_entry + group.entry_count; ++at) {
        const auto& entry = g_entries[at];
        auto& matrices = g_matrices[at];
        if (entry.child) node_view(entry.sprite, sprite_view, matrices.view);
        else matrices.view = sprite_view;
        kmMat4Multiply(&matrices.view_projection, &g_projection, &matrices.view);
    }
}

static void place_all() {
    g_matrices.resize(g_entries.size());
    size_t total = g_groups.size();
    auto job = [total](unsigned part) {
        size_t from = total * part / MovingObjects::MoveThreads::PARTS, to = total * (part + 1) / MovingObjects::MoveThreads::PARTS;
        for (size_t at = from; at < to; ++at) {
            if (g_groups[at].copy_of < 0) place_group(g_groups[at]);
        }
    };
    MovingObjects::MoveThreads::get().run(job);
    for (const auto& group : g_groups) {
        if (group.copy_of < 0) continue;
        const auto& source = g_groups[group.copy_of];
        for (unsigned at = 0; at < group.entry_count; ++at) g_matrices[group.first_entry + at] = g_matrices[source.first_entry + at];
    }
}

struct BuiltinLocations {
    CCGLProgram* program = nullptr;
    GLint projection = -1, view = -1, view_projection = -1;
};

static const BuiltinLocations& builtin_locations(CCGLProgram* program) {
    static BuiltinLocations locations;
    if (locations.program == program) return locations;
    locations.program = program;
    locations.projection = program->getUniformLocationForName(kCCUniformPMatrix_s);
    locations.view = program->getUniformLocationForName(kCCUniformMVMatrix_s);
    locations.view_projection = program->getUniformLocationForName(kCCUniformMVPMatrix_s);
    return locations;
}

static void draw_sprite(CCSprite* sprite, CCGLProgram* program, const BuiltinLocations& locations, EntryMatrices& matrices) {
    const int quad_vertices = 4;
    const int position_components = 3;
    const int tex_coord_components = 2;
    const int color_components = 4;
    ccGLEnable(sprite->getGLServerState());
    program->use();
    program->setUniformLocationWithMatrix4fv(locations.projection, g_projection.mat, 1);
    program->setUniformLocationWithMatrix4fv(locations.view, matrices.view.mat, 1);
    program->setUniformLocationWithMatrix4fv(locations.view_projection, matrices.view_projection.mat, 1);
    ccBlendFunc blend = sprite->getBlendFunc();
    ccGLBlendFunc(blend.src, blend.dst);
    ccGLBindTexture2D(sprite->getTexture()->getName());
    ccGLEnableVertexAttribs(kCCVertexAttribFlag_PosColorTex);
    auto* quad = reinterpret_cast<const char*>(sprite) + SPRITE_QUAD_OFFSET;
    glVertexAttribPointer(kCCVertexAttrib_Position, position_components, GL_FLOAT, GL_FALSE, sizeof(ccV3F_C4B_T2F), quad + offsetof(ccV3F_C4B_T2F, vertices));
    glVertexAttribPointer(kCCVertexAttrib_TexCoords, tex_coord_components, GL_FLOAT, GL_FALSE, sizeof(ccV3F_C4B_T2F), quad + offsetof(ccV3F_C4B_T2F, texCoords));
    glVertexAttribPointer(kCCVertexAttrib_Color, color_components, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ccV3F_C4B_T2F), quad + offsetof(ccV3F_C4B_T2F, colors));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, quad_vertices);
}

static void draw_entries(unsigned from, unsigned to) {
    auto* program = sprite_program();
    const auto& locations = builtin_locations(program);
    for (unsigned at = from; at < to; ++at) {
        if (g_entries[at].drawn)
            draw_sprite(g_entries[at].sprite, program, locations, g_matrices[at]);
    }
}

// --- skkBot2 diagnostics ---------------------------------------------------
// We cannot run the game ourselves, so every measurement flows into the log
// (skkbot2.log). Once per second of batched visiting we report how long the
// batched visit took and how much work it did, plus the first-time state
// transitions so it is obvious from the log whether batching engaged at all.
#include "logger/logger.hpp"

static long long qpc_now() {
    LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart;
}
static double qpc_to_ms(long long delta) {
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    return 1000.0 * (double)delta / (double)freq.QuadPart;
}

struct BatchStats {
    long long last_ms = 0;          // wall time of last batched visit
    long long total_ns = 0;         // accumulated batched-visit time units
    unsigned long long batched_frames = 0;
    unsigned long long batch_visits_ms = 0;   // accumulated ms
    long long last_report_qpc = 0;
    bool reported_enabled = false;
};
static BatchStats g_stats;

static void draw_batched_layer(CCNode* layer) {
    long long start = qpc_now();
    ++g_stats.batched_frames;
    if (!layer->isVisible()) { g_stats.last_ms = qpc_now() - start; return; }
    kmGLPushMatrix();
    layer->transform();
    kmGLGetMatrix(KM_GL_PROJECTION, &g_projection);
    kmGLGetMatrix(KM_GL_MODELVIEW, &g_layer_view);
    if (layer->getChildren() && layer->getChildren()->count() > 0) layer->sortAllChildren();
    hold_copy_gl_state(true);
    gather(layer);
    hold_copy_gl_state(false);
    place_all();
    for (const auto& step : g_steps) {
        if (step.layer_draw) layer->draw();
        else if (step.foreign) step.foreign->visit();
        else draw_entries(step.entry_from, step.entry_to);
    }
    kmGLPopMatrix();
    g_stats.last_ms = qpc_now() - start;
    g_stats.batch_visits_ms = g_stats.batch_visits_ms + g_stats.last_ms;
    g_stats.total_ns = g_stats.total_ns + g_stats.last_ms;
}

class $modify(EchoBatchNode, CCNode) {
    void visit() {
        if (this != g_batched_layer) {
            CCNode::visit();
            return;
        }
        g_batched_layer = nullptr;
        if (read<void*>(this, NODE_GRID_OFFSET) || read<void*>(this, NODE_CAMERA_OFFSET)) {
            CCNode::visit();
            return;
        }
        draw_batched_layer(this);
    }
};

class $modify(EchoBatchGameLayer, GJBaseGameLayer) {
    void visit() {
        const unsigned min_batched_children = 1024;

        bool batch = true && m_objectLayer && m_objectLayer->getParent() == m_objectParent &&
            m_objectLayer->getChildrenCount() >= min_batched_children;
        if (batch && !g_stats.reported_enabled) {
            g_stats.reported_enabled = true;
            skkbot2::log::info("effective_batch: ENABLED, objectLayer children={}",
                m_objectLayer->getChildrenCount());
        }
        g_batched_layer = batch ? m_objectLayer : nullptr;
        GJBaseGameLayer::visit();
        g_batched_layer = nullptr;

        // Once per second report no matter whether batching engaged: from the
        // log we need the reason (children count) even when batch stays false.
        long long now = qpc_now();
        if (g_stats.last_report_qpc == 0)
            g_stats.last_report_qpc = now;
        if (qpc_to_ms(now - g_stats.last_report_qpc) >= 1000.0) {
            g_stats.last_report_qpc = now;
            unsigned children = m_objectLayer ? m_objectLayer->getChildrenCount() : 0;
            double batch_ms = qpc_to_ms(g_stats.batch_visits_ms);
            skkbot2::log::info("effective_batch: batch={} children={} batched_frames={} batched_visit_ms/frame={:.3f}",
                batch ? 1 : 0, children, g_stats.batched_frames, 
                g_stats.batched_frames ? batch_ms / (double)g_stats.batched_frames : 0.0);
            g_stats.batched_frames = 0;
            g_stats.batch_visits_ms = 0;
        }
    }
};
#endif
