#include "capture.hpp"

#include <Geode/Geode.hpp>
#include <Geode/cocos/shaders/ccGLStateCache.h>

#include <algorithm>
#include <vector>

#include "../logger/logger.hpp"

using namespace geode::prelude;
using namespace cocos2d;


namespace skkbot2 {
namespace render {

// ============ SHADERS ============
//
// The frame to record is produced by re-rendering the scene into m_sceneTex
// at the OUTPUT size (pass 0, see issueReadback), then two passes sample that
// texture and emit NV12 planes directly (BT.709 limited range, same
// coefficients sws_scale uses for RGBA->NV12):
//   pass Y:  full-res, writes luma into the RED channel
//   pass UV: half-res, writes U and V into the RG channels
// readPixels then grabs GL_RED (Y) and GL_RG (UV) with zero CPU conversion.
// The vertex shader flips V, since glReadPixels returns rows bottom-up.
// The quad maps the texture 1:1 (m_sceneTex is already output-sized).

static const char* kVertexShader = R"(#version 130
in vec2 a_position;
in vec2 a_texCoord;
out vec2 v_texCoord;
void main() {
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texCoord = vec2(a_texCoord.x, 1.0 - a_texCoord.y);
}
)";

static const char* kFragY = R"(#version 130
precision highp float;
in vec2 v_texCoord;
out vec4 fragColor;
uniform sampler2D u_texture;
uniform float u_fade;
void main() {
    vec3 rgb = texture2D(u_texture, v_texCoord).rgb * u_fade;
    float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    y = y * 219.0 / 255.0 + 16.0 / 255.0;
    fragColor = vec4(y, 0.0, 0.0, 1.0);
}
)";

static const char* kFragUV = R"(#version 130
precision highp float;
in vec2 v_texCoord;
out vec4 fragColor;
uniform sampler2D u_texture;
uniform float u_fade;
void main() {
    vec3 rgb = texture2D(u_texture, v_texCoord).rgb * u_fade;
    float u = dot(rgb, vec3(-0.11457, -0.38543, 0.5)) + 0.5;
    float v = dot(rgb, vec3(0.5, -0.45415, -0.04585)) + 0.5;
    u = u * 224.0 / 255.0 + 16.0 / 255.0;
    v = v * 224.0 / 255.0 + 16.0 / 255.0;
    fragColor = vec4(u, v, 0.0, 1.0);
}
)";

static GLuint compileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei len = 0;
        glGetShaderInfoLog(sh, sizeof(log), &len, log);
        geode::log::error("capture shader compile failed: {}", len ? log : "?");
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint linkProgram(const char* frag) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, frag);
    if (!vs || !fs) {
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_position");
    glBindAttribLocation(prog, 1, "a_texCoord");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei len = 0;
        glGetProgramInfoLog(prog, sizeof(log), &len, log);
        geode::log::error("capture program link failed: {}", len ? log : "?");
        glDeleteProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

bool FboCapture::initShaders() {
    m_progY = linkProgram(kFragY);
    m_progUV = linkProgram(kFragUV);
    if (!m_progY || !m_progUV) {
        return false;
    }
    m_uTexY = glGetUniformLocation(m_progY, "u_texture");
    m_uTexUV = glGetUniformLocation(m_progUV, "u_texture");
    m_uFadeY = glGetUniformLocation(m_progY, "u_fade");
    m_uFadeUV = glGetUniformLocation(m_progUV, "u_fade");

    float vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,  //
        1.0f,  -1.0f, 1.0f, 0.0f,  //
        1.0f,  1.0f,  1.0f, 1.0f,  //
        -1.0f, 1.0f,  0.0f, 1.0f   //
    };
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    return true;
}

