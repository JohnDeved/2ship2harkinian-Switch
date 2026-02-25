#ifdef ENABLE_OPENGL

#include "PostProcess.h"
#include "2s2h/ShipInit.hpp"
#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/BenGui/UIWidgets.hpp"

#include <fast/Fast3dWindow.h>
#include <ship/Context.h>

#ifdef _MSC_VER
#include <SDL2/SDL.h>
#include <GL/glew.h>
#elif __APPLE__
#include <SDL2/SDL.h>
#include <GL/glew.h>
#elif defined(USE_OPENGLES) || defined(__SWITCH__)
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>
#else
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengl.h>
#endif

#include <spdlog/spdlog.h>
#include <string>
#include <vector>

// GLSL version/precision header per platform
#if defined(__APPLE__)
#define PP_GLSL_VERSION "#version 410 core\n"
#define PP_GLSL_TEXFUNC "texture"
#elif defined(USE_OPENGLES) || defined(__SWITCH__)
#define PP_GLSL_VERSION "#version 300 es\nprecision mediump float;\n"
#define PP_GLSL_TEXFUNC "texture"
#else
#define PP_GLSL_VERSION "#version 130\n"
#define PP_GLSL_TEXFUNC "texture2D"
#endif

// Use "in"/"out" for GLSL 130+ and 300 es; "attribute"/"varying" for 110
#if defined(__APPLE__) || defined(USE_OPENGLES) || defined(__SWITCH__)
#define PP_ATTR_IN "in"
#define PP_ATTR_OUT "out"
#define PP_FRAG_OUT "out vec4 fragColor;\n"
#define PP_FRAG_COLOR "fragColor"
#else
#define PP_ATTR_IN "varying"
#define PP_ATTR_OUT "varying"
#define PP_FRAG_OUT ""
#define PP_FRAG_COLOR "gl_FragColor"
#endif

// CVar names
#define CVAR_PP_FXAA "gEnhancements.Graphics.PostProcess.FXAA"
#define CVAR_PP_CAS "gEnhancements.Graphics.PostProcess.CAS"
#define CVAR_PP_CAS_STRENGTH "gEnhancements.Graphics.PostProcess.CAS.Strength"
#define CVAR_PP_VIGNETTE "gEnhancements.Graphics.PostProcess.Vignette"
#define CVAR_PP_VIGNETTE_STRENGTH "gEnhancements.Graphics.PostProcess.Vignette.Strength"

// ─── Shader sources ──────────────────────────────────────────────────────────

// Simple full-screen triangle vertex shader (shared by all effects)
static const char* sVertexShaderSrc =
    PP_GLSL_VERSION
    PP_ATTR_OUT " vec2 vTexCoord;\n"
    "void main() {\n"
    "    float x = float(gl_VertexID & 1) * 4.0 - 1.0;\n"
    "    float y = float((gl_VertexID >> 1) & 1) * 4.0 - 1.0;\n"
    "    vTexCoord = vec2(x * 0.5 + 0.5, y * 0.5 + 0.5);\n"
    "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
    "}\n";

// FXAA fragment shader (FXAA 3.11 quality preset, adapted for portability)
static const char* sFxaaFragSrc =
    PP_GLSL_VERSION
    PP_ATTR_IN " vec2 vTexCoord;\n"
    PP_FRAG_OUT
    "uniform sampler2D uTexture;\n"
    "uniform vec2 uTexelSize;\n"
    "void main() {\n"
    "    vec3 rgbNW = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + vec2(-1.0, -1.0) * uTexelSize).rgb;\n"
    "    vec3 rgbNE = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + vec2( 1.0, -1.0) * uTexelSize).rgb;\n"
    "    vec3 rgbSW = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + vec2(-1.0,  1.0) * uTexelSize).rgb;\n"
    "    vec3 rgbSE = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + vec2( 1.0,  1.0) * uTexelSize).rgb;\n"
    "    vec3 rgbM  = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord).rgb;\n"
    "    vec3 luma = vec3(0.299, 0.587, 0.114);\n"
    "    float lumaNW = dot(rgbNW, luma);\n"
    "    float lumaNE = dot(rgbNE, luma);\n"
    "    float lumaSW = dot(rgbSW, luma);\n"
    "    float lumaSE = dot(rgbSE, luma);\n"
    "    float lumaM  = dot(rgbM,  luma);\n"
    "    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));\n"
    "    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));\n"
    "    float lumaRange = lumaMax - lumaMin;\n"
    "    if (lumaRange < max(0.0312, lumaMax * 0.125)) {\n"
    "        " PP_FRAG_COLOR " = vec4(rgbM, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec2 dir;\n"
    "    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));\n"
    "    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));\n"
    "    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);\n"
    "    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
    "    dir = min(vec2(8.0), max(vec2(-8.0), dir * rcpDirMin)) * uTexelSize;\n"
    "    vec3 rgbA = 0.5 * (\n"
    "        " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + dir * (1.0 / 3.0 - 0.5)).rgb +\n"
    "        " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "    vec3 rgbB = rgbA * 0.5 + 0.25 * (\n"
    "        " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + dir * -0.5).rgb +\n"
    "        " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + dir *  0.5).rgb);\n"
    "    float lumaB = dot(rgbB, luma);\n"
    "    if (lumaB < lumaMin || lumaB > lumaMax) {\n"
    "        " PP_FRAG_COLOR " = vec4(rgbA, 1.0);\n"
    "    } else {\n"
    "        " PP_FRAG_COLOR " = vec4(rgbB, 1.0);\n"
    "    }\n"
    "}\n";

