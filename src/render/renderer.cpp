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

constexpr double kTrailerHold = 2.0 / 60.0;

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
    if (size.width <= 0.0f || size.height <= 0.0f) {
        skkbot2::log::warn("[RENDER] silentChangeSize({}, {}) rejected - "
                           "refusing to zero the cocos view",
                           size.width, size.height);
        return;
    }
    auto director = CCDirector::sharedDirector();
    auto view = CCEGLView::sharedOpenGLView();
    diagViewState("pre-set", 100);
    view->CCEGLViewProtocol::setFrameSize(size.width, size.height);
    director->updateScreenScale(size);
    diagViewState("post-scale", 100);
    director->setViewport();
    director->setProjection(kCCDirectorProjection2D);
    glViewport(0, 0, size.width, size.height);
}

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

}

void Renderer::acquireView() {
    const CCSize renderSize(static_cast<float>(mWidth),
                            static_cast<float>(mHeight));
    if (m_viewResized) {
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
    if (!m_viewResized || m_windowW <= 0.0f || m_windowH <= 0.0f) {
        return;
    }
    silentChangeSize(CCSize(m_windowW, m_windowH));
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (auto* other = CCShaderCache::sharedShaderCache()->programForKey(
            kCCShader_PositionColor)) {
        other->use();
    }
}

void Renderer::storeWindowFbSize(float fbW, float fbH) {
    m_windowFbW = fbW;
    m_windowFbH = fbH;
    m_windowW = fbW * m_viewToFbScaleX;
    m_windowH = fbH * m_viewToFbScaleY;
}

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

std::vector<std::filesystem::path> Renderer::ffmpegSearchPaths() {
    return skkbot2::paths::ffmpegSearchPaths();
}

std::filesystem::path Renderer::preferredFfmpegDir() {
    return skkbot2::paths::ffmpeg();
}

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

    skkbot2::log::info("[RENDER] hw probe: {}",
                       hwProbeLine(probeHwFrames(m_ffDir)));

    const char* kCurated[] = {
        "libx264",
        "libx265",
        "h264_nvenc",
        "hevc_nvenc",
        "h264_amf",
        "hevc_amf",
        "h264_qsv",
        "hevc_qsv",
        "libsvtav1",
        "libvpx-vp9",
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
        mCtx->max_b_frames = 0;
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
            { "p4",      true,  true },
            { "medium",  true,  true },
            { "default", false, true },
            { nullptr,   false, false }
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
            AVDictionary* bareOpts = nullptr;
            if (cname == "libx264" || cname == "libx265") {
                m_ff.av_dict_set(&bareOpts, "threads", "8", 0);
            }
            clearLastFfLogMsg();
            ret = m_ff.avcodec_open2(mCtx, mCodec, &bareOpts);
            m_ff.av_dict_free(&bareOpts);
        }
        if (ret < 0) {
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

    ret = m_ff.avformat_write_header(mFmt, nullptr);
    if (ret < 0) {
        m_error = "avformat_write_header: " + ffErrString(&m_ff, ret);
        return false;
    }
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

    const size_t ySize = static_cast<size_t>(mWidth) * mHeight;
    AVFrame* f = mYuvFrames[m_yuvIdx];
    m_yuvIdx = (m_yuvIdx + 1) % ENC_FRAMES;
    if (!f) {
        return;
    }
    if (m_ff.av_frame_make_writable(f) < 0) {
        skkbot2::log::error("[RENDER] av_frame_make_writable failed вЂ” frame "
                            "dropped");
        return;
    }
      const double copyT0 = nowS();
      if (alreadyOwned) {
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
    const int64_t sentPts = m_videoPts++;
    f->pts = sentPts;
    m_lastSentPts = sentPts;

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

    drainPackets();
    int sendRet = 0;
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

    if (m_audioChannels != 2) {
        m_error = "audio: expected a stereo mix, got " +
                  std::to_string(m_audioChannels) + " channels";
        return false;
    }

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
    m_viewResized = false;
    m_windowW = 0.0f;
    m_windowH = 0.0f;
    m_windowFbW = 0.0f;
    m_windowFbH = 0.0f;
    if (m_encRunning.load()) {
        m_error = "previous render's encoder thread is still finishing (it "
                  "was too slow) - wait a moment and try again";
        skkbot2::log::error("[RENDER] {}", m_error);
        return false;
    }
    if (!layer || !loadFFmpeg()) {
        return false;
    }


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

    m_audioEnabled = m_audioWanted;

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

    m_audioKickUnknown = false;
    m_audioKickResolved = false;
    m_avSyncHasPrev = false;
    m_avSyncPrevPts = -1;
    m_avSyncPrevSim = 0.0;
    if (auto* pl = PlayLayer::get()) {
if (pl->m_musicPrepared) {
        pl->startMusic();
        if (auto* engine = FMODAudioEngine::get()) {
            const int musicId = getLastMusicId();
            if (musicId <= 0) {
                armMusicRestart();
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
            bool haveChannel = false;
            if (auto* engine0 = FMODAudioEngine::get()) {
                if (FMOD::Channel* chan0 = engine0->getActiveMusicChannel(0)) {
                    bool playing0 = false;
                    haveChannel = chan0->isPlaying(&playing0) == FMOD_OK &&
                                  playing0;
                }
            }
            if (!haveChannel) {
                armMusicRestart();
                m_audioKickUnknown = true;
                m_audioKickResolved = false;
                skkbot2::log::info("[RENDER] audio kick: song not prepared and "
                                   "no live music channel - deferred restart "
                                   "armed");
            } else {
                auto* engine = FMODAudioEngine::get();
                const double songOffset = pl->m_levelSettings
                    ? pl->m_levelSettings->m_songOffset : 0.0;
                const unsigned int target = static_cast<unsigned int>(
                    std::max(0.0,
                             (songOffset + pl->m_gameState.m_levelTime) * 1000.0));
                skkbot2::log::info(
                    "[RENDER] audio kick: song not prepared but the live music "
                    "channel is paused at the pre-render position - NOT "
                    "restarting; seeking the channel to {}ms (songOffset "
                    "{:.1f} + levelTime {:.3f})",
                    target, songOffset, pl->m_gameState.m_levelTime);
                engine->setMusicTimeMS(target, false, 0);
            }
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

    {
        diagViewState("begin", 100);
        skkbot2::log::info("[RENDER] capture targets {}x{} (view armed "
                           "silicate-style on next render frame)", mWidth, mHeight);
    }

    auto* nEngine = ReplayEngine::get();
    mTps = std::max(1.0, nEngine->runtimeTickRate());
    mSpeed = nEngine ? std::max(0.0, nEngine->gameSpeed) : 1.0;
    if (mSpeed <= 0.0) {
        mSpeed = 1.0;
    }
    skkbot2::log::info("[RENDER] speed captured: {:.3f}x (clock = tps x speed, "
                       "independent of Lock Delta / Real Time)", mSpeed);

    mCapture.init(mWidth, mHeight);

    m_ring.assign(RING_SIZE, RingEntry{});
    m_ringPts.assign(RING_SIZE, -1);
    m_rd = 0;
    m_wr = 0;
    m_qClosed = false;

    mLastPts = -1;
    m_audioAnchor = 0.0;
    m_avSyncLogPts = -1;
    m_videoPts = 0;
    mLastFramed = 0;
    mGrabFailStreak = 0;
    mRingFullFrames = 0;
    mTrailerRemain = std::max(kTrailerHold, m_fadeOutSeconds);
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
    m_encCopyMs = 0.0;
    m_encSwsMs = 0.0;
    m_encSendMs = 0.0;
    m_encDrainMs = 0.0;
    m_encEagain = 0;
    m_batchLimit = 3;
    m_batchFullStreak = 0;
    m_batchShortStreak = 0;
    m_pauseLogged = false;
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
        encodeOneFrame(entry.ptr, pts, entry.preCopied);
        if (entry.slot >= 0) {
            mCapture.releaseSlot(entry.slot);
        } else if (entry.preCopied) {
            mCapture.releaseSync();
        }
        m_qCv.notify_one();
    }
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
struct StageTimer {
    double* acc;
    double t0;
    explicit StageTimer(double* a) : acc(a), t0(nowS()) {}
    ~StageTimer() { *acc += nowS() - t0; }
};
}

void Renderer::drive(GJBaseGameLayer* layer,
                     std::function<void(float)> stepOne) {
    if (!mRecording.load()) {
        return;
    }
    m_driveCalls++;

    int produced = 0;
    for (int k = 0; k < m_batchLimit; ++k) {
        if (!driveOneFrame(layer, stepOne)) {
            ++mBatchStopDrive;
            break;
        }
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
    const float simDt = 1.0f / static_cast<float>(mFps);

    if (ringFull() || !mCapture.hasFreeSlot()) {
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

    const double nowW = nowS();
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

    bool levelDone = pl->m_hasCompletedLevel;
    if (levelDone) {
        if (mTrailerRemain <= 0.0) {
            stop();
            return false;
        }
        if (m_pendingFrame) {
            return false;
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
        audioPump(m_audioAnchor + static_cast<double>(mLastPts) / mFps);
        logAvSync(pl);
        return true;
    }

    const uint64_t f = computeRenderFrame(pl, mTps);
    if (pl->m_isPaused) {
        if (m_pendingFrame) {
            ++m_pendingDrops;
        }
        m_pendingFrame = false;
        m_pendingFadeOut = 1.0f;
        return false;
    }
    if (mLastPts < 0 && pl->m_gameState.m_levelTime <= 0.0) {
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
        return false;
    }
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

    const int64_t frameIndex = mLastPts < 0 ? 0 : mLastPts + 1;
    if (frameIndex == 0) {
        m_audioAnchor = pl->m_gameState.m_levelTime;
        const bool pendingRestart = musicRestartPending();
        const double trackPos = pendingRestart
            ? 0.0
            : musicPositionSeconds();
        AudioCapture::get().anchorAt(m_audioAnchor,
                                     trackPos < 0.0 ? 0.0 : trackPos);
        AudioCapture::get().seedLastLevelTime(m_audioAnchor);
        if (m_audioEnabled && musicRestartPending()) {
            m_audioKickResolved = true;
            skkbot2::log::warn(
                "[RENDER] audio: the level song is not prepared at video pts 0 "
                "- the deferred start will begin the track at position 0 a few "
                "frames in, so the music will be late by that much (the video "
                "timeline itself is unaffected)");
        }
    }
    const int64_t targetTick = static_cast<int64_t>(std::llround(
        static_cast<double>(frameIndex) * mTps * mSpeed /
        static_cast<double>(mFps)));
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
        ++mClockZeroFrames;
    }

    if (ticksThisFrame > 0) {
        StageTimer simTimer(&m_stageMs[static_cast<int>(Stage::Sim)]);
        stepOne(static_cast<float>(static_cast<double>(ticksThisFrame) / mTps));
        mClockTick += static_cast<uint64_t>(ticksThisFrame);
        m_stepCount += static_cast<uint64_t>(ticksThisFrame);
    }
    const uint64_t after = computeRenderFrame(pl, mTps);
    const int64_t actual = static_cast<int64_t>(after) - static_cast<int64_t>(f);
    mClockAskedSum += ticksThisFrame;
    mClockDoneSum += actual;
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
        return false;
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

    mLastPts = frameIndex;
    m_pendingPts = frameIndex;
    m_pendingFadeOut = 1.0f;
    m_pendingFrame = true;

    audioPump(m_audioAnchor + static_cast<double>(mLastPts) / mFps);
    logAvSync(pl);
    return true;
}

double Renderer::renderElapsedSeconds() const {
    if (!mRecording.load()) {
        return 0.0;
    }
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
    if (!mRecording.load() || !m_pendingFrame) {
        return false;
    }

    if (!tryAcquireSlot()) {
        return false;
    }
    m_pendingFrame = false;

    acquireView();

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

    render::GrabResult got;
    const double g0 = nowS();
    const bool grabbed = mCapture.grab(m_pendingPts, &got);
    const int64_t gotPts = got.pts;

    if (!grabbed) {
        m_grabFailTotal++;
        if (++mGrabFailStreak <= 5 || (mGrabFailStreak % 30) == 0) {
            skkbot2::log::warn("[RENDER] grab failed (streak={} pts={} "
                               "captured={} encoded={})",
                               mGrabFailStreak, m_pendingPts,
                               mCapturedFrames.load(), mEncodedFrames.load());
        }
        if (mGrabFailStreak >= 30) {
            skkbot2::log::error(
                "[RENDER] capture pipeline produced no frames ({} "
                "consecutive grab failures) вЂ” aborting render",
                mGrabFailStreak);
            stop();
        }
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

    static constexpr double kStallMs = 200.0;

    const int idx = static_cast<int>(which);
    if (ms < kStallMs) return;
    m_stageStallPending = true;

    if (which == Stage::Frame) {
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
    restoreView();
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
    skkbot2::log::info(
        "[RENDER] stages: frame={:.2f}ms body={:.3f} drive={:.3f} sim={:.3f} "
        "grab={:.3f} preview={:.3f} viewUi={:.3f} swap={:.3f} "
        "outsideStages={:.3f} [raw us: drive={:.1f} sim={:.1f} preview={:.1f} "
        "viewUi={:.1f} swap={:.1f} frames={:.0f}]",
        frameMs, hookAvg, driveAvg, simAvg, grabAvgStage, prevAvg, viewUiAvg,
        swapAvg,
        std::max(0.0, frameMs - hookAvg - grabAvgStage),
        m_stageMs[static_cast<int>(Stage::Drive)] * 1e3,
        m_stageMs[static_cast<int>(Stage::Sim)] * 1e3,
        m_stageMs[static_cast<int>(Stage::Preview)] * 1e3,
        m_stageMs[static_cast<int>(Stage::ViewUi)] * 1e3,
        m_stageMs[static_cast<int>(Stage::Swap)] * 1e3, frames);

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

    if (frames > 0) {
        const double secs = wall > 0.0 ? wall : 1.0;
        skkbot2::log::info(
            "[RENDER] hot path per wall second ({}s wall, {:.0f} sched calls/s):",
            std::to_string(secs).substr(0, 5), frames / secs);
        auto& hs = skkbot2::hot::stats();
        const double stepsPerFrame =
            hs.calls[static_cast<size_t>(skkbot2::hot::PhysStep)] / frames;
        const double schedPerFrame =
            hs.calls[static_cast<size_t>(skkbot2::hot::SchedUpdate)] / frames;
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


    if (m_audioEnabled) {
        AudioCapture::get().detach();
        m_audioEnabled = false;
    }

    restoreView();
    skkbot2::log::info("[RENDER] view restored {}x{}", m_windowW, m_windowH);
}

}
}
