//
//  gfx_metal.cpp
//  libultraship
//
//  Created by David Chavez on 16.08.22.
//

#ifdef __APPLE__

#include "fast/backends/gfx_metal.h"

#include <vector>
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <time.h>
#include <math.h>
#include <cmath>
#include <stddef.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>

#include <SDL_render.h>
#include <imgui_impl_metal.h>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include "fast/backends/gfx_metal_shader.h"

#include "libultraship/libultra/abi.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/resource/ResourceManager.h"

#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreGraphics/CoreGraphics.h>

// macOS HDR experiment settings (see GfxRenderingAPIMetal::ApplyColorSettings).
#define CVAR_HDR_HIGH_PRECISION "gHdr.HighPrecision"
#define CVAR_HDR_AUTO "gHdr.AutoHdr"
#define CVAR_HDR_AUTO_PEAK "gHdr.AutoHdrPeak"
#define CVAR_HDR_AUTO_SHOULDER_START "gHdr.AutoHdrShoulderStart"
#define CVAR_HDR_AUTO_SHOULDER_POW "gHdr.AutoHdrShoulderPow"
#define CVAR_HDR_HEADROOM_READOUT "gHdr.Internal.Headroom"

namespace {
// CAMetalLayer's colorspace and EDR switches are not wrapped by metal-cpp, so set them through the ObjC runtime.
void SetLayerExtendedRange(CA::MetalLayer* layer, bool extended) {
    CGColorSpaceRef colorspace = extended ? CGColorSpaceCreateWithName(kCGColorSpaceExtendedSRGB) : nullptr;
    ((void (*)(id, SEL, CGColorSpaceRef))objc_msgSend)((id)layer, sel_registerName("setColorspace:"), colorspace);
    if (colorspace != nullptr) {
        CGColorSpaceRelease(colorspace);
    }
    ((void (*)(id, SEL, BOOL))objc_msgSend)((id)layer, sel_registerName("setWantsExtendedDynamicRangeContent:"),
                                            extended ? YES : NO);
}

// How many times brighter than SDR white the main display can show right now (1.0 = no HDR headroom).
// Changes with the brightness setting, so it is polled rather than read once.
float QueryEdrHeadroom() {
    id screen = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSScreen"), sel_registerName("mainScreen"));
    if (screen == nullptr) {
        return 1.0f;
    }
    double headroom = ((double (*)(id, SEL))objc_msgSend)(
        screen, sel_registerName("maximumExtendedDynamicRangeColorComponentValue"));
    return headroom > 1.0 ? (float)headroom : 1.0f;
}

MTL::PixelFormat ColorFormat(bool high_precision) {
    return high_precision ? MTL::PixelFormatRGBA16Float : MTL::PixelFormatBGRA8Unorm;
}

float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 113;
            while ((mantissa & 0x400) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000 | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

uint32_t UnitToFiveBits(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (uint32_t)(v * 255.0f) >> 3;
}

// Copies one image into another of the same size but a different color format, pixel for pixel. Used when the
// 16-bit setting flips so images that persist across frames (e.g. the frozen pause-menu background) keep their
// contents instead of going blank.
const char* kFormatCopyShader = R"(
#include <metal_stdlib>
using namespace metal;

vertex float4 formatCopyVertex(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

fragment float4 formatCopyFragment(float4 pos [[position]], texture2d<float, access::read> src [[texture(0)]]) {
    return src.read(uint2(pos.xy));
}
)";

// Auto-HDR after Filippo Tarpini's (Filoppi) PumboAutoHDR and his Dolphin port (Data/Sys/Shaders/AutoHDR.glsl):
// leave darker colors alone and push the brightest ones past SDR white along a curve, up to `peak` times SDR white.
// Works in linear light on the game image's display-encoded colors and writes extended sRGB, matching the window's
// extended sRGB colorspace.
const char* kAutoHdrShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; float2 uv; };
struct AutoHdrParams { float peak; float shoulderStart; float shoulderPow; float pad; };

vertex VOut autoHdrVertex(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

static float3 srgbToLinear(float3 c) {
    float3 a = abs(c);
    return sign(c) * select(pow((a + 0.055) / 1.055, 2.4), a / 12.92, a <= 0.04045);
}
static float3 linearToSrgb(float3 c) {
    float3 a = abs(c);
    return sign(c) * select(1.055 * pow(a, 1.0 / 2.4) - 0.055, a * 12.92, a <= 0.0031308);
}

fragment float4 autoHdrFragment(VOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                                constant AutoHdrParams& p [[buffer(0)]]) {
    constexpr sampler s(coord::normalized, filter::nearest, address::clamp_to_edge);
    float4 c = src.sample(s, in.uv);
    float3 lin = srgbToLinear(saturate(c.rgb));
    float y = dot(lin, float3(0.2126, 0.7152, 0.0722));
    if (p.peak > 1.0 && y > 0.0 && y > p.shoulderStart && p.shoulderStart < 1.0) {
        float shoulder = 1.0 - (max(1.0 - y, 0.0) / (1.0 - p.shoulderStart));
        float extra = pow(shoulder, p.shoulderPow) * (p.peak - 1.0);
        lin *= (y + extra) / y;
    }
    return float4(linearToSrgb(lin), c.a);
}
)";
} // namespace

#define ARRAY_COUNT(arr) (int32_t)(sizeof(arr) / sizeof(arr[0]))

// MARK: - Helpers
namespace Fast {

GfxRenderingAPIMetal::GfxRenderingAPIMetal(std::shared_ptr<Ship::ConsoleVariable> consoleVariable,
                                           std::shared_ptr<Ship::ResourceManager> resourceManager)
    : mConsoleVariable(std::move(consoleVariable)), mResourceManager(std::move(resourceManager)) {
}

static MTL::SamplerAddressMode gfx_cm_to_metal(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return MTL::SamplerAddressModeClampToEdge;
        case G_TX_MIRROR | G_TX_WRAP:
            return MTL::SamplerAddressModeMirrorRepeat;
        case G_TX_MIRROR | G_TX_CLAMP:
            return MTL::SamplerAddressModeMirrorClampToEdge;
        case G_TX_NOMIRROR | G_TX_WRAP:
            return MTL::SamplerAddressModeRepeat;
    }

    return MTL::SamplerAddressModeClampToEdge;
}

// MARK: - ImGui & SDL Wrappers

void GfxRenderingAPIMetal::CopyIntoNewFormat(std::vector<std::pair<MTL::Texture*, MTL::Texture*>>& copies) {
    if (copies.empty()) {
        return;
    }
    int slot = mHighPrecision ? 1 : 0;
    if (mFormatCopyPipelines[slot] == nullptr) {
        NS::Error* error = nullptr;
        MTL::Library* library =
            mDevice->newLibrary(NS::String::string(kFormatCopyShader, NS::UTF8StringEncoding), nullptr, &error);
        if (library != nullptr) {
            MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
            MTL::Function* vs = library->newFunction(NS::String::string("formatCopyVertex", NS::UTF8StringEncoding));
            MTL::Function* fs =
                library->newFunction(NS::String::string("formatCopyFragment", NS::UTF8StringEncoding));
            desc->setVertexFunction(vs);
            desc->setFragmentFunction(fs);
            desc->colorAttachments()->object(0)->setPixelFormat(ColorFormat(mHighPrecision));
            mFormatCopyPipelines[slot] = mDevice->newRenderPipelineState(desc, &error);
            vs->release();
            fs->release();
            desc->release();
            library->release();
        }
        if (mFormatCopyPipelines[slot] == nullptr) {
            SPDLOG_ERROR("Failed to create the format copy pipeline; persistent images restart blank");
        }
    }

    // Committed before this frame's command buffers exist, so the copies land before anything reads them.
    // The command buffer keeps the old textures alive until it has run.
    MTL::CommandBuffer* command_buffer = mCommandQueue->commandBuffer();
    for (auto& [old_texture, new_texture] : copies) {
        if (mFormatCopyPipelines[slot] != nullptr) {
            MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            pass->colorAttachments()->object(0)->setTexture(new_texture);
            pass->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionDontCare);
            pass->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
            MTL::RenderCommandEncoder* encoder = command_buffer->renderCommandEncoder(pass);
            encoder->setRenderPipelineState(mFormatCopyPipelines[slot]);
            encoder->setFragmentTexture(old_texture, 0);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
            encoder->endEncoding();
        }
        old_texture->release();
    }
    command_buffer->commit();
    copies.clear();
}

bool GfxRenderingAPIMetal::NonUniformThreadGroupSupported() {
#ifdef __IOS__
    // iOS devices with A11 or later support dispatch threads
    return mDevice->supportsFamily(MTL::GPUFamilyApple4);
#else
    // macOS devices with Metal 2 support dispatch threads
    return mDevice->supportsFamily(MTL::GPUFamilyMac2);
#endif
}

bool GfxRenderingAPIMetal::MetalInit(SDL_Renderer* renderer) {
    mRenderer = renderer;
    NS::AutoreleasePool* autorelease_pool = NS::AutoreleasePool::alloc()->init();

    mLayer = (CA::MetalLayer*)SDL_RenderGetMetalLayer(renderer);
    mLayer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);

    mDevice = mLayer->device();
    mCommandQueue = mDevice->newCommandQueue();
    mReadbackQueue = mDevice->newCommandQueue();

    for (size_t i = 0; i < kMaxVertexBufferPoolSize; i++) {
        MTL::Buffer* new_buffer = mDevice->newBuffer(kVertexBufferBaseSize, MTL::ResourceStorageModeShared);
        mVertexBufferPool[i] = new_buffer;
        mVertexBufferCapacity[i] = kVertexBufferBaseSize;
    }

    autorelease_pool->release();
    mNonUniformThreadgroupSupported = NonUniformThreadGroupSupported();

    return ImGui_ImplMetal_Init(mDevice);
}

static void SetupScreenFramebuffer(uint32_t width, uint32_t height);

void GfxRenderingAPIMetal::NewFrame() {
    ApplyColorSettings();

    int width, height;
    SDL_GetRendererOutputSize(mRenderer, &width, &height);
    SetupScreenFramebuffer(width, height);

    MTL::RenderPassDescriptor* current_render_pass = mFramebuffers[0].mRenderPassDescriptor;
    ImGui_ImplMetal_NewFrame(current_render_pass);
}

void GfxRenderingAPIMetal::SetupFloatingFrame() {
    // We need the descriptor for the main framebuffer and to clear the existing depth attachment
    // so that we can set ImGui up again for our floating windows. Helps avoid Metal API validation issues.
    MTL::RenderPassDescriptor* current_render_pass = mFramebuffers[0].mRenderPassDescriptor;
    current_render_pass->setDepthAttachment(nullptr);
    ImGui_ImplMetal_NewFrame(current_render_pass);
}