// CAS (Contrast Adaptive Sharpening) fragment shader - based on AMD FidelityFX CAS
static const char* sCasFragSrc =
    PP_GLSL_VERSION
    PP_ATTR_IN " vec2 vTexCoord;\n"
    PP_FRAG_OUT
    "uniform sampler2D uTexture;\n"
    "uniform vec2 uTexelSize;\n"
    "uniform float uSharpness;\n"
    "void main() {\n"
    "    vec3 a = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + vec2(-1.0,  0.0) * uTexelSize).rgb;\n"
    "    vec3 b = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + vec2( 0.0, -1.0) * uTexelSize).rgb;\n"
    "    vec3 c = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord).rgb;\n"
    "    vec3 d = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + vec2( 0.0,  1.0) * uTexelSize).rgb;\n"
    "    vec3 e = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord + vec2( 1.0,  0.0) * uTexelSize).rgb;\n"
    "    vec3 mnRGB = min(c, min(min(a, b), min(d, e)));\n"
    "    vec3 mxRGB = max(c, max(max(a, b), max(d, e)));\n"
    "    vec3 ampRGB = clamp(min(mnRGB, 1.0 - mxRGB) / mxRGB, 0.0, 1.0);\n"
    "    ampRGB = sqrt(ampRGB);\n"
    "    float peak = -3.0 * uSharpness + 8.0;\n"
    "    vec3 w = ampRGB / peak;\n"
    "    vec3 rcpW = 1.0 / (1.0 + 4.0 * w);\n"
    "    vec3 output0 = ((b + d + a + e) * w + c) * rcpW;\n"
    "    " PP_FRAG_COLOR " = vec4(clamp(output0, 0.0, 1.0), 1.0);\n"
    "}\n";

// Vignette fragment shader
static const char* sVignetteFragSrc =
    PP_GLSL_VERSION
    PP_ATTR_IN " vec2 vTexCoord;\n"
    PP_FRAG_OUT
    "uniform sampler2D uTexture;\n"
    "uniform float uStrength;\n"
    "void main() {\n"
    "    vec4 color = " PP_GLSL_TEXFUNC "(uTexture, vTexCoord);\n"
    "    vec2 uv = vTexCoord * 2.0 - 1.0;\n"
    "    float vignette = 1.0 - dot(uv, uv) * uStrength;\n"
    "    color.rgb *= clamp(vignette, 0.0, 1.0);\n"
    "    " PP_FRAG_COLOR " = color;\n"
    "}\n";

// ─── GL resource management ─────────────────────────────────────────────────

struct PostProcessPass {
    GLuint program = 0;
    GLint texLoc = -1;
    GLint texelSizeLoc = -1;
    GLint sharpnessLoc = -1;
    GLint strengthLoc = -1;
};

