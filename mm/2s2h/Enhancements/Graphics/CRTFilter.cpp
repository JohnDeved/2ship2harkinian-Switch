// CRT Filter — Post-processing using RetroArch CRT shaders (public domain).
// Based on "crt-lottes" by Timothy Lottes (PUBLIC DOMAIN).
// https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-lottes.glsl
//
// Adapted for the 2Ship2Harkinian rendering pipeline with a focus on maximum
// GPU performance on the Nintendo Switch (Tegra X1 / Maxwell 256-core).
//
// Performance notes:
//   - Single fullscreen triangle (1 draw call, 3 verts, no index buffer).
//   - No glGet* state queries — known call-site state is restored directly.
//   - VAO configured once; bound/unbound per frame with zero reconfiguration.
//   - Shader compiled once on first enable; FBO resized only on dimension change.
//   - "Lite" preset disables bloom (saves ~31 texture fetches/pixel).

#ifdef ENABLE_OPENGL

#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/BenGui/UIWidgets.hpp"
#include "2s2h/ShipInit.hpp"
#include "fast/backends/gfx_opengl.h"
#include "fast/Fast3dWindow.h"
#include "ship/Context.h"

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------
#define CVAR_CRT_ENABLED "gEnhancements.Graphics.CRTFilter.Enabled"
#define CVAR_CRT_PRESET "gEnhancements.Graphics.CRTFilter.Preset"
#define CVAR_CRT_SCANLINE "gEnhancements.Graphics.CRTFilter.ScanlineHardness"
#define CVAR_CRT_WARP_X "gEnhancements.Graphics.CRTFilter.WarpX"
#define CVAR_CRT_WARP_Y "gEnhancements.Graphics.CRTFilter.WarpY"
#define CVAR_CRT_MASK_DARK "gEnhancements.Graphics.CRTFilter.MaskDark"
#define CVAR_CRT_MASK_LIGHT "gEnhancements.Graphics.CRTFilter.MaskLight"
#define CVAR_CRT_MASK_TYPE "gEnhancements.Graphics.CRTFilter.MaskType"
#define CVAR_CRT_BRIGHTNESS "gEnhancements.Graphics.CRTFilter.Brightness"

// Preset indices
enum CRTPreset {
    CRT_PRESET_LITE = 0, // No bloom — fast path for Switch
    CRT_PRESET_FULL = 1, // Full crt-lottes with bloom
};

// ---------------------------------------------------------------------------
// GLSL version header — selected at compile time per platform
// ---------------------------------------------------------------------------
#if defined(__SWITCH__) || defined(USE_OPENGLES)
static const char* sGlslHeader =
    "#version 300 es\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n";
#elif defined(__APPLE__)
static const char* sGlslHeader = "#version 410 core\n";
#else
static const char* sGlslHeader = "#version 130\n";
#endif

// ---------------------------------------------------------------------------
// Vertex shader — minimal fullscreen triangle
// ---------------------------------------------------------------------------
static const char* sVertBody = R"(
#if __VERSION__ >= 300
#define COMPAT_ATTRIBUTE in
#define COMPAT_VARYING   out
#else
#define COMPAT_ATTRIBUTE attribute
#define COMPAT_VARYING   varying
#endif

COMPAT_ATTRIBUTE vec2 aPosition;
COMPAT_VARYING   vec4 TEX0;

