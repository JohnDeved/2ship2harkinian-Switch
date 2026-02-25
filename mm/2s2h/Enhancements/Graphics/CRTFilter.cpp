// CRT Filter — Dynamic post-processing using RetroArch GLSL CRT shaders.
// Loads shaders at runtime from the glsl-shaders submodule, allowing the user
// to pick any .glsl CRT shader from the menu.
//
// Shader format: RetroArch combined VERTEX/FRAGMENT with #pragma parameter.
// https://github.com/libretro/glsl-shaders/tree/master/crt
//
// Performance notes:
//   - Single fullscreen triangle (1 draw call, 3 verts, no index buffer).
//   - No glGet* state queries — known call-site state is restored directly.
//   - Shader compiled once per selection; recompiled only on shader change.
//   - FBO resized only on dimension change.

#ifdef ENABLE_OPENGL

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

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
#define CVAR_CRT_SHADER "gEnhancements.Graphics.CRTFilter.Shader"

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
// Fragment compat header: maps old GLSL keywords to GLES 300 es equivalents.
// This allows RetroArch shaders using varying/texture2D/gl_FragColor to compile.
static const char* sFragCompatHeader =
    "#version 300 es\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "#define varying in\n"
    "#define texture2D texture\n"
    "out vec4 _fragColor_;\n"
    "#define gl_FragColor _fragColor_\n";
#elif defined(__APPLE__)
static const char* sGlslHeader = "#version 410 core\n";
static const char* sFragCompatHeader =
    "#version 410 core\n"
    "#define varying in\n"
    "#define texture2D texture\n"
    "out vec4 _fragColor_;\n"
    "#define gl_FragColor _fragColor_\n";
#else
static const char* sGlslHeader = "#version 130\n";
// GLSL 130 supports both old and new style — no compat needed.
static const char* sFragCompatHeader = "#version 130\n";
#endif

// ---------------------------------------------------------------------------
// Our own vertex shader — fullscreen triangle, provides TEX0 like RetroArch.
// We override the RetroArch vertex section with this for performance
// (avoids MVPMatrix uniform and uses a 3-vert fullscreen triangle).
// ---------------------------------------------------------------------------
static const char* sVertSource = R"(
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
// Parsed parameter from #pragma parameter lines
// ---------------------------------------------------------------------------
struct ShaderParam {
    std::string name;
    std::string description;
    float defaultValue;
    float minValue;
    float maxValue;
    float step;
    GLint uniformLoc = -1;
};

// ---------------------------------------------------------------------------
// State for the currently loaded shader
// ---------------------------------------------------------------------------
struct LoadedShader {
    GLuint program = 0;
    GLint locOutputSize = -1;
    GLint locTextureSize = -1;
    GLint locInputSize = -1;
    GLint locTexture = -1;
    GLint locFrameCount = -1;
    GLint locFrameDirection = -1;
    std::vector<ShaderParam> params;
    std::string name;
};

static LoadedShader sActiveShader;
static GLuint sCrtVao = 0;
static GLuint sCrtVbo = 0;
static GLuint sCrtFbo = 0;
static GLuint sCrtTexture = 0;
static uint32_t sCrtTexWidth = 0;
static uint32_t sCrtTexHeight = 0;
static int sFrameCount = 0;

// Shader file list — populated once at init
static std::vector<std::string> sShaderNames; // display names
static std::vector<std::string> sShaderPaths; // full paths
static bool sShaderListInitialized = false;

// Persistent const char* list for BenMenu combobox (pointers into sShaderNames)
static std::vector<const char*> sShaderNamePtrs;

// Track which shader is currently compiled to avoid recompilation
static int32_t sCompiledShaderIndex = -1;

// ---------------------------------------------------------------------------
// Find the shader directory
// ---------------------------------------------------------------------------
static std::string GetShaderDir() {
    std::string bundlePath = Ship::Context::GetAppBundlePath();
    return bundlePath + "/glsl-shaders/crt/shaders";
}

// ---------------------------------------------------------------------------
// Scan for available .glsl shader files (single-pass shaders only)
// ---------------------------------------------------------------------------
static void ScanShaderFiles() {
    if (sShaderListInitialized)
        return;
    sShaderListInitialized = true;

    std::string dir = GetShaderDir();
    if (!std::filesystem::exists(dir))
        return;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        if (ext != ".glsl")
            continue;

        // Only include single-file shaders (must contain both VERTEX and FRAGMENT sections)
        std::ifstream f(entry.path());
        if (!f.is_open())
            continue;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (content.find("defined(VERTEX)") == std::string::npos ||
            content.find("defined(FRAGMENT)") == std::string::npos)
            continue;

        sShaderPaths.push_back(entry.path().string());
        sShaderNames.push_back(entry.path().stem().string());
    }

    // Sort alphabetically
    std::vector<size_t> indices(sShaderNames.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(),
              [](size_t a, size_t b) { return sShaderNames[a] < sShaderNames[b]; });

    std::vector<std::string> sortedNames, sortedPaths;
    sortedNames.reserve(indices.size());
    sortedPaths.reserve(indices.size());
    for (size_t i : indices) {
        sortedNames.push_back(std::move(sShaderNames[i]));
        sortedPaths.push_back(std::move(sShaderPaths[i]));
    }
    sShaderNames = std::move(sortedNames);
    sShaderPaths = std::move(sortedPaths);

    // Build persistent const char* list for BenMenu combobox
    sShaderNamePtrs.clear();
    sShaderNamePtrs.reserve(sShaderNames.size());
    for (const auto& name : sShaderNames) {
        sShaderNamePtrs.push_back(name.c_str());
    }
}