void GfxRenderingAPIMetal::RenderDrawData(ImDrawData* drawData) {
    auto framebuffer = mFramebuffers[0];

    // Workaround for detecting when transitioning to/from full screen mode.
    MTL::Texture* screen_texture = mTextures[framebuffer.mTextureId].texture;
    int fb_width = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    int fb_height = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (screen_texture->width() != fb_width || screen_texture->height() != fb_height)
        return;

    ImGui_ImplMetal_RenderDrawData(drawData, framebuffer.mCommandBuffer, framebuffer.mCommandEncoder);
}

// MARK: - Metal Graphics Rendering API

const char* GfxRenderingAPIMetal::GetName() {
    return "Metal";
}

int GfxRenderingAPIMetal::GetMaxTextureSize() {
    return mDevice->supportsFamily(MTL::GPUFamilyApple3) ? 16384 : 8192;
}

void GfxRenderingAPIMetal::Init() {
    // Create the default framebuffer which represents the window
    FramebufferMetal& fb = mFramebuffers[CreateFramebuffer()];
    fb.mMsaaLevel = 1;

    // Check device for supported msaa levels
    for (uint32_t sample_count = 1; sample_count <= METAL_MAX_MULTISAMPLE_SAMPLE_COUNT; sample_count++) {
        if (mDevice->supportsTextureSampleCount(sample_count)) {
            mMsaaNumQualityLevels[sample_count - 1] = 1;
        } else {
            mMsaaNumQualityLevels[sample_count - 1] = 0;
        }
    }

    // Compute shader for retrieving depth values
    const char* depth_shader = R"(
        #include <metal_stdlib>
        using namespace metal;

        struct CoordUniforms {
            uint2 coords[1024];
        };

        kernel void depthKernel(depth2d<float, access::read> depth_texture [[ texture(0) ]],
                                     constant CoordUniforms& query_coords [[ buffer(0) ]],
                                     device float* output_values [[ buffer(1) ]],
                                     ushort2 thread_position [[ thread_position_in_grid ]]) {
            uint2 coord = query_coords.coords[thread_position.x];
            output_values[thread_position.x] = depth_texture.read(coord);
        }

        kernel void convertToRGB5A1(texture2d<half, access::read> inTexture [[ texture(0) ]],
                                    device short* outputBuffer [[ buffer(0) ]],
                                    uint2 gid [[ thread_position_in_grid ]]) {
            uint index = gid.x + (inTexture.get_width() * gid.y);
            half4 pixel = inTexture.read(gid);
            uint r = pixel.r * 0x1F;
            uint g = pixel.g * 0x1F;
            uint b = pixel.b * 0x1F;
            uint a = pixel.a > 0;
            outputBuffer[index] = (r << 11) | (g << 6) | (b << 1) | a;
        }
    )";

    NS::AutoreleasePool* autorelease_pool = NS::AutoreleasePool::alloc()->init();

    NS::Error* error = nullptr;
    MTL::Library* library =
        mDevice->newLibrary(NS::String::string(depth_shader, NS::UTF8StringEncoding), nullptr, &error);

    if (error != nullptr)
        SPDLOG_ERROR("Failed to compile shader library: {}",
                     error->localizedDescription()->cString(NS::UTF8StringEncoding));

    mDepthComputeFunction = library->newFunction(NS::String::string("depthKernel", NS::UTF8StringEncoding));
    mConvertToRgb5a1Function = library->newFunction(NS::String::string("convertToRGB5A1", NS::UTF8StringEncoding));

    library->release();
    autorelease_pool->release();

    gfx_metal_shader_set_resource_manager(mResourceManager);
}

struct GfxClipParameters GfxRenderingAPIMetal::GetClipParameters() {
    return { true, false };
}

void GfxRenderingAPIMetal::UnloadShader(struct ShaderProgram* old_prg) {
}

void GfxRenderingAPIMetal::LoadShader(struct ShaderProgram* new_prg) {
    mShaderProgram = (struct ShaderProgramMetal*)new_prg;
}

void GfxRenderingAPIMetal::ClearShaderCache() {
    for (auto& [key, prg] : mShaderProgramPool) {
        prg.markedForDeletion = true;
    }
}

struct ShaderProgram* GfxRenderingAPIMetal::CreateAndLoadNewShader(uint64_t shader_id0, uint64_t shader_id1) {
    CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    size_t numFloats = 0;
    std::string buf;
    NS::AutoreleasePool* autorelease_pool = NS::AutoreleasePool::alloc()->init();

    MTL::VertexDescriptor* vertex_descriptor =
        gfx_metal_build_shader(buf, numFloats, cc_features, mCurrentFilterMode == FILTER_THREE_POINT);

    NS::Error* error = nullptr;
    MTL::Library* library =
        mDevice->newLibrary(NS::String::string(buf.data(), NS::UTF8StringEncoding), nullptr, &error);

    if (library == nullptr || error != nullptr) {
        SPDLOG_ERROR("Failed to compile shader library, error {}",
                     error ? error->localizedDescription()->cString(NS::UTF8StringEncoding) : "(null library, no error)");
        autorelease_pool->release();
        return nullptr;
    }

    MTL::RenderPipelineDescriptor* pipeline_descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    MTL::Function* vertexFunc = library->newFunction(NS::String::string("vertexShader", NS::UTF8StringEncoding));
    MTL::Function* fragmentFunc = library->newFunction(NS::String::string("fragmentShader", NS::UTF8StringEncoding));

    if (vertexFunc == nullptr || fragmentFunc == nullptr) {
        SPDLOG_ERROR("Failed to find shader functions in compiled library (vertexShader={}, fragmentShader={})",
                     vertexFunc != nullptr, fragmentFunc != nullptr);
        pipeline_descriptor->release();
        library->release();
        autorelease_pool->release();
        return nullptr;
    }

    pipeline_descriptor->setVertexFunction(vertexFunc);
    pipeline_descriptor->setFragmentFunction(fragmentFunc);
    pipeline_descriptor->setVertexDescriptor(vertex_descriptor);

    pipeline_descriptor->colorAttachments()->object(0)->setPixelFormat(ColorFormat(mHighPrecision));
    pipeline_descriptor->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    if (cc_features.opt_alpha) {
        pipeline_descriptor->colorAttachments()->object(0)->setBlendingEnabled(true);
        pipeline_descriptor->colorAttachments()->object(0)->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
        pipeline_descriptor->colorAttachments()->object(0)->setDestinationRGBBlendFactor(
            MTL::BlendFactorOneMinusSourceAlpha);
        pipeline_descriptor->colorAttachments()->object(0)->setRgbBlendOperation(MTL::BlendOperationAdd);
        pipeline_descriptor->colorAttachments()->object(0)->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
        pipeline_descriptor->colorAttachments()->object(0)->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
        pipeline_descriptor->colorAttachments()->object(0)->setAlphaBlendOperation(MTL::BlendOperationAdd);
        pipeline_descriptor->colorAttachments()->object(0)->setWriteMask(MTL::ColorWriteMaskAll);
    } else {
        pipeline_descriptor->colorAttachments()->object(0)->setBlendingEnabled(false);
        pipeline_descriptor->colorAttachments()->object(0)->setWriteMask(MTL::ColorWriteMaskAll);
    }

    struct ShaderProgramMetal* prg = &mShaderProgramPool[std::make_pair(shader_id0, shader_id1)];
    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    prg->usedTextures[0] = cc_features.usedTextures[0];
    prg->usedTextures[1] = cc_features.usedTextures[1];
    prg->usedTextures[2] = cc_features.used_masks[0];
    prg->usedTextures[3] = cc_features.used_masks[1];
    prg->usedTextures[4] = cc_features.used_blend[0];
    prg->usedTextures[5] = cc_features.used_blend[1];
    prg->usedTextures[SHADER_PALETTE_TEXTURE] = cc_features.used_palette[0] || cc_features.used_palette[1];
    prg->numInputs = cc_features.numInputs;
    prg->numFloats = numFloats;
    prg->usedLighting = cc_features.opt_lighting || cc_features.opt_texgen;

    // Prepoluate pipeline state cache with program and available msaa levels
    for (int i = 0; i < ARRAY_COUNT(mMsaaNumQualityLevels); i++) {
        if (mMsaaNumQualityLevels[i] == 1) {
            int msaa_level = i + 1;
            pipeline_descriptor->setSampleCount(msaa_level);
            MTL::RenderPipelineState* pipeline_state = mDevice->newRenderPipelineState(pipeline_descriptor, &error);

            if (!pipeline_state || error != nullptr) {
                // Pipeline State creation could fail if we haven't properly set up our pipeline descriptor.
                // If the Metal API validation is enabled, we can find out more information about what
                // went wrong.  (Metal API validation is enabled by default when a debug build is run
                // from Xcode)
                SPDLOG_ERROR("Failed to create pipeline state, error {}",
                             error->localizedDescription()->cString(NS::UTF8StringEncoding));
            }

            if (mHighPrecision) {
                prg->pipeline_state_variants_f16[msaa_level] = pipeline_state;
            } else {
                prg->pipeline_state_variants[msaa_level] = pipeline_state;
            }
        }
    }

    LoadShader((struct ShaderProgram*)prg);

    vertexFunc->release();
    fragmentFunc->release();
    library->release();
    // Kept so the other color format's pipelines can be built when the 16-bit setting is toggled.
    prg->pipeline_descriptor = pipeline_descriptor;
    autorelease_pool->release();

    return (struct ShaderProgram*)prg;
}

struct ShaderProgram* GfxRenderingAPIMetal::LookupShader(uint64_t shader_id0, uint64_t shader_id1) {
    auto it = mShaderProgramPool.find(std::make_pair(shader_id0, shader_id1));

    if (it == mShaderProgramPool.end()) {
        return nullptr;
    }

    struct ShaderProgramMetal* prg = &it->second;

    if (prg->markedForDeletion) {
        mShaderProgramPool.erase(it);
        return nullptr;
    }

    return it == mShaderProgramPool.end() ? nullptr : (struct ShaderProgram*)&it->second;
}

void GfxRenderingAPIMetal::ShaderGetInfo(struct ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    struct ShaderProgramMetal* p = (struct ShaderProgramMetal*)prg;

    *numInputs = p->numInputs;
    usedTextures[0] = p->usedTextures[0];
    usedTextures[1] = p->usedTextures[1];
}

uint32_t GfxRenderingAPIMetal::NewTexture() {
    mTextures.resize(mTextures.size() + 1);
    return (uint32_t)(mTextures.size() - 1);
}

void GfxRenderingAPIMetal::DeleteTexture(uint32_t texID) {
}

void GfxRenderingAPIMetal::SelectTexture(int tile, uint32_t texture_id) {
    mCurrentTile = tile;
    mCurrentTextureIds[tile] = texture_id;
}

