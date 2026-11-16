#include "skkBot.hpp"
#include "render/renderer.hpp"
#include "render/audiocap.hpp"
#include "gui/skkbot_gui.hpp"
#include "version_offsets.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/GJGameState.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/loader/Tulip.hpp>
#include <Geode/utils/addresser.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fmt/format.h>

using namespace geode::prelude;


namespace skkbot2 {
namespace render {

namespace {
std::atomic<bool> s_musicRestartPending{false};
int s_lastMusicId = -1;
}

int getLastMusicId() {
    return s_lastMusicId;
}

void setLastMusicId(int id) {
    s_lastMusicId = id;
}

void armMusicRestart() {
    s_musicRestartPending.store(true);
}

void disarmMusicRestart() {
    s_musicRestartPending.store(false);
}

bool musicRestartPending() {
    return s_musicRestartPending.load();
}

namespace {
}

double musicPositionSeconds() {
    auto* audio = FMODAudioEngine::get();
    if (!audio) {
        return -1.0;
    }
    FMOD::Channel* chan = nullptr;
    if (s_lastMusicId > 0) {
        const int chanId = audio->getMusicChannelID(s_lastMusicId);
        if (chanId >= 0) {
            chan = audio->channelForChannelID(chanId);
        }
    }
    if (!chan) {
        unsigned int best = 0;
        for (auto& [id, c] : audio->m_channelIDToChannel) {
            if (!c) continue;
            bool playing = false;
            if (c->isPlaying(&playing) != FMOD_OK || !playing) continue;
            unsigned int pos = 0;
            if (c->getPosition(&pos, FMOD_TIMEUNIT_MS) == FMOD_OK &&
                pos > best) {
                best = pos;
                chan = c;
            }
        }
        return chan ? static_cast<double>(best) / 1000.0 : -1.0;
    }
    unsigned int pos = 0;
    if (chan->getPosition(&pos, FMOD_TIMEUNIT_MS) != FMOD_OK) {
        return -1.0;
    }
    return static_cast<double>(pos) / 1000.0;
}

bool tickPendingMusicRestart() {
    if (!s_musicRestartPending.load()) {
        return false;
    }
    if (!Renderer::get().isRecording()) {
        disarmMusicRestart();
        return false;
    }
    auto* pl = PlayLayer::get();
    static auto s_lastWaitLog = std::chrono::steady_clock::time_point{};
    if (!pl || !pl->m_musicPrepared) {
        auto now = std::chrono::steady_clock::now();
        if (now - s_lastWaitLog > std::chrono::seconds(1)) {
            s_lastWaitLog = now;
            skkbot2::log::debug("AUDIO_DEFERRED_WAIT pl={} prepared={}",
                pl ? 1 : 0, pl ? (pl->m_musicPrepared ? 1 : 0) : -1);
        }
        return false;
    }
    s_musicRestartPending.store(false);
    skkbot2::render::Renderer::get().noteMusicRestartFired();
    pl->startMusic();
    if (auto* engine = FMODAudioEngine::get()) {
        engine->setBackgroundMusicVolume(engine->m_musicVolume);
        engine->startMusic(0, 0, 0, 0, false, s_lastMusicId, false, false);
        engine->pauseAllAudio();
        engine->resumeAllAudio();
        engine->m_allAudioPaused = false;
        skkbot2::log::info("AUDIO_RESTART_DEFERRED musicID={} vol={:.3f} "
                           "start+pause/resume",
            s_lastMusicId, engine->m_musicVolume);
    } else {
        skkbot2::log::info("AUDIO_RESTART_DEFERRED musicID={}", s_lastMusicId);
    }
    return false;
}

static geode::Hook* s_fmodUpdateHook = nullptr;

void enableFmodUpdateHook(bool enable) {
    if (s_fmodUpdateHook) {
        if (enable) {
            (void)s_fmodUpdateHook->enable();
        } else {
            (void)s_fmodUpdateHook->disable();
        }
    }
}

void FMODSystemUpdateHook(FMOD::System* self) {
    auto& ac = AudioCapture::get();
    if (!ac.active()) {
        self->update();
        return;
    }
    if (!ac.shouldUpdateFmod()) {
        return;
    }
    unsigned int bufferLength = 0;
    int bufferCount = 0;
    self->getDSPBufferSize(&bufferLength, &bufferCount);
    if (bufferLength == 0) {
        bufferLength = 1024;
    }
    const double requiredDt = std::max(ac.fmodTime() - ac.time(), 0.0);
    const int requiredSamples = static_cast<int>(
        requiredDt * static_cast<double>(ac.sampleRate()));
    int processedSamples = 0;
    while (requiredSamples > processedSamples && processedSamples < 1000000) {
        self->update();
        processedSamples += static_cast<int>(bufferLength);
    }
    ac.onPumped(processedSamples);
}

}
}