// ---------------------------------------------------------------------------
// Parse #pragma parameter lines from shader source
// Format: #pragma parameter NAME "DESCRIPTION" DEFAULT MIN MAX STEP
// ---------------------------------------------------------------------------
static std::vector<ShaderParam> ParsePragmaParams(const std::string& source) {
    std::vector<ShaderParam> params;
    std::istringstream stream(source);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.find("#pragma parameter") == std::string::npos)
            continue;

        // Find the parameter name (first token after "#pragma parameter")
        size_t pos = line.find("parameter") + 9;
        while (pos < line.size() && line[pos] == ' ')
            pos++;

        std::istringstream ls(line.substr(pos));
        ShaderParam p;
        ls >> p.name;

        // Parse description in quotes
        size_t q1 = line.find('"', pos);
        if (q1 == std::string::npos)
            continue;
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos)
            continue;
        p.description = line.substr(q1 + 1, q2 - q1 - 1);

        // Parse numeric values after the closing quote
        std::istringstream nums(line.substr(q2 + 1));
        if (!(nums >> p.defaultValue >> p.minValue >> p.maxValue >> p.step))
            continue;

        params.push_back(std::move(p));
    }

    return params;
}

// ---------------------------------------------------------------------------
// Extract the FRAGMENT section from a RetroArch combined shader
// ---------------------------------------------------------------------------
static std::string ExtractFragmentSection(const std::string& source) {
    // Find "#elif defined(FRAGMENT)" or "#if defined(FRAGMENT)"
    size_t fragStart = source.find("#elif defined(FRAGMENT)");
    if (fragStart == std::string::npos)
        fragStart = source.find("#if defined(FRAGMENT)");
    if (fragStart == std::string::npos)
        return "";

    // Skip the #elif/#if line itself
    size_t lineEnd = source.find('\n', fragStart);
    if (lineEnd == std::string::npos)
        return "";
    std::string fragBody = source.substr(lineEnd + 1);

    // Remove trailing #endif if present
    size_t lastEndif = fragBody.rfind("#endif");
    if (lastEndif != std::string::npos) {
        fragBody = fragBody.substr(0, lastEndif);
    }

    return fragBody;
}

// ---------------------------------------------------------------------------
// GL helpers
// ---------------------------------------------------------------------------
static GLuint CompileShader(GLenum type, const char* header, const std::string& body) {
    GLuint shader = glCreateShader(type);
    const char* sources[2] = { header, body.c_str() };
    glShaderSource(shader, 2, sources, nullptr);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "CRT %s compile error:\n%s\n", type == GL_VERTEX_SHADER ? "VS" : "FS", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static void DestroyActiveShader() {
    if (sActiveShader.program) {
        glDeleteProgram(sActiveShader.program);
    }
    sActiveShader = {};
    sCompiledShaderIndex = -1;
}

// ---------------------------------------------------------------------------
// Load and compile a RetroArch CRT shader by index
// ---------------------------------------------------------------------------
static bool LoadShader(int index) {
    if (index < 0 || index >= (int)sShaderPaths.size())
        return false;
    if (sCompiledShaderIndex == index && sActiveShader.program)
        return true;

    // Read file
    std::ifstream f(sShaderPaths[index]);
    if (!f.is_open()) {
        fprintf(stderr, "CRT: Failed to open shader file: %s\n", sShaderPaths[index].c_str());
        return false;
    }
    std::string source((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Parse parameters and extract fragment section
    auto params = ParsePragmaParams(source);
    std::string fragBody = ExtractFragmentSection(source);
    if (fragBody.empty()) {
        fprintf(stderr, "CRT: No FRAGMENT section found in: %s\n", sShaderPaths[index].c_str());
        return false;
    }

    // Build fragment source: PARAMETER_UNIFORM define + shader body.
    // The shader's own fragment section already contains its compat defines,
    // uniform declarations, and varying declarations.
    std::string fragSource;
    fragSource += "#define PARAMETER_UNIFORM\n";
    fragSource += fragBody;

    // Determine which header to use: if the shader has its own compat system
    // (uses COMPAT_VARYING or #if __VERSION__), use the plain header.
    // Otherwise use the compat header that maps old-style keywords.
    bool hasOwnCompat =
        (fragBody.find("COMPAT_VARYING") != std::string::npos || fragBody.find("__VERSION__") != std::string::npos);
    const char* fragHeader = hasOwnCompat ? sGlslHeader : sFragCompatHeader;

    // Destroy old shader
    DestroyActiveShader();

    // Compile
    GLuint vs = CompileShader(GL_VERTEX_SHADER, sGlslHeader, sVertSource);
    if (!vs)
        return false;

    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragHeader, fragSource);
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
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "CRT link error (%s):\n%s\n", sShaderNames[index].c_str(), log);
        glDeleteProgram(prog);
        return false;
    }

    // Cache uniform locations
    sActiveShader.program = prog;
    sActiveShader.name = sShaderNames[index];
    sActiveShader.locOutputSize = glGetUniformLocation(prog, "OutputSize");
    sActiveShader.locTextureSize = glGetUniformLocation(prog, "TextureSize");
    sActiveShader.locInputSize = glGetUniformLocation(prog, "InputSize");
    sActiveShader.locTexture = glGetUniformLocation(prog, "Texture");
    sActiveShader.locFrameCount = glGetUniformLocation(prog, "FrameCount");
    sActiveShader.locFrameDirection = glGetUniformLocation(prog, "FrameDirection");

    // Cache parameter uniform locations
    sActiveShader.params = std::move(params);
    for (auto& p : sActiveShader.params) {
        p.uniformLoc = glGetUniformLocation(prog, p.name.c_str());
    }

    sCompiledShaderIndex = index;
    return true;
}

