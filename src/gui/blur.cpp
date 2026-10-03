#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "gui/blur.hpp"
#include <imgui.h>
#include <imgui_internal.h>

#include <Geode/Geode.hpp>
#include <cocos2d.h>

#include <vector>
#include <cstdint>
#include <algorithm>

using namespace cocos2d;

namespace {

struct BlurRegion {
    float x0;
    float y0;
    float x1;
    float y1;
    float rounding;
    float radius;
    float alpha;
};

constexpr size_t kMaxRegions = 16;

std::vector<BlurRegion> g_regions;

GLuint g_texture  = 0;
GLuint g_snapshot = 0;
int    g_snapW    = 0;
int    g_snapH    = 0;
bool   g_failed   = false;

// Device tilt (parallax input), -1..1. Fed from CMMotionManager on Apple
// platforms, stays zero elsewhere (and on desktop).
float g_parX = 0.f;
float g_parY = 0.f;

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>

namespace {
id g_motionManager = nullptr;
bool g_motionInit  = false;

void initParallaxReader() {
    if (g_motionInit) return;
    g_motionInit = true;
    Class cls = objc_getClass("CMMotionManager");
    if (!cls) return;
    id mgr = ((id(*)(Class, SEL))objc_msgSend)(cls, sel_registerName("alloc"));
    if (!mgr) return;
    mgr = ((id(*)(id, SEL))objc_msgSend)(mgr, sel_registerName("init"));
    if (!mgr) return;
    g_motionManager = mgr;
    ((void(*)(id, SEL, double))objc_msgSend)(mgr, sel_registerName("setDeviceMotionUpdateInterval:"), 1.0 / 60.0);
    ((void(*)(id, SEL))objc_msgSend)(mgr, sel_registerName("startDeviceMotionUpdates"));
}

void pollParallax() {
    initParallaxReader();
    g_parX = 0.f;
    g_parY = 0.f;
    if (!g_motionManager) return;
    id motion = ((id(*)(id, SEL))objc_msgSend)(g_motionManager, sel_registerName("deviceMotion"));
    if (!motion) return;
    id attitude = ((id(*)(id, SEL))objc_msgSend)(motion, sel_registerName("attitude"));
    if (!attitude) return;
    double roll  = ((double(*)(id, SEL))objc_msgSend)(attitude, sel_registerName("roll"));
    double pitch = ((double(*)(id, SEL))objc_msgSend)(attitude, sel_registerName("pitch"));
    g_parX = std::clamp((float)(roll / 1.2), -1.0f, 1.0f);
    g_parY = std::clamp((float)(pitch / 1.2), -1.0f, 1.0f);
}
} // anonymous namespace
#else
namespace {
void pollParallax() {
    g_parX = 0.f;
    g_parY = 0.f;
}
} // anonymous namespace
#endif

struct BlurProgram {
    CCGLProgram* program = nullptr;
    GLint locTex         = -1;
    GLint locHalfpixel   = -1;
    GLint locAlpha       = -1;
    GLint locRound       = -1;
    GLint locSize        = -1;
    GLint locMask        = -1;
    GLint locRefract     = -1;
    GLint locRefractBand = -1;
    GLint locNoise       = -1;
    GLint locFresnel     = -1;
    GLint locChroma      = -1;
    GLint locSpecular    = -1;
    GLint locLens        = -1;
    GLint locParallax    = -1;
    GLint locGloss       = -1;
    GLint locLiquid      = -1;
    GLint locSheen       = -1;
    GLint locTime        = -1;
};

BlurProgram g_down;
BlurProgram g_up;

const char* kVertexShader =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texCoord;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_uv = a_texCoord;\n"
    "}\n";

const char* kDownsampleShader =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_halfpixel;\n"
    "void main() {\n"
    "    vec4 sum = texture2D(u_tex, v_uv) * 4.0;\n"
    "    sum += texture2D(u_tex, v_uv - u_halfpixel);\n"
    "    sum += texture2D(u_tex, v_uv + u_halfpixel);\n"
    "    sum += texture2D(u_tex, v_uv + vec2(u_halfpixel.x, -u_halfpixel.y));\n"
    "    sum += texture2D(u_tex, v_uv - vec2(u_halfpixel.x, -u_halfpixel.y));\n"
    "    gl_FragColor = sum / 8.0;\n"
    "}\n";

const char* kUpsampleShader =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_halfpixel;\n"
    "uniform float u_alpha;\n"
    "uniform float u_round;\n"
    "uniform vec2 u_size;\n"
    "uniform float u_mask;\n"
    "uniform float u_refract;\n"
    "uniform float u_refractBand;\n"
    "uniform float u_noise;\n"
    "uniform float u_fresnel;\n"
    "uniform float u_chroma;\n"
    "uniform float u_specular;\n"
    "uniform float u_lens;\n"
    "uniform vec2 u_parallax;\n"
    "uniform float u_gloss;\n"
    "uniform float u_liquid;\n"
    "uniform float u_sheen;\n"
    "uniform float u_time;\n"
    "float sdfDist(vec2 uv) {\n"
    "    vec2 halfSize = u_size * 0.5;\n"
    "    vec2 p = abs((uv - vec2(0.5)) * u_size) - (halfSize - vec2(u_round));\n"
    "    return length(max(p, vec2(0.0))) + min(max(p.x, p.y), 0.0) - u_round;\n"
    "}\n"
    "float randNoise(vec2 co) {\n"
    "    return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "}\n"
    "float hash21(vec2 p) {\n"
    "    p = fract(p * vec2(234.34, 435.345));\n"
    "    p += dot(p, p + 34.23);\n"
    "    return fract(p.x * p.y);\n"
    "}\n"
    "float vnoise(vec2 p) {\n"
    "    vec2 i = floor(p);\n"
    "    vec2 f = fract(p);\n"
    "    vec2 u = f * f * (3.0 - 2.0 * f);\n"
    "    float a = hash21(i);\n"
    "    float b = hash21(i + vec2(1.0, 0.0));\n"
    "    float c = hash21(i + vec2(0.0, 1.0));\n"
    "    float d = hash21(i + vec2(1.0, 1.0));\n"
    "    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);\n"
    "}\n"
    //  Liquid field: low-frequency billowed noise (feTurbulence fractalNoise,
    //  baseFrequency 0.01 -> a couple of large slow-cresting blobs over the
    //  panel), two octaves so the crests carry a finer ripple. This single
    //  field drives BOTH the wave displacement of the sampled backdrop and a
    //  top-left-light specular sheen (feDisplacementMap + feSpecularLighting).
    "float liquidField(vec2 uv, float t) {\n"
    "    vec2 p = uv * 2.2 + vec2(t * 0.040, t * 0.023);\n"
    "    float n = vnoise(p);\n"
    "    n += 0.5 * vnoise(p * 2.3 + 31.7);\n"
    "    n += 0.25 * vnoise(p * 5.1 + 91.3);\n"
    "    n /= 1.75;\n"
    "    return n * n * (3.0 - 2.0 * n);\n"
    "}\n"
    "void main() {\n"
    "    vec2 sampleUV = v_uv;\n"
    "    float mask = 1.0;\n"
    "    float edge = 0.0;\n"
    "    vec2 inwardDir = vec2(0.0);\n"
    //  Liquid field (feTurbulence): two independently-seeded low-freq noise
    //  values drive the X and Y wave displacement (feDisplacementMap uses the
    //  R/G channels), a third pair of samples feed the specular sheen gradient.
    "    float liqX = 0.5;\n"
    "    float liqY = 0.5;\n"
    "    float liqDx = 0.5;\n"
    "    float liqDy = 0.5;\n"
    "    vec2 liqPhase = vec2(0.37, 0.11);\n"
    "    if (u_mask > 0.5) {\n"
    "        liqX = liquidField(v_uv + liqPhase, u_time);\n"
    "        liqY = liquidField(v_uv + vec2(0.83, 0.47), u_time);\n"
    "        liqDx = liquidField(v_uv + liqPhase + vec2(0.020, 0.0), u_time);\n"
    "        liqDy = liquidField(v_uv + liqPhase + vec2(0.0, 0.020), u_time);\n"
    "    }\n"
    "    if (u_mask > 0.5) {\n"
    "        float d = sdfDist(v_uv);\n"
    "        mask = 1.0 - smoothstep(-1.0, 1.0, d);\n"
    //  Edge-focused glass (hyprglass / iOS): refraction lives in a thin band
    //  right at the rim with a Gaussian falloff — Snell's law bends most at the
    //  glancing angles (the exact boundary) and the bend dies out quickly toward
    //  the panel centre, so the middle of the glass stays flat and clean.
    //  The dome lens is only a faint interior magnifier, also fading at the rim.
    "        float bezel = max(u_refractBand, 1.0);\n"
    "        edge = exp(-pow(abs(d) / bezel, 2.0));\n"
    //  Fresnel: reflection grows toward the glancing rim (Schlick, glass/air
    //  f0 ~ 0.04), so transmission collapses at the very edge — the Snell
    //  shimmer must die there too, peaking instead just inside the rim.
    "        float fresnelR = 0.04 + 0.96 * pow(edge, 5.0);\n"
    "        float transmit = 1.0 - fresnelR;\n"
    "        vec2 toCentrePx = (vec2(0.5) - v_uv) * u_size;\n"
    "        inwardDir = length(toCentrePx) > 0.1 ? toCentrePx / length(toCentrePx) : vec2(0.0);\n"
    "        float refractionPx = u_refract * 150.0 * transmit;\n"
    "        vec2 baseOffset = inwardDir * edge * refractionPx / u_size;\n"
    "        vec2 domeUV = vec2(0.0);\n"
    "        if (u_lens > 0.001) {\n"
    "            vec2 c = (v_uv - vec2(0.5)) * 2.0;\n"
    "            vec2 dGrad = vec2(-4.0 * c.x * (1.0 - c.y * c.y),\n"
    "                             -4.0 * c.y * (1.0 - c.x * c.x));\n"
    "            float lensPx = u_lens * min(u_size.x, u_size.y) * 0.010;\n"
    "            domeUV = dGrad * lensPx * (1.0 - edge) / u_size;\n"
    "        }\n"
    "        sampleUV = v_uv + baseOffset + domeUV;\n"
    "        sampleUV += u_parallax * (2.0 + 22.0 * edge) / u_size;\n"
    //  Liquid wave displacement (feDisplacementMap scale 150): the backdrop is
    //  pushed along the noise crests — subtle in the clean centre, full ripple
    //  near the rim (where the glass is thickest, like a lens slab).
    "        vec2 liqWave = (vec2(liqX, liqY) - 0.5) * mix(0.30, 1.0, edge);\n"
    "        sampleUV += liqWave * u_liquid / u_size;\n"
    "    }\n"
    "    vec4 sum = texture2D(u_tex, sampleUV + vec2(-u_halfpixel.x * 2.0, 0.0));\n"
    "    sum += texture2D(u_tex, sampleUV + vec2(-u_halfpixel.x, u_halfpixel.y)) * 2.0;\n"
    "    sum += texture2D(u_tex, sampleUV + vec2(0.0, u_halfpixel.y * 2.0));\n"
    "    sum += texture2D(u_tex, sampleUV + vec2(u_halfpixel.x, u_halfpixel.y)) * 2.0;\n"
    "    sum += texture2D(u_tex, sampleUV + vec2(u_halfpixel.x * 2.0, 0.0));\n"
    "    sum += texture2D(u_tex, sampleUV + vec2(u_halfpixel.x, -u_halfpixel.y)) * 2.0;\n"
    "    sum += texture2D(u_tex, sampleUV + vec2(0.0, -u_halfpixel.y * 2.0));\n"
    "    sum += texture2D(u_tex, sampleUV + vec2(-u_halfpixel.x, -u_halfpixel.y)) * 2.0;\n"
    "    vec3 col = (sum / 12.0).rgb;\n"
    "    if (u_mask > 0.5) {\n"
    //  Colour fidelity: the glass body must read as the same backdrop, only
    //  slightly altered — a "psychophysical" check of Цветопередача. The HDR
    //  recolor is therefore scaled by `edge`: an honest, near-identical centre
    //  and a ramp up to the strong tint only at the rim, where the refraction/
    //  Fresnel/chroma make the edge itself a "glass object".
    "        float hdrBoost = mix(0.35, 1.0, edge);\n"
    "        col = pow(max(col, vec3(0.0)), vec3(1.0 - 0.10 * hdrBoost));\n"
    "        float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));\n"
    "        col = mix(vec3(luma), col, vec3(1.0 + 0.35 * hdrBoost));\n"
    "        col = clamp(col * (1.0 + 0.22 * hdrBoost) + vec3(0.03 * hdrBoost), 0.0, 1.0);\n"
    //  Chromatic aberration at the rim: red and blue refract on a slightly wider
    //  path than green, mimicking dispersion of the curved glass edge.
    "        if (u_chroma > 0.001) {\n"
    "            vec2 chroma = inwardDir * edge * u_chroma * 10.0 / u_size;\n"
    "            float cw = u_chroma * edge;\n"
    "            col.r = mix(col.r, texture2D(u_tex, sampleUV + chroma).r, cw);\n"
    "            col.b = mix(col.b, texture2D(u_tex, sampleUV - chroma).b, cw);\n"
    "        }\n"
    //  Fresnel reflection (Отражение): reflectance R = f0 + (1-f0)·(1-cosθ)^5
    //  climbs toward glancing angles, so the rim stops transmitting and reads
    //  as a soft neutral mirror streak instead of a coloured glow.
    "        float reflectR = 0.04 + 0.96 * pow(edge, 5.0);\n"
    "        float mirror = reflectR * u_fresnel * 0.22;\n"
    "        vec3 reflectCol = vec3(dot(col, vec3(0.2126, 0.7152, 0.0722))) * 0.5 + vec3(0.10);\n"
    "        col = mix(col, reflectCol, mirror);\n"
    //  Tyndall effect (Эффект Тиндаля): Rayleigh volume-scattering of the
    //  colloid body — milky frosted haze deepest in the middle of the glass,
    //  slightly cool because short wavelengths scatter hardest. Rim stays clear.
    "        float frost = u_gloss * (1.0 - edge) * 0.08;\n"
    "        col = mix(col, vec3(0.90, 0.92, 0.97), frost);\n"
    //  Refractometry + spectroscopy (Beer–Lambert): n = n0 + a·C, but the
    //  absorption I = I0·exp(−ε·C·l) grows with the chord length, so the rim
    //  of the slab is a little deeper and tinted.
    "        col *= exp(-0.035 * (1.0 + edge * 1.4));\n"
