#include "skkBot.hpp"
#include "render/renderer.hpp"
#include "render/audiocap.hpp"
#include "gui/skkbot_gui.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
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

// Video renderer integration (ported from skkBot v0.0.4):
//   - CCDirector::drawScene hijack while rendering (Silicate-style): the
//     level is NEVER drawn to the window during a render вЂ” the sim is stepped
//     tick-by-tick (Renderer::drive -> scheduler update), the last captured
//     frame is blitted as a letterboxed preview, then swapBuffers runs the
//     imgui-cocos hook which issues the frame capture.
//   - FMOD audio capture gate: while a render captures audio the offline
//     NOSOUND_NRT mixer may advance ONLY inside AudioCapture::pump's update
//     window. GD's own FMODAudioEngine::update is swallowed outside it, and
//     the raw FMOD::System::update hook freezes the mixer the same way.
//   - music-id tracker: the renderer needs the level song's musicID to kick
//     the music from position 0 at capture start (renders inherit whatever
//     FMOD state the game is in; a mute/unprepared song would produce a
//     silent video).

namespace skkbot2 {
namespace render {

namespace {
std::atomic<bool> s_musicRestartPending{false};
int s_lastMusicId = -1;
}  // namespace

int getLastMusicId() {
    return s_lastMusicId;
}

void setLastMusicId(int id) {
    s_lastMusicId = id;
}

void armMusicRestart() {
    s_musicRestartPending.store(true);
}

// Drop a pending deferred song start. Called when the PlayLayer goes away
// (the song can never start without a level); otherwise the armed flag keeps
// the FMOD update hook ticking AUDIO_DEFERRED_WAIT forever (the log spammed
// pl=0 prepared=-1 once per second for minutes after leaving the level).
void disarmMusicRestart() {
    s_musicRestartPending.store(false);
}

bool musicRestartPending() {
    return s_musicRestartPending.load();
}

// Where the level song currently sits inside its own track, in seconds, or a
// negative value when no channel can be identified. The renderer uses it to
// anchor the audio timeline: begin() restarts the song from 0, so this is
// normally 0, but if that restart silently failed the track is already
// somewhere else and the pre-roll must not double the offset.
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
        // Fallback: the channel that has been running the longest is the
        // track (SFX are short and restart constantly).
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

// Deferred song start used by the renderer: the level song may still be
// loading when a render begins (m_musicPrepared = false). Armed by begin(),
// ticked from FMODAudioEngine::update (below) until the song is ready, then
// started so the NRT audio pump captures music from the level start.
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
        // Rate-limited visibility: if this NEVER logs while a restart is
        // pending, the deferred mechanism is dead (the FMOD update hook does
        // not tick or m_musicPrepared never flips).
        auto now = std::chrono::steady_clock::now();
        if (now - s_lastWaitLog > std::chrono::seconds(1)) {
            s_lastWaitLog = now;
            skkbot2::log::debug("AUDIO_DEFERRED_WAIT pl={} prepared={}",
                pl ? 1 : 0, pl ? (pl->m_musicPrepared ? 1 : 0) : -1);
        }
        return false;  // still loading вЂ” keep waiting
    }
    s_musicRestartPending.store(false);
    // Audio-kick audit: report the success to the renderer before doing
    // anything else, so even a partially failed start records that the
    // deferred path fired.
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

// v58.22 silicate parity: freeze the mixer at the very bottom of GD's audio
// stack. While a render is active and AudioCapture::pump's update window is
// closed, the offline mixer does NOT advance no matter who calls
// System::update (the FMODAudioEngine::update hook above only covers GD's
// normal engine path). Inside the window the mixer advances exactly as much
// as (fmodTime - time) * sampleRate demands, so audio stays locked to the
// level's sim time (and to the encoder-consumed audio via time(): a slow
// encoder stalls the mixer, it never races ahead).
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

// v58.22 silicate parity: freeze the mixer at the very bottom of GD's audio
// stack. While a render is active and AudioCapture::pump's update window is
// closed, the offline mixer does NOT advance no matter who calls
// System::update (the FMODAudioEngine::update hook above only covers GD's
// normal engine path). Inside the window the mixer advances exactly as much
// as (fmodTime - time) * sampleRate demands, so audio stays locked to the
// level's sim time (and to the encoder-consumed audio via time(): a slow
// encoder stalls the mixer, it never races ahead).
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
    // Safety net: a 0 buffer length would make the loop below spin forever
    // (processedSamples never advances) and soft-lock the main thread - the
    // 2026/09/16 session went fully black with dead buttons during a render.
    if (bufferLength == 0) {
        bufferLength = 1024;  // FMOD default
    }
    const double requiredDt = std::max(ac.fmodTime() - ac.time(), 0.0);
    const int requiredSamples = static_cast<int>(
        requiredDt * static_cast<double>(ac.sampleRate()));
    int processedSamples = 0;
    // Hard iteration cap: the encoder-consume backpressure in time() keeps
    // this in single-digit iterations normally. A mixer that produces no PCM
    // (silent render) without advancing time() must still give up and let the
    // wall frame proceed instead of hogging the main thread forever.
    while (requiredSamples > processedSamples && processedSamples < 1000000) {
        self->update();  // one DSP buffer = bufferLength samples
        processedSamples += static_cast<int>(bufferLength);
    }
    ac.onPumped(processedSamples);
}

}  // namespace render
}  // namespace skkbot2

