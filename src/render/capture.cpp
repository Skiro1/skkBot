#include "capture.hpp"

#include <Geode/Geode.hpp>
#include <Geode/cocos/shaders/ccGLStateCache.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include <map>
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
    // WebGL2 validates internalformat against format, so the sized formats we
    // use for the NV12 planes need their own upload format: GL_R8 pairs with
    // GL_RED and GL_RG8 with GL_RG. GL_RGBA8 keeps GL_RGBA.
    GLenum fmt = GL_RGBA;
    if (internal == GL_R8) {
        fmt = GL_RED;
    } else if (internal == GL_RG8) {
        fmt = GL_RG;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, GL_UNSIGNED_BYTE,
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
    // Per-render diagnostics. These were never reset, so the summary mixed
    // every render the session had ever done: Stereo Madness on 2026-10-18
    // reported "scene visit 189% of grab" because visitN was 15553 while the
    // render produced 5350 frames - 10201 of those visits belonged to the
    // GRIEF render before it. GRIEF's own number was right only because it was
    // the first render of the session. destroy() resets m_issued but never
    // touched these two.
    m_visitMs = 0.0;
    m_visitN = 0;

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

    // Y pass (full-res luma). GL_R8, not GL_RGBA8: the shader writes only .r and
    // the readback reads only GL_RED, so an RGBA8 target wasted 3 of every 4
    // bytes written - 33 MB of render-target traffic per 4K frame instead of
    // 8.3 MB. Same for the UV plane below, which only needs two channels.
    m_yTex = makeTexture(GL_R8, static_cast<GLsizei>(width),
                         static_cast<GLsizei>(height));
    glGenFramebuffers(1, &m_yFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_yFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_yTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        skkbot2::log::error(
            "[RENDER] Y plane FBO incomplete - the luma target must be a "
            "colour-renderable single-channel format");
        return false;
    }

    // UV pass (half-res chroma). GL_RG8 - the shader writes .rg and the
    // readback reads GL_RG, so RGBA8 was writing twice the bytes it kept.
    m_uvTex = makeTexture(GL_RG8, static_cast<GLsizei>(width / 2),
                          static_cast<GLsizei>(height / 2));
    glGenFramebuffers(1, &m_uvFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_uvFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_uvTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        skkbot2::log::error(
            "[RENDER] UV plane FBO incomplete - the chroma target must be a "
            "colour-renderable two-channel format");
        return false;
    }

    // Triple-buffered async readback (Y plane + UV plane per slot)
    const size_t yBytes = static_cast<size_t>(width) * height;
    const size_t uvBytes = yBytes / 2;
    glGenBuffers(PBO_COUNT, m_pbo);
    for (int i = 0; i < PBO_COUNT; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, yBytes + uvBytes, nullptr,
                     GL_STREAM_READ);
        m_fence[i] = 0;
        m_mapped[i] = nullptr;
        m_encBusy[i].store(false, std::memory_order_relaxed);
        m_pts[i] = -1;
        m_seq[i] = 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    m_issued = 0;
    m_syncBuf.assign(m_nv12Size, 0);
    m_syncBusy.store(false, std::memory_order_relaxed);
    m_syncBusySkips = 0;

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
            // A mapped slot must be unmapped before its buffer is deleted or
            // the driver is left holding storage it can no longer give back.
            if (m_mapped[i]) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                m_mapped[i] = nullptr;
                m_encBusy[i].store(false, std::memory_order_relaxed);
            }
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
// In release/performance builds, glGetError() calls are bypassed to prevent
// CPU-GPU command stream pipeline stalls on every frame.
static void logGLErr(const char* where) {
#ifdef SKKBOT_DEBUG_GL
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) {
        return;
    }
    static std::map<std::string, int> s_count;
    const std::string key = std::string(where) + ":" + std::to_string(e);
    int& n = s_count[key];
    ++n;
    if (n > 3 && n % 1000 != 0) {
        return;
    }
    skkbot2::log::error("[RENDER] GL error {:#x} after {} (occurrence #{})",
                        static_cast<unsigned>(e), where, n);
#else
    (void)where;
#endif
}