static GLuint makeTexture(GLenum internal, GLsizei w, GLsizei h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

bool FboCapture::init(unsigned width, unsigned height) {
    destroy();
    m_width = width;
    m_height = height;
    m_nv12Size = static_cast<size_t>(width) * height * 3 / 2;
    m_fade = 1.0f;

    if (!initShaders()) {
        return false;
    }

    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* ren = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    skkbot2::log::info("[RENDER] GL context: {} ({})",
                       ver ? ver : "?", ren ? ren : "?");

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    // Scene re-render target вЂ” ALWAYS output-sized. The cocos view was
    // silently resized to the output size by Renderer::begin() for the
    // duration of the render, so visit() draws the level at exactly
    // m_width x m_height, independent of the visible window/zoom.
    m_sceneTex = makeTexture(GL_RGBA8, static_cast<GLsizei>(m_width),
                             static_cast<GLsizei>(m_height));
    glGenFramebuffers(1, &m_sceneFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_sceneTex, 0);
    GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
        skkbot2::log::error("[RENDER] scene FBO incomplete: {:#x}",
                            static_cast<unsigned>(fboStatus));
    }
    // Prime the scene target to black so the first preview frames (before
    // the first real capture lands) are black instead of garbage.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Y pass (full-res luma)
    m_yTex = makeTexture(GL_RGBA8, static_cast<GLsizei>(width),
                         static_cast<GLsizei>(height));
    glGenFramebuffers(1, &m_yFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_yFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_yTex, 0);

    // UV pass (half-res chroma)
    m_uvTex = makeTexture(GL_RGBA8, static_cast<GLsizei>(width / 2),
                          static_cast<GLsizei>(height / 2));
    glGenFramebuffers(1, &m_uvFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_uvFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_uvTex, 0);

    // Triple-buffered async readback (Y plane + UV plane per slot)
    const size_t yBytes = static_cast<size_t>(width) * height;
    const size_t uvBytes = yBytes / 2;
    glGenBuffers(PBO_COUNT, m_pbo);
    for (int i = 0; i < PBO_COUNT; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, yBytes + uvBytes, nullptr,
                     GL_STREAM_READ);
        m_fence[i] = 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // don't leave our FBO bound вЂ” the game must keep rendering to the screen
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    return true;
}

void FboCapture::destroy() {
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }
    if (m_progY) {
        glDeleteProgram(m_progY);
        m_progY = 0;
    }
    if (m_progUV) {
        glDeleteProgram(m_progUV);
        m_progUV = 0;
    }
    for (int i = 0; i < PBO_COUNT; i++) {
        if (m_pbo[i]) {
            if (m_fence[i]) {
                glDeleteSync(reinterpret_cast<GLsync>(m_fence[i]));
                m_fence[i] = 0;
            }
            glDeleteBuffers(1, &m_pbo[i]);
            m_pbo[i] = 0;
        }
    }    if (m_yFbo) {
        glDeleteFramebuffers(1, &m_yFbo);
        m_yFbo = 0;
    }
    if (m_uvFbo) {
        glDeleteFramebuffers(1, &m_uvFbo);
        m_uvFbo = 0;
    }
    if (m_sceneFbo) {
        glDeleteFramebuffers(1, &m_sceneFbo);
        m_sceneFbo = 0;
    }
    if (m_sceneTex) {
        glDeleteTextures(1, &m_sceneTex);
        m_sceneTex = 0;
    }
    if (m_yTex) {
        glDeleteTextures(1, &m_yTex);
        m_yTex = 0;
    }
    if (m_uvTex) {
        glDeleteTextures(1, &m_uvTex);
        m_uvTex = 0;
    }
    m_issued = 0;
}

// Diagnostics: logs the FIRST occurrence of each (op, error) pair so a
// broken pipeline step is pinpointed instead of silently timing out fences.
static void logGLErr(const char* where) {
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) {
        return;
    }
    static std::string s_seen;
    std::string key = std::string(where) + ":" + std::to_string(e);
    if (s_seen.find(key) != std::string::npos) {
        return;
    }
    s_seen += key + ";";
    skkbot2::log::error("[RENDER] GL error {:#x} after {}", 
                        static_cast<unsigned>(e), where);
}

