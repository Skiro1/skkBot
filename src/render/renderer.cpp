#include "renderer.hpp"

#include "core/hotpath.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/ShaderLayer.hpp>
#include <Geode/cocos/shaders/CCShaderCache.h>
#include <Geode/cocos/shaders/ccGLStateCache.h>

#include "../skkBot.hpp"
#include "../core/paths.hpp"
#include "../logger/logger.hpp"
#include "audiocap.hpp"
#include "hwprobe.hpp"
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

// Where the renderer looks for the FFmpeg DLLs, in order. The save-dir drop-in
// comes first on purpose:
//   * `geode/unzipped/<id>/resources/<id>` (Mod::get()->getResourcesDir()) is
//     where Geode unpacks the copy bundled inside the .geode -- but that whole
//     tree is DELETED when the game exits (LoaderImpl removes getModRuntimeDir
//     on unload), so it is a moving target: users cannot put a file there and
//     expect it to survive, and one failed unpack leaves nothing at all;
//   * `geode/mods/<id>/ffmpeg` is a plain folder next to replays/ and videos/,
//     which is what the RENDER tab's folder button opens and what a user can
//     actually find.
// The resources dir stays as the second candidate so a mod update that ships
// the DLLs keeps working without any manual step.
std::vector<std::filesystem::path> Renderer::ffmpegSearchPaths() {
    return skkbot2::paths::ffmpegSearchPaths();
}

std::filesystem::path Renderer::preferredFfmpegDir() {
    return skkbot2::paths::ffmpeg();
}