void main() {
    TEX0.xy     = aPosition * 0.5 + 0.5;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Fragment shader — crt-lottes (Timothy Lottes, PUBLIC DOMAIN)
// Adapted: split from combined file, PARAMETER_UNIFORM always active,
// bloom gated by DO_BLOOM define, sRGB gamma path used.
// ---------------------------------------------------------------------------
static const char* sFragBody = R"(
#if __VERSION__ >= 300
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor      gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

COMPAT_VARYING vec4 TEX0;

uniform vec2      OutputSize;
uniform vec2      TextureSize;
uniform vec2      InputSize;
uniform sampler2D Texture;

// Tunable uniforms
uniform float hardScan;
uniform float hardPix;
uniform float warpX;
uniform float warpY;
uniform float maskDark;
uniform float maskLight;
uniform float shadowMask;
uniform float brightBoost;
uniform float hardBloomPix;
uniform float hardBloomScan;
uniform float bloomAmount;
uniform float shape;

#define vTexCoord  TEX0.xy
#define Source     Texture
#define SourceSize vec4(TextureSize, 1.0 / TextureSize)

// sRGB ↔ linear
float ToLinear1(float c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
vec3  ToLinear(vec3 c)   { return vec3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b)); }
float ToSrgb1(float c)   { return (c < 0.0031308) ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055; }
vec3  ToSrgb(vec3 c)     { return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b)); }

// Nearest emulated sample given float position and texel offset.
vec3 Fetch(vec2 pos, vec2 off) {
    pos = (floor(pos * SourceSize.xy + off) + vec2(0.5)) / SourceSize.xy;
    return ToLinear(brightBoost * COMPAT_TEXTURE(Source, pos.xy).rgb);
}

// Distance in emulated pixels to nearest texel.
vec2 Dist(vec2 pos) { pos = pos * SourceSize.xy; return -((pos - floor(pos)) - vec2(0.5)); }

// 1-D Gaussian.
float Gaus(float pos, float scale) { return exp2(scale * pow(abs(pos), shape)); }

