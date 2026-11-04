#include "audiocap.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include <Geode/binding/FMODAudioEngine.hpp>

#include "../logger/logger.hpp"

using namespace geode::prelude;


namespace skkbot2 {
namespace render {

AudioCapture::~AudioCapture() {
    // Process/DLL teardown safety net: do NOT touch FMOD here. By the time this
    // static destructor runs (dllmain_crt_process_detach) the FMOD runtime is
    // already in teardown - detach()'s setOutput(AUTODETECT) crashed inside
    // AUDIOSES/UM-PDC (c000000d), and m_dsp->release() on a half-unloaded FMOD
    // is equally unsafe. detach() is called by Renderer::stop() during the
    // normal session; if the game was killed mid-render just leak - the OS
    // reclaims the DSP and system on process exit.
    m_active = false;
    m_dsp = nullptr;
}

bool AudioCapture::init() {
    if (m_dsp) {
        return true;
    }
    auto* engine = FMODAudioEngine::get();
    if (!engine || !engine->m_system) {
        return false;
    }

    FMOD_DSP_DESCRIPTION desc = {};
    std::strcpy(desc.name, "skk audio capture");
    desc.version = 0x00020000;
    desc.numinputbuffers = 1;
    desc.numoutputbuffers = 1;
    desc.read = &AudioCapture::dspRead;
    desc.numparameters = 0;

    FMOD::System* system = engine->m_system;
    if (system->getMasterChannelGroup(&m_master) != FMOD_OK || !m_master) {
        return false;
    }
    if (system->createDSP(&desc, &m_dsp) != FMOD_OK || !m_dsp) {
        return false;
    }
    skkbot2::log::info("[RENDER] audio capture DSP created");
    return true;
}

bool AudioCapture::attach(float musicVolume, float sfxVolume) {
    if (m_active || !init()) {
        return m_active;
    }
    auto* engine = FMODAudioEngine::get();
    if (!engine || !engine->m_system) {
        return false;
    }

    int numDsps = 0;
    m_master->getNumDSPs(&numDsps);
    if (m_master->addDSP(numDsps, m_dsp) != FMOD_OK) {
        skkbot2::log::error("[RENDER] audio: addDSP failed");
        return false;
    }

    // Silicate parity: make the offline mixer PRODUCE sound no matter what
    // the game state is вЂ” GD can leave the master group paused (observed
    // as FMODAudioEngine::m_allAudioPaused=1: the mixer mixed pure digital
    // silence for the whole render) and the user's music/SFX sliders can be
    // at 0 (music=0.19, sfx=0.0 in the 21:37 render). Unpause the master
    // group and force both engine volumes to 100% for the duration of the
    // render, restoring the previous values in detach(). This is exactly
    // what Silicate's AudioRecorder::attach does (setPaused(false) +
    // setBackgroundMusicVolume/setEffectsVolume with its own render
    // settings, default 1.0).
    m_master->setPaused(false);
    // v58.24: unfreeze the engine's global audio-paused flag. We attach from the
    // pause menu (m_allAudioPaused=1) and FMODAudioEngine::update re-pauses every
    // channel on each frame while the flag is set вЂ” queue-triggered music swaps
    // keep starting paused=1 and stay paused, producing 1s audio holes.
    engine->m_allAudioPaused = false;
    m_prevMusicVolume = engine->getBackgroundMusicVolume();
    m_prevSfxVolume = engine->getEffectsVolume();
    engine->setBackgroundMusicVolume(std::clamp(musicVolume, 0.0f, 1.0f));
    engine->setEffectsVolume(std::clamp(sfxVolume, 0.0f, 1.0f));

    // Mixer block size: 1024 samples x 2 buffers (same as silicate) вЂ” this
    // is also the granularity of pump()'s per-update advance.
    engine->m_system->getDSPBufferSize(&m_dspBufferLen, &m_dspBufferCount);
    engine->m_system->setDSPBufferSize(1024, 2);

    // Switch to the offline mixer FIRST, then read the format it actually
    // mixes at (silicate parity: the NRT output can change the rate).
    engine->m_system->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT);
    engine->m_system->getSoftwareFormat(&m_sampleRate, nullptr, &m_channels);
    if (m_sampleRate <= 0) {
        m_sampleRate = 44100;
    }

