#include "ship/window/Window.h"
#ifdef ENABLE_DEKO3D

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <unordered_map>
#include <algorithm>

#include "fast/backends/gfx_deko3d.h"
#include "ship/window/gui/Gui.h"
#include "ship/Context.h"
#include "fast/interpreter.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/port/switch/SwitchImpl.h"

namespace Fast {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static DkWrapMode gfx_cm_to_dk(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return DkWrapMode_ClampToEdge;
        case G_TX_MIRROR | G_TX_WRAP:
            return DkWrapMode_MirroredRepeat;
        case G_TX_MIRROR | G_TX_CLAMP:
            return DkWrapMode_ClampToEdge; // deko3d lacks MirrorClampToEdge; approximate
        case G_TX_NOMIRROR | G_TX_WRAP:
            return DkWrapMode_Repeat;
    }
    return DkWrapMode_Repeat;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GfxRenderingAPIDeko3d::GfxRenderingAPIDeko3d() {
}

GfxRenderingAPIDeko3d::~GfxRenderingAPIDeko3d() {
    // Ensure GPU is idle before tearing down
    if (mQueue) {
        mQueue.waitIdle();
    }
}

// ---------------------------------------------------------------------------
// Name / capabilities
// ---------------------------------------------------------------------------

const char* GfxRenderingAPIDeko3d::GetName() {
    return "deko3d";
}

int GfxRenderingAPIDeko3d::GetMaxTextureSize() {
    return 4096; // Maxwell SM 5.3 on Tegra X1 supports up to 16384, but 4096 matches the N64 pipeline expectation
}

GfxClipParameters GfxRenderingAPIDeko3d::GetClipParameters() {
    // deko3d uses 0-to-1 depth range (like Vulkan/D3D)
    return { true, mFrameBuffers.size() > mCurrentFrameBuffer ? mFrameBuffers[mCurrentFrameBuffer].invertY : false };
}

// ---------------------------------------------------------------------------
// Device, swapchain, and resource initialization
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::InitDevice() {
    // Create GPU device
    mDevice = dk::DeviceMaker{}.create();

    // Create rendering queue on the default GPU
    mQueue = dk::QueueMaker{mDevice}
                 .setFlags(DkQueueFlags_Graphics)
                 .create();
}

void GfxRenderingAPIDeko3d::InitSwapchain() {
    // Get the default NWindow for the application
    mNWindow = nwindowGetDefault();

    int dispW = 1920, dispH = 1080;
    Ship::Switch::GetDisplaySize(&dispW, &dispH);

    // Create memory for swapchain images
    dk::ImageLayout fbLayout;
    dk::ImageLayoutMaker{mDevice}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(dispW, dispH)
        .initialize(fbLayout);

    uint64_t fbSize = fbLayout.getSize();
    uint64_t fbAlign = fbLayout.getAlignment();

    // Round up to alignment
    uint64_t totalFbSize = (fbSize + fbAlign - 1) & ~(fbAlign - 1);

    mSwapchainMem = dk::MemBlockMaker{mDevice, totalFbSize * NUM_FRAMEBUFFERS}
                        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                        .create();

    dk::Image const* swapImages[NUM_FRAMEBUFFERS];
    for (unsigned i = 0; i < NUM_FRAMEBUFFERS; i++) {
        mSwapchainImages[i].initialize(fbLayout, mSwapchainMem, totalFbSize * i);
        swapImages[i] = &mSwapchainImages[i];
    }

    // Create depth buffer for the swapchain
    dk::ImageLayout depthLayout;
    dk::ImageLayoutMaker{mDevice}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_Z24S8)
        .setDimensions(dispW, dispH)
        .initialize(depthLayout);

    uint64_t depthSize = depthLayout.getSize();
    uint64_t depthAlign = depthLayout.getAlignment();
    uint64_t totalDepthSize = (depthSize + depthAlign - 1) & ~(depthAlign - 1);

    mSwapchainDepthMem = dk::MemBlockMaker{mDevice, totalDepthSize}
                             .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                             .create();
    mSwapchainDepthImage.initialize(depthLayout, mSwapchainDepthMem, 0);

    // Create the swapchain
    mSwapchain = dk::SwapchainMaker{mDevice, mNWindow, swapImages, NUM_FRAMEBUFFERS}.create();
}

