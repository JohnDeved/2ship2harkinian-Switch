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

// ReShade FX compiler (standalone integration from crosire/reshade)
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include "effect_preprocessor.hpp"

#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cstring>

// CVar names
#define CVAR_PP_ENABLED "gEnhancements.Graphics.PostProcess.Enabled"
#define CVAR_PP_EFFECT "gEnhancements.Graphics.PostProcess.Effect"

// ─── Platform GLSL version patching ─────────────────────────────────────────
// ReShade FX GLSL codegen emits #version 430. We adapt for each platform.

#if defined(__APPLE__)
#define PP_TARGET_GLSL_VERSION "#version 410 core"
#elif defined(USE_OPENGLES) || defined(__SWITCH__)
#define PP_TARGET_GLSL_VERSION "#version 300 es\nprecision mediump float;"
#else
#define PP_TARGET_GLSL_VERSION "#version 130"
#endif

static std::string PatchGLSLForPlatform(const std::string& glsl) {
    std::string patched = glsl;

    // Replace the #version 430 directive with our target
    const std::string versionTag = "#version 430";
    size_t pos = patched.find(versionTag);
    if (pos != std::string::npos) {
        patched.replace(pos, versionTag.size(), PP_TARGET_GLSL_VERSION);
    }

#if defined(USE_OPENGLES) || defined(__SWITCH__)
    // GLES 300 es does not support layout(binding = X) — strip binding qualifiers.
    // "layout(binding = N) uniform" -> "uniform"
    // "layout(std140, column_major, binding = 0) uniform" -> "layout(std140) uniform"
    std::string result;
    result.reserve(patched.size());
    size_t i = 0;
    while (i < patched.size()) {
        if (patched.compare(i, 7, "layout(") == 0) {
            size_t close = patched.find(')', i);
            if (close != std::string::npos) {
                std::string layoutContent = patched.substr(i + 7, close - i - 7);
                size_t nextNonSpace = patched.find_first_not_of(" \t", close + 1);
                bool isUniform =
                    (nextNonSpace != std::string::npos && patched.compare(nextNonSpace, 7, "uniform") == 0);

                if (isUniform) {
                    // Simple binding-only layout: remove entirely
                    if (layoutContent.find("binding") != std::string::npos &&
                        layoutContent.find("std140") == std::string::npos) {
                        i = close + 1;
                        while (i < patched.size() && (patched[i] == ' ' || patched[i] == '\t'))
                            i++;
                        continue;
                    }
                    // UBO layout: keep std140, drop binding qualifier
                    if (layoutContent.find("std140") != std::string::npos) {
                        result += "layout(std140) ";
                        i = close + 1;
                        while (i < patched.size() && (patched[i] == ' ' || patched[i] == '\t'))
                            i++;
                        continue;
                    }
                }
            }
        }
        result += patched[i];
        i++;
    }
    patched = result;
#endif

    return patched;
}

// ─── Effect data structures ─────────────────────────────────────────────────

struct CompiledPass {
    GLuint program = 0;
};

struct CompiledEffect {
    std::string name;
    std::vector<CompiledPass> passes;
    std::vector<reshadefx::uniform> uniforms;
    std::vector<uint8_t> uniformData;
    GLuint uniformBuffer = 0;
};

// ─── GL state ───────────────────────────────────────────────────────────────

static struct {
    bool initialized = false;
    GLuint vao = 0;
    GLuint fbo = 0;
    GLuint textures[2] = { 0, 0 };
    uint32_t texWidth = 0;
    uint32_t texHeight = 0;
    CompiledEffect currentEffect;
    std::vector<std::string> availableEffects;
    std::string loadedEffectName;
} sState;

// ─── Shader compilation helpers ─────────────────────────────────────────────