// ===================== RENDER DRAW LOOP =====================
// CCDirector::drawScene hook вЂ” Silicate-style hijack during an active render.
// Normally the game scene is drawn here; while rendering we instead:
//   1. step the sim (drive()) вЂ” one physics step per dropped wall frame,
//      with capture queued at video-pts boundaries,
//   2. blit the last captured scene letterboxed into the window (drawPreview),
//   3. present. The imgui swapBuffers hook captures the new frame afterwards.
// The level is NEVER drawn to the window during a render (that's the point).
// Draw order per wall frame: [prev capture] -> preview -> menu -> [new capture]
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
            // Show the pause menu via the normal path; drive() has already
            // cleared the pending capture so the menu doesn't leak into the
            // video. Restore the real window-size view so the pause menu (and
            // imgui-cocos, which sizes the bot menu off getFrameSize()) render
            // at full window size вЂ” the output view is re-applied lazily by
            // onFrameDraw() before the next capture.
            // Both streams freeze: drive() is not called, so no video frame is
            // queued and no audioPump runs - the pause is NOT recorded, it only
            // stalls the wall clock. Logged once so the hold is visible.
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
        // Silicate parity: before the level has started (fade-in/spawn), the
        // vanilla drawScene callback is the only thing that advances the intro
        // sequence вЂ” the scheduler that drive() steps is gated on m_started.
        if (!pl->m_started) {
            CCDirector::drawScene();
            return;
        }
        // Silicate-style view lock: the cocos view is held at the OUTPUT size
        // for the whole render (applied ONCE here instead of per-capture).
        renderer.acquireView();
        // Everything measured so far accounted for ~1.9ms of a 10.28ms frame.
        // This timer covers the WHOLE hijacked frame; whatever it does not
        // account for is GD's own main-loop overhead, and whatever it does is
        // either our code or the GPU blocking us in swapBuffers below.
        const double frameT0 = skkbot2::render::Renderer::nowWall();
        // Step the sim tick-by-tick (each step = one scheduler update), blit
        // the preview, then present. swapBuffers runs the imgui hook which
        // issues the capture for this frame.
        renderer.drive(pl, [this](float physicsDt) {
            this->m_pScheduler->update(physicsDt);
        });
        // The preview is the render's only live feedback. Skipping it (there was a
        // version that did, under "hide menu while rendering") left a BLACK
        // WINDOW for the whole render and froze every HUD counter, so it read
        // as a hang. The blit stays; the ImGui pass is what actually got
        // skipped, because that one never reaches the video.
        renderer.drawPreview();
        // Keep the bot menu at real-window scale: imgui-cocos reads
        // getFrameSize() in newFrame()/renderFrame(), so switch the view back
        // to the WINDOW size before presenting (the output view is re-applied
        // by onFrameDraw()->acquireView() on the next render frame).
        const double viewT0 = skkbot2::render::Renderer::nowWall();
        renderer.useWindowViewForUi();
        renderer.noteStage(skkbot2::render::Renderer::nowWall() - viewT0, skkbot2::render::Renderer::Stage::ViewUi);
        const double swapT0 = skkbot2::render::Renderer::nowWall();
        this->m_pobOpenGLView->swapBuffers();
        renderer.noteStage(skkbot2::render::Renderer::nowWall() - swapT0, skkbot2::render::Renderer::Stage::Swap);
        renderer.noteStage(skkbot2::render::Renderer::nowWall() - frameT0, skkbot2::render::Renderer::Stage::Frame);
    }
};