// 3-tap horizontal filter.
vec3 Horz3(vec2 pos, float off) {
    vec3 b = Fetch(pos, vec2(-1.0, off));
    vec3 c = Fetch(pos, vec2( 0.0, off));
    vec3 d = Fetch(pos, vec2( 1.0, off));
    float dst = Dist(pos).x;
    float wb = Gaus(dst - 1.0, hardPix);
    float wc = Gaus(dst + 0.0, hardPix);
    float wd = Gaus(dst + 1.0, hardPix);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

// 5-tap horizontal filter.
vec3 Horz5(vec2 pos, float off) {
    vec3 a = Fetch(pos, vec2(-2.0, off));
    vec3 b = Fetch(pos, vec2(-1.0, off));
    vec3 c = Fetch(pos, vec2( 0.0, off));
    vec3 d = Fetch(pos, vec2( 1.0, off));
    vec3 e = Fetch(pos, vec2( 2.0, off));
    float dst = Dist(pos).x;
    float scale = hardPix;
    float wa = Gaus(dst - 2.0, scale);
    float wb = Gaus(dst - 1.0, scale);
    float wc = Gaus(dst + 0.0, scale);
    float wd = Gaus(dst + 1.0, scale);
    float we = Gaus(dst + 2.0, scale);
    return (a*wa + b*wb + c*wc + d*wd + e*we) / (wa + wb + wc + wd + we);
}

#ifdef DO_BLOOM
// 7-tap horizontal filter (bloom only).
vec3 Horz7(vec2 pos, float off) {
    vec3 a = Fetch(pos, vec2(-3.0, off));
    vec3 b = Fetch(pos, vec2(-2.0, off));
    vec3 c = Fetch(pos, vec2(-1.0, off));
    vec3 d = Fetch(pos, vec2( 0.0, off));
    vec3 e = Fetch(pos, vec2( 1.0, off));
    vec3 f = Fetch(pos, vec2( 2.0, off));
    vec3 g = Fetch(pos, vec2( 3.0, off));
    float dst = Dist(pos).x;
    float scale = hardBloomPix;
    float wa = Gaus(dst - 3.0, scale);
    float wb = Gaus(dst - 2.0, scale);
    float wc = Gaus(dst - 1.0, scale);
    float wd = Gaus(dst + 0.0, scale);
    float we = Gaus(dst + 1.0, scale);
    float wf = Gaus(dst + 2.0, scale);
    float wg = Gaus(dst + 3.0, scale);
    return (a*wa+b*wb+c*wc+d*wd+e*we+f*wf+g*wg)/(wa+wb+wc+wd+we+wf+wg);
}
#endif

// Scanline weight.
float Scan(vec2 pos, float off)      { return Gaus(Dist(pos).y + off, hardScan); }
#ifdef DO_BLOOM
float BloomScan(vec2 pos, float off) { return Gaus(Dist(pos).y + off, hardBloomScan); }
#endif

// Allow nearest three scanlines to affect pixel.
vec3 Tri(vec2 pos) {
    vec3 a = Horz3(pos, -1.0);
    vec3 b = Horz5(pos,  0.0);
    vec3 c = Horz3(pos,  1.0);
    float wa = Scan(pos, -1.0);
    float wb = Scan(pos,  0.0);
    float wc = Scan(pos,  1.0);
    return a * wa + b * wb + c * wc;
}

#ifdef DO_BLOOM
vec3 Bloom(vec2 pos) {
    vec3 a = Horz5(pos, -2.0);
    vec3 b = Horz7(pos, -1.0);
    vec3 c = Horz7(pos,  0.0);
    vec3 d = Horz7(pos,  1.0);
    vec3 e = Horz5(pos,  2.0);
    float wa = BloomScan(pos, -2.0);
    float wb = BloomScan(pos, -1.0);
    float wc = BloomScan(pos,  0.0);
    float wd = BloomScan(pos,  1.0);
    float we = BloomScan(pos,  2.0);
    return a*wa + b*wb + c*wc + d*wd + e*we;
}
#endif

// Screen warp (barrel distortion).
vec2 Warp(vec2 pos) {
    pos = pos * 2.0 - 1.0;
    pos *= vec2(1.0 + (pos.y*pos.y) * warpX, 1.0 + (pos.x*pos.x) * warpY);
    return pos * 0.5 + 0.5;
}

// Shadow mask.
vec3 Mask(vec2 pos) {
    vec3 mask = vec3(maskDark);

    // Very compressed TV style shadow mask.
    if (shadowMask == 1.0) {
        float line = maskLight;
        float odd  = 0.0;
        if (fract(pos.x * 0.166666666) < 0.5) odd = 1.0;
        if (fract((pos.y + odd) * 0.5) < 0.5) line = maskDark;
        pos.x = fract(pos.x * 0.333333333);
        if      (pos.x < 0.333) mask.r = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.b = maskLight;
        mask *= line;
    }
    // Aperture grille.
    else if (shadowMask == 2.0) {
        pos.x = fract(pos.x * 0.333333333);
        if      (pos.x < 0.333) mask.r = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.b = maskLight;
    }
    // Stretched VGA style shadow mask.
    else if (shadowMask == 3.0) {
        pos.x += pos.y * 3.0;
        pos.x  = fract(pos.x * 0.166666666);
        if      (pos.x < 0.333) mask.r = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.b = maskLight;
    }
    // VGA style shadow mask.
    else if (shadowMask == 4.0) {
        pos.xy = floor(pos.xy * vec2(1.0, 0.5));
        pos.x += pos.y * 3.0;
        pos.x  = fract(pos.x * 0.166666666);
        if      (pos.x < 0.333) mask.r = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.b = maskLight;
    }

    return mask;
}

void main() {
    vec2 pos = Warp(vTexCoord * (TextureSize.xy / InputSize.xy)) * (InputSize.xy / TextureSize.xy);
    vec3 outColor = Tri(pos);

#ifdef DO_BLOOM
    outColor.rgb += Bloom(pos) * bloomAmount;
#endif

    if (shadowMask > 0.0)
        outColor.rgb *= Mask(gl_FragCoord.xy * 1.000001);

#ifdef GL_ES
    // Black out pixels outside the valid area (GLES border clamp workaround).
    if (pos.x < 0.0001 || pos.x > 0.9999 || pos.y < 0.0001 || pos.y > 0.9999)
        outColor.rgb = vec3(0.0);
#endif

    FragColor = vec4(ToSrgb(outColor.rgb), 1.0);
}
)";