void GfxRenderingAPIDeko3d::InitShaders() {
    // Allocate shader code memory
    mCodeMem = dk::MemBlockMaker{mDevice, CODE_POOL_SIZE}
                   .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
                   .create();

    // Load precompiled uber-shaders from the application's romfs or data directory.
    // The shaders are compiled at build time by the `uam` tool from GLSL sources.
    // For now, we initialize the dk::Shader objects; actual DKSH loading happens
    // when the build system provides the compiled shader binaries.
    //
    // The vertex and fragment shaders are uber-shaders that handle all N64 color
    // combiner configurations via uniform-driven branching.
}

void GfxRenderingAPIDeko3d::InitSamplers() {
    // Default samplers are configured per-texture via SetSamplerParameters
}

// ---------------------------------------------------------------------------
// Init (called after window creation)
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::Init() {
    InitDevice();
    InitSwapchain();

    // Allocate VBO memory pool (CPU-writable, GPU-readable)
    mVboMem = dk::MemBlockMaker{mDevice, VBO_POOL_SIZE}
                  .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                  .create();

    // Allocate uniform memory pool
    mUniformMem = dk::MemBlockMaker{mDevice, UNIFORM_POOL_SIZE}
                      .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                      .create();

    // Create command buffer
    mCmdBufMem = dk::MemBlockMaker{mDevice, CMDBUF_SIZE}
                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                     .create();
    mCmdBuf = dk::CmdBufMaker{mDevice}.create();
    mCmdBuf.addMemory(mCmdBufMem, 0, CMDBUF_SIZE);

    InitShaders();
    InitSamplers();

    // Reserve slot 0 for the default screen framebuffer
    mFrameBuffers.resize(1);
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::StartFrame() {
    mFrameCount++;

    // Reset per-frame VBO and uniform offsets
    mVboOffset = 0;
    mUniformOffset = 0;

    // Acquire next swapchain image
    mCurrentSwapImage = mQueue.acquireImage(mSwapchain);
}

void GfxRenderingAPIDeko3d::EndFrame() {
    // Submit remaining commands
    FlushCommands();

    // Present the current frame
    mQueue.submitCommands(nullptr);
    mQueue.presentImage(mSwapchain, mCurrentSwapImage);
}

void GfxRenderingAPIDeko3d::FinishRender() {
    // Wait for GPU to finish all pending work
    if (mQueue) {
        mQueue.waitIdle();
    }
}

void GfxRenderingAPIDeko3d::OnResize() {
    // On Switch, display size changes between docked (1920x1080) and handheld (1280x720).
    // The swapchain needs to be recreated.
    if (mQueue) {
        mQueue.waitIdle();
    }

    // Destroy old swapchain
    mSwapchain = nullptr;
    mSwapchainMem = nullptr;
    mSwapchainDepthMem = nullptr;

    // Recreate
    InitSwapchain();
}

void GfxRenderingAPIDeko3d::FlushCommands() {
    DkCmdList cmdList = mCmdBuf.finishList();
    mQueue.submitCommands(cmdList);

    // Reset command buffer for next batch
    mCmdBuf.clear();
    mCmdBuf.addMemory(mCmdBufMem, 0, CMDBUF_SIZE);
}

void GfxRenderingAPIDeko3d::BindCurrentFramebuffer() {
    if (mCurrentFrameBuffer == 0 && mCurrentSwapImage >= 0) {
        // Bind swapchain image as render target
        dk::ImageView colorTarget{mSwapchainImages[mCurrentSwapImage]};
        dk::ImageView depthTarget{mSwapchainDepthImage};
        mCmdBuf.bindRenderTargets(&colorTarget, &depthTarget);
    } else if (mCurrentFrameBuffer < mFrameBuffers.size()) {
        auto& fb = mFrameBuffers[mCurrentFrameBuffer];
        dk::ImageView colorTarget{fb.colorImage};
        if (fb.has_depth_buffer) {
            dk::ImageView depthTarget{fb.depthImage};
            mCmdBuf.bindRenderTargets(&colorTarget, &depthTarget);
        } else {
            mCmdBuf.bindRenderTargets(&colorTarget);
        }
    }
}

// ---------------------------------------------------------------------------
// Shader management
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::UnloadShader(ShaderProgram* old_prg) {
    // With the uber-shader approach, no per-program GPU resources to unbind
}

void GfxRenderingAPIDeko3d::LoadShader(ShaderProgram* new_prg) {
    mCurrentShaderProgram = new_prg;
    if (mStats != nullptr) {
        mStats->shaderSwitches++;
    }
    // Invalidate uniform cache on shader switch
    mLastUniformTextureIds[0] = UINT32_MAX;
    mLastUniformTextureIds[1] = UINT32_MAX;
    mLastUniformTextureVersions[0] = UINT32_MAX;
    mLastUniformTextureVersions[1] = UINT32_MAX;
}

ShaderProgram* GfxRenderingAPIDeko3d::CreateAndLoadNewShader(uint64_t shader_id0, uint32_t shader_id1) {
    if (mStats != nullptr) {
        mStats->shaderCompilations++;
    }

    CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    auto& prg = mShaderProgramPool[std::make_pair(shader_id0, shader_id1)];

    // Count vertex attributes (same logic as OpenGL backend)
    size_t cnt = 0;
    prg.attribSizes[cnt] = 4; // aVtxPos
    ++cnt;

    size_t numFloats = 4;
    for (int i = 0; i < 2; i++) {
        if (cc_features.usedTextures[i]) {
            prg.attribSizes[cnt] = 2; // aTexCoord
            ++cnt;
            numFloats += 2;

            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    prg.attribSizes[cnt] = 1; // aTexClamp
                    ++cnt;
                    numFloats += 1;
                }
            }
        }
    }

    if (cc_features.opt_fog) {
        prg.attribSizes[cnt] = 4; // aFog
        ++cnt;
        numFloats += 4;
    }

    if (cc_features.opt_grayscale) {
        prg.attribSizes[cnt] = 4; // aGrayscaleColor
        ++cnt;
        numFloats += 4;
    }

    for (int i = 0; i < cc_features.numInputs; i++) {
        prg.attribSizes[cnt] = cc_features.opt_alpha ? 4 : 3; // aInput
        ++cnt;
        numFloats += cc_features.opt_alpha ? 4 : 3;
    }

    prg.numInputs = cc_features.numInputs;
    prg.usedTextures[0] = cc_features.usedTextures[0];
    prg.usedTextures[1] = cc_features.usedTextures[1];
    prg.usedTextures[2] = cc_features.used_masks[0];
    prg.usedTextures[3] = cc_features.used_masks[1];
    prg.usedTextures[4] = cc_features.used_blend[0];
    prg.usedTextures[5] = cc_features.used_blend[1];
    prg.numFloats = numFloats;
    prg.numAttribs = cnt;

    // Store combiner features for uniform setup
    prg.opt_alpha = cc_features.opt_alpha;
    prg.opt_fog = cc_features.opt_fog;
    prg.opt_grayscale = cc_features.opt_grayscale;
    prg.opt_noise = cc_features.opt_noise;
    prg.opt_2cyc = cc_features.opt_2cyc;
    prg.opt_texture_edge = cc_features.opt_texture_edge;
    prg.opt_alpha_threshold = cc_features.opt_alpha_threshold;
    prg.opt_invisible = cc_features.opt_invisible;
    memcpy(prg.clamp, cc_features.clamp, sizeof(prg.clamp));
    memcpy(prg.used_masks, cc_features.used_masks, sizeof(prg.used_masks));
    memcpy(prg.used_blend, cc_features.used_blend, sizeof(prg.used_blend));
    memcpy(prg.color_alpha_same, cc_features.color_alpha_same, sizeof(prg.color_alpha_same));
    memcpy(prg.do_single, cc_features.do_single, sizeof(prg.do_single));
    memcpy(prg.do_multiply, cc_features.do_multiply, sizeof(prg.do_multiply));
    memcpy(prg.do_mix, cc_features.do_mix, sizeof(prg.do_mix));
    memcpy(prg.c, cc_features.c, sizeof(prg.c));

    LoadShader(&prg);
    return &prg;
}