// Re-renders the game scene into m_sceneTex at the OUTPUT size (Silicate-
// style). The cocos view is silently set to the output size by
// Renderer::begin() for the whole render, so visit() produces the level at
// exactly m_width x m_height with the exact same camera/zoom as the sim вЂ”
// nothing depends on the visible window anymore. Our own FBO is bound first;
// the game's FBO bindings are restored by the caller.
static void renderSceneInto(unsigned w, unsigned h, GLuint fbo) {
    auto* scene = CCDirector::get()->getRunningScene();
    if (!scene) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    scene->visit();
    // The scene leaves arbitrary GL state behind (program, cull, scissor,
    // depth): normalize what the Y/UV passes below rely on.
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Re-renders the scene into m_sceneTex (output-sized) and runs the Y/UV
// colorspace passes into the NV12 planes of slot m_issued % PBO_COUNT, then
// issues the async readback. Must run on the GL thread (main thread). Runs
// after drawScene. The game's own FBO bindings are restored at the end.
void FboCapture::issueReadback(int64_t pts) {
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint prevReadFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLint prevActiveTex = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    GLint prevTexBind = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexBind);

    const int slot = static_cast<int>(m_issued % PBO_COUNT);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[slot]);
    if (m_fence[slot]) {
        // shouldn't happen (3-deep pipeline), but be safe: wait and reuse
        glClientWaitSync(reinterpret_cast<GLsync>(m_fence[slot]),
                         GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000'000ull);
        glDeleteSync(reinterpret_cast<GLsync>(m_fence[slot]));
        m_fence[slot] = 0;
    }
    // This slot now carries the frame for THIS pts. The harvest reads it back
    // two grabs from now; recording the pts here is what keeps timestamp and
    // pixels in lockstep (Silicat's m_ptsQueue push-time labeling).
    m_pts[slot] = pts;

    // --- Pass 0: scene -> m_sceneTex (output size) ---
    // Re-render the level into our own FBO at the output resolution instead
    // of copying the visible backbuffer. The result is identical to what the
    // player sees at the default zoom, and is immune to window size / camera
    // zoom drift during the render.
    renderSceneInto(m_width, m_height, m_sceneFbo);

    // --- Pass 1: m_sceneTex -> Y ---
    glBindFramebuffer(GL_FRAMEBUFFER, m_yFbo);
    glViewport(0, 0, static_cast<GLsizei>(m_width),
               static_cast<GLsizei>(m_height));
    glDisable(GL_BLEND);
    glUseProgram(m_progY);
    glUniform1f(m_uFadeY, m_fade);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_sceneTex);
    glUniform1i(m_uTexY, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_yTex);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
    logGLErr("passY draw");

    // --- Pass 2: m_sceneTex -> UV (half res) ---
    glBindFramebuffer(GL_FRAMEBUFFER, m_uvFbo);
    glViewport(0, 0, static_cast<GLsizei>(m_width / 2),
               static_cast<GLsizei>(m_height / 2));
    glDisable(GL_BLEND);
    glUseProgram(m_progUV);
    glUniform1f(m_uFadeUV, m_fade);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_sceneTex);
    glUniform1i(m_uTexUV, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_uvTex);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
    logGLErr("passUV draw");

    // --- Async readback into the PBO slot ---
    const size_t yBytes = static_cast<size_t>(m_width) * m_height;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_yFbo);
    glReadPixels(0, 0, static_cast<GLsizei>(m_width),
                 static_cast<GLsizei>(m_height), GL_RED, GL_UNSIGNED_BYTE, 0);
    logGLErr("readPixels Y");
    glBindFramebuffer(GL_FRAMEBUFFER, m_uvFbo);
    glReadPixels(0, 0, static_cast<GLsizei>(m_width / 2),
                 static_cast<GLsizei>(m_height / 2), GL_RG, GL_UNSIGNED_BYTE,
                 reinterpret_cast<void*>(yBytes));
    logGLErr("readPixels UV");
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    m_fence[slot] =
        reinterpret_cast<uintptr_t>(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
    logGLErr("glFenceSync");
    m_issued++;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
    glViewport(prevViewport[0], prevViewport[1],
               static_cast<GLsizei>(prevViewport[2]),
               static_cast<GLsizei>(prevViewport[3]));
    glEnable(GL_BLEND);

    // Restore program / active unit / unit-0 texture binding. The capture
    // passes above used raw glBindTexture/glUseProgram/glActiveTexture, which
    // leaves cocos's GL state cache (ccGLBindTexture2D, ccGLUseProgram) out of
    // sync: the next cached call would SKIP the real glBind/glUse because the
    // cache still matches the pre-render value. Since imgui-cocos draws the
    // bot menu through those cached wrappers in the same frame (our grab runs
    // inside ImGui's m_drawCall, before renderFrame paints the text), a stale
    // cache paints the font texture with the wrong GL state в†’ the menu text
    // garbles during a render. ccGLInvalidateStateCache() forces the wrappers
    // to re-issue real GL calls next time they're used.
    if (prevProgram) {
        glUseProgram(static_cast<GLuint>(prevProgram));
    }
    if (prevActiveTex) {
        glActiveTexture(static_cast<GLenum>(prevActiveTex));
    }
    // v57.2: bind unconditionally вЂ” when prevTexBind was 0 the old `if`
    // skipped the restore, leaving unit 0 pinned to our yTex/uvTex. With the
    // cocos state cache still claiming the font atlas is bound, imgui's
    // ccGLBindTexture2D(atlas) in the frames between captures skipped the real
    // bind and painted glyphs from the capture texture (garbled/fat menu
    // text).
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTexBind));
    ccGLInvalidateStateCache();
}

