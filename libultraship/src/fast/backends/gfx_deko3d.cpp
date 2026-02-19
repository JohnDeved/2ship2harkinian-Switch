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
            return DkWrapMode_MirrorClampToEdge;
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
    return 4096;
}

GfxClipParameters GfxRenderingAPIDeko3d::GetClipParameters() {
    return { true, mFrameBuffers.size() > mCurrentFrameBuffer ? mFrameBuffers[mCurrentFrameBuffer].invertY : false };
}

// ---------------------------------------------------------------------------
// Device, swapchain, and resource initialization
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::InitDevice() {
    mDevice = dk::DeviceMaker{}.create();

    mQueue = dk::QueueMaker{ mDevice }
                 .setFlags(DkQueueFlags_Graphics)
                 .create();
}

void GfxRenderingAPIDeko3d::InitSwapchain() {
    mNWindow = nwindowGetDefault();

    int dispW = 1920, dispH = 1080;
    Ship::Switch::GetDisplaySize(&dispW, &dispH);

    dk::ImageLayout fbLayout;
    dk::ImageLayoutMaker{ mDevice }
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(dispW, dispH)
        .initialize(fbLayout);

    uint64_t fbSize = fbLayout.getSize();
    uint64_t fbAlign = fbLayout.getAlignment();
    uint64_t totalFbSize = (fbSize + fbAlign - 1) & ~(fbAlign - 1);

    mSwapchainMem = dk::MemBlockMaker{ mDevice, (uint32_t)(totalFbSize * NUM_FRAMEBUFFERS) }
                        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                        .create();

    DkImage const* swapImages[NUM_FRAMEBUFFERS];
    for (unsigned i = 0; i < NUM_FRAMEBUFFERS; i++) {
        mSwapchainImages[i].initialize(fbLayout, mSwapchainMem, totalFbSize * i);
        swapImages[i] = &mSwapchainImages[i];
    }

    // Depth buffer for the swapchain
    dk::ImageLayout depthLayout;
    dk::ImageLayoutMaker{ mDevice }
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_Z24S8)
        .setDimensions(dispW, dispH)
        .initialize(depthLayout);

    uint64_t depthSize = depthLayout.getSize();
    uint64_t depthAlign = depthLayout.getAlignment();
    uint64_t totalDepthSize = (depthSize + depthAlign - 1) & ~(depthAlign - 1);
    totalDepthSize = (totalDepthSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

    mSwapchainDepthMem = dk::MemBlockMaker{ mDevice, (uint32_t)totalDepthSize }
                             .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                             .create();
    mSwapchainDepthImage.initialize(depthLayout, mSwapchainDepthMem, 0);

    mSwapchain = dk::SwapchainMaker{ mDevice, mNWindow, swapImages, NUM_FRAMEBUFFERS }.create();
}