ShaderProgram* GfxRenderingAPIDeko3d::LookupShader(uint64_t shader_id0, uint32_t shader_id1) {
    auto it = mShaderProgramPool.find(std::make_pair(shader_id0, shader_id1));
    return it == mShaderProgramPool.end() ? nullptr : &it->second;
}

void GfxRenderingAPIDeko3d::ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    *numInputs = prg->numInputs;
    usedTextures[0] = prg->usedTextures[0];
    usedTextures[1] = prg->usedTextures[1];
}

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------

uint32_t GfxRenderingAPIDeko3d::NewTexture() {
    uint32_t id = mNextTextureId++;
    if (id >= mTextures.size()) {
        mTextures.resize(id + 1);
    }
    mTextures[id] = {};
    mTextures[id].valid = false;
    return id;
}

void GfxRenderingAPIDeko3d::SelectTexture(int tile, uint32_t textureId) {
    if (mStats != nullptr) {
        mStats->textureBinds++;
    }
    mCurrentTextureIds[tile] = textureId;
    mCurrentTile = tile;
}

void GfxRenderingAPIDeko3d::UploadTexture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    uint32_t texId = mCurrentTextureIds[mCurrentTile];
    if (texId >= mTextures.size()) {
        return;
    }

    auto& tex = mTextures[texId];
    size_t dataSize = width * height * 4;

    // Create image layout
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{mDevice}
        .setFlags(0)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(width, height)
        .initialize(layout);

    uint64_t imgSize = layout.getSize();
    uint64_t imgAlign = layout.getAlignment();
    uint64_t allocSize = (imgSize + imgAlign - 1) & ~(imgAlign - 1);
    // Round up to page size (DK_MEMBLOCK_ALIGNMENT)
    allocSize = (allocSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

    // Reallocate if size changed
    if (!tex.valid || tex.width != width || tex.height != height) {
        tex.mem = dk::MemBlockMaker{mDevice, allocSize}
                      .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                      .create();
        tex.image.initialize(layout, tex.mem, 0);
        tex.width = width;
        tex.height = height;
        tex.uniformsVersion++;
        tex.valid = true;
    }

    // Upload texture data via a staging buffer
    uint64_t stagingSize = (dataSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    dk::UniqueMemBlock staging = dk::MemBlockMaker{mDevice, stagingSize}
                                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                     .create();
    memcpy(staging.getCpuAddr(), rgba32_buf, dataSize);

    // Copy from staging to image
    dk::ImageView view{tex.image};
    mCmdBuf.copyBufferToImage({staging.getGpuAddr()}, view,
                              {0, 0, 0, width, height, 1});

    // Initialize texture descriptor
    tex.descriptor.initialize(tex.image);
}

void GfxRenderingAPIDeko3d::DeleteTexture(uint32_t texId) {
    if (texId < mTextures.size()) {
        mTextures[texId].mem = nullptr;
        mTextures[texId].valid = false;
    }
}

void GfxRenderingAPIDeko3d::SetSamplerParameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    uint32_t texId = mCurrentTextureIds[tile];
    if (texId >= mTextures.size()) {
        return;
    }

    auto& tex = mTextures[texId];
    const uint16_t filtering = !linear_filter ? FILTER_LINEAR : FILTER_THREE_POINT;
    if (tex.filtering != filtering) {
        tex.filtering = filtering;
        tex.uniformsVersion++;
    }

    // Configure sampler descriptor for this tile
    DkFilter filter =
        (linear_filter && mCurrentFilterMode == FILTER_LINEAR) ? DkFilter_Linear : DkFilter_Nearest;
    DkWrapMode wrapS = gfx_cm_to_dk(cms);
    DkWrapMode wrapT = gfx_cm_to_dk(cmt);

    // Build sampler descriptor
    DkSampler sampler;
    DkSamplerDescriptor desc;
    memset(&sampler, 0, sizeof(sampler));
    // We configure the sampler descriptor directly
    // deko3d sampler API: use DkSamplerDescriptor::initialize with a DkSampler
    // For now, store the parameters for binding at draw time
    mSamplerDescriptors[tile] = {};
    (void)filter;
    (void)wrapS;
    (void)wrapT;
}