// Synchronous CPU readback of the current Y/UV pass output (no PBO). Blocks
// the GPU pipeline until the pixels are in client memory вЂ” slower than the
// async PBO path, but immune to PBO/map issues. Used as a fallback so a
// broken PBO path can't stall the render forever.
void FboCapture::syncReadback(uint8_t* dst) {
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint prevReadFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLint prevActiveTex = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    GLint prevTexBind = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexBind);

    const size_t yBytes = static_cast<size_t>(m_width) * m_height;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_yFbo);
    glReadPixels(0, 0, static_cast<GLsizei>(m_width),
                 static_cast<GLsizei>(m_height), GL_RED, GL_UNSIGNED_BYTE,
                 dst);
    glBindFramebuffer(GL_FRAMEBUFFER, m_uvFbo);
    glReadPixels(0, 0, static_cast<GLsizei>(m_width / 2),
                 static_cast<GLsizei>(m_height / 2), GL_RG, GL_UNSIGNED_BYTE,
                 dst + yBytes);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
    glViewport(prevViewport[0], prevViewport[1],
               static_cast<GLsizei>(prevViewport[2]),
               static_cast<GLsizei>(prevViewport[3]));
    glEnable(GL_BLEND);

    // Same cocos state-cache resync as issueReadback (see there).
    if (prevProgram) {
        glUseProgram(static_cast<GLuint>(prevProgram));
    }
    if (prevActiveTex) {
        glActiveTexture(static_cast<GLenum>(prevActiveTex));
    }
    // v57.2: bind unconditionally вЂ” when prevTexBind was 0 the old `if`
    // skipped the restore, leaving unit 0 pinned to our yTex/uvTex. With the
    // cocos state cache still claiming the font atlas is bound, imgui's
    // ccGLBindTexture2D(atlas) in the frames between captures skipped the real
    // bind and painted glyphs from the capture texture (garbled/fat menu
    // text).
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTexBind));
    ccGLInvalidateStateCache();
}

