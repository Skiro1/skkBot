#pragma once

#include <cstdint>

namespace skkbot2 {
namespace render {

// FboCapture::grab(dst, pts, outPts): pts is the pts of the CURRENT scene
// (issued for readback right after the harvest); outPts is filled with the
// pts of the frame actually returned (the frame harvested from the PBO ring,
// which was issued two grabs ago) so the encode thread always pairs the
// correct timestamp with the pixels (Silicat's m_ptsQueue-FIFO behavior).

// Captures the running cocos2d scene into NV12 directly on the GPU.
//
// v6 (Silicate-style scene re-render + GPU colorspace + async PBO readback):
// the scene is re-rendered into our own FBO at the OUTPUT resolution via
// visit() (the cocos view was silently resized to the output size for the
// duration of the render, so the projection matches exactly and the result is
// independent of the visible window/zoom), then two fullscreen shader passes
// convert it to NV12 (Y plane full-res, UV plane half-res) and glReadPixels
// copies the planes into a triple-buffered PBO. No sws_scale on the CPU, no
// 8 MB RGBA readback вЂ” the encode thread gets a compact NV12 frame
// (w*h*3/2 bytes) that NVENC consumes natively.
//
// The readback runs one grab() behind the issued frame (triple-buffered), so
// grab() returns data captured 2 calls ago вЂ” completely fine for video.
class FboCapture {
   public:
    FboCapture() = default;
    ~FboCapture() { destroy(); }

    bool init(unsigned width, unsigned height);
    void destroy();

    // Sync readback of a scene frame captured 2 grabs ago, written as NV12
    // into dst (must hold width*height*3/2 bytes). Also issues the readback
    // of the CURRENT scene state (labeled pts). Returns false when the
    // pipeline is not yet warmed up (first two calls) or not initialized.
    // On success *outPts is the pts of the frame actually written to dst вЂ”
    // the pts recorded at the harvested slot's issue, NOT the pts argument
    // (the returned pixels are two requests old).
    bool grab(uint8_t* dst, int64_t pts, int64_t* outPts = nullptr);

    // Fade-in from black: multiplies the scene RGB by fade (0..1) inside the
    // Y/UV shader passes. Set per-frame by the Renderer (video pts based);
    // resets to 1.0 in init().
    void setFade(float fade) { m_fade = fade; }
    float fade() const { return m_fade; }

    // Silicate-style live preview: blits the last scene captured into
    // m_sceneFbo letterboxed into whatever framebuffer is currently bound for
    // drawing (the default framebuffer in the drawScene hook). Used to show
    // the video preview in the window while the level itself is not drawn.
    // winW/winH = REAL window size (window.cpp values); the cocos viewport
    // lies here during a render because silentChangeSize shrank it to the
    // output resolution. Safe before the first grab: cleared to black in init().
    void displayPreview(float winW, float winH);

    // GL texture id of the last scene rendered into the output FBO (RGBA8,
    // output-sized). 0 while not initialized. Lets the GUI show a live
    // in-window preview via ImGui::Image without touching the framebuffer.
    unsigned sceneTexture() const { return m_sceneTex; }

    unsigned m_width = 0;
    unsigned m_height = 0;
    size_t m_nv12Size = 0;  // width*height*3/2

   private:
    bool initShaders();
    void issueReadback(int64_t pts);
    void syncReadback(uint8_t* dst);

    // Scene re-render target (output-sized) + Y and UV pass outputs
    unsigned m_sceneFbo = 0;
    unsigned m_sceneTex = 0;
    unsigned m_yFbo = 0;
    unsigned m_yTex = 0;
    unsigned m_uvFbo = 0;
    unsigned m_uvTex = 0;
    // Fullscreen quad
    unsigned m_vao = 0;
    unsigned m_vbo = 0;
    unsigned m_progY = 0;
    unsigned m_progUV = 0;
    int m_uTexY = -1;
    int m_uTexUV = -1;
    int m_uFadeY = -1;
    int m_uFadeUV = -1;
    float m_fade = 1.0f;

    // Triple-buffered async readback (6-deep: more slack for GPU lag under
    // heavy load, so the harvest wait almost never times out)
    static constexpr int PBO_COUNT = 6;
    unsigned m_pbo[PBO_COUNT] = {0, 0, 0};
    uintptr_t m_fence[PBO_COUNT] = {0, 0, 0};
    // pts labeled at each slot's issue, so grab() can pair the harvested
    // pixels with the timestamp they were actually rendered under (Silicat's
    // m_ptsQueue push-time labeling; the sync fallback reads the pass outputs
    // of the last issue and reports THAT issue's pts).
    int64_t m_pts[PBO_COUNT] = {-1, -1, -1, -1, -1, -1};
    unsigned m_issued = 0;
};

}  // namespace render
}  // namespace skkbot2