class $modify(CCDirector) {
    $override
    void drawScene() {
        auto& renderer = skkbot2::render::Renderer::get();
        if (!renderer.isRecording()) {
            CCDirector::drawScene();
            return;
        }
        auto* pl = PlayLayer::get();
        if (!pl) {
            renderer.stop();
            CCDirector::drawScene();
            return;
        }
        if (pl->m_isPaused) {
            if (!renderer.levelPauseLogged()) {
                renderer.noteLevelPaused();
                skkbot2::log::info("[RENDER] level paused - recording held; "
                                   "the pause is not written to the video");
            }
            renderer.restoreViewForPauseMenu();
            CCDirector::drawScene();
            return;
        }
        if (renderer.levelPauseLogged()) {
            renderer.noteLevelResumed();
            skkbot2::log::info("[RENDER] level resumed - video and audio "
                               "continue from the pre-pause position");
        }
        if (!pl->m_started) {
            CCDirector::drawScene();
            return;
        }
        renderer.acquireView();
        const double frameT0 = skkbot2::render::Renderer::nowWall();
        renderer.drive(pl, [this](float physicsDt) {
            this->m_pScheduler->update(physicsDt);
        });
        renderer.drawPreview();
        const double viewT0 = skkbot2::render::Renderer::nowWall();
        renderer.useWindowViewForUi();
        renderer.noteStage(skkbot2::render::Renderer::nowWall() - viewT0, skkbot2::render::Renderer::Stage::ViewUi);
        const double swapT0 = skkbot2::render::Renderer::nowWall();
        this->m_pobOpenGLView->swapBuffers();
        renderer.noteStage(skkbot2::render::Renderer::nowWall() - swapT0, skkbot2::render::Renderer::Stage::Swap);
        renderer.noteStage(skkbot2::render::Renderer::nowWall() - frameT0, skkbot2::render::Renderer::Stage::Frame);
    }
};

class $modify(CCEGLView) {
    $override
    void setFrameSize(float width, float height) {
        if (skkbot2::render::Renderer::get().handleFrameSizeChange(width,
                                                                   height)) {
            return;
        }
        CCEGLView::setFrameSize(width, height);
    }

    $override
    void onGLFWframebuffersize(GLFWwindow* window, int width, int height) {
        if (skkbot2::render::Renderer::get().handleFramebufferSizeChange(
                width, height)) {
            return;
        }
        CCEGLView::onGLFWframebuffersize(window, width, height);
    }

    $override
    void onGLFWWindowSizeFunCallback(GLFWwindow* window, int width,
                                     int height) {
        if (skkbot2::render::Renderer::get().handleWindowSizeChange()) {
            return;
        }
        CCEGLView::onGLFWWindowSizeFunCallback(window, width, height);
    }

    $override
    void toggleLockCursor(bool locked) {
        if (locked && skkbot2::gui::get().open) {
            locked = false;
        }
        CCEGLView::toggleLockCursor(locked);
    }
};

namespace {
    unsigned int g_lastSongTimeMS = 0;
    bool g_haveSongTime = false;
}

void traceMusicStart(const char* tag, unsigned int musicID, unsigned int timeArg);
class $modify(PlayLayer) {
    $override
    void onExit() {
        skkbot2::render::disarmMusicRestart();
        g_lastSongTimeMS = 0;
        g_haveSongTime = false;
        skkbot2::entryFreezeReset();
        PlayLayer::onExit();
    }

    $override
    void startMusic() {
        traceMusicStart("playLayer.startMusic", 0, 0);
        PlayLayer::startMusic();
    }
};