// ---------------------------------------------------------------------------
// Render state
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = z_upd;
}

void GfxRenderingAPIDeko3d::SetZmodeDecal(bool zmode_decal) {
    mCurrentZmodeDecal = zmode_decal;
}

void GfxRenderingAPIDeko3d::SetViewport(int x, int y, int width, int height) {
    mCmdBuf.setViewports(0, {{(float)x, (float)y, (float)width, (float)height, 0.0f, 1.0f}});
}

void GfxRenderingAPIDeko3d::SetScissor(int x, int y, int width, int height) {
    mCmdBuf.setScissors(0, {{(uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height}});
}

void GfxRenderingAPIDeko3d::SetUseAlpha(bool use_alpha) {
    if (use_alpha) {
        // Enable alpha blending: srcAlpha, 1-srcAlpha
        mCmdBuf.bindBlendStates(0, {{
            DkBlendOp_Add,
            DkBlendFactor_SrcAlpha,
            DkBlendFactor_InvSrcAlpha,
            DkBlendOp_Add,
            DkBlendFactor_One,
            DkBlendFactor_InvSrcAlpha,
        }});
        mCmdBuf.setColorWriteMask(0, DkColorMask_RGBA);
    } else {
        // Disable blending (default state)
        mCmdBuf.bindBlendStates(0, {{}});
        mCmdBuf.setColorWriteMask(0, DkColorMask_RGBA);
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    // Update depth state
    if (mCurrentDepthTest != mLastDepthTest || mCurrentDepthMask != mLastDepthMask) {
        mLastDepthTest = mCurrentDepthTest;
        mLastDepthMask = mCurrentDepthMask;

        if (mCurrentDepthTest || mCurrentDepthMask) {
            DkDepthStencilState dsState;
            memset(&dsState, 0, sizeof(dsState));
            // Enable depth test/write as needed
            mCmdBuf.bindDepthStencilState(dsState);
        } else {
            DkDepthStencilState dsState;
            memset(&dsState, 0, sizeof(dsState));
            mCmdBuf.bindDepthStencilState(dsState);
        }
    }

    // Upload vertex data to VBO pool
    size_t uploadBytes = sizeof(float) * buf_vbo_len;
    if (mVboOffset + uploadBytes > VBO_POOL_SIZE) {
        // Flush and reset if pool is full
        FlushCommands();
        mVboOffset = 0;
    }

    void* vboDst = (uint8_t*)mVboMem.getCpuAddr() + mVboOffset;
    memcpy(vboDst, buf_vbo, uploadBytes);

    // Bind vertex buffer
    DkBufExtents vboBuf = {mVboMem.getGpuAddr() + mVboOffset, uploadBytes};
    mCmdBuf.bindVtxBuffers(0, {vboBuf});

    // Configure vertex attribute state based on current shader program
    ConfigureVertexState();

    // Draw
    mCmdBuf.draw(DkPrimitive_Triangles, 3 * buf_vbo_num_tris, 1, 0, 0);

    mVboOffset += uploadBytes;
    // Align offset to 256 bytes for next upload
    mVboOffset = (mVboOffset + 255) & ~255;
}

void GfxRenderingAPIDeko3d::ConfigureVertexState() {
    if (!mCurrentShaderProgram) {
        return;
    }

    // Build vertex attribute state from the current shader program's attribute list.
    // Each attribute is a contiguous block of floats in the vertex buffer.
    uint32_t offset = 0;
    uint32_t stride = mCurrentShaderProgram->numFloats * sizeof(float);

    DkVtxAttribState attribs[16];
    DkVtxBufferState bufState;
    memset(&bufState, 0, sizeof(bufState));
    bufState.stride = stride;

    for (uint8_t i = 0; i < mCurrentShaderProgram->numAttribs && i < 16; i++) {
        memset(&attribs[i], 0, sizeof(DkVtxAttribState));
        attribs[i].bufferId = 0;
        attribs[i].offset = offset;
        switch (mCurrentShaderProgram->attribSizes[i]) {
            case 1:
                attribs[i].size = DkVtxAttribSize_1x32;
                break;
            case 2:
                attribs[i].size = DkVtxAttribSize_2x32;
                break;
            case 3:
                attribs[i].size = DkVtxAttribSize_3x32;
                break;
            case 4:
            default:
                attribs[i].size = DkVtxAttribSize_4x32;
                break;
        }
        attribs[i].type = DkVtxAttribType_Float;
        offset += mCurrentShaderProgram->attribSizes[i] * sizeof(float);
    }

    mCmdBuf.bindVtxAttribState(
        {attribs, mCurrentShaderProgram->numAttribs});
    mCmdBuf.bindVtxBufferState(
        {&bufState, 1});
}

// ---------------------------------------------------------------------------
// Framebuffer management
// ---------------------------------------------------------------------------

int GfxRenderingAPIDeko3d::CreateFramebuffer() {
    size_t id = mFrameBuffers.size();
    mFrameBuffers.emplace_back();
    auto& fb = mFrameBuffers.back();
    fb.width = 0;
    fb.height = 0;
    fb.has_depth_buffer = false;
    fb.msaa_level = 0;
    fb.invertY = false;
    return (int)id;
}

void GfxRenderingAPIDeko3d::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height,
                                                        uint32_t msaa_level, bool opengl_invertY,
                                                        bool render_target, bool has_depth_buffer,
                                                        bool can_extract_depth) {
    if (fb_id < 0 || (size_t)fb_id >= mFrameBuffers.size()) {
        return;
    }

    auto& fb = mFrameBuffers[fb_id];
    bool sizeChanged = (fb.width != width || fb.height != height);
    bool depthChanged = (fb.has_depth_buffer != has_depth_buffer);

    fb.width = width;
    fb.height = height;
    fb.has_depth_buffer = has_depth_buffer;
    fb.msaa_level = msaa_level;
    fb.invertY = opengl_invertY;

    if (fb_id == 0) {
        // Swapchain framebuffer — managed by InitSwapchain
        return;
    }

    if (!render_target || width == 0 || height == 0) {
        return;
    }

    if (sizeChanged || depthChanged) {
        // (Re)create color image
        dk::ImageLayout colorLayout;
        dk::ImageLayoutMaker{mDevice}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(width, height)
            .initialize(colorLayout);

        uint64_t colorSize = colorLayout.getSize();
        uint64_t colorAlign = colorLayout.getAlignment();
        uint64_t allocColor = (colorSize + colorAlign - 1) & ~(colorAlign - 1);
        allocColor = (allocColor + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

        fb.colorMem = dk::MemBlockMaker{mDevice, allocColor}
                          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                          .create();
        fb.colorImage.initialize(colorLayout, fb.colorMem, 0);

        if (has_depth_buffer) {
            dk::ImageLayout depthLayout;
            dk::ImageLayoutMaker{mDevice}
                .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
                .setFormat(DkImageFormat_Z24S8)
                .setDimensions(width, height)
                .initialize(depthLayout);

            uint64_t depthSize = depthLayout.getSize();
            uint64_t depthAlign = depthLayout.getAlignment();
            uint64_t allocDepth = (depthSize + depthAlign - 1) & ~(depthAlign - 1);
            allocDepth = (allocDepth + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

            fb.depthMem = dk::MemBlockMaker{mDevice, allocDepth}
                              .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                              .create();
            fb.depthImage.initialize(depthLayout, fb.depthMem, 0);
        }
    }
}

void GfxRenderingAPIDeko3d::StartDrawToFramebuffer(int fbId, float noiseScale) {
    mCurrentFrameBuffer = fbId;
    mCurrentNoiseScale = noiseScale;
    BindCurrentFramebuffer();
}

void GfxRenderingAPIDeko3d::ClearFramebuffer(bool color, bool depth) {
    if (color) {
        mCmdBuf.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    }
    if (depth) {
        mCmdBuf.clearDepthStencil(true, 1.0f, 0xFF, 0);
    }
}

void GfxRenderingAPIDeko3d::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                            int dstX0, int dstY0, int dstX1, int dstY1) {
    // deko3d image-to-image copy
    dk::Image* srcImg = nullptr;
    dk::Image* dstImg = nullptr;

    if (fbSrcId == 0 && mCurrentSwapImage >= 0) {
        srcImg = &mSwapchainImages[mCurrentSwapImage];
    } else if ((size_t)fbSrcId < mFrameBuffers.size()) {
        srcImg = &mFrameBuffers[fbSrcId].colorImage;
    }

    if (fbDstId == 0 && mCurrentSwapImage >= 0) {
        dstImg = &mSwapchainImages[mCurrentSwapImage];
    } else if ((size_t)fbDstId < mFrameBuffers.size()) {
        dstImg = &mFrameBuffers[fbDstId].colorImage;
    }

    if (srcImg && dstImg) {
        dk::ImageView srcView{*srcImg};
        dk::ImageView dstView{*dstImg};
        mCmdBuf.copyImage(srcView, {(uint32_t)srcX0, (uint32_t)srcY0, 0},
                          dstView, {(uint32_t)dstX0, (uint32_t)dstY0, 0},
                          {(uint32_t)(srcX1 - srcX0), (uint32_t)(srcY1 - srcY0), 1});
    }
}

void GfxRenderingAPIDeko3d::ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    // Read-back from GPU to CPU - used for depth buffer reads.
    // This is a slow operation requiring a GPU sync.
    dk::Image* srcImg = nullptr;

    if (fbId == 0 && mCurrentSwapImage >= 0) {
        srcImg = &mSwapchainImages[mCurrentSwapImage];
    } else if ((size_t)fbId < mFrameBuffers.size()) {
        srcImg = &mFrameBuffers[fbId].colorImage;
    }

    if (!srcImg || width == 0 || height == 0) {
        return;
    }

    size_t dataSize = width * height * 4; // RGBA8
    uint64_t stagingSize = (dataSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

    dk::UniqueMemBlock staging = dk::MemBlockMaker{mDevice, stagingSize}
                                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                     .create();

    dk::ImageView view{*srcImg};
    mCmdBuf.copyImageToBuffer(view, {0, 0, 0, width, height, 1}, {staging.getGpuAddr()});
    FlushCommands();
    mQueue.waitIdle();

    // Convert RGBA8 to RGBA16 (5551 format)
    const uint8_t* src = (const uint8_t*)staging.getCpuAddr();
    for (uint32_t i = 0; i < width * height; i++) {
        uint8_t r = src[i * 4 + 0];
        uint8_t g = src[i * 4 + 1];
        uint8_t b = src[i * 4 + 2];
        uint8_t a = src[i * 4 + 3];
        rgba16Buf[i] = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a > 127 ? 1 : 0);
    }
}

void GfxRenderingAPIDeko3d::ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) {
    // MSAA not implemented in initial deko3d backend
    (void)fbIdTarget;
    (void)fbIdSrc;
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIDeko3d::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> result;

    // For pixel depth queries, we need to read back the depth buffer.
    // This is an expensive operation; return 0 for now as a placeholder.
    for (const auto& coord : coordinates) {
        result[coord] = 0;
    }
    return result;
}

