#include "audiocap.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include <Geode/binding/FMODAudioEngine.hpp>

#include "../logger/logger.hpp"

#include <atomic>
#include <cstring>

using namespace geode::prelude;

namespace {

constexpr uint64_t kMonitorRingFrames = 1u << 17;

}


namespace skkbot2 {
namespace render {

AudioCapture::~AudioCapture() {
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

    m_master->setPaused(false);
    engine->m_allAudioPaused = false;
    m_prevMusicVolume = engine->getBackgroundMusicVolume();
    m_prevSfxVolume = engine->getEffectsVolume();
    engine->setBackgroundMusicVolume(std::clamp(musicVolume, 0.0f, 1.0f));
    engine->setEffectsVolume(std::clamp(sfxVolume, 0.0f, 1.0f));

    engine->m_system->getDSPBufferSize(&m_dspBufferLen, &m_dspBufferCount);
    engine->m_system->setDSPBufferSize(1024, 2);

    engine->m_system->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT);
    engine->m_system->getSoftwareFormat(&m_sampleRate, nullptr, &m_channels);
    if (m_sampleRate <= 0) {
        m_sampleRate = 44100;
    }

    if (m_channels != 2) {
        FMOD_RESULT sret = engine->m_system->setSoftwareFormat(
            m_sampleRate, FMOD_SPEAKERMODE_STEREO, 2);
        if (sret == FMOD_OK) {
            engine->m_system->getSoftwareFormat(&m_sampleRate, nullptr,
                                                &m_channels);
        }
        if (m_channels != 2) {
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

    setMonitorEnabled(
        geode::Mod::get()->getSavedValue<bool>("gui_render_monitor", false));

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
    shutdownMonitor();
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
    const double dt = levelTime - m_lastLevelTime;
    if (dt <= 0.0) {
        return;
    }
    if (dt > 1.0 && m_seedWarned == false) {
        m_seedWarned = true;
        skkbot2::log::error(
            "[RENDER] pump dt anomaly: {:.3f}s in one update "
            "(lastLevelTime={:.3f} -> levelTime={:.3f}) - the mixer will "
            "race, the track will run ahead of the footage",
            dt, m_lastLevelTime, levelTime);
    }

    if (!m_anchored) {
        anchorAt(levelTime, 0.0);
    }

    m_lastLevelTime = levelTime;
    m_shouldUpdateFmod = true;
    FMODAudioEngine::get()->update(static_cast<float>(dt));
    m_shouldUpdateFmod = false;

    if (m_monitorSystem) {
        m_monitorSystem->update();
    }

    if (m_prerollEnd > 0.0) {
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
    m_time = static_cast<double>(m_pumpedConsumed + m_discardedSamples) /
             static_cast<double>(m_sampleRate);
    return consumed;
}

static void downmixToStereo(const float* in, float* out, unsigned int samples,
                            int channels) {
    constexpr float k = 0.70710678f;
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
        case 4:
            for (unsigned int i = 0; i < samples; ++i) {
                const float* s = in + i * 4;
                out[i * 2] = s[0] + k * s[2];
                out[i * 2 + 1] = s[1] + k * s[3];
            }
            break;
        case 5:
            for (unsigned int i = 0; i < samples; ++i) {
                const float* s = in + i * 5;
                out[i * 2] = s[0] + k * (s[2] + s[3]);
                out[i * 2 + 1] = s[1] + k * (s[2] + s[4]);
            }
            break;
        case 6:
            for (unsigned int i = 0; i < samples; ++i) {
                const float* s = in + i * 6;
                out[i * 2] = s[0] + k * (s[2] + s[4] + s[3]);
                out[i * 2 + 1] = s[1] + k * (s[2] + s[5] + s[3]);
            }
            break;
        case 8:
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
        if (m_monitorSystem) {
            pushMonitor(m_pcm.data() + off, samples);
        }
        for (size_t i = 0; i < static_cast<size_t>(samples) * 2; i++) {
            const double v = static_cast<double>(data[i]);
            m_rmsSum += v * v;
            m_rmsWinSum += v * v;
        }
    } else {
        m_pcm.insert(m_pcm.end(), data,
                     data + static_cast<size_t>(samples) * 2);
        if (m_monitorSystem) {
            pushMonitor(data, samples);
        }
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


void AudioCapture::setMonitorEnabled(bool on) {
    if (m_monitorEnabled == on) {
        return;
    }
    m_monitorEnabled = on;
    if (!m_active) {
        return;
    }
    if (on) {
        if (!initMonitor()) {
            skkbot2::log::warn(
                "[RENDER] monitor: FMOD system unavailable - live monitor off");
            m_monitorEnabled = false;
        }
    } else {
        shutdownMonitor();
    }
}

bool AudioCapture::initMonitor() {
    if (m_monitorSystem) {
        return true;
    }
    if (FMOD::System_Create(&m_monitorSystem) != FMOD_OK || !m_monitorSystem) {
        m_monitorSystem = nullptr;
        return false;
    }
    const int rate = m_sampleRate > 0 ? m_sampleRate : 44100;
    if (m_monitorSystem->setSoftwareFormat(rate, FMOD_SPEAKERMODE_DEFAULT, 0) !=
            FMOD_OK ||
        m_monitorSystem->init(32, FMOD_INIT_NORMAL, nullptr) != FMOD_OK) {
        skkbot2::log::warn("[RENDER] monitor: system init failed");
        m_monitorSystem->release();
        m_monitorSystem = nullptr;
        return false;
    }
    m_monitorRing.assign(
        static_cast<size_t>(kMonitorRingFrames) * 2, 0.0f);
    m_monWrite.store(0, std::memory_order_relaxed);
    m_monRead.store(0, std::memory_order_relaxed);

    FMOD_CREATESOUNDEXINFO info = {};
    info.cbsize = sizeof(info);
    info.defaultfrequency = rate;
    info.numchannels = 2;
    info.numsubsounds = 0;
    info.format = FMOD_SOUND_FORMAT_PCMFLOAT;
    info.decodebuffersize = 1024;
    info.length = static_cast<unsigned>(rate) * 2 * sizeof(float);
    info.pcmreadcallback = &AudioCapture::monitorRead;
    FMOD_RESULT res = m_monitorSystem->createSound(
        nullptr, FMOD_OPENUSER | FMOD_CREATESTREAM | FMOD_LOOP_NORMAL, &info,
        &m_monitorSound);
    if (res != FMOD_OK || !m_monitorSound) {
        skkbot2::log::warn(
            "[RENDER] monitor: createSound failed ({}) - live monitor off",
            static_cast<int>(res));
        m_monitorSound = nullptr;
        m_monitorSystem->release();
        m_monitorSystem = nullptr;
        return false;
    }
    res = m_monitorSystem->playSound(m_monitorSound, nullptr, false,
                                     &m_monitorChannel);
    if (res != FMOD_OK || !m_monitorChannel) {
        skkbot2::log::warn(
            "[RENDER] monitor: playSound failed ({}) - live monitor off",
            static_cast<int>(res));
        m_monitorChannel = nullptr;
        m_monitorSound->release();
        m_monitorSound = nullptr;
        m_monitorSystem->release();
        m_monitorSystem = nullptr;
        return false;
    }
    m_monitorChannel->setVolume(m_monitorGain);
    m_monitorLogged = false;
    skkbot2::log::info(
        "[RENDER] monitor attached: {} Hz stereo, gain {:.2f} "
        "(live playback of the render mix)",
        rate, m_monitorGain);
    return true;
}

void AudioCapture::shutdownMonitor() {
    m_monitorChannel = nullptr;
    if (m_monitorSound) {
        m_monitorSound->release();
        m_monitorSound = nullptr;
    }
    if (m_monitorSystem) {
        m_monitorSystem->close();
        m_monitorSystem->release();
        m_monitorSystem = nullptr;
    }
    m_monitorRing.clear();
    m_monitorRing.shrink_to_fit();
}

void AudioCapture::pushMonitor(const float* stereo, unsigned int frames) {
    const uint64_t w = m_monWrite.load(std::memory_order_relaxed);
    const uint64_t r = m_monRead.load(std::memory_order_acquire);
    if (w - r + frames > kMonitorRingFrames) {
        m_monRead.store(w + frames - kMonitorRingFrames,
                        std::memory_order_release);
        if (!m_monitorLogged) {
            m_monitorLogged = true;
            skkbot2::log::info(
                "[RENDER] monitor: device slower than the mix - tail "
                "dropped to keep the ring live");
        }
    }
    for (unsigned int i = 0; i < frames; ++i) {
        const uint64_t idx = (w + i) & (kMonitorRingFrames - 1);
        m_monitorRing[static_cast<size_t>(idx) * 2] = stereo[i * 2];
        m_monitorRing[static_cast<size_t>(idx) * 2 + 1] = stereo[i * 2 + 1];
    }
    m_monWrite.store(w + frames, std::memory_order_release);
}

FMOD_RESULT F_CALLBACK AudioCapture::monitorRead(FMOD_SOUND*, void* data,
                                                 unsigned int datalen) {
    auto& ac = AudioCapture::get();
    if (!data || !ac.m_monitorSystem) {
        return FMOD_OK;
    }
    auto* out = static_cast<float*>(data);
    const size_t frames = datalen / (2 * sizeof(float));
    uint64_t r = ac.m_monRead.load(std::memory_order_relaxed);
    const uint64_t w = ac.m_monWrite.load(std::memory_order_acquire);
    for (size_t i = 0; i < frames; ++i) {
        if (r < w) {
            const uint64_t idx = r & (kMonitorRingFrames - 1);
            out[i * 2] = ac.m_monitorRing[static_cast<size_t>(idx) * 2];
            out[i * 2 + 1] = ac.m_monitorRing[static_cast<size_t>(idx) * 2 + 1];
            ++r;
        } else {
            out[i * 2] = 0.0f;
            out[i * 2 + 1] = 0.0f;
        }
    }
    ac.m_monRead.store(r, std::memory_order_release);
    return FMOD_OK;
}

}
}