//  Pseudo-random dithering: breaks up banding on the HDR gradients.
    "        col += (randNoise(gl_FragCoord.xy * 1e-3) - 0.5) * u_noise;\n"
    //  Liquid specular sheen (feSpecularLighting): a soft white glint that
    //  slides over the crests of the liquid field, lit from above-left like the
    //  macOS point light (x=-200, y=-200, z=300). Applies to the whole pane,
    //  slightly stronger at the rim where the surface curves.
    "        vec3 liqN = normalize(vec3(-(liqDx - liqX) * 14.0, -(liqDy - liqY) * 14.0, 1.0));\n"
    "        vec3 liqL = normalize(vec3(-0.35, -0.35, 0.87));\n"
    "        vec3 liqH = normalize(liqL + vec3(0.0, 0.0, 1.0));\n"
    "        float liqSpec = pow(max(dot(liqN, liqH), 0.0), 48.0);\n"
    "        col += vec3(1.0) * liqSpec * u_sheen * mix(0.55, 1.0, edge);\n"
    "        col *= 1.0 - (pow(v_uv.y, 2.0) * edge * edge * 0.09);\n"
    "        col = clamp(col, 0.0, 1.0);\n"
    "    }\n"
    "    gl_FragColor = vec4(col, u_alpha * mask);\n"
    "}\n";