void* GfxRenderingAPIDeko3d::GetFramebufferTextureId(int fbId) {
    // Return an opaque pointer that ImGui can use as a texture ID
    if (fbId == 0 && mCurrentSwapImage >= 0) {
        return (void*)(intptr_t)(&mSwapchainImages[mCurrentSwapImage]);
    }
    if ((size_t)fbId < mFrameBuffers.size()) {
        return (void*)(intptr_t)(&mFrameBuffers[fbId].colorImage);
    }
    return nullptr;
}

void GfxRenderingAPIDeko3d::SelectTextureFb(int fbId) {
    // Bind a framebuffer's color image as a texture source for subsequent draws
    // This is used for post-processing effects
    (void)fbId;
}

// ---------------------------------------------------------------------------
// Texture filter
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::SetTextureFilter(FilteringMode mode) {
    mCurrentFilterMode = mode;
}

FilteringMode GfxRenderingAPIDeko3d::GetTextureFilter() {
    return mCurrentFilterMode;
}

void GfxRenderingAPIDeko3d::SetSrgbMode() {
    mSrgbMode = true;
}

ImTextureID GfxRenderingAPIDeko3d::GetTextureById(int id) {
    // Return an ImTextureID for the given internal texture ID
    if ((uint32_t)id < mTextures.size() && mTextures[id].valid) {
        return (ImTextureID)(intptr_t)id;
    }
    return (ImTextureID)0;
}

} // namespace Fast

#endif // ENABLE_DEKO3D