// Re-renders the game scene into m_sceneTex at the OUTPUT size (Silicate-
// style). The cocos view is silently set to the output size by
// Renderer::begin() for the whole render, so visit() produces the level at
// exactly m_width x m_height with the exact same camera/zoom as the sim —
// nothing depends on the visible window anymore. Our own FBO is bound first;
// the game's FBO bindings are restored by the caller.
//
// The scene->visit() inside is timed into g_lastVisitMs. This traversal is the
// duplicate under investigation: the preview blit already put a full scene on
// screen this frame, and this walks the same ~34k nodes a second time. Whether
// it is worth removing depends entirely on what that costs, and the stage
// timers could not answer it because glDrawArrays only queues work - the CPU
// side of the traversal is invisible to them, while the GPU side drains later
// inside swap.
static double g_lastVisitMs = 0.0;

// Same clock as renderer's nowS(), which is private to renderer.cpp and not
// reachable from here. steady_clock either way, in seconds.
static double nowCapture() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void renderSceneInto(unsigned w, unsigned h, GLuint fbo) {
    auto* scene = CCDirector::get()->getRunningScene();
    if (!scene) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const double visitT0 = nowCapture();
    scene->visit();
    g_lastVisitMs = (nowCapture() - visitT0) * 1e3;
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
    // Slot selection happens BEFORE any GL state is touched, so the
    // "everything is held" path can bail out without having to restore
    // anything. It is backpressure that slipped past the hasFreeSlot() probe
    // the renderer asks before stepping the simulation, so this frame's capture
    // is skipped rather than overwriting a frame somebody is still reading.
    int slot = -1;
    if (!pickIssueSlot(&slot)) {
        static uint64_t s_noSlot = 0;
        if (++s_noSlot <= 3 || (s_noSlot % 100) == 0) {
            skkbot2::log::warn("[RENDER] no free PBO slot (#{}) - frame skipped",
                               s_noSlot);
        }
        return;
    }

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint prevReadFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[slot]);
    // This slot now carries the frame for THIS pts. The harvest claims the
    // OLDEST such slot a few grabs later; recording the pts here is what keeps
    // timestamp and pixels in lockstep (Silicat's m_ptsQueue push-time
    // labeling).
    m_pts[slot] = pts;
    m_seq[slot] = m_issued;

    // --- Pass 0: scene -> m_sceneTex (output size) ---
    // Re-render the level into our own FBO at the output resolution instead
    // of copying the visible backbuffer. The result is identical to what the
    // player sees at the default zoom, and is immune to window size / camera
    // zoom drift during the render.
    renderSceneInto(m_width, m_height, m_sceneFbo);
    m_visitMs += g_lastVisitMs;
    m_visitN++;
    // Drain the scene's own GL errors. GD raises INVALID_OPERATION inside
    // scene->visit() every frame (verified: it survives every change to our
    // own passes), and glGetError returns the OLDEST pending error - so leaving
    // it queued would attribute GD's fault to the Y pass and make every check
    // below meaningless. Verified against a rendered frame: the planes are
    // correct, so this is noise, not a broken capture.
    static bool logged = false;
    if (!logged) {
        logged = true;
        if (GLenum e = glGetError(); e != GL_NO_ERROR) {
            skkbot2::log::info(
                "[RENDER] scene visit() leaves GL error {:#x} pending (startup check)",
                static_cast<unsigned>(e));
        }
    }

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
    // The luma target must NOT be bound to a texture unit while it is the
    // bound framebuffer's colour attachment - that is a feedback loop and the
    // draw comes back GL_INVALID_OPERATION (0x502). Leave unit 0 empty; the
    // program only samples unit 1.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
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
    glBindTexture(GL_TEXTURE_2D, 0);
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
    if (!m_fence[slot]) {
        GLenum e = glGetError();
        skkbot2::log::error("[RENDER] glFenceSync failed: {:#x}", static_cast<unsigned>(e));
    }
    m_issued++;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
    glViewport(prevViewport[0], prevViewport[1],
               static_cast<GLsizei>(prevViewport[2]),
               static_cast<GLsizei>(prevViewport[3]));
    // scene->visit() in renderSceneInto drives cocos's own draw path, which
    // overwrites the GL blend FUNCTION to whatever the level's nodes last set
    // (GD's sprites/particles use premultiplied and additive blends, not
    // imgui's). imgui-cocos sets its blend func exactly ONCE at the top of
    // drawFrame (backend.cpp, BEFORE this capture callback runs) and never
    // re-asserts it before renderFrame() paints the bot menu — so a leaked
    // func is what the whole menu composites with for the rest of the frame.
    // With a replace blend (GL_ONE/GL_ZERO) every glyph renders as a SOLID
    // colour BLOCK (the atlas RGB coverage is taken literally, alpha is
    // ignored): text looks fat, "filled with one colour" and unreadable, and
    // every translucent panel turns opaque — the entire UI reads as broken
    // for the whole render. This only shows while recording: scene->visit()
    // in the grab is the ONLY path that touches blend, and it runs solely
    // during a render. Re-apply imgui's blend func so the menu renders right.
    // Raw glBlendFunc (not the cached ccGLBlendFunc wrapper): the scene may
    // have changed the real GL state through cocos's cached path, and
    // ccGLBlendFunc would no-op if its cache still holds imgui's value while
    // the actual state differs. ccGLInvalidateStateCache() below resyncs the
    // cache either way.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);

    // Unbind unit-0 texture, reset active unit/program, and invalidate cocos cache.
    // ccGLInvalidateStateCache() forces cocos and imgui-cocos wrappers to
    // re-issue real GL calls next time they're used.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
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
    // Re-apply imgui's blend func: the Y/UV passes above disabled GL_BLEND
    // and the scene re-render in issueReadback (which runs right before this
    // fallback in grab()) left a non-imgui blend FUNCTION behind. Same leak
    // as the async path — see the restore there.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);

    // Same cocos state-cache resync as issueReadback (see there).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
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