void GfxRenderingAPIMetal::UploadTexture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }

    TextureDataMetal* texture_data = &mTextures[mCurrentTextureIds[mCurrentTile]];

    NS::AutoreleasePool* autorelease_pool = NS::AutoreleasePool::alloc()->init();

    MTL::TextureDescriptor* texture_descriptor =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, width, height, true);
    texture_descriptor->setArrayLength(1);
    texture_descriptor->setMipmapLevelCount(1);
    texture_descriptor->setSampleCount(1);
    texture_descriptor->setStorageMode(MTL::StorageModeShared);

    MTL::Region region = MTL::Region::Make2D(0, 0, width, height);

    MTL::Texture* texture = texture_data->texture;
    if (texture_data->texture == nullptr || texture_data->texture->width() != width ||
        texture_data->texture->height() != height || texture_data->texture->mipmapLevelCount() != 1) {
        if (texture_data->texture != nullptr)
            texture_data->texture->release();

        texture = mDevice->newTexture(texture_descriptor);
    }

    NS::UInteger bytes_per_pixel = 4;
    NS::UInteger bytes_per_row = bytes_per_pixel * width;
    texture->replaceRegion(region, 0, rgba32_buf, bytes_per_row);
    texture_data->texture = texture;
    texture_data->mip_levels = 1;
    texture_data->auto_mipmaps = false;

    autorelease_pool->release();
}

void GfxRenderingAPIMetal::UploadTextureMip(const uint8_t* rgba32_buf, uint32_t width, uint32_t height, uint32_t level,
                                            uint32_t totalLevels) {
    if (width == 0 || height == 0) {
        return;
    }

    TextureDataMetal* texture_data = &mTextures[mCurrentTextureIds[mCurrentTile]];

    NS::AutoreleasePool* autorelease_pool = NS::AutoreleasePool::alloc()->init();

    if (level == 0) {
        if (texture_data->texture == nullptr || texture_data->texture->width() != width ||
            texture_data->texture->height() != height || texture_data->texture->mipmapLevelCount() != totalLevels) {
            if (texture_data->texture != nullptr)
                texture_data->texture->release();

            MTL::TextureDescriptor* texture_descriptor =
                MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, width, height, true);
            texture_descriptor->setArrayLength(1);
            texture_descriptor->setMipmapLevelCount(totalLevels);
            texture_descriptor->setSampleCount(1);
            texture_descriptor->setStorageMode(MTL::StorageModeShared);
            texture_data->texture = mDevice->newTexture(texture_descriptor);
        }
        texture_data->mip_levels = totalLevels;
        texture_data->auto_mipmaps = mNextTextureAutoMipmap;
    }

    if (texture_data->texture != nullptr && level < texture_data->texture->mipmapLevelCount()) {
        MTL::Region region = MTL::Region::Make2D(0, 0, width, height);
        texture_data->texture->replaceRegion(region, level, rgba32_buf, 4 * width);
    }

    autorelease_pool->release();
}

void GfxRenderingAPIMetal::SetSamplerParameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    TextureDataMetal* texture_data = &mTextures[mCurrentTextureIds[tile]];
    texture_data->linear_filtering = linear_filter;
    texture_data->filtering = !linear_filter ? FILTER_LINEAR : FILTER_THREE_POINT;

    // This function is called twice per texture, the first one only to set default values.
    // Maybe that could be skipped? Anyway, make sure to release the first default sampler
    // state before setting the actual one.
    if (texture_data->sampler != nullptr) {
        texture_data->sampler->release();
    }

    MTL::SamplerDescriptor* sampler_descriptor = MTL::SamplerDescriptor::alloc()->init();
    MTL::SamplerMinMagFilter filter = linear_filter && mCurrentFilterMode == FILTER_LINEAR
                                          ? MTL::SamplerMinMagFilterLinear
                                          : MTL::SamplerMinMagFilterNearest;
    sampler_descriptor->setMinFilter(filter);
    sampler_descriptor->setMagFilter(filter);
    // Native N64 mip chains are sampled with explicit integer LODs (level()) in the
    // shader; Nearest picks the exact level. CPU auto-generated pyramids use hardware
    // derivative LOD with trilinear blending (no per-pixel stipple) + anisotropy.
    if (texture_data->auto_mipmaps) {
        sampler_descriptor->setMipFilter(MTL::SamplerMipFilterLinear);
        sampler_descriptor->setMaxAnisotropy(8);
    } else {
        sampler_descriptor->setMipFilter(MTL::SamplerMipFilterNearest);
    }
    sampler_descriptor->setSAddressMode(gfx_cm_to_metal(cms));
    sampler_descriptor->setTAddressMode(gfx_cm_to_metal(cmt));
    sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeRepeat);

    texture_data->sampler = mDevice->newSamplerState(sampler_descriptor);
    sampler_descriptor->release();
}

void GfxRenderingAPIMetal::SetDepthTestAndMask(bool depth_test, bool depth_mask) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = depth_mask;
}

void GfxRenderingAPIMetal::SetCurrentPrimDepth(float depth) {
    if (depth != mCurrentPrimDepth) {
        mCurrentPrimDepth = depth;
        mPrimDepthDirty = true;
    }
}

void GfxRenderingAPIMetal::SetCurrentMaxLod(float maxLod) {
    if (maxLod != mCurrentMaxLod) {
        mCurrentMaxLod = maxLod;
        mLodMaxDirty = true;
    }
}

void GfxRenderingAPIMetal::SetZmodeDecal(bool zmode_decal) {
    mCurrentZmodeDecal = zmode_decal;
}

void GfxRenderingAPIMetal::SetStrictDecal(bool on) {
    mCurrentStrictDecal = on;
}

void GfxRenderingAPIMetal::SetViewport(int x, int y, int width, int height) {
    FramebufferMetal& fb = mFramebuffers[mCurrentFramebuffer];

    fb.mViewport->originX = x;
    fb.mViewport->originY = mRenderTargetHeight - y - height;
    fb.mViewport->width = width;
    fb.mViewport->height = height;
    fb.mViewport->znear = 0;
    fb.mViewport->zfar = 1;

    fb.mCommandEncoder->setViewport(*fb.mViewport);
}

void GfxRenderingAPIMetal::SetScissor(int x, int y, int width, int height) {
    FramebufferMetal& fb = mFramebuffers[mCurrentFramebuffer];
    TextureDataMetal tex = mTextures[fb.mTextureId];

    // clamp to viewport size as metal does not support larger values than viewport size
    fb.mScissorRect->x = std::max(0, std::min<int>(x, tex.width));
    fb.mScissorRect->y = std::max(0, std::min<int>(mRenderTargetHeight - y - height, tex.height));
    fb.mScissorRect->width = std::max(0, std::min<int>(width, tex.width));
    fb.mScissorRect->height = std::max(0, std::min<int>(height, tex.height));

    fb.mCommandEncoder->setScissorRect(*fb.mScissorRect);
}

void GfxRenderingAPIMetal::SetUseAlpha(bool use_alpha) {
    // Already part of the pipeline state from shader info
}