    if (m_channels != 2) {
        // Preferred path: tell the mixer to output plain stereo (the mixer
        // resamples to whatever format is requested, pitch unaffected).
        FMOD_RESULT sret = engine->m_system->setSoftwareFormat(
            m_sampleRate, FMOD_SPEAKERMODE_STEREO, 2);
        if (sret == FMOD_OK) {
            engine->m_system->getSoftwareFormat(&m_sampleRate, nullptr,
                                                &m_channels);
        }
        if (m_channels != 2) {
            // The mixer keeps its speaker count (e.g. a 7.1 device config)
            // вЂ” the DSP then delivers that many interleaved channels and
            // onData() downmixes them to stereo. The encoder path only
            // ever sees 2ch either way.
            skkbot2::log::info(
                "[RENDER] audio: mixer stays at {} ch -> downmix to stereo",
                m_channels);
        }
    }
    m_channels = 2;

    m_pumped = 0;
    m_pumpedConsumed = 0;
    m_pcm.clear();
    m_rmsSum = 0.0;
    m_rmsSamples = 0;
    m_rmsWinSum = 0.0;
    m_rmsWinSamples = 0;
    m_rmsLastLog = 0;
    m_active = true;

    // v58.22: reset the silicate-parity clock. m_fmodTime must start at 0
    // here вЂ” if it kept the pre-render accumulated session time, the first
    // update window would demand hours of audio (the 39.3s desync pattern,
    // but far worse). m_time tracks what the encoder has consumed, and
    // m_lastLevelTime seeds the pump's per-frame dt.
    m_shouldUpdateFmod = false;
    m_fmodTime = 0.0;
    m_time = 0.0;
    m_lastLevelTime = 0.0;
    m_anchored = false;
    m_prerollEnd = 0.0;
    m_discardedSamples = 0;
    m_masterClockBase = 0;
    if (m_master) {
        unsigned long long clock = 0;
        unsigned long long parent = 0;
        if (m_master->getDSPClock(&clock, &parent) == FMOD_OK) {
            m_masterClockBase = clock;
        }
    }

    // Volume diagnostics: a silent track usually means the game's own mixer
    // (or the master group) is muted / at 0 volume, not a capture bug.
    float masterVol = 0.0f;
    bool masterMute = false;
    bool masterPaused = false;
    m_master->getVolume(&masterVol);
    m_master->getMute(&masterMute);
    m_master->getPaused(&masterPaused);
    auto* engine2 = FMODAudioEngine::get();
    skkbot2::log::info("[RENDER] audio groups: master vol={:.2f} mute={} "
                       "paused={}",
                       masterVol, masterMute ? 1 : 0, masterPaused ? 1 : 0);
    if (engine2) {
        skkbot2::log::info("[RENDER] audio groups: engine music vol={:.2f} "
                           "sfx vol={:.2f} allPaused={}",
                           engine2->m_musicVolume, engine2->m_sfxVolume,
                           engine2->m_allAudioPaused ? 1 : 0);
    }