bool compileProgram(BlurProgram& out, const char* fragSrc, bool isUpsample) {
    CCGLProgram* program = new CCGLProgram();

    if (!program->initWithVertexShaderByteArray(kVertexShader, fragSrc)) {
        delete program;
        return false;
    }

    program->addAttribute("a_position", kCCVertexAttrib_Position);
    program->addAttribute("a_texCoord", kCCVertexAttrib_TexCoords);
    program->link();
    program->updateUniforms();

    const GLuint handle = program->getProgram();

    out.program      = program;
    out.locTex       = glGetUniformLocation(handle, "u_tex");
    out.locHalfpixel = glGetUniformLocation(handle, "u_halfpixel");

    if (isUpsample) {
        out.locAlpha       = glGetUniformLocation(handle, "u_alpha");
        out.locRound       = glGetUniformLocation(handle, "u_round");
        out.locSize        = glGetUniformLocation(handle, "u_size");
        out.locMask        = glGetUniformLocation(handle, "u_mask");
        out.locRefract     = glGetUniformLocation(handle, "u_refract");
        out.locRefractBand = glGetUniformLocation(handle, "u_refractBand");
        out.locNoise       = glGetUniformLocation(handle, "u_noise");
        out.locFresnel     = glGetUniformLocation(handle, "u_fresnel");
        out.locChroma      = glGetUniformLocation(handle, "u_chroma");
        out.locSpecular    = glGetUniformLocation(handle, "u_specular");
        out.locLens        = glGetUniformLocation(handle, "u_lens");
        out.locParallax    = glGetUniformLocation(handle, "u_parallax");
        out.locGloss       = glGetUniformLocation(handle, "u_gloss");
        out.locLiquid      = glGetUniformLocation(handle, "u_liquid");
        out.locSheen       = glGetUniformLocation(handle, "u_sheen");
        out.locTime        = glGetUniformLocation(handle, "u_time");
    }

    return true;
}