// ---------------------------------------------------------------------------
// GL resources (static, process-lifetime)
// ---------------------------------------------------------------------------
static GLuint sCrtProgram[2] = {}; // [CRT_PRESET_LITE], [CRT_PRESET_FULL]
static GLuint sCrtVao = 0;
static GLuint sCrtVbo = 0;
static GLuint sCrtFbo = 0;
static GLuint sCrtTexture = 0;
static uint32_t sCrtTexWidth = 0;
static uint32_t sCrtTexHeight = 0;

// Uniform locations per preset (avoid glGetUniformLocation per frame)
struct CRTUniforms {
    GLint outputSize, textureSize, inputSize, texture;
    GLint hardScan, hardPix, warpX, warpY;
    GLint maskDark, maskLight, shadowMask, brightBoost;
    GLint hardBloomPix, hardBloomScan, bloomAmount, shape;
};
static CRTUniforms sUniforms[2] = {};
static bool sInitialized[2] = {};

// ---------------------------------------------------------------------------
// Shader compilation helpers
// ---------------------------------------------------------------------------
static GLuint CompileShader(GLenum type, const char* header, const char* defines, const char* body) {
    GLuint shader = glCreateShader(type);
    const char* sources[3] = { header, defines, body };
    glShaderSource(shader, 3, sources, nullptr);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "CRT %s compile error:\n%s\n", type == GL_VERTEX_SHADER ? "VS" : "FS", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool InitPreset(int preset) {
    if (sInitialized[preset])
        return sCrtProgram[preset] != 0;

    const char* fragDefines = (preset == CRT_PRESET_FULL) ? "#define DO_BLOOM\n" : "\n";

    GLuint vs = CompileShader(GL_VERTEX_SHADER, sGlslHeader, "\n", sVertBody);
    if (!vs)
        return false;
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, sGlslHeader, fragDefines, sFragBody);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "CRT link error (preset %d):\n%s\n", preset, log);
        glDeleteProgram(prog);
        sInitialized[preset] = true;
        return false;
    }

    sCrtProgram[preset] = prog;

    // Cache all uniform locations once
    CRTUniforms& u = sUniforms[preset];
    u.outputSize = glGetUniformLocation(prog, "OutputSize");
    u.textureSize = glGetUniformLocation(prog, "TextureSize");
    u.inputSize = glGetUniformLocation(prog, "InputSize");
    u.texture = glGetUniformLocation(prog, "Texture");
    u.hardScan = glGetUniformLocation(prog, "hardScan");
    u.hardPix = glGetUniformLocation(prog, "hardPix");
    u.warpX = glGetUniformLocation(prog, "warpX");
    u.warpY = glGetUniformLocation(prog, "warpY");
    u.maskDark = glGetUniformLocation(prog, "maskDark");
    u.maskLight = glGetUniformLocation(prog, "maskLight");
    u.shadowMask = glGetUniformLocation(prog, "shadowMask");
    u.brightBoost = glGetUniformLocation(prog, "brightBoost");
    u.hardBloomPix = glGetUniformLocation(prog, "hardBloomPix");
    u.hardBloomScan = glGetUniformLocation(prog, "hardBloomScan");
    u.bloomAmount = glGetUniformLocation(prog, "bloomAmount");
    u.shape = glGetUniformLocation(prog, "shape");

    sInitialized[preset] = true;
    return true;
}

static bool InitGeometry() {
    if (sCrtVao)
        return true;

    // Single fullscreen triangle — 3 verts, no index buffer, covers full NDC.
    static const float kTriVerts[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };

    glGenVertexArrays(1, &sCrtVao);
    glGenBuffers(1, &sCrtVbo);

    glBindVertexArray(sCrtVao);
    glBindBuffer(GL_ARRAY_BUFFER, sCrtVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kTriVerts), kTriVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); // aPosition always at location 0
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenFramebuffers(1, &sCrtFbo);
    glGenTextures(1, &sCrtTexture);

    return true;
}

