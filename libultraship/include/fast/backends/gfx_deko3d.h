#ifdef ENABLE_DEKO3D
#pragma once

#include "gfx_rendering_api.h"
#include "../interpreter.h"

#include <deko3d.hpp>
#include <switch.h>
#include <imgui.h>

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <stack>

namespace Fast {

// Per-shader program data for deko3d
struct ShaderProgram {
    uint8_t numInputs;
    bool usedTextures[SHADER_MAX_TEXTURES];
    uint8_t numFloats;
    uint8_t attribSizes[16];
    uint8_t numAttribs;
    // Combiner features needed to set uniforms at draw time
    bool opt_alpha;
    bool opt_fog;
    bool opt_grayscale;
    bool opt_noise;
    bool opt_2cyc;
    bool opt_texture_edge;
    bool opt_alpha_threshold;
    bool opt_invisible;
    bool clamp[2][2];
    bool used_masks[2];
    bool used_blend[2];
    bool color_alpha_same[2];
    bool do_single[2][2];
    bool do_multiply[2][2];
    bool do_mix[2][2];
    int c[2][2][4];
};

struct FramebufferDk {
    uint32_t width, height;
    bool has_depth_buffer;
    uint32_t msaa_level;
    bool invertY;
    dk::Image colorImage;
    dk::Image depthImage;
    dk::UniqueMemBlock colorMem;
    dk::UniqueMemBlock depthMem;
    uint32_t descriptorIdx = 0; // Index into the image/sampler descriptor pool
};

// Uniforms layout for the uber-shader (must match GLSL std140 layout exactly).
// All fields use ivec4/ivec2/int/float aligned to avoid std140 padding surprises.
struct alignas(DK_UNIFORM_BUF_ALIGNMENT) UberUniforms {
    // Color combiner terms: cc_CYC_TYPE[a,b,c,d]
    // cc[0][0] = cycle 0 RGB, cc[0][1] = cycle 0 Alpha
    // cc[1][0] = cycle 1 RGB, cc[1][1] = cycle 1 Alpha
    int32_t cc_0_0[4]; // ivec4 cc_0_0
    int32_t cc_0_1[4]; // ivec4 cc_0_1
    int32_t cc_1_0[4]; // ivec4 cc_1_0
    int32_t cc_1_1[4]; // ivec4 cc_1_1

    // Texture/mask/blend usage flags (packed as ivec2)
    int32_t useTexture[2]; // ivec2
    int32_t useMask[2];    // ivec2
    int32_t useBlend[2];   // ivec2

    // Clamp flags: [tex0_s, tex0_t, tex1_s, tex1_t] (packed as ivec4)
    int32_t useClamp[4]; // ivec4

    // Feature flags (individual ints)
    int32_t optAlpha;
    int32_t optFog;
    int32_t optNoise;
    int32_t opt2Cyc;
    int32_t optTextureEdge;
    int32_t optAlphaThreshold;
    int32_t optInvisible;
    int32_t optGrayscale;

    // Operation mode flags: [cyc0_rgb, cyc0_a, cyc1_rgb, cyc1_a] (packed as ivec4)
    int32_t doSingle[4];   // ivec4
    int32_t doMultiply[4]; // ivec4
    int32_t doMix[4];      // ivec4

    // Color/alpha same per cycle (packed as ivec2)
    int32_t colorAlphaSame[2]; // ivec2

    // Texture info (packed as ivec2)
    int32_t textureFiltering[2]; // ivec2
    int32_t textureWidth[2];     // ivec2
    int32_t textureHeight[2];    // ivec2