static struct {
    bool initialized = false;
    GLuint vao = 0;
    GLuint fbo = 0;
    GLuint textures[2] = { 0, 0 }; // ping-pong textures
    uint32_t texWidth = 0;
    uint32_t texHeight = 0;
    PostProcessPass fxaa;
    PostProcessPass cas;
    PostProcessPass vignette;
} sState;

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        SPDLOG_ERROR("PostProcess shader compile error: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool LinkProgram(PostProcessPass& pass, const char* fragSrc) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, sVertexShaderSrc);
    if (!vs) return false;
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!fs) { glDeleteShader(vs); return false; }

    pass.program = glCreateProgram();
    glAttachShader(pass.program, vs);
    glAttachShader(pass.program, fs);
    glLinkProgram(pass.program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint status;
    glGetProgramiv(pass.program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(pass.program, sizeof(log), nullptr, log);
        SPDLOG_ERROR("PostProcess program link error: {}", log);
        glDeleteProgram(pass.program);
        pass.program = 0;
        return false;
    }

    pass.texLoc = glGetUniformLocation(pass.program, "uTexture");
    pass.texelSizeLoc = glGetUniformLocation(pass.program, "uTexelSize");
    pass.sharpnessLoc = glGetUniformLocation(pass.program, "uSharpness");
    pass.strengthLoc = glGetUniformLocation(pass.program, "uStrength");
    return true;
}