static GLuint CompileGLShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        SPDLOG_ERROR("ReShade PostProcess shader compile error: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint LinkGLProgram(GLuint vs, GLuint fs) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint status;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        SPDLOG_ERROR("ReShade PostProcess program link error: {}", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// ─── ReShade FX compiler integration ────────────────────────────────────────

static std::string GetEffectsPath() {
    std::string basePath = Ship::Context::GetInstance()->GetAppBundlePath();
    if (basePath.empty() || basePath == ".") {
        basePath = ".";
    }
    return basePath + "/reshade-shaders/Shaders";
}

static void ScanAvailableEffects() {
    sState.availableEffects.clear();
    std::string path = GetEffectsPath();

    if (!std::filesystem::exists(path)) {
        SPDLOG_INFO("ReShade effects directory not found: {}", path);
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_regular_file()) {
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".fx") {
                sState.availableEffects.push_back(entry.path().stem().string());
            }
        }
    }

    std::sort(sState.availableEffects.begin(), sState.availableEffects.end());
    SPDLOG_INFO("ReShade: Found {} .fx effects", sState.availableEffects.size());
}

static void FreeEffect(CompiledEffect& effect) {
    for (auto& pass : effect.passes) {
        if (pass.program) {
            glDeleteProgram(pass.program);
            pass.program = 0;
        }
    }
    if (effect.uniformBuffer) {
        glDeleteBuffers(1, &effect.uniformBuffer);
        effect.uniformBuffer = 0;
    }
    effect.passes.clear();
    effect.uniforms.clear();
    effect.uniformData.clear();
    effect.name.clear();
}