static void traceMusicStart(const char* tag, unsigned int musicID, unsigned int timeArg) {
    auto* playLayer = PlayLayer::get();
    double levelTime = 0.0;
    int frozenFlag = -1;
    if (playLayer) {
        levelTime = playLayer->m_gameState.m_levelTime;
        frozenFlag = static_cast<int>(
            *reinterpret_cast<const uint8_t*>(
                reinterpret_cast<uintptr_t>(playLayer) + skk::offsets::kFieldPauseFlagOffset));
    }
    auto* engine = ReplayEngine::get();
    skkbot2::log::info(
        "[SONG] {} id={} offset={}ms levelTime={:.4f} pauseFlag={} engineMode={} playLayer={}",
        tag, musicID, timeArg, levelTime, frozenFlag,
        engine ? static_cast<int>(engine->engineMode) : -2,
        playLayer ? 1 : 0);
    auto* audio = FMODAudioEngine::get();
    if (!audio) {
        skkbot2::log::info("[SONG] fmod: no engine");
        return;
    }
    if (audio->m_channelIDToChannel.empty()) {
        skkbot2::log::info("[SONG] fmod: no channels");
        return;
    }
    std::string dump;
    for (auto& [id, chan] : audio->m_channelIDToChannel) {
        if (!chan) continue;
        bool playing = false;
        bool paused = false;
        unsigned int pos = 0;
        int state = 0;
        if (chan->isPlaying(&playing) == FMOD_OK && playing) {
            state = (chan->getPaused(&paused) == FMOD_OK && paused) ? 2 : 1;
        }
        (void)chan->getPosition(&pos, FMOD_TIMEUNIT_MS);
        dump += fmt::format(" [{}:s{}@{}]", id, state, pos);
    }
    skkbot2::log::info("[SONG] fmod: busVol={:.2f} allPaused={} chans={}",
        audio->m_musicVolume, audio->m_allAudioPaused ? 1 : 0, dump);
}

class $modify(FMODAudioEngine) {
    $override
    void update(float dt) {
        skkbot2::render::tickPendingMusicRestart();
        auto& ac = skkbot2::render::AudioCapture::get();
        if (ac.active()) {
            if (!ac.shouldUpdateFmod()) {
                return;
            }
            ac.addFmodTime(dt);
            FMODAudioEngine::update(dt);
            return;
        }
        FMODAudioEngine::update(dt);
    }

    $override
    void loadAndPlayMusic(std::string path, unsigned int time, int musicID) {
        traceMusicStart("loadAndPlayMusic", static_cast<unsigned int>(musicID), time);
        skkbot2::render::setLastMusicId(musicID);
        FMODAudioEngine::loadAndPlayMusic(path, time, musicID);
    }

    $override
    void queueStartMusic(std::string audioFilename, float pitch, float unknown,
                         float volume, bool loop, int start, int end,
                         int fadeIn, int fadeOut, int musicID, bool p10,
                         int channelID, bool noPrepare, bool dontReset) {
        traceMusicStart("queueStartMusic", static_cast<unsigned int>(musicID),
                        static_cast<unsigned int>(start));
        skkbot2::render::setLastMusicId(musicID);
        FMODAudioEngine::queueStartMusic(audioFilename, pitch, unknown, volume,
            loop, start, end, fadeIn, fadeOut, musicID, p10, channelID,
            noPrepare, dontReset);
    }

    $override
    void startMusic(int start, int end, int fadeIn, int fadeOut, bool loop,
                    int musicID, bool noResume, bool dontReset) {
        traceMusicStart("startMusic", static_cast<unsigned int>(musicID),
                        static_cast<unsigned int>(start));
        skkbot2::render::setLastMusicId(musicID);
        FMODAudioEngine::startMusic(start, end, fadeIn, fadeOut, loop, musicID,
            noResume, dontReset);
    }

    $override
    void setMusicTimeMS(unsigned int time, bool dontWait, int musicID) {
        g_lastSongTimeMS = time;
        g_haveSongTime = true;
        traceMusicStart("setMusicTimeMS", static_cast<unsigned int>(musicID), time);
        FMODAudioEngine::setMusicTimeMS(time, dontWait, musicID);
    }

    $override
    void playMusic(std::string path, bool shouldLoop, float fadeInTime,
                   int channel) {
        traceMusicStart("playMusic", 0, 0);
        FMODAudioEngine::playMusic(path, shouldLoop, fadeInTime, channel);
    }

    $override
    void loadMusic(std::string path, float speed, float unknown, float volume,
                   bool shouldLoop, int musicID, int channelID,
                   bool dontReset) {
        traceMusicStart("loadMusic", static_cast<unsigned int>(musicID), 0);
        skkbot2::render::setLastMusicId(musicID);
        FMODAudioEngine::loadMusic(path, speed, unknown, volume, shouldLoop,
            musicID, channelID, dontReset);
    }
};

$execute {
    auto addr = reinterpret_cast<void*>(
        geode::addresser::getNonVirtual(&FMOD::System::update));
    auto res = Mod::get()->hook(
        addr,
        &skkbot2::render::FMODSystemUpdateHook,
        "FMOD::System::update",
        tulip::hook::TulipConvention::Stdcall);
    if (res.isOk()) {
        skkbot2::render::s_fmodUpdateHook = res.unwrap();
        (void)skkbot2::render::s_fmodUpdateHook->disable();
    } else {
        skkbot2::log::error("Failed to hook FMOD::System::update: {}", res.unwrapErr());
    }
}