// ---------------------------------------------------------------------------
// Geometry init (once)
// ---------------------------------------------------------------------------
static bool InitGeometry() {
    if (sCrtVao)
        return true;

    static const float kTriVerts[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };

    glGenVertexArrays(1, &sCrtVao);
    glGenBuffers(1, &sCrtVbo);

    glBindVertexArray(sCrtVao);
    glBindBuffer(GL_ARRAY_BUFFER, sCrtVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kTriVerts), kTriVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
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
// ---------------------------------------------------------------------------
static uintptr_t CRTPostProcess(uintptr_t inputTexId, uint32_t width, uint32_t height) {
    if (!CVarGetInteger(CVAR_CRT_ENABLED, 0))
        return inputTexId;

    ScanShaderFiles();
    if (sShaderPaths.empty())
        return inputTexId;

    int shaderIdx = CVarGetInteger(CVAR_CRT_SHADER, 0);
    if (shaderIdx < 0 || shaderIdx >= (int)sShaderPaths.size())
        shaderIdx = 0;

    if (!InitGeometry())
        return inputTexId;
    if (!LoadShader(shaderIdx))
        return inputTexId;

    EnsureFboSize(width, height);
    sFrameCount++;

    GLuint prog = sActiveShader.program;

    // --- Render CRT pass ---
    glBindFramebuffer(GL_FRAMEBUFFER, sCrtFbo);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)inputTexId);
    glUniform1i(sActiveShader.locTexture, 0);

    float w_f = (float)width;
    float h_f = (float)height;
    glUniform2f(sActiveShader.locOutputSize, w_f, h_f);
    glUniform2f(sActiveShader.locTextureSize, w_f, h_f);
    glUniform2f(sActiveShader.locInputSize, w_f, h_f);
    if (sActiveShader.locFrameCount >= 0)
        glUniform1i(sActiveShader.locFrameCount, sFrameCount);
    if (sActiveShader.locFrameDirection >= 0)
        glUniform1i(sActiveShader.locFrameDirection, 1);

    // Set all parameters to their default values
    for (const auto& p : sActiveShader.params) {
        if (p.uniformLoc >= 0)
            glUniform1f(p.uniformLoc, p.defaultValue);
    }

    glBindVertexArray(sCrtVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // --- Restore state ---
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return (uintptr_t)sCrtTexture;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
static RegisterShipInitFunc initFunc(
    []() {
        // Scan shaders early so the menu can show the dropdown
        ScanShaderFiles();
        auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
        if (wnd) {
            auto interp = wnd->GetInterpreterWeak().lock();
            if (interp) {
                interp->SetPostProcessCallback(CRTPostProcess);
            }
        }
    },
    {});

// ---------------------------------------------------------------------------
// Public API for BenMenu
// ---------------------------------------------------------------------------
const std::vector<const char*>* CRTFilter_GetShaderNames() {
    ScanShaderFiles();
    return &sShaderNamePtrs;
}

#else // !ENABLE_OPENGL

const std::vector<const char*>* CRTFilter_GetShaderNames() {
    static std::vector<const char*> empty;
    return &empty;
}

#endif // ENABLE_OPENGL