static bool CompileEffect(const std::string& effectName, CompiledEffect& effect) {
    FreeEffect(effect);

    std::string fxPath = GetEffectsPath() + "/" + effectName + ".fx";
    if (!std::filesystem::exists(fxPath)) {
        SPDLOG_ERROR("ReShade: Effect file not found: {}", fxPath);
        return false;
    }

    SPDLOG_INFO("ReShade: Compiling effect '{}'...", effectName);

    // Set up preprocessor with standard ReShade macros
    reshadefx::preprocessor pp;
    pp.add_macro_definition("__RESHADE__", "60000");
    pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "1");
    pp.add_macro_definition("BUFFER_WIDTH", std::to_string(sState.texWidth > 0 ? sState.texWidth : 1280));
    pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(sState.texHeight > 0 ? sState.texHeight : 720));
    pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");

    // Add include paths for ReShade shader headers
    std::string shadersPath = GetEffectsPath();
    pp.add_include_path(shadersPath);
    // Standard ReShade shader repo layout: reshade-shaders/Shaders/ and reshade-shaders/Textures/
    std::string parentDir = shadersPath + "/..";
    if (std::filesystem::exists(parentDir)) {
        pp.add_include_path(parentDir);
    }

    if (!pp.append_file(fxPath)) {
        SPDLOG_ERROR("ReShade: Preprocessor failed for '{}': {}", effectName, pp.errors());
        return false;
    }

    // Create GLSL codegen (OpenGL semantics, no debug info, no spec constants)
    std::unique_ptr<reshadefx::codegen> backend(reshadefx::create_codegen_glsl(false, false, false));

    reshadefx::parser parser;
    if (!parser.parse(pp.output(), backend.get())) {
        SPDLOG_ERROR("ReShade: Parse failed for '{}': {}{}", effectName, pp.errors(), parser.errors());
        return false;
    }

    reshadefx::effect_module& mod = backend->module();
    effect.name = effectName;
    effect.uniforms = mod.uniforms;

    // Initialize uniform data buffer with default values
    effect.uniformData.resize(mod.total_uniform_size, 0);
    for (const auto& u : mod.uniforms) {
        if (u.has_initializer_value && u.offset + u.size <= effect.uniformData.size()) {
            std::memcpy(effect.uniformData.data() + u.offset, &u.initializer_value, u.size);
        }
    }

    // Compile each technique's passes
    for (const auto& technique : mod.techniques) {
        for (const auto& passInfo : technique.passes) {
            CompiledPass compiledPass;

            // Assemble vertex shader GLSL for this entry point
            std::string vsSrc, vsAsm, vsErr;
            if (!passInfo.vs_entry_point.empty()) {
                if (!backend->assemble_code_for_entry_point(passInfo.vs_entry_point, vsSrc, vsAsm, vsErr)) {
                    SPDLOG_ERROR("ReShade: VS assembly failed for '{}': {}", passInfo.vs_entry_point, vsErr);
                    FreeEffect(effect);
                    return false;
                }
            }

            // Assemble fragment shader GLSL for this entry point
            std::string fsSrc, fsAsm, fsErr;
            if (!passInfo.ps_entry_point.empty()) {
                if (!backend->assemble_code_for_entry_point(passInfo.ps_entry_point, fsSrc, fsAsm, fsErr)) {
                    SPDLOG_ERROR("ReShade: FS assembly failed for '{}': {}", passInfo.ps_entry_point, fsErr);
                    FreeEffect(effect);
                    return false;
                }
            }

            // Patch GLSL for the target platform (version, binding qualifiers)
            vsSrc = PatchGLSLForPlatform(vsSrc);
            fsSrc = PatchGLSLForPlatform(fsSrc);

            // Compile GL shaders
            GLuint vs = CompileGLShader(GL_VERTEX_SHADER, vsSrc.c_str());
            GLuint fs = CompileGLShader(GL_FRAGMENT_SHADER, fsSrc.c_str());
            if (!vs || !fs) {
                if (vs)
                    glDeleteShader(vs);
                if (fs)
                    glDeleteShader(fs);
                SPDLOG_ERROR("ReShade: Shader compilation failed for effect '{}'", effectName);
                FreeEffect(effect);
                return false;
            }

            compiledPass.program = LinkGLProgram(vs, fs);
            glDeleteShader(vs);
            glDeleteShader(fs);

            if (!compiledPass.program) {
                FreeEffect(effect);
                return false;
            }

            // Set up sampler uniform locations
            glUseProgram(compiledPass.program);
            for (size_t s = 0; s < passInfo.sampler_bindings.size(); s++) {
                if (passInfo.sampler_bindings[s].index < mod.samplers.size()) {
                    const auto& sampler = mod.samplers[passInfo.sampler_bindings[s].index];
                    GLint loc = glGetUniformLocation(compiledPass.program, sampler.unique_name.c_str());
                    if (loc >= 0) {
                        glUniform1i(loc, static_cast<GLint>(passInfo.sampler_bindings[s].entry_point_binding));
                    }
                }
            }

            effect.passes.push_back(std::move(compiledPass));
        }
    }

    // Create UBO for uniform variables
    if (mod.total_uniform_size > 0) {
        glGenBuffers(1, &effect.uniformBuffer);
        glBindBuffer(GL_UNIFORM_BUFFER, effect.uniformBuffer);
        glBufferData(GL_UNIFORM_BUFFER, mod.total_uniform_size, effect.uniformData.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    SPDLOG_INFO("ReShade: Successfully compiled effect '{}' ({} passes)", effectName, effect.passes.size());
    return true;
}

// ─── GL resource management ─────────────────────────────────────────────────

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
    if (sState.initialized)
        return;

    glGenVertexArrays(1, &sState.vao);
    glGenFramebuffers(1, &sState.fbo);

    sState.initialized = true;
    ScanAvailableEffects();
    SPDLOG_INFO("ReShade PostProcess: GL resources initialized");
}

static void CleanupGL() {
    if (!sState.initialized)
        return;

    FreeEffect(sState.currentEffect);

    for (int i = 0; i < 2; i++) {
        if (sState.textures[i]) {
            glDeleteTextures(1, &sState.textures[i]);
            sState.textures[i] = 0;
        }
    }
    if (sState.fbo) {
        glDeleteFramebuffers(1, &sState.fbo);
        sState.fbo = 0;
    }
    if (sState.vao) {
        glDeleteVertexArrays(1, &sState.vao);
        sState.vao = 0;
    }
    sState.texWidth = 0;
    sState.texHeight = 0;
    sState.initialized = false;
    sState.loadedEffectName.clear();
    SPDLOG_INFO("ReShade PostProcess: GL resources cleaned up");
}

// ─── Post-process callback ──────────────────────────────────────────────────

