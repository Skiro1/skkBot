#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include <Geode/fmod/fmod.hpp>

namespace skkbot2 {
namespace render {

void enableFmodUpdateHook(bool enable);

// Realtime audio capture for the renderer (Silicate-style).
//
// A DSP plugin is inserted at the END of FMOD's master channel group, so it
// sees the FULL mix: level music, NONG, all SFX, practice music. During a
// render the FMOD output is switched to FMOD_OUTPUTTYPE_NOSOUND_NRT (offline
// mixer, no device, no sound) and the engine is advanced manually:
// AudioCapture::pump(levelTime) calls FMOD::System::update() in 1024-sample
// steps until the mixer has produced as many samples as the level's game
// time demands. The game time drives the pump (NOT wall time), so the audio
// is generated at the level's own speed вЂ” the final video plays it back at
// exactly 1x, perfectly synced to the video pts, with no time-stretch.
//
// While the render is active the game's own FMODAudioEngine::update is
// blocked by a hook (see hooks.cpp) so the mixer can only advance through
// pump() вЂ” that is what lets the offline mixer run faster than real time.
//
// Threading: everything runs on the MAIN thread (pump is called from
// drive(), the DSP read callback fires inside system->update()), so no
// locking is needed between the DSP callback and the consumer.
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

    // One-time: create the DSP plugin (FMOD system must exist). Returns
    // false if FMOD is not available (audio capture is then simply off).
    bool init();

    // Activates capture: adds the DSP to the master group, switches the
    // output to the offline NRT mixer and reads the software format.
    // Attaches the capture DSP to the FMOD master group and switches the
    // mixer to the offline NOSOUND_NRT output. For the duration of the render
    // the engine's Music/SFX volumes are forced to musicVolume/sfxVolume
    // (silicate parity; the previous values are restored in detach()).
    bool attach(float musicVolume = 1.0f, float sfxVolume = 1.0f);
    // Restores the previous output and removes the DSP.
    void detach();

    bool active() const { return m_active; }
    int sampleRate() const { return m_sampleRate; }
    int channels() const { return m_channels; }

    // v58.22 silicate parity: the FMOD update gate. While a render is
    // active the game's FMODAudioEngine::update and every FMOD::System::update
    // call is frozen (see hooks.cpp). The only way the offline mixer may
    // advance is inside pump()'s update window: m_shouldUpdateFmod is opened,
    // engine->update(dt) runs, and the FMOD::System::update hook advances the
    // mixer exactly as much as (m_fmodTime - m_time) demands вЂ” identical to
    // Silicate's AudioRecorder (m_fmodTime += dt in the engine update hook,
    // requiredDt = m_fmodTime - m_time in the System::update hook).
    bool shouldUpdateFmod() const { return m_shouldUpdateFmod; }
    double fmodTime() const { return m_fmodTime; }
    double time() const { return m_time; }
    void addFmodTime(double dt) { m_fmodTime += dt; }
    // Diagnostics: how many mixer samples the System::update hook produced.
    void onPumped(int samples) { m_pumped += samples; }

    // Advances the offline mixer until it has produced levelTime worth of
    // samples (game seconds). Call once per drive() вЂ” the loop only runs
    // when the level time has advanced past the pumped samples, so it
    // naturally stalls while the sim is held (encoder backpressure, pause).
    //
    // The FIRST call also anchors the audio timeline: everything the mixer
    // produces up to that `levelTime` is the pre-roll and is DROPPED, because
    // video pts 0 is that same moment. A render started mid-attempt (the
    // normal flow: the macro is already playing when Start Render is pressed)
    // would otherwise encode the track from 0 while the video starts at
    // levelTime L0, and the whole video would be out of sync by L0 seconds.
    void pump(double levelTime);

    // Pins the audio timeline to the video timeline. `levelTime` is the level
    // time of video pts 0 and `trackPositionSeconds` is where the level song
    // actually is inside its own track at that moment (begin() restarts it
    // from 0, so normally 0). Everything the mixer produces up to
    // `levelTime - trackPositionSeconds` is pre-roll and gets dropped, so the
    // first encoded block (pts 0) carries the track at the position the
    // footage shows. Without it a render started mid-attempt plays the track
    // from 0 over footage from L0 and the whole video is late by L0.
    void anchorAt(double levelTime, double trackPositionSeconds);

