#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace skkbot2 {
namespace render {

// What one grab() hands back. It is a POINTER into a mapped PBO slot, not a
// copy: the 24.9 MB memcpy that used to sit on the main thread (25% of a GRIEF
// 4K frame) is now the encoder thread's job to do, and it does exactly the same
// work it already did in encodeOneFrame.
//
// The pointer is only valid while `slot` is held. The consumer must call
// releaseSlot(slot) when it has finished reading, and until then FboCapture
// will not reuse that slot for a new glReadPixels - reusing it underneath a
// live reader is exactly the tearing the old design had already paid for once.
struct GrabResult {
    const uint8_t* data = nullptr;
    int slot = -1;                // PBO slot to hand back, -1 when preCopied
    bool preCopied = false;       // data is a private buffer; do NOT copy again
    int64_t pts = -1;
};

// FboCapture::grab(pts, out): issues the readback of the CURRENT scene labelled
// pts and returns the OLDEST already-issued frame. out->pts is that frame's own
// issue-time pts, so the encoder always pairs the correct timestamp with the
// pixels it is actually given (Silicat's m_ptsQueue push/pop behaviour).
//
// One NV12 copy per frame is gone, not two. The remaining one is the encoder
// thread's `ring -> AVFrame` memcpy, which cannot go away: NVENC reads the
// AVFrame, and handing it our mapped memory is precisely the design that
// already tore frames once (see the mYuvFrames comment in renderer.cpp).

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

    // Harvest the oldest issued-but-not-yet-taken PBO slot, map it, and hand
    // back a POINTER to it. Also issues the readback of the CURRENT scene state
    // (labeled pts). Returns false when the pipeline is not warmed up yet or
    // not initialized. The caller OWNS the returned slot until it calls
    // releaseSlot().
    bool grab(int64_t pts, GrabResult* out);

    // Give the slot back. Called from the encoder thread once it has finished
    // reading the mapped memory. Touches no GL state - only a flag - because it
    // runs off the main thread.
    void releaseSlot(int slot);

    // Backpressure probe, asked BEFORE the simulation is allowed to step. A
    // slot is free if it is neither currently mapped nor carrying a live fence.
    // Only the main thread ever TAKES a slot, and the encoder thread only ever
    // frees one, so a "yes" here cannot go stale before grab() consumes it.
    bool hasFreeSlot() const;

    // The sync-readback fallback copies on the main thread (it has to: the GL
    // context is here). Its buffer therefore needs the same hand-back discipline
    // as a PBO slot, or the next fallback would overwrite a frame the encoder
    // is still reading.
    void releaseSync();

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

    // Unmap every slot the encoder thread has handed back. Main thread only:
    // glUnmapBuffer needs the GL context, which the encoder thread does not
    // have and must never take.
    void unmapReturnedSlots();

    // A slot may be given a new glReadPixels only if it is neither mapped right
    // now nor still carrying the fence of an unfinished readback.
    bool pickIssueSlot(int* slot) const;

    // The oldest slot that has an issued-but-unclaimed frame: lowest issue
    // sequence among slots with a live fence and no mapping yet. "Oldest"
    // rather than "exactly two behind" is what lets the pipeline survive a slow
    // encoder without silently reusing a slot somebody still holds.
    bool pickHarvestSlot(int* slot) const;

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

// Async readback pool: 12 slots, each a full 4K NV12 pair (Y + half-res UV).
    //
    // Was 8, and the reason it was 8 is gone. The old harvest was "exactly two
    // grabs back" by index arithmetic and issueReadback blocked on
    // glClientWaitSync for up to a FULL SECOND when handed a slot whose fence
    // was still live - which is why the renderer capped its batch at
    // PBO_COUNT - 2 and left two slots of slack against that stall. With the
    // pointer handover (380b82e) issueReadback no longer waits at all: it takes
    // a free slot or skips the frame, so there is no one-second stall to guard
    // against. The cap was therefore the binding constraint on render speed,
    // not slack - and the batch is what actually sets the wall clock
    // (drawScene rate x frames per batch), which is why making grab 31%
    // cheaper moved nothing.
    //
    // Depth is now sized to the batch instead: a batch of B issues B readbacks
    // while harvesting B, so the pool must exceed B with room for the one slot
    // the encoder is reading. At 4K, 12 slots is 299 MB - still LESS than the
    // 348 MB this pool plus the old 6-slot ring buffer together used, because
    // the ring was deleted in the same change. Not a tradeoff, a reduction.
    static constexpr int PBO_COUNT = 16;
    unsigned m_pbo[PBO_COUNT] = {};
    uintptr_t m_fence[PBO_COUNT] = {};
    // pts labeled at each slot's issue, so grab() can pair the harvested
    // pixels with the timestamp they were actually rendered under (Silicat's
    // m_ptsQueue push-time labeling; the sync fallback reads the pass outputs
    // of the last issue and reports THAT issue's pts).
    int64_t m_pts[PBO_COUNT] = {};
    unsigned m_issued = 0;

    // --- Pointer handover (the removed main-thread copy) ---
    //
    // m_mapped[s] non-null means slot s is MAPPED and its bytes belong to
    // whoever holds it; m_encBusy[s] means that someone does. The main thread
    // sets both when it hands the slot over and clears them once the encoder
    // says it is done reading. Between those two moments the slot must never be
    // picked as an issue target - that reuse-under-a-live-reader is what would
    // tear a frame, and it would tear it silently.
    uint8_t* m_mapped[PBO_COUNT] = {};
    std::atomic<bool> m_encBusy[PBO_COUNT];
    // Issue sequence per slot, so "oldest claimable" is a total order and not a
    // guess based on index arithmetic.
    unsigned m_seq[PBO_COUNT] = {};

    // Destination of the synchronous fallback, plus the same hand-back flag.
    std::vector<uint8_t> m_syncBuf;
    std::atomic<bool> m_syncBusy{false};
    // Fallbacks the fallback-guard turned away. Should stay at zero in practice;
    // if it climbs, the PBO path is failing and the guard is hiding frames.
    unsigned m_syncBusySkips = 0;

    // CPU cost of the second scene traversal: renderSceneInto() calls
    // scene->visit() on top of the draw the preview already did, and the
    // question of whether that duplicate is worth removing cannot be answered
    // without a number. Accumulated in ms, reset by init().
    double m_visitMs = 0.0;
    unsigned m_visitN = 0;

   public:
    double visitMs() const { return m_visitMs; }
    unsigned visitN() const { return m_visitN; }
};

}  // namespace render
}  // namespace skkbot2