// --- Slot bookkeeping for the pointer handover -----------------------------
//
// Four states per slot, all of them readable from m_mapped / m_fence:
//
//   free      m_mapped == nullptr && m_fence == 0
//   pending   m_mapped == nullptr && m_fence != 0   (GPU still writing)
//   held      m_mapped != 0                         (somebody is reading)
//   retired   m_mapped != 0 && m_fence == 0         (read out, waiting to be
//                                                     unmapped once returned)
//
// The rule that makes the whole thing safe is one line: a slot may be given a
// new glReadPixels ONLY in the free state. Everything else follows from it.

bool FboCapture::pickIssueSlot(int* slot) const {
    // Round-robin from where the last issue left off, so back-to-back issues
    // still rotate through the ring instead of hammering one slot.
    const unsigned start = m_issued % PBO_COUNT;
    for (int i = 0; i < PBO_COUNT; i++) {
        const int s = static_cast<int>((start + i) % PBO_COUNT);
        if (m_mapped[s] == nullptr && m_fence[s] == 0) {
            *slot = s;
            return true;
        }
    }
    return false;
}

bool FboCapture::pickHarvestSlot(int* slot) const {
    int best = -1;
    unsigned bestSeq = 0;
    for (int s = 0; s < PBO_COUNT; s++) {
        if (m_mapped[s] != nullptr || m_fence[s] == 0) {
            continue;
        }
        if (best < 0 || m_seq[s] < bestSeq) {
            best = s;
            bestSeq = m_seq[s];
        }
    }
    if (best < 0) {
        return false;
    }
    *slot = best;
    return true;
}