    skkbot2::log::info("[RENDER] audio capture attached: {} Hz x {} ch",
                       m_sampleRate, m_channels);
    enableFmodUpdateHook(true);
    return true;
}

void AudioCapture::detach() {
    if (!m_active) {
        return;
    }
    m_active = false;
    enableFmodUpdateHook(false);
    auto* engine = FMODAudioEngine::get();
    if (engine && engine->m_system) {
        m_master->removeDSP(m_dsp);
        engine->m_system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);
        engine->m_system->setDSPBufferSize(m_dspBufferLen, m_dspBufferCount);
        if (m_prevMusicVolume >= 0.0f) {
            engine->setBackgroundMusicVolume(m_prevMusicVolume);
            engine->setEffectsVolume(m_prevSfxVolume);
            m_prevMusicVolume = -1.0f;
            m_prevSfxVolume = -1.0f;
        }
    }
    if (m_rmsSamples > 0) {
        const double rms = std::sqrt(m_rmsSum / m_rmsSamples);
        const double db = 20.0 * std::log10(rms);
        skkbot2::log::info(
            "[RENDER] audio summary: {} samples, avg level {:+.1f} dB{}",
            m_rmsSamples, db,
            db < -70.0
                ? " (SILENT вЂ” the game's mixer produced no sound during the "
                  "render: muted music/SFX or nothing was playing)"
                : "");
    }
    m_pcm.clear();
    m_pumped = 0;
    skkbot2::log::info("[RENDER] audio capture detached");
}

void AudioCapture::anchorAt(double levelTime, double trackPositionSeconds) {
    if (!m_active || m_anchored) {
        return;
    }
    m_anchored = true;
    const double track = std::max(0.0, trackPositionSeconds);
    m_prerollEnd = std::max(0.0, levelTime - track);
    skkbot2::log::info(
        "[RENDER] audio anchor: video pts 0 = levelTime={:.3f}s, level song at "
        "{:.3f}s of its track -> {:.3f}s of pre-roll will be mixed and dropped "
        "so the first encoded block starts the track at the position the "
        "footage starts at", levelTime, track, m_prerollEnd);
}

void AudioCapture::pump(double levelTime) {
    if (!m_active) {
        return;
    }
    // v58.22 silicate parity: instead of driving system->update() directly
    // (which bypasses GD's FMODAudioEngine::update), open the update window,
    // let the ENGINE update with the level's own sim dt, and close the
    // window. GD's m_system->update() inside the engine update lands in the
    // FMOD::System::update hook (hooks.cpp), which advances the offline
    // mixer exactly as much as (m_fmodTime - m_time) demands вЂ” the mixer can
    // never run ahead of the encoder-consumed audio, so the final audio pts
    // stays locked to the video pts.
    const double dt = levelTime - m_lastLevelTime;
    if (dt <= 0.0) {
        return;
    }

    // Timeline anchor. The renderer calls pump() with the VIDEO timeline
    // (anchor + frameIndex / fps), so the first call carries the level time
    // the render started at. That is video pts 0: the music has to be at that
    // same position inside the track, not at 0. Everything the mixer produces
    // up to it is the pre-roll - nobody will ever hear it.
    if (!m_anchored) {
        anchorAt(levelTime, 0.0);
    }

    m_lastLevelTime = levelTime;
    m_shouldUpdateFmod = true;
    FMODAudioEngine::get()->update(static_cast<float>(dt));
    m_shouldUpdateFmod = false;

    if (m_prerollEnd > 0.0) {
        // Drop EXACTLY the pre-roll, sample-accurately. Clearing the whole
        // buffer would overshoot by up to one pump window (a frame plus a DSP
        // block, ~25-40 ms), and the music would then start that much ahead of
        // the footage - a small, permanent offset, which is the very thing
        // this code exists to remove. Whatever is left after the cut stays in
        // m_pcm and is drained as the first encoded block, at pts=0.
        const size_t frames = m_pcm.size() / 2;
        const double already =
            static_cast<double>(m_discardedSamples) / m_sampleRate;
        const double need = m_prerollEnd - already;
        size_t drop = 0;
        if (need > 0.0) {
            const double needFrames = need * static_cast<double>(m_sampleRate);
            drop = needFrames >= static_cast<double>(frames)
                       ? frames
                       : static_cast<size_t>(std::llround(needFrames));
        }
        if (drop > 0) {
            m_pcm.erase(m_pcm.begin(),
                        m_pcm.begin() + static_cast<std::ptrdiff_t>(drop) * 2);
            m_discardedSamples += static_cast<int64_t>(drop);
        }
        m_time = static_cast<double>(m_pumpedConsumed + m_discardedSamples) /
                 static_cast<double>(m_sampleRate);
        const double dropped =
            static_cast<double>(m_discardedSamples) / m_sampleRate;
        if (dropped + 1.0e-6 >= m_prerollEnd) {
            skkbot2::log::info(
                "[RENDER] audio: pre-roll done at {:.3f}s ({} sample(s) "
                "dropped, first encoded block is pts=0 = track position "
                "{:.3f}s, mixer at {:.3f}s)", dropped, m_discardedSamples,
                m_prerollEnd, mixerSeconds());
            m_prerollEnd = 0.0;
        }
    }
}