// ===================== VIEW SIZE GATE =====================
// While a render holds the cocos view at the OUTPUT size (silicate
// acquireView), GD's own window resize / framebuffer notifications must NOT
// reach the cocos view вЂ” they would fight the output projection mid-render.
// The renderer swallows the events (returns true) and remembers the new
// physical window size so restoreView() can still restore the CURRENT window
// after the render. Outside a render these pass through untouched.
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

// ===================== DEFERRED MUSIC RESTART CLEANUP =====================
// begin() may arm a deferred song start when the level song is not prepared
// yet. If the player leaves the level (PlayLayer gone) the armed flag would
// otherwise stay set forever, and tickPendingMusicRestart() would log
// AUDIO_DEFERRED_WAIT once per second for the rest of the session. Drop the
// arm when the PlayLayer exits.
void traceMusicStart(const char* tag, unsigned int musicID, unsigned int timeArg);
class $modify(PlayLayer) {
    $override
    void onExit() {
        skkbot2::render::disarmMusicRestart();
        // BUG-80: the dead PlayLayer's heap address will be reused by the
        // next one; disarm the shared [ENTRY-FREEZE] watcher so the next
        // entry (vanilla or queued-session) rearms instead of being mistaken
        // for the watched one.
        skkbot2::entryFreezeReset();
        PlayLayer::onExit();
    }

    // BUG-80: the attempt-start song kick. This is the function that decides
    // WHEN the (already loaded by loadMusic) channel becomes the run's audible
    // track. Vanilla calls it with the run; logging it next to [START-WIN]
    // shows whether the bot entry kicks the song at the same physics moment
    // or lets the loadMusic channel play through the freeze window.
    $override
    void startMusic() {
        traceMusicStart("playLayer.startMusic", 0, 0);
        PlayLayer::startMusic();
    }
};

// ===================== AUDIO CAPTURE GATE =====================
// While the renderer captures audio, FMOD runs on the offline NOSOUND_NRT
// mixer advanced manually from drive() (Renderer::audioPump) at the level's
// game time. GD's own per-frame engine update must NOT run in parallel or the
// mixer would also advance in wall time and desync.

