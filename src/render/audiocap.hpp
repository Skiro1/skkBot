#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <Geode/fmod/fmod.hpp>

namespace skkbot2 {
namespace render {

void enableFmodUpdateHook(bool enable);

class AudioCapture {
   public:
    static AudioCapture& get() {
        static AudioCapture inst;
        return inst;
    }

    AudioCapture() = default;
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool init();

    bool attach(float musicVolume = 1.0f, float sfxVolume = 1.0f);
    void detach();

    void setMonitorEnabled(bool on);
    bool monitorEnabled() const { return m_monitorEnabled; }
    void setMonitorGain(float gain) { m_monitorGain = gain; }
    float monitorGain() const { return m_monitorGain; }

    bool active() const { return m_active; }
    int sampleRate() const { return m_sampleRate; }
    int channels() const { return m_channels; }

    bool shouldUpdateFmod() const { return m_shouldUpdateFmod; }
    double fmodTime() const { return m_fmodTime; }
    double time() const { return m_time.load(std::memory_order_acquire); }
    void addFmodTime(double dt) { m_fmodTime += dt; }
    void onPumped(int samples) { m_pumped += samples; }

    void pump(double levelTime);

    void anchorAt(double levelTime, double trackPositionSeconds);

    void seedLastLevelTime(double t) { m_lastLevelTime = t; }

    double producedSeconds() const {
        const int64_t consumed = m_pumpedConsumed.load(std::memory_order_relaxed);
        return m_sampleRate > 0
                   ? static_cast<double>(consumed) /
                         static_cast<double>(m_sampleRate)
                   : 0.0;
    }
    double mixerSeconds();
    double prerollRemainSeconds() const {
        if (m_prerollEnd <= 0.0 || m_sampleRate <= 0) {
            return 0.0;
        }
        const double dropped =
            static_cast<double>(m_discardedSamples.load(std::memory_order_relaxed)) /
            static_cast<double>(m_sampleRate);
        return std::max(0.0, m_prerollEnd - dropped);
    }

    size_t drain(std::vector<float>& accum, int frameSize,
                 const std::function<void(const float* block, int64_t pts)>&
                     sink);

   private:
    static FMOD_RESULT F_CALLBACK
    dspRead(FMOD_DSP_STATE* state, float* inBuffer, float* outBuffer,
            unsigned int length, int inChannels, int* outChannels);

    void onData(const float* data, unsigned int samples, int channels);

    static FMOD_RESULT F_CALLBACK
    monitorRead(FMOD_SOUND* sound, void* data, unsigned int datalen);
    bool initMonitor();
    void shutdownMonitor();
    void pushMonitor(const float* stereo, unsigned int frames);

    FMOD::System* m_monitorSystem = nullptr;
    FMOD::Sound* m_monitorSound = nullptr;
    FMOD::Channel* m_monitorChannel = nullptr;
    bool m_monitorEnabled = false;
    float m_monitorGain = 1.0f;
    std::vector<float> m_monitorRing;
    std::atomic<uint64_t> m_monWrite{0};
    std::atomic<uint64_t> m_monRead{0};
    bool m_monitorLogged = false;

    FMOD::DSP* m_dsp = nullptr;
    FMOD::ChannelGroup* m_master = nullptr;
    bool m_active = false;

    int m_sampleRate = 0;
    int m_channels = 2;
    unsigned int m_dspBufferLen = 1024;
    int m_dspBufferCount = 2;

    int64_t m_pumped = 0;
    std::atomic<int64_t> m_pumpedConsumed{0};
    mutable std::mutex m_pcmMu;
    std::vector<float> m_pcm;

    float m_prevMusicVolume = -1.0f;
    float m_prevSfxVolume = -1.0f;

    double m_rmsSum = 0.0;
    int64_t m_rmsSamples = 0;
    double m_rmsWinSum = 0.0;
    int64_t m_rmsWinSamples = 0;
    int64_t m_rmsLastLog = 0;

    bool m_shouldUpdateFmod = false;
    double m_fmodTime = 0.0;
    std::atomic<double> m_time{0.0};
    double m_lastLevelTime = 0.0;
    bool m_seedWarned = false;

    bool m_anchored = false;
    double m_prerollEnd = 0.0;
    std::atomic<int64_t> m_discardedSamples{0};
    unsigned long long m_masterClockBase = 0;
};

}
}