    // Misc
    int32_t frameCount;
    float noiseScale;
    int32_t srgbMode;
    int32_t pad0;
};

// Hash for shader program pool key
struct HashPairShaderIds {
    size_t operator()(const std::pair<uint64_t, uint32_t>& p) const {
        size_t h = std::hash<uint64_t>{}(p.first);
        h ^= std::hash<uint32_t>{}(p.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// Constants
static constexpr unsigned NUM_FRAMEBUFFERS = 2;
static constexpr unsigned CMDBUF_SIZE = 1024 * 1024;         // 1 MB command buffer
static constexpr unsigned VBO_POOL_SIZE = 4 * 1024 * 1024;   // 4 MB vertex pool
static constexpr unsigned UNIFORM_POOL_SIZE = 256 * 1024;    // 256 KB uniform pool
static constexpr unsigned CODE_POOL_SIZE = 512 * 1024;       // 512 KB shader code pool
static constexpr unsigned MAX_DESCRIPTORS = 4096;             // Max image/sampler descriptors

class GfxRenderingAPIDeko3d final : public GfxRenderingAPI {
  public:
    GfxRenderingAPIDeko3d();
    ~GfxRenderingAPIDeko3d() override;

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
    void ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;

    // deko3d-specific accessors for ImGui integration
    dk::Device GetDevice() const { return mDevice; }
    dk::Queue GetQueue() const { return mQueue; }

    // ImGui draw data rendering
    void RenderImGuiDrawData(ImDrawData* drawData);

  private:
    void InitDevice();
    void InitSwapchain();
    void InitDescriptorPools();
    void InitShaders();
    void FlushCommands();
    void BindCurrentFramebuffer();
    void ConfigureVertexState();

    // Device and queue
    dk::UniqueDevice mDevice;
    dk::UniqueQueue mQueue;

    // Swapchain
    NWindow* mNWindow = nullptr;
    dk::UniqueSwapchain mSwapchain;
    dk::Image mSwapchainImages[NUM_FRAMEBUFFERS];
    dk::UniqueMemBlock mSwapchainMem;
    dk::Image mSwapchainDepthImage;
    dk::UniqueMemBlock mSwapchainDepthMem;
    int mCurrentSwapImage = -1;

    // Memory pools
    dk::UniqueMemBlock mCodeMem;
    dk::UniqueMemBlock mVboMem;
    dk::UniqueMemBlock mUniformMem;

    // Command buffer
    dk::UniqueCmdBuf mCmdBuf;
    dk::UniqueMemBlock mCmdBufMem;

    // Shaders
    dk::Shader mVertexShader;
    dk::Shader mFragmentShader;
    bool mShadersLoaded = false;

    // Shader programs (combiner configurations)
    std::unordered_map<std::pair<uint64_t, uint32_t>, ShaderProgram, HashPairShaderIds> mShaderProgramPool;
    ShaderProgram* mCurrentShaderProgram = nullptr;

    // Descriptor pools (GPU-visible memory for image/sampler descriptors)
    dk::UniqueMemBlock mImageDescMem;
    dk::UniqueMemBlock mSamplerDescMem;
    DkImageDescriptor* mImageDescriptors = nullptr;
    DkSamplerDescriptor* mSamplerDescriptors = nullptr;

    // Textures
    struct TextureData {
        dk::Image image;
        dk::UniqueMemBlock mem;
        uint16_t width;
        uint16_t height;
        uint16_t filtering;
        uint32_t uniformsVersion;
        uint32_t descriptorIdx;
        bool valid;
    };
    std::vector<TextureData> mTextures;
    uint32_t mNextTextureId = 1;
    uint32_t mCurrentTextureIds[SHADER_MAX_TEXTURES]{};
    uint8_t mCurrentTile = 0;
    std::stack<uint32_t> mFreeDescriptorIndices; // Recycled descriptor pool indices

    // Per-frame fence for GPU/CPU sync without waitIdle on EndFrame
    dk::Fence mFrameFence;

    // VBO state
    size_t mVboOffset = 0;

    // Uniform state
    size_t mUniformOffset = 0;
    uint32_t mFrameCount = 0;

    // Framebuffers
    std::vector<FramebufferDk> mFrameBuffers;
    size_t mCurrentFrameBuffer = 0;
    float mCurrentNoiseScale = 0.0f;
    FilteringMode mCurrentFilterMode = FILTER_THREE_POINT;

    // Render state tracking
    bool mCurrentDepthTest = false;
    bool mCurrentDepthMask = false;
    bool mCurrentZmodeDecal = false;
    bool mLastDepthTest = false;
    bool mLastDepthMask = false;
    bool mLastZmodeDecal = false;
    bool mSrgbMode = false;
};

} // namespace Fast
#endif