static uintptr_t PostProcessCallback(uintptr_t texId, uint32_t width, uint32_t height) {
    InitGL();
    EnsureTextures(width, height);

    if (!CVarGetInteger(CVAR_PP_ENABLED, 0)) {
        return texId;
    }

    int effectIdx = CVarGetInteger(CVAR_PP_EFFECT, 0);
    if (effectIdx < 0 || effectIdx >= static_cast<int>(sState.availableEffects.size())) {
        return texId;
    }

    const std::string& effectName = sState.availableEffects[effectIdx];

    // Compile effect lazily on render thread (requires GL context)
    if (sState.loadedEffectName != effectName || sState.currentEffect.passes.empty()) {
        sState.texWidth = width;
        sState.texHeight = height;
        if (!CompileEffect(effectName, sState.currentEffect)) {
            return texId;
        }
        sState.loadedEffectName = effectName;
    }

    if (sState.currentEffect.passes.empty()) {
        return texId;
    }

    // Save GL state
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

    // Upload uniform data
    if (sState.currentEffect.uniformBuffer && !sState.currentEffect.uniformData.empty()) {
        glBindBuffer(GL_UNIFORM_BUFFER, sState.currentEffect.uniformBuffer);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sState.currentEffect.uniformData.size(),
                        sState.currentEffect.uniformData.data());
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    // Execute passes
    for (const auto& pass : sState.currentEffect.passes) {
        if (!pass.program)
            continue;

        GLuint outputTex = sState.textures[pingPongIdx];

        glBindFramebuffer(GL_FRAMEBUFFER, sState.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTex, 0);
        glViewport(0, 0, width, height);

        glUseProgram(pass.program);

        // Bind input texture (backbuffer from previous pass)
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, currentTex);

        // Bind uniform buffer
        if (sState.currentEffect.uniformBuffer) {
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, sState.currentEffect.uniformBuffer);
        }

        glBindVertexArray(sState.vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

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
    if (prevDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (prevBlend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);

    return (uintptr_t)currentTex;
}

// ─── Registration ───────────────────────────────────────────────────────────

static void RegisterPostProcess() {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
    if (!wnd)
        return;
    auto interp = wnd->GetInterpreterWeak().lock();
    if (!interp)
        return;

    if (CVarGetInteger(CVAR_PP_ENABLED, 0)) {
        interp->SetPostProcessCallback(PostProcessCallback);
    } else {
        interp->ClearPostProcessCallback();
        CleanupGL();
    }
}

static RegisterShipInitFunc initFunc(RegisterPostProcess, { CVAR_PP_ENABLED, CVAR_PP_EFFECT });

// ─── Menu UI ────────────────────────────────────────────────────────────────

void PostProcess_RenderMenuOptions() {
    ImGui::SeparatorText("Post-Processing (ReShade)");
    UIWidgets::CVarCheckbox(
        "Enable ReShade Effects", CVAR_PP_ENABLED,
        UIWidgets::CheckboxOptions().Tooltip("Enable post-processing effects using the ReShade FX shader system.\n"
                                             "Place .fx shader files in the reshade-shaders/Shaders/ directory."));

    if (CVarGetInteger(CVAR_PP_ENABLED, 0)) {
        if (sState.availableEffects.empty()) {
            ImGui::TextWrapped("No .fx files found in reshade-shaders/Shaders/");
            ImGui::TextWrapped("Download ReShade shaders from https://github.com/crosire/reshade-shaders");
        } else {
            int effectIdx = CVarGetInteger(CVAR_PP_EFFECT, 0);
            if (effectIdx >= static_cast<int>(sState.availableEffects.size())) {
                effectIdx = 0;
            }

            if (ImGui::BeginCombo("Effect", sState.availableEffects[effectIdx].c_str())) {
                for (int i = 0; i < static_cast<int>(sState.availableEffects.size()); i++) {
                    bool isSelected = (i == effectIdx);
                    if (ImGui::Selectable(sState.availableEffects[i].c_str(), isSelected)) {
                        CVarSetInteger(CVAR_PP_EFFECT, i);
                        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            if (ImGui::Button("Rescan Effects")) {
                ScanAvailableEffects();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload Effect")) {
                sState.loadedEffectName.clear();
            }
        }
    }
}

#else // !ENABLE_OPENGL

#include "PostProcess.h"
void PostProcess_RenderMenuOptions() {
}

#endif // ENABLE_OPENGL