void GfxRenderingAPIDeko3d::InitDescriptorPools() {
    // Create memory for image and sampler descriptor pools
    uint32_t imageDescSize = MAX_DESCRIPTORS * sizeof(DkImageDescriptor);
    imageDescSize = (imageDescSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

    uint32_t samplerDescSize = MAX_DESCRIPTORS * sizeof(DkSamplerDescriptor);
    samplerDescSize = (samplerDescSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

    mImageDescMem = dk::MemBlockMaker{ mDevice, imageDescSize }
                        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                        .create();
    mSamplerDescMem = dk::MemBlockMaker{ mDevice, samplerDescSize }
                          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                          .create();

    mImageDescriptors = (DkImageDescriptor*)mImageDescMem.getCpuAddr();
    mSamplerDescriptors = (DkSamplerDescriptor*)mSamplerDescMem.getCpuAddr();

    // Zero-initialize
    memset(mImageDescriptors, 0, imageDescSize);
    memset(mSamplerDescriptors, 0, samplerDescSize);
}

void GfxRenderingAPIDeko3d::InitShaders() {
    mCodeMem = dk::MemBlockMaker{ mDevice, CODE_POOL_SIZE }
                   .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
                   .create();

    // Load precompiled uber-shaders from romfs.
    // The shaders are compiled at build time by the `uam` tool from GLSL sources
    // and placed in romfs as .dksh files.
    //
    // The uber-shader approach: a single vertex + fragment shader pair handles all
    // N64 color combiner configurations via uniform-driven branching, matching
    // what the OpenGL backend does with its per-combiner generated shaders.
    //
    // File paths: romfs:/shaders/fast3d_vs.dksh and romfs:/shaders/fast3d_fs.dksh
    //
    // For the initial integration, shader loading happens here.
    // If the shader files don't exist yet (build system not wired), the backend
    // will operate without shaders and draw calls will be no-ops until shaders
    // are provided.

    FILE* vsFile = fopen("romfs:/shaders/fast3d_vs.dksh", "rb");
    FILE* fsFile = fopen("romfs:/shaders/fast3d_fs.dksh", "rb");

    if (vsFile && fsFile) {
        // Read vertex shader
        fseek(vsFile, 0, SEEK_END);
        long vsSize = ftell(vsFile);
        fseek(vsFile, 0, SEEK_SET);

        // Read fragment shader
        fseek(fsFile, 0, SEEK_END);
        long fsSize = ftell(fsFile);
        fseek(fsFile, 0, SEEK_SET);

        if (vsSize > 0 && fsSize > 0) {
            // Align code offsets to DK_SHADER_CODE_ALIGNMENT
            uint32_t vsCodeOff = DK_SHADER_CODE_UNUSABLE_SIZE;
            uint32_t vsAligned = (vsSize + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
            uint32_t fsCodeOff = vsCodeOff + vsAligned;

            // Read control blocks into temp buffers
            std::vector<uint8_t> vsData(vsSize);
            std::vector<uint8_t> fsData(fsSize);
            fread(vsData.data(), 1, vsSize, vsFile);
            fread(fsData.data(), 1, fsSize, fsFile);

            // Copy shader code into code memory
            uint8_t* codeBase = (uint8_t*)mCodeMem.getCpuAddr();
            memcpy(codeBase + vsCodeOff, vsData.data(), vsSize);
            memcpy(codeBase + fsCodeOff, fsData.data(), fsSize);

            // Initialize shader objects using the C++ wrapper
            dk::ShaderMaker{ mCodeMem, vsCodeOff }
                .setControl(vsData.data())
                .initialize(mVertexShader);

            dk::ShaderMaker{ mCodeMem, fsCodeOff }
                .setControl(fsData.data())
                .initialize(mFragmentShader);

            mShadersLoaded = true;
        }
    }

    if (vsFile) fclose(vsFile);
    if (fsFile) fclose(fsFile);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::Init() {
    InitDevice();
    InitSwapchain();
    InitDescriptorPools();

    // VBO memory pool (CPU-writable, GPU-readable)
    mVboMem = dk::MemBlockMaker{ mDevice, VBO_POOL_SIZE }
                  .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                  .create();

    // Uniform memory pool
    mUniformMem = dk::MemBlockMaker{ mDevice, UNIFORM_POOL_SIZE }
                      .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                      .create();

    // Command buffer
    mCmdBufMem = dk::MemBlockMaker{ mDevice, CMDBUF_SIZE }
                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                     .create();
    mCmdBuf = dk::CmdBufMaker{ mDevice }.create();
    mCmdBuf.addMemory(mCmdBufMem, 0, CMDBUF_SIZE);

    InitShaders();

    // Set up initial rasterizer state: no culling, depth clamp enabled
    dk::RasterizerState rasterState;
    rasterState.setCullMode(DkFace_None).setDepthClampEnable(true);
    mCmdBuf.bindRasterizerState(rasterState);

    // Set up initial color state: blending disabled
    dk::ColorState colorState;
    mCmdBuf.bindColorState(colorState);

    // Set up initial color write state: write all channels
    dk::ColorWriteState cwState;
    mCmdBuf.bindColorWriteState(cwState);

    // Set up initial depth-stencil state
    dk::DepthStencilState dsState;
    dsState.setDepthTestEnable(false).setDepthWriteEnable(false);
    mCmdBuf.bindDepthStencilState(dsState);

    // Bind descriptor pools so the GPU knows where to find texture/sampler descriptors
    mCmdBuf.bindImageDescriptorSet(mImageDescMem.getGpuAddr(), MAX_DESCRIPTORS);
    mCmdBuf.bindSamplerDescriptorSet(mSamplerDescMem.getGpuAddr(), MAX_DESCRIPTORS);

    // Bind shaders if loaded
    if (mShadersLoaded) {
        const DkShader* shaders[] = { &mVertexShader, &mFragmentShader };
        mCmdBuf.bindShaders(DkStageFlag_Vertex | DkStageFlag_Fragment,
                            dk::detail::ArrayProxy<DkShader const* const>(2, shaders));
    }

    // Submit initial state commands and wait for them to complete
    FlushCommands();
    mQueue.waitIdle();

    // Reserve slot 0 for the default screen framebuffer
    mFrameBuffers.resize(1);
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::StartFrame() {
    mFrameCount++;

    // Wait for previous frame's GPU work to complete via fence
    mFrameFence.wait();

    mVboOffset = 0;
    mUniformOffset = 0;

    // Acquire next swapchain image
    mCurrentSwapImage = mQueue.acquireImage(mSwapchain);

    // Re-bind descriptor pools each frame (they may have been invalidated)
    mCmdBuf.bindImageDescriptorSet(mImageDescMem.getGpuAddr(), MAX_DESCRIPTORS);
    mCmdBuf.bindSamplerDescriptorSet(mSamplerDescMem.getGpuAddr(), MAX_DESCRIPTORS);

    // Re-bind shaders each frame
    if (mShadersLoaded) {
        const DkShader* shaders[] = { &mVertexShader, &mFragmentShader };
        mCmdBuf.bindShaders(DkStageFlag_Vertex | DkStageFlag_Fragment,
                            dk::detail::ArrayProxy<DkShader const* const>(2, shaders));
    }
}

void GfxRenderingAPIDeko3d::EndFrame() {
    // Signal fence to allow per-frame pipelining instead of full GPU stall
    mCmdBuf.signalFence(mFrameFence, true);
    FlushCommands();

    mQueue.presentImage(mSwapchain, mCurrentSwapImage);
}

void GfxRenderingAPIDeko3d::FinishRender() {
    if (mQueue) {
        mQueue.waitIdle();
    }
}

void GfxRenderingAPIDeko3d::OnResize() {
    if (mQueue) {
        mQueue.waitIdle();
    }

    mSwapchain = nullptr;
    mSwapchainMem = nullptr;
    mSwapchainDepthMem = nullptr;

    InitSwapchain();
}

void GfxRenderingAPIDeko3d::FlushCommands() {
    DkCmdList cmdList = mCmdBuf.finishList();
    mQueue.submitCommands(cmdList);

    mCmdBuf.clear();
    mCmdBuf.addMemory(mCmdBufMem, 0, CMDBUF_SIZE);
}

void GfxRenderingAPIDeko3d::BindCurrentFramebuffer() {
    if (mCurrentFrameBuffer == 0 && mCurrentSwapImage >= 0) {
        dk::ImageView colorTarget{ mSwapchainImages[mCurrentSwapImage] };
        dk::ImageView depthTarget{ mSwapchainDepthImage };
        const DkImageView* colorTargetPtr = &colorTarget;
        mCmdBuf.bindRenderTargets(
            dk::detail::ArrayProxy<DkImageView const* const>(1, &colorTargetPtr), &depthTarget);
    } else if (mCurrentFrameBuffer < mFrameBuffers.size()) {
        auto& fb = mFrameBuffers[mCurrentFrameBuffer];
        dk::ImageView colorTarget{ fb.colorImage };
        const DkImageView* colorTargetPtr = &colorTarget;
        if (fb.has_depth_buffer) {
            dk::ImageView depthTarget{ fb.depthImage };
            mCmdBuf.bindRenderTargets(
                dk::detail::ArrayProxy<DkImageView const* const>(1, &colorTargetPtr), &depthTarget);
        } else {
            mCmdBuf.bindRenderTargets(
                dk::detail::ArrayProxy<DkImageView const* const>(1, &colorTargetPtr));
        }
    }
}

// ---------------------------------------------------------------------------
// Shader management
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::UnloadShader(ShaderProgram* old_prg) {
    // Uber-shader approach: nothing to unbind per program
}

void GfxRenderingAPIDeko3d::LoadShader(ShaderProgram* new_prg) {
    mCurrentShaderProgram = new_prg;
    if (mStats != nullptr) {
        mStats->shaderSwitches++;
    }
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
    prg.attribSizes[cnt] = 4; // aVtxPos (vec4)
    ++cnt;

    size_t numFloats = 4;
    for (int i = 0; i < 2; i++) {
        if (cc_features.usedTextures[i]) {
            prg.attribSizes[cnt] = 2; // aTexCoord (vec2)
            ++cnt;
            numFloats += 2;

            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    prg.attribSizes[cnt] = 1; // aTexClamp (float)
                    ++cnt;
                    numFloats += 1;
                }
            }
        }
    }

    if (cc_features.opt_fog) {
        prg.attribSizes[cnt] = 4; // aFog (vec4)
        ++cnt;
        numFloats += 4;
    }

    if (cc_features.opt_grayscale) {
        prg.attribSizes[cnt] = 4; // aGrayscaleColor (vec4)
        ++cnt;
        numFloats += 4;
    }

    for (int i = 0; i < cc_features.numInputs; i++) {
        prg.attribSizes[cnt] = cc_features.opt_alpha ? 4 : 3; // aInput (vec3/vec4)
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

    // Store combiner features for uniform setup at draw time
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

    // Recycle a descriptor index from the free list, or allocate a new one
    if (!mFreeDescriptorIndices.empty()) {
        mTextures[id].descriptorIdx = mFreeDescriptorIndices.top();
        mFreeDescriptorIndices.pop();
    } else {
        mTextures[id].descriptorIdx = id;
    }
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

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{ mDevice }
        .setFlags(0)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(width, height)
        .initialize(layout);

    uint64_t imgSize = layout.getSize();
    uint64_t imgAlign = layout.getAlignment();
    uint64_t allocSize = (imgSize + imgAlign - 1) & ~(imgAlign - 1);
    allocSize = (allocSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

    // Reallocate if size changed
    if (!tex.valid || tex.width != width || tex.height != height) {
        tex.mem = dk::MemBlockMaker{ mDevice, (uint32_t)allocSize }
                      .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                      .create();
        tex.image.initialize(layout, tex.mem, 0);
        tex.width = width;
        tex.height = height;
        tex.uniformsVersion++;
        tex.valid = true;
    }

    // Upload via staging buffer
    uint64_t stagingSize = (dataSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    dk::UniqueMemBlock staging = dk::MemBlockMaker{ mDevice, (uint32_t)stagingSize }
                                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                     .create();
    memcpy(staging.getCpuAddr(), rgba32_buf, dataSize);

    dk::ImageView view{ tex.image };
    DkCopyBuf srcBuf = { staging.getGpuAddr(), 0, 0 };
    DkImageRect dstRect = { 0, 0, 0, width, height, 1 };
    mCmdBuf.copyBufferToImage(srcBuf, view, dstRect);

    // Update image descriptor in the pool
    dk::ImageView descView{ tex.image };
    dk::ImageDescriptor imgDesc;
    imgDesc.initialize(descView);
    mImageDescriptors[tex.descriptorIdx] = *reinterpret_cast<DkImageDescriptor*>(&imgDesc);

    // Flush the upload and wait for it to complete so the staging buffer can be freed
    FlushCommands();
    mQueue.waitIdle();
}

void GfxRenderingAPIDeko3d::DeleteTexture(uint32_t texId) {
    if (texId < mTextures.size() && mTextures[texId].valid) {
        // Recycle the descriptor index for future textures
        mFreeDescriptorIndices.push(mTextures[texId].descriptorIdx);
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

    // Determine filter mode
    DkFilter dkFilter =
        (linear_filter && mCurrentFilterMode == FILTER_LINEAR) ? DkFilter_Linear : DkFilter_Nearest;
    DkWrapMode wrapS = gfx_cm_to_dk(cms);
    DkWrapMode wrapT = gfx_cm_to_dk(cmt);

    // Build a proper DkSampler and initialize its descriptor
    dk::Sampler sampler;
    sampler.setFilter(dkFilter, dkFilter, DkMipFilter_None);
    sampler.setWrapMode(wrapS, wrapT, DkWrapMode_ClampToEdge);

    // Store in the sampler descriptor pool at the same index as the texture
    dk::SamplerDescriptor desc;
    desc.initialize(sampler);
    mSamplerDescriptors[tex.descriptorIdx] = *reinterpret_cast<DkSamplerDescriptor*>(&desc);
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
    DkViewport vp;
    vp.x = (float)x;
    vp.y = (float)y;
    vp.width = (float)width;
    vp.height = (float)height;
    vp.near = 0.0f;
    vp.far = 1.0f;
    mCmdBuf.setViewports(0, dk::detail::ArrayProxy<DkViewport const>(1, &vp));
}

void GfxRenderingAPIDeko3d::SetScissor(int x, int y, int width, int height) {
    DkScissor sc;
    sc.x = (uint32_t)x;
    sc.y = (uint32_t)y;
    sc.width = (uint32_t)width;
    sc.height = (uint32_t)height;
    mCmdBuf.setScissors(0, dk::detail::ArrayProxy<DkScissor const>(1, &sc));
}

void GfxRenderingAPIDeko3d::SetUseAlpha(bool use_alpha) {
    dk::ColorState colorState;
    if (use_alpha) {
        colorState.setBlendEnable(0, true);
    }
    mCmdBuf.bindColorState(colorState);

    if (use_alpha) {
        dk::BlendState blendState;
        blendState.setFactors(DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
                              DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha);
        blendState.setOps(DkBlendOp_Add, DkBlendOp_Add);
        mCmdBuf.bindBlendStates(0, dk::detail::ArrayProxy<DkBlendState const>(1, &blendState));
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!mShadersLoaded || !mCurrentShaderProgram) {
        return;
    }

    // --- Depth/stencil state ---
    if (mCurrentDepthTest != mLastDepthTest || mCurrentDepthMask != mLastDepthMask ||
        mCurrentZmodeDecal != mLastZmodeDecal) {
        mLastDepthTest = mCurrentDepthTest;
        mLastDepthMask = mCurrentDepthMask;
        mLastZmodeDecal = mCurrentZmodeDecal;

        dk::DepthStencilState dsState;

        if (mCurrentDepthTest || mCurrentDepthMask) {
            dsState.setDepthTestEnable(true);
            dsState.setDepthWriteEnable(mCurrentDepthMask ? true : false);
            dsState.setDepthCompareOp(mCurrentZmodeDecal ? DkCompareOp_Lequal : DkCompareOp_Less);
        } else {
            dsState.setDepthTestEnable(false);
            dsState.setDepthWriteEnable(false);
        }

        mCmdBuf.bindDepthStencilState(dsState);

        // Depth bias for decal mode (polygon offset equivalent)
        dk::RasterizerState rasterState;
        rasterState.setCullMode(DkFace_None).setDepthClampEnable(true);
        if (mCurrentZmodeDecal) {
            rasterState.setDepthBiasEnableMask(DkPolygonFlag_Fill);
            mCmdBuf.bindRasterizerState(rasterState);
            mCmdBuf.setDepthBias(0.0f, 0.0f, -2.0f);
        } else {
            rasterState.setDepthBiasEnableMask(0);
            mCmdBuf.bindRasterizerState(rasterState);
        }
    }

    // --- Upload uniforms ---
    if (mUniformOffset + sizeof(UberUniforms) > UNIFORM_POOL_SIZE) {
        FlushCommands();
        mQueue.waitIdle();
        mUniformOffset = 0;
    }

    UberUniforms* uniforms = (UberUniforms*)((uint8_t*)mUniformMem.getCpuAddr() + mUniformOffset);
    memset(uniforms, 0, sizeof(UberUniforms));

    // Fill combiner control — flatten c[cycle][colorOrAlpha][4] into cc_CYC_TYPE[4]
    memcpy(uniforms->cc_0_0, mCurrentShaderProgram->c[0][0], sizeof(uniforms->cc_0_0));
    memcpy(uniforms->cc_0_1, mCurrentShaderProgram->c[0][1], sizeof(uniforms->cc_0_1));
    memcpy(uniforms->cc_1_0, mCurrentShaderProgram->c[1][0], sizeof(uniforms->cc_1_0));
    memcpy(uniforms->cc_1_1, mCurrentShaderProgram->c[1][1], sizeof(uniforms->cc_1_1));

    uniforms->useTexture[0] = mCurrentShaderProgram->usedTextures[0] ? 1 : 0;
    uniforms->useTexture[1] = mCurrentShaderProgram->usedTextures[1] ? 1 : 0;
    uniforms->useMask[0] = mCurrentShaderProgram->used_masks[0] ? 1 : 0;
    uniforms->useMask[1] = mCurrentShaderProgram->used_masks[1] ? 1 : 0;
    uniforms->useBlend[0] = mCurrentShaderProgram->used_blend[0] ? 1 : 0;
    uniforms->useBlend[1] = mCurrentShaderProgram->used_blend[1] ? 1 : 0;
    uniforms->optAlpha = mCurrentShaderProgram->opt_alpha ? 1 : 0;
    uniforms->optFog = mCurrentShaderProgram->opt_fog ? 1 : 0;
    uniforms->optNoise = mCurrentShaderProgram->opt_noise ? 1 : 0;
    uniforms->opt2Cyc = mCurrentShaderProgram->opt_2cyc ? 1 : 0;
    uniforms->optTextureEdge = mCurrentShaderProgram->opt_texture_edge ? 1 : 0;
    uniforms->optAlphaThreshold = mCurrentShaderProgram->opt_alpha_threshold ? 1 : 0;
    uniforms->optInvisible = mCurrentShaderProgram->opt_invisible ? 1 : 0;
    uniforms->optGrayscale = mCurrentShaderProgram->opt_grayscale ? 1 : 0;
    uniforms->frameCount = mFrameCount;
    uniforms->noiseScale = mCurrentNoiseScale;
    uniforms->srgbMode = mSrgbMode ? 1 : 0;

    // Flatten [tex][s/t] to [tex0_s, tex0_t, tex1_s, tex1_t]
    uniforms->useClamp[0] = mCurrentShaderProgram->clamp[0][0] ? 1 : 0;
    uniforms->useClamp[1] = mCurrentShaderProgram->clamp[0][1] ? 1 : 0;
    uniforms->useClamp[2] = mCurrentShaderProgram->clamp[1][0] ? 1 : 0;
    uniforms->useClamp[3] = mCurrentShaderProgram->clamp[1][1] ? 1 : 0;

    // Flatten [cycle][colorOrAlpha] to [cyc0_rgb, cyc0_a, cyc1_rgb, cyc1_a]
    uniforms->doSingle[0] = mCurrentShaderProgram->do_single[0][0] ? 1 : 0;
    uniforms->doSingle[1] = mCurrentShaderProgram->do_single[0][1] ? 1 : 0;
    uniforms->doSingle[2] = mCurrentShaderProgram->do_single[1][0] ? 1 : 0;
    uniforms->doSingle[3] = mCurrentShaderProgram->do_single[1][1] ? 1 : 0;
    uniforms->doMultiply[0] = mCurrentShaderProgram->do_multiply[0][0] ? 1 : 0;
    uniforms->doMultiply[1] = mCurrentShaderProgram->do_multiply[0][1] ? 1 : 0;
    uniforms->doMultiply[2] = mCurrentShaderProgram->do_multiply[1][0] ? 1 : 0;
    uniforms->doMultiply[3] = mCurrentShaderProgram->do_multiply[1][1] ? 1 : 0;
    uniforms->doMix[0] = mCurrentShaderProgram->do_mix[0][0] ? 1 : 0;
    uniforms->doMix[1] = mCurrentShaderProgram->do_mix[0][1] ? 1 : 0;
    uniforms->doMix[2] = mCurrentShaderProgram->do_mix[1][0] ? 1 : 0;
    uniforms->doMix[3] = mCurrentShaderProgram->do_mix[1][1] ? 1 : 0;

    for (int i = 0; i < 2; i++) {
        uniforms->colorAlphaSame[i] = mCurrentShaderProgram->color_alpha_same[i] ? 1 : 0;
    }

    // Texture info
    for (int i = 0; i < 2; i++) {
        if (mCurrentShaderProgram->usedTextures[i]) {
            uint32_t texId = mCurrentTextureIds[i];
            if (texId < mTextures.size() && mTextures[texId].valid) {
                uniforms->textureWidth[i] = mTextures[texId].width;
                uniforms->textureHeight[i] = mTextures[texId].height;
                uniforms->textureFiltering[i] = mTextures[texId].filtering;
            }
        }
    }

    // Bind uniform buffer
    DkGpuAddr uboAddr = mUniformMem.getGpuAddr() + mUniformOffset;
    mCmdBuf.bindUniformBuffer(DkStage_Fragment, 0, uboAddr, sizeof(UberUniforms));
    mCmdBuf.bindUniformBuffer(DkStage_Vertex, 0, uboAddr, sizeof(UberUniforms));

    mUniformOffset += sizeof(UberUniforms);
    mUniformOffset = (mUniformOffset + DK_UNIFORM_BUF_ALIGNMENT - 1) & ~(DK_UNIFORM_BUF_ALIGNMENT - 1);

    // --- Bind textures ---
    for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
        if (mCurrentShaderProgram->usedTextures[i]) {
            uint32_t texId = mCurrentTextureIds[i];
            if (texId < mTextures.size() && mTextures[texId].valid) {
                uint32_t descIdx = mTextures[texId].descriptorIdx;
                DkResHandle handle = dkMakeTextureHandle(descIdx, descIdx);
                mCmdBuf.bindTextures(DkStage_Fragment, i,
                                     dk::detail::ArrayProxy<DkResHandle const>(1, &handle));
            }
        }
    }

    // --- Upload vertex data ---
    size_t uploadBytes = sizeof(float) * buf_vbo_len;
    if (mVboOffset + uploadBytes > VBO_POOL_SIZE) {
        FlushCommands();
        mQueue.waitIdle();
        mVboOffset = 0;
    }

    void* vboDst = (uint8_t*)mVboMem.getCpuAddr() + mVboOffset;
    memcpy(vboDst, buf_vbo, uploadBytes);

    // Bind vertex buffer
    DkBufExtents vboBuf;
    vboBuf.addr = mVboMem.getGpuAddr() + mVboOffset;
    vboBuf.size = (uint32_t)uploadBytes;
    mCmdBuf.bindVtxBuffers(0, dk::detail::ArrayProxy<DkBufExtents const>(1, &vboBuf));

    // Configure vertex attribute state
    ConfigureVertexState();

    // Draw
    mCmdBuf.draw(DkPrimitive_Triangles, 3 * buf_vbo_num_tris, 1, 0, 0);

    mVboOffset += uploadBytes;
    mVboOffset = (mVboOffset + 255) & ~(size_t)255;
}

void GfxRenderingAPIDeko3d::ConfigureVertexState() {
    if (!mCurrentShaderProgram) {
        return;
    }

    uint32_t stride = mCurrentShaderProgram->numFloats * sizeof(float);

    // The uber-shader has 13 attribute locations (0-12), matching this order:
    //   0: aVtxPos (vec4)        — always
    //   1: aTexCoord0 (vec2)     — if usedTextures[0]
    //   2: aTexClampS0 (float)   — if clamp[0][0]
    //   3: aTexClampT0 (float)   — if clamp[0][1]
    //   4: aTexCoord1 (vec2)     — if usedTextures[1]
    //   5: aTexClampS1 (float)   — if clamp[1][0]
    //   6: aTexClampT1 (float)   — if clamp[1][1]
    //   7: aFog (vec4)           — if opt_fog
    //   8: aGrayscaleColor (vec4) — if opt_grayscale
    //   9-12: aInput1-4 (vec3/vec4) — numInputs
    //
    // We bind all 13 locations. Active ones point into the VBO at their
    // packed offset; inactive ones use isFixed=1 (default to zero).

    static constexpr int NUM_SHADER_ATTRIBS = 13;
    DkVtxAttribState attribs[NUM_SHADER_ATTRIBS];
    memset(attribs, 0, sizeof(attribs));

    // Mark all as fixed (default zero) initially
    for (int i = 0; i < NUM_SHADER_ATTRIBS; i++) {
        attribs[i].isFixed = 1;
        attribs[i].size = DkVtxAttribSize_1x32;
        attribs[i].type = DkVtxAttribType_Float;
    }

    auto setAttrib = [&](int loc, uint32_t offset, int numComponents) {
        attribs[loc].bufferId = 0;
        attribs[loc].isFixed = 0;
        attribs[loc].offset = offset;
        attribs[loc].type = DkVtxAttribType_Float;
        switch (numComponents) {
            case 1:
                attribs[loc].size = DkVtxAttribSize_1x32;
                break;
            case 2:
                attribs[loc].size = DkVtxAttribSize_2x32;
                break;
            case 3:
                attribs[loc].size = DkVtxAttribSize_3x32;
                break;
            case 4:
            default:
                attribs[loc].size = DkVtxAttribSize_4x32;
                break;
        }
    };

    uint32_t offset = 0;

    // Location 0: aVtxPos (vec4) — always present
    setAttrib(0, offset, 4);
    offset += 4 * sizeof(float);

    // Textures
    for (int i = 0; i < 2; i++) {
        if (mCurrentShaderProgram->usedTextures[i]) {
            int texCoordLoc = (i == 0) ? 1 : 4;
            setAttrib(texCoordLoc, offset, 2);
            offset += 2 * sizeof(float);

            for (int j = 0; j < 2; j++) {
                if (mCurrentShaderProgram->clamp[i][j]) {
                    int clampLoc = (i == 0) ? (2 + j) : (5 + j);
                    setAttrib(clampLoc, offset, 1);
                    offset += 1 * sizeof(float);
                }
            }
        }
    }

    // Location 7: aFog (vec4)
    if (mCurrentShaderProgram->opt_fog) {
        setAttrib(7, offset, 4);
        offset += 4 * sizeof(float);
    }

    // Location 8: aGrayscaleColor (vec4)
    if (mCurrentShaderProgram->opt_grayscale) {
        setAttrib(8, offset, 4);
        offset += 4 * sizeof(float);
    }

    // Locations 9-12: aInput1-4
    int inputSize = mCurrentShaderProgram->opt_alpha ? 4 : 3;
    for (int i = 0; i < mCurrentShaderProgram->numInputs && i < 4; i++) {
        setAttrib(9 + i, offset, inputSize);
        offset += inputSize * sizeof(float);
    }

    DkVtxBufferState bufState;
    memset(&bufState, 0, sizeof(bufState));
    bufState.stride = stride;

    mCmdBuf.bindVtxAttribState(dk::detail::ArrayProxy<DkVtxAttribState const>(NUM_SHADER_ATTRIBS, attribs));
    mCmdBuf.bindVtxBufferState(dk::detail::ArrayProxy<DkVtxBufferState const>(1, &bufState));
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
        return; // Swapchain framebuffer managed by InitSwapchain
    }

    if (!render_target || width == 0 || height == 0) {
        return;
    }

    if (sizeChanged || depthChanged) {
        // (Re)create color image
        dk::ImageLayout colorLayout;
        dk::ImageLayoutMaker{ mDevice }
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(width, height)
            .initialize(colorLayout);

        uint64_t colorSize = colorLayout.getSize();
        uint64_t colorAlign = colorLayout.getAlignment();
        uint64_t allocColor = (colorSize + colorAlign - 1) & ~(colorAlign - 1);
        allocColor = (allocColor + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

        fb.colorMem = dk::MemBlockMaker{ mDevice, (uint32_t)allocColor }
                          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                          .create();
        fb.colorImage.initialize(colorLayout, fb.colorMem, 0);

        // Also create an image descriptor for the framebuffer so it can be used as a texture
        uint32_t fbDescIdx = MAX_DESCRIPTORS / 2 + fb_id; // Use second half of descriptor pool for FBs
        if (fbDescIdx < MAX_DESCRIPTORS) {
            dk::ImageView descView{ fb.colorImage };
            dk::ImageDescriptor imgDesc;
            imgDesc.initialize(descView);
            mImageDescriptors[fbDescIdx] = *reinterpret_cast<DkImageDescriptor*>(&imgDesc);
            fb.descriptorIdx = fbDescIdx;

            // Create a default sampler for the FB texture (linear filtering, clamp to edge)
            dk::Sampler sampler;
            sampler.setFilter(DkFilter_Linear, DkFilter_Linear, DkMipFilter_None);
            sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
            dk::SamplerDescriptor sampDesc;
            sampDesc.initialize(sampler);
            mSamplerDescriptors[fbDescIdx] = *reinterpret_cast<DkSamplerDescriptor*>(&sampDesc);
        }

        if (has_depth_buffer) {
            dk::ImageLayout depthLayout;
            dk::ImageLayoutMaker{ mDevice }
                .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
                .setFormat(DkImageFormat_Z24S8)
                .setDimensions(width, height)
                .initialize(depthLayout);

            uint64_t depthSize = depthLayout.getSize();
            uint64_t depthAlign = depthLayout.getAlignment();
            uint64_t allocDepth = (depthSize + depthAlign - 1) & ~(depthAlign - 1);
            allocDepth = (allocDepth + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

            fb.depthMem = dk::MemBlockMaker{ mDevice, (uint32_t)allocDepth }
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
        dk::ImageView srcView{ *srcImg };
        dk::ImageView dstView{ *dstImg };

        uint32_t srcW = (uint32_t)(srcX1 - srcX0);
        uint32_t srcH = (uint32_t)(srcY1 - srcY0);
        uint32_t dstW = (uint32_t)(dstX1 - dstX0);
        uint32_t dstH = (uint32_t)(dstY1 - dstY0);

        if (srcW == dstW && srcH == dstH) {
            // Same-size copy
            DkImageRect srcRect = { (uint32_t)srcX0, (uint32_t)srcY0, 0, srcW, srcH, 1 };
            DkImageRect dstRect = { (uint32_t)dstX0, (uint32_t)dstY0, 0, dstW, dstH, 1 };
            mCmdBuf.copyImage(srcView, srcRect, dstView, dstRect);
        } else {
            // Scaled blit
            DkImageRect srcRect = { (uint32_t)srcX0, (uint32_t)srcY0, 0, srcW, srcH, 1 };
            DkImageRect dstRect = { (uint32_t)dstX0, (uint32_t)dstY0, 0, dstW, dstH, 1 };
            mCmdBuf.blitImage(srcView, srcRect, dstView, dstRect);
        }
    }
}

void GfxRenderingAPIDeko3d::ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    dk::Image* srcImg = nullptr;

    if (fbId == 0 && mCurrentSwapImage >= 0) {
        srcImg = &mSwapchainImages[mCurrentSwapImage];
    } else if ((size_t)fbId < mFrameBuffers.size()) {
        srcImg = &mFrameBuffers[fbId].colorImage;
    }

    if (!srcImg || width == 0 || height == 0) {
        return;
    }

    size_t dataSize = width * height * 4;
    uint64_t stagingSize = (dataSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

    dk::UniqueMemBlock staging = dk::MemBlockMaker{ mDevice, (uint32_t)stagingSize }
                                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                     .create();

    dk::ImageView view{ *srcImg };
    DkImageRect srcRect = { 0, 0, 0, width, height, 1 };
    DkCopyBuf dstBuf = { staging.getGpuAddr(), 0, 0 };
    mCmdBuf.copyImageToBuffer(view, srcRect, dstBuf);
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
    if ((size_t)fbIdSrc < mFrameBuffers.size() && (size_t)fbIdTarget < mFrameBuffers.size()) {
        dk::ImageView srcView{ mFrameBuffers[fbIdSrc].colorImage };
        dk::ImageView dstView{ mFrameBuffers[fbIdTarget].colorImage };
        mCmdBuf.resolveImage(srcView, dstView);
    }
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIDeko3d::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> result;

    if (coordinates.empty()) {
        return result;
    }

    // Determine which depth image to read
    dk::Image* depthImg = nullptr;
    uint32_t fbW = 0, fbH = 0;

    if (fb_id == 0 && mCurrentSwapImage >= 0) {
        depthImg = &mSwapchainDepthImage;
        // Use swapchain dimensions
        fbW = 1280; // Default Switch resolution
        fbH = 720;
    } else if ((size_t)fb_id < mFrameBuffers.size() && mFrameBuffers[fb_id].has_depth_buffer) {
        depthImg = &mFrameBuffers[fb_id].depthImage;
        fbW = mFrameBuffers[fb_id].width;
        fbH = mFrameBuffers[fb_id].height;
    }

    if (!depthImg || fbW == 0 || fbH == 0) {
        for (const auto& coord : coordinates) {
            result[coord] = 0;
        }
        return result;
    }

    // Read the depth buffer to a staging buffer
    size_t pixelSize = 4; // Z24S8 = 4 bytes per pixel
    size_t dataSize = fbW * fbH * pixelSize;
    uint64_t stagingSize = (dataSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

    dk::UniqueMemBlock staging = dk::MemBlockMaker{ mDevice, (uint32_t)stagingSize }
                                     .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                     .create();

    dk::ImageView view{ *depthImg };
    DkImageRect srcRect = { 0, 0, 0, fbW, fbH, 1 };
    DkCopyBuf dstBuf = { staging.getGpuAddr(), 0, 0 };
    mCmdBuf.copyImageToBuffer(view, srcRect, dstBuf);
    FlushCommands();
    mQueue.waitIdle();

    const uint32_t* depthData = (const uint32_t*)staging.getCpuAddr();

    for (const auto& coord : coordinates) {
        int x = (int)coord.first;
        int y = (int)coord.second;

        if (x >= 0 && x < (int)fbW && y >= 0 && y < (int)fbH) {
            // Z24S8: upper 24 bits are depth, lower 8 bits are stencil
            uint32_t z24s8 = depthData[y * fbW + x];
            uint32_t z24 = z24s8 >> 8;
            // Convert 24-bit depth to 16-bit
            uint16_t z16 = (uint16_t)(z24 >> 8);
            result[coord] = z16;
        } else {
            result[coord] = 0;
        }
    }

    return result;
}

void* GfxRenderingAPIDeko3d::GetFramebufferTextureId(int fbId) {
    if (fbId == 0 && mCurrentSwapImage >= 0) {
        return (void*)(intptr_t)(&mSwapchainImages[mCurrentSwapImage]);
    }
    if ((size_t)fbId < mFrameBuffers.size()) {
        return (void*)(intptr_t)(&mFrameBuffers[fbId].colorImage);
    }
    return nullptr;
}

void GfxRenderingAPIDeko3d::SelectTextureFb(int fbId) {
    // Bind a framebuffer's color image as a texture source.
    // Insert a barrier to ensure prior rendering to the FB is complete before sampling.
    mCmdBuf.barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);

    if ((size_t)fbId < mFrameBuffers.size() && mFrameBuffers[fbId].descriptorIdx > 0) {
        uint32_t descIdx = mFrameBuffers[fbId].descriptorIdx;
        DkResHandle handle = dkMakeTextureHandle(descIdx, descIdx);
        mCmdBuf.bindTextures(DkStage_Fragment, 0,
                             dk::detail::ArrayProxy<DkResHandle const>(1, &handle));
    }
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
    if ((uint32_t)id < mTextures.size() && mTextures[id].valid) {
        return (ImTextureID)(intptr_t)id;
    }
    return (ImTextureID)0;
}

// ---------------------------------------------------------------------------
// ImGui draw data rendering
// ---------------------------------------------------------------------------

void GfxRenderingAPIDeko3d::RenderImGuiDrawData(ImDrawData* drawData) {
    if (!drawData || drawData->CmdListsCount == 0 || !mShadersLoaded) {
        return;
    }

    // Save current state
    size_t savedVboOffset = mVboOffset;
    size_t savedUniformOffset = mUniformOffset;

    // Set up orthographic projection via viewport
    float L = drawData->DisplayPos.x;
    float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
    float T = drawData->DisplayPos.y;
    float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
    ImVec2 clip_off = drawData->DisplayPos;
    ImVec2 clip_scale = drawData->FramebufferScale;

    // Disable depth test for UI overlay
    dk::DepthStencilState dsState;
    dsState.setDepthTestEnable(false).setDepthWriteEnable(false);
    mCmdBuf.bindDepthStencilState(dsState);

    // Enable alpha blending
    dk::ColorState colorState;
    colorState.setBlendEnable(0, true);
    mCmdBuf.bindColorState(colorState);

    dk::BlendState blendState;
    blendState.setFactors(DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
                          DkBlendFactor_One, DkBlendFactor_InvSrcAlpha);
    blendState.setOps(DkBlendOp_Add, DkBlendOp_Add);
    mCmdBuf.bindBlendStates(0, dk::detail::ArrayProxy<DkBlendState const>(1, &blendState));

    // Set viewport to cover the full display
    DkViewport vp;
    vp.x = L;
    vp.y = T;
    vp.width = R - L;
    vp.height = B - T;
    vp.near = 0.0f;
    vp.far = 1.0f;
    mCmdBuf.setViewports(0, dk::detail::ArrayProxy<DkViewport const>(1, &vp));

    for (int n = 0; n < drawData->CmdListsCount; n++) {
        const ImDrawList* cmd_list = drawData->CmdLists[n];

        // Upload vertex data — ImGui vertices are {pos.x, pos.y, uv.x, uv.y, col}
        // We need to convert ImDrawVert to our uber-shader format.
        // ImDrawVert: ImVec2 pos, ImVec2 uv, ImU32 col
        //
        // For the uber-shader, we set up uniforms so that:
        //   - output = vInput1 (which carries the vertex color)
        //   - texture is sampled and multiplied
        //
        // We pack each ImDrawVert as:
        //   aVtxPos = vec4(ndc_x, ndc_y, 0, 1)
        //   aTexCoord0 = vec2(uv)
        //   aInput1 = vec4(r, g, b, a) normalized

        size_t vtxCount = cmd_list->VtxBuffer.Size;
        size_t idxCount = cmd_list->IdxBuffer.Size;

        // Each vertex: 4 (pos) + 2 (tex) + 4 (color) = 10 floats
        size_t floatsPerVert = 10;
        size_t uploadBytes = vtxCount * floatsPerVert * sizeof(float);

        if (mVboOffset + uploadBytes > VBO_POOL_SIZE) {
            FlushCommands();
            mQueue.waitIdle();
            mVboOffset = 0;
        }

        float* vboDst = (float*)((uint8_t*)mVboMem.getCpuAddr() + mVboOffset);

        for (int i = 0; i < (int)vtxCount; i++) {
            const ImDrawVert& v = cmd_list->VtxBuffer[i];

            // Convert screen-space position to NDC
            float ndcX = (v.pos.x - L) / (R - L) * 2.0f - 1.0f;
            float ndcY = (v.pos.y - T) / (B - T) * 2.0f - 1.0f;

            // Position
            vboDst[0] = ndcX;
            vboDst[1] = ndcY;
            vboDst[2] = 0.0f;
            vboDst[3] = 1.0f;

            // UV
            vboDst[4] = v.uv.x;
            vboDst[5] = v.uv.y;

            // Color (RGBA normalized from packed u32)
            vboDst[6] = (float)((v.col >> 0) & 0xFF) / 255.0f;
            vboDst[7] = (float)((v.col >> 8) & 0xFF) / 255.0f;
            vboDst[8] = (float)((v.col >> 16) & 0xFF) / 255.0f;
            vboDst[9] = (float)((v.col >> 24) & 0xFF) / 255.0f;

            vboDst += floatsPerVert;
        }

        // Bind vertex buffer
        DkBufExtents vboBuf;
        vboBuf.addr = mVboMem.getGpuAddr() + mVboOffset;
        vboBuf.size = (uint32_t)uploadBytes;
        mCmdBuf.bindVtxBuffers(0, dk::detail::ArrayProxy<DkBufExtents const>(1, &vboBuf));

        // Set up vertex attributes: pos(4) + texcoord(2) + color(4)
        DkVtxAttribState attribs[3];
        memset(attribs, 0, sizeof(attribs));

        // aVtxPos (location 0): vec4 at offset 0
        attribs[0].bufferId = 0;
        attribs[0].offset = 0;
        attribs[0].size = DkVtxAttribSize_4x32;
        attribs[0].type = DkVtxAttribType_Float;

        // aTexCoord0 (location 1): vec2 at offset 16
        attribs[1].bufferId = 0;
        attribs[1].offset = 4 * sizeof(float);
        attribs[1].size = DkVtxAttribSize_2x32;
        attribs[1].type = DkVtxAttribType_Float;

        // aInput1 (location 9): vec4 at offset 24
        // But we need to map it to location 9 in the shader.
        // Unfortunately deko3d binds attribs by index, not by location.
        // We need to bind all 10 attrib slots (0-9) with slots 2-8 as dummy.
        DkVtxAttribState fullAttribs[10];
        memset(fullAttribs, 0, sizeof(fullAttribs));

        // Location 0: aVtxPos (vec4)
        fullAttribs[0].bufferId = 0;
        fullAttribs[0].offset = 0;
        fullAttribs[0].size = DkVtxAttribSize_4x32;
        fullAttribs[0].type = DkVtxAttribType_Float;

        // Location 1: aTexCoord0 (vec2)
        fullAttribs[1].bufferId = 0;
        fullAttribs[1].offset = 4 * sizeof(float);
        fullAttribs[1].size = DkVtxAttribSize_2x32;
        fullAttribs[1].type = DkVtxAttribType_Float;

        // Locations 2-8: dummy (isFixed = 1 means use fixed value 0)
        for (int j = 2; j <= 8; j++) {
            fullAttribs[j].bufferId = 0;
            fullAttribs[j].isFixed = 1;
            fullAttribs[j].offset = 0;
            fullAttribs[j].size = DkVtxAttribSize_1x32;
            fullAttribs[j].type = DkVtxAttribType_Float;
        }

        // Location 9: aInput1 (vec4) — the vertex color
        fullAttribs[9].bufferId = 0;
        fullAttribs[9].offset = 6 * sizeof(float);
        fullAttribs[9].size = DkVtxAttribSize_4x32;
        fullAttribs[9].type = DkVtxAttribType_Float;

        DkVtxBufferState bufState;
        memset(&bufState, 0, sizeof(bufState));
        bufState.stride = (uint32_t)(floatsPerVert * sizeof(float));

        mCmdBuf.bindVtxAttribState(dk::detail::ArrayProxy<DkVtxAttribState const>(10, fullAttribs));
        mCmdBuf.bindVtxBufferState(dk::detail::ArrayProxy<DkVtxBufferState const>(1, &bufState));

        // Set up uniforms for ImGui rendering:
        // Use SHADER_TEXEL0 * SHADER_INPUT_1 formula
        // c[0][0] = {SHADER_TEXEL0, SHADER_0, SHADER_INPUT_1, SHADER_0} → texel0 * input1
        // c[0][1] = {SHADER_TEXEL0A, SHADER_0, SHADER_INPUT_1, SHADER_0} → texel0.a * input1.a
        if (mUniformOffset + sizeof(UberUniforms) > UNIFORM_POOL_SIZE) {
            FlushCommands();
            mQueue.waitIdle();
            mUniformOffset = 0;
        }

        UberUniforms* uniforms = (UberUniforms*)((uint8_t*)mUniformMem.getCpuAddr() + mUniformOffset);
        memset(uniforms, 0, sizeof(UberUniforms));

        // Set up color combiner: texel0.rgb * input1.rgb
        uniforms->cc_0_0[0] = 8;  // SHADER_TEXEL0 = a
        uniforms->cc_0_0[1] = 0;  // SHADER_0 = b
        uniforms->cc_0_0[2] = 1;  // SHADER_INPUT_1 = c
        uniforms->cc_0_0[3] = 0;  // SHADER_0 = d → (a - b) * c + d = texel0 * input1

        // Alpha combiner: texel0.a * input1.a
        uniforms->cc_0_1[0] = 9;  // SHADER_TEXEL0A = a
        uniforms->cc_0_1[1] = 0;  // SHADER_0 = b
        uniforms->cc_0_1[2] = 1;  // SHADER_INPUT_1 = c
        uniforms->cc_0_1[3] = 0;  // SHADER_0 = d

        uniforms->useTexture[0] = 1;
        uniforms->optAlpha = 1;
        uniforms->doMultiply[0] = 1; // Multiply mode for RGB
        uniforms->doMultiply[1] = 1; // Multiply mode for Alpha

        DkGpuAddr uboAddr = mUniformMem.getGpuAddr() + mUniformOffset;
        mCmdBuf.bindUniformBuffer(DkStage_Fragment, 0, uboAddr, sizeof(UberUniforms));
        mCmdBuf.bindUniformBuffer(DkStage_Vertex, 0, uboAddr, sizeof(UberUniforms));

        mUniformOffset += sizeof(UberUniforms);
        mUniformOffset = (mUniformOffset + DK_UNIFORM_BUF_ALIGNMENT - 1) & ~(DK_UNIFORM_BUF_ALIGNMENT - 1);

        mVboOffset += uploadBytes;
        mVboOffset = (mVboOffset + 255) & ~(size_t)255;

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];

            if (pcmd->UserCallback) {
                pcmd->UserCallback(cmd_list, pcmd);
                continue;
            }

            // Set scissor rect
            ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x,
                            (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
            ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x,
                            (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);
            if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) {
                continue;
            }

            DkScissor sc;
            sc.x = (uint32_t)clip_min.x;
            sc.y = (uint32_t)clip_min.y;
            sc.width = (uint32_t)(clip_max.x - clip_min.x);
            sc.height = (uint32_t)(clip_max.y - clip_min.y);
            mCmdBuf.setScissors(0, dk::detail::ArrayProxy<DkScissor const>(1, &sc));

            // Bind texture
            uint32_t texId = (uint32_t)(intptr_t)pcmd->GetTexID();
            if (texId < mTextures.size() && mTextures[texId].valid) {
                uint32_t descIdx = mTextures[texId].descriptorIdx;
                DkResHandle handle = dkMakeTextureHandle(descIdx, descIdx);
                mCmdBuf.bindTextures(DkStage_Fragment, 0,
                                     dk::detail::ArrayProxy<DkResHandle const>(1, &handle));
            }

            // Draw indexed triangles using vertex offset
            // ImGui provides indices as ImDrawIdx (uint16_t by default).
            // deko3d doesn't have a base vertex offset for indexed draws,
            // so we draw non-indexed using the index buffer to look up vertices.
            // For simplicity, draw each triangle by emitting 3 vertices per index triple.
            uint32_t firstVtx = pcmd->VtxOffset;
            uint32_t firstIdx = pcmd->IdxOffset;
            uint32_t elemCount = pcmd->ElemCount;

            // We already uploaded all vertices; draw using the vertex offset
            mCmdBuf.draw(DkPrimitive_Triangles, elemCount, 1, firstVtx + firstIdx, 0);
        }
    }

    // Restore full scissor
    uint32_t fbW = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    uint32_t fbH = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    DkScissor fullSc;
    fullSc.x = 0;
    fullSc.y = 0;
    fullSc.width = fbW;
    fullSc.height = fbH;
    mCmdBuf.setScissors(0, dk::detail::ArrayProxy<DkScissor const>(1, &fullSc));
}

} // namespace Fast

#endif // ENABLE_DEKO3D
