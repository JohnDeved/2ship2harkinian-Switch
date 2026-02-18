#ifdef ENABLE_OPENGL
#pragma once

#include "gfx_rendering_api.h"
#include "../interpreter.h"

#ifdef _MSC_VER
#include <SDL2/SDL.h>
// #define GL_GLEXT_PROTOTYPES 1
#include <GL/glew.h>
#elif FOR_WINDOWS
#include <GL/glew.h>
#include "SDL.h"
#define GL_GLEXT_PROTOTYPES 1
#include "SDL_opengl.h"
#elif __APPLE__
#include <SDL2/SDL.h>
#include <GL/glew.h>
#elif USE_OPENGLES
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>
#else
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengl.h>
#endif
namespace Fast {
struct ShaderProgram {
    GLuint openglProgramId;
    uint8_t numInputs;
    bool usedTextures[SHADER_MAX_TEXTURES];
    uint8_t numFloats;
    GLint attribLocations[16];
    uint8_t attribSizes[16];
    uint8_t numAttribs;
    GLint frameCountLocation;
    GLint noiseScaleLocation;
    GLint texture_width_location;
    GLint texture_height_location;
    GLint texture_filtering_location;
    GLint celEnabledLocation;
    GLint celBandsLocation;
    GLint celSoftnessLocation;
    GLint tonemappingEnabledLocation;
    GLint colorTempLocation;
    GLint brightnessLocation;
    GLint contrastLocation;
    GLint filmGrainLocation;
    GLint shadowTintIntensityLocation;
    GLint shadowTintMixLocation;
    GLint rimIntensityLocation;
    GLint bloomThresholdLocation;
    GLint bloomIntensityLocation;
    GLint outlineIntensityLocation;
    GLint vignetteLocation;
    GLint specularIntensityLocation;
    GLint subsurfaceIntensityLocation;
    GLint micronormalIntensityLocation;
    GLint sharpeningLocation;
    GLint hemiAmbientIntensityLocation;
    GLint hemiAmbientSkyRLocation;
    GLint hemiAmbientSkyGLocation;
    GLint hemiAmbientSkyBLocation;
    GLint hemiAmbientGroundRLocation;
    GLint hemiAmbientGroundGLocation;
    GLint hemiAmbientGroundBLocation;
    GLint saturationLocation;
    GLint viewportWidthLocation;
    GLint viewportHeightLocation;
#if defined(__SWITCH__) || defined(USE_OPENGLES)
    GLuint vao; // Per-shader VAO: configured once, bound on shader switch
#endif
};

struct FramebufferOGL {
    uint32_t width, height;
    bool has_depth_buffer;
    uint32_t msaa_level;
    bool invertY;

    GLuint fbo, clrbuf, clrbufMsaa, rbo;
    GLuint depthTex = 0; // Depth texture for post-processing reads
};

// Post-processing pipeline resources
struct PostProcessOGL {
    bool initialized = false;
    GLuint quadVao = 0;
    GLuint quadVbo = 0;

    // SSAO pass
    GLuint ssaoProgram = 0;
    GLuint ssaoFbo = 0;
    GLuint ssaoTex = 0;

    // Bloom passes (downsample + blur)
    GLuint bloomExtractProgram = 0;
    GLuint bloomBlurProgram = 0;
    GLuint bloomComposeProgram = 0;
    GLuint bloomFbo[2] = {};
    GLuint bloomTex[2] = {};

    // Height fog pass
    GLuint fogProgram = 0;

    // Light shaft pass
    GLuint lightShaftProgram = 0;

    // Intermediate framebuffer for compositing
    GLuint compositeFbo = 0;
    GLuint compositeTex = 0;

