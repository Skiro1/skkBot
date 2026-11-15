#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace skkbot2 {
namespace render {

struct GrabResult {
    const uint8_t* data = nullptr;
    int slot = -1;
    bool preCopied = false;
    int64_t pts = -1;
};


class FboCapture {
   public:
    FboCapture() = default;
    ~FboCapture() { destroy(); }

    bool init(unsigned width, unsigned height);
    void destroy();

    bool grab(int64_t pts, GrabResult* out);

    void releaseSlot(int slot);

    bool hasFreeSlot() const;

    void releaseSync();

    void setFade(float fade) { m_fade = fade; }
    float fade() const { return m_fade; }

    void displayPreview(float winW, float winH);

    unsigned sceneTexture() const { return m_sceneTex; }

    unsigned m_width = 0;
    unsigned m_height = 0;
    size_t m_nv12Size = 0;

   private:
    bool initShaders();
    void issueReadback(int64_t pts);
    void syncReadback(uint8_t* dst);

    void unmapReturnedSlots();

    bool pickIssueSlot(int* slot) const;

    bool pickHarvestSlot(int* slot) const;

    unsigned m_sceneFbo = 0;
    unsigned m_sceneTex = 0;
    unsigned m_yFbo = 0;
    unsigned m_yTex = 0;
    unsigned m_uvFbo = 0;
    unsigned m_uvTex = 0;
    unsigned m_vao = 0;
    unsigned m_vbo = 0;
    unsigned m_progY = 0;
    unsigned m_progUV = 0;
    int m_uTexY = -1;
    int m_uTexUV = -1;
    int m_uFadeY = -1;
    int m_uFadeUV = -1;
    float m_fade = 1.0f;

    static constexpr int PBO_COUNT = 16;
    unsigned m_pbo[PBO_COUNT] = {};
    uintptr_t m_fence[PBO_COUNT] = {};
    int64_t m_pts[PBO_COUNT] = {};
    unsigned m_issued = 0;

    uint8_t* m_mapped[PBO_COUNT] = {};
    std::atomic<bool> m_encBusy[PBO_COUNT];
    unsigned m_seq[PBO_COUNT] = {};

    std::vector<uint8_t> m_syncBuf;
    std::atomic<bool> m_syncBusy{false};
    unsigned m_syncBusySkips = 0;

    double m_visitMs = 0.0;
    unsigned m_visitN = 0;

   public:
    double visitMs() const { return m_visitMs; }
    unsigned visitN() const { return m_visitN; }
};

}
}