bool FboCapture::hasFreeSlot() const {
    // A slot counts as available if it is free NOW **or** if the encoder thread
    // has already handed it back and it is only waiting to be unmapped.
    //
    // That second clause is a deadlock fix, not an optimisation. The only place
    // slots are unmapped is the top of grab(), and grab() is only reached AFTER
    // this gate has said yes. A gate counting only currently-free slots is
    // therefore a gate waiting on code that sits behind it: once the pool ran
    // dry the main thread refused to step, so it never called grab(), so the
    // returned slots were never unmapped, so the pool stayed dry. Measured
    // 2026-10-19 01:11: pboGate=1801, stopDrive=1801, batchAvg=0.074, and the
    // render sat there until the 30 s stall guard killed it.
    //
    // Counting a returned slot is safe because the promise is kept: grab() calls
    // unmapReturnedSlots() before issueReadback(), so by the time the issue runs
    // that slot really is free. This was invisible while RING_SIZE=6, because
    // the ring kept the main thread from ever getting far enough ahead to drain
    // the pool - the ring was accidentally throttling the bug away.
    for (int s = 0; s < PBO_COUNT; s++) {
        if (m_mapped[s] == nullptr && m_fence[s] == 0) {
            return true;
        }
        if (m_mapped[s] != nullptr && !m_encBusy[s].load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void FboCapture::releaseSlot(int slot) {
    if (slot < 0 || slot >= PBO_COUNT) {
        return;
    }
    m_encBusy[slot].store(false, std::memory_order_release);
}

void FboCapture::releaseSync() {
    m_syncBusy.store(false, std::memory_order_release);
}

void FboCapture::unmapReturnedSlots() {
    for (int s = 0; s < PBO_COUNT; s++) {
        if (m_mapped[s] == nullptr || m_encBusy[s].load(std::memory_order_acquire)) {
            continue;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[s]);
        // A FALSE here would mean the GPU clobbered our mapping, i.e. somebody
        // issued a readback into a slot we still owned - the exact failure the
        // state machine exists to prevent, so it is worth saying out loud.
        if (glUnmapBuffer(GL_PIXEL_PACK_BUFFER) == GL_FALSE) {
            skkbot2::log::warn("[RENDER] glUnmapBuffer reported data loss on "
                               "slot {} - ownership bug", s);
        }
        m_mapped[s] = nullptr;
        if (m_fence[s]) {
            glDeleteSync(reinterpret_cast<GLsync>(m_fence[s]));
            m_fence[s] = 0;
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

bool FboCapture::grab(int64_t pts, GrabResult* out) {
    if (!out || !m_pbo[0]) {
        return false;
    }
    // Anything the encoder has finished with becomes free before we pick.
    unmapReturnedSlots();

    int slot = -1;
    bool got = false;
    if (m_issued >= 1 && pickHarvestSlot(&slot)) {
        // Its fence is normally long signaled; the wait below is only a fallback
        // for GPU lag under heavy load - but it MUST be long enough, because
        // giving up here loses that frame's data for good.
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[slot]);
        GLenum st = GL_TIMEOUT_EXPIRED;
        for (int tryWait = 0; tryWait < 5 && st == GL_TIMEOUT_EXPIRED; tryWait++) {
            st = glClientWaitSync(reinterpret_cast<GLsync>(m_fence[slot]),
                                  GL_SYNC_FLUSH_COMMANDS_BIT, 50'000'000ull);
        }
        if (st == GL_ALREADY_SIGNALED || st == GL_CONDITION_SATISFIED) {
            void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, m_nv12Size,
                                            GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
            if (!mapped) {
                // Some drivers reject UNSYNC on pack buffers still owned by a
                // pending glReadPixels - retry the plain (blocking) map.
                mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, m_nv12Size,
                                          GL_MAP_READ_BIT);
            }
            if (mapped) {
                // THE POINT OF THE CHANGE: no memcpy here. The mapping stays
                // alive (m_mapped[slot]) and the encoder thread reads straight
                // out of it, so the 24.9 MB that used to cross the main thread
                // at 4K never crosses it at all.
                m_mapped[slot] = static_cast<uint8_t*>(mapped);
                m_encBusy[slot].store(true, std::memory_order_release);
                out->data = m_mapped[slot];
                out->slot = slot;
                out->preCopied = false;
                out->pts = m_pts[slot];
                got = true;
            } else {
                logGLErr("glMapBufferRange");
                skkbot2::log::warn("[RENDER] capture map failed (fence was "
                                   "signaled)");
            }
        } else {
            // Fence not done even after ~250ms: skip this slot. Only reachable
            // when the GPU is severely stalled.
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

    if (!got && m_issued >= 1) {
        // PBO harvest failed (map error / fence timeout): fall back to a
        // synchronous readback of the pass output. Slower, but the render keeps
        // producing frames instead of stalling. The pass buffers hold the MOST
        // RECENT issue, so the returned frame is labeled with that issue's pts
        // and the pairing stays exact.
        //
        // Unlike the PBO path this copy happens HERE, on the main thread: the
        // GL context is here and nowhere else. The frame therefore travels as a
        // finished buffer (preCopied) and the encoder must not copy it again -
        // and the next fallback may not start until the encoder has released
        // this one.
        if (m_syncBusy.load(std::memory_order_acquire)) {
            ++m_syncBusySkips;
            if (m_syncBusySkips <= 3) {
                skkbot2::log::warn("[RENDER] sync fallback skipped: previous "
                                   "one still held by the encoder (#{})",
                                   m_syncBusySkips);
            }
        } else {
            static uint64_t s_fbLog = 0;
            if (++s_fbLog <= 3 || (s_fbLog % 30) == 0) {
                skkbot2::log::warn("[RENDER] sync-readback fallback (#{})", s_fbLog);
            }
            syncReadback(m_syncBuf.data());
            m_syncBusy.store(true, std::memory_order_release);
            const int lastIssued = static_cast<int>((m_issued - 1) % PBO_COUNT);
            out->data = m_syncBuf.data();
            out->slot = -1;
            out->preCopied = true;
            out->pts = m_pts[lastIssued];
            got = true;
        }
    }

    // Issue the readback of the CURRENT scene (even if we had no data yet -
    // this warms the pipeline).
    issueReadback(pts);

    return got;
}

}  // namespace render
}  // namespace skkbot2