static void EnsureTextures(uint32_t width, uint32_t height) {
    if (sState.texWidth == width && sState.texHeight == height && sState.textures[0]) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (!sState.textures[i]) {
            glGenTextures(1, &sState.textures[i]);
        }
        glBindTexture(GL_TEXTURE_2D, sState.textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    sState.texWidth = width;
    sState.texHeight = height;
}

static void InitGL() {
    if (sState.initialized) return;

    glGenVertexArrays(1, &sState.vao);
    glGenFramebuffers(1, &sState.fbo);

    LinkProgram(sState.fxaa, sFxaaFragSrc);
    LinkProgram(sState.cas, sCasFragSrc);
    LinkProgram(sState.vignette, sVignetteFragSrc);

    sState.initialized = true;
    SPDLOG_INFO("PostProcess: GL resources initialized");
}

static void CleanupGL() {
    if (!sState.initialized) return;

    auto deletePass = [](PostProcessPass& p) {
        if (p.program) { glDeleteProgram(p.program); p.program = 0; }
    };
    deletePass(sState.fxaa);
    deletePass(sState.cas);
    deletePass(sState.vignette);

    for (int i = 0; i < 2; i++) {
        if (sState.textures[i]) { glDeleteTextures(1, &sState.textures[i]); sState.textures[i] = 0; }
    }
    if (sState.fbo) { glDeleteFramebuffers(1, &sState.fbo); sState.fbo = 0; }
    if (sState.vao) { glDeleteVertexArrays(1, &sState.vao); sState.vao = 0; }
    sState.texWidth = 0;
    sState.texHeight = 0;
    sState.initialized = false;
    SPDLOG_INFO("PostProcess: GL resources cleaned up");
}

// Renders a full-screen pass: binds inputTex, renders into outputTex via FBO
static void RenderPass(const PostProcessPass& pass, GLuint inputTex, GLuint outputTex,
                        uint32_t width, uint32_t height) {
    glBindFramebuffer(GL_FRAMEBUFFER, sState.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTex, 0);
    glViewport(0, 0, width, height);

    glUseProgram(pass.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTex);
    if (pass.texLoc >= 0) glUniform1i(pass.texLoc, 0);
    if (pass.texelSizeLoc >= 0) glUniform2f(pass.texelSizeLoc, 1.0f / width, 1.0f / height);

    glBindVertexArray(sState.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ─── Post-process callback ──────────────────────────────────────────────────

static uintptr_t PostProcessCallback(uintptr_t texId, uint32_t width, uint32_t height) {
    InitGL();
    EnsureTextures(width, height);

    bool fxaaEnabled = CVarGetInteger(CVAR_PP_FXAA, 0) != 0;
    bool casEnabled = CVarGetInteger(CVAR_PP_CAS, 0) != 0;
    bool vignetteEnabled = CVarGetInteger(CVAR_PP_VIGNETTE, 0) != 0;

    if (!fxaaEnabled && !casEnabled && !vignetteEnabled) {
        return texId;
    }

    // Save GL state that we modify
    GLint prevFbo, prevViewport[4], prevProgram, prevVao, prevTex;
    GLboolean prevDepthTest, prevBlend;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
#if defined(__APPLE__) || defined(USE_OPENGLES) || defined(__SWITCH__)
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
#endif
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
    prevBlend = glIsEnabled(GL_BLEND);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    GLuint currentTex = (GLuint)texId;
    int pingPongIdx = 0;

    // Apply effects in chain
    if (fxaaEnabled && sState.fxaa.program) {
        GLuint outputTex = sState.textures[pingPongIdx];
        RenderPass(sState.fxaa, currentTex, outputTex, width, height);
        currentTex = outputTex;
        pingPongIdx = 1 - pingPongIdx;
    }

    if (casEnabled && sState.cas.program) {
        float sharpness = CVarGetFloat(CVAR_PP_CAS_STRENGTH, 0.5f);
        glUseProgram(sState.cas.program);
        if (sState.cas.sharpnessLoc >= 0) glUniform1f(sState.cas.sharpnessLoc, sharpness);
        GLuint outputTex = sState.textures[pingPongIdx];
        RenderPass(sState.cas, currentTex, outputTex, width, height);
        currentTex = outputTex;
        pingPongIdx = 1 - pingPongIdx;
    }

    if (vignetteEnabled && sState.vignette.program) {
        float strength = CVarGetFloat(CVAR_PP_VIGNETTE_STRENGTH, 0.5f);
        glUseProgram(sState.vignette.program);
        if (sState.vignette.strengthLoc >= 0) glUniform1f(sState.vignette.strengthLoc, strength);
        GLuint outputTex = sState.textures[pingPongIdx];
        RenderPass(sState.vignette, currentTex, outputTex, width, height);
        currentTex = outputTex;
        pingPongIdx = 1 - pingPongIdx;
    }

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glUseProgram(prevProgram);
#if defined(__APPLE__) || defined(USE_OPENGLES) || defined(__SWITCH__)
    glBindVertexArray(prevVao);
#endif
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);

    return (uintptr_t)currentTex;
}

// ─── Registration ───────────────────────────────────────────────────────────

static bool IsAnyEffectEnabled() {
    return CVarGetInteger(CVAR_PP_FXAA, 0) != 0 ||
           CVarGetInteger(CVAR_PP_CAS, 0) != 0 ||
           CVarGetInteger(CVAR_PP_VIGNETTE, 0) != 0;
}

static void RegisterPostProcess() {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
    if (!wnd) return;
    auto interp = wnd->GetInterpreterWeak().lock();
    if (!interp) return;

    if (IsAnyEffectEnabled()) {
        interp->SetPostProcessCallback(PostProcessCallback);
    } else {
        interp->ClearPostProcessCallback();
        CleanupGL();
    }
}

static RegisterShipInitFunc initFunc(RegisterPostProcess,
    { CVAR_PP_FXAA, CVAR_PP_CAS, CVAR_PP_CAS_STRENGTH, CVAR_PP_VIGNETTE, CVAR_PP_VIGNETTE_STRENGTH });

// ─── Menu UI ────────────────────────────────────────────────────────────────

void PostProcess_RenderMenuOptions() {
    ImGui::SeparatorText("Post-Processing (ReShade)");
    UIWidgets::CVarCheckbox("FXAA Anti-Aliasing", CVAR_PP_FXAA,
        UIWidgets::CheckboxOptions().Tooltip("Fast Approximate Anti-Aliasing. Smooths jagged edges."));
    UIWidgets::CVarCheckbox("CAS Sharpening", CVAR_PP_CAS,
        UIWidgets::CheckboxOptions().Tooltip(
            "Contrast Adaptive Sharpening (AMD FidelityFX CAS). Enhances image clarity."));
    if (CVarGetInteger(CVAR_PP_CAS, 0)) {
        UIWidgets::CVarSliderFloat("CAS Strength: %.2f", CVAR_PP_CAS_STRENGTH,
            UIWidgets::FloatSliderOptions().Min(0.0f).Max(1.0f).DefaultValue(0.5f)
                .Tooltip("How strong the sharpening effect is. 0 = subtle, 1 = maximum."));
    }
    UIWidgets::CVarCheckbox("Vignette", CVAR_PP_VIGNETTE,
        UIWidgets::CheckboxOptions().Tooltip("Darkens the edges of the screen for a cinematic effect."));
    if (CVarGetInteger(CVAR_PP_VIGNETTE, 0)) {
        UIWidgets::CVarSliderFloat("Vignette Strength: %.2f", CVAR_PP_VIGNETTE_STRENGTH,
            UIWidgets::FloatSliderOptions().Min(0.1f).Max(1.5f).DefaultValue(0.5f)
                .Tooltip("How strong the vignette darkening is."));
    }
}

#else // !ENABLE_OPENGL

#include "PostProcess.h"
void PostProcess_RenderMenuOptions() {}

#endif // ENABLE_OPENGL