// Creates the drop-in folder and leaves a note in it. A user who was told "put
// the DLLs into geode/unzipped/..." has nowhere to look: that tree only exists
// while the game runs. This folder sits next to replays/ and videos/, is opened
// by the RENDER tab's folder button, and is the FIRST place the loader checks.
void Renderer::prepareFfmpegDropIn() {
    std::error_code ec;
    const auto dir = preferredFfmpegDir();
    std::filesystem::create_directories(dir, ec);
    const auto note = dir / "README.txt";
    if (std::filesystem::exists(note, ec)) {
        return;
    }
    std::ofstream out(note, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "FFmpeg DLLs for the skkBot renderer\n"
           "==================================\n\n"
           "Target folder for the 7 FFmpeg DLLs:\n"
           "  avutil-61.dll\n"
           "  swresample-7.dll\n"
           "  swscale-10.dll\n"
           "  avcodec-63.dll\n"
           "  avformat-63.dll\n"
           "  avfilter-12.dll\n"
           "  avdevice-63.dll\n\n"
           "You can download them automatically in the RENDER tab (press DOWNLOAD FFMPEG),\n"
           "or download dll.zip manually and extract the files directly here.\n\n"
           "Download mirror:\n"           "  - GitHub: https://github.com/Skiro1/skkBot/releases/download/ffmpeg/dll.zip\n\n"
           "After placing the files, click RELOAD FFMPEG in the RENDER tab.\n";
}

bool Renderer::loadFFmpeg() {
    if (m_ffLoaded) {
        return true;
    }
    prepareFfmpegDropIn();
    // Every candidate is tried and logged: "FFmpeg does not work" without the
    // list of places that were checked is exactly the report that made this
    // audit necessary.
    std::string lastError;
    for (auto const& dir : ffmpegSearchPaths()) {
        if (!ffLoadFunctions(&m_ff, dir, lastError)) {
            skkbot2::log::info("[RENDER] FFmpeg not in {}: {}", skk::pathToUtf8(dir),
                               lastError);
            continue;
        }
        m_ffLoaded = true;
        m_ffDir = dir;
        skkbot2::log::info("[RENDER] FFmpeg loaded from {}", skk::pathToUtf8(dir));
        break;
    }
    if (!m_ffLoaded) {
        m_error = lastError.empty() ? std::string("FFmpeg not found")
                                    : lastError;
        skkbot2::log::error("[RENDER] {}", m_error);
        return false;
    }

    // One line, once per session: could a zero-copy GPU->NVENC path exist here?
    // Purely informational - nothing consumes it yet and no UI is wired up.
    // cuda is NOT universal, which is exactly why a future toggle must default
    // OFF, and the honest way to know is to look rather than assume.
    skkbot2::log::info("[RENDER] hw probe: {}",
                       hwProbeLine(probeHwFrames(m_ffDir)));

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
    std::string cname = mCodec->name ? mCodec->name : "";
    bool isNvenc = (cname.find("nvenc") != std::string::npos);

    auto fillContext = [&](bool cbr) {
        mCtx->codec_id = mCodec->id;
        mCtx->width = static_cast<int>(mWidth & ~1);
        mCtx->height = static_cast<int>(mHeight & ~1);
        mCtx->pix_fmt = AV_PIX_FMT_NV12;
        mCtx->time_base = AVRational{1, mFps};
        mCtx->framerate = AVRational{mFps, 1};
        mCtx->max_b_frames = 0;  // b-frames hurt realtime preview
        mCtx->gop_size = mFps;
        mCtx->bit_rate = static_cast<int64_t>(mBitrateMbps) * 1000000LL;
        if (cbr) {
            mCtx->rc_max_rate = mCtx->bit_rate;
            mCtx->rc_buffer_size = static_cast<int>(mCtx->bit_rate);
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
    };

    if (isNvenc) {
        struct NvencOpt {
            const char* preset;
            bool cbr;
            bool gpuAny;
        };
        const NvencOpt attempts[] = {
            { "p4",      true,  true },  // 1: Modern balanced preset + CBR + GPU probe
            { "medium",  true,  true },  // 2: Universal SDK preset + CBR + GPU probe
            { "default", false, true },  // 3: Default preset + VBR + GPU probe
            { nullptr,   false, false }  // 4: Minimal barebones fallback
        };

        bool opened = false;
        std::string lastErr;
        for (size_t att = 0; att < sizeof(attempts) / sizeof(attempts[0]); ++att) {
            if (att > 0) {
                m_ff.avcodec_free_context(&mCtx);
                mCtx = m_ff.avcodec_alloc_context3(mCodec);
                if (!mCtx) break;
            }
            fillContext(attempts[att].cbr);
            AVDictionary* opts = nullptr;
            if (attempts[att].preset) {
                m_ff.av_dict_set(&opts, "preset", attempts[att].preset, 0);
            }
            if (attempts[att].gpuAny) {
                m_ff.av_dict_set(&opts, "gpu", "any", 0);
            }
            clearLastFfLogMsg();
            ret = m_ff.avcodec_open2(mCtx, mCodec, &opts);
            m_ff.av_dict_free(&opts);
            if (ret >= 0) {
                opened = true;
                if (att > 0) {
                    skkbot2::log::info("[RENDER] NVENC initialized on fallback attempt {} (preset={})",
                                       att + 1, attempts[att].preset ? attempts[att].preset : "barebones");
                }
                break;
            }
            std::string ffMsg = getLastFfLogMsg();
            lastErr = !ffMsg.empty() ? ffMsg : ffErrString(&m_ff, ret);
            skkbot2::log::warn("[RENDER] NVENC init attempt {} (preset={}, cbr={}) failed: {} (ret={})",
                               att + 1, attempts[att].preset ? attempts[att].preset : "none",
                               attempts[att].cbr, lastErr, ret);
        }

        if (!opened) {
            m_ff.avcodec_free_context(&mCtx);
            mCtx = nullptr;
            mCodec = nullptr;
            m_error = "NVENC (" + m_encoderSel + ") failed to initialize: " + lastErr;
            if (m_encoderSel == "hevc_nvenc") {
                m_error += " [Try h264_nvenc or libx264]";
            }
            skkbot2::log::error("[RENDER] {}", m_error);
            return false;
        }
    } else {
        auto applyCodecOpts = [&](AVDictionary** dict) {
            const bool isQsv = cname.find("qsv") != std::string::npos;
            const bool isAmf = cname.find("amf") != std::string::npos;
            if (isQsv) {
                m_ff.av_dict_set(dict, "preset", "veryfast", 0);
                m_ff.av_dict_set(dict, "async_depth", "4", 0);
            } else if (isAmf) {
                m_ff.av_dict_set(dict, "quality", "speed", 0);
                m_ff.av_dict_set(dict, "usage", "lowlatency", 0);
            } else if (cname == "libx264" || cname == "libx265") {
                m_ff.av_dict_set(dict, "preset", "veryfast", 0);
                m_ff.av_dict_set(dict, "threads", "8", 0);
            }
        };

        fillContext(false);
        AVDictionary* opts = nullptr;
        applyCodecOpts(&opts);
        clearLastFfLogMsg();
        ret = m_ff.avcodec_open2(mCtx, mCodec, &opts);
        m_ff.av_dict_free(&opts);
        if (ret < 0) {
            // If specialized opts failed, retry barebones
            AVDictionary* bareOpts = nullptr;
            if (cname == "libx264" || cname == "libx265") {
                m_ff.av_dict_set(&bareOpts, "threads", "8", 0);
            }
            clearLastFfLogMsg();
            ret = m_ff.avcodec_open2(mCtx, mCodec, &bareOpts);
            m_ff.av_dict_free(&bareOpts);
        }
        if (ret < 0) {
            // The codec refused NV12 — retry with YUV420P + swscale conversion.
            m_ff.avcodec_free_context(&mCtx);
            mCtx = m_ff.avcodec_alloc_context3(mCodec);
            if (!mCtx) {
                m_error = "avcodec_alloc_context3 failed: " + m_encoderSel;
                return false;
            }
            mCtx->codec_id = mCodec->id;
            mCtx->width = static_cast<int>(mWidth & ~1);
            mCtx->height = static_cast<int>(mHeight & ~1);
            mCtx->pix_fmt = AV_PIX_FMT_YUV420P;
            mCtx->time_base = AVRational{1, mFps};
            mCtx->framerate = AVRational{mFps, 1};
            mCtx->max_b_frames = 0;
            mCtx->gop_size = mFps;
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
            applyCodecOpts(&opts2);
            clearLastFfLogMsg();
            ret = m_ff.avcodec_open2(mCtx, mCodec, &opts2);
            m_ff.av_dict_free(&opts2);
            if (ret < 0) {
                AVDictionary* bareOpts2 = nullptr;
                if (cname == "libx264" || cname == "libx265") {
                    m_ff.av_dict_set(&bareOpts2, "threads", "8", 0);
                }
                clearLastFfLogMsg();
                ret = m_ff.avcodec_open2(mCtx, mCodec, &bareOpts2);
                m_ff.av_dict_free(&bareOpts2);
            }
            if (ret < 0) {
                std::string ffMsg = getLastFfLogMsg();
                std::string err = !ffMsg.empty() ? ffMsg : ffErrString(&m_ff, ret);
                m_ff.avcodec_free_context(&mCtx);
                mCtx = nullptr;
                mCodec = nullptr;
                m_error = "encoder failed to initialize: " + m_encoderSel + " (" + err + ")";
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
                m_error = "av_frame_alloc failed for YUV420P conversion frame";
                m_ff.avcodec_free_context(&mCtx);
                mCtx = nullptr;
                mCodec = nullptr;
                return false;
            }
            m_convFrame->format = AV_PIX_FMT_YUV420P;
            m_convFrame->width = static_cast<int>(mWidth);
            m_convFrame->height = static_cast<int>(mHeight);
            ret = m_ff.av_frame_get_buffer(m_convFrame, 32);
            if (ret < 0) {
                m_error = "av_frame_get_buffer failed: " + ffErrString(&m_ff, ret);
                m_ff.av_frame_free(&m_convFrame);
                m_convFrame = nullptr;
                m_ff.avcodec_free_context(&mCtx);
                mCtx = nullptr;
                mCodec = nullptr;
                return false;
            }
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

    clearLastFfLogMsg();
    ret = m_ff.avio_open(&mFmt->pb, outPath.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        std::string ffMsg = getLastFfLogMsg();
        m_error = "avio_open: " + (!ffMsg.empty() ? ffMsg : ffErrString(&m_ff, ret));
        skkbot2::log::error("[RENDER] avio_open failed for '{}': {} (ret={})", outPath, m_error, ret);
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

void Renderer::encodeOneFrame(const uint8_t* nv12, int64_t pts, bool alreadyOwned) {
    if (!mPkt || !mCtx || !mFmt) {
        return;
    }

    auto drainPackets = [this]() {
        const double drainT0 = nowS();
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
        m_encDrainMs += (nowS() - drainT0) * 1e3;
    };

    // NV12 planes come straight from the GPU capture pipeline (already
    // flipped by the vertex shader).
    const size_t ySize = static_cast<size_t>(mWidth) * mHeight;
    AVFrame* f = mYuvFrames[m_yuvIdx];
    m_yuvIdx = (m_yuvIdx + 1) % ENC_FRAMES;
    if (!f) {
        return;
    }
    // Copy the grabbed NV12 into the frame's own buffer BEFORE the slot is
    // released. If NVENC still holds a reference (async copy in flight),
    // make_writable allocates a fresh buffer for the next frame instead of
    // reusing it вЂ” the data the codec is reading can never be overwritten.
    //
    // `nv12` points straight into a mapped PBO slot rather than into a ring
    // buffer of our own. The main thread USED to memcpy out of that mapping
    // into a ring buffer, and this used to memcpy the ring buffer in here -
    // two copies of the same bytes, one of them on the thread that also owns
    // GD's physics. There is one now, and it is here. `alreadyOwned` says the
    // caller already copied (the sync-readback fallback, which has to: the GL
    // context is on the main thread and nowhere else).
    if (m_ff.av_frame_make_writable(f) < 0) {
        skkbot2::log::error("[RENDER] av_frame_make_writable failed вЂ” frame "
                            "dropped");
        return;
    }
// The row-wise loop cost one memcpy call per row of both planes: at 4K
      // that is 2160 + 1080 = 3240 calls per frame to move 12.4 MB. glReadPixels
      // returns tightly packed rows with no padding (GL_PACK_ALIGNMENT is 1 for
      // the capture), and av_frame_get_buffer(f, 32) only pads linesize up to a
      // multiple of 32 - which every render resolution already is (3840, 2560,
      // 1920, 1280 are all multiples of 32). So the two planes are contiguous
      // and one memcpy each does the whole job.
      //
      // Verified against the log rather than assumed: linesize is not what we
      // set at line 670, it is what get_buffer computed afterwards, so the
      // equality is CHECKED, not assumed. The row-wise path stays as the
      // fallback for a resolution or alignment that breaks it.
      const double copyT0 = nowS();
      if (alreadyOwned) {
          // Nothing to do: nv12 already IS the buffer the encoder must read.
          // m_encCopyMs stays untouched rather than recording a fake zero.
      } else if (f->linesize[0] == static_cast<int>(mWidth) &&
          f->linesize[1] == static_cast<int>(mWidth)) {
          std::memcpy(f->data[0], nv12, ySize);
          std::memcpy(f->data[1], nv12 + ySize, ySize / 2);
      } else {
          for (int r = 0; r < static_cast<int>(mHeight); r++) {
              std::memcpy(f->data[0] + static_cast<size_t>(r) * f->linesize[0],
                          nv12 + static_cast<size_t>(r) * mWidth, mWidth);
          }
          for (int r = 0; r < static_cast<int>(mHeight) / 2; r++) {
              std::memcpy(f->data[1] + static_cast<size_t>(r) * f->linesize[1],
                          nv12 + ySize + static_cast<size_t>(r) * mWidth, mWidth);
          }
      }
      if (!alreadyOwned) {
          m_encCopyMs += (nowS() - copyT0) * 1e3;
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
        const double swsT0 = nowS();
        m_ff.sws_scale(m_sws,
                       reinterpret_cast<const uint8_t* const*>(f->data),
                       f->linesize, 0, static_cast<int>(mHeight),
                       m_convFrame->data, m_convFrame->linesize);
        m_encSwsMs += (nowS() - swsT0) * 1e3;
        m_convFrame->pts = sentPts;
        sendF = m_convFrame;
    }

    // Drain whatever the codec already finished, then send. If the codec
    // queue is full (EAGAIN), drain again and retry briefly вЂ” never drop a
    // frame, never spin forever.
    drainPackets();
    int sendRet = 0;
    // send_frame is timed separately from the drains it triggers, otherwise a
    // full codec queue would show up as "send is slow" when the cost was the
    // packets leaving. The delta on m_encDrainMs isolates the two.
    const double sendT0 = nowS();
    const double drainBefore = m_encDrainMs;
    for (int attempt = 0; attempt < 500; attempt++) {
        sendRet = m_ff.avcodec_send_frame(mCtx, sendF);
        if (sendRet == 0) {
            break;
        }
        if (sendRet == AVERROR(EAGAIN)) {
            m_encEagain++;
            drainPackets();
            continue;
        }
        skkbot2::log::error("[RENDER] send_frame: {}", ffErrString(&m_ff, sendRet));
        m_encSendMs += (nowS() - sendT0) * 1e3 - (m_encDrainMs - drainBefore);
        return;
    }
    m_encSendMs += (nowS() - sendT0) * 1e3 - (m_encDrainMs - drainBefore);
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

// A/V drift audit. The audio is pumped along the video timeline by
// construction, so in a healthy render all four numbers advance together:
//   video  = pts / fps                    (the container's video clock)
//   sim    = levelTime - anchor           (what the frames actually show)
//   audio  = samples written / rate       (the container's audio clock)
//   mixer  = master DSP clock / rate      (what FMOD actually mixed)
// mixer - audio is the dropped pre-roll; sim - video is the render clock
// slipping against the sim (mClockMismatch); audio - video is the desync the
// viewer would hear. Logging them on a fixed pts grid makes two renders
// directly diffable.
void Renderer::logAvSync(GJBaseGameLayer* layer) {
    if (!m_audioEnabled || !m_audioCtx || mFps <= 0 || mLastPts < 0) {
        return;
    }
    const int64_t step = std::max<int64_t>(1, static_cast<int64_t>(mFps) * 2);
    if (mLastPts % step != 0 || mLastPts == m_avSyncLogPts) {
        return;
    }
    m_avSyncLogPts = mLastPts;
    auto& ac = AudioCapture::get();
    const double videoT = static_cast<double>(mLastPts) / static_cast<double>(mFps);
    const double simT = layer
                            ? layer->m_gameState.m_levelTime - m_audioAnchor
                            : 0.0;
    const double audioT = ac.producedSeconds();
    const double mixT = ac.mixerSeconds();
    const double pre = ac.prerollRemainSeconds();
    const double drift = audioT - videoT;
    // Content-speed meter: how much SIM time the frames delivered per unit of
    // video time since the last grid point. The encoder/queue wall numbers
    // (stats speed=x.xx) have no relation to this - it is measured in the
    // content itself. A healthy render holds exactly mSpeed here for the
    // whole session; a dip or spike is the wobble, with the video time it
    // happened at, so a user-seen segment can be matched to the log directly.
    std::string contentNote;
    double contentSpeed = 0.0;
    if (m_avSyncHasPrev) {
        const double dv = static_cast<double>(mLastPts - m_avSyncPrevPts) /
                          static_cast<double>(mFps);
        const double ds = simT - m_avSyncPrevSim;
        if (dv > 0.0) {
            contentSpeed = ds / dv;
            const double want = std::max(0.001, mSpeed);
            if (std::abs(contentSpeed - want) > 0.05 * want) {
                contentNote = fmt::format(
                    "  << CONTENT SPEED OFF: want {:.3f}x", want);
                skkbot2::log::warn(
                    "[RENDER] av-sync: content speed {:.3f}x over "
                    "video {:.1f}-{:.1f}s (want {:.3f}x) - the level moved "
                    "{:+.3f}s of sim time in {:.3f}s of video",
                    contentSpeed,
                    static_cast<double>(m_avSyncPrevPts) / mFps, videoT,
                    want, ds, dv);
            }
        }
    }
    m_avSyncPrevPts = mLastPts;
    m_avSyncPrevSim = simT;
    m_avSyncHasPrev = true;
    if (m_audioKickUnknown) {
        contentNote = m_audioKickResolved
            ? "  << audio kick was UNKNOWN and the deferred restart fired"
            : "  << audio kick UNKNOWN, deferred restart never fired";
    }
    skkbot2::log::info(
        "[RENDER] av-sync: video={:.3f}s sim={:+.3f}s audio={:.3f}s "
        "mixer={:.3f}s preroll={:.3f}s drift(audio-video)={:+.3f}s "
        "drift(sim-video)={:+.3f}s content={:.3f}x{}", videoT, simT, audioT,
        mixT, pre, drift, simT - videoT, contentSpeed, contentNote);
    if (std::abs(drift) > 0.25) {
        skkbot2::log::warn(
            "[RENDER] av-sync: audio and video drifted apart by {:+.3f}s at "
            "video {:.3f}s - the written track is NOT aligned with the frames",
            drift, videoT);
    }
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
    auto vidDir = skkbot2::paths::videos();
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
        if (static_cast<unsigned char>(ch) < 32 ||
            ch == ' ' || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    while (!levelName.empty() && (levelName.back() == ' ' || levelName.back() == '.')) {
        levelName.pop_back();
    }
    if (levelName.empty()) {
        levelName = "level";
    }
    if (levelName.size() > 64) {
        levelName.resize(64);
    }
    std::string filename = levelName + "_" + tbuf + ".mp4";
    fs::path fullOutPath = vidDir / skk::stringToPath(filename);
    fs::create_directories(fullOutPath.parent_path(), ec);

    std::string outPath = skk::pathToUtf8(fullOutPath);
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

    // v59.7: kick the song from 0 AFTER attach
    // Meter reset BEFORE the audio-kick detection below: the flags must be
    // fresh at the moment begin() decides whether the song could start, or
    // the reset later in this function would erase the evidence (that exact
    // ordering bug made the 09:26/09:28 renders report kick=clean when the
    // song was in fact kicked through the unknown path).
    m_audioKickUnknown = false;
    m_audioKickResolved = false;
    m_avSyncHasPrev = false;
    m_avSyncPrevPts = -1;
    m_avSyncPrevSim = 0.0;
    // The kick section follows (v59.7): kick the song from 0 AFTER attach,
    // which clears m_allAudioPaused
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
            const int musicId = getLastMusicId();
            // Our music-ID hooks only see loadAndPlayMusic / queueStartMusic /
            // loadMusic. If the level got its track prepared through some other
            // path, m_musicPrepared is true while s_lastMusicId is still 0 -
            // and startMusic(..., musicID=0) then asks GD for a channel that
            // was never set up. The crash that motivated this guard was an
            // EXCEPTION_ACCESS_VIOLATION inside FMOD's own file thread, at
            // FMOD_Debug_Initialize, on exactly that run.
            //
            // Not treated as proof: two renders with audio completed fine
            // before it, so the trigger is a state race, not a constant. The
            // guard removes the one call that logged an obviously invalid
            // argument; if the crash survives it, the remaining suspects are
            // setDSPBufferSize(1024, 2) on the live system and addDSP on the
            // 8-channel mixer, and they get disabled one at a time.
            if (musicId <= 0) {
                armMusicRestart();
                // Audio-kick audit (02.11): remember that the song was NOT
                // started in begin(); the av-sync lines and stop() report
                // whether the deferred restart ever fired, so a silent start
                // segment can be told from a late one in the log.
                m_audioKickUnknown = true;
                m_audioKickResolved = false;
                skkbot2::log::warn(
                    "[RENDER] audio kick: musicID unknown - startMusic skipped, "
                    "deferred restart armed");
            } else {
                engine->startMusic(0, 0, 0, 0, false, musicId, false, false);
                engine->pauseAllAudio();
                engine->resumeAllAudio();
                engine->m_allAudioPaused = false;
                skkbot2::log::info("[RENDER] audio kick: start+pause/resume "
                                   "from 0 (musicID={})",
                                   musicId);
            }
        }
    } else {
            armMusicRestart();
            m_audioKickUnknown = true;
            m_audioKickResolved = false;
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
        disarmMusicRestart();
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
    // BUG-96 stage 8: the speedhack is MASTER over the video timeline too.
    // The captured value drives the render clock (driveOneFrame): one video
    // frame always shows mSpeed x (1/fps) of LEVEL time, so the footage plays
    // the level at exactly the speedhack rate. Changing the slider mid-render
    // has no effect until the next render.
    mSpeed = nEngine ? std::max(0.0, nEngine->gameSpeed) : 1.0;
    if (mSpeed <= 0.0) {
        mSpeed = 1.0;
    }
    skkbot2::log::info("[RENDER] speed captured: {:.3f}x (clock = tps x speed, "
                       "independent of Lock Delta / Real Time)", mSpeed);

    mCapture.init(mWidth, mHeight);

    // The ring carries frame DESCRIPTORS only - a pointer into a mapped PBO
    // slot plus the slot index - so there is no per-slot buffer to allocate
    // here any more. That also retires 6 x 24.9 MB (150 MB at 4K) of VRAM/RAM
    // that existed only to be memcpy'd through once per frame.
    m_ring.assign(RING_SIZE, RingEntry{});
    m_ringPts.assign(RING_SIZE, -1);
    m_rd = 0;
    m_wr = 0;
    m_qClosed = false;

    mLastPts = -1;
    m_audioAnchor = 0.0;
    m_avSyncLogPts = -1;
    // NOTE: m_audioKickUnknown/Resolved and m_avSyncPrev* are reset earlier in
    // begin(), right before the audio-kick detection - resetting them here
    // would erase the evidence the detection just wrote.
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
    mClockTick = 0;
    mFrameTicksMin = 0;
    mFrameTicksMax = 0;
    mClockAskedSum = 0;
    mClockDoneSum = 0;
    mClockMismatch = 0;
    mClockZeroFrames = 0;
    mClockCapHits = 0;
    mClockStallFrames = 0;
    mClockLogs = 0;
    mPboGateFrames = 0;
    mBatchCalls = 0;
    mBatchProduced = 0;
    mBatchDeliveredFull = 0;
    mBatchStopDrive = 0;
    mBatchStopDraw = 0;
    m_grabMs = 0.0;
    m_grabN = 0;
    m_grabMaxMs = 0.0;
    m_grabMaxPts = 0;
    for (double& v : m_stageMs) {
        v = 0.0;
    }
    m_lastStatsWall = 0.0;
    m_firstCaptureLogged = false;
    // Encoder stage accumulators, same bug class as FboCapture::m_visitN (fixed
    // 18.10.2026): these were never reset either, so the SECOND render of a
    // session reported the first render's numbers PLUS its own.
    //
    // Not a subtle inflation. Stereo Madness at 4K on 2026-10-18 reported
    // copy + send + drain = 103.5s of work inside a thread that lived 48.6s -
    // 213% of a thread, arithmetically impossible. Subtracting GRIEF's run from
    // the cumulative totals gives 47.8s, i.e. 98% of the thread, and that is
    // the real number. Every encoder figure quoted for a second or later render
    // before this fix was wrong by exactly the previous render's contribution.
    m_encCopyMs = 0.0;
    m_encSwsMs = 0.0;
    m_encSendMs = 0.0;
    m_encDrainMs = 0.0;
    m_encEagain = 0;
    // Every render starts at batch=3 for fast initial throughput and re-probes.
    m_batchLimit = 3;
    m_batchFullStreak = 0;
    m_batchShortStreak = 0;
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
    // No render-session flag any more (removed 19.10.2026 with logs/render.txt):
    // every diagnostic line now goes wherever every other line goes and follows
    // the same file-logging switch. The hot-path rows printed below are the
    // reason this was worth a second look - they carry no [RENDER] prefix, so a
    // prefix-based mirror used to drop exactly them.
    return true;
}

bool Renderer::tryAcquireSlot() {
    std::lock_guard lk(m_qMu);
    if (m_qClosed || (m_wr + 1) % RING_SIZE == m_rd) {
        return false;
    }
    return true;
}

bool Renderer::ringFull() {
    std::lock_guard lk(m_qMu);
    return (m_wr + 1) % RING_SIZE == m_rd;
}

void Renderer::commitSlot(const render::GrabResult& frame, int64_t pts) {
    {
        std::lock_guard lk(m_qMu);
        m_ring[m_wr] = RingEntry{frame.data, frame.slot, frame.preCopied};
        m_ringPts[m_wr] = pts;
        m_wr = (m_wr + 1) % RING_SIZE;
    }
    m_qCv.notify_one();
}

void Renderer::encodeThreadMain() {
    m_encRunning.store(true);
    while (true) {
        RingEntry entry;
        int64_t pts = 0;
        {
            std::unique_lock lk(m_qMu);
            m_qCv.wait(lk, [this] {
                return m_qClosed && m_rd == m_wr || m_rd != m_wr;
            });
            if (m_qClosed && m_rd == m_wr) {
                break;
            }
            entry = m_ring[m_rd];
            pts = m_ringPts[m_rd];
            m_rd = (m_rd + 1) % RING_SIZE;
        }
        // The slot (or the fallback scratch buffer) stays ours until the copy
        // out of it is done, and not one instruction longer. Releasing it in
        // the next statement instead of at the top of the loop is deliberate:
        // FboCapture reuses slots the instant this flag drops.
        encodeOneFrame(entry.ptr, pts, entry.preCopied);
        if (entry.slot >= 0) {
            mCapture.releaseSlot(entry.slot);
        } else if (entry.preCopied) {
            mCapture.releaseSync();
        }
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
        if (m_audioKickUnknown) {
            skkbot2::log::warn(
                "[RENDER] done: audio kick was UNKNOWN at begin "
                "(musicID not available) and the deferred restart "
                "{} - expect the song to be {} in the first seconds of "
                "this file",
                m_audioKickResolved ? "fired a few frames in"
                                    : "NEVER fired",
                m_audioKickResolved ? "late by those frames"
                                    : "missing / silence");
        }
    }
    freeEncoder();
    m_encRunning.store(false);
    m_qCv.notify_all();
}

namespace {
// Accumulates elapsed time into a slot on scope exit, so every early return in
// drive() is still measured. drive() bails out on pause, ring backpressure and
// several safety nets - timing only the happy path would silently drop exactly
// the frames where the stall lives.
struct StageTimer {
    double* acc;
    double t0;
    explicit StageTimer(double* a) : acc(a), t0(nowS()) {}
    ~StageTimer() { *acc += nowS() - t0; }
};
} // namespace

void Renderer::drive(GJBaseGameLayer* layer,
                     std::function<void(float)> stepOne) {
    if (!mRecording.load()) {
        return;
    }
    m_driveCalls++;

    // Batched capture. One drawScene used to mean exactly one video frame, so
    // the wall frame rate - not the capture pipeline - decided how long the
    // render took. Measured 2026-10-18 on a 180 Hz panel: Stereo Madness ran at
    // driveHz=181 (frame 5.52ms = exactly the refresh period) while the video
    // wanted 60 frames/s, i.e. two thirds of the wall frames existed only to
    // wait for a vertical blank.
    //
    // Each extra frame costs one sim step to the pts boundary plus one
    // renderSceneInto + NV12 + async readback, and nothing waits on the display
    // in between, so the wait is what gets converted.
    //
    // Stop conditions, all of them "this frame could not be produced":
    //   driveOneFrame false - ring full, level paused, level done, stop()
    //   onFrameDraw false   - the capture itself could not be committed
    // Both leave the sim at a boundary, so no pts is ever skipped.
    int produced = 0;
    for (int k = 0; k < m_batchLimit; ++k) {
        if (!driveOneFrame(layer, stepOne)) {
            ++mBatchStopDrive;
            break;
        }
        // Capture NOW rather than leaving it to the ImGui callback at
        // swapBuffers time: that callback runs once per drawScene, so leaving
        // it there caps the batch at one no matter what m_batchLimit says.
        //
        // Safe to run inside drawScene because issueReadback() re-renders the
        // scene into its own FBO. Afterwards drawPreview() blits that FBO to
        // the window, so the operator watches the LAST frame of the batch -
        // which is the newest state, not a stale one.
        if (!onFrameDraw()) {
            ++mBatchStopDraw;
            break;
        }
        ++produced;
    }
    mBatchCalls++;
    mBatchProduced += produced;
    if (produced >= m_batchLimit && m_batchLimit > 0) {
        ++mBatchDeliveredFull;
    }

    // Adapt the batch, with hysteresis on both edges.
    //
    // The first version reacted to every single drawScene and oscillated. On
    // Stereo Madness (2026-10-18) batch walked 3 -> 5 -> 2 -> 2 -> 3 over five
    // seconds while sitting right at the encoder's limit: one drawScene
    // delivered its full batch, the next hit the full ring, and the pair of
    // rules fought each other. The batch also decides how much work one wall
    // frame does, so a limit that changes every frame is a wall frame that
    // changes every frame.
    //
    // Grow only after kGrowStreak consecutive full drawScenes, shrink only after
    // kShrinkStreak consecutive short ones. Both thresholds are in drawScenes,
    // not seconds: on a level running at 100 driveHz that is 40ms of evidence
    // before the limit moves, which is long enough to be a measurement and
    // short enough to be invisible in a render.
    const bool full = produced >= m_batchLimit && m_batchLimit > 0;
    if (full) {
        ++m_batchFullStreak;
        m_batchShortStreak = 0;
    } else if (m_batchLimit > 1) {
        ++m_batchShortStreak;
        m_batchFullStreak = 0;
    }
    if (m_batchShortStreak >= kShrinkStreak && m_batchLimit > 1) {
        m_batchLimit = std::max(1, produced > 0 ? produced : m_batchLimit - 1);
        m_batchShortStreak = 0;
        m_batchFullStreak = 0;
    } else if (full && m_batchFullStreak >= kGrowStreak &&
               m_batchLimit < kBatchMax) {
        ++m_batchLimit;
        m_batchShortStreak = 0;
        m_batchFullStreak = 0;
    }
}

bool Renderer::driveOneFrame(GJBaseGameLayer* layer,
                             std::function<void(float)> stepOne) {
    if (!mRecording.load()) {
        return false;
    }
    StageTimer driveTimer(&m_stageMs[static_cast<int>(Stage::Drive)]);
    m_stageStallPending = false;
    auto* pl = static_cast<PlayLayer*>(layer);
    // Silicate parity: feed the scheduler 1/fps (e.g. 1/60) — GD's own
    // getModifiedDelta quantises to the 240-Hz tick grid internally.
    // Feeding 1/tps directly can leave the internal accumulator stranded at
    // zero if the first fractional tick rounds down.
    const float simDt = 1.0f / static_cast<float>(mFps);

    // PBO pool is a SECOND reason to hold, and it is checked HERE on
    // purpose. issueReadback() runs inside grab(), which runs AFTER the
    // simulation has already stepped - a pool that turned out to be empty
    // at that point would have advanced the sim without capturing it,
    // i.e. a silently dropped frame. Asked before the step, it is just
    // backpressure.
    if (ringFull() || !mCapture.hasFreeSlot()) {
        // Counted SEPARATELY, because these two mean opposite things. A full
        // ring is the encoder being slow - known, handled, and capped. A full
        // PBO pool is a gate this change introduced, and it is a NEW way for
        // the render to lose throughput without saying so anywhere. The first
        // render after 380b82e showed grab 2.6 -> 1.8 ms and the wall clock
        // dead at 107.1 s, and the most likely explanation available is that
        // this gate fires and eats the gain. That is a guess, and a counter is
        // the difference between a guess and a measurement.
        if (!ringFull()) {
            ++mPboGateFrames;
        }
        if (++mRingFullFrames > 60 * 30) {
            skkbot2::log::error(
                "[RENDER] encoder stalled: capture ring full for ~30s вЂ” "
                "aborting render");
            stop();
        }
        return false;
    }
    mRingFullFrames = 0;

    // Fast-forward render (same idea as Silicat): NO wall-clock pacing. Every
    // drive() call advances exactly one video frame's worth of sim
    // (mTps/mFps ticks) and queues one capture; the game loop runs uncapped
    // (~180Hz on this machine), so a 37.5s level renders in ~13s wall instead
    // of real-time. pts are derived from the sim tick count, so the video
    // always plays at 1x regardless of the wall rate — speed only affects how
    // long the render takes. The capture no longer waits for the ImGui
    // callback at swapBuffers: drive() calls onFrameDraw() itself, once per
    // batched frame, right after this function queues the boundary.
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
            "ticks={} tickRate={:.0f}/s driveHz={:.0f} batch={} captured={} "
            "encoded={} grabFails={} drops={} started={}",
            wall, lvlT, speed, m_stepCount,
            wall > 0.0 ? m_stepCount / wall : 0.0,
            wall > 0.0 ? static_cast<double>(m_driveCalls) / wall : 0.0,
            m_batchLimit,
            mCapturedFrames.load(), mEncodedFrames.load(), m_grabFailTotal,
            m_pendingDrops, pl->m_started);
        // Clock audit: ticks per captured frame is the number the judder lives
        // in. A correct render has min/max within one tick of tps/fps, asked
        // == done, and zero mismatches. Anything else is named here instead of
        // being discovered in the finished video.
        skkbot2::log::info(
            "[RENDER] clock: ticks/frame {}..{} (ideal {:.2f}) asked={} "
            "done={} mismatch={} zero={} capHits={} drops={}",
            mFrameTicksMin, mFrameTicksMax,
            static_cast<double>(mTps) / static_cast<double>(mFps),
            mClockAskedSum, mClockDoneSum, mClockMismatch,
            mClockZeroFrames, mClockCapHits, m_pendingDrops);
        mFrameTicksMin = 0;
        mFrameTicksMax = 0;
        mClockAskedSum = 0;
        mClockDoneSum = 0;
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
            return false;
        }
        if (m_pendingFrame) {
            return false;  // previous trailer frame not drawn yet
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
        // pump along the same video timeline so the audio tail matches the
        // last frames.
        audioPump(m_audioAnchor + static_cast<double>(mLastPts) / mFps);
        logAvSync(pl);
        return true;
    }

    const uint64_t f = computeRenderFrame(pl, mTps);
    // Do not capture while the level is paused вЂ” the pause menu must not
    // end up in the video. Drop any pending capture too: a boundary queued
    // before pausing would be grabbed once onFrameDraw() next runs, baking
    // the pause menu into the video.
    if (pl->m_isPaused) {
        if (m_pendingFrame) {
            ++m_pendingDrops;  // counted: this boundary is gone, not retried
        }
        m_pendingFrame = false;
        m_pendingFadeOut = 1.0f;
        return false;
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
        return false;
    }
    if (m_pendingFrame) {
        // The draw callback has not consumed the previous boundary yet (the
        // loop fired faster than the frame drew). Wait instead of queueing a
        // new capture вЂ” overwriting would lose a pts and stutter the video.
        return false;
    }
    // ---- guards that used to live inside the stepping loop ----
    // After the macro's last click the bot no longer steers the player: if it
    // dies before the finish line the level resets and completion will never
    // fire. Give it a few seconds of sim time, then give up. (Playback
    // exhausted marks "macro consumed" вЂ” inputs delivered, the player must
    // reach the end on its own.)
    {
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
            return false;
        }
    }
    if (f < mLastFramed) {
        // Level time moved BACKWARDS. Two very different things cause this
        // and only one of them is recoverable.
        //
        // A practice checkpoint respawn rewinds a little: the player is put
        // back on solid ground a short way behind. The video timeline simply
        // continues (the respawn IS part of the video); only the sim clock is
        // re-anchored.
        //
        // A level RESTART throws the level back to frame 1 from wherever it
        // was - measured: f=6461->1 mid-render. There is no timeline to
        // resync to, the frames already captured describe a run that no
        // longer exists, and the level comes back paused, so the render sits
        // there holding a pause for as long as the user leaves it. Observed:
        // 77 frames over 34 seconds, all of it paused.
        //
        // Restarting the recording silently is the wrong answer in both
        // cases - the first case would throw away a long, good recording.
        // The honest answer is to finish this one and say why.
        const bool isRestart = f <= 1;
        if (isRestart) {
            skkbot2::log::error(
                "[RENDER] the level was RESTARTED mid-render (frame {} -> {}). "
                "The captured frames describe a run that no longer exists, so "
                "this render ends here rather than rewinding the timeline and "
                "sitting in a pause forever. Start a new render after the "
                "restart.",
                mLastFramed, f);
            stop();
            return false;
        }
        skkbot2::log::info("[RENDER] level time rewound f={}->{} "
                           "(respawn) вЂ” sim clock re-anchored, video timeline "
                           "continues",
                           mLastFramed, f);
        mLastFramed = f;
        return false;
    }
    mLastFramed = f;

    // ---- RENDER CLOCK ----
    // One captured frame == exactly `ticksThisFrame` physics ticks, chosen so
    // that video frame k lands on sim tick round(k * tps * speed / fps).
    //
    // The old loop stepped a fixed 1/fps of SCHEDULER time and then asked the
    // game where it had landed (ptsOfFrame(computeRenderFrame())). That is not
    // a clock: when tps/fps is not an integer the scheduler serves an
    // alternating step count (8,8,9,...), and a 9-tick step can straddle TWO
    // pts boundaries. The loop then exited with the pts two ahead, `mLastPts =
    // curPts` swallowed the difference and that video-frame slot was never
    // captured - while the encoder still writes a strict sequential 1/fps
    // timeline. What the viewer sees is a stretch that runs fast and then
    // slow. That is the judder reported on high-TPS renders, and it needs
    // tps/fps to be non-integer to appear, which is why 240/60 looked fine.
    //
    // Counting ticks here instead makes the content time a function of the
    // FRAME INDEX by construction: no slot can be skipped, and the per-frame
    // tick count differs by at most one tick - which is the correct rounding
    // of a non-integer ratio, not a defect.
    const int64_t frameIndex = mLastPts < 0 ? 0 : mLastPts + 1;
    if (frameIndex == 0) {
        // Video pts 0 == this level time. begin() restarts the level song from
        // 0, so everything the mixer produces up to this level time is
        // pre-roll (dropped inside AudioCapture) and the first encoded block
        // starts the track where the footage starts.
        m_audioAnchor = pl->m_gameState.m_levelTime;
        const double trackPos = musicPositionSeconds();
        AudioCapture::get().anchorAt(m_audioAnchor,
                                     trackPos < 0.0 ? 0.0 : trackPos);
        if (m_audioEnabled && musicRestartPending()) {
            // The deferred restart is about to run on this drive tick: the
            // song starts here, a few frames late. Record it so the av-sync
            // lines can tell a resolved late start from a never-started one.
            m_audioKickResolved = true;
            skkbot2::log::warn(
                "[RENDER] audio: the level song is not prepared at video pts 0 "
                "- the deferred start will begin the track at position 0 a few "
                "frames in, so the music will be late by that much (the video "
                "timeline itself is unaffected)");
        }
    }
    // One video frame shows mSpeed x (1/fps) of LEVEL time: targetTick =
    // round(k * tps * speed / fps) -> the footage plays the level at exactly
    // the captured speedhack rate (speed 1 -> the original pace everywhere,
    // whatever the live timing mode is). The SpeedControlScheduler feed path
    // contributes NO extra multiplier here (render drives are gated to the
    // raw asked dt in advanceNormalScheduler), so this is the only place the
    // speed enters the render.
    const int64_t targetTick = static_cast<int64_t>(std::llround(
        static_cast<double>(frameIndex) * mTps * mSpeed /
        static_cast<double>(mFps)));
    // A frame never needs more than one frame's worth of ticks; the cap only
    // exists so a nonsense tps/fps ratio cannot freeze the render on frame 0.
    const int64_t kClockTicksCap = std::max<int64_t>(
        16, static_cast<int64_t>(
                std::ceil(mTps * mSpeed / static_cast<double>(mFps))) + 8);
    int64_t ticksThisFrame = targetTick - static_cast<int64_t>(mClockTick);
    if (ticksThisFrame < 0) ticksThisFrame = 0;
    if (ticksThisFrame > kClockTicksCap) {
        ticksThisFrame = kClockTicksCap;
        ++mClockCapHits;
    }
    if (ticksThisFrame == 0) {
        // tps < fps: two video frames describe the same sim state. Correct,
        // and it is the only case where a repeated image is not a defect.
        ++mClockZeroFrames;
    }

    if (ticksThisFrame > 0) {
        StageTimer simTimer(&m_stageMs[static_cast<int>(Stage::Sim)]);
        // Ask for exactly that many ticks: dt is an exact multiple of the
        // scheduler's 1/tps step, so the step planner serves it without a
        // carry (it may still refuse at its own budget - measured below).
        stepOne(static_cast<float>(static_cast<double>(ticksThisFrame) / mTps));
        mClockTick += static_cast<uint64_t>(ticksThisFrame);
        m_stepCount += static_cast<uint64_t>(ticksThisFrame);
    }
    const uint64_t after = computeRenderFrame(pl, mTps);
    const int64_t actual = static_cast<int64_t>(after) - static_cast<int64_t>(f);
    mClockAskedSum += ticksThisFrame;
    mClockDoneSum += actual;
    // One tick of slack: level time is a float accumulator, so floor() lands
    // either side of a boundary now and then. Anything beyond that is the sim
    // refusing the request (step budget, pause, warp) and is worth knowing.
    if (actual < ticksThisFrame - 1 || actual > ticksThisFrame + 1) {
        ++mClockMismatch;
        if (mClockLogs < 10) {
            ++mClockLogs;
            skkbot2::log::warn(
                "[RENDER] clock: asked {} ticks, sim advanced {} "
                "(frame={} tick={}->{} levelTime={:.6f} tps={:.1f})",
                ticksThisFrame, actual, frameIndex, f, after,
                pl->m_gameState.m_levelTime, mTps);
        }
    }
    if (actual == 0 && ticksThisFrame > 0) {
        if (++mClockStallFrames > 240) {
            skkbot2::log::error(
                "[RENDER] drive stalled: levelTime not advancing for {} frames "
                "(f={} asked={} levelTime={:.6f}) вЂ” bailing out",
                mClockStallFrames, f, ticksThisFrame,
                pl->m_gameState.m_levelTime);
            return false;
        }
    } else {
        mClockStallFrames = 0;
    }
    if (mFrameTicksMin == 0 || ticksThisFrame < mFrameTicksMin) {
        mFrameTicksMin = ticksThisFrame;
    }
    if (ticksThisFrame > mFrameTicksMax) {
        mFrameTicksMax = ticksThisFrame;
    }
    mLastFramed = after;

    // Boundary reached: queue the capture. The actual grab runs in
    // onFrameDraw() (start of the ImGui draw callback, right after the game
    // drew this wall frame's scene and BEFORE the GUI overlay is rendered) вЂ”
    // so the video gets a fresh frame with no bot menu baked in.
    mLastPts = frameIndex;
    m_pendingPts = frameIndex;
    m_pendingFadeOut = 1.0f;
    m_pendingFrame = true;

    // Advance the audio mixer along the VIDEO timeline (anchor + pts/fps), so
    // the written audio is exactly as long as the written video by
    // construction. The pump is a no-op while the encoder is behind
    // (backpressure) and the mixer never runs ahead. The FMOD output is
    // NOSOUND_NRT, so the mixer only advances through this pump.
    audioPump(m_audioAnchor + static_cast<double>(mLastPts) / mFps);
    logAvSync(pl);
    return true;
}

double Renderer::renderElapsedSeconds() const {
    if (!mRecording.load()) {
        return 0.0;
    }
    // A level pause freezes the sim and produces no frames, so it is dead time.
    // Bill the hold that is still open to the accumulator first, then subtract
    // everything: the wall figure is the render's own cost, not the time the
    // user spent sitting in the pause menu.
    double open = 0.0;
    if (mLevelPaused && mPauseStartWall > 0.0) {
        open = nowS() - mPauseStartWall;
    }
    const double wall = nowS() - m_renderStartWall;
    return std::max(0.0, wall - mPausedWallSecs - open);
}

double Renderer::videoSeconds() const {
    return mFps > 0 ? static_cast<double>(mCapturedFrames.load()) /
                          static_cast<double>(mFps)
                    : 0.0;
}

void Renderer::noteLevelPaused() {
    mLevelPauseLogged = true;
    mLevelPaused = true;
    mPauseStartWall = nowS();
}

void Renderer::noteLevelResumed() {
    mLevelPauseLogged = false;
    mLevelPaused = false;
    if (mPauseStartWall > 0.0) {
        mPausedWallSecs += nowS() - mPauseStartWall;
        mPauseStartWall = 0.0;
    }
}

bool Renderer::onFrameDraw() {
    // Returns whether a video frame was actually committed. drive() uses that
    // as the batching stop signal: a false here means the encoder ring is full
    // or the pipeline could not deliver, and the batch stops for this drawScene
    // instead of spinning.
    if (!mRecording.load() || !m_pendingFrame) {
        return false;
    }

    if (!tryAcquireSlot()) {
        // Encoder backpressure: every ring entry is still being encoded.
        // Do NOT consume the pending boundary - drive() holds the sim while
        // the ring is full, so this boundary is retried on the next wall
        // frame with no pts lost (Silicat-style skip, main thread never
        // blocks).
        return false;
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
    // and hands back the OLDEST already-issued frame together with ITS pts and
    // a POINTER into its mapped PBO slot - the encoder always gets the
    // timestamp of the pixels it is actually given (Silicat's m_ptsQueue
    // push/pop pairing), and the main thread no longer copies them anywhere.
    render::GrabResult got;
    const double g0 = nowS();
    const bool grabbed = mCapture.grab(m_pendingPts, &got);
    const int64_t gotPts = got.pts;

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
        // Retry, do not drop. The boundary was NOT consumed by the failed
        // grab (tryAcquireSlot() above already returned early for the other
        // failure mode), and the sim has not moved since it was queued, so
        // the next wall frame grabs exactly this frame. Clearing the flag
        // here used to lose a pts for good - one missing slot, which the
        // encoder's sequential timeline turns into a visible speed change.
        m_pendingFrame = true;
        return false;
    }
    const double grabDelta = (nowS() - g0) * 1e3;
    m_grabMs += grabDelta;
    m_grabN++;
    if (grabDelta > m_grabMaxMs) {
        m_grabMaxMs = grabDelta;
        m_grabMaxPts = m_pendingPts;
    }
    // grabDelta is milliseconds (it feeds m_grabMs and the "grabAvg=1.8ms"
    // summary); noteStage counts in seconds, like every other stage.
    noteStage(grabDelta * 1e-3, Stage::Grab);
    mGrabFailStreak = 0;
    if (!m_firstCaptureLogged) {
        m_firstCaptureLogged = true;
        skkbot2::log::info(
            "[RENDER] first frame captured pts={} grabMs={:.1f} "
            "nv12Size={}",
            gotPts, m_grabMs / m_grabN, mCapture.m_nv12Size);
    }
    commitSlot(got, gotPts);
    mCapturedFrames.fetch_add(1, std::memory_order_release);
    if ((mCapturedFrames.load() % 120) == 0) {
        skkbot2::log::info("[RENDER] progress: captured={} encoded={} pts={}",
                           mCapturedFrames.load(), mEncodedFrames.load(),
                           gotPts);
    }
    return true;
}

double Renderer::nowWall() {
    return nowS();
}

void Renderer::noteStage(double seconds, Stage which) {
    m_stageMs[static_cast<int>(which)] += seconds;
    const double ms = seconds * 1e3;

    // Watchdog. A stall inside the render frame logs nothing at all - the whole
    // main thread is blocked, so the probes, the DIAG lines and the audio level
    // all stop too, and the gap in the log is the only symptom. Measured on
    // Artemis 13: one 1.46s silence at level frame 14380 (75% of the level) and
    // 4794 frames otherwise clean, drops=0, grabFails=2 - so nothing is
    // overfilling and nothing is dropped, the thread is simply stuck.
    //
    // The stage timers hand us SECONDS (nowWall() returns seconds), while the
    // grab timer is in milliseconds. The first version of this compared seconds
    // against 200.0 and therefore needed a 200 second stall to fire - which is
    // why it stayed silent through a run that visibly froze. Converting once,
    // here, keeps every caller free to pass seconds.
    static constexpr double kStallMs = 200.0;

    const int idx = static_cast<int>(which);
    if (ms < kStallMs) return;
    m_stageStallPending = true;

    if (which == Stage::Frame) {
        // The drawScene BODY was slow, but nothing timed inside it explains it.
        //
        // Two things this message used to get wrong, both found on the GRIEF 4K
        // run (2026-10-18) that produced the line above: it claimed "NO stage of
        // ours was slow" while grab - the largest stage at 2.43ms of a 13.04ms
        // frame - was not even considered, because grab is timed in
        // onFrameDraw() and lives outside Stage::Frame. And it attributed the
        // remainder to "GD's own loop", when it is at least as likely to be the
        // GPU draining our own 4K passes: gl* returns immediately, so the wait
        // shows up as a gap between frames rather than inside one.
        skkbot2::log::warn(
            "[RENDER] STALL: the drawScene body took {:.0f} ms with no stage "
            "inside it slow. grab is timed in onFrameDraw, outside the body - "
            "compare grabMax in the summary: if it is nowhere near this number "
            "the stall was NOT the capture, and what is left is either GD's loop "
            "or the GPU draining our 4K passes. These timers cannot tell those "
            "two apart.",
            ms);
        return;
    }

    static const char* kNames[] = {"drive", "sim",   "grab",     "preview",
                                   "viewUi", "swap", "frame"};
    skkbot2::log::warn(
        "[RENDER] STALL: the '{}' stage took {:.0f} ms of a single wall frame "
        "(threshold {:.0f} ms). Main thread was blocked here.",
        kNames[idx], ms, kStallMs);
}

void Renderer::drawPreview() {
    StageTimer previewTimer(&m_stageMs[static_cast<int>(Stage::Preview)]);
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
    disarmMusicRestart();

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
    // wall excludes level pauses (nothing is simulated or captured then), and
    // the time spent holding is reported separately as paused=.
    double paused = mPausedWallSecs;
    if (mLevelPaused && mPauseStartWall > 0.0) {
        paused += nowS() - mPauseStartWall;
    }
    double wall = std::max(0.0, nowS() - m_renderStartWall - paused);
    double lvl = mLastPts > 0 ? static_cast<double>(mLastPts) / mFps : 0.0;
    skkbot2::log::info(
        "[RENDER] Summary: captured={} encoded={} wall={:.1f}s paused={:.1f}s "
        "level={:.2f}s speed={:.2f}x ticks={} driveHz={:.0f} batch={} "
        "grabAvg={:.1f}ms grabMax={:.1f}ms@pts={} grabFails={} drops={} "
        "pboGate={} batchAvg={:.2} batchFull={} stopDrive={} stopDraw={}{}",
        mCapturedFrames.load(), mEncodedFrames.load(), wall, paused, lvl,
        wall > 0.0 ? lvl / wall : 0.0, m_stepCount,
        wall > 0.0 ? static_cast<double>(m_driveCalls) / wall : 0.0,
        m_batchLimit,
        m_grabN > 0 ? m_grabMs / m_grabN : 0.0, m_grabMaxMs, m_grabMaxPts,
        m_grabFailTotal,
        m_pendingDrops,
        mPboGateFrames,
        mBatchCalls ? static_cast<double>(mBatchProduced) /
                          static_cast<double>(mBatchCalls)
                    : 0.0,
        mBatchDeliveredFull,
        mBatchStopDrive,
        mBatchStopDraw,
        m_audioPackets > 0
            ? (std::string(" audio=") + std::to_string(m_audioPackets) +
               "pk (" + m_audioCodecUsed + ")")
            : std::string(" audio=off"));
    // Where the wall frame actually goes. These are CPU submission costs only
    // (the GL calls just queue work), so frameMs - their sum is the part the
    // main thread spends waiting on the GPU to finish the frame.
    // The per-frame divisor is CAPTURED FRAMES, not drive() calls. They used to
    // be the same number - one video frame per drawScene - and batching broke
    // that identity: one drive() call now delivers up to kBatchMax frames. Left
    // as m_driveCalls, every per-frame figure in this summary would have been
    // divided by a number up to 8x too small.
    const double frames =
        mCapturedFrames.load() > 0 ? static_cast<double>(mCapturedFrames.load()) : 1.0;
    const double frameMs = wall * 1e3 / frames;
    auto avg = [&](Stage s) { return m_stageMs[static_cast<int>(s)] / frames; };
    const double driveAvg = avg(Stage::Drive);
    const double simAvg = avg(Stage::Sim);
    const double grabAvgStage = avg(Stage::Grab);
    const double prevAvg = avg(Stage::Preview);
    const double viewUiAvg = avg(Stage::ViewUi);
    const double swapAvg = avg(Stage::Swap);
    const double hookAvg = avg(Stage::Frame);
    // Raw sums too, in us with 1 decimal: {:.2} on a value under 5 microseconds
    // prints 0.00, which is indistinguishable from "never ran" and sent us
    // hunting for a counter bug that did not exist.
    skkbot2::log::info(
        "[RENDER] stages: frame={:.2f}ms body={:.3f} drive={:.3f} sim={:.3f} "
        "grab={:.3f} preview={:.3f} viewUi={:.3f} swap={:.3f} "
        "outsideStages={:.3f} [raw us: drive={:.1f} sim={:.1f} preview={:.1f} "
        "viewUi={:.1f} swap={:.1f} frames={:.0f}]",
        frameMs, hookAvg, driveAvg, simAvg, grabAvgStage, prevAvg, viewUiAvg,
        swapAvg,
        // Subtracted BOTH the drawScene body and grab. grab is timed in
        // onFrameDraw(), which runs in the imgui hook OUTSIDE Stage::Frame, so
        // the old `frameMs - body` left it inside the remainder and then called
        // that remainder "GD's own loop". On GRIEF 4K (2026-10-18) grab was
        // 2.43ms of a 13.04ms frame - the single biggest measured stage, filed
        // under GD. What is left now is genuinely unattributed: gl* calls
        // return immediately and the wait lands on the next frame's first call,
        // so this bucket holds the GPU draining the 4K scene render, the NV12
        // passes and the present. Calling it GD's loop was a guess that
        // pointed the next investigation at the wrong subsystem.
        std::max(0.0, frameMs - hookAvg - grabAvgStage),
        m_stageMs[static_cast<int>(Stage::Drive)] * 1e3,
        m_stageMs[static_cast<int>(Stage::Sim)] * 1e3,
        m_stageMs[static_cast<int>(Stage::Preview)] * 1e3,
        m_stageMs[static_cast<int>(Stage::ViewUi)] * 1e3,
        m_stageMs[static_cast<int>(Stage::Swap)] * 1e3, frames);

    // Encoder-thread breakdown, per encoded frame. Deliberately a separate line
    // from the GL-thread stages above: the two threads run concurrently, so
    // summing them into one table would read as "more than a frame", which is
    // what made the earlier memcpy change impossible to judge.
    //
    // EAGAIN count is the signal for the encoder ring: a non-zero value means
    // the codec queue filled up and the send had to wait for packets to leave,
    // which is the case ENC_FRAMES exists to absorb.
    skkbot2::log::info(
        "[RENDER] enc stages: per frame copy={:.3f}ms sws={:.3f}ms "
        "send={:.3f}ms drain={:.3f}ms eagain={} "
        "[totals ms: copy={:.1f} sws={:.1f} send={:.1f} drain={:.1f} "
        "frames={:.0f}]",
        frames > 0 ? m_encCopyMs / frames : 0.0,
        frames > 0 ? m_encSwsMs / frames : 0.0,
        frames > 0 ? m_encSendMs / frames : 0.0,
        frames > 0 ? m_encDrainMs / frames : 0.0,
        m_encEagain, m_encCopyMs, m_encSwsMs, m_encSendMs, m_encDrainMs,
        frames);

    // The scene traversal inside the capture, measured on its own. The old
    // comment here called it "the duplicate" and promised a ~6.7s saving on
    // GRIEF by removing it. That was wrong: displayPreview() has been a
    // glBlitFramebuffer of the already-rendered scene FBO (capture.cpp:617),
    // not a second walk of the scene graph, so renderSceneInto() is the ONLY
    // traversal per frame and there is no duplicate to remove. The number is
    // still worth having - it separates "telling the GPU what to draw" from
    // "reading pixels back" inside grab.
    //
    // It has to be measured on its own because glDrawArrays only queues: the
    // traversal's CPU cost is visible, but its GPU cost drains later and never
    // appears in grab. Reported against grabAvg so the fraction of the capture
    // stage it accounts for is explicit.
    const double visitAvg =
        mCapture.visitN() > 0 ? mCapture.visitMs() / mCapture.visitN() : 0.0;
    skkbot2::log::info(
        "[RENDER] scene visit (the only traversal; preview is a blit): "
        "{:.3}ms per call over {} calls "
"= {:.1f}ms of {:.2f}ms of grab = {:.0f}% of it, "
          "{:.1f}% of a {:.2f}ms frame",
          visitAvg, mCapture.visitN(), mCapture.visitMs(),
          m_grabN > 0 ? m_grabMs / m_grabN : 0.0,
          (m_grabMs > 0.0) ? 100.0 * mCapture.visitMs() / m_grabMs : 0.0,
          (frameMs > 0.0) ? 100.0 * visitAvg / frameMs : 0.0, frameMs);

    // How hot the bot's own per-tick hooks ran during this render. Added
    // because the bot, not the renderer, is the expensive half: Xenon's
    // kerender takes 2:04 with Xenon's bot and 2:40 with ours, and ours is
    // not the slower renderer.
    //
    // Rates are per wall second because that is the number that has to be
    // explained - it is what the 36s of extra render time is made of. The
    // scheduler row also carries its own wall time; the per-object rows carry
    // call counts only, because a clock read inside a per-object per-tick hook
    // would cost more than the thing being measured.
    if (frames > 0) {
        const double secs = wall > 0.0 ? wall : 1.0;
        skkbot2::log::info(
            "[RENDER] hot path per wall second ({}s wall, {:.0f} sched calls/s):",
            std::to_string(secs).substr(0, 5), frames / secs);
        auto& hs = skkbot2::hot::stats();
        // The ratio that matters: physics steps per captured video frame. The
        // step-grid hook pins the loop to g_forcedSteps iterations of 1/240s,
        // so one scheduler update advances the level by g_forcedSteps/240 -
        // not by the 1/60 handed to the scheduler. A renderer that gets 1/60
        // per call therefore needs 240/60 times fewer calls for the same video.
        const double stepsPerFrame =
            hs.calls[static_cast<size_t>(skkbot2::hot::PhysStep)] / frames;
        const double schedPerFrame =
            hs.calls[static_cast<size_t>(skkbot2::hot::SchedUpdate)] / frames;
        // The ratio line and the per-slot rows carry the [RENDER] prefix purely so a
        // grep finds the whole block; it no longer has to, because the
        // dedicated render.txt mirror is gone (removed 19.10.2026). They used
        // to have to: that file was fed by a prefix match in Logger::log, so a
        // continuation line indented with spaces silently never reached it -
        // which is exactly what happened on the 2026-10-18 GRIEF run, where the
        // header was present and both the ratio line and every counter row were
        // missing. That read as "no hot-path work happened" rather than as
        // "this line was never captured", which is the worse of the two.
        //
        // The divisor is m_driveCalls (captured video frames), NOT the slot's
        // own call count - the row used to say "ms per call" over a per-frame
        // figure, which for the scheduler rows is off by the batch factor
        // (advanceNormalScheduler calls GD's base up to 8 times per hook call):
        // GRIEF read "5.597413ms per call" when the real per-call figure was
        // 0.34ms and the per-frame figure was 5.597. Both are useful, so both
        // are printed from here on.
        skkbot2::log::info(
            "[RENDER]   ratio: {:.2f} physics steps and {:.2f} scheduler "
            "calls per captured frame ({:.2f} ms of level time per frame at "
            "{:.0f} tps)",
            stepsPerFrame, schedPerFrame,
            stepsPerFrame * 1000.0 / static_cast<double>(mTps), mTps);
        for (size_t i = 0; i < skkbot2::hot::N; i++) {
            if (hs.calls[i] == 0) continue;
            const double perSec = hs.calls[i] / secs;
            const double perFrame = hs.ms[i] / frames;
            const double perCall =
                hs.ms[i] / static_cast<double>(hs.calls[i]);
            std::string row = std::string("[RENDER]   ") + skkbot2::hot::name(
                                                         static_cast<skkbot2::hot::Slot>(i));
            row += " = " + std::to_string(hs.calls[i]) + " calls (" +
                   std::to_string(static_cast<long long>(perSec)) + "/s)";
            if (hs.ms[i] > 0.0) {
                row += ", " + std::to_string(perFrame) +
                       "ms per captured frame, " + std::to_string(perCall * 1e3) +
                       "us per call";
            }
            skkbot2::log::info("{}", row);
        }
    }

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
    // Every summary line above has already gone to the regular log by now, so
    // there is no session to close. stop() is also called from failure paths
    // (grab never produced a frame, the encoder stalled, the level restarted
    // mid-render), and those summaries are exactly the ones worth keeping.
}

}  // namespace render
}  // namespace skkbot2
