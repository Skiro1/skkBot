#include "renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/ShaderLayer.hpp>
#include <Geode/cocos/shaders/CCShaderCache.h>
#include <Geode/cocos/shaders/ccGLStateCache.h>

#include "../skkBot.hpp"
#include "../logger/logger.hpp"
#include "audiocap.hpp"
#include "../hacks/physicsbypass.hpp"

using namespace geode::prelude;
using namespace cocos2d;

namespace skkbot2 {
namespace render {

namespace {
inline double nowS() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Absolute video frame axis (bot's computeFrame equivalent): the current
// physics tick number on the macro's tps grid, 1-based (respawn-aware — the
// level time rewinds with the respawn, so the video timeline follows).
static uint64_t computeRenderFrame(GJBaseGameLayer* layer, double tps) {
    if (!layer) {
        return 1;
    }
    double lt = layer->m_gameState.m_levelTime;
    if (lt < 0.0) {
        lt = 0.0;
    }
    return static_cast<uint64_t>(std::floor(lt * tps)) + 1;
}

// The GD pause menu (PauseLayer) is not guaranteed to be a DIRECT child of
// the PlayLayer. A plain getChildByType<PauseLayer>(0) lookup on the level
// can miss it, and merely clearing m_isPaused is not enough then: the menu
// node stays alive, blocks the frame loop and covers the game (freezing the
// window black until the user presses the Resume button). Walk the whole
// running scene recursively and dismiss the menu via onResume() - the same
// approach as the GucciBot reference (findOpenPauseLayerRecursive in its
// engine_core.cpp).
static PauseLayer* findOpenPauseLayerRecursive(CCNode* node) {
    if (!node) {
        return nullptr;
    }
    if (auto* pauseLayer = node->getChildByType<PauseLayer>(0)) {
        return pauseLayer;
    }
    auto* children = node->getChildren();
    if (!children) {
        return nullptr;
    }
    for (unsigned i = 0; i < children->count(); i++) {
        auto* child = static_cast<CCNode*>(children->objectAtIndex(i));
        if (auto* found = findOpenPauseLayerRecursive(child)) {
            return found;
        }
    }
    return nullptr;
}

// Live trailer: keep capturing the REAL scene for a couple of frames after
// the level completes, then stop вЂ” the video ends on the actual completion
// frame instead of a frozen after-end hold. The fade-out ramp (if configured)
// runs over the last m_fadeOutSeconds of the trailer.
constexpr double kTrailerHold = 2.0 / 60.0;

// Silicate-style silent view change: the cocos view size (winSize /
// design-resolution) is set to `size` WITHOUT touching the real window (the
// CCEGLView override of setFrameSize is skipped via the qualified protocol
// call, so no swap-chain resize happens). The game then renders the level at
// exactly `size` with the matching projection, which is what the visit()-based
// capture (capture.cpp pass 0) needs to produce output-sized frames.
// TEMP DIAG: dump view state at a named point (used to chase the winSize/csf
// drift that garbles the bot-menu text during a render). Per-point counters so
// each site gets its own budget.
static int s_diagBeg = 0, s_diagPre = 0, s_diagPost = 0, s_diagOut = 0,
           s_diagRest = 0;
void diagViewState(const char* where, int cap) {
    int* counter = nullptr;
    if (strcmp(where, "begin") == 0) {
        counter = &s_diagBeg;
    } else if (strcmp(where, "pre-set") == 0) {
        counter = &s_diagPre;
    } else if (strcmp(where, "post-scale") == 0) {
        counter = &s_diagPost;
    } else if (strcmp(where, "to-output") == 0) {
        counter = &s_diagOut;
    } else if (strcmp(where, "restored") == 0) {
        counter = &s_diagRest;
    }
    if (counter == nullptr || *counter >= cap) {
        return;
    }
    (*counter)++;
    auto dir = CCDirector::get();
    auto view = CCEGLView::sharedOpenGLView();
    CCSize fsz = view->getFrameSize();
    CCSize wsz = dir->getWinSize();
    CCSize dsz = view->getDesignResolutionSize();
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    GLint tex = 0, prog = 0, activeTex = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    skkbot2::log::info(
        "[RENDER DIAG {}] frame={}x{} win={}x{} design={}x{} csf={:.3f} "
        "vp={}x{}@{}x{} tex={} active={} prog={}",
        where, fsz.width, fsz.height, wsz.width, wsz.height, dsz.width,
        dsz.height, dir->getContentScaleFactor(), vp[2], vp[3], vp[0], vp[1],
        tex, activeTex, prog);
}

void silentChangeSize(CCSize size) {
    // A zero/negative size must never reach the cocos view: setFrameSize(0,0)
    // + updateScreenScale corrupts the window model (frame 0x0, win size NaN)
    // for the whole render (2026/09/16, 03:33 session). Callers that have no
    // real window size yet (useWindowViewForUi firing before acquireView from
    // the imgui swapBuffers hook) no-op here instead of zeroing the view.
    if (size.width <= 0.0f || size.height <= 0.0f) {
        skkbot2::log::warn("[RENDER] silentChangeSize({}, {}) rejected - "
                           "refusing to zero the cocos view",
                           size.width, size.height);
        return;
    }
    auto director = CCDirector::sharedDirector();
    auto view = CCEGLView::sharedOpenGLView();
    diagViewState("pre-set", 100);     // TEMP DIAG
    view->CCEGLViewProtocol::setFrameSize(size.width, size.height);
    director->updateScreenScale(size);
    diagViewState("post-scale", 100);  // TEMP DIAG
    director->setViewport();
    director->setProjection(kCCDirectorProjection2D);
    glViewport(0, 0, size.width, size.height);
}

// Keeps the background-shader layer consistent with the new view size (its
// render texture must match what the scene draws into). Recreated at the
// output size during a render, restored in stop(). No-op when no level is
// live. Guarded against redundant calls: once a size is applied, repeated
// requests with the SAME size are no-ops вЂ” the active render toggles the
// shader-layer texture between the window and output size once per
// pause/resume transition, not once per frame.
static CCSize s_shaderLayerSize(-1.0f, -1.0f);
void resizeShaderLayer(CCSize size) {
    auto* gjbgl = GJBaseGameLayer::get();
    if (!gjbgl || !gjbgl->m_shaderLayer) {
        return;
    }
    if (s_shaderLayerSize == size) {
        return;
    }
    s_shaderLayerSize = size;
    ShaderLayer* sh = gjbgl->m_shaderLayer;
    auto winSize = CCDirector::get()->getWinSize();

    sh->m_screenSize = size;
    sh->m_scaleFactor = size.height / winSize.height;
    sh->m_aspectRatio = size.width / size.height;

    float csf = CCDirector::get()->getContentScaleFactor();
    CCDirector::get()->setContentScaleFactor(1.0f);
    sh->m_renderTexture->release();
    sh->m_renderTexture = CCRenderTexture::create(
        size.width, size.height, kCCTexture2DPixelFormat_RGBA8888);
    sh->m_renderTexture->retain();
    CCDirector::get()->setContentScaleFactor(csf);

    sh->m_sprite->setTexture(sh->m_renderTexture->getSprite()->getTexture());
    sh->m_textureContentSize = sh->m_sprite->getTexture()->getContentSize();
    sh->m_targetTextureSize = size;
    sh->m_targetTextureSizeExtra = CCSize(0.0f, 0.0f);
    sh->m_sprite->setTextureRect(
        {-1.0f, -1.0f, size.width + 2.0f, size.height + 2.0f});
    sh->m_state.m_textureScaleX = size.width / winSize.width;
    sh->m_state.m_textureScaleY = size.height / winSize.height;
}

// Physical client size of the GL window (layout points can differ from pixels
// under DPI scaling); returns the latest known size when extraction fails.
static CCSize getWindowFramebufferSize() {
    HDC deviceContext = wglGetCurrentDC();
    if (!deviceContext) {
        return CCSize(0.0f, 0.0f);
    }

    HWND window = WindowFromDC(deviceContext);
    RECT clientRect{};
    if (!window || !GetClientRect(window, &clientRect)) {
        return CCSize(0.0f, 0.0f);
    }

    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0) {
        return CCSize(0.0f, 0.0f);
    }

    return CCSize(static_cast<float>(width), static_cast<float>(height));
}

}  // namespace

void Renderer::acquireView() {
    const CCSize renderSize(static_cast<float>(mWidth),
                            static_cast<float>(mHeight));
    if (m_viewResized) {
        // Already holding the output view (or restored by pause and re-armed
        // on the first render frame after resume): re-assert it in case GD
        // touched the view size in between. The shader-layer render texture is
        // kept at the output size for the whole render.
        silentChangeSize(renderSize);
        resizeShaderLayer(renderSize);
        return;
    }

    auto view = CCDirector::get()->getOpenGLView();
    m_windowW = view->getFrameSize().width;
    m_windowH = view->getFrameSize().height;
    CCSize fb = getWindowFramebufferSize();
    if (fb.width > 0.0f && fb.height > 0.0f) {
        m_windowFbW = fb.width;
        m_windowFbH = fb.height;
    } else {
        m_windowFbW = m_windowW;
        m_windowFbH = m_windowH;
    }
    if (m_windowW <= 0.0f || m_windowH <= 0.0f) {
        // The cocos frame may still read 0x0 after a previous
        // silentChangeSize corruption; the physical client rect (WinAPI)
        // is authoritative. Keep the tracked window size real so
        // drawPreview()/restoreView() never get a 0x0 rect.
        m_windowW = m_windowFbW;
        m_windowH = m_windowFbH;
    }
    m_viewToFbScaleX = m_windowFbW > 0.0f ? m_windowW / m_windowFbW : 1.0f;
    m_viewToFbScaleY = m_windowFbH > 0.0f ? m_windowH / m_windowFbH : 1.0f;
    m_viewResized = true;

    silentChangeSize(renderSize);
    resizeShaderLayer(renderSize);
    skkbot2::log::info(
        "[RENDER] window captured {}x{} (fb {}x{}) -> output view {}x{} "
        "held for whole render",
        m_windowW, m_windowH, m_windowFbW, m_windowFbH, mWidth, mHeight);
}

void Renderer::restoreView() {
    if (!m_viewResized) {
        return;
    }
    m_viewResized = false;
    silentChangeSize(CCSize(m_windowW, m_windowH));
    resizeShaderLayer(CCSize(m_windowW, m_windowH));
}

void Renderer::useWindowViewForUi() {
    if (!mRecording.load()) {
        return;
    }
    // Only switch the view back to the window size after acquireView() has
    // armed the output view (m_viewResized) AND captured a real window size.
    // Before the first render-frame drawScene (level intro, m_started=false)
    // this runs from the imgui swapBuffers hook on EVERY wall frame - without
    // this guard it passed (m_windowW=0, m_windowH=0) and zeroed the cocos
    // view (2026/09/16, 03:33 session: "window captured 0x0", black preview,
    // black restored view).
    if (!m_viewResized || m_windowW <= 0.0f || m_windowH <= 0.0f) {
        return;
    }
    // Menu-space swap (no shader-layer resize — the texture must stay at the
    // output size for the visit()-based grab that runs between menu frames).
    silentChangeSize(CCSize(m_windowW, m_windowH));
    // Cocos's CCDirector::setViewport()/setProjection(), invoked by
    // silentChangeSize above, reset the GL blend FUNCTION to cocos's default
    // premultiplied pair (GL_ONE / GL_ONE_MINUS_SRC_ALPHA). imgui-cocos sets
    // its own (GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA) exactly ONCE at the top
    // of drawFrame — BEFORE this callback runs — and never re-asserts it, so
    // the whole menu composites with the leaked premultiplied func.
    // ImGui vertex colors are NOT premultiplied, so with GL_ONE every
    // translucent pixel paints at ~2x intensity: anti-aliased glyph edges go
    // solid (text looks fat/garbled/"filled with one color") and the 14%-alpha
    // glass panels turn nearly opaque. This is why the UI only breaks while a
    // render is on — silentChangeSize runs every render frame (acquireView +
    // this), and never outside one. Re-assert imgui's func HERE (after the
    // swap) so renderFrame() below paints with correct alpha. Raw glBlendFunc
    // (not the cached ccGLBlendFunc): the scene path may have changed the real
    // GL state through the cache, and the wrapper would no-op if its cache
    // still holds the old value while the actual state differs.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // The render path leaves a non-cocos GL program bound (the capture/Y-UV
    // shaders, set through raw glUseProgram). gd-imgui-cocos renders the menu
    // through shader->use() -> the CACHED ccGLUseProgram wrapper, which no-ops
    // when its cache still holds the imgui program while a different one is
    // actually bound. The whole menu then composites with a program that has
    // no matching vertex attrs/uniforms - every widget paints invisible while
    // our blur callback (raw GL, own program) stays visible.
    // Fix: bind a DIFFERENT cocos program THROUGH the cached path. This makes
    // the wrapper's cache agree with the real GL state (both hold the other
    // program), so renderFrame()'s use() of the imgui program sees a cache
    // mismatch and issues a REAL glUseProgram. (Raw glUseProgram(0) +
    // ccGLInvalidateStateCache did NOT fix it: 2026/09/27 session, still
    // invisible; invalidate apparently does not reset the program slot.)
    if (auto* other = CCShaderCache::sharedShaderCache()->programForKey(
            kCCShader_PositionColor)) {
        other->use();
    }
}

void Renderer::storeWindowFbSize(float fbW, float fbH) {
    m_windowFbW = fbW;
    m_windowFbH = fbH;
    // Keep the layout size in sync so restoreView() restores the CURRENT
    // window, not the one captured at render start.
    m_windowW = fbW * m_viewToFbScaleX;
    m_windowH = fbH * m_viewToFbScaleY;
}

// --- CCEGLView hooks: swallow resize events while the render view is locked.
bool Renderer::handleFrameSizeChange(float width, float height) {
    if (!m_viewResized) {
        return false;
    }
    auto fb = getWindowFramebufferSize();
    if (fb.width > 0.0f && fb.height > 0.0f) {
        storeWindowFbSize(fb.width, fb.height);
    } else {
        m_windowW = width;
        m_windowH = height;
    }
    return true;
}

bool Renderer::handleFramebufferSizeChange(int width, int height) {
    if (!m_viewResized) {
        return false;
    }
    storeWindowFbSize(static_cast<float>(width), static_cast<float>(height));
    return true;
}

bool Renderer::handleWindowSizeChange() {
    if (!m_viewResized) {
        return false;
    }
    auto fb = getWindowFramebufferSize();
    if (fb.width > 0.0f && fb.height > 0.0f) {
        storeWindowFbSize(fb.width, fb.height);
    }
    return true;
}

Renderer::~Renderer() {
    if (!mRecording.load() && !m_encThread.joinable()) {
        return;
    }
    // Process/DLL teardown safety net: do NOT touch ffmpeg here. By the time
    // the static destructor runs (dllmain_crt_process_detach) the NVENC/CUDA
    // stack may be partially unloaded, and freeEncoder() has crashed with
    // av_log/strlen inside avcodec_free_context. Renderers are stopped by
    // stop() during the normal session (onQuit/levelComplete/drive); if that
    // never happened (game killed mid-render), just leak вЂ” the OS reclaims
    // everything on process exit.
    {
        std::lock_guard lk(m_qMu);
        m_qClosed = true;
    }
    m_qCv.notify_all();
    if (m_encThread.joinable()) {
        m_encThread.detach();
    }
    mRecording.store(false);
}

bool Renderer::loadFFmpeg() {
    if (m_ffLoaded) {
        return true;
    }
    std::string dir = Mod::get()->getResourcesDir().string();
    if (!ffLoadFunctions(&m_ff, dir, m_error)) {
        skkbot2::log::error("[RENDER] {}", m_error);
        return false;
    }
    m_ffLoaded = true;
    skkbot2::log::info("[RENDER] FFmpeg loaded from {}", dir);

    // Enumerate available encoders once (used by the GUI combo), keeping only
    // a curated short-list (software + the three hardware GPU families).
    // The full FFmpeg build ships dozens of encoders (mpeg4/prores/mjpeg/vc1
    // and legacy variants) that make the dropdown unusable.
    const char* kCurated[] = {
        "libx264",    // software, always present
        "libx265",    // software HEVC
        "h264_nvenc", // NVIDIA
        "hevc_nvenc",
        "h264_amf",   // AMD
        "hevc_amf",
        "h264_qsv",   // Intel
        "hevc_qsv",
        "libsvtav1",  // AV1 software
        "libvpx-vp9", // VP9 software
    };
    m_encoderNames.clear();
    const AVCodec* c = nullptr;
    void* it = nullptr;
    while ((c = m_ff.av_codec_iterate(&it)) != nullptr) {
        if (m_ff.av_codec_is_encoder(c) && c->name) {
            const std::string n = c->name;
            for (const char* want : kCurated) {
                if (n == want) {
                    m_encoderNames.push_back(n);
                    break;
                }
            }
        }
    }
    skkbot2::log::info("[RENDER] {} encoder(s) available", m_encoderNames.size());
    return true;
}

bool Renderer::startEncoder(const std::string& outPath) {
    if (!m_ffLoaded) {
        return false;
    }
    int ret = m_ff.avformat_alloc_output_context2(&mFmt, nullptr,
                                                  nullptr, outPath.c_str());
    if (ret < 0 || !mFmt) {
        m_error = "avformat_alloc_output_context2: " + ffErrString(&m_ff, ret);
        return false;
    }

    // No auto-config: only the encoder the user picked in the GUI is used
    // (the GUI resolves the default h264_nvenc/libx264 on first open). No
    // silent fallback to another codec — if the pick is unavailable or fails
    // to initialize, the render fails with a clear error.
    if (m_encoderSel.empty()) {
        m_error = "no video encoder selected";
        return false;
    }
    mCodec = m_ff.avcodec_find_encoder_by_name(m_encoderSel.c_str());
    if (!mCodec) {
        m_error = "video encoder not found in FFmpeg: " + m_encoderSel;
        return false;
    }
    mCtx = m_ff.avcodec_alloc_context3(mCodec);
    if (!mCtx) {
        m_error = "avcodec_alloc_context3 failed: " + m_encoderSel;
        return false;
    }
    mCtx->codec_id = mCodec->id;
    mCtx->width = static_cast<int>(mWidth);
    mCtx->height = static_cast<int>(mHeight);
    // NV12 is the native capture format (matches the GPU pipeline). Encoders
    // that reject it (software x265/AV1/VP9/mpeg4/mjpeg/prores) get a
    // swscale-based NV12 -> YUV420P conversion; the frames fed to the codec
    // always match the pinned format.
    mCtx->pix_fmt = AV_PIX_FMT_NV12;
    mCtx->time_base = AVRational{1, mFps};
    mCtx->framerate = AVRational{mFps, 1};
    mCtx->max_b_frames = 0;  // b-frames hurt realtime preview
    mCtx->gop_size = mFps * 2;
    mCtx->bit_rate = static_cast<int64_t>(mBitrateMbps) * 1000000LL;
    // Cap the output tightly: rc_max_rate == bit_rate makes NVENC run in
    // CBR mode (the target is hit exactly, same as xdBot's renders).
    // The VBV buffer is half a second so bitrate spikes stay near the
    // target instead of ballooning to ~2x (measured with a 1s buffer).
    // libx264 keeps a small headroom so ABR doesn't overshoot wildly on
    // complex content (measured 48 Mbps from a 20 Mbps target).
    if (mCodec->name && std::string(mCodec->name).find("nvenc") != std::string::npos) {
        mCtx->rc_max_rate = mCtx->bit_rate;
        mCtx->rc_buffer_size = static_cast<int>(mCtx->bit_rate / 2);
    } else {
        mCtx->rc_max_rate = mCtx->bit_rate * 12 / 10;
        mCtx->rc_buffer_size = static_cast<int>(mCtx->bit_rate);
    }
    mCtx->color_primaries = AVCOL_PRI_BT709;
    mCtx->color_trc = AVCOL_TRC_BT709;
    mCtx->colorspace = AVCOL_SPC_BT709;
    mCtx->color_range = AVCOL_RANGE_MPEG;
    if (mFmt->oformat->flags & AVFMT_GLOBALHEADER) {
        mCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* opts = nullptr;
    std::string cname = mCodec->name;
    if (cname.find("nvenc") != std::string::npos) {
        m_ff.av_dict_set(&opts, "preset", "p4", 0);  // balanced speed/quality
    } else if (cname == "libx264" || cname == "libx265") {
        m_ff.av_dict_set(&opts, "threads", "8", 0);
    }
    ret = m_ff.avcodec_open2(mCtx, mCodec, &opts);
    m_ff.av_dict_free(&opts);
    if (ret < 0) {
        // The codec refused NV12 — retry with YUV420P + swscale conversion.
        m_ff.avcodec_free_context(&mCtx);
        mCtx = m_ff.avcodec_alloc_context3(mCodec);
        if (!mCtx) {
            m_error = "avcodec_alloc_context3 failed: " + m_encoderSel;
            return false;
        }
        mCtx->codec_id = mCodec->id;
        mCtx->width = static_cast<int>(mWidth);
        mCtx->height = static_cast<int>(mHeight);
        mCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        mCtx->time_base = AVRational{1, mFps};
        mCtx->framerate = AVRational{mFps, 1};
        mCtx->max_b_frames = 0;
        mCtx->gop_size = mFps * 2;
        mCtx->bit_rate = static_cast<int64_t>(mBitrateMbps) * 1000000LL;
        mCtx->rc_max_rate = mCtx->bit_rate * 12 / 10;
        mCtx->rc_buffer_size = static_cast<int>(mCtx->bit_rate);
        mCtx->color_primaries = AVCOL_PRI_BT709;
        mCtx->color_trc = AVCOL_TRC_BT709;
        mCtx->colorspace = AVCOL_SPC_BT709;
        mCtx->color_range = AVCOL_RANGE_MPEG;
        if (mFmt->oformat->flags & AVFMT_GLOBALHEADER) {
            mCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        AVDictionary* opts2 = nullptr;
        if (cname == "libx264" || cname == "libx265") {
            m_ff.av_dict_set(&opts2, "threads", "8", 0);
        }
        ret = m_ff.avcodec_open2(mCtx, mCodec, &opts2);
        m_ff.av_dict_free(&opts2);
        if (ret < 0) {
            std::string err = ffErrString(&m_ff, ret);
            m_ff.avcodec_free_context(&mCtx);
            mCtx = nullptr;
            mCodec = nullptr;
            m_error = "encoder failed to initialize: " + m_encoderSel +
                      " (NV12 and YUV420P both rejected: " + err + ")";
            return false;
        }
        m_sws = m_ff.sws_getContext(
            static_cast<int>(mWidth), static_cast<int>(mHeight), AV_PIX_FMT_NV12,
            static_cast<int>(mWidth), static_cast<int>(mHeight), AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            m_error = "sws_getContext failed (NV12 -> YUV420P)";
            m_ff.avcodec_free_context(&mCtx);
            mCtx = nullptr;
            mCodec = nullptr;
            return false;
        }
        m_convFrame = m_ff.av_frame_alloc();
        if (!m_convFrame) {
            m_error = "conversion frame alloc";
            m_ff.avcodec_free_context(&mCtx);
            mCtx = nullptr;
            mCodec = nullptr;
            m_ff.sws_freeContext(m_sws);
            m_sws = nullptr;
            return false;
        }
        m_convFrame->format = AV_PIX_FMT_YUV420P;
        m_convFrame->width = static_cast<int>(mWidth);
        m_convFrame->height = static_cast<int>(mHeight);
        if (m_ff.av_frame_get_buffer(m_convFrame, 32) < 0) {
            m_error = "conversion frame buffer alloc";
            m_ff.avcodec_free_context(&mCtx);
            mCtx = nullptr;
            mCodec = nullptr;
            m_ff.sws_freeContext(m_sws);
            m_sws = nullptr;
            m_ff.av_frame_free(&m_convFrame);
            m_convFrame = nullptr;
            return false;
        }
    }
    mUsingNvenc = (cname.find("nvenc") != std::string::npos);
    m_encoderUsed = cname;
    skkbot2::log::info("[RENDER] encoder: {} ({})", m_encoderUsed,
                       mUsingNvenc ? "hardware" : "software");

    AVStream* vs = m_ff.avformat_new_stream(mFmt, mCodec);
    if (!vs) {
        m_error = "avformat_new_stream";
        return false;
    }
    ret = m_ff.avcodec_parameters_from_context(vs->codecpar, mCtx);
    if (ret < 0) {
        m_error = "avcodec_parameters_from_context";
        return false;
    }
    vs->time_base = mCtx->time_base;

    // Audio stream (level music + SFX captured from FMOD's master group).
    // If the audio codec fails to init the render still proceeds with video
    // only вЂ” audio is a bonus, not a blocker.
    if (m_audioEnabled && AudioCapture::get().active()) {
        if (!setupAudioStream()) {
            skkbot2::log::warn("[RENDER] audio stream disabled: {}",
                               m_error);
            m_audioEnabled = false;
        }
    }

    ret = m_ff.avio_open(&mFmt->pb, outPath.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        m_error = "avio_open: " + ffErrString(&m_ff, ret);
        return false;
    }

    // Plain (non-fragmented) mp4: av_write_trailer writes the moov box with
    // the full stts/stco sample tables at the END of the file. Fragmented
    // output (frag_keyframe+delay_moov) is NOT used: it leaves an empty moov
    // stub at the start that the trailer must rewrite in place, and when that
    // rewrite fails the file keeps empty tables - players then play the two
    // tracks with broken timing (audio drift). Plain mp4 cannot end up with
    // empty tables: the whole moov is produced by the trailer in one pass.
    ret = m_ff.avformat_write_header(mFmt, nullptr);
    if (ret < 0) {
        m_error = "avformat_write_header: " + ffErrString(&m_ff, ret);
        return false;
    }
    // TEMP DIAG: ground truth for the fps investigation вЂ” the codec and
    // stream time bases the muxer is actually working with.
    skkbot2::log::info(
        "[RENDER DIAG] tb: codec={}/{} stream={}/{} fps={} nvenc={}",
        mCtx->time_base.num, mCtx->time_base.den,
        mFmt->streams[0]->time_base.num, mFmt->streams[0]->time_base.den,
        mFps, mUsingNvenc ? 1 : 0);

    mPkt = m_ff.av_packet_alloc();
    if (!mPkt) {
        m_error = "packet alloc";
        return false;
    }
    // NV12: plane 0 = Y (w*h), plane 1 = interleaved UV (w*h/2). The capture
    // pipeline produces this layout directly on the GPU вЂ” no CPU conversion.
    // mCtx->pix_fmt matches the frames (NV12): FFmpeg does NOT validate, so a
    // mismatch would silently corrupt the picture (muxed planes, merged
    // frames вЂ” bug "3 СЂР°Р·РјС‹С‚С‹С… С†РІРµС‚Р° РІ РІРёРґРµРѕ"). NVENC and libx264 both
    // accept NV12 natively.
    // Refcounted input frame pool: the encode thread copies each ring slot's
    // NV12 data into a writable frame buffer, so the ring slot is released as
    // soon as the copy finishes while NVENC's ASYNC copy of the frame data
    // keeps running safely (the AVFrame refcount keeps the buffer alive until
    // the codec drops it). The old design handed the raw ring pointer to
    // avcodec_send_frame and reused the slot immediately вЂ” NVENC was still
    // reading the buffer while the next capture overwrote it, tearing frames
    // (row ripple, color mixing, ghosting).
    m_yuvIdx = 0;
    for (auto& f : mYuvFrames) {
        f = m_ff.av_frame_alloc();
        if (!f) {
            m_error = "frame alloc";
            return false;
        }
        f->format = AV_PIX_FMT_NV12;
        f->width = static_cast<int>(mWidth);
        f->height = static_cast<int>(mHeight);
        f->linesize[0] = static_cast<int>(mWidth);
        f->linesize[1] = static_cast<int>(mWidth);
        if (m_ff.av_frame_get_buffer(f, 32) < 0) {
            m_error = "frame buffer alloc";
            return false;
        }
    }

    return true;
}

void Renderer::encodeOneFrame(uint8_t* nv12, int64_t pts) {
    if (!mPkt || !mCtx || !mFmt) {
        return;
    }

    auto drainPackets = [this]() {
        while (true) {
            int ret = m_ff.avcodec_receive_packet(mCtx, mPkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                break;
            }
            const int64_t rawPts = mPkt->pts;
            const int64_t rawDts = mPkt->dts;
            m_ff.av_packet_rescale_ts(mPkt, mCtx->time_base,
                                      mFmt->streams[0]->time_base);
            if (m_diagPackets < 10) {
                skkbot2::log::info(
                    "[RENDER DIAG] pkt#{} sent={} raw pts={} dts={} "
                    "dur={} -> resc pts={} dts={}",
                    m_diagPackets++, m_lastSentPts, rawPts, rawDts,
                    mPkt->duration, mPkt->pts, mPkt->dts);
            }
            mPkt->stream_index = 0;
            int wret = 0;
            {
                std::lock_guard<std::mutex> lock(m_muxMu);
                wret = m_ff.av_interleaved_write_frame(mFmt, mPkt);
            }
            if (wret < 0) {
                skkbot2::log::error("[RENDER] write_frame: {}",
                                    ffErrString(&m_ff, wret));
            }
            m_ff.av_packet_unref(mPkt);
        }
    };

    // NV12 planes come straight from the GPU capture pipeline (already
    // flipped by the vertex shader).
    const size_t ySize = static_cast<size_t>(mWidth) * mHeight;
    AVFrame* f = mYuvFrames[m_yuvIdx];
    m_yuvIdx = (m_yuvIdx + 1) % ENC_FRAMES;
    if (!f) {
        return;
    }
    // Copy the ring slot's NV12 data into the frame's own buffer BEFORE the
    // slot is released. If NVENC still holds a reference (async copy in
    // flight), make_writable allocates a fresh buffer for the next frame
    // instead of reusing it вЂ” the data the codec is reading can never be
    // overwritten.
    if (m_ff.av_frame_make_writable(f) < 0) {
        skkbot2::log::error("[RENDER] av_frame_make_writable failed вЂ” frame "
                            "dropped");
        return;
    }
    // Row-wise copy: robust against linesize padding from av_frame_get_buffer
    // alignment (all GUI resolutions are 16:9 multiples of 64, so in practice
    // linesize == width, but this stays correct either way).
    for (int r = 0; r < static_cast<int>(mHeight); r++) {
        std::memcpy(f->data[0] + static_cast<size_t>(r) * f->linesize[0],
                    nv12 + static_cast<size_t>(r) * mWidth, mWidth);
    }
    for (int r = 0; r < static_cast<int>(mHeight) / 2; r++) {
        std::memcpy(f->data[1] + static_cast<size_t>(r) * f->linesize[1],
                    nv12 + ySize + static_cast<size_t>(r) * mWidth, mWidth);
    }
    // Strict sequential pts (frame units): the codec and the muxer then see a
    // perfectly uniform 1/mFps grid from pts 0, so the output is EXACTLY the
    // configured fps. (See m_videoPts comment in renderer.hpp.)
    const int64_t sentPts = m_videoPts++;
    f->pts = sentPts;
    m_lastSentPts = sentPts;

    // Encoders that rejected NV12 get a swscale conversion into the pinned
    // YUV420P frame before send. Software encoders are synchronous, so the
    // single reusable m_convFrame can never be read while it is rewritten.
    AVFrame* sendF = f;
    if (m_sws && m_convFrame) {
        m_ff.sws_scale(m_sws,
                       reinterpret_cast<const uint8_t* const*>(f->data),
                       f->linesize, 0, static_cast<int>(mHeight),
                       m_convFrame->data, m_convFrame->linesize);
        m_convFrame->pts = sentPts;
        sendF = m_convFrame;
    }

    // Drain whatever the codec already finished, then send. If the codec
    // queue is full (EAGAIN), drain again and retry briefly вЂ” never drop a
    // frame, never spin forever.
    drainPackets();
    int sendRet = 0;
    for (int attempt = 0; attempt < 500; attempt++) {
        sendRet = m_ff.avcodec_send_frame(mCtx, sendF);
        if (sendRet == 0) {
            break;
        }
        if (sendRet == AVERROR(EAGAIN)) {
            drainPackets();
            continue;
        }
        skkbot2::log::error("[RENDER] send_frame: {}", ffErrString(&m_ff, sendRet));
        return;
    }
    if (sendRet == AVERROR(EAGAIN)) {
        skkbot2::log::error("[RENDER] send_frame stayed EAGAIN вЂ” frame dropped");
        return;
    }
    drainPackets();
    mEncodedFrames.fetch_add(1, std::memory_order_release);
}

void Renderer::flush() {
    if (!mFmt || !mCtx) {
        return;
    }
    m_ff.avcodec_send_frame(mCtx, nullptr);
    while (true) {
        int ret = m_ff.avcodec_receive_packet(mCtx, mPkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }
        m_ff.av_packet_rescale_ts(mPkt, mCtx->time_base,
                                  mFmt->streams[0]->time_base);
        mPkt->stream_index = 0;
        {
            std::lock_guard<std::mutex> lock(m_muxMu);
            m_ff.av_interleaved_write_frame(mFmt, mPkt);
        }
        m_ff.av_packet_unref(mPkt);
    }
    // Flush the audio codec too (the last captured blocks are still in its
    // delay). Runs on the encode thread after the main thread has stopped
    // pumping (mRecording is already false in stop()).
    flushAudio();
    {
        std::lock_guard<std::mutex> lock(m_muxMu);
        int ret = m_ff.av_write_trailer(mFmt);
        if (ret < 0) {
            skkbot2::log::error("[RENDER] av_write_trailer: {}",
                                ffErrString(&m_ff, ret));
        }
    }
}

void Renderer::freeEncoder() {
    if (mPkt) {
        m_ff.av_packet_free(&mPkt);
        mPkt = nullptr;
    }
    if (mCtx) {
        m_ff.avcodec_free_context(&mCtx);
        mCtx = nullptr;
    }
    if (m_sws) {
        m_ff.sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_convFrame) {
        m_ff.av_frame_free(&m_convFrame);
        m_convFrame = nullptr;
    }
    for (auto& f : mYuvFrames) {
        if (f) {
            m_ff.av_frame_free(&f);
            f = nullptr;
        }
    }
    if (mFmt) {
        if (mFmt->pb) {
            m_ff.avio_close(mFmt->pb);
        }
        m_ff.avformat_free_context(mFmt);
        mFmt = nullptr;
    }
    mCodec = nullptr;
    freeAudio();
}

void Renderer::applySettings(const RenderSettings& s) {
    mWidth = s.width;
    mHeight = s.height;
    mFps = s.fps;
    if (mFps < 1) mFps = 1;
    if (mFps > 240) mFps = 240;
    mBitrateMbps = s.bitrateMbps;
    if (mBitrateMbps < 1) mBitrateMbps = 1;
    if (mBitrateMbps > 300) mBitrateMbps = 300;
    m_audioWanted = s.audioEnabled;
    m_audioEnabled = s.audioEnabled;
    m_audioCodecSel = s.audioCodec.empty() ? std::string("aac") : s.audioCodec;
    m_audioBitrate = s.audioBitrate;
    if (m_audioBitrate < 32) m_audioBitrate = 32;
    if (m_audioBitrate > 1024) m_audioBitrate = 1024;
    m_musicVolume = std::clamp(s.musicVolume, 0.0f, 1.0f);
    m_sfxVolume = std::clamp(s.sfxVolume, 0.0f, 1.0f);
    m_fadeInSeconds = s.fadeInSeconds;
    if (m_fadeInSeconds < 0.0) m_fadeInSeconds = 0.0;
    if (m_fadeInSeconds > 10.0) m_fadeInSeconds = 10.0;
    m_fadeOutSeconds = s.fadeOutSeconds;
    if (m_fadeOutSeconds < 0.0) m_fadeOutSeconds = 0.0;
    if (m_fadeOutSeconds > 10.0) m_fadeOutSeconds = 10.0;
    m_audioFadeInSec = s.audioFadeInSec;
    if (m_audioFadeInSec < 0.0) m_audioFadeInSec = 0.0;
    if (m_audioFadeInSec > 10.0) m_audioFadeInSec = 10.0;
    m_audioFadeOutSec = s.audioFadeOutSec;
    if (m_audioFadeOutSec < 0.0) m_audioFadeOutSec = 0.0;
    if (m_audioFadeOutSec > 10.0) m_audioFadeOutSec = 10.0;
}

Renderer::RenderSettings Renderer::settings() const {
    return RenderSettings{mWidth, mHeight, mFps, mBitrateMbps, m_audioWanted,
                          m_audioCodecSel, m_audioBitrate, m_musicVolume,
                          m_sfxVolume, m_fadeInSeconds, m_fadeOutSeconds,
                          m_audioFadeInSec, m_audioFadeOutSec};
}

bool Renderer::hasAudioCodec(const std::string& name) const {
    if (!m_ffLoaded || name.empty()) {
        return false;
    }
    return m_ff.avcodec_find_encoder_by_name(name.c_str()) != nullptr;
}

bool Renderer::setupAudioStream() {
    auto& ac = AudioCapture::get();
    m_audioSampleRate = ac.sampleRate();
    m_audioChannels = ac.channels();
    m_audioCodecUsed.clear();

    // The capture pipeline is stereo-only (AudioCapture::attach() forces the
    // offline mixer to FMOD_SPEAKERMODE_STEREO); anything else would need a
    // downmix the encoder path no longer performs.
    if (m_audioChannels != 2) {
        m_error = "audio: expected a stereo mix, got " +
                  std::to_string(m_audioChannels) + " channels";
        return false;
    }

    // Prefer the user's pick, fall back to aac (always present in a full
    // gyan.dev build). libopus only accepts its fixed set of rates
    // (48k/24k/16k/12k/8k), so a non-48000 FMOD rate disqualifies it вЂ” the
    // other codecs accept any rate (the PCM is fed at the FMOD rate, no
    // resampling is done anymore).
    std::vector<std::string> candidates;
    candidates.push_back(m_audioCodecSel);
    candidates.push_back("aac");

    m_audioCodec = nullptr;
    m_audioCtx = nullptr;
    for (const auto& name : candidates) {
        const AVCodec* codec = m_ff.avcodec_find_encoder_by_name(name.c_str());
        if (!codec) {
            continue;
        }
        if (name == "libopus" && m_audioSampleRate != 48000) {
            continue;
        }
        AVCodecContext* ctx = m_ff.avcodec_alloc_context3(codec);
        if (!ctx) {
            continue;
        }
        ctx->codec_type = AVMEDIA_TYPE_AUDIO;
        // Stereo, with the format fixed per codec: aac/mp3/flac take planar
        // float (FLTP), libopus interleaved float (FLT). This mirrors what
        // writeAudioBlock() produces вЂ” no swr involved (the FFmpeg
        // resampler used to crash mid-render on a channel-layout mismatch).
        ctx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        ctx->sample_rate = m_audioSampleRate;
        ctx->sample_fmt =
            (name == "libopus") ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP;
        ctx->bit_rate = static_cast<int64_t>(m_audioBitrate) * 1000LL;
        ctx->time_base = AVRational{1, m_audioSampleRate};
        if (mFmt->oformat->flags & AVFMT_GLOBALHEADER) {
            ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (m_ff.avcodec_open2(ctx, codec, nullptr) < 0) {
            m_ff.avcodec_free_context(&ctx);
            continue;
        }
        m_audioCodec = codec;
        m_audioCtx = ctx;
        m_audioCodecUsed = name;
        break;
    }
    if (!m_audioCtx) {
        m_error = "no usable audio codec (tried: " + m_audioCodecSel +
                  ", aac)";
        return false;
    }

    // swr is gone: writeAudioBlock() converts FLT -> FLTP manually. The
    // frame buffer below is allocated in the codec's format/rate, so the
    // conversion can write straight into it.
    const int frameSize = m_audioCtx->frame_size > 0 ? m_audioCtx->frame_size
                                                     : 1024;
    m_audioFrame = m_ff.av_frame_alloc();
    m_audioPkt = m_ff.av_packet_alloc();
    if (!m_audioFrame || !m_audioPkt) {
        m_error = "audio frame/pkt alloc";
        freeAudio();
        return false;
    }
    m_audioFrame->format = m_audioCtx->sample_fmt;
    m_audioFrame->ch_layout = m_audioCtx->ch_layout;
    m_audioFrame->nb_samples = frameSize;
    if (m_ff.av_frame_get_buffer(m_audioFrame, 32) < 0) {
        m_error = "audio frame buffer alloc";
        freeAudio();
        return false;
    }

    // The stream is created LAST: if anything above failed, no empty audio
    // track is left behind in the container.
    m_audioStream = m_ff.avformat_new_stream(mFmt, m_audioCodec);
    if (!m_audioStream) {
        m_error = "avformat_new_stream (audio)";
        freeAudio();
        return false;
    }
    if (m_ff.avcodec_parameters_from_context(m_audioStream->codecpar,
                                             m_audioCtx) < 0) {
        m_error = "avcodec_parameters_from_context (audio)";
        freeAudio();
        return false;
    }
    m_audioStream->time_base = m_audioCtx->time_base;

    m_audioPts = 0;
    m_audioPackets = 0;
    m_audioAccum.clear();
    m_audioFadeDiag = 0;
    skkbot2::log::info(
        "[RENDER] audio stream: {} @ {} kbps, {} Hz x {} ch ({} fps codec) "
        "fade: in={:.1f}s out={:.1f}s",
        m_audioCodecUsed, m_audioBitrate, m_audioSampleRate, m_audioChannels,
        frameSize, m_audioFadeInSec, m_audioFadeOutSec);
    return true;
}

void Renderer::audioPump(double levelTime) {
    if (!m_audioEnabled || !m_audioCtx) {
        return;
    }
    AudioCapture::get().pump(levelTime);
    audioEncodePending();
}

void Renderer::audioEncodePending() {
    if (!m_audioCtx || !m_audioPkt) {
        return;
    }
    const int frameSize =
        m_audioCtx->frame_size > 0 ? m_audioCtx->frame_size : 1024;
    AudioCapture::get().drain(
        m_audioAccum, frameSize,
        [this](const float* block, int64_t pts) {
            this->writeAudioBlock(block, pts);
        });
}

void Renderer::writeAudioBlock(const float* block, int64_t pts) {
    if (!m_audioCtx || !m_audioFrame || !m_audioPkt) {
        return;
    }
    const int frameSize =
        m_audioCtx->frame_size > 0 ? m_audioCtx->frame_size : 1024;

    // Audio fade: multiply the whole block by a ramp gain computed from this
    // block's time. Fade-in ramps up over the first m_audioFadeInSec of the
    // track; fade-out mirrors the video trailer, ramping to silence over the
    // last m_audioFadeOutSec (m_audioFadeOutRemain is decremented in the same
    // trailer path the video fade uses). 0.0 settings leave the block intact.
    double gain = 1.0;
    if (m_audioFadeInSec > 0.0 && m_audioSampleRate > 0) {
        const double t = static_cast<double>(pts) / m_audioSampleRate;
        if (t < m_audioFadeInSec) {
            gain = std::max(t / m_audioFadeInSec, 0.0);
        }
    }
    if (m_audioFadeOutSec > 0.0 && m_audioFadeOutRemain < m_audioFadeOutSec) {
        gain = std::min(gain, std::max(m_audioFadeOutRemain / m_audioFadeOutSec, 0.0));
    }
    const float gainF = static_cast<float>(gain);

    // AUDIO-FADE DIAGNOSTIC (bug: audio fade sounds silent/full):
    // log the first block ever, then notable transitions, so a test render
    // shows whether the ramp is engaged and where it points.
    if (m_audioFadeDiag < 4 || (gain < 1.0 && m_audioFadeDiag < 12 &&
        (pts / 2048) > static_cast<int64_t>(m_audioFadeDiag))) {
        skkbot2::log::info(
            "[RENDER] audio-fade diag: blk#{} pts={} t={:.3f}s gain={:.3f} "
            "fade(in={:.1f}s out={:.1f}s remain={:.3f}) sr={} ctxSR={} fmt={}",
            m_audioFadeDiag, pts,
            m_audioSampleRate > 0 ? static_cast<double>(pts) / m_audioSampleRate
                                  : 0.0,
            gain, m_audioFadeInSec, m_audioFadeOutSec, m_audioFadeOutRemain,
            m_audioSampleRate,
            m_audioCtx ? m_audioCtx->sample_rate : -1,
            m_audioCtx ? static_cast<int>(m_audioCtx->sample_fmt) : -1);
        ++m_audioFadeDiag;
    }

    // Manual format conversion (swr removed вЂ” it crashed mid-render). The
    // block is interleaved stereo FLT (the DSP output); the codec takes
    // either planar FLTP (aac/mp3/flac) or interleaved FLT (libopus) вЂ”
    // exactly what setupAudioStream() pinned on the context.
    if (m_audioCtx->sample_fmt == AV_SAMPLE_FMT_FLT) {
        if (gainF != 1.0f) {
            float* out = reinterpret_cast<float*>(m_audioFrame->data[0]);
            const int n = frameSize * 2;
            for (int i = 0; i < n; ++i) out[i] = block[i] * gainF;
        } else {
            std::memcpy(m_audioFrame->data[0], block,
                        static_cast<size_t>(frameSize) * 2 * sizeof(float));
        }
    } else {
        float* left = reinterpret_cast<float*>(m_audioFrame->data[0]);
        float* right = reinterpret_cast<float*>(m_audioFrame->data[1]);
        for (int i = 0; i < frameSize; ++i) {
            left[i] = block[i * 2] * gainF;
            right[i] = block[i * 2 + 1] * gainF;
        }
    }

    m_audioFrame->pts = pts;
    int ret = m_ff.avcodec_send_frame(m_audioCtx, m_audioFrame);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        skkbot2::log::error("[RENDER] audio send_frame: {}",
                            ffErrString(&m_ff, ret));
        return;
    }
    while (true) {
        ret = m_ff.avcodec_receive_packet(m_audioCtx, m_audioPkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }
        m_ff.av_packet_rescale_ts(m_audioPkt, m_audioCtx->time_base,
                                  m_audioStream->time_base);
        m_audioPkt->stream_index = m_audioStream->index;
        {
            std::lock_guard<std::mutex> lock(m_muxMu);
            int wret = m_ff.av_interleaved_write_frame(mFmt, m_audioPkt);
            if (wret < 0) {
                skkbot2::log::error("[RENDER] audio write_frame: {}",
                                    ffErrString(&m_ff, wret));
            }
        }
        m_ff.av_packet_unref(m_audioPkt);
        m_audioPackets++;
    }
}

void Renderer::flushAudio() {
    if (!m_audioCtx || !m_audioPkt || !m_audioStream) {
        return;
    }
    m_ff.avcodec_send_frame(m_audioCtx, nullptr);
    while (true) {
        int ret = m_ff.avcodec_receive_packet(m_audioCtx, m_audioPkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }
        m_ff.av_packet_rescale_ts(m_audioPkt, m_audioCtx->time_base,
                                  m_audioStream->time_base);
        m_audioPkt->stream_index = m_audioStream->index;
        {
            std::lock_guard<std::mutex> lock(m_muxMu);
            int wret = m_ff.av_interleaved_write_frame(mFmt, m_audioPkt);
            if (wret < 0) {
                skkbot2::log::error("[RENDER] audio flush write: {}",
                                    ffErrString(&m_ff, wret));
            }
        }
        m_ff.av_packet_unref(m_audioPkt);
        m_audioPackets++;
    }
}

void Renderer::freeAudio() {
    if (m_audioPkt) {
        m_ff.av_packet_free(&m_audioPkt);
        m_audioPkt = nullptr;
    }
    if (m_audioFrame) {
        m_ff.av_frame_free(&m_audioFrame);
        m_audioFrame = nullptr;
    }
    if (m_audioCtx) {
        m_ff.avcodec_free_context(&m_audioCtx);
        m_audioCtx = nullptr;
    }
    m_audioCodec = nullptr;
    m_audioStream = nullptr;
    m_audioAccum.clear();
}

bool Renderer::begin(PlayLayer* layer) {
    if (mRecording.load()) {
        return true;
    }
    // A previous render may have been killed mid-view-swap (or its restore
    // never ran), leaving the view state stale. A fresh render re-acquires the
    // CURRENT window size on its first drive frame, so reset here - otherwise
    // a stale m_viewResized=true could re-apply a stale (or zero) window size
    // and corrupt the new session's preview/restore math.
    m_viewResized = false;
    m_windowW = 0.0f;
    m_windowH = 0.0f;
    m_windowFbW = 0.0f;
    m_windowFbH = 0.0f;
    // The previous encoder thread may still be draining after an aborted
    // render (stop() detaches it when it is too slow). It owns the CURRENT
    // mFmt/mCtx/m_audioCtx and frees them itself - starting a new render
    // would reassign those members under it, and it would free the new
    // session's contexts (or write the new session's frames into the old
    // file). Refuse until it has finished; this also re-opens the render the
    // moment a stuck-but-recovering encoder completes.
    if (m_encRunning.load()) {
        m_error = "previous render's encoder thread is still finishing (it "
                  "was too slow) - wait a moment and try again";
        skkbot2::log::error("[RENDER] {}", m_error);
        return false;
    }
    if (!layer || !loadFFmpeg()) {
        return false;
    }

    // MANUAL LEVEL START (user's choice, 24.09): begin() ARMS the render
    // (encoder, audio capture, view lock) but never launches or restarts the
    // level. If the level is paused or has not started, drive() and the
    // drawScene hook hold all captures (the pause menu never reaches the
    // video — drive() drops the pending frame and the hook draws the real
    // scene), so the render simply waits until the user starts/resumes the
    // level themselves. This replaces the old auto-restart that closed the
    // pause menu and called resetLevel()/resetLevelFromStart() on START.

    if (mWidth < 64 || mHeight < 64 || (mWidth % 2) != 0 || (mHeight % 2) != 0) {
        m_error = "invalid resolution " + std::to_string(mWidth) + "x" + std::to_string(mHeight);
        skkbot2::log::error("[RENDER] {}", m_error);
        return false;
    }
    if (mFps < 1 || mFps > 240) {
        m_error = "invalid fps " + std::to_string(mFps);
        skkbot2::log::error("[RENDER] {}", m_error);
        return false;
    }

    namespace fs = std::filesystem;
    auto vidDir = Mod::get()->getSaveDir() / "videos";
    std::error_code ec;
    fs::create_directories(vidDir, ec);

    auto now = std::chrono::system_clock::now();
    auto nowTT = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &nowTT);
    char tbuf[32];
    std::strftime(tbuf, sizeof(tbuf), "%Y%m%d_%H%M%S", &tm);
    std::string levelName = layer->m_level ? layer->m_level->m_levelName
                                           : std::string("level");
    for (auto& ch : levelName) {
        if (ch == ' ' || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    std::string outPath = (vidDir / (levelName + "_" + tbuf + ".mp4")).string();
    m_lastOutputPath = outPath;

    // Seed the runtime audio latch from the SETTING. m_audioEnabled is the
    // "capture attached for THIS render" flag and was left false by the
    // previous stop(); without re-seeding, every render after the first one
    // would start video-only unless a render setting happened to be edited
    // in between (which re-runs applySettings and sets it back).
    m_audioEnabled = m_audioWanted;

    // Audio capture attaches BEFORE the encoder so the audio stream can be
    // configured with the FMOD mix format (rate/channels). If FMOD is not
    // available the render simply continues video-only.
    if (m_audioEnabled) {
        if (AudioCapture::get().attach(m_musicVolume, m_sfxVolume)) {
            skkbot2::log::info("[RENDER] audio capture ready");
            skkbot2::log::info("[RENDER] render volume: music={:.2f} sfx={:.2f} "
                               "(restored to the game's own on stop)",
                               m_musicVolume, m_sfxVolume);
        } else {
            skkbot2::log::warn("[RENDER] audio capture unavailable вЂ” render "
                               "will be video-only");
            m_audioEnabled = false;
        }
    }

    // v59.7: kick the song from 0 AFTER attach (which clears m_allAudioPaused
    // and unpauses the master group). FMODAudioEngine::update() re-pauses every
    // channel each frame while m_allAudioPaused==true — attach() is the gate
    // that clears it, so the engine start MUST come after. Force position 0 with
    // the real musicID so block 0 of the NRT pump carries the level start.
    //
    // After starting, cycle pause→resume to force the FMOD channel from
    // STOPPED to PLAYING. Plain startMusic() does not restart a stopped
    // channel — only GD's resumeAllAudio() path (called by PauseLayer::onResume)
    // handles stopped-channel restart. Empirically this is what makes the
    // difference between silence and audible output.
    if (auto* pl = PlayLayer::get()) {
        if (pl->m_musicPrepared) {
            pl->startMusic();
            if (auto* engine = FMODAudioEngine::get()) {
                engine->startMusic(0, 0, 0, 0, false, getLastMusicId(),
                                   false, false);
                engine->pauseAllAudio();
                engine->resumeAllAudio();
                engine->m_allAudioPaused = false;
                skkbot2::log::info("[RENDER] audio kick: start+pause/resume "
                                   "from 0 (musicID={})",
                                   getLastMusicId());
            }
        } else {
            armMusicRestart();
            skkbot2::log::info("[RENDER] audio kick: song not prepared — "
                               "deferred restart armed");
        }
    } else {
        skkbot2::log::info("[RENDER] audio kick: no PlayLayer");
    }

    if (!startEncoder(outPath)) {
        skkbot2::log::error("[RENDER] start failed: {}", m_error);
        if (m_audioEnabled) {
            AudioCapture::get().detach();
            m_audioEnabled = false;
        }
        return false;
    }

// Silicate-style silent view change: the cocos view is pointed at the
    // OUTPUT size for the whole render (acquireView, called from the first
    // render-frame drawScene). This keeps the visit()-based capture re-rendering
    // the level at exactly m_width x m_height with the exact sim camera — the
    // visible window/zoom can no longer influence the video. The real window
    // dimensions are captured inside acquireView() for drawPreview()'s letterbox
    // math and later restore.
    {
        diagViewState("begin", 100);    // TEMP DIAG
        skkbot2::log::info("[RENDER] capture targets {}x{} (view armed "
                           "silicate-style on next render frame)", mWidth, mHeight);
    }

    auto* nEngine = ReplayEngine::get();
    mTps = std::max(1.0, nEngine->runtimeTickRate());

    mCapture.init(mWidth, mHeight);

    // Allocate the ring of NV12 buffers (one full frame each).
    size_t frameSize = static_cast<size_t>(mWidth) * mHeight * 3 / 2;
    m_ring.resize(RING_SIZE);
    for (auto& buf : m_ring) {
        buf.resize(frameSize);
    }
    m_ringPts.assign(RING_SIZE, -1);
    m_rd = 0;
    m_wr = 0;
    m_qClosed = false;

    mLastPts = -1;
    m_videoPts = 0;          // fresh pts grid per session (see m_videoPts
                             // comment in renderer.hpp)
    mLastFramed = 0;
    mGrabFailStreak = 0;
    mRingFullFrames = 0;
    // Silicat-style live trailer: after the level completes we keep capturing
    // the REAL scene for kTrailerHold seconds (fading out over the last
    // m_fadeOutSeconds), so the video ends on live footage, not a frozen
    // frame. The trailer is extended when a fade-out is configured so the
    // ramp has room to play.
    mTrailerRemain = std::max(kTrailerHold, m_fadeOutSeconds);
    // Audio fade-out rides the same trailer timeline as the video fade.
    m_audioFadeOutRemain = std::max(kTrailerHold, m_audioFadeOutSec);
    m_pendingFrame = false;
    m_pendingPts = -1;
    m_pendingFadeOut = 1.0f;
    m_renderStartWall = nowS();
    m_driveCalls = 0;
    m_stepCount = 0;
    m_grabFailTotal = 0;
    m_pendingDrops = 0;
    m_grabMs = 0.0;
    m_grabN = 0;
    m_lastStatsWall = 0.0;
    m_firstCaptureLogged = false;
    m_pauseLogged = false;
    // The render drive feeds the scheduler 1/fps through the SAME step-grid
    // pipeline the macro was recorded on (SpeedControlScheduler ->
    // advanceNormalScheduler -> applyCustomTickPlan), so the expected-ticks
    // patch must stay ENABLED: applyCustomTickPlan keeps g_expectedTicks in
    // sync and getModifiedDelta freezes the substep delta to 1/tps. Disabling
    // the patch here makes GD fall back to its vanilla 240-Hz grid, which
    // desyncs the input dispatch (and the video timeline) whenever tps != 240.
    mRecording.store(true);
    mCapturedFrames.store(0);
    mEncodedFrames.store(0);

    m_encThread = std::thread(&Renderer::encodeThreadMain, this);

    skkbot2::log::info("[RENDER] Started -> {} ({}x{}@{}fps, {} Mbps, {}{})",
                       outPath, mWidth, mHeight, mFps, mBitrateMbps,
                       m_encoderUsed.empty() ? "auto" : m_encoderUsed,
                       m_audioEnabled
                           ? (std::string(", audio ") + m_audioCodecUsed +
                              " " + std::to_string(m_audioBitrate) + " kbps")
                           : "");
    return true;
}

uint8_t* Renderer::tryAcquireSlot() {
    std::lock_guard lk(m_qMu);
    if (m_qClosed || (m_wr + 1) % RING_SIZE == m_rd) {
        return nullptr;
    }
    return m_ring[m_wr].data();
}

bool Renderer::ringFull() {
    std::lock_guard lk(m_qMu);
    return (m_wr + 1) % RING_SIZE == m_rd;
}

void Renderer::commitSlot(int64_t pts) {
    {
        std::lock_guard lk(m_qMu);
        m_ringPts[m_wr] = pts;
        m_wr = (m_wr + 1) % RING_SIZE;
    }
    m_qCv.notify_one();
}

void Renderer::encodeThreadMain() {
    m_encRunning.store(true);
    while (true) {
        uint8_t* rgba = nullptr;
        int64_t pts = 0;
        {
            std::unique_lock lk(m_qMu);
            m_qCv.wait(lk, [this] {
                return m_qClosed && m_rd == m_wr || m_rd != m_wr;
            });
            if (m_qClosed && m_rd == m_wr) {
                break;
            }
            rgba = m_ring[m_rd].data();
            pts = m_ringPts[m_rd];
            m_rd = (m_rd + 1) % RING_SIZE;
        }
        encodeOneFrame(rgba, pts);
        m_qCv.notify_one();
    }
    // Queue drained and closed: flush the codec + muxer and write the trailer.
    // Doing it here (on the encode thread) keeps all ffmpeg context usage on
    // a single thread; stop() only waits for us (bounded). freeEncoder() runs
    // here too - the thread that owns the ffmpeg contexts (mFmt/mCtx/audio)
    // frees them, so the main thread must NOT free them after the wait, and
    // must NOT start a new render before m_encRunning clears.
    if (mFmt) {
        flush();
        skkbot2::log::info(
            "[RENDER] Done -> {} ({} frames, {}x{}, {} Mbps)",
            m_lastOutputPath, mEncodedFrames.load(), mWidth, mHeight,
            mBitrateMbps);
    }
    freeEncoder();
    m_encRunning.store(false);
    m_qCv.notify_all();
}

void Renderer::drive(GJBaseGameLayer* layer,
                     std::function<void(float)> stepOne) {
    if (!mRecording.load()) {
        return;
    }
    auto* pl = static_cast<PlayLayer*>(layer);
    // Silicate parity: feed the scheduler 1/fps (e.g. 1/60) — GD's own
    // getModifiedDelta quantises to the 240-Hz tick grid internally.
    // Feeding 1/tps directly can leave the internal accumulator stranded at
    // zero if the first fractional tick rounds down.
    const float simDt = 1.0f / static_cast<float>(mFps);

    // Encoder backpressure (Silicat's tryBeginFrame): if every ring buffer is
    // still being encoded, HOLD the sim at the current position instead of
    // stepping (never block the main thread). The boundary is retried on the
    // next wall frame, so no pts is ever lost and the render rate naturally
    // caps at the encoder rate. If the ring stays full for ~30s of wall
    // frames the encoder is dead (NVENC session or muxer failure) вЂ” abort
    // instead of freezing the game forever.
    if (ringFull()) {
        if (++mRingFullFrames > 60 * 30) {
            skkbot2::log::error(
                "[RENDER] encoder stalled: capture ring full for ~30s вЂ” "
                "aborting render");
            stop();
        }
        return;
    }
    mRingFullFrames = 0;

    auto ptsOfFrame = [&](uint64_t f) {
        return static_cast<int64_t>(static_cast<double>(f - 1) *
                                    static_cast<double>(mFps) / mTps);
    };

    // Fast-forward render (same idea as Silicat): NO wall-clock pacing. Every
    // drive() call advances exactly one video frame's worth of sim
    // (mTps/mFps ticks) and queues one capture; the game loop runs uncapped
    // (~180Hz on this machine), so a 37.5s level renders in ~13s wall instead
    // of real-time. pts are derived from the sim tick count, so the video
    // always plays at 1x regardless of the wall rate вЂ” speed only affects how
    // long the render takes. Captures read the backbuffer right after the
    // scene is drawn for this frame (onFrameDraw), so every frame is fresh вЂ”
    // no re-sampling of a stale present like the old sync-grab in drive().
    m_driveCalls++;
    const double nowW = nowS();
    // Periodic diagnostics (~1 dump / 5s wall): render speed (level/wall вЂ”
    // >1x is fast-forward, expected), sim rate and drive rate.
    if (nowW - m_lastStatsWall >= 5.0) {
        m_lastStatsWall = nowW;
        double wall = nowW - m_renderStartWall;
        double lvlT = pl->m_gameState.m_levelTime;
        double speed = wall > 0.0 ? lvlT / wall : 0.0;
        skkbot2::log::info(
            "[RENDER] stats: wall={:.1f}s level={:.2f}s speed={:.2f}x "
            "ticks={} tickRate={:.0f}/s driveHz={:.0f} captured={} "
            "encoded={} grabFails={} drops={} started={}",
            wall, lvlT, speed, m_stepCount,
            wall > 0.0 ? m_stepCount / wall : 0.0,
            wall > 0.0 ? static_cast<double>(m_driveCalls) / wall : 0.0,
            mCapturedFrames.load(), mEncodedFrames.load(), m_grabFailTotal,
            m_pendingDrops, pl->m_started);
        if (speed < 0.85) {
            skkbot2::log::warn(
                "[RENDER] render slower than real-time ({:.2f}x) вЂ” the "
                "encoder or vsync is bottlenecking; video is still correct",
                speed);
        }
    }

    // The render ends when the LEVEL completes, not when the macro's last
    // click is delivered: after the final input the player still flies to
    // the finish line (~1-2s), and that tail must be in the video.
    // NOTE: do NOT use bot.isPlaybackFinished() here вЂ” the bot's auto-stop
    // on macro end calls setState(Disabled), which RESETS m_playbackFinished
    // to false, so the flag would never be true at this point.
    bool levelDone = pl->m_hasCompletedLevel;
    if (levelDone) {
        // Live trailer (Silicat-style): keep capturing the REAL scene for
        // kTrailerHold seconds after the level completes, fading to black
        // over the last m_fadeOutSeconds (m_pendingFadeOut is baked into the
        // Y/UV passes at issue time, like the fade-in). The sim is not
        // stepped here вЂ” GD animates the end of the level itself вЂ” the video
        // just keeps grabbing fresh frames at video fps.
        if (mTrailerRemain <= 0.0) {
            stop();
            return;
        }
        if (m_pendingFrame) {
            return;  // previous trailer frame not drawn yet
        }
        mLastPts++;
        m_pendingPts = mLastPts;
        m_pendingFadeOut =
            m_fadeOutSeconds > 0.0 && mTrailerRemain <= m_fadeOutSeconds
                ? static_cast<float>(mTrailerRemain / m_fadeOutSeconds)
                : 1.0f;
        m_pendingFrame = true;
        mTrailerRemain -= 1.0 / mFps;
        m_audioFadeOutRemain -= 1.0 / mFps;
        // The level's own end music (if any) keeps playing at video speed вЂ”
        // pump to the trailer's video time so the audio tail matches the
        // last frames.
        audioPump(static_cast<double>(mLastPts) / mFps);
        return;
    }

    uint64_t f = computeRenderFrame(pl, mTps);
    int64_t curPts = ptsOfFrame(f);
    // Do not capture while the level is paused вЂ” the pause menu must not
    // end up in the video. Drop any pending capture too: a boundary queued
    // before pausing would be grabbed once onFrameDraw() next runs, baking
    // the pause menu into the video.
    if (pl->m_isPaused) {
        m_pendingFrame = false;
        m_pendingFadeOut = 1.0f;
        return;
    }
    if (mLastPts < 0 && pl->m_gameState.m_levelTime <= 0.0) {
        // One-shot diagnostic: log the first warm-up entry so we know
        // whether the gate let us through and what the level state is.
        if (m_stepCount == 0) {
            skkbot2::log::info(
                "[RENDER] warmup: started={} paused={} levelTime={:.6f} "
                "hasCompleted={} simDt={:.6f}",
                pl->m_started, pl->m_isPaused,
                pl->m_gameState.m_levelTime,
                pl->m_hasCompletedLevel, simDt);
        }
        mLastFramed = 0;
        stepOne(simDt);
        m_stepCount++;
        return;
    }
    if (m_pendingFrame) {
        // The draw callback has not consumed the previous boundary yet (the
        // loop fired faster than the frame drew). Wait instead of queueing a
        // new capture вЂ” overwriting would lose a pts and stutter the video.
        return;
    }
    int ticks = 0;
    while (curPts <= mLastPts) {
        if (pl->m_isPaused) {
            return;  // can't safely sim while paused; wait next wall frame
        }
        // After the macro's last click the bot no longer steers the
        // player: if it dies before the finish line the level resets and
        // completion will never fire. Give it a few seconds of sim time,
        // then give up. (Playback exhausted marks "macro consumed" вЂ” inputs
        // delivered, the player must reach the end on its own.)
        bool macroConsumed = false;
        if (auto* eng = ReplayEngine::get()) {
            macroConsumed = eng->playbackExhaustionNotified ||
                            (eng->activeSk && eng->executeIndex >= eng->activeSk->inputs.size());
        }
        if (macroConsumed && ++mPostMacroTicks > 240 * 15) {
            skkbot2::log::error(
                "[RENDER] player never reached the end after the macro "
                "finished (died before the finish line?) вЂ” stopping");
            stop();
            return;
        }
        // Safety net: if level time stops advancing while the game is NOT
        // paused (e.g. a practice checkpoint respawn reset m_levelTime
        // backward), stepping forever would hang the whole game. Cap the
        // ticks and bail out вЂ” the render simply stalls instead.
        if (++ticks > 2400) {
            skkbot2::log::error(
                "[RENDER] drive stalled: levelTime not advancing "
                "(f={} pts={}/{}) вЂ” bailing out of this wall frame",
                f, curPts, mLastPts);
            return;
        }
        if (f < mLastFramed) {
            // Level time rewound (practice respawn): rewind the video
            // timeline to the new position and restart capture from there.
            skkbot2::log::info("[RENDER] level time rewound f={}->{} "
                               "(respawn) вЂ” video timeline resynced",
                               mLastFramed, f);
            mLastPts = curPts;
            mLastFramed = f;
            return;
        }
        mLastFramed = f;
        stepOne(simDt);
        m_stepCount++;
        f = computeRenderFrame(pl, mTps);
        curPts = ptsOfFrame(f);
    }

    // Boundary reached: queue the capture. The actual grab runs in
    // onFrameDraw() (start of the ImGui draw callback, right after the game
    // drew this wall frame's scene and BEFORE the GUI overlay is rendered) вЂ”
    // so the video gets a fresh frame with no bot menu baked in.
    mLastPts = curPts;
    m_pendingPts = curPts;
    m_pendingFadeOut = 1.0f;
    m_pendingFrame = true;

    // Advance the audio mixer to the level's game time (pump is a no-op
    // when the time has not moved: pause, encoder backpressure, respawn
    // rewind). The FMOD output is NOSOUND_NRT, so the mixer only advances
    // through this pump вЂ” the audio is generated at the level's own speed
    // and plays back at exactly 1x in the video.
    audioPump(pl->m_gameState.m_levelTime);
}

void Renderer::onFrameDraw() {
    if (!mRecording.load() || !m_pendingFrame) {
        return;
    }

    uint8_t* slot = tryAcquireSlot();
    if (!slot) {
        // Encoder backpressure: every ring buffer is still being encoded.
        // Do NOT consume the pending boundary вЂ” drive() holds the sim while
        // the ring is full, so this boundary is retried on the next wall
        // frame with no pts lost (Silicat-style skip, main thread never
        // blocks).
        return;
    }
m_pendingFrame = false;

    // Silicate-style: the cocos view already points at the OUTPUT size for the
    // whole render (acquireView applied it once on the first render frame).
    // acquireView() re-asserts it if the pause menu / game restored the window
    // size between captures (restoreView()/pause path) — the visit()-based grab
    // must always re-render the scene into the output-sized FBO at the output
    // projection.
    acquireView();

    // Silicate-style fade from black at the start of the video and back to
    // black at the very end: the scene RGB is multiplied by the ramp inside
    // the Y/UV shader passes, so the fade is baked into the frame at ISSUE
    // time (m_fadeInSeconds fade-in, plus the trailer fade-out ramp
    // m_pendingFadeOut computed by drive()). A frame is always rendered under
    // the fade its pts deserves. 0.0 = no fade.
    const double kFadeSeconds = m_fadeInSeconds;
    float fade = 1.0f;
    if (kFadeSeconds > 0.0) {
        const int fadeFrames = static_cast<int>(kFadeSeconds * mFps);
        fade = m_pendingPts >= fadeFrames
                   ? 1.0f
                   : (m_pendingPts <= 0
                          ? 0.0f
                          : static_cast<float>(m_pendingPts) /
                                static_cast<float>(fadeFrames));
    }
    fade = std::min(fade, m_pendingFadeOut);
    mCapture.setFade(fade);

    // grab() issues the readback of the CURRENT scene (labeled m_pendingPts)
    // and hands back the frame captured two requests ago with ITS pts вЂ” the
    // encoder always gets the timestamp of the pixels it is actually given
    // (Silicat's m_ptsQueue push/pop pairing).
    int64_t gotPts = -1;
    const double g0 = nowS();
    const bool grabbed = mCapture.grab(slot, m_pendingPts, &gotPts);

    if (!grabbed) {
        m_grabFailTotal++;
        // Pipeline not warmed up yet or GPU temporarily stalled: skip
        // this boundary вЂ” the next wall frame retries it. NEVER commit a
        // pts=-1 frame here: the muxer rejects it and the endless
        // "Invalid argument" stream corrupts the container.
        if (++mGrabFailStreak <= 5 || (mGrabFailStreak % 30) == 0) {
            skkbot2::log::warn("[RENDER] grab failed (streak={} pts={} "
                               "captured={} encoded={})",
                               mGrabFailStreak, m_pendingPts,
                               mCapturedFrames.load(), mEncodedFrames.load());
        }
        if (mGrabFailStreak >= 30) {
            // The capture pipeline never produced a single frame: abort
            // instead of freezing the game at frame 1 forever (a failing
            // grab leaves the sim unstepped and the level never starts).
            skkbot2::log::error(
                "[RENDER] capture pipeline produced no frames ({} "
                "consecutive grab failures) вЂ” aborting render",
                mGrabFailStreak);
            stop();
        }
        return;
    }
    m_grabMs += (nowS() - g0) * 1e3;
    m_grabN++;
    mGrabFailStreak = 0;
    if (!m_firstCaptureLogged) {
        m_firstCaptureLogged = true;
        skkbot2::log::info(
            "[RENDER] first frame captured pts={} grabMs={:.1f} "
            "nv12Size={}",
            gotPts, m_grabMs / m_grabN, mCapture.m_nv12Size);
    }
    commitSlot(gotPts);
    mCapturedFrames.fetch_add(1, std::memory_order_release);
    if ((mCapturedFrames.load() % 120) == 0) {
        skkbot2::log::info("[RENDER] progress: captured={} encoded={} pts={}",
                           mCapturedFrames.load(), mEncodedFrames.load(),
                           gotPts);
    }
}

void Renderer::drawPreview() {
    mCapture.displayPreview(m_windowW, m_windowH);
}

void Renderer::restoreViewForPauseMenu() {
    if (!mRecording.load()) {
        return;
    }
    // Restore the window-size view so the pause menu (and imgui-cocos, which
    // sizes the bot menu off getFrameSize()) render at full window size. The
    // output view is re-applied lazily by acquireView() on the next render
    // frame (inside onFrameDraw -> acquireView before the next capture).
    restoreView();
    // Logged ONCE per render session: with the game paused this path runs
    // every wall frame (60+ lines/sec of identical "pause menu" spam drowned
    // the whole 2026/09/16 log). The transition notice is enough; the level
    // paused mid-render also stops captures (drive() drops the pending frame).
    if (!m_pauseLogged) {
        m_pauseLogged = true;
        skkbot2::log::info("[RENDER] view -> {}x{} (pause menu)", m_windowW,
                           m_windowH);
    }
}

void Renderer::stop() {
    if (!mRecording.load()) {
        return;
    }
    mRecording.store(false);

    {
        std::lock_guard lk(m_qMu);
        m_qClosed = true;
    }
    m_qCv.notify_all();
    if (m_encThread.joinable()) {
        // Bounded tail: normally the encoder thread drains + flushes + writes
        // the trailer in milliseconds. A stuck encoder (e.g. hevc_nvenc
        // hanging on the first send_frame at 4K) must NEVER freeze the whole
        // game again - the 2026/09/16 session black-screened every button
        // because this join() blocked the main thread forever (the finished
        // mp4 was a bare 44-byte header). Wait a few seconds, then detach and
        // let the thread finish in the background (it owns its ffmpeg cleanup
        // now). A new render is refused until m_encRunning clears.
        std::unique_lock lk(m_qMu);
        const bool done = m_qCv.wait_for(
            lk, std::chrono::seconds(kStopTailWaitSeconds),
            [this] { return !m_encRunning.load(); });
        if (done) {
            m_encThread.join();
            skkbot2::log::info("[RENDER] encoder thread joined");
        } else {
            m_encThread.detach();
            skkbot2::log::warn(
                "[RENDER] encoder thread too slow to finish in {}s - detached; "
                "file keeps writing in the background, new renders are refused "
                "until it finishes",
                kStopTailWaitSeconds);
        }
    }

    // End-of-render diagnostics: the speed factor is the single best check
    // that pacing worked (must be ~1.0); anything else = stutter/stretch.
    double wall = nowS() - m_renderStartWall;
    double lvl = mLastPts > 0 ? static_cast<double>(mLastPts) / mFps : 0.0;
    skkbot2::log::info(
        "[RENDER] Summary: captured={} encoded={} wall={:.1f}s "
        "level={:.2f}s speed={:.2f}x ticks={} driveHz={:.0f} "
        "grabAvg={:.1f}ms grabFails={} drops={}{}",
        mCapturedFrames.load(), mEncodedFrames.load(), wall, lvl,
        wall > 0.0 ? lvl / wall : 0.0, m_stepCount,
        wall > 0.0 ? static_cast<double>(m_driveCalls) / wall : 0.0,
        m_grabN > 0 ? m_grabMs / m_grabN : 0.0, m_grabFailTotal,
        m_pendingDrops,
        m_audioPackets > 0
            ? (std::string(" audio=") + std::to_string(m_audioPackets) +
               "pk (" + m_audioCodecUsed + ")")
            : std::string(" audio=off"));

    // NOTE: ffmpeg contexts are freed by the encoder thread itself (it owns
    // them: flush + trailer + free). The main thread detaches/joins it above
    // and must NOT call freeEncoder() here.

    // Restore the FMOD output so normal audio plays again (and the DSP is
    // removed from the master group).
    if (m_audioEnabled) {
        AudioCapture::get().detach();
        m_audioEnabled = false;
    }

    // Restore the cocos view (winSize, projection) to the real window size
    // and rebuild the shader layer's render texture at the window size. During
    // the render the view was held at the OUTPUT size for its whole lifetime
    // (silicate acquireView) вЂ” swap it back so the normal game scene draws
    // correctly again. Must run on the GL/main thread вЂ” stop() is always
    // called from there. resizeShaderLayer no-ops if the size already matches.
    restoreView();
    skkbot2::log::info("[RENDER] view restored {}x{}", m_windowW, m_windowH);
}

}  // namespace render
}  // namespace skkbot2