bool ensurePrograms() {
    if (g_failed) return false;
    if (g_down.program && g_up.program) return true;

    if (!compileProgram(g_down, kDownsampleShader, false) ||
        !compileProgram(g_up, kUpsampleShader, true)) {
        g_failed = true;
        geode::log::warn("skkBot2 blur: shader compilation failed, blur disabled");
        return false;
    }

    return true;
}

bool ensureTexture() {
    if (g_texture && g_snapshot) return true;

    if (!g_texture) {
        glGenTextures(1, &g_texture);
        if (!g_texture) return false;
        glBindTexture(GL_TEXTURE_2D, g_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!g_snapshot) {
        glGenTextures(1, &g_snapshot);
        if (!g_snapshot) return false;
        glBindTexture(GL_TEXTURE_2D, g_snapshot);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

// Grab the whole freshly-rendered game frame into g_snapshot. Called from
// newFrame(), i.e. at the START of the imgui draw hook: the game has finished
// rendering this frame into the backbuffer, but NO imgui geometry has been
// drawn yet, so the snapshot is pristine game content — it can never contain
// our own window, even while it is being dragged.
void captureSnapshot() {
    if (!g_snapshot) return;

    GLint viewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int fbW = viewport[2];
    const int fbH = viewport[3];
    if (fbW <= 0 || fbH <= 0) return;

    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    glBindTexture(GL_TEXTURE_2D, g_snapshot);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, fbW, fbH, 0);
    g_snapW = fbW;
    g_snapH = fbH;

    glBindTexture(GL_TEXTURE_2D, (GLuint)previousTexture);
}

void renderRegion(const ImDrawList*, const ImDrawCmd* cmd) {
    if (!cmd) return;

    const size_t index = (size_t)(intptr_t)cmd->UserCallbackData;
    if (index >= g_regions.size()) return;

    if (!ensurePrograms() || !ensureTexture()) return;

    const BlurRegion region = g_regions[index];

    GLint viewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, viewport);

    const int fbW = viewport[2];
    const int fbH = viewport[3];
    if (fbW <= 0 || fbH <= 0) return;

    ImGuiIO& io = ImGui::GetIO();

    const float scaleX = io.DisplaySize.x > 0.f ? (float)fbW / io.DisplaySize.x : 1.f;
    const float scaleY = io.DisplaySize.y > 0.f ? (float)fbH / io.DisplaySize.y : 1.f;

    int px0 = (int)(region.x0 * scaleX);
    int py1 = (int)(region.y1 * scaleY);
    int pw  = (int)((region.x1 - region.x0) * scaleX);
    int ph  = (int)((region.y1 - region.y0) * scaleY);

    if (pw <= 2 || ph <= 2) return;

    int py0 = fbH - py1;

    if (px0 < 0) { pw += px0; px0 = 0; }
    if (py0 < 0) { ph += py0; py0 = 0; }
    if (px0 + pw > fbW) pw = fbW - px0;
    if (py0 + ph > fbH) ph = fbH - py0;

    if (pw <= 2 || ph <= 2) return;

    GLint previousTexture = 0;
    GLint previousProgram = 0;
    GLint previousArray   = 0;
    GLint previousVao     = 0;
    GLint previousBlendSrc = GL_SRC_ALPHA;
    GLint previousBlendDst = GL_ONE_MINUS_SRC_ALPHA;

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArray);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDst);

    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_texture);

    ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position | kCCVertexAttribFlag_TexCoords);

    const GLfloat coords[8] = {
        0.f, 0.f,
        1.f, 0.f,
        0.f, 1.f,
        1.f, 1.f
    };
    glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, 0, coords);

    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    auto drawRect = [&](int w, int h) {
        const float ndcX0 = ((float)px0 / (float)fbW) * 2.f - 1.f;
        const float ndcX1 = ((float)(px0 + w) / (float)fbW) * 2.f - 1.f;
        const float ndcY0 = ((float)py0 / (float)fbH) * 2.f - 1.f;
        const float ndcY1 = ((float)(py0 + h) / (float)fbH) * 2.f - 1.f;

        const GLfloat vertices[8] = {
            ndcX0, ndcY0,
            ndcX1, ndcY0,
            ndcX0, ndcY1,
            ndcX1, ndcY1
        };

        glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, 0, coords);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };

    auto drawRectUV = [&](int w, int h, const GLfloat coords[8]) {
        const float ndcX0 = ((float)px0 / (float)fbW) * 2.f - 1.f;
        const float ndcX1 = ((float)(px0 + w) / (float)fbW) * 2.f - 1.f;
        const float ndcY0 = ((float)py0 / (float)fbH) * 2.f - 1.f;
        const float ndcY1 = ((float)(py0 + h) / (float)fbH) * 2.f - 1.f;

        const GLfloat vertices[8] = {
            ndcX0, ndcY0,
            ndcX1, ndcY0,
            ndcX0, ndcY1,
            ndcX1, ndcY1
        };

        glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, 0, coords);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };

    // Source the Kawase chain from the per-frame pristine snapshot instead of
    // the live backbuffer. The snapshot was taken at the very start of the
    // imgui frame, before any GL geometry was emitted, so the content under the
    // glass is always this frame's real game pixels — never our own window,
    // never a partially-updated mid-render buffer, no matter how fast the
    // window is being dragged. Sample the sub-rect via UVs into the snapshot.
    const bool useSnap = g_snapshot && g_snapW == fbW && g_snapH == fbH;

    const float snX = (float)px0 / (float)fbW;
    const float snY = (float)py0 / (float)fbH;
    const float snW = (float)pw / (float)fbW;
    const float snH = (float)ph / (float)fbH;
    const GLfloat regionCoords[8] = {
        snX,       snY,
        snX + snW, snY,
        snX,       snY + snH,
        snX + snW, snY + snH
    };

    int levels = 1 + (int)(region.radius * 0.5f);
    levels = std::clamp(levels, 1, 3);
    while (levels > 1 && (std::min(pw, ph) >> levels) < 8)
        levels--;

    int curW = pw;
    int curH = ph;

    glBlendFunc(GL_ONE, GL_ZERO);

    // Bind the snapshot for the first (base) downsample: the shader samples at
    // v_uv, and the region quad maps 1:1 to the snapshot's sub-rect.
    const auto bindBase = [&] {
        glBindTexture(GL_TEXTURE_2D, g_snapshot);
        g_down.program->use();
        if (g_down.locTex >= 0) glUniform1i(g_down.locTex, 0);
        if (g_down.locHalfpixel >= 0)
            glUniform2f(g_down.locHalfpixel, 0.5f / (float)fbW, 0.5f / (float)fbH);
    };

    // Snap (viewport) mismatch fallback: copy the region straight from the
    // live backbuffer, as the pre-snapshot versions did.
    if (!useSnap) {
        glBindTexture(GL_TEXTURE_2D, g_texture);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, px0, py0, pw, ph, 0);
    }

    for (int i = 0; i < levels; i++) {
        const int nextW = std::max(curW / 2, 4);
        const int nextH = std::max(curH / 2, 4);

        if (i == 0 && useSnap) {
            bindBase();
            drawRectUV(nextW, nextH, regionCoords);
        } else {
            g_down.program->use();
            if (g_down.locTex >= 0) glUniform1i(g_down.locTex, 0);
            if (g_down.locHalfpixel >= 0)
                glUniform2f(g_down.locHalfpixel, 0.5f / (float)curW, 0.5f / (float)curH);
            drawRect(nextW, nextH);
        }

        glBindTexture(GL_TEXTURE_2D, g_texture);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, px0, py0, nextW, nextH, 0);

        curW = nextW;
        curH = nextH;
    }

    for (int i = 0; i < levels; i++) {
        const bool last = i == levels - 1;
        const int  nextW = last ? pw : std::min(curW * 2, pw);
        const int  nextH = last ? ph : std::min(curH * 2, ph);

        g_up.program->use();
        if (g_up.locTex >= 0) glUniform1i(g_up.locTex, 0);
        if (g_up.locHalfpixel >= 0)
            glUniform2f(g_up.locHalfpixel, 0.5f / (float)curW, 0.5f / (float)curH);
        if (g_up.locAlpha >= 0) glUniform1f(g_up.locAlpha, last ? region.alpha : 1.f);
        if (g_up.locRound >= 0) glUniform1f(g_up.locRound, region.rounding * scaleX);
        if (g_up.locSize  >= 0) glUniform2f(g_up.locSize, (float)pw, (float)ph);
        if (g_up.locMask  >= 0) glUniform1f(g_up.locMask, last ? 1.f : 0.f);
        if (g_up.locRefract >= 0) glUniform1f(g_up.locRefract, 1.0f);
        if (g_up.locRefractBand >= 0)
            glUniform1f(g_up.locRefractBand, std::clamp(std::min((float)pw, (float)ph) * 0.08f, 5.0f, 34.0f));
        if (g_up.locNoise >= 0) glUniform1f(g_up.locNoise, 0.06f);
        if (g_up.locFresnel >= 0) glUniform1f(g_up.locFresnel, 1.0f);
        if (g_up.locChroma >= 0) glUniform1f(g_up.locChroma, 1.0f);
        if (g_up.locSpecular >= 0) glUniform1f(g_up.locSpecular, 2.5f);
        if (g_up.locLens >= 0) glUniform1f(g_up.locLens, 1.6f);
        if (g_up.locParallax >= 0) glUniform2f(g_up.locParallax, g_parX, g_parY);
        if (g_up.locGloss >= 0) glUniform1f(g_up.locGloss, 1.0f);
        if (g_up.locLiquid >= 0) glUniform1f(g_up.locLiquid, 8.0f);
        if (g_up.locSheen >= 0) glUniform1f(g_up.locSheen, 0.10f);
        if (g_up.locTime >= 0) glUniform1f(g_up.locTime, std::fmod(static_cast<double>(ImGui::GetTime()), 100.0));

        glBlendFunc(last ? GL_SRC_ALPHA : GL_ONE, last ? GL_ONE_MINUS_SRC_ALPHA : GL_ZERO);

        drawRect(nextW, nextH);

        if (!last)
            glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, px0, py0, nextW, nextH, 0);

        curW = nextW;
        curH = nextH;
    }

    glBindTexture(GL_TEXTURE_2D, (GLuint)previousTexture);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previousArray);
    glBindVertexArray((GLuint)previousVao);
    glUseProgram((GLuint)previousProgram);

    glBlendFunc((GLenum)previousBlendSrc, (GLenum)previousBlendDst);

    if (previousScissor)
        glEnable(GL_SCISSOR_TEST);
}

} // anonymous namespace

namespace skkbot2::gui::blur {

void newFrame() {
    pollParallax();
    g_regions.clear();
    if (!g_failed) {
        if (!ensurePrograms() || !ensureTexture()) return;
        captureSnapshot();
    }
}

bool available() {
    return !g_failed;
}

void submit(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, float radius, float alpha) {
    if (!list || g_failed) return;
    if (alpha <= 0.01f) return;
    if (max.x - min.x <= 4.f || max.y - min.y <= 4.f) return;
    if (g_regions.size() >= kMaxRegions) return;

    g_regions.push_back(BlurRegion{ min.x, min.y, max.x, max.y, rounding, radius, alpha });
    list->AddCallback(renderRegion, (void*)(intptr_t)(g_regions.size() - 1));
}

} // namespace skkbot2::gui::blur