    // Seconds of audio the encoder has written (pts of the next frame).
    double producedSeconds() const {
        return m_sampleRate > 0
                   ? static_cast<double>(m_pumpedConsumed) /
                         static_cast<double>(m_sampleRate)
                   : 0.0;
    }
    // Seconds the offline mixer has actually mixed since attach() (master
    // group DSP clock). Equals producedSeconds() + the dropped pre-roll once
    // the pump is caught up; a difference means the mixer is behind or ahead.
    double mixerSeconds();
    // Pre-roll still to be dropped (0 once the anchor is reached).
    double prerollRemainSeconds() const {
        if (m_prerollEnd <= 0.0 || m_sampleRate <= 0) {
            return 0.0;
        }
        const double dropped =
            static_cast<double>(m_discardedSamples) /
            static_cast<double>(m_sampleRate);
        return std::max(0.0, m_prerollEnd - dropped);
    }

    // Encodes as many complete codec frames as possible from the captured
    // PCM. sink(block, ptsSamples) is called once per codec frame with an
    // interleaved float block of frameSize*channels samples; ptsSamples is
    // the sample counter the block STARTS at (monotonic). Returns the number
    // of samples (per channel) consumed.
    size_t drain(std::vector<float>& accum, int frameSize,
                 const std::function<void(const float* block, int64_t pts)>&
                     sink);

   private:
    static FMOD_RESULT F_CALLBACK
    dspRead(FMOD_DSP_STATE* state, float* inBuffer, float* outBuffer,
            unsigned int length, int inChannels, int* outChannels);

    void onData(const float* data, unsigned int samples, int channels);

    FMOD::DSP* m_dsp = nullptr;
    FMOD::ChannelGroup* m_master = nullptr;
    bool m_active = false;

    int m_sampleRate = 0;
    int m_channels = 2;
    unsigned int m_dspBufferLen = 1024;
    int m_dspBufferCount = 2;

    // Samples (per channel) the mixer has produced since attach().
    int64_t m_pumped = 0;
    // Samples (per channel) already handed to the encoder вЂ” the pts of the
    // NEXT codec frame.
    int64_t m_pumpedConsumed = 0;
    // Captured interleaved STEREO float PCM, appended by the DSP callback.
    // When the mixer runs >2ch (e.g. a 7.1 device config), onData downmixes
    // to stereo before appending, so the encoder path always sees 2ch.
    std::vector<float> m_pcm;

    // Pre-render music/SFX volumes, restored in detach() (silicate parity:
    // the render forces both to 100% so the offline mixer definitely
    // produces sound regardless of the user's in-game volume sliders).
    float m_prevMusicVolume = -1.0f;
    float m_prevSfxVolume = -1.0f;

    // RMS diagnostics: squared-sum accumulator + sample counter, logged every
    // ~0.5s of audio so a silent capture is visible in the log.
    double m_rmsSum = 0.0;
    int64_t m_rmsSamples = 0;
    double m_rmsWinSum = 0.0;
    int64_t m_rmsWinSamples = 0;
    int64_t m_rmsLastLog = 0;

    // Silicate-parity FMOD gating (see pump/update-hook comments above).
    // m_shouldUpdateFmod is the update window; m_fmodTime accumulates the
    // level-simulation dt inside the window; m_time mirrors the samples
    // already consumed by the encoder (m_pumpedConsumed / m_sampleRate) so
    // the mixer can never run ahead of the written audio.
    bool m_shouldUpdateFmod = false;
    double m_fmodTime = 0.0;
    double m_time = 0.0;
    double m_lastLevelTime = 0.0;

    // Timeline anchor (see pump()): the first pump records the level time the
    // render actually starts at; everything mixed before that is dropped so
    // the music starts at the same position the footage does.
    bool m_anchored = false;
    double m_prerollEnd = 0.0;
    // Samples (per channel) dropped by the pre-roll. They are NOT part of the
    // written track (pts stays at 0 for the first encoded block) but they DID
    // advance the mixer, so backpressure has to count them in m_time.
    int64_t m_discardedSamples = 0;
    // Master-group DSP clock at attach: the mixer's own sample counter.
    unsigned long long m_masterClockBase = 0;
};

}  // namespace render
}  // namespace skkbot2