    uint32_t fbWidth = 0;
    uint32_t fbHeight = 0;
};

// Hash for shader program pool key (same approach as Metal backend)
struct HashPairShaderIds {
    size_t operator()(const std::pair<uint64_t, uint32_t>& p) const {
        // Mix the two IDs with xor-shift; collisions are rare with shader ID pairs.
        size_t h = std::hash<uint64_t>{}(p.first);
        h ^= std::hash<uint32_t>{}(p.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class GfxRenderingAPIOGL final : public GfxRenderingAPI {
  public:
    ~GfxRenderingAPIOGL() override;
    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint32_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint32_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depth_test, bool z_upd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override;
    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                     bool can_extract_depth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;

  private:
    void SetUniforms(ShaderProgram* prg) const;
    std::string BuildFsShader(const CCFeatures& cc_features);
    void SetPerDrawUniforms();

    struct TextureInfo {
        uint16_t width;
        uint16_t height;
        uint16_t filtering;
        uint16_t pad;
        uint32_t uniformsVersion;
    } textures[1024]{};

    GLuint mCurrentTextureIds[SHADER_MAX_TEXTURES]{};
    uint8_t mCurrentTile;

    std::unordered_map<std::pair<uint64_t, uint32_t>, ShaderProgram, HashPairShaderIds> mShaderProgramPool;
    ShaderProgram* mCurrentShaderProgram;

    GLuint mOpenglVbo = 0;
#if defined(__APPLE__) || defined(USE_OPENGLES)
    GLuint mOpenglVao;
#endif

    // Per-iteration VBO batching: orphan once per DL iteration, then use
    // glBufferSubData + glDrawArrays(first=N) for each draw within the
    // iteration. Reduces ~320 glBufferData allocations per iteration to 1.
    static constexpr size_t VBO_ITER_SIZE = 2 * 1024 * 1024; // 2MB per iteration
    size_t mVboIterOffset = 0;  // Running byte offset within current iteration's VBO
    bool mVboIterActive = false; // True after orphaning for this iteration

    // Cache state to skip redundant SetPerDrawUniforms calls
    uint32_t mLastUniformTextureIds[2] = { UINT32_MAX, UINT32_MAX };
    uint32_t mLastUniformTextureVersions[2] = { UINT32_MAX, UINT32_MAX };

    uint32_t mFrameCount = 0;

    std::vector<FramebufferOGL> mFrameBuffers;
    size_t mCurrentFrameBuffer = 0;
    float mCurrentNoiseScale = 0.0f;
    FilteringMode mCurrentFilterMode = FILTER_THREE_POINT;

    int mShaderCelEnabled = 0;
    int mShaderCelBands = 3;
    float mShaderCelSoftness = 0.3f;
    int mShaderTonemappingEnabled = 0;
    float mShaderColorTemp = 0.0f;
    float mShaderBrightness = 0.0f;
    float mShaderContrast = 0.0f;
    float mShaderFilmGrain = 0.0f;
    float mShaderShadowTintIntensity = 0.0f;
    float mShaderShadowTintMix = 0.3f;
    float mShaderRimIntensity = 0.0f;
    float mShaderBloomThreshold = 0.7f;
    float mShaderBloomIntensity = 0.0f;
    float mShaderOutlineIntensity = 0.0f;
    float mShaderVignette = 0.0f;
    float mShaderSpecularIntensity = 0.0f;
    float mShaderSubsurfaceIntensity = 0.0f;
    float mShaderMicronormalIntensity = 0.0f;
    float mShaderSharpening = 0.0f;
    float mShaderHemiAmbientIntensity = 0.0f;
    float mShaderHemiAmbientSkyR = 0.6f;
    float mShaderHemiAmbientSkyG = 0.7f;
    float mShaderHemiAmbientSkyB = 1.0f;
    float mShaderHemiAmbientGroundR = 0.4f;
    float mShaderHemiAmbientGroundG = 0.3f;
    float mShaderHemiAmbientGroundB = 0.2f;
    float mShaderSaturation = 0.0f;
    float mShaderViewportWidth = 0.0f;
    float mShaderViewportHeight = 0.0f;

    // Post-processing CVars
    float mPPSsaoIntensity = 0.0f;
    float mPPSsaoRadius = 0.5f;
    float mPPBloomBlurIntensity = 0.0f;
    float mPPBloomBlurThreshold = 0.7f;
    float mPPFogIntensity = 0.0f;
    float mPPFogDensity = 0.02f;
    float mPPFogHeightFalloff = 0.1f;
    float mPPLightShaftIntensity = 0.0f;
    float mPPLightShaftDecay = 0.96f;
    float mPPLightShaftDensity = 0.5f;

    // Post-processing resources
    PostProcessOGL mPostProcess;
    int mGameFbId = 0; // Tracks which FB the game renders to

    void InitPostProcess();
    void RunPostProcess(int gameFbId);
    GLuint CompilePostProcessShader(const char* vertSrc, const char* fragSrc);
    void DrawFullscreenQuad();

    GLint mMaxMsaaLevel = 1;
    GLuint mPixelDepthRb = 0;
    GLuint mPixelDepthFb = 0;
    size_t mPixelDepthRbSize = 0;
};

} // namespace Fast
#endif