// Silicate-style live preview: the level is not drawn to the window during a
// render, so blit the last scene captured into m_sceneTex letterboxed into the
// currently bound DRAW framebuffer (the default framebuffer when called from
// the CCDirector::drawScene hook). Restores ALL modified GL state so imgui
// (drawn right after swapBuffers inside the hook) and the game's next frame
// are not affected.
void FboCapture::displayPreview(float winW, float winH) {
    if (!m_sceneFbo) {
        return;
    }
    if (winW <= 0.0f || winH <= 0.0f) {
        // Never blit into a zero-sized window rect: it clears the window black
        // and glBlitFramebuffer with a 0x0 target raises GL_INVALID_VALUE
        // ("displayPreview blit" 0x501 in the 2026/09/16 logs). With the
        // acquireView()>0 guards this is unreachable, but a corrupted view
        // must not kill the frame.
        return;
    }
    GLint prevDrawFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    GLint prevReadFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean prevScissor = static_cast<GLboolean>(glIsEnabled(GL_SCISSOR_TEST));
    GLboolean prevBlend = static_cast<GLboolean>(glIsEnabled(GL_BLEND));
    GLboolean prevDepth = static_cast<GLboolean>(glIsEnabled(GL_DEPTH_TEST));

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Letterbox scale + centered rect in the real window.
    const float scale = std::min(winW / static_cast<float>(m_width),
                                 winH / static_cast<float>(m_height));
    const int dw = static_cast<int>(static_cast<float>(m_width) * scale);
    const int dh = static_cast<int>(static_cast<float>(m_height) * scale);
    const int dx = static_cast<int>(winW - dw) / 2;
    const int dy = static_cast<int>(winH - dh) / 2;
    glBlitFramebuffer(0, 0, static_cast<GLint>(m_width),
                      static_cast<GLint>(m_height), dx, dy, dx + dw, dy + dh,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    logGLErr("displayPreview blit");

    glBindFramebuffer(GL_FRAMEBUFFER, prevDrawFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
    glViewport(prevViewport[0], prevViewport[1],
               static_cast<GLsizei>(prevViewport[2]),
               static_cast<GLsizei>(prevViewport[3]));
    if (prevScissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    if (prevDepth) {
        glEnable(GL_DEPTH_TEST);
    }
    if (prevBlend) {
        glEnable(GL_BLEND);
    }
}

bool FboCapture::grab(uint8_t* dst, int64_t pts, int64_t* outPts) {
    if (!dst || !m_pbo[0]) {
        return false;
    }    // Harvest the oldest issued slot (6-deep pipeline). Its fence is normally
    // long signaled; the wait below is only a fallback for GPU lag under heavy
    // load вЂ” but it MUST be long enough, because if we give up here the slot
    // is recycled and that frame is lost forever.
    const int slot = static_cast<int>((m_issued + PBO_COUNT - 2) %
                                      PBO_COUNT);
    bool haveData = m_issued >= 2 && m_fence[slot] != 0;
    const int lastIssued = static_cast<int>((m_issued - 1 + PBO_COUNT) %
                                            PBO_COUNT);

    bool got = false;
    if (haveData) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[slot]);
        GLenum st = GL_TIMEOUT_EXPIRED;
        for (int tryWait = 0; tryWait < 5 && st == GL_TIMEOUT_EXPIRED;
             tryWait++) {
            st = glClientWaitSync(reinterpret_cast<GLsync>(m_fence[slot]),
                                  GL_SYNC_FLUSH_COMMANDS_BIT, 50'000'000ull);
        }
        if (st == GL_ALREADY_SIGNALED || st == GL_CONDITION_SATISFIED) {
            void* mapped = glMapBufferRange(
                GL_PIXEL_PACK_BUFFER, 0, m_nv12Size,
                GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
            if (!mapped) {
                // Some drivers reject UNSYNC on pack buffers still owned by a
                // pending glReadPixels вЂ” retry the plain (blocking) map.
                mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, m_nv12Size,
                                          GL_MAP_READ_BIT);
            }
            if (mapped) {
                std::memcpy(dst, mapped, m_nv12Size);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                got = true;
                if (outPts) {
                    *outPts = m_pts[slot];
                }
            } else {
                logGLErr("glMapBufferRange");
                skkbot2::log::warn("[RENDER] capture map failed (fence was "
                                   "signaled)");
            }
        } else {
            // Fence not done even after ~250ms: skip this slot. The pipeline
            // is 6 deep, so this only happens if the GPU is severely stalled.
            static uint64_t s_failLog = 0;
            if (++s_failLog <= 3 || (s_failLog % 20) == 0) {
                skkbot2::log::warn(
                    "[RENDER] capture fence not signaled: st={:#x} issued={} "
                    "slot={} fence={}",
                    static_cast<unsigned>(st), m_issued, slot,
                    static_cast<unsigned long long>(m_fence[slot]));
            }
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    if (!got && m_issued >= 2) {
        // PBO harvest failed (map error / fence timeout): fall back to a
        // synchronous readback of the pass output. Slower, but the render
        // keeps producing frames instead of stalling. The pass buffers hold
        // the MOST RECENT issue (one grab newer than the harvest slot), so
        // the returned frame is labeled with that issue's pts вЂ” the pairing
        // stays exact. (Gated on m_issued >= 2 so the cold-start grabs return
        // false instead of double-delivering the warmup frame.)
        static uint64_t s_fbLog = 0;
        if (++s_fbLog <= 3 || (s_fbLog % 30) == 0) {
            skkbot2::log::warn("[RENDER] sync-readback fallback (#{})",
                               s_fbLog);
        }
        syncReadback(dst);
        got = true;
        if (outPts) {
            *outPts = m_pts[lastIssued];
        }
    }

    // Issue the readback of the CURRENT scene (even if we had no data yet вЂ”
    // this warms the pipeline).
    issueReadback(pts);

    return got;
}

}  // namespace render
}  // namespace skkbot2