double AudioCapture::mixerSeconds() {
    if (m_sampleRate <= 0) {
        return 0.0;
    }
    if (m_master) {
        unsigned long long clock = 0;
        unsigned long long parent = 0;
        if (m_master->getDSPClock(&clock, &parent) == FMOD_OK &&
            clock >= m_masterClockBase) {
            return static_cast<double>(clock - m_masterClockBase) /
                   static_cast<double>(m_sampleRate);
        }
    }
    return static_cast<double>(m_pumped) / static_cast<double>(m_sampleRate);
}

size_t AudioCapture::drain(std::vector<float>& accum, int frameSize,
                           const std::function<void(const float*, int64_t)>&
                               sink) {
    if (m_pcm.empty()) {
        return 0;
    }
    accum.insert(accum.end(), m_pcm.begin(), m_pcm.end());
    m_pcm.clear();

    const size_t blockSize = static_cast<size_t>(frameSize) * m_channels;
    size_t consumed = 0;
    size_t available = accum.size();
    int64_t pts = 0;
    // NOTE: the loop MUST subtract what was already consumed вЂ” comparing the
    // full size() (which only shrinks after the loop, in the erase below)
    // runs one iteration too many whenever the buffer holds an exact number
    // of blocks (it always does: the DSP appends full 2048-float updates).
    // That extra block read past the data end вЂ” it survived until the read
    // crossed into unmapped memory, crashing writeAudioBlock (the swr crash
    // in v57.4 was this same bug surfacing one call deeper).
    while (available >= blockSize) {
        pts = static_cast<int64_t>(m_pumpedConsumed);
        sink(accum.data() + consumed, pts);
        consumed += blockSize;
        available -= blockSize;
        m_pumpedConsumed += frameSize;
    }
    if (consumed > 0) {
        accum.erase(accum.begin(), accum.begin() + consumed);
    }
    // v58.22: keep m_time glued to what the encoder actually consumed вЂ” the
    // System::update hook computes its required advance from
    // (m_fmodTime - m_time), so an encoder that lags the sim automatically
    // stalls the mixer (backpressure), never lets it race ahead. The dropped
    // pre-roll counts here too: the mixer really did advance over it.
    m_time = static_cast<double>(m_pumpedConsumed + m_discardedSamples) /
             static_cast<double>(m_sampleRate);
    return consumed;
}