static void EnsureFboSize(uint32_t w, uint32_t h) {
    if (sCrtTexWidth == w && sCrtTexHeight == h)
        return;

    glBindTexture(GL_TEXTURE_2D, sCrtTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, sCrtFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sCrtTexture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    sCrtTexWidth = w;
    sCrtTexHeight = h;
}

// ---------------------------------------------------------------------------
// Post-process callback — called from interpreter on the render thread.
// No GL state queries; the call-site state is known:
//   - FB 0 bound, depth test off, blend off, no program bound.
// ---------------------------------------------------------------------------
static uintptr_t CRTPostProcess(uintptr_t inputTexId, uint32_t width, uint32_t height) {
    if (!CVarGetInteger(CVAR_CRT_ENABLED, 0))
        return inputTexId;

    int preset = CVarGetInteger(CVAR_CRT_PRESET, CRT_PRESET_LITE);
    if (preset < 0 || preset > CRT_PRESET_FULL)
        preset = CRT_PRESET_LITE;

    if (!InitGeometry())
        return inputTexId;
    if (!InitPreset(preset))
        return inputTexId;

    EnsureFboSize(width, height);

    GLuint prog = sCrtProgram[preset];
    const CRTUniforms& u = sUniforms[preset];

    // --- Render CRT pass (no state save — known entry state) ---
    glBindFramebuffer(GL_FRAMEBUFFER, sCrtFbo);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)inputTexId);
    glUniform1i(u.texture, 0);

    // RetroArch uniforms
    float w_f = (float)width;
    float h_f = (float)height;
    glUniform2f(u.outputSize, w_f, h_f);
    glUniform2f(u.textureSize, w_f, h_f);
    glUniform2f(u.inputSize, w_f, h_f);

    // crt-lottes parameters from CVars (with RetroArch defaults)
    glUniform1f(u.hardScan, CVarGetFloat(CVAR_CRT_SCANLINE, -8.0f));
    glUniform1f(u.hardPix, -3.0f);
    glUniform1f(u.warpX, CVarGetFloat(CVAR_CRT_WARP_X, 0.031f));
    glUniform1f(u.warpY, CVarGetFloat(CVAR_CRT_WARP_Y, 0.041f));
    glUniform1f(u.maskDark, CVarGetFloat(CVAR_CRT_MASK_DARK, 0.5f));
    glUniform1f(u.maskLight, CVarGetFloat(CVAR_CRT_MASK_LIGHT, 1.5f));
    glUniform1f(u.shadowMask, (float)CVarGetInteger(CVAR_CRT_MASK_TYPE, 3));
    glUniform1f(u.brightBoost, CVarGetFloat(CVAR_CRT_BRIGHTNESS, 1.0f));
    glUniform1f(u.shape, 2.0f);

    // Bloom uniforms (only matter for FULL preset but harmless to set)
    glUniform1f(u.hardBloomPix, -1.5f);
    glUniform1f(u.hardBloomScan, -2.0f);
    glUniform1f(u.bloomAmount, 0.15f);

    glBindVertexArray(sCrtVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // --- Restore known state for ImGui that follows ---
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return (uintptr_t)sCrtTexture;
}

// ---------------------------------------------------------------------------
// Registration — callback set once at init; CVar checked inside callback.
// ---------------------------------------------------------------------------
static RegisterShipInitFunc initFunc(
    []() {
        auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
        if (wnd) {
            auto interp = wnd->GetInterpreterWeak().lock();
            if (interp) {
                interp->SetPostProcessCallback(CRTPostProcess);
            }
        }
    },
    {});

#endif // ENABLE_OPENGL