void GfxRenderingAPIMetal::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    NS::AutoreleasePool* autorelease_pool = NS::AutoreleasePool::alloc()->init();
    bool textures_changed = false;

    auto& current_framebuffer = mFramebuffers[mCurrentFramebuffer];

    if (current_framebuffer.mLastDepthTest != mCurrentDepthTest ||
        current_framebuffer.mLastDepthMask != mCurrentDepthMask ||
        current_framebuffer.mLastStrictDecal != mCurrentStrictDecal ||
        current_framebuffer.mLastZmodeDecal != mCurrentZmodeDecal) {
        current_framebuffer.mLastDepthTest = mCurrentDepthTest;
        current_framebuffer.mLastDepthMask = mCurrentDepthMask;
        current_framebuffer.mLastStrictDecal = mCurrentStrictDecal;

        MTL::DepthStencilDescriptor* depth_descriptor = MTL::DepthStencilDescriptor::alloc()->init();
        depth_descriptor->setDepthWriteEnabled(mCurrentDepthMask);
        depth_descriptor->setDepthCompareFunction(
            mCurrentDepthTest ? (mCurrentZmodeDecal
                                     ? (mCurrentStrictDecal ? MTL::CompareFunctionEqual : MTL::CompareFunctionLessEqual)
                                     // LessEqual (not Less) so coplanar transparent overlays
                                     // (e.g. DynOS recolorable parts) at equal depth still draw
                                     : MTL::CompareFunctionLessEqual)
                              : MTL::CompareFunctionAlways);

        MTL::DepthStencilState* depth_stencil_state = mDevice->newDepthStencilState(depth_descriptor);
        current_framebuffer.mCommandEncoder->setDepthStencilState(depth_stencil_state);

        depth_descriptor->release();
    }

    if (current_framebuffer.mLastZmodeDecal != mCurrentZmodeDecal) {
        current_framebuffer.mLastZmodeDecal = mCurrentZmodeDecal;

        current_framebuffer.mCommandEncoder->setTriangleFillMode(MTL::TriangleFillModeFill);
        current_framebuffer.mCommandEncoder->setFrontFacingWinding(MTL::WindingCounterClockwise);

        // SSDB = SlopeScaledDepthBias 120 leads to -2 at 240p which is the same as N64 mode which has very little
        // fighting
        const int n64modeFactor = 120;
        const int noVanishFactor = 100;
        float SSDB = -2;
        switch (mConsoleVariable->GetInteger(CVAR_Z_FIGHTING_MODE, 0)) {
            case 1: // scaled z-fighting (N64 mode like)
                SSDB = -1.0f * (float)mRenderTargetHeight / n64modeFactor;
                break;
            case 2: // no vanishing paths
                SSDB = -1.0f * (float)mRenderTargetHeight / noVanishFactor;
                break;
            case 0: // disabled
            default:
                SSDB = -2;
        }
        current_framebuffer.mCommandEncoder->setDepthBias(0, (mCurrentZmodeDecal && !mCurrentStrictDecal) ? SSDB : 0,
                                                          0);
    }

    MTL::Buffer* vertex_buffer = mVertexBufferPool[mCurrentVertexBufferPoolIndex];
    const size_t needed = mCurrentVertexBufferOffset + sizeof(float) * buf_vbo_len;
    if (needed > mVertexBufferPeakThisFrame) {
        mVertexBufferPeakThisFrame = needed;
    }
    if (needed > mVertexBufferCapacity[mCurrentVertexBufferPoolIndex]) {
        // StartFrame grows the buffer to the previous frame's peak; if a single frame spikes
        // past even that, drop this batch rather than overrun the buffer (would SIGBUS). The
        // peak above ensures next frame's buffer is sized to fit. Warn once to stay visible.
        static bool warned = false;
        if (!warned) {
            warned = true;
            SPDLOG_WARN("metal: vertex batch ({} B) exceeds buffer ({} B); dropping this frame, growing next",
                        needed, mVertexBufferCapacity[mCurrentVertexBufferPoolIndex]);
        }
        return;
    }
    memcpy((char*)vertex_buffer->contents() + mCurrentVertexBufferOffset, buf_vbo, sizeof(float) * buf_vbo_len);

    if (!current_framebuffer.mHasBoundVertexShader) {
        current_framebuffer.mCommandEncoder->setVertexBuffer(vertex_buffer, 0, 0);
        current_framebuffer.mHasBoundVertexShader = true;
    }

    current_framebuffer.mCommandEncoder->setVertexBufferOffset(mCurrentVertexBufferOffset, 0);

    if (!current_framebuffer.mHasBoundFragShader) {
        current_framebuffer.mCommandEncoder->setFragmentBuffer(mFrameUniformBuffer, 0, 0);
        current_framebuffer.mHasBoundFragShader = true;
    }

    // Lighting/texgen uniforms for the vertex shader. setVertexBytes is per-encoder
    // state, so re-send for every lit draw (encoders are recreated per frame).
    if (mShaderProgram->usedLighting) {
        current_framebuffer.mCommandEncoder->setVertexBytes(&mLightingUniforms, sizeof(LightingUniforms), 1);
        mLightingUniformsDirty = false;
    }

    for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
        if (mShaderProgram->usedTextures[i]) {
            if (current_framebuffer.mLastBoundTextures[i] != mTextures[mCurrentTextureIds[i]].texture) {
                current_framebuffer.mLastBoundTextures[i] = mTextures[mCurrentTextureIds[i]].texture;
                current_framebuffer.mCommandEncoder->setFragmentTexture(mTextures[mCurrentTextureIds[i]].texture, i);

                if (current_framebuffer.mLastBoundSamplers[i] != mTextures[mCurrentTextureIds[i]].sampler) {
                    current_framebuffer.mLastBoundSamplers[i] = mTextures[mCurrentTextureIds[i]].sampler;
                    current_framebuffer.mCommandEncoder->setFragmentSamplerState(
                        mTextures[mCurrentTextureIds[i]].sampler, i);
                }
            }
        }

        if (mCurrentFilterMode == FILTER_THREE_POINT) {
            mDrawUniforms.textureFiltering[i] = mTextures[mCurrentTextureIds[i]].filtering;
            mDrawUniforms.textureFiltering[i] = mTextures[mCurrentTextureIds[i]].filtering;
            textures_changed = true;
        }
    }

    if (textures_changed || mPrimDepthDirty || mLodMaxDirty || mCombinerUniformsDirty || mCustomUniformsDirty) {
        mDrawUniforms.prim_depth = mCurrentPrimDepth;
        mDrawUniforms.lod_max = mCurrentMaxLod;
        for (int i = 0; i < 6; i++) {
            mDrawUniforms.inputs[i] = simd::float4{ mCombinerUniforms.inputs[i][0], mCombinerUniforms.inputs[i][1],
                                                    mCombinerUniforms.inputs[i][2], mCombinerUniforms.inputs[i][3] };
        }
        mDrawUniforms.fog_color = simd::float4{ mCombinerUniforms.fog_color[0], mCombinerUniforms.fog_color[1],
                                                mCombinerUniforms.fog_color[2], mCombinerUniforms.fog_color[3] };
        mDrawUniforms.grayscale_color =
            simd::float4{ mCombinerUniforms.grayscale_color[0], mCombinerUniforms.grayscale_color[1],
                          mCombinerUniforms.grayscale_color[2], mCombinerUniforms.grayscale_color[3] };
        for (int i = 0; i < 2; i++) {
            mDrawUniforms.uv_transform[i] =
                simd::float4{ mCombinerUniforms.uv_transform[i][0], mCombinerUniforms.uv_transform[i][1],
                              mCombinerUniforms.uv_transform[i][2], mCombinerUniforms.uv_transform[i][3] };
            mDrawUniforms.texture_clamp[i] =
                simd::float4{ mCombinerUniforms.texture_clamp[i][0], mCombinerUniforms.texture_clamp[i][1],
                              mCombinerUniforms.texture_clamp[i][2], mCombinerUniforms.texture_clamp[i][3] };
        }
        mDrawUniforms.fog_params = simd::float4{ mCombinerUniforms.fog_params[0], mCombinerUniforms.fog_params[1],
                                                 mCombinerUniforms.fog_params[2], mCombinerUniforms.fog_params[3] };
        for (int i = 0; i < 2; i++) {
            mDrawUniforms.palette_params[i] =
                simd::float4{ mCombinerUniforms.palette_params[i][0], mCombinerUniforms.palette_params[i][1],
                              mCombinerUniforms.palette_params[i][2], mCombinerUniforms.palette_params[i][3] };
        }
        mDrawUniforms.lod_params = simd::float4{ mCombinerUniforms.lod_params[0], mCombinerUniforms.lod_params[1],
                                                 mCombinerUniforms.lod_params[2], mCombinerUniforms.lod_params[3] };
        mDrawUniforms.debug_tint = simd::float4{ mCombinerUniforms.debug_tint[0], mCombinerUniforms.debug_tint[1],
                                                 mCombinerUniforms.debug_tint[2], mCombinerUniforms.debug_tint[3] };
        for (int i = 0; i < GFX_NUM_CUSTOM_UNIFORMS; i++) {
            mDrawUniforms.uCustom[i] = simd::float4{ mCustomUniforms.regs[i][0], mCustomUniforms.regs[i][1],
                                                     mCustomUniforms.regs[i][2], mCustomUniforms.regs[i][3] };
        }
        current_framebuffer.mCommandEncoder->setFragmentBytes(&mDrawUniforms, sizeof(DrawUniforms), 1);
        mPrimDepthDirty = false;
        mLodMaxDirty = false;
        mCombinerUniformsDirty = false;
        mCustomUniformsDirty = false;
    }

    // The vertex shader reads the same DrawUniforms (UV transform, fog params);
    // setVertexBytes is per-encoder state, so re-send every draw.
    current_framebuffer.mCommandEncoder->setVertexBytes(&mDrawUniforms, sizeof(DrawUniforms), 2);
    // Matrix palette + y flip for the vertex shader
    current_framebuffer.mCommandEncoder->setVertexBytes(&mTransformUniforms, sizeof(TransformUniforms), 3);
    mTransformUniformsDirty = false;

    // N64 backface culling: Metal NDC keeps y up and the front face is CCW; with
    // no VS y flip the signed area A = -C, so keeping C > 0 keeps clockwise
    // triangles, i.e. cull the front (CCW) faces.
    MTL::CullMode cull = mCurrentCullKeepSign > 0 ? MTL::CullModeFront
                                                  : (mCurrentCullKeepSign < 0 ? MTL::CullModeBack : MTL::CullModeNone);
    current_framebuffer.mCommandEncoder->setCullMode(cull);

    if (current_framebuffer.mLastShaderProgram != mShaderProgram) {
        current_framebuffer.mLastShaderProgram = mShaderProgram;

        MTL::RenderPipelineState* pipeline_state =
            GetPipelineState(mShaderProgram,
                             mTextures[current_framebuffer.mTextureId].texture->pixelFormat() ==
                                 MTL::PixelFormatRGBA16Float,
                             current_framebuffer.mMsaaLevel);
        current_framebuffer.mCommandEncoder->setRenderPipelineState(pipeline_state);
    }

    current_framebuffer.mCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle, 0.f, buf_vbo_num_tris * 3);
    mCurrentVertexBufferOffset += sizeof(float) * buf_vbo_len;

    autorelease_pool->release();
}

void GfxRenderingAPIMetal::OnResize() {
}

void GfxRenderingAPIMetal::StartFrame() {
    mFrameUniforms.frameCount++;
    if (mFrameUniforms.frameCount > 150) {
        // No high values, as noise starts to look ugly
        mFrameUniforms.frameCount = 0;
    }

    if (!mFrameUniformBuffer) {
        mFrameUniformBuffer = mDevice->newBuffer(sizeof(FrameUniforms), MTL::ResourceCPUCacheModeDefaultCache);
    }
    if (!mCoordUniformBuffer) {
        mCoordUniformBuffer = mDevice->newBuffer(sizeof(CoordUniforms), MTL::ResourceCPUCacheModeDefaultCache);
    }

    // Grow the buffer we're about to use to fit last frame's peak vertex volume. The pool
    // index advances at end-of-frame, so this buffer was last used kMaxVertexBufferPoolSize
    // frames ago — the GPU is done with it, making release+realloc here safe.
    const int vbIdx = mCurrentVertexBufferPoolIndex;
    if (mVertexBufferPeakLastFrame > mVertexBufferCapacity[vbIdx]) {
        size_t newCap = mVertexBufferCapacity[vbIdx];
        while (newCap < mVertexBufferPeakLastFrame) {
            newCap *= 2;
        }
        mVertexBufferPool[vbIdx]->release();
        mVertexBufferPool[vbIdx] = mDevice->newBuffer(newCap, MTL::ResourceStorageModeShared);
        mVertexBufferCapacity[vbIdx] = newCap;
    }
    mVertexBufferPeakLastFrame = mVertexBufferPeakThisFrame;
    mVertexBufferPeakThisFrame = 0;

    mCurrentVertexBufferOffset = 0;

    mFrameAutoreleasePool = NS::AutoreleasePool::alloc()->init();
}

void GfxRenderingAPIMetal::EndFrame() {
    std::set<int>::iterator it = mDrawnFramebuffers.begin();
    it++;

    while (it != mDrawnFramebuffers.end()) {
        auto& framebuffer = mFramebuffers[*it];

        if (!framebuffer.mHasEndedEncoding)
            framebuffer.mCommandEncoder->endEncoding();

        framebuffer.mCommandBuffer->commit();
        it++;
    }

    auto& screen_framebuffer = mFramebuffers[0];
    if (!screen_framebuffer.mHasEndedEncoding)
        screen_framebuffer.mCommandEncoder->endEncoding();

    // Deferred screen FB readback: blit the rendered texture into a shared
    // MTLBuffer using a blit encoder (cheap DMA — no render pass boundary,
    // no TBDR tile flush/reload).  The CPU reads this buffer next frame in
    // ReadFramebufferToCPU and converts BGRA8 → RGBA5551 on the CPU side.
    if (mScreenReadbackRequested) {
        MTL::Texture* screenTex = mTextures[screen_framebuffer.mTextureId].texture;
        uint32_t w = mScreenReadbackWidth;
        uint32_t h = mScreenReadbackHeight;
        mScreenReadbackIsHalf = screenTex->pixelFormat() == MTL::PixelFormatRGBA16Float;
        size_t bytesPerRow = w * (mScreenReadbackIsHalf ? 8 : 4); // RGBA16F = 8 bytes, BGRA8 = 4 bytes per pixel
        size_t bufSize = bytesPerRow * h;

        // Allocate / reuse a persistent shared buffer for CPU readback.
        if (!mScreenReadbackBuffer || mScreenReadbackBuffer->length() < bufSize) {
            if (mScreenReadbackBuffer)
                mScreenReadbackBuffer->release();
            mScreenReadbackBuffer = mDevice->newBuffer(bufSize, MTL::ResourceStorageModeShared);
        }

        MTL::BlitCommandEncoder* blit = screen_framebuffer.mCommandBuffer->blitCommandEncoder();
        blit->copyFromTexture(screenTex, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w, h, 1), mScreenReadbackBuffer, 0,
                              bytesPerRow, bufSize);
        blit->endEncoding();

        // Don't set mScreenReadbackDataReady yet — the blit hasn't executed.
        // Retain the command buffer so ReadFramebufferToCPU can wait on it.
        mScreenReadbackRequested = false;
    }

    if (mCurrentDrawable != nullptr) {
        screen_framebuffer.mCommandBuffer->presentDrawable(mCurrentDrawable);
    }
    mCurrentVertexBufferPoolIndex = (mCurrentVertexBufferPoolIndex + 1) % kMaxVertexBufferPoolSize;
    screen_framebuffer.mCommandBuffer->commit();

    // Now that commit has been called, retain the command buffer for GPU sync
    // in ReadFramebufferToCPU next frame (waitUntilCompleted before reading).
    if (mScreenReadbackBuffer && !mScreenReadbackDataReady) {
        mScreenReadbackCmdBuf = screen_framebuffer.mCommandBuffer->retain();
        mScreenReadbackDataReady = true;
    } else if (mScreenReadbackDataReady && mScreenReadbackCmdBuf) {
        // Nobody consumed the readback this frame — data is going stale.
        // Release the retained command buffer to avoid leaking it, and reset
        // so the next ReadFramebufferToCPU requests a fresh blit instead of
        // serving outdated pixels.
        mScreenReadbackCmdBuf->release();
        mScreenReadbackCmdBuf = nullptr;
        mScreenReadbackDataReady = false;
    }

    mDrawnFramebuffers.clear();

    // Cleanup states
    for (int fb_id = 0; fb_id < (int)mFramebuffers.size(); fb_id++) {
        FramebufferMetal& fb = mFramebuffers[fb_id];

        fb.mLastShaderProgram = nullptr;
        fb.mCommandBuffer = nullptr;
        fb.mCommandEncoder = nullptr;
        fb.mHasEndedEncoding = false;
        fb.mHasBoundVertexShader = false;
        fb.mHasBoundFragShader = false;
        for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
            fb.mLastBoundTextures[i] = nullptr;
            fb.mLastBoundSamplers[i] = nullptr;
        }
        memset(fb.mViewport, 0, sizeof(MTL::Viewport));
        memset(fb.mScissorRect, 0, sizeof(MTL::ScissorRect));
        fb.mLastDepthTest = -1;
        fb.mLastDepthMask = -1;
        fb.mLastZmodeDecal = -1;
        fb.mLastStrictDecal = -1;
    }

    mFrameAutoreleasePool->release();
}

void GfxRenderingAPIMetal::FinishRender() {
}

int GfxRenderingAPIMetal::CreateFramebuffer() {
    uint32_t texture_id = NewTexture();
    TextureDataMetal& t = mTextures[texture_id];

    size_t index = mFramebuffers.size();
    mFramebuffers.resize(mFramebuffers.size() + 1);
    FramebufferMetal& data = mFramebuffers.back();
    data.mScissorRect = new MTL::ScissorRect();
    data.mViewport = new MTL::Viewport();
    data.mTextureId = texture_id;

    uint32_t tile = 0;
    uint32_t saved = mCurrentTextureIds[tile];
    mCurrentTextureIds[tile] = texture_id;
    SetSamplerParameters(0, true, G_TX_WRAP, G_TX_WRAP);
    mCurrentTextureIds[tile] = saved;

    return (int)index;
}

void GfxRenderingAPIMetal::SetupScreenFramebuffer(uint32_t width, uint32_t height) {
#ifdef __IOS__
    // Keep the layer's drawable size pinned to the pixel viewport.
    if (mLayer->drawableSize().width != width || mLayer->drawableSize().height != height) {
        mLayer->setDrawableSize({ CGFloat(width), CGFloat(height) });
    }
#endif
    mCurrentDrawable = nullptr;
    mCurrentDrawable = mLayer->nextDrawable();

    bool msaa_enabled = mConsoleVariable->GetInteger("gMSAAValue", 1) > 1;

    FramebufferMetal& fb = mFramebuffers[0];
    TextureDataMetal& tex = mTextures[fb.mTextureId];

    NS::AutoreleasePool* autorelease_pool = NS::AutoreleasePool::alloc()->init();

    // Safety net: the visibility gate in Fast3dWindow normally prevents us from
    // reaching here while occluded, but if the window is occluded mid-frame
    // nextDrawable can still return null. Don't dereference it (would crash);
    // keep the previous texture for this frame.
    if (mCurrentDrawable == nullptr) {
        autorelease_pool->release();
        return;
    }

    tex.texture = mCurrentDrawable->texture();

    MTL::RenderPassDescriptor* render_pass_descriptor = MTL::RenderPassDescriptor::renderPassDescriptor();
    render_pass_descriptor->colorAttachments()->object(0)->setTexture(tex.texture);
    render_pass_descriptor->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionLoad);
    render_pass_descriptor->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);

    tex.width = width;
    tex.height = height;

    // recreate depth texture only if necessary (size changed)
    if (fb.mDepthTexture == nullptr || (fb.mDepthTexture->width() != width || fb.mDepthTexture->height() != height)) {
        if (fb.mDepthTexture != nullptr)
            fb.mDepthTexture->release();

        // If possible, we eventually we want to disable this when msaa is enabled since we don't need this depth
        // texture However, problem is if the user switches to msaa during game, we need a way to then generate it
        // before drawing.
        MTL::TextureDescriptor* depth_tex_desc =
            MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatDepth32Float, width, height, true);

        depth_tex_desc->setTextureType(MTL::TextureType2D);
        depth_tex_desc->setStorageMode(MTL::StorageModePrivate);
        depth_tex_desc->setSampleCount(1);
        depth_tex_desc->setArrayLength(1);
        depth_tex_desc->setMipmapLevelCount(1);
        depth_tex_desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);

        fb.mDepthTexture = mDevice->newTexture(depth_tex_desc);
    }

    render_pass_descriptor->depthAttachment()->setTexture(fb.mDepthTexture);
    render_pass_descriptor->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
    render_pass_descriptor->depthAttachment()->setStoreAction(MTL::StoreActionStore);
    render_pass_descriptor->depthAttachment()->setClearDepth(1);

    if (fb.mRenderPassDescriptor != nullptr)
        fb.mRenderPassDescriptor->release();

    fb.mRenderPassDescriptor = render_pass_descriptor;
    fb.mRenderPassDescriptor->retain();
    fb.mRenderTarget = true;
    fb.mHasDepthBuffer = true;

    autorelease_pool->release();
}

void GfxRenderingAPIMetal::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                       bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                                       bool can_extract_depth) {
    // Screen framebuffer is handled separately on a frame by frame basis
    // see `SetupScreenFramebuffer`.
    if (fb_id == 0) {
        int width, height;
        SDL_GetRendererOutputSize(mRenderer, &width, &height);
        mLayer->setDrawableSize({ CGFloat(width), CGFloat(height) });

        return;
    }

    FramebufferMetal& fb = mFramebuffers[fb_id];
    TextureDataMetal& tex = mTextures[fb.mTextureId];

    width = std::max(width, 1U);
    height = std::max(height, 1U);
    while (msaa_level > 1 && mMsaaNumQualityLevels[msaa_level - 1] == 0) {
        --msaa_level;
    }

    bool diff = tex.width != width || tex.height != height || fb.mMsaaLevel != msaa_level;
    bool format_changed =
        tex.texture != nullptr && tex.texture->pixelFormat() != ColorFormat(mHighPrecision);

    NS::AutoreleasePool* autorelease_pool = NS::AutoreleasePool::alloc()->init();

    if (diff || format_changed || (fb.mRenderPassDescriptor != nullptr) != render_target) {
        MTL::TextureDescriptor* tex_descriptor = MTL::TextureDescriptor::alloc()->init();
        tex_descriptor->setTextureType(MTL::TextureType2D);
        tex_descriptor->setWidth(width);
        tex_descriptor->setHeight(height);
        tex_descriptor->setSampleCount(1);
        tex_descriptor->setMipmapLevelCount(1);
        tex_descriptor->setPixelFormat(ColorFormat(mHighPrecision));
        tex_descriptor->setUsage((render_target ? MTL::TextureUsageRenderTarget : 0) | MTL::TextureUsageShaderRead);

        if (tex.texture != nullptr)
            tex.texture->release();

        tex.texture = mDevice->newTexture(tex_descriptor);

        if (msaa_level > 1) {
            tex_descriptor->setTextureType(MTL::TextureType2DMultisample);
            tex_descriptor->setSampleCount(msaa_level);
            tex_descriptor->setStorageMode(MTL::StorageModePrivate);
            tex_descriptor->setUsage(render_target ? MTL::TextureUsageRenderTarget : 0);

            if (tex.msaaTexture != nullptr)
                tex.msaaTexture->release();
            tex.msaaTexture = mDevice->newTexture(tex_descriptor);
        }

        if (render_target) {
            MTL::RenderPassDescriptor* render_pass_descriptor = MTL::RenderPassDescriptor::renderPassDescriptor();

            bool fb_msaa_enabled = (msaa_level > 1);
            bool game_msaa_enabled = mConsoleVariable->GetInteger("gMSAAValue", 1) > 1;

            if (fb_msaa_enabled) {
                render_pass_descriptor->colorAttachments()->object(0)->setTexture(tex.msaaTexture);
                render_pass_descriptor->colorAttachments()->object(0)->setResolveTexture(tex.texture);
                render_pass_descriptor->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionLoad);
                render_pass_descriptor->colorAttachments()->object(0)->setStoreAction(
                    MTL::StoreActionStoreAndMultisampleResolve);
            } else {
                render_pass_descriptor->colorAttachments()->object(0)->setTexture(tex.texture);
                render_pass_descriptor->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionLoad);
                render_pass_descriptor->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
            }

            if (fb.mRenderPassDescriptor != nullptr)
                fb.mRenderPassDescriptor->release();

            fb.mRenderPassDescriptor = render_pass_descriptor;
            fb.mRenderPassDescriptor->retain();
        }

        tex.width = width;
        tex.height = height;

        tex_descriptor->release();
    }

    if (has_depth_buffer && (diff || !fb.mHasDepthBuffer || (fb.mDepthTexture != nullptr) != can_extract_depth)) {
        MTL::TextureDescriptor* depth_tex_desc =
            MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatDepth32Float, width, height, true);
        depth_tex_desc->setTextureType(MTL::TextureType2D);
        depth_tex_desc->setStorageMode(MTL::StorageModePrivate);
        depth_tex_desc->setSampleCount(1);
        depth_tex_desc->setArrayLength(1);
        depth_tex_desc->setMipmapLevelCount(1);
        depth_tex_desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);

        if (fb.mDepthTexture != nullptr)
            fb.mDepthTexture->release();

        fb.mDepthTexture = mDevice->newTexture(depth_tex_desc);

        if (msaa_level > 1) {
            depth_tex_desc->setTextureType(MTL::TextureType2DMultisample);
            depth_tex_desc->setSampleCount(msaa_level);

            if (fb.mMsaaDepthTexture != nullptr)
                fb.mMsaaDepthTexture->release();

            fb.mMsaaDepthTexture = mDevice->newTexture(depth_tex_desc);
        }
    }

    if (has_depth_buffer && fb.mRenderPassDescriptor != nullptr) {
        if (msaa_level > 1) {
            fb.mRenderPassDescriptor->depthAttachment()->setTexture(fb.mMsaaDepthTexture);
            fb.mRenderPassDescriptor->depthAttachment()->setResolveTexture(fb.mDepthTexture);
            fb.mRenderPassDescriptor->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
            fb.mRenderPassDescriptor->depthAttachment()->setStoreAction(MTL::StoreActionMultisampleResolve);
            fb.mRenderPassDescriptor->depthAttachment()->setClearDepth(1);
        } else {
            fb.mRenderPassDescriptor->depthAttachment()->setTexture(fb.mDepthTexture);
            fb.mRenderPassDescriptor->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
            fb.mRenderPassDescriptor->depthAttachment()->setStoreAction(MTL::StoreActionStore);
            fb.mRenderPassDescriptor->depthAttachment()->setClearDepth(1);
        }
    } else if (fb.mRenderPassDescriptor != nullptr) {
        fb.mRenderPassDescriptor->setDepthAttachment(nullptr);
    }

    fb.mRenderTarget = render_target;
    fb.mHasDepthBuffer = has_depth_buffer;
    fb.mMsaaLevel = msaa_level;

    autorelease_pool->release();
}

void GfxRenderingAPIMetal::StartDrawToFramebuffer(int fb_id, float noise_scale) {
    FramebufferMetal& fb = mFramebuffers[fb_id];
    mRenderTargetHeight = mTextures[fb.mTextureId].height;

    mCurrentFramebuffer = fb_id;
    mDrawnFramebuffers.insert(fb_id);

    if (fb.mRenderTarget && fb.mCommandBuffer == nullptr && fb.mCommandEncoder == nullptr &&
        fb.mRenderPassDescriptor != nullptr) {
        // FBs that are read back to CPU use a separate queue so
        // commit+waitUntilCompleted doesn't deadlock on enqueue ordering.
        MTL::CommandQueue* queue = fb.mUseReadbackQueue ? mReadbackQueue : mCommandQueue;
        fb.mCommandBuffer = queue->commandBuffer();
        std::string fbcb_label = fmt::format("FrameBuffer {} Command Buffer", fb_id);
        fb.mCommandBuffer->setLabel(NS::String::string(fbcb_label.c_str(), NS::UTF8StringEncoding));

        if (!fb.mUseReadbackQueue) {
            // Queue the command buffers in order of start draw
            fb.mCommandBuffer->enqueue();
        }

        fb.mCommandEncoder = fb.mCommandBuffer->renderCommandEncoder(fb.mRenderPassDescriptor);
        std::string fbce_label = fmt::format("FrameBuffer {} Command Encoder", fb_id);
        fb.mCommandEncoder->setLabel(NS::String::string(fbce_label.c_str(), NS::UTF8StringEncoding));
        fb.mCommandEncoder->setDepthClipMode(MTL::DepthClipModeClamp);
    }

    if (noise_scale != 0.0f) {
        mFrameUniforms.noiseScale = 1.0f / noise_scale;
    }

    memcpy(mFrameUniformBuffer->contents(), &mFrameUniforms, sizeof(FrameUniforms));
}

void GfxRenderingAPIMetal::ClearFramebuffer(bool color, bool depth) {
    if (!color && !depth) {
        return;
    }

    auto& framebuffer = mFramebuffers[mCurrentFramebuffer];

    // End the current render encoder
    framebuffer.mCommandEncoder->endEncoding();

    // Track the original load action and set the next load actions to Load to leverage the blit results
    MTL::RenderPassColorAttachmentDescriptor* srcColorAttachment =
        framebuffer.mRenderPassDescriptor->colorAttachments()->object(0);
    MTL::LoadAction origLoadAction = srcColorAttachment->loadAction();
    if (color) {
        srcColorAttachment->setLoadAction(MTL::LoadActionClear);
    }

    MTL::RenderPassDepthAttachmentDescriptor* srcDepthAttachment = framebuffer.mRenderPassDescriptor->depthAttachment();
    MTL::LoadAction origDepthLoadAction = MTL::LoadActionDontCare;
    if (depth && framebuffer.mHasDepthBuffer) {
        origDepthLoadAction = srcDepthAttachment->loadAction();
        srcDepthAttachment->setLoadAction(MTL::LoadActionClear);
    }

    // Create a new render encoder back onto the framebuffer
    framebuffer.mCommandEncoder = framebuffer.mCommandBuffer->renderCommandEncoder(framebuffer.mRenderPassDescriptor);

    std::string fbce_label = fmt::format("FrameBuffer {} Command Encoder After Clear", mCurrentFramebuffer);
    framebuffer.mCommandEncoder->setLabel(NS::String::string(fbce_label.c_str(), NS::UTF8StringEncoding));
    framebuffer.mCommandEncoder->setDepthClipMode(MTL::DepthClipModeClamp);
    framebuffer.mCommandEncoder->setViewport(*framebuffer.mViewport);
    framebuffer.mCommandEncoder->setScissorRect(*framebuffer.mScissorRect);

    // Now that the command encoder is started, we set the original load actions back for the next frame's use
    srcColorAttachment->setLoadAction(origLoadAction);
    if (depth && framebuffer.mHasDepthBuffer) {
        srcDepthAttachment->setLoadAction(origDepthLoadAction);
    }

    // Reset the framebuffer so the encoder is setup again when rendering triangles
    framebuffer.mHasBoundVertexShader = false;
    framebuffer.mHasBoundFragShader = false;
    framebuffer.mLastShaderProgram = nullptr;
    for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
        framebuffer.mLastBoundTextures[i] = nullptr;
        framebuffer.mLastBoundSamplers[i] = nullptr;
    }
    framebuffer.mLastDepthTest = -1;
    framebuffer.mLastDepthMask = -1;
    framebuffer.mLastZmodeDecal = -1;
    framebuffer.mLastStrictDecal = -1;

    // setFragmentBytes is per-encoder state, just like setVertexBytes (which is
    // sent unconditionally every draw). The dirty flags are cleared by DrawTriangles,
    // so a new encoder created mid-frame would never receive fragment uniforms
    // (UV transforms, fog, etc.) — causing texture stutter. Force a re-send on the
    // first draw into this new encoder.
    mCombinerUniformsDirty = true;
    mPrimDepthDirty = true;
    mLodMaxDirty = true;
    mCustomUniformsDirty = true;
}

void GfxRenderingAPIMetal::ResolveMSAAColorBuffer(int fb_id_target, int fb_id_source) {
    int source_texture_id = mFramebuffers[fb_id_source].mTextureId;
    MTL::Texture* source_texture = mTextures[source_texture_id].texture;

    int target_texture_id = mFramebuffers[fb_id_target].mTextureId;
    MTL::Texture* target_texture =
        target_texture_id == 0 ? mCurrentDrawable->texture() : mTextures[target_texture_id].texture;

    // Workaround for detecting when transitioning to/from full screen mode.
    if (source_texture->width() != target_texture->width() || source_texture->height() != target_texture->height()) {
        return;
    }

    // When the target buffer is our main window buffer, we need to perform the blit operation on the target
    // buffer instead of the source buffer
    if (fb_id_target != 0) {
        // Copy over the source framebuffer's texture to the target
        auto& source_framebuffer = mFramebuffers[fb_id_source];
        source_framebuffer.mCommandEncoder->endEncoding();
        source_framebuffer.mHasEndedEncoding = true;

        MTL::BlitCommandEncoder* blit_encoder = source_framebuffer.mCommandBuffer->blitCommandEncoder();
        blit_encoder->setLabel(NS::String::string("MSAA Copy Encoder", NS::UTF8StringEncoding));
        blit_encoder->copyFromTexture(source_texture, target_texture);
        blit_encoder->endEncoding();
    } else {
        // End the current render encoder
        auto& target_framebuffer = mFramebuffers[fb_id_target];
        target_framebuffer.mCommandEncoder->endEncoding();

        // Create a blit encoder
        MTL::BlitCommandEncoder* blit_encoder = target_framebuffer.mCommandBuffer->blitCommandEncoder();
        blit_encoder->setLabel(NS::String::string("MSAA Copy Encoder", NS::UTF8StringEncoding));

        // Copy the texture over using the origins and size
        blit_encoder->copyFromTexture(source_texture, target_texture);
        blit_encoder->endEncoding();

        // Update the load action to Load to leverage the blit results
        // The original load action will be set back on the next frame by SetupScreenFramebuffer
        MTL::RenderPassColorAttachmentDescriptor* targetColorAttachment =
            target_framebuffer.mRenderPassDescriptor->colorAttachments()->object(0);
        targetColorAttachment->setLoadAction(MTL::LoadActionLoad);

        // Create a new render encoder back onto the framebuffer
        target_framebuffer.mCommandEncoder =
            target_framebuffer.mCommandBuffer->renderCommandEncoder(target_framebuffer.mRenderPassDescriptor);

        std::string fbce_label = fmt::format("FrameBuffer {} Command Encoder After MSAA Resolve", fb_id_target);
        target_framebuffer.mCommandEncoder->setLabel(NS::String::string(fbce_label.c_str(), NS::UTF8StringEncoding));
    }
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIMetal::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    auto framebuffer = mFramebuffers[fb_id];

    if (coordinates.size() > mCoordBufferSize) {
        if (mDepthValueOutputBuffer != nullptr)
            mDepthValueOutputBuffer->release();

        mDepthValueOutputBuffer =
            mDevice->newBuffer(sizeof(float) * coordinates.size(), MTL::ResourceOptionCPUCacheModeDefault);
        mDepthValueOutputBuffer->setLabel(NS::String::string("Depth output buffer", NS::UTF8StringEncoding));

        mCoordBufferSize = coordinates.size();
    }

    // zero out the buffer
    memset(mCoordUniformBuffer->contents(), 0, sizeof(CoordUniforms));
    memset(mDepthValueOutputBuffer->contents(), 0, sizeof(float) * coordinates.size());

    // map coordinates to right y axis
    size_t i = 0;
    for (const auto& coord : coordinates) {
        mCoordUniforms.coords[i].x = (uint32_t)(int32_t)coord.first;
        mCoordUniforms.coords[i].y = (uint32_t)(int32_t)(framebuffer.mDepthTexture->height() - 1 - coord.second);
        ++i;
    }

    // set uniform values
    memcpy(mCoordUniformBuffer->contents(), &mCoordUniforms, sizeof(CoordUniforms));

    NS::AutoreleasePool* autorelease_pool = NS::AutoreleasePool::alloc()->init();

    auto command_buffer = mCommandQueue->commandBuffer();
    command_buffer->setLabel(NS::String::string("Depth Shader Command Buffer", NS::UTF8StringEncoding));

    NS::Error* error = nullptr;
    MTL::ComputePipelineState* compute_pipeline_state = mDevice->newComputePipelineState(mDepthComputeFunction, &error);

    MTL::ComputeCommandEncoder* compute_encoder = command_buffer->computeCommandEncoder();
    compute_encoder->setComputePipelineState(compute_pipeline_state);
    compute_encoder->setTexture(framebuffer.mDepthTexture, 0);
    compute_encoder->setBuffer(mCoordUniformBuffer, 0, 0);
    compute_encoder->setBuffer(mDepthValueOutputBuffer, 0, 1);

    MTL::Size thread_group_size = MTL::Size::Make(1, 1, 1);
    MTL::Size thread_group_count = MTL::Size::Make(coordinates.size(), 1, 1);

    // We validate if the device supports non-uniform threadgroup sizes
    if (mNonUniformThreadgroupSupported) {
        compute_encoder->dispatchThreads(thread_group_count, thread_group_size);
    } else {
        compute_encoder->dispatchThreadgroups(thread_group_count, thread_group_size);
    }

    compute_encoder->endEncoding();

    command_buffer->commit();
    command_buffer->waitUntilCompleted();

    // Now the depth values can be accessed in the buffer.
    float* depth_values = (float*)mDepthValueOutputBuffer->contents();

    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;
    {
        size_t i = 0;
        for (const auto& coord : coordinates) {
            res.emplace(coord, depth_values[i++] * 65532.0f);
        }
    }

    compute_pipeline_state->release();
    autorelease_pool->release();

    return res;
}

void* GfxRenderingAPIMetal::GetFramebufferTextureId(int fb_id) {
    return (void*)mTextures[mFramebuffers[fb_id].mTextureId].texture;
}

void GfxRenderingAPIMetal::SelectTextureFb(int fb_id) {
    // If the source framebuffer's render encoder is still open, Metal does not
    // automatically synchronize the texture for reading in another encoder —
    // sampling it yields garbage (undefined) data.  End the encoder to finalise
    // the texture content, then immediately reopen with LoadActionLoad so any
    // subsequent rendering to this framebuffer preserves what was drawn.
    FramebufferMetal& src = mFramebuffers[fb_id];
    if (src.mCommandEncoder != nullptr && !src.mHasEndedEncoding) {
        src.mCommandEncoder->endEncoding();

        MTL::RenderPassColorAttachmentDescriptor* colorAttachment =
            src.mRenderPassDescriptor->colorAttachments()->object(0);
        MTL::LoadAction origColorLoad = colorAttachment->loadAction();
        colorAttachment->setLoadAction(MTL::LoadActionLoad);

        MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = src.mRenderPassDescriptor->depthAttachment();
        MTL::LoadAction origDepthLoad = MTL::LoadActionDontCare;
        if (src.mHasDepthBuffer) {
            origDepthLoad = depthAttachment->loadAction();
            depthAttachment->setLoadAction(MTL::LoadActionLoad);
        }

        src.mCommandEncoder = src.mCommandBuffer->renderCommandEncoder(src.mRenderPassDescriptor);
        std::string label = fmt::format("FrameBuffer {} Command Encoder After SelectTextureFb", fb_id);
        src.mCommandEncoder->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
        src.mCommandEncoder->setDepthClipMode(MTL::DepthClipModeClamp);
        src.mCommandEncoder->setViewport(*src.mViewport);
        src.mCommandEncoder->setScissorRect(*src.mScissorRect);

        colorAttachment->setLoadAction(origColorLoad);
        if (src.mHasDepthBuffer) {
            depthAttachment->setLoadAction(origDepthLoad);
        }

        src.mHasBoundVertexShader = false;
        src.mHasBoundFragShader = false;
        src.mLastShaderProgram = nullptr;
        for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
            src.mLastBoundTextures[i] = nullptr;
            src.mLastBoundSamplers[i] = nullptr;
        }
        src.mLastDepthTest = -1;
        src.mLastDepthMask = -1;
        src.mLastZmodeDecal = -1;
        src.mLastStrictDecal = -1;

        // New encoder — force fragment uniform re-send (see comment in ClearFramebuffer).
        mCombinerUniformsDirty = true;
        mPrimDepthDirty = true;
        mLodMaxDirty = true;
        mCustomUniformsDirty = true;
    }

    int tile = 0;
    SelectTexture(tile, src.mTextureId);
}

void GfxRenderingAPIMetal::CopyFramebuffer(int fb_dst_id, int fb_src_id, int srcX0, int srcY0, int srcX1, int srcY1,
                                           int dstX0, int dstY0, int dstX1, int dstY1) {
    if (fb_src_id >= (int)mFramebuffers.size() || fb_dst_id >= (int)mFramebuffers.size()) {
        return;
    }

    FramebufferMetal& source_framebuffer = mFramebuffers[fb_src_id];

    int source_texture_id = source_framebuffer.mTextureId;
    MTL::Texture* source_texture = mTextures[source_texture_id].texture;

    int target_texture_id = mFramebuffers[fb_dst_id].mTextureId;
    MTL::Texture* target_texture = mTextures[target_texture_id].texture;

    // End the current render encoder
    source_framebuffer.mCommandEncoder->endEncoding();

    // Create a blit encoder
    MTL::BlitCommandEncoder* blit_encoder = source_framebuffer.mCommandBuffer->blitCommandEncoder();
    blit_encoder->setLabel(NS::String::string("Copy Framebuffer Encoder", NS::UTF8StringEncoding));

    MTL::Origin source_origin = MTL::Origin(srcX0, srcY0, 0);
    MTL::Origin target_origin = MTL::Origin(dstX0, dstY0, 0);
    MTL::Size source_size = MTL::Size(srcX1 - srcX0, srcY1 - srcY0, 1);

    // Copy the texture over using the origins and size
    blit_encoder->copyFromTexture(source_texture, 0, 0, source_origin, source_size, target_texture, 0, 0,
                                  target_origin);
    blit_encoder->endEncoding();

    // Track the original load action and set the next load actions to Load to leverage the blit results
    MTL::RenderPassColorAttachmentDescriptor* srcColorAttachment =
        source_framebuffer.mRenderPassDescriptor->colorAttachments()->object(0);
    MTL::LoadAction origLoadAction = srcColorAttachment->loadAction();
    srcColorAttachment->setLoadAction(MTL::LoadActionLoad);

    MTL::RenderPassDepthAttachmentDescriptor* srcDepthAttachment =
        source_framebuffer.mRenderPassDescriptor->depthAttachment();
    MTL::LoadAction origDepthLoadAction = MTL::LoadActionDontCare;
    if (source_framebuffer.mHasDepthBuffer) {
        origDepthLoadAction = srcDepthAttachment->loadAction();
        srcDepthAttachment->setLoadAction(MTL::LoadActionLoad);
    }

    // Create a new render encoder back onto the framebuffer
    source_framebuffer.mCommandEncoder =
        source_framebuffer.mCommandBuffer->renderCommandEncoder(source_framebuffer.mRenderPassDescriptor);

    std::string fbce_label = fmt::format("FrameBuffer {} Command Encoder After Copy", fb_src_id);
    source_framebuffer.mCommandEncoder->setLabel(NS::String::string(fbce_label.c_str(), NS::UTF8StringEncoding));
    source_framebuffer.mCommandEncoder->setDepthClipMode(MTL::DepthClipModeClamp);
    source_framebuffer.mCommandEncoder->setViewport(*source_framebuffer.mViewport);
    source_framebuffer.mCommandEncoder->setScissorRect(*source_framebuffer.mScissorRect);

    // Now that the command encoder is started, we set the original load actions back for the next frame's use
    srcColorAttachment->setLoadAction(origLoadAction);
    if (source_framebuffer.mHasDepthBuffer) {
        srcDepthAttachment->setLoadAction(origDepthLoadAction);
    }

    // Reset the framebuffer so the encoder is setup again when rendering triangles
    source_framebuffer.mHasBoundVertexShader = false;
    source_framebuffer.mHasBoundFragShader = false;
    source_framebuffer.mLastShaderProgram = nullptr;
    for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
        source_framebuffer.mLastBoundTextures[i] = nullptr;
        source_framebuffer.mLastBoundSamplers[i] = nullptr;
    }
    source_framebuffer.mLastDepthTest = -1;
    source_framebuffer.mLastDepthMask = -1;
    source_framebuffer.mLastZmodeDecal = -1;
    source_framebuffer.mLastStrictDecal = -1;

    // New encoder — force fragment uniform re-send (see comment in ClearFramebuffer).
    mCombinerUniformsDirty = true;
    mPrimDepthDirty = true;
    mLodMaxDirty = true;
    mCustomUniformsDirty = true;
}

void GfxRenderingAPIMetal::ReadFramebufferToCPU(int fb_id, uint32_t width, uint32_t height, uint16_t* rgba16_buf) {
    if (fb_id >= (int)mFramebuffers.size()) {
        return;
    }

    FramebufferMetal& framebuffer = mFramebuffers[fb_id];
    MTL::Texture* texture = mTextures[framebuffer.mTextureId].texture;

    if (fb_id == 0) {
        // Screen Framebuffer: deferred blit (zero encoder overhead)
        // Return previous frame's data.  Wait on the retained command buffer
        // to guarantee the GPU blit has finished before reading the shared buffer.
        if (mScreenReadbackDataReady && mScreenReadbackBuffer && mScreenReadbackCmdBuf) {
            mScreenReadbackCmdBuf->waitUntilCompleted();
            mScreenReadbackCmdBuf->release();
            mScreenReadbackCmdBuf = nullptr;
            mScreenReadbackDataReady = false;

            uint8_t* bgra = (uint8_t*)mScreenReadbackBuffer->contents();
            uint32_t count = mScreenReadbackWidth * mScreenReadbackHeight;
            if (mScreenReadbackIsHalf) {
                uint16_t* rgba = (uint16_t*)mScreenReadbackBuffer->contents();
                for (uint32_t i = 0; i < count; i++) {
                    rgba16_buf[i] = (UnitToFiveBits(HalfToFloat(rgba[i * 4 + 0])) << 11) |
                                    (UnitToFiveBits(HalfToFloat(rgba[i * 4 + 1])) << 6) |
                                    (UnitToFiveBits(HalfToFloat(rgba[i * 4 + 2])) << 1) |
                                    (HalfToFloat(rgba[i * 4 + 3]) >= 0.5f ? 1 : 0);
                }
                count = 0; // already converted
            }
            for (uint32_t i = 0; i < count; i++) {
                uint8_t b = bgra[i * 4 + 0];
                uint8_t g = bgra[i * 4 + 1];
                uint8_t r = bgra[i * 4 + 2];
                uint8_t a = bgra[i * 4 + 3];
                rgba16_buf[i] = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7);
            }
        }

        // Request a blit in EndFrame and we don't touch any encoders here
        mScreenReadbackRequested = true;
        mScreenReadbackWidth = width;
        mScreenReadbackHeight = height;
    } else {
        // Custom Framebuffer: synchronous readback via readback queue
        if (framebuffer.mCommandEncoder && !framebuffer.mHasEndedEncoding) {
            framebuffer.mCommandEncoder->endEncoding();
        }

        if (!mConvertToRgb5a1PipelineState) {
            NS::Error* pso_error = nullptr;
            mConvertToRgb5a1PipelineState = mDevice->newComputePipelineState(mConvertToRgb5a1Function, &pso_error);
        }

        MTL::Buffer* output_buffer =
            mDevice->newBuffer(sizeof(uint16_t) * width * height, MTL::ResourceStorageModeShared);

        MTL::ComputeCommandEncoder* compute_encoder = framebuffer.mCommandBuffer->computeCommandEncoder();
        compute_encoder->setComputePipelineState(mConvertToRgb5a1PipelineState);
        compute_encoder->setTexture(texture, 0);
        compute_encoder->setBuffer(output_buffer, 0, 0);

        MTL::Size tg = MTL::Size::Make(8, 8, 1);
        MTL::Size total = MTL::Size::Make(width, height, 1);
        if (mNonUniformThreadgroupSupported) {
            compute_encoder->dispatchThreads(total, tg);
        } else {
            compute_encoder->dispatchThreadgroups(MTL::Size::Make((width + 7) / 8, (height + 7) / 8, 1), tg);
        }
        compute_encoder->endEncoding();

        framebuffer.mCommandBuffer->commit();

        if (framebuffer.mUseReadbackQueue) {
            framebuffer.mCommandBuffer->waitUntilCompleted();
            uint16_t* values = (uint16_t*)output_buffer->contents();
            memcpy(rgba16_buf, values, sizeof(uint16_t) * width * height);
        } else {
            // First frame on main queue — can't block without deadlock.
            // Return zeros so the caller gets black instead of garbage.
            memset(rgba16_buf, 0, sizeof(uint16_t) * width * height);
            framebuffer.mUseReadbackQueue = true;
        }

        output_buffer->release();

        // Reset state so StartDrawToFramebuffer can create a fresh command buffer.
        // Example case: the FB can be reused for another sprite in the same frame.
        framebuffer.mCommandBuffer = nullptr;
        framebuffer.mCommandEncoder = nullptr;
        framebuffer.mHasEndedEncoding = false;
        mDrawnFramebuffers.erase(fb_id);
    }
}

void GfxRenderingAPIMetal::SetTextureFilter(FilteringMode mode) {
    mCurrentFilterMode = mode;
    gfx_texture_cache_clear();
}

FilteringMode GfxRenderingAPIMetal::GetTextureFilter() {
    return mCurrentFilterMode;
}

ImTextureID GfxRenderingAPIMetal::GetTextureById(int fb_id) {
    return (void*)mTextures[fb_id].texture;
}

MTL::RenderPipelineState* GfxRenderingAPIMetal::GetPipelineState(ShaderProgramMetal* prg, bool f16,
                                                                  uint32_t msaa_level) {
    MTL::RenderPipelineState*& pipeline_state =
        f16 ? prg->pipeline_state_variants_f16[msaa_level] : prg->pipeline_state_variants[msaa_level];
    if (pipeline_state == nullptr && prg->pipeline_descriptor != nullptr) {
        prg->pipeline_descriptor->colorAttachments()->object(0)->setPixelFormat(ColorFormat(f16));
        prg->pipeline_descriptor->setSampleCount(msaa_level);
        NS::Error* error = nullptr;
        pipeline_state = mDevice->newRenderPipelineState(prg->pipeline_descriptor, &error);
        if (!pipeline_state || error != nullptr) {
            SPDLOG_ERROR("Failed to create pipeline state, error {}",
                         error ? error->localizedDescription()->cString(NS::UTF8StringEncoding) : "unknown");
        }
    }
    return pipeline_state;
}

void GfxRenderingAPIMetal::ApplyColorSettings() {
    auto& cvars = mConsoleVariable;
    bool high_precision = cvars->GetInteger(CVAR_HDR_HIGH_PRECISION, 0) != 0;
    if (high_precision != mHighPrecision) {
        mHighPrecision = high_precision;
        mLayer->setPixelFormat(ColorFormat(high_precision));
        SetLayerExtendedRange(mLayer, high_precision);
        // Recreate every offscreen game image in the new format; the window follows on its next drawable.
        // Drawable single-sample images get their old contents copied across, so persistent ones such as the
        // frozen pause-menu background survive the switch.
        std::vector<std::pair<MTL::Texture*, MTL::Texture*>> carry_over; // (old, new)
        for (int fb_id = 1; fb_id < (int)mFramebuffers.size(); fb_id++) {
            FramebufferMetal& fb = mFramebuffers[fb_id];
            TextureDataMetal& tex = mTextures[fb.mTextureId];
            if (tex.texture != nullptr) {
                MTL::Texture* old_texture = tex.texture->retain();
                UpdateFramebufferParameters(fb_id, tex.width, tex.height, fb.mMsaaLevel, true, fb.mRenderTarget,
                                            fb.mHasDepthBuffer, fb.mDepthTexture != nullptr);
                if (fb.mRenderTarget && fb.mMsaaLevel == 1 && tex.texture != old_texture &&
                    tex.texture->width() == old_texture->width() && tex.texture->height() == old_texture->height()) {
                    carry_over.emplace_back(old_texture, tex.texture);
                } else {
                    old_texture->release();
                }
            }
        }
        CopyIntoNewFormat(carry_over);
        mEdrHeadroomPollFrames = 0;
    }

    mAutoHdr = high_precision && cvars->GetInteger(CVAR_HDR_AUTO, 0) != 0;

    if (high_precision && --mEdrHeadroomPollFrames <= 0) {
        mEdrHeadroom = QueryEdrHeadroom();
        mEdrHeadroomPollFrames = 30;
        cvars->SetFloat(CVAR_HDR_HEADROOM_READOUT, mEdrHeadroom);
    } else if (!high_precision) {
        mEdrHeadroom = 1.0f;
    }
}

void* GfxRenderingAPIMetal::PostProcessGameImage(void* gameImage) {
    if (!mAutoHdr || gameImage == nullptr) {
        return gameImage;
    }
    auto& screen = mFramebuffers[0];
    if (screen.mCommandBuffer == nullptr || screen.mCommandEncoder == nullptr || screen.mHasEndedEncoding) {
        return gameImage;
    }

    MTL::Texture* source = (MTL::Texture*)gameImage;
    if (mAutoHdrTexture == nullptr || mAutoHdrTexture->width() != source->width() ||
        mAutoHdrTexture->height() != source->height()) {
        if (mAutoHdrTexture != nullptr) {
            mAutoHdrTexture->release();
        }
        MTL::TextureDescriptor* desc = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatRGBA16Float, source->width(), source->height(), false);
        desc->setStorageMode(MTL::StorageModePrivate);
        desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        mAutoHdrTexture = mDevice->newTexture(desc);
    }

    if (mAutoHdrPipeline == nullptr) {
        NS::Error* error = nullptr;
        MTL::Library* library =
            mDevice->newLibrary(NS::String::string(kAutoHdrShader, NS::UTF8StringEncoding), nullptr, &error);
        if (library == nullptr) {
            SPDLOG_ERROR("Failed to compile auto-HDR shader: {}",
                         error ? error->localizedDescription()->cString(NS::UTF8StringEncoding) : "unknown");
            return gameImage;
        }
        MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
        MTL::Function* vs = library->newFunction(NS::String::string("autoHdrVertex", NS::UTF8StringEncoding));
        MTL::Function* fs = library->newFunction(NS::String::string("autoHdrFragment", NS::UTF8StringEncoding));
        desc->setVertexFunction(vs);
        desc->setFragmentFunction(fs);
        desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);
        mAutoHdrPipeline = mDevice->newRenderPipelineState(desc, &error);
        vs->release();
        fs->release();
        desc->release();
        library->release();
        if (mAutoHdrPipeline == nullptr) {
            SPDLOG_ERROR("Failed to create auto-HDR pipeline");
            return gameImage;
        }
    }

    auto& cvars = mConsoleVariable;
    struct {
        float peak, shoulderStart, shoulderPow, pad;
    } params = { std::min(cvars->GetFloat(CVAR_HDR_AUTO_PEAK, 2.0f), mEdrHeadroom),
                 cvars->GetFloat(CVAR_HDR_AUTO_SHOULDER_START, 0.0f),
                 cvars->GetFloat(CVAR_HDR_AUTO_SHOULDER_POW, 2.5f), 0.0f };

    // Encode into the window's command buffer: it runs after the game image's own command buffer and before the
    // GUI draws, so pause the window's render pass, run the pass, and resume it keeping what was already drawn.
    screen.mCommandEncoder->endEncoding();

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    pass->colorAttachments()->object(0)->setTexture(mAutoHdrTexture);
    pass->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionDontCare);
    pass->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* encoder = screen.mCommandBuffer->renderCommandEncoder(pass);
    encoder->setLabel(NS::String::string("Auto-HDR Encoder", NS::UTF8StringEncoding));
    encoder->setRenderPipelineState(mAutoHdrPipeline);
    encoder->setFragmentTexture(source, 0);
    encoder->setFragmentBytes(&params, sizeof(params), 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    encoder->endEncoding();

    MTL::RenderPassColorAttachmentDescriptor* color = screen.mRenderPassDescriptor->colorAttachments()->object(0);
    MTL::LoadAction color_load = color->loadAction();
    color->setLoadAction(MTL::LoadActionLoad);
    MTL::RenderPassDepthAttachmentDescriptor* depth = screen.mRenderPassDescriptor->depthAttachment();
    MTL::LoadAction depth_load = depth->loadAction();
    if (screen.mHasDepthBuffer) {
        depth->setLoadAction(MTL::LoadActionLoad);
    }
    screen.mCommandEncoder = screen.mCommandBuffer->renderCommandEncoder(screen.mRenderPassDescriptor);
    screen.mCommandEncoder->setLabel(NS::String::string("FrameBuffer 0 Command Encoder After Auto-HDR",
                                                        NS::UTF8StringEncoding));
    screen.mCommandEncoder->setDepthClipMode(MTL::DepthClipModeClamp);
    color->setLoadAction(color_load);
    if (screen.mHasDepthBuffer) {
        depth->setLoadAction(depth_load);
    }
    screen.mHasBoundVertexShader = false;
    screen.mHasBoundFragShader = false;
    screen.mLastShaderProgram = nullptr;
    for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
        screen.mLastBoundTextures[i] = nullptr;
        screen.mLastBoundSamplers[i] = nullptr;
    }
    screen.mLastDepthTest = -1;
    screen.mLastDepthMask = -1;
    screen.mLastZmodeDecal = -1;
    screen.mLastStrictDecal = -1;
    if (screen.mViewport->width > 0 && screen.mScissorRect->width > 0) {
        screen.mCommandEncoder->setViewport(*screen.mViewport);
        screen.mCommandEncoder->setScissorRect(*screen.mScissorRect);
    }

    return mAutoHdrTexture;
}
} // namespace Fast

bool Metal_IsSupported() {
#ifdef __IOS__
    // iOS always supports Metal and MTLCopyAllDevices is not available
    return true;
#else
    NS::Array* devices = MTLCopyAllDevices();
    NS::UInteger count = devices->count();

    devices->release();

    return count > 0;
#endif
}

#endif