// Standard ATSC/ffmpeg-style downmix for FMOD's canonical speaker order
// (FL FR FC LFE BL BR [SL SR]). Unknown layouts keep the first two
// channels. Used when the mixer runs >2ch (e.g. a 7.1 device config) вЂ”
// the encoder path is stereo-only.
static void downmixToStereo(const float* in, float* out, unsigned int samples,
                            int channels) {
    constexpr float k = 0.70710678f;  // 1/sqrt(2)
    switch (channels) {
        case 1:
            for (unsigned int i = 0; i < samples; ++i) {
                out[i * 2] = in[i];
                out[i * 2 + 1] = in[i];
            }
            break;
        case 2:
            std::memcpy(out, in, samples * 2 * sizeof(float));
            break;
        case 4:  // quad: FL FR BL BR
            for (unsigned int i = 0; i < samples; ++i) {
                const float* s = in + i * 4;
                out[i * 2] = s[0] + k * s[2];
                out[i * 2 + 1] = s[1] + k * s[3];
            }
            break;
        case 5:  // surround: FL FR FC BL BR
            for (unsigned int i = 0; i < samples; ++i) {
                const float* s = in + i * 5;
                out[i * 2] = s[0] + k * (s[2] + s[3]);
                out[i * 2 + 1] = s[1] + k * (s[2] + s[4]);
            }
            break;
        case 6:  // 5.1: FL FR FC LFE BL BR
            for (unsigned int i = 0; i < samples; ++i) {
                const float* s = in + i * 6;
                out[i * 2] = s[0] + k * (s[2] + s[4] + s[3]);
                out[i * 2 + 1] = s[1] + k * (s[2] + s[5] + s[3]);
            }
            break;
        case 8:  // 7.1: FL FR FC LFE BL BR SL SR
            for (unsigned int i = 0; i < samples; ++i) {
                const float* s = in + i * 8;
                out[i * 2] = s[0] + k * (s[2] + s[4] + s[6] + s[3]);
                out[i * 2 + 1] = s[1] + k * (s[2] + s[5] + s[7] + s[3]);
            }
            break;
        default:
            for (unsigned int i = 0; i < samples; ++i) {
                out[i * 2] = in[i * channels];
                out[i * 2 + 1] = in[i * channels + 1];
            }
            break;
    }
}

FMOD_RESULT F_CALLBACK AudioCapture::dspRead(FMOD_DSP_STATE*, float* inBuffer,
                                             float* outBuffer,
                                             unsigned int length,
                                             int inChannels, int*) {
    auto& ac = AudioCapture::get();
    if (ac.m_active && inBuffer && inChannels > 0) {
        ac.onData(inBuffer, length, inChannels);
    }
    if (outBuffer) {
        // Silence the output: the render runs the mix many times faster than
        // real time, so nothing should be heard while capturing.
        std::memset(outBuffer, 0,
                    length * static_cast<unsigned int>(inChannels) *
                        sizeof(float));
    }
    return FMOD_OK;
}

void AudioCapture::onData(const float* data, unsigned int samples,
                          int channels) {
    if (channels != 2) {
        const size_t off = m_pcm.size();
        m_pcm.resize(off + static_cast<size_t>(samples) * 2);
        downmixToStereo(data, m_pcm.data() + off, samples, channels);
        for (size_t i = 0; i < static_cast<size_t>(samples) * 2; i++) {
            const double v = static_cast<double>(data[i]);
            m_rmsSum += v * v;
            m_rmsWinSum += v * v;
        }
    } else {
        m_pcm.insert(m_pcm.end(), data,
                     data + static_cast<size_t>(samples) * 2);
        for (unsigned int i = 0; i < samples * 2; i++) {
            const double v = static_cast<double>(data[i]);
            m_rmsSum += v * v;
            m_rmsWinSum += v * v;
        }
    }
    m_rmsSamples += static_cast<int64_t>(samples) * 2;
    m_rmsWinSamples += static_cast<int64_t>(samples) * 2;
    if (m_rmsSamples - m_rmsLastLog >= static_cast<int64_t>(m_sampleRate)) {
        m_rmsLastLog = m_rmsSamples;
        const double winRms = m_rmsWinSamples > 0
                                  ? std::sqrt(m_rmsWinSum / m_rmsWinSamples)
                                  : 0.0;
        skkbot2::log::info("[RENDER] audio level: {:+.1f} dB (samples={})",
                           20.0 * std::log10(winRms), m_rmsSamples);
        m_rmsWinSum = 0.0;
        m_rmsWinSamples = 0;
    }
}

}  // namespace render
}  // namespace skkbot2