// BUG-80 music-start trace: the user reports the level song plays DURING the
// ~1 s level-entry freeze when a bot session is queued outside the level,
// while a vanilla entry starts the song with the run. Log every song-start
// entry point with the game clock + the pause-family byte GD freezes physics
// on; comparing vanilla vs bot entries then shows WHO started the song early
// and WHEN relative to the frozen-gate window.
// v3: also read the FMOD truth of the song channel (channel 0): playing /
// paused / position in ms + the music bus volume - the AUDIBLE moment, not
// just the API calls.
static void traceMusicStart(const char* tag, unsigned int musicID, unsigned int timeArg) {
    auto* playLayer = PlayLayer::get();
    double levelTime = 0.0;
    int frozenFlag = -1;
    if (playLayer) {
        levelTime = playLayer->m_gameState.m_levelTime;
        frozenFlag = static_cast<int>(
            *reinterpret_cast<const uint8_t*>(
                reinterpret_cast<uintptr_t>(playLayer) + 12888));
    }
    auto* engine = ReplayEngine::get();
    skkbot2::log::info(
        "[SONG] {} id={} offset={}ms levelTime={:.4f} pauseFlag={} engineMode={} playLayer={}",
        tag, musicID, timeArg, levelTime, frozenFlag,
        engine ? static_cast<int>(engine->engineMode) : -2,
        playLayer ? 1 : 0);
    // FMOD truth: dump ALL channels with playing/paused/position. The level
    // song's key in m_channelIDToChannel is not obviously 0 (chan0=-2 misses),
    // so instead of guessing the key, log every live channel - one line.
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
        // Deferred restart tick runs BEFORE the capture gate so the song can
        // start mid-render (the NRT pump picks it up immediately).
        skkbot2::render::tickPendingMusicRestart();
        auto& ac = skkbot2::render::AudioCapture::get();
        if (ac.active()) {
            // Full freeze: the mixer may advance ONLY inside pump()'s update
            // window (m_shouldUpdateFmod). Any engine update reaching here
            // outside the window is swallowed вЂ” the mixer stays put.
            if (!ac.shouldUpdateFmod()) {
                return;
            }
            ac.addFmodTime(dt);
            FMODAudioEngine::update(dt);
            return;
        }
        FMODAudioEngine::update(dt);
    }

    // Music-id tracker: capture the ID GD assigns to the level song (starts
    // through loadAndPlayMusic / queueStartMusic on 2.2 / loadMusic via the
    // engine's startMusic flow). The renderer kicks the song from position 0
    // with this ID at capture start and logs the AUDIO_* flow.
    $override
    void loadAndPlayMusic(gd::string path, unsigned int time, int musicID) {
        traceMusicStart("loadAndPlayMusic", static_cast<unsigned int>(musicID), time);
        skkbot2::render::setLastMusicId(musicID);
        FMODAudioEngine::loadAndPlayMusic(path, time, musicID);
    }

    $override
    void queueStartMusic(gd::string audioFilename, float pitch, float unknown,
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

    // Channel lifecycle around the run: setMusicTimeMS repositions the level
    // song; pauseAllMusic/stopMusic are inline in the bindings (not hookable)
    // so only the seek is traced.
    $override
    void setMusicTimeMS(unsigned int time, bool dontWait, int musicID) {
        traceMusicStart("setMusicTimeMS", static_cast<unsigned int>(musicID), time);
        FMODAudioEngine::setMusicTimeMS(time, dontWait, musicID);
    }

    $override
    void playMusic(gd::string path, bool shouldLoop, float fadeInTime,
                   int channel) {
        traceMusicStart("playMusic", 0, 0);
        FMODAudioEngine::playMusic(path, shouldLoop, fadeInTime, channel);
    }

    $override
    void loadMusic(gd::string path, float speed, float unknown, float volume,
                   bool shouldLoop, int musicID, int channelID,
                   bool dontReset) {
        traceMusicStart("loadMusic", static_cast<unsigned int>(musicID), 0);
        skkbot2::render::setLastMusicId(musicID);
        FMODAudioEngine::loadMusic(path, speed, unknown, volume, shouldLoop,
            musicID, channelID, dontReset);
    }
};

$execute {
    // Raw FMOD::System::update freeze (see FMODSystemUpdateHook above).
    auto addr = reinterpret_cast<void*>(
        geode::addresser::getNonVirtual(&FMOD::System::update));
    auto res = Mod::get()->hook(
        addr,
        &skkbot2::render::FMODSystemUpdateHook,
        "FMOD::System::update",
        tulip::hook::TulipConvention::Stdcall);
    if (res.isOk()) {
        skkbot2::render::s_fmodUpdateHook = res.unwrap();
        // Disabled by default so normal gameplay and editor use 100% vanilla FMOD
        (void)skkbot2::render::s_fmodUpdateHook->disable();
    } else {
        skkbot2::log::error("Failed to hook FMOD::System::update: {}", res.unwrapErr());
    }
}