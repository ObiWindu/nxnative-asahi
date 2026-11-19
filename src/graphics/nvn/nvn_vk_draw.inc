// ---------------------------------------------------------------------------
// General NVN draw path on Vulkan.
//
// The recording layer has already resolved the game's NVN state and the
// offline build has translated its shaders to SPIR-V (nvn_spv.c). This block
// links the two: it builds a Vulkan pipeline and descriptor set from the
// shader reflection, uploads the guest's vertex/uniform/storage data, binds
// the guest textures and samplers, and replays the draw.
//
// It deliberately refuses what it cannot do correctly (runtime descriptor
// arrays, bindless pools, mesh/task stages and unknown reflection kinds) with a
// red line instead of drawing a wrong picture. Fixed arrays of directly bound
// textures and samplers use the NVN slot + element mapping established by the
// translator and the Metal replay. Geometry, tessellation, compute, buffer
// textures and bindless handles in the game's uniform buffers are replayed.
// The same file replays the operations that use no game program: the clears a
// load action cannot express and nvnCommandBufferDrawTexture, both drawn as one
// triangle with the built-in shaders in nvn_spv_builtin.h.
// ---------------------------------------------------------------------------
#include "nvn_spv.h"
#include "dmsh_chain.h"
#include "dmsh_abi.h"

static int copy_format(const tex_t *t, VkFormat *vf, size_t *bpp);

int prog_key(const prog_t *p, unsigned st, uint8_t key[8]);

static int shader_cache_dir(char *out, size_t n) {
    const char *e = nx_env("NX_NVN_SHADER_CACHE");
    if (e && *e) { snprintf(out, n, "%s", e); return 0; }
    snprintf(out, n, "%s/nxnative/shaders/b-%s", nx_cache_dir(), DMSH_CHAIN);
    return 0;
}

// Apple GPUs (and so Honeykrisp) expose no 24-bit depth format: D24 and
// D24S8 become the 32-bit float depth formats of the same aspect. As a render
// target that is a precision upgrade with the same comparisons; depth bytes
// are never handed back to the guest (depth/stencil copies are refused), so
// the storage layout difference does not leak. The Metal host substitutes the
// same way (nvn_fmt_mtl.c).
static VkFormat nvn_depth_format(int with_stencil) {
    VkFormat want = with_stencil ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_X8_D24_UNORM_PACK32;
    VkFormat fallback = with_stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
    static VkFormat cached[2];
    if (cached[with_stencil]) return cached[with_stencil];
    VkFormatProperties p = {0};
    vkGetPhysicalDeviceFormatProperties(g_vk.physical, want, &p);
    uint32_t need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    cached[with_stencil] = (p.optimalTilingFeatures & need) == need ? want : fallback;
    return cached[with_stencil];
}

static VkFormat vkfmt(int f) {
    switch (f) {
    case NVN_FORMAT_R8: return VK_FORMAT_R8_UNORM;
    case NVN_FORMAT_R8SN: return VK_FORMAT_R8_SNORM;
    case NVN_FORMAT_R8UI: return VK_FORMAT_R8_UINT;
    case NVN_FORMAT_R8I: return VK_FORMAT_R8_SINT;
    case NVN_FORMAT_R16F: return VK_FORMAT_R16_SFLOAT;
    case NVN_FORMAT_R16: return VK_FORMAT_R16_UNORM;
    case NVN_FORMAT_R16SN: return VK_FORMAT_R16_SNORM;
    case NVN_FORMAT_R16UI: return VK_FORMAT_R16_UINT;
    case NVN_FORMAT_R16I: return VK_FORMAT_R16_SINT;
    case NVN_FORMAT_R32F: return VK_FORMAT_R32_SFLOAT;
    case NVN_FORMAT_R32UI: return VK_FORMAT_R32_UINT;
    case NVN_FORMAT_R32I: return VK_FORMAT_R32_SINT;
    case NVN_FORMAT_RG8: return VK_FORMAT_R8G8_UNORM;
    case NVN_FORMAT_RG8SN: return VK_FORMAT_R8G8_SNORM;
    case NVN_FORMAT_RG8UI: return VK_FORMAT_R8G8_UINT;
    case NVN_FORMAT_RG8I: return VK_FORMAT_R8G8_SINT;
    case NVN_FORMAT_RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case NVN_FORMAT_RG16: return VK_FORMAT_R16G16_UNORM;
    case NVN_FORMAT_RG16SN: return VK_FORMAT_R16G16_SNORM;
    case NVN_FORMAT_RG16UI: return VK_FORMAT_R16G16_UINT;
    case NVN_FORMAT_RG16I: return VK_FORMAT_R16G16_SINT;
    case NVN_FORMAT_RG32F: return VK_FORMAT_R32G32_SFLOAT;
    case NVN_FORMAT_RG32UI: return VK_FORMAT_R32G32_UINT;
    case NVN_FORMAT_RG32I: return VK_FORMAT_R32G32_SINT;
    case NVN_FORMAT_RGB8: return VK_FORMAT_R8G8B8_UNORM;
    case NVN_FORMAT_RGB8SN: return VK_FORMAT_R8G8B8_SNORM;
    case NVN_FORMAT_RGB8UI: return VK_FORMAT_R8G8B8_UINT;
    case NVN_FORMAT_RGB8I: return VK_FORMAT_R8G8B8_SINT;
    case NVN_FORMAT_RGB16F: return VK_FORMAT_R16G16B16_SFLOAT;
    case NVN_FORMAT_RGB16: return VK_FORMAT_R16G16B16_UNORM;
    case NVN_FORMAT_RGB16SN: return VK_FORMAT_R16G16B16_SNORM;
    case NVN_FORMAT_RGB16UI: return VK_FORMAT_R16G16B16_UINT;
    case NVN_FORMAT_RGB16I: return VK_FORMAT_R16G16B16_SINT;
    case NVN_FORMAT_RGB32F: return VK_FORMAT_R32G32B32_SFLOAT;
    case NVN_FORMAT_RGB32UI: return VK_FORMAT_R32G32B32_UINT;
    case NVN_FORMAT_RGB32I: return VK_FORMAT_R32G32B32_SINT;
    case NVN_FORMAT_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case NVN_FORMAT_RGBA8SN: return VK_FORMAT_R8G8B8A8_SNORM;
    case NVN_FORMAT_RGBA8UI: return VK_FORMAT_R8G8B8A8_UINT;
    case NVN_FORMAT_RGBA8I: return VK_FORMAT_R8G8B8A8_SINT;
    case NVN_FORMAT_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case NVN_FORMAT_RGBA16: return VK_FORMAT_R16G16B16A16_UNORM;
    case NVN_FORMAT_RGBA16SN: return VK_FORMAT_R16G16B16A16_SNORM;
    case NVN_FORMAT_RGBA16UI: return VK_FORMAT_R16G16B16A16_UINT;
    case NVN_FORMAT_RGBA16I: return VK_FORMAT_R16G16B16A16_SINT;
    case NVN_FORMAT_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case NVN_FORMAT_RGBA32UI: return VK_FORMAT_R32G32B32A32_UINT;
    case NVN_FORMAT_RGBA32I: return VK_FORMAT_R32G32B32A32_SINT;
    case NVN_FORMAT_RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case NVN_FORMAT_RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case NVN_FORMAT_RGB10A2SN: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    // Vertex-fetch conversions: NVN's *_UI2F / *_I2F formats hand the shader a
    // float built from an unnormalized (scaled) integer, Vulkan's USCALED /
    // SSCALED formats, and RGB10A2I is the packed signed integer of its bits.
    case NVN_FORMAT_R8_UI2F: return VK_FORMAT_R8_USCALED;
    case NVN_FORMAT_R8_I2F: return VK_FORMAT_R8_SSCALED;
    case NVN_FORMAT_R16_UI2F: return VK_FORMAT_R16_USCALED;
    case NVN_FORMAT_R16_I2F: return VK_FORMAT_R16_SSCALED;
    case NVN_FORMAT_R32_UI2F: return VK_FORMAT_R32_UINT;
    case NVN_FORMAT_R32_I2F: return VK_FORMAT_R32_SINT;
    case NVN_FORMAT_RG8_UI2F: return VK_FORMAT_R8G8_USCALED;
    case NVN_FORMAT_RG8_I2F: return VK_FORMAT_R8G8_SSCALED;
    case NVN_FORMAT_RG16_UI2F: return VK_FORMAT_R16G16_USCALED;
    case NVN_FORMAT_RG16_I2F: return VK_FORMAT_R16G16_SSCALED;
    case NVN_FORMAT_RG32_UI2F: return VK_FORMAT_R32G32_UINT;
    case NVN_FORMAT_RG32_I2F: return VK_FORMAT_R32G32_SINT;
    case NVN_FORMAT_RGB8_UI2F: return VK_FORMAT_R8G8B8_USCALED;
    case NVN_FORMAT_RGB8_I2F: return VK_FORMAT_R8G8B8_SSCALED;
    case NVN_FORMAT_RGB16_UI2F: return VK_FORMAT_R16G16B16_USCALED;
    case NVN_FORMAT_RGB16_I2F: return VK_FORMAT_R16G16B16_SSCALED;
    case NVN_FORMAT_RGB32_UI2F: return VK_FORMAT_R32G32B32_UINT;
    case NVN_FORMAT_RGB32_I2F: return VK_FORMAT_R32G32B32_SINT;
    case NVN_FORMAT_RGBA8_UI2F: return VK_FORMAT_R8G8B8A8_USCALED;
    case NVN_FORMAT_RGBA8_I2F: return VK_FORMAT_R8G8B8A8_SSCALED;
    case NVN_FORMAT_RGBA16_UI2F: return VK_FORMAT_R16G16B16A16_USCALED;
    case NVN_FORMAT_RGBA16_I2F: return VK_FORMAT_R16G16B16A16_SSCALED;
    case NVN_FORMAT_RGBA32_UI2F: return VK_FORMAT_R32G32B32A32_UINT;
    case NVN_FORMAT_RGBA32_I2F: return VK_FORMAT_R32G32B32A32_SINT;
    case NVN_FORMAT_RGB10A2I: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case NVN_FORMAT_RGB10A2_UI2F: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case NVN_FORMAT_RGB10A2_I2F: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case NVN_FORMAT_R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    // The packed 16-bit family, by its NVN name order: RGB* puts red in the high
    // bits (as the Metal table's r/b swap says), BGR* puts blue there. The
    // alpha-less members read alpha as 1 (nvn_fmt_no_alpha).
    case NVN_FORMAT_RGB5: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case NVN_FORMAT_RGB565: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case NVN_FORMAT_RGB5A1: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case NVN_FORMAT_RGBA4: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case NVN_FORMAT_BGR565: return VK_FORMAT_B5G6R5_UNORM_PACK16;
    case NVN_FORMAT_BGR5: return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
    case NVN_FORMAT_BGR5A1: return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
    case NVN_FORMAT_A1BGR5: return VK_FORMAT_A1B5G5R5_UNORM_PACK16;
    case NVN_FORMAT_BGRX8: return VK_FORMAT_B8G8R8A8_UNORM;
    case NVN_FORMAT_BGRX8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case NVN_FORMAT_DEPTH16: return VK_FORMAT_D16_UNORM;
    case NVN_FORMAT_DEPTH24: return nvn_depth_format(0);
    case NVN_FORMAT_DEPTH32F: return VK_FORMAT_D32_SFLOAT;
    case NVN_FORMAT_DEPTH24_STENCIL8: return nvn_depth_format(1);
    case NVN_FORMAT_DEPTH32F_STENCIL8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case NVN_FORMAT_STENCIL8: return VK_FORMAT_S8_UINT;
    case NVN_FORMAT_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case NVN_FORMAT_BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case NVN_FORMAT_RGBA_DXT1: case NVN_FORMAT_RGB_DXT1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_DXT1_SRGB: case NVN_FORMAT_RGB_DXT1_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_DXT3: return VK_FORMAT_BC2_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_DXT3_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_DXT5: return VK_FORMAT_BC3_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_DXT5_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
    case NVN_FORMAT_RGTC1_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
    case NVN_FORMAT_RGTC1_SNORM: return VK_FORMAT_BC4_SNORM_BLOCK;
    case NVN_FORMAT_RGTC2_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    case NVN_FORMAT_RGTC2_SNORM: return VK_FORMAT_BC5_SNORM_BLOCK;
    case NVN_FORMAT_BPTC_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
    case NVN_FORMAT_BPTC_UNORM_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
    case NVN_FORMAT_BPTC_SFLOAT: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case NVN_FORMAT_BPTC_UFLOAT: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_4x4: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_5x4: return VK_FORMAT_ASTC_5x4_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_5x5: return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_6x5: return VK_FORMAT_ASTC_6x5_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_6x6: return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x5: return VK_FORMAT_ASTC_8x5_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x6: return VK_FORMAT_ASTC_8x6_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x8: return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x5: return VK_FORMAT_ASTC_10x5_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x6: return VK_FORMAT_ASTC_10x6_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x8: return VK_FORMAT_ASTC_10x8_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x10: return VK_FORMAT_ASTC_10x10_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_12x10: return VK_FORMAT_ASTC_12x10_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_12x12: return VK_FORMAT_ASTC_12x12_UNORM_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_4x4_SRGB: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_5x4_SRGB: return VK_FORMAT_ASTC_5x4_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_5x5_SRGB: return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_6x5_SRGB: return VK_FORMAT_ASTC_6x5_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_6x6_SRGB: return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x5_SRGB: return VK_FORMAT_ASTC_8x5_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x6_SRGB: return VK_FORMAT_ASTC_8x6_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_8x8_SRGB: return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x5_SRGB: return VK_FORMAT_ASTC_10x5_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x6_SRGB: return VK_FORMAT_ASTC_10x6_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x8_SRGB: return VK_FORMAT_ASTC_10x8_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_10x10_SRGB: return VK_FORMAT_ASTC_10x10_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_12x10_SRGB: return VK_FORMAT_ASTC_12x10_SRGB_BLOCK;
    case NVN_FORMAT_RGBA_ASTC_12x12_SRGB: return VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    // RGBX: Vulkan has no X component; the RGBA format of the same size holds
    // the same texels and a sampled view reads alpha as 1 (nvn_fmt_no_alpha).
    case NVN_FORMAT_RGBX8: return VK_FORMAT_R8G8B8A8_UNORM;
    case NVN_FORMAT_RGBX8SN: return VK_FORMAT_R8G8B8A8_SNORM;
    case NVN_FORMAT_RGBX8UI: return VK_FORMAT_R8G8B8A8_UINT;
    case NVN_FORMAT_RGBX8I: return VK_FORMAT_R8G8B8A8_SINT;
    case NVN_FORMAT_RGBX8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case NVN_FORMAT_RGBX16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case NVN_FORMAT_RGBX16: return VK_FORMAT_R16G16B16A16_UNORM;
    case NVN_FORMAT_RGBX16SN: return VK_FORMAT_R16G16B16A16_SNORM;
    case NVN_FORMAT_RGBX32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case NVN_FORMAT_RGB9E5F: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkFilter vkfilter_mag(int f) { return f == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }
static VkFilter vkfilter_min(int f) { return (f == NVN_MIN_FILTER_LINEAR || f == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || f == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }
static VkSamplerMipmapMode vkmip(int f) { return (f == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || f == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST; }
static VkSamplerAddressMode vkwrap(int w) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case NVN_WRAP_MODE_CLAMP_TO_EDGE: case NVN_WRAP_MODE_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
static VkCompareOp vkcmp(int f) {
    switch (f) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}
static VkStencilOp vkstop(int o) {
    switch (o) {
    case NVN_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
    case NVN_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
    case NVN_STENCIL_OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case NVN_STENCIL_OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case NVN_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
    case NVN_STENCIL_OP_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case NVN_STENCIL_OP_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}
static VkBlendFactor vkbfac(int f) {
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO;
    case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkLogicOp vklogic(int o) {
    switch (o) {
    case NVN_LOGIC_OP_CLEAR: return VK_LOGIC_OP_CLEAR;
    case NVN_LOGIC_OP_AND: return VK_LOGIC_OP_AND;
    case NVN_LOGIC_OP_AND_REVERSE: return VK_LOGIC_OP_AND_REVERSE;
    case NVN_LOGIC_OP_AND_INVERTED: return VK_LOGIC_OP_AND_INVERTED;
    case NVN_LOGIC_OP_NOOP: return VK_LOGIC_OP_NO_OP;
    case NVN_LOGIC_OP_XOR: return VK_LOGIC_OP_XOR;
    case NVN_LOGIC_OP_OR: return VK_LOGIC_OP_OR;
    case NVN_LOGIC_OP_NOR: return VK_LOGIC_OP_NOR;
    case NVN_LOGIC_OP_EQUIV: return VK_LOGIC_OP_EQUIVALENT;
    case NVN_LOGIC_OP_INVERT: return VK_LOGIC_OP_INVERT;
    case NVN_LOGIC_OP_OR_REVERSE: return VK_LOGIC_OP_OR_REVERSE;
    case NVN_LOGIC_OP_COPY_INVERTED: return VK_LOGIC_OP_COPY_INVERTED;
    case NVN_LOGIC_OP_OR_INVERTED: return VK_LOGIC_OP_OR_INVERTED;
    case NVN_LOGIC_OP_NAND: return VK_LOGIC_OP_NAND;
    case NVN_LOGIC_OP_SET: return VK_LOGIC_OP_SET;
    default: return VK_LOGIC_OP_COPY;
    }
}
static VkBlendOp vkbeq(int e) {
    switch (e) {
    case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT;
    case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN;
    case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
// Fixed-function fallback for the Porter-Duff advanced modes. The native
// VK_EXT_blend_operation_advanced path below handles the complete equations
// when the device capabilities and NVN state match. On other devices these
// factors preserve the modes expressible with ordinary Vulkan blending.
static blend_st adv_blend(blend_st b) {
    if (!b.adv) return b;
    enum { Z = NVN_BLEND_FUNC_ZERO, O = NVN_BLEND_FUNC_ONE, SA = NVN_BLEND_FUNC_SRC_ALPHA,
           ISA = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, DA = NVN_BLEND_FUNC_DST_ALPHA,
           IDA = NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA };
    // {source, destination} for colour, then for alpha
    static const uint8_t pd[][4] = {
        [NVN_BLEND_ADVANCED_MODE_ZERO] = {Z, Z, Z, Z},
        [NVN_BLEND_ADVANCED_MODE_SRC] = {O, Z, O, Z},
        [NVN_BLEND_ADVANCED_MODE_DST] = {Z, O, Z, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_OVER] = {O, ISA, O, ISA},
        [NVN_BLEND_ADVANCED_MODE_DST_OVER] = {IDA, O, IDA, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_IN] = {DA, Z, DA, Z},
        [NVN_BLEND_ADVANCED_MODE_DST_IN] = {Z, SA, Z, SA},
        [NVN_BLEND_ADVANCED_MODE_SRC_OUT] = {IDA, Z, IDA, Z},
        [NVN_BLEND_ADVANCED_MODE_DST_OUT] = {Z, ISA, Z, ISA},
        [NVN_BLEND_ADVANCED_MODE_SRC_ATOP] = {DA, ISA, Z, O},
        [NVN_BLEND_ADVANCED_MODE_DST_ATOP] = {IDA, SA, O, Z},
        [NVN_BLEND_ADVANCED_MODE_XOR] = {IDA, ISA, IDA, ISA},
        [NVN_BLEND_ADVANCED_MODE_PLUS] = {O, O, O, O},
        [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED] = {O, O, O, O},
        [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA] = {O, O, O, O},
    };
    const uint8_t *f = b.adv < sizeof pd / sizeof *pd && pd[b.adv][0] ? pd[b.adv] : NULL;
    if (!f || b.adv_ovl || (b.adv_npm && f[0] != O && f[0] != Z)) {
        static uint64_t seen;
        if (!(seen >> (b.adv & 63) & 1)) {
            seen |= 1ull << (b.adv & 63);
            LOG("[RED] advanced blend mode %#x (overlap %d, %spremultiplied source) has no fixed-function equivalent: the state's blend functions are used",
                (unsigned)b.adv, (int)b.adv_ovl, b.adv_npm ? "not " : "");
        }
        return b;
    }
    b.src_rgb = b.adv_npm && f[0] == O ? SA : f[0];
    b.dst_rgb = f[1]; b.src_a = f[2]; b.dst_a = f[3];
    b.eq_rgb = b.eq_a = NVN_BLEND_EQUATION_ADD;
    return b;
}
// Vulkan advanced blend equations map directly to these NVN modes. Only use
// them when the device reports all operations and supports the requested
// overlap and premultiplication state.
static int nvn_advanced_blend_op(unsigned mode, VkBlendOp *out) {
    switch (mode) {
    case NVN_BLEND_ADVANCED_MODE_ZERO: *out = VK_BLEND_OP_ZERO_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_SRC: *out = VK_BLEND_OP_SRC_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_DST: *out = VK_BLEND_OP_DST_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_SRC_OVER: *out = VK_BLEND_OP_SRC_OVER_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_DST_OVER: *out = VK_BLEND_OP_DST_OVER_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_SRC_IN: *out = VK_BLEND_OP_SRC_IN_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_DST_IN: *out = VK_BLEND_OP_DST_IN_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_SRC_OUT: *out = VK_BLEND_OP_SRC_OUT_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_DST_OUT: *out = VK_BLEND_OP_DST_OUT_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_SRC_ATOP: *out = VK_BLEND_OP_SRC_ATOP_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_DST_ATOP: *out = VK_BLEND_OP_DST_ATOP_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_XOR: *out = VK_BLEND_OP_XOR_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_PLUS: *out = VK_BLEND_OP_PLUS_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED: *out = VK_BLEND_OP_PLUS_CLAMPED_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA: *out = VK_BLEND_OP_PLUS_CLAMPED_ALPHA_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_PLUS_DARKER: *out = VK_BLEND_OP_PLUS_DARKER_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_MULTIPLY: *out = VK_BLEND_OP_MULTIPLY_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_SCREEN: *out = VK_BLEND_OP_SCREEN_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_OVERLAY: *out = VK_BLEND_OP_OVERLAY_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_DARKEN: *out = VK_BLEND_OP_DARKEN_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_LIGHTEN: *out = VK_BLEND_OP_LIGHTEN_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_COLORDODGE: *out = VK_BLEND_OP_COLORDODGE_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_COLORBURN: *out = VK_BLEND_OP_COLORBURN_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_HARDLIGHT: *out = VK_BLEND_OP_HARDLIGHT_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_SOFTLIGHT: *out = VK_BLEND_OP_SOFTLIGHT_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_DIFFERENCE: *out = VK_BLEND_OP_DIFFERENCE_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_MINUS: *out = VK_BLEND_OP_MINUS_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED: *out = VK_BLEND_OP_MINUS_CLAMPED_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_EXCLUSION: *out = VK_BLEND_OP_EXCLUSION_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_CONTRAST: *out = VK_BLEND_OP_CONTRAST_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_INVERT: *out = VK_BLEND_OP_INVERT_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_INVERT_RGB: *out = VK_BLEND_OP_INVERT_RGB_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_INVERT_OVG: *out = VK_BLEND_OP_INVERT_OVG_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_LINEARDODGE: *out = VK_BLEND_OP_LINEARDODGE_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_LINEARBURN: *out = VK_BLEND_OP_LINEARBURN_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_VIVIDLIGHT: *out = VK_BLEND_OP_VIVIDLIGHT_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_LINEARLIGHT: *out = VK_BLEND_OP_LINEARLIGHT_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_PINLIGHT: *out = VK_BLEND_OP_PINLIGHT_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_HARDMIX: *out = VK_BLEND_OP_HARDMIX_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_RED: *out = VK_BLEND_OP_RED_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_GREEN: *out = VK_BLEND_OP_GREEN_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_BLUE: *out = VK_BLEND_OP_BLUE_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_HSL_HUE: *out = VK_BLEND_OP_HSL_HUE_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_HSL_SATURATION: *out = VK_BLEND_OP_HSL_SATURATION_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_HSL_COLOR: *out = VK_BLEND_OP_HSL_COLOR_EXT; return 1;
    case NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY: *out = VK_BLEND_OP_HSL_LUMINOSITY_EXT; return 1;
    default: return 0;
    }
}

static int nvn_advanced_blend_supported(blend_st b, int ncolor, VkBlendOp *op) {
    const VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT *p = &g_vk.caps.advanced_blend_properties;
    if (!b.adv || !nx_vk_supports_extension(&g_vk, VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME) ||
        !p->advancedBlendAllOperations || ncolor > (int)p->advancedBlendMaxColorAttachments ||
        (b.adv_npm && !p->advancedBlendNonPremultipliedSrcColor) ||
        b.adv_dst_free || b.adv_ovl > NVN_BLEND_ADVANCED_OVERLAP_CONJOINT ||
        (b.adv_ovl && !p->advancedBlendCorrelatedOverlap)) return 0;
    return nvn_advanced_blend_op((unsigned)b.adv, op);
}

static VkPrimitiveTopology vktopo(int p) {
    switch (p) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_LOOP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;   // loop has no Vulkan equivalent; first vertex is re-appended by the caller when needed
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
static int nvkd_expand_segment(int prim, const uint32_t *src, size_t count,
                               uint32_t *dst, size_t capacity, size_t *written) {
    size_t need = 0;
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_LINE_LOOP: need = count >= 2 ? count * 2 : 0; break;
    case NVN_DRAW_PRIMITIVE_QUADS: need = (count / 4) * 6; break;
    case NVN_DRAW_PRIMITIVE_QUAD_STRIP: need = count >= 4 ? ((count - 2) / 2) * 6 : 0; break;
    case NVN_DRAW_PRIMITIVE_POLYGON: need = count >= 3 ? (count - 2) * 3 : 0; break;
    default: return -1;
    }
    if (need > capacity - *written) return -1;
    size_t n = *written;
    if (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP) {
        for (size_t i = 0; i + 1 < count; i++) { dst[n++] = src[i]; dst[n++] = src[i + 1]; }
        dst[n++] = src[count - 1]; dst[n++] = src[0];
    } else if (prim == NVN_DRAW_PRIMITIVE_QUADS) {
        for (size_t i = 0; i + 3 < count; i += 4) {
            dst[n++] = src[i]; dst[n++] = src[i + 1]; dst[n++] = src[i + 2];
            dst[n++] = src[i + 2]; dst[n++] = src[i + 3]; dst[n++] = src[i];
        }
    } else if (prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP) {
        for (size_t i = 0; i + 3 < count; i += 2) {
            dst[n++] = src[i]; dst[n++] = src[i + 1]; dst[n++] = src[i + 3];
            dst[n++] = src[i]; dst[n++] = src[i + 3]; dst[n++] = src[i + 2];
        }
    } else {
        for (size_t i = 1; i + 1 < count; i++) {
            dst[n++] = src[0]; dst[n++] = src[i]; dst[n++] = src[i + 1];
        }
    }
    *written = n;
    return 0;
}
static int nvkd_expand_primitive(int prim, const uint32_t *src, size_t count,
                                 int restart, uint32_t restart_index,
                                 uint32_t **out, uint32_t *out_count) {
    if (count > SIZE_MAX / (3 * sizeof(uint32_t))) return -1;
    size_t capacity = count * 3;
    uint32_t *dst = capacity ? malloc(capacity * sizeof *dst) : NULL;
    if (capacity && !dst) return -1;
    size_t written = 0, start = 0;
    for (size_t i = 0; i <= count; i++) {
        if (i != count && (!restart || src[i] != restart_index)) continue;
        if (nvkd_expand_segment(prim, src + start, i - start, dst, capacity, &written) != 0) {
            free(dst); return -1;
        }
        start = i + 1;
    }
    if (written > UINT32_MAX) { free(dst); return -1; }
    *out = dst;
    *out_count = (uint32_t)written;
    return 0;
}
static int geo_topo_index(VkPrimitiveTopology t) {
    // dmsh_build's geometry variants: 0 points, 1 lines, 2 lines with
    // adjacency, 3 triangles, 4 triangles with adjacency.
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return 1;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return 2;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return 3;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: return 4;
    default: return -1;
    }
}

typedef struct {
    uint64_t serial; int state;         // 0 not built, 1 ready, -1 failed
    uint64_t cserial[6]; int combined;  // the program bound to each stage (a combination)
    nvn_shader sh[6]; int have[6]; uint32_t loaded;
    VkShaderModule mod[6];
    nvn_shader gsh[5]; int ghave[5];    // geometry variants by input topology (0 is sh[2] / mod[2])
    VkShaderModule gmod[5];
    VkDescriptorSetLayout setl;
    VkPipelineLayout pl;
    VkPipeline pipe; uint64_t pipe_key; VkRenderPass pipe_rp;
    VkPipeline cpipe; uint64_t cpipe_key;
    int has_alpha, alpha_ref_id, alpha_mask_id;
} prog_vk;
static prog_vk *g_pvks; static int g_npvk, g_cpvk;

static uint32_t nvn_vk_descriptor_limit(VkDescriptorType type) {
    const VkPhysicalDeviceLimits *l = &g_vk.caps.props.limits;
    uint32_t per_set = 0, per_stage = 0;
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
        per_set = l->maxDescriptorSetSamplers; per_stage = l->maxPerStageDescriptorSamplers; break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        per_set = l->maxDescriptorSetSamplers < l->maxDescriptorSetSampledImages ? l->maxDescriptorSetSamplers : l->maxDescriptorSetSampledImages;
        per_stage = l->maxPerStageDescriptorSamplers < l->maxPerStageDescriptorSampledImages ? l->maxPerStageDescriptorSamplers : l->maxPerStageDescriptorSampledImages; break;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        per_set = l->maxDescriptorSetSampledImages; per_stage = l->maxPerStageDescriptorSampledImages; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        per_set = l->maxDescriptorSetStorageImages; per_stage = l->maxPerStageDescriptorStorageImages; break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        per_set = l->maxDescriptorSetSampledImages; per_stage = l->maxPerStageDescriptorSampledImages; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        per_set = l->maxDescriptorSetStorageImages; per_stage = l->maxPerStageDescriptorStorageImages; break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        per_set = l->maxDescriptorSetUniformBuffers; per_stage = l->maxPerStageDescriptorUniformBuffers; break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        per_set = l->maxDescriptorSetUniformBuffersDynamic; per_stage = l->maxPerStageDescriptorUniformBuffers; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        per_set = l->maxDescriptorSetStorageBuffers; per_stage = l->maxPerStageDescriptorStorageBuffers; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        per_set = l->maxDescriptorSetStorageBuffersDynamic; per_stage = l->maxPerStageDescriptorStorageBuffers; break;
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        per_set = l->maxDescriptorSetInputAttachments; per_stage = l->maxPerStageDescriptorInputAttachments; break;
    default: return 0;
    }
    return per_set < per_stage ? per_set : per_stage;
}

static prog_vk *pvk_find(const uint64_t cserial[6]) {
    for (int i = 0; i < g_npvk; i++)
        if (g_pvks[i].combined && !memcmp(g_pvks[i].cserial, cserial, 6 * sizeof *cserial)) return &g_pvks[i];
    return NULL;
}
static int stage_index(const char *s) {
    // dmsh_build.py writes the long names; the tests use the short ones.
    if (!strcmp(s, "vs") || !strcmp(s, "vertex")) return 0;
    if (!strcmp(s, "ps") || !strcmp(s, "pixel") || !strcmp(s, "fragment")) return 1;
    if (!strcmp(s, "gs") || !strcmp(s, "geometry")) return 2;
    if (!strcmp(s, "hs") || !strcmp(s, "hull") || !strcmp(s, "tess_control")) return 3;
    if (!strcmp(s, "ds") || !strcmp(s, "domain") || !strcmp(s, "tess_evaluation")) return 4;
    if (!strcmp(s, "cs") || !strcmp(s, "compute")) return 5;
    return -1;
}
// Load the stages a bound program object holds. A program carries its own
// stage(s) — one per shader, as the reflection says — and a game may bind the
// same object for several stage slots (Unity binds one program per stage).
static void pvk_load_program(prog_vk *pv, const prog_t *p, const char *dir) {
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        uint8_t key[8];
        if (!prog_key(p, i, key)) continue;
        char hex[17]; nvn_shader_key_hex(key, hex);
        nvn_shader sh;
        if (nvn_shader_load(dir, hex, &sh) != 0) {
            LOG("[RED] [nvn/vk] shader %s (stage %u): %s (build it with dm/gfx/tools/dmsh_build.py)", hex, i, sh.load_error);
            continue;
        }
        if (!strcmp(sh.stage, "mesh") || !strcmp(sh.stage, "task")) {
            LOG("[RED] [nvn/vk] shader %s is an NVN2 %s stage: mesh/task replay is not implemented", hex, sh.stage);
            nvn_shader_free(&sh);
            pv->state = -1;
            return;
        }
        int st = stage_index(sh.stage);
        if (st < 0) st = (int)i;
        if (st >= 6) { nvn_shader_free(&sh); continue; }
        pv->sh[st] = sh; pv->have[st] = 1; pv->loaded |= 1u << st;
    }
}
// The combination the bound stage slots name: each graphics (and compute)
// stage's shader comes from the program that last claimed it, as NVN's
// per-stage program binding does (the Metal host's bind_stages).
// A graphics draw combines the draw stages (0-4); a dispatch is the compute
// stage (5) alone. NVN binds the compute program independently and it stays
// bound across draws, so a combination key that included stage 5 mixed a
// leftover compute program into graphics pipelines: its ubo_ptr bindings then
// had no place in the draw's descriptor set (Super Mario Odyssey's effect
// passes after the first compute dispatch). mask selects the stages.
static prog_vk *pvk_get_stages(const prog_t *const stg[6], unsigned mask) {
    uint64_t cserial[6] = {0};
    int any = 0;
    for (int s = 0; s < 6; s++) if ((mask >> s & 1) && stg[s] && stg[s]->serial) { cserial[s] = stg[s]->serial; any = 1; }
    if (!any) return NULL;
    prog_vk *pv = pvk_find(cserial);
    if (pv) return pv->state == 1 ? pv : NULL;
    if (g_npvk == g_cpvk) { g_cpvk = g_cpvk ? g_cpvk * 2 : 32; g_pvks = realloc(g_pvks, (size_t)g_cpvk * sizeof *g_pvks); }
    pv = &g_pvks[g_npvk++]; memset(pv, 0, sizeof *pv);
    memcpy(pv->cserial, cserial, sizeof cserial); pv->combined = 1; pv->serial = cserial[0] ? cserial[0] : cserial[1];
    char dir[1024];
    shader_cache_dir(dir, sizeof dir);
    for (int s = 0; s < 6; s++) {
        const prog_t *p = (mask >> s & 1) ? stg[s] : NULL;
        if (!p || !p->serial) continue;
        // One object bound for several stages (mask 3) appears in several
        // slots as separate copies with the same serial: load its shaders once.
        int seen = 0;
        for (int t = 0; t < s; t++) if ((mask >> t & 1) && stg[t] && stg[t]->serial == p->serial) seen = 1;
        if (seen) continue;
        pvk_load_program(pv, p, dir);
        if (pv->state < 0) return NULL;
    }
    // A geometry shader is built once per input topology (dmsh_build's
    // geometry_finish); the draw picks the variant matching its primitive.
    // Topology 0 is already loaded as the base stage above.
    if (pv->have[2]) {
        for (int t = 1; t < 5; t++) {
            nvn_shader g;
            if (nvn_shader_load_geo(dir, pv->sh[2].key, t, &g) == 0) { pv->gsh[t] = g; pv->ghave[t] = 1; }
        }
    }
    // NVN's default depth mode is near=-w (GL): transform the final
    // vertex-processing stage's gl_Position. The translator emits near=0
    // SPIR-V; this is the same z' = z/2 + w/2 the Metal main0 entry uses.
    if (!g_st.depth_zero) {
        int ds = pv->have[2] ? 2 : pv->have[4] ? 4 : 0;
        if (pv->have[ds]) {
            int dr = nvn_spv_depth_gl(&pv->sh[ds]);
            if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] depth-convention rewrite stage %d: %s", ds, dr == 0 ? "applied" : "no position output");
        }
        if (ds == 2) for (int t = 1; t < 5; t++) if (pv->ghave[t]) {
            int dr = nvn_spv_depth_gl(&pv->gsh[t]);
            if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] depth-convention rewrite stage %d/%d: %s", t, ds, dr == 0 ? "applied" : "no position output");
        }
    }
    if (pv->have[1] && nvn_spv_alpha_test(&pv->sh[1]) == 0) {
        pv->has_alpha = 1;
        pv->alpha_ref_id = pv->sh[1].alpha_ref_id;
        pv->alpha_mask_id = pv->sh[1].alpha_mask_id;
    }
    if (nx_env("NX_VK_DUMP_SPV")) for (int st = 0; st < 6; st++) if (pv->have[st]) {
        char path[1200]; snprintf(path, sizeof path, "%s/%s_%d.spv", nx_env("NX_VK_DUMP_SPV"), pv->sh[st].key, st);
        FILE *df = fopen(path, "wb");
        if (df) { fwrite(pv->sh[st].spv, 4, pv->sh[st].spv_words, df); fclose(df); }
        else LOG("[RED] [nvn/vk] cannot write %s: %s", path, strerror(errno));
    }
    for (int st = 0; st < 6; st++) if (pv->have[st]) {
        nvn_spv_rebind(&pv->sh[st], (uint32_t)st * 1000u);
        pv->mod[st] = nx_vk_shader_module(&g_vk, pv->sh[st].spv, pv->sh[st].spv_words * 4);
        if (!pv->mod[st]) { LOG("[RED] [nvn/vk] shader %s did not make a Vulkan module", pv->sh[st].key); pv->state = -1; return NULL; }
    }
    if (pv->have[2]) {
        pv->gmod[0] = pv->mod[2];
        for (int t = 1; t < 5; t++) if (pv->ghave[t]) {
            nvn_spv_rebind(&pv->gsh[t], 2000u);   // the geometry stage's binding range
            pv->gmod[t] = nx_vk_shader_module(&g_vk, pv->gsh[t].spv, pv->gsh[t].spv_words * 4);
            if (!pv->gmod[t]) { LOG("[RED] [nvn/vk] geometry variant %d of %s did not make a Vulkan module", t, pv->sh[2].key); pv->state = -1; return NULL; }
        }
    }
    if (!pv->have[5] && !pv->have[0]) {
        char stages[256]; size_t used = 0;
        for (int s = 0; s < 5; s++) if (pv->have[s]) {
            int n = snprintf(stages + used, sizeof stages - used, "%s%d:%s", used ? "," : "", s, pv->sh[s].key);
            if (n > 0 && (size_t)n < sizeof stages - used) used += (size_t)n;
        }
        if (!used) snprintf(stages, sizeof stages, "none loaded");
        LOG("[RED] [nvn/vk] the bound programs have no vertex stage; the draws are dropped (loaded: %s, serials %llu %llu %llu %llu %llu / compute %llu)",
            stages, (unsigned long long)cserial[0], (unsigned long long)cserial[1], (unsigned long long)cserial[2],
            (unsigned long long)cserial[3], (unsigned long long)cserial[4], (unsigned long long)cserial[5]);
        pv->state = -1; return NULL;
    }
    // Runtime descriptor arrays need descriptor-indexing layout flags and
    // features. Fixed arrays use the guest NVN slot tables (256 textures, 128
    // samplers) and the queried Vulkan descriptor limits; they are not capped
    // by the temporary descriptor-write storage, which is sized per draw.
    for (int st = 0; st < 6; st++) if (pv->have[st]) {
        nvn_shader *sh = &pv->sh[st];
        for (int i = 0; i < sh->ndesc; i++) {
            const nvn_desc_bind *d = &sh->desc[i];
            if (!d->count) {
                LOG("[RED] [nvn/vk] shader %s binding %u is a runtime descriptor array; descriptor indexing is not replayed yet", sh->key, d->binding);
                pv->state = -1; return NULL;
            }
            if (d->count > 256) {
                LOG("[RED] [nvn/vk] shader %s binding %u has %u descriptors; this replay cannot address more than 256 elements", sh->key, d->binding, d->count);
                pv->state = -1; return NULL;
            }
            uint32_t device_limit = nvn_vk_descriptor_limit(d->type);
            if (!device_limit || d->count > device_limit) {
                LOG("[RED] [nvn/vk] shader %s binding %u requests %u descriptors of type %u; device limit is %u",
                    sh->key, d->binding, d->count, (unsigned)d->type, device_limit);
                pv->state = -1; return NULL;
            }
        }
    }
    // One descriptor set for all stages, each stage's bindings in its own range.
    VkDescriptorSetLayoutBinding b[64 * 6]; int nb = 0;
    for (int st = 0; st < 6; st++) if (pv->have[st]) {
        VkShaderStageFlags flags = st == 0 ? VK_SHADER_STAGE_VERTEX_BIT : st == 2 ? VK_SHADER_STAGE_GEOMETRY_BIT :
                                    st == 3 ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : st == 4 ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT :
                                    st == 5 ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        for (int i = 0; i < pv->sh[st].ndesc; i++) {
            if (pv->sh[st].desc[i].set != 0) {
                LOG("[RED] [nvn/vk] shader %s uses descriptor set %u; the replay binds set 0 only", pv->sh[st].key, pv->sh[st].desc[i].set);
                pv->state = -1; return NULL;
            }
            if (nb >= (int)(sizeof b / sizeof *b)) {
                LOG("[RED] [nvn/vk] the bound programs have more than %zu descriptor bindings", sizeof b / sizeof *b);
                pv->state = -1; return NULL;
            }
            VkDescriptorSetLayoutBinding *x = &b[nb++];
            memset(x, 0, sizeof *x);
            x->binding = pv->sh[st].desc[i].binding;
            x->descriptorType = pv->sh[st].desc[i].type;
            x->descriptorCount = pv->sh[st].desc[i].count;
            x->stageFlags = flags;
        }
    }
    VkDescriptorSetLayoutCreateInfo sl = {0};
    sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sl.bindingCount = (uint32_t)nb; sl.pBindings = b;
    if (vkCreateDescriptorSetLayout(g_vk.device, &sl, NULL, &pv->setl) != VK_SUCCESS) { pv->state = -1; return NULL; }
    VkShaderStageFlags pcs = pv->have[0] ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        (pv->have[2] ? VK_SHADER_STAGE_GEOMETRY_BIT : 0) | (pv->have[3] ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : 0) |
        (pv->have[4] ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : 0)) : VK_SHADER_STAGE_COMPUTE_BIT;
    VkPushConstantRange pcr = {pcs, 0, 256};
    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1; pl.pSetLayouts = &pv->setl;
    if (pv->sh[0].push_size || pv->sh[1].push_size || pv->sh[2].push_size || pv->sh[3].push_size || pv->sh[4].push_size || pv->sh[5].push_size) { pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr; }
    if (vkCreatePipelineLayout(g_vk.device, &pl, NULL, &pv->pl) != VK_SUCCESS) { pv->state = -1; return NULL; }
    pv->state = 1;
    static int logged;
    if (nx_env_bool("NX_VK_DEBUG", 0) || logged++ < 8) {
        if (pv->have[0]) LOG("[nvn/vk] program %llu: %d Vulkan shader bindings (%s + %s)", (unsigned long long)pv->serial, nb, pv->sh[0].key, pv->have[1] ? pv->sh[1].key : "no fragment");
        else LOG("[nvn/vk] program %llu: %d Vulkan compute bindings (%s)", (unsigned long long)pv->serial, nb, pv->sh[5].key);
    }
    return pv;
}

// A program object now carries only its own stages, so there is no single
// program to prebuild the bound combination from: the first draw of each
// combination loads its shaders. (The pipeline cache persists the compiled
// pipelines across runs.)
static void nvkd_prefetch(const prog_t *p) { vk_ready(); (void)p; }

static uint64_t state_hash(void) {
    uint64_t h = 1469598103934665603ull;
#define MIX(x) do { h ^= (uint64_t)(x); h *= 1099511628211ull; } while (0)
    MIX(g_st.poly.cull); MIX(g_st.poly.front); MIX(g_st.color.blend_en);
    MIX(g_st.blend.src_rgb); MIX(g_st.blend.dst_rgb); MIX(g_st.blend.src_a); MIX(g_st.blend.dst_a);
    MIX(g_st.blend.eq_rgb); MIX(g_st.blend.eq_a); MIX(g_st.blend.adv); MIX(g_st.blend.adv_npm);
    MIX(g_st.blend.adv_ovl); MIX(g_st.blend.adv_dst_free);
    MIX(g_st.chmask.mask); MIX(g_st.color.logic_op);
    MIX(g_st.color.alpha_func);
    { uint32_t ref; memcpy(&ref, &g_st.alpha_ref, sizeof ref); MIX(ref); }   // alpha ref/mask are pipeline specialization constants
    MIX(g_st.ds.dt); MIX(g_st.ds.dw); MIX(g_st.ds.df); MIX(g_st.ds.st); MIX(g_st.ds.sf_f); MIX(g_st.ds.sf_b);
    MIX(g_st.ds.fail_f); MIX(g_st.ds.zfail_f); MIX(g_st.ds.zpass_f); MIX(g_st.ds.fail_b); MIX(g_st.ds.zfail_b); MIX(g_st.ds.zpass_b);
    MIX(g_st.st_vmask[0]); MIX(g_st.st_vmask[1]); MIX(g_st.st_wmask[0]); MIX(g_st.st_wmask[1]);
    MIX(g_st.ms.enable); MIX(g_st.depth_clamp); MIX(g_st.rast_discard); MIX(g_st.patch_size); MIX((int)g_draw_topo); MIX(g_st.has_po);
    MIX(g_queue_origin_ll);   // the queue's raster flip selects the effective front face
    { uint32_t lw; memcpy(&lw, &g_st.line_width, sizeof lw); MIX(lw); }
    MIX(g_st.po.factor); MIX(g_st.po.units); MIX(g_st.po.clamp); MIX(g_st.restart.on);
    for (int i = 0; i < 32; i++) { MIX(g_st.attr[i].fmt); MIX(g_st.attr[i].offset); MIX(g_st.attr[i].stream); }
    for (int i = 0; i < 16; i++) MIX(g_st.stream[i].stride);
    return h;
}

typedef struct { int ncolor; VkFormat color[8]; VkFormat depth; uint32_t samples; VkRenderPass rp; } rp_ent;
static rp_ent g_rps[32]; static int g_nrp;

static VkRenderPass rp_get(int ncolor, const VkFormat *color, VkFormat depth, uint32_t samples) {
    for (int i = 0; i < g_nrp; i++) {
        rp_ent *r = &g_rps[i];
        if (r->ncolor != ncolor || r->depth != depth || r->samples != samples) continue;
        int same = 1;
        for (int k = 0; k < ncolor; k++) if (r->color[k] != color[k]) same = 0;
        if (same) return r->rp;
    }
    if (g_nrp == (int)(sizeof g_rps / sizeof *g_rps)) return VK_NULL_HANDLE;
    rp_ent *r = &g_rps[g_nrp];
    VkAttachmentDescription at[9] = {{0}};
    VkAttachmentReference ref[9] = {{0}};
    for (int i = 0; i < ncolor; i++) {
        at[i].format = color[i]; at[i].samples = (VkSampleCountFlagBits)samples;
        at[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[i].initialLayout = VK_IMAGE_LAYOUT_GENERAL; at[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        ref[i] = (VkAttachmentReference){(uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkAttachmentReference dref = {0};
    int has_depth = depth != VK_FORMAT_UNDEFINED;
    if (has_depth) {
        at[ncolor].format = depth; at[ncolor].samples = (VkSampleCountFlagBits)samples;
        at[ncolor].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[ncolor].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[ncolor].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[ncolor].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[ncolor].initialLayout = VK_IMAGE_LAYOUT_GENERAL; at[ncolor].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        dref = (VkAttachmentReference){(uint32_t)ncolor, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = (uint32_t)ncolor; sub.pColorAttachments = ref;
    sub.pDepthStencilAttachment = has_depth ? &dref : NULL;
    VkSubpassDependency dep[2] = {{0}};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = (uint32_t)(ncolor + (has_depth ? 1 : 0)); ci.pAttachments = at;
    ci.subpassCount = 1; ci.pSubpasses = &sub;
    ci.dependencyCount = 2; ci.pDependencies = dep;
    if (vkCreateRenderPass(g_vk.device, &ci, NULL, &r->rp) != VK_SUCCESS) return VK_NULL_HANDLE;
    r->ncolor = ncolor; r->depth = depth; r->samples = samples;
    for (int k = 0; k < ncolor; k++) r->color[k] = color[k];
    g_nrp++;
    return r->rp;
}

static VkPipeline pvk_pipeline(prog_vk *pv, VkRenderPass rp, int ncolor, const VkFormat *color, VkFormat depth, uint32_t samples) {
    uint64_t key = state_hash() ^ ((uint64_t)(uintptr_t)rp * 1000003ull) ^ ((uint64_t)samples << 57);
    if (pv->pipe && pv->pipe_key == key && pv->pipe_rp == rp) return pv->pipe;
    if (pv->pipe) { vkDestroyPipeline(g_vk.device, pv->pipe, NULL); pv->pipe = VK_NULL_HANDLE; }
    pv->pipe_rp = rp; pv->pipe_key = key;
    if (pv->have[3] != pv->have[4]) { LOG("[RED] [nvn/vk] program %llu has only one tessellation stage", (unsigned long long)pv->serial); return VK_NULL_HANDLE; }
    VkShaderModule geo_mod = VK_NULL_HANDLE;
    if (pv->have[2]) {
        // After tessellation the geometry stage sees triangles; otherwise the
        // draw primitive selects the variant.
        int gtopo = pv->have[3] ? 3 : geo_topo_index(g_draw_topo);
        if (gtopo < 0 || gtopo > 4 || !pv->gmod[gtopo]) {
            LOG("[RED] [nvn/vk] program %llu has no geometry shader for input topology %d (draw topology %d): the draw is dropped",
                (unsigned long long)pv->serial, gtopo, (int)g_draw_topo);
            return VK_NULL_HANDLE;
        }
        geo_mod = pv->gmod[gtopo];
    }
    VkPipelineShaderStageCreateInfo st[5] = {{0}};
    int ns = 0, frag_i = -1;
    st[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[ns].stage = VK_SHADER_STAGE_VERTEX_BIT; st[ns].module = pv->mod[0]; st[ns].pName = pv->sh[0].entry; ns++;
    if (pv->have[3]) { st[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[ns].stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; st[ns].module = pv->mod[3]; st[ns].pName = pv->sh[3].entry; ns++; }
    if (pv->have[4]) { st[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[ns].stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; st[ns].module = pv->mod[4]; st[ns].pName = pv->sh[4].entry; ns++; }
    if (pv->have[2]) { st[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[ns].stage = VK_SHADER_STAGE_GEOMETRY_BIT; st[ns].module = geo_mod; st[ns].pName = pv->sh[2].entry; ns++; }
    if (pv->have[1]) { frag_i = ns; st[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[ns].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[ns].module = pv->mod[1]; st[ns].pName = pv->sh[1].entry; ns++; }
    if (!pv->have[1] && ncolor > 0) { LOG("[RED] [nvn/vk] program %llu has no fragment stage but colour targets are bound", (unsigned long long)pv->serial); return VK_NULL_HANDLE; }
    struct { float ref; uint32_t mask; } aspec;
    VkSpecializationMapEntry aspec_entries[2] = {{0, 0, 4}, {0, 4, 4}};
    VkSpecializationInfo aspec_info = {0};
    if (pv->has_alpha && frag_i >= 0) {
        int f = g_st.color.alpha_func;
        uint32_t mask = 7;
        switch (f) {
        case NVN_ALPHA_FUNC_NEVER: mask = 0; break;
        case NVN_ALPHA_FUNC_LESS: mask = 1; break;
        case NVN_ALPHA_FUNC_EQUAL: mask = 2; break;
        case NVN_ALPHA_FUNC_LEQUAL: mask = 3; break;
        case NVN_ALPHA_FUNC_GREATER: mask = 4; break;
        case NVN_ALPHA_FUNC_NOTEQUAL: mask = 5; break;
        case NVN_ALPHA_FUNC_GEQUAL: mask = 6; break;
        default: mask = 7; break;
        }
        aspec.ref = g_st.alpha_ref;
        aspec.mask = mask;
        aspec_entries[0].constantID = (uint32_t)pv->alpha_ref_id;
        aspec_entries[1].constantID = (uint32_t)pv->alpha_mask_id;
        aspec_info.mapEntryCount = 2; aspec_info.pMapEntries = aspec_entries;
        aspec_info.dataSize = sizeof aspec; aspec_info.pData = &aspec;
        st[frag_i].pSpecializationInfo = &aspec_info;
    }
    VkVertexInputBindingDescription vb[16] = {{0}};
    VkVertexInputAttributeDescription va[32] = {{0}};
    int nvb = 0, nva = 0;
    int used_stream[16] = {0};
    for (int i = 0; i < pv->sh[0].nvin && nva < 32; i++) {
        int loc = pv->sh[0].vin[i].location;
        if (loc < 0 || loc >= 32) continue;
        va_st a = g_st.attr[loc];
        if (!a.fmt) continue;
        VkFormat f = vkfmt((int)a.fmt);
        if (f == VK_FORMAT_UNDEFINED) { LOG("[RED] [nvn/vk] vertex attribute %d format %u is not mapped", loc, a.fmt); continue; }
        if (a.stream >= 16) continue;
        va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)loc, a.stream, f, a.offset};
        used_stream[a.stream] = 1;
    }
    for (int s = 0; s < 16; s++) if (used_stream[s]) {
        vb[nvb++] = (VkVertexInputBindingDescription){(uint32_t)s, g_st.stream[s].stride,
                                                      g_st.stream[s].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    }
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = (uint32_t)nvb; vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = (uint32_t)nva; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = pv->have[3] ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : g_draw_topo;
    ia.primitiveRestartEnable = g_st.restart.on != 0 && (g_draw_topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || g_draw_topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
    VkPipelineTessellationStateCreateInfo tess = {0};
    tess.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tess.patchControlPoints = g_st.patch_size > 0 ? (uint32_t)g_st.patch_size : 3;
    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = g_st.depth_clamp;
    rs.rasterizerDiscardEnable = g_st.rast_discard;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = g_st.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : g_st.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : g_st.poly.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    // NVN's declared front face is the winding after the viewport swizzle. A
    // lower-left queue origin flips the raster direction, so it flips the
    // Vulkan front-face enum with it (the same rule as the Metal host).
    rs.frontFace = (g_st.poly.front == NVN_FRONT_FACE_CW) != g_queue_origin_ll
                   ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // NVN draws lines wider than a pixel; Vulkan can only exceed 1 when the
    // device advertises wideLines (queried in nx_vk.c). Clamp to the device's
    // supported range instead of rejecting the draw.
    float lw = g_st.line_width > 0.0f ? g_st.line_width : 1.0f;
    if (!g_vk.caps.features.wideLines) lw = 1.0f;
    if (lw > g_vk.caps.props.limits.lineWidthRange[1]) lw = g_vk.caps.props.limits.lineWidthRange[1];
    if (lw < g_vk.caps.props.limits.lineWidthRange[0]) lw = g_vk.caps.props.limits.lineWidthRange[0];
    rs.lineWidth = lw;
    rs.depthBiasEnable = g_st.has_po && (g_st.po.factor != 0 || g_st.po.units != 0);
    rs.depthBiasConstantFactor = g_st.po.units;
    rs.depthBiasSlopeFactor = g_st.po.factor;
    rs.depthBiasClamp = g_st.po.clamp;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)samples;
    VkPipelineDepthStencilStateCreateInfo dss = {0};
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = depth != VK_FORMAT_UNDEFINED && g_st.ds.dt;
    dss.depthWriteEnable = g_st.ds.dw;
    dss.depthCompareOp = vkcmp(g_st.ds.df);
    dss.depthBoundsTestEnable = VK_FALSE;
    dss.stencilTestEnable = g_st.ds.st;
    dss.front.failOp = vkstop(g_st.ds.fail_f);
    dss.front.passOp = vkstop(g_st.ds.zpass_f);
    dss.front.depthFailOp = vkstop(g_st.ds.zfail_f);
    dss.front.compareOp = vkcmp(g_st.ds.sf_f);
    dss.front.compareMask = g_st.st_vmask[0];
    dss.front.writeMask = g_st.st_wmask[0];
    dss.front.reference = (uint32_t)g_st.st_ref[0];
    dss.back.failOp = vkstop(g_st.ds.fail_b);
    dss.back.passOp = vkstop(g_st.ds.zpass_b);
    dss.back.depthFailOp = vkstop(g_st.ds.zfail_b);
    dss.back.compareOp = vkcmp(g_st.ds.sf_b);
    dss.back.compareMask = g_st.st_vmask[1];
    dss.back.writeMask = g_st.st_wmask[1];
    dss.back.reference = (uint32_t)g_st.st_ref[1];
    int logic = ncolor > 0 && g_st.color.logic_op != NVN_LOGIC_OP_COPY;
    int blend_active = 0;
    for (int i = 0; i < ncolor; i++) blend_active |= (g_st.color.blend_en >> i) & 1;
    VkBlendOp advanced_op = VK_BLEND_OP_ADD;
    int native_advanced = !logic && blend_active &&
        nvn_advanced_blend_supported(g_st.blend, ncolor, &advanced_op);
    blend_st bl = native_advanced ? g_st.blend : adv_blend(g_st.blend);
    VkPipelineColorBlendAttachmentState cba[8] = {{0}};
    for (int i = 0; i < ncolor; i++) {
        int on = (g_st.color.blend_en >> i) & 1;
        cba[i].blendEnable = (VkBool32)(!logic && on);   // a logic operation takes the place of blending
        if (native_advanced && on) {
            cba[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba[i].colorBlendOp = advanced_op;
            cba[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba[i].alphaBlendOp = advanced_op;
        } else {
            cba[i].srcColorBlendFactor = vkbfac(bl.src_rgb);
            cba[i].dstColorBlendFactor = vkbfac(bl.dst_rgb);
            cba[i].colorBlendOp = vkbeq(bl.eq_rgb);
            cba[i].srcAlphaBlendFactor = vkbfac(bl.src_a);
            cba[i].dstAlphaBlendFactor = vkbfac(bl.dst_a);
            cba[i].alphaBlendOp = vkbeq(bl.eq_a);
        }
        cba[i].colorWriteMask = (VkColorComponentFlags)((g_st.chmask.mask >> (4 * i)) & 0xf);
    }
    VkPipelineColorBlendAdvancedStateCreateInfoEXT advanced_state = {0};
    advanced_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT;
    advanced_state.srcPremultiplied = !g_st.blend.adv_npm;
    advanced_state.dstPremultiplied = VK_TRUE;
    advanced_state.blendOverlap = (VkBlendOverlapEXT)g_st.blend.adv_ovl;
    VkPipelineColorBlendStateCreateInfo cbs = {0};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = (uint32_t)ncolor; cbs.pAttachments = ncolor ? cba : NULL;
    if (native_advanced) cbs.pNext = &advanced_state;
    if (logic) {
        cbs.logicOpEnable = VK_TRUE;
        cbs.logicOp = vklogic(g_st.color.logic_op);
    }
    cbs.blendConstants[0] = g_st.blend_color[0]; cbs.blendConstants[1] = g_st.blend_color[1];
    cbs.blendConstants[2] = g_st.blend_color[2]; cbs.blendConstants[3] = g_st.blend_color[3];
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 3; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = (uint32_t)ns; ci.pStages = st;
    ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps; ci.pRasterizationState = &rs; ci.pMultisampleState = &ms;
    if (pv->have[3]) ci.pTessellationState = &tess;
    ci.pDepthStencilState = &dss; ci.pColorBlendState = &cbs; ci.pDynamicState = &ds;
    ci.layout = pv->pl; ci.renderPass = rp; ci.subpass = 0;
    if (vkCreateGraphicsPipelines(g_vk.device, g_vk.pipeline_cache, 1, &ci, NULL, &pv->pipe) != VK_SUCCESS) {
        LOG("[RED] [nvn/vk] pipeline creation failed for program %llu", (unsigned long long)pv->serial);
        pv->pipe = VK_NULL_HANDLE; return VK_NULL_HANDLE;
    }
    return pv->pipe;
}

typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; size_t size, cap; } hbuf;
// Upload blocks are pooled across draws and batches: the replay used to create
// a VkBuffer and bind a VkDeviceMemory for every uniform/vertex copy (about
// 5 ms a frame on Piccolo S). A released block returns to this list when its
// batch completes, so the next draw reuses it.
typedef struct hb_free { struct hb_free *next; VkBuffer buf; VkDeviceMemory mem; void *map; size_t cap; } hb_free;
static hb_free *g_hb_free;
void hb_free_push(VkBuffer b, VkDeviceMemory m, void *map, size_t cap) {
    hb_free *e = malloc(sizeof *e);
    if (!e) { nx_vk_destroy_buffer(&g_vk, b, m); return; }
    e->buf = b; e->mem = m; e->map = map; e->cap = cap;
    e->next = g_hb_free; g_hb_free = e;
}
static hb_free *hb_take(size_t n) {
    hb_free **pp = &g_hb_free;
    while (*pp) {
        if ((*pp)->cap >= n) { hb_free *e = *pp; *pp = e->next; return e; }
        pp = &(*pp)->next;
    }
    size_t cap = (n + 4095) & ~(size_t)4095;
    hb_free *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (nx_vk_create_buffer(&g_vk, cap,
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &e->buf, &e->mem, &e->map) != 0) {
        free(e); return NULL;
    }
    e->cap = cap;
    return e;
}
static int hbuf_make_impl(hbuf *b, const void *src, size_t n);
static int hbuf_make(hbuf *b, const void *src, size_t n) {
    uint64_t _t0 = nx_monotonic_ns();
    memset(b, 0, sizeof *b);
    if (!n) n = 4;
    int _r = hbuf_make_impl(b, src, n);
    g_prof[PROF_HBUF] += nx_monotonic_ns() - _t0;
    return _r;
}
uint64_t g_hbuf_bytes, g_hbuf_calls, g_hbuf_big;
static int hbuf_make_impl(hbuf *b, const void *src, size_t n) {
    memset(b, 0, sizeof *b);
    if (!n) n = 4;
    g_hbuf_calls++; g_hbuf_bytes += n; if (n > 65536) g_hbuf_big++;
    hb_free *e = hb_take(n);
    if (!e) return -1;
    b->buf = e->buf; b->mem = e->mem; b->map = e->map; b->size = n; b->cap = e->cap;
    free(e);
    if (src) memcpy(b->map, src, n);
    return 0;
}
// Staging buffers are used by commands in the submission batch, so their
// destruction waits for the batch's wait (nvn_vk.c sub_flush).
static void sub_free_hbuf(hbuf *b) { if (b->buf || b->mem) sub_release_upload(b->buf, b->mem, b->map, b->cap); memset(b, 0, sizeof *b); }

static VkSampler sampler_get_impl(int sid);
static VkSampler sampler_get(int sid) {
    uint64_t t0 = nx_monotonic_ns();
    VkSampler r = sampler_get_impl(sid);
    g_prof[PROF_SAMP] += nx_monotonic_ns() - t0;
    return r;
}
static VkSampler sampler_get_impl(int sid) {
    mu_lock();
    for (int i = g_nsamp - 1; i >= 0; i--) if (g_samps[i].id == sid) {
        if (g_samps[i].vk) { VkSampler r = g_samps[i].vk; mu_unlock(); return r; }
        samp_par p = g_samps[i].p;
        VkSamplerCreateInfo ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = vkfilter_mag(p.magf);
        ci.minFilter = vkfilter_min(p.minf);
        ci.mipmapMode = vkmip(p.minf);
        ci.addressModeU = vkwrap(p.ws); ci.addressModeV = vkwrap(p.wt); ci.addressModeW = vkwrap(p.wr);
        ci.mipLodBias = p.lod_bias;
        ci.anisotropyEnable = p.aniso > 1.0f && g_vk.caps.features.samplerAnisotropy;
        ci.maxAnisotropy = p.aniso > 1.0f ? p.aniso : 1.0f;
        ci.compareEnable = p.cmp_mode != NVN_COMPARE_MODE_NONE;
        ci.compareOp = vkcmp(p.cmp_func);
        ci.minLod = p.lod_min; ci.maxLod = p.lod_max;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        if (p.border[0] == 0 && p.border[1] == 0 && p.border[2] == 0 && p.border[3] >= 0.5f) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        ci.unnormalizedCoordinates = VK_FALSE;
        if (vkCreateSampler(g_vk.device, &ci, NULL, &g_samps[i].vk) != VK_SUCCESS) { mu_unlock(); return VK_NULL_HANDLE; }
        VkSampler r = g_samps[i].vk; mu_unlock(); return r;
    }
    // A handle with no registered sampler: NVN's default (nearest, clamp)
    VkSamplerCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_NEAREST; ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(g_vk.device, &ci, NULL, &s);
    mu_unlock();
    return s;
}

static void drv_fill(uint8_t *d, int bank, int st, const char *shader_key) {
    memset(d, 0, DMSH_DRV_SIZE);
    if (bank == 0) {
        static const unsigned bases[6] = {DMSH_C0_SSBO_VS, DMSH_C0_SSBO_CS, DMSH_C0_SSBO_GS, 0, 0, DMSH_C0_SSBO_CS};
        unsigned base = st < 6 ? bases[st] : 0;
        if (st == 5) {
            memcpy(d + DMSH_C0_DISPATCH, g_st.dispatch, sizeof g_st.dispatch);
            if (nx_env_bool("NX_VK_DEBUG", 0))
                LOG("[nvn/vk] shader %s driver c0 stage=%d dispatch=(%u,%u,%u)", shader_key, st,
                    g_st.dispatch[0], g_st.dispatch[1], g_st.dispatch[2]);
            for (int r = 0; r < DMSH_C0_CS_UBO_SLOTS; r++) {
                uint64_t a = g_st.ubo[5][r].addr; uint32_t n = (uint32_t)g_st.ubo[5][r].size;
                memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r, &a, 8);
                memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r + 8, &n, 4);
                if (a && nx_env_bool("NX_VK_DEBUG", 0))
                    LOG("[nvn/vk] shader %s driver c0 stage=%d ubo[%d] address=%#llx size=%u", shader_key, st, r,
                        (unsigned long long)a, n);
            }
        }
        if (base) for (int r = 0; r < DMSH_C0_SSBO_SLOTS; r++) {
            uint64_t a = g_st.ssbo[st][r].addr; uint32_t n = (uint32_t)g_st.ssbo[st][r].size;
            memcpy(d + base + DMSH_C0_ROW * r, &a, 8);
            memcpy(d + base + DMSH_C0_ROW * r + 8, &n, 4);
            if (a && nx_env_bool("NX_VK_DEBUG", 0))
                LOG("[nvn/vk] shader %s driver c0 stage=%d ssbo[%d] address=%#llx size=%u", shader_key, st, r,
                    (unsigned long long)a, n);
        }
        return;
    }
    if (bank == 2) {
        for (int r = 0; r < 256; r++) {
            uint64_t h = g_st.tex[st][r];
            if (!h) continue;
            unsigned t = (unsigned)(h & 0xfffff);
            unsigned s = (unsigned)(h >> 20);
            if (t) t += (unsigned)g_st.tex_base;
            if (s) s += (unsigned)g_st.samp_base;
            uint64_t v = t | ((uint64_t)(s & 0xfff) << 20);
            unsigned off;
            if (r < DMSH_C2_TEX_SLOTS) off = DMSH_C2_TEX + DMSH_C2_ROW * (unsigned)r;
            else off = DMSH_C2_SEPTEX + DMSH_C2_ROW * (unsigned)(r - NVN_SEPTEX_SLOT0);
            if (off + 8 <= DMSH_DRV_SIZE) memcpy(d + off, &v, 4);
            if (v && nx_env_bool("NX_VK_DEBUG", 0))
                LOG("[nvn/vk] shader %s driver c2 stage=%d texture[%d] handle=%#llx", shader_key, st, r, (unsigned long long)v);
        }
        for (int r = 0; r < 8; r++) {
            uint64_t h = g_st.img[st][r];
            if (!h) continue;
            uint64_t v = (h & 0xfffff) + (uint64_t)g_st.tex_base;
            unsigned off = DMSH_C2_IMG + DMSH_C2_ROW * (unsigned)r;
            if (off + 8 <= DMSH_DRV_SIZE) memcpy(d + off, &v, 4);
            if (v && nx_env_bool("NX_VK_DEBUG", 0))
                LOG("[nvn/vk] shader %s driver c2 stage=%d image[%d] handle=%#llx", shader_key, st, r, (unsigned long long)v);
        }
        for (int r = 0; r < 128; r++) {
            uint64_t h = g_st.samp[st][r];
            if (!h) continue;
            uint64_t v = (h & 0xfffff) + (uint64_t)g_st.samp_base;
            unsigned off = DMSH_C2_SAMP + DMSH_C2_ROW * (unsigned)r;
            if (off + 8 <= DMSH_DRV_SIZE) memcpy(d + off, &v, 4);
            if (v && nx_env_bool("NX_VK_DEBUG", 0))
                LOG("[nvn/vk] shader %s driver c2 stage=%d sampler[%d] handle=%#llx", shader_key, st, r, (unsigned long long)v);
        }
    }
}

// Views: NVN texture views can change the format (an sRGB twin of a linear
// texture) and pick a level/layer range. Vulkan requires a separate image view
// for that; the images are created mutable, as Vulkan requires for twin views.
typedef struct { uint64_t serial, used; int fmt, base, levels, minl, nlayers; uint8_t has_swz, swz[4], one_alpha; VkImageView view; int hnext; } tex_view_ent;
static tex_view_ent g_tviews[1024]; static int g_ntv;
// A hash of the cache key: walking up to 1,024 entries cost a frame's 2.5 ms.
// Buckets hold entry index + 1; a removal that swaps entries rebuilds them.
static int *g_tv_hash; static int g_tv_hash_n;
static unsigned tv_hash_of(uint64_t serial, int fmt, int base, int levels, int minl, int nlayers, int has_swz, const uint8_t swz[4], int one_alpha) {
    uint64_t h = serial * 0x9E3779B97F4A7C15ull ^ (uint64_t)(fmt * 131 + base * 17 + levels * 7 + minl * 3 + nlayers);
    h ^= (uint64_t)has_swz << 60 ^ (uint64_t)one_alpha << 59;
    if (has_swz) for (int i = 0; i < 4; i++) h = (h ^ swz[i]) * 0x100000001B3ull;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 29;
    return (int)(h & (uint64_t)(g_tv_hash_n - 1));
}
static void tv_hash_rebuild(void) {
    if (!g_tv_hash) { g_tv_hash_n = 2048; g_tv_hash = calloc((size_t)g_tv_hash_n, sizeof *g_tv_hash); }
    if (!g_tv_hash) return;
    memset(g_tv_hash, 0, (size_t)g_tv_hash_n * sizeof *g_tv_hash);
    for (int i = 0; i < g_ntv; i++) {
        tex_view_ent *x = &g_tviews[i];
        int h = tv_hash_of(x->serial, x->fmt, x->base, x->levels, x->minl, x->nlayers, x->has_swz, x->swz, x->one_alpha);
        x->hnext = g_tv_hash[h]; g_tv_hash[h] = i + 1;
    }
}
// Grow the table before the new entry is written: a rebuild walks the live
// entries, and adding an entry a rebuild already linked would link it to
// itself (the chain then never ends, which hung the replay inside cached_view).
static void tv_hash_ensure(void) {
    if (!g_tv_hash) tv_hash_rebuild();
    else if (g_ntv >= g_tv_hash_n * 2) tv_hash_rebuild();
}
static void tv_hash_add(int ix) {
    if (!g_tv_hash) return;
    tex_view_ent *x = &g_tviews[ix];
    int h = tv_hash_of(x->serial, x->fmt, x->base, x->levels, x->minl, x->nlayers, x->has_swz, x->swz, x->one_alpha);
    x->hnext = g_tv_hash[h]; g_tv_hash[h] = ix + 1;
}
static void tv_hash_del(int ix) {
    if (!g_tv_hash) return;
    tex_view_ent *x = &g_tviews[ix];
    int h = tv_hash_of(x->serial, x->fmt, x->base, x->levels, x->minl, x->nlayers, x->has_swz, x->swz, x->one_alpha);
    int *pp = &g_tv_hash[h];
    while (*pp && *pp != ix + 1) pp = &g_tviews[*pp - 1].hnext;
    if (*pp) *pp = x->hnext;
}
static uint64_t g_tv_clock;   // a view cache entry's last use, for eviction
// One-entry memo of the last view lookup: consecutive draws sample nearly the
// same textures, and walking the 1,024-entry cache was 3 ms a frame.
static struct { uint64_t serial; int fmt, base, levels, minl, nlayers; uint8_t has_swz, swz[4], one_alpha; VkImageView view; } g_tv_memo;

static VkImageAspectFlags nvn_view_aspect(int fmt) {
    if (fmt == NVN_FORMAT_DEPTH16 || fmt == NVN_FORMAT_DEPTH24 || fmt == NVN_FORMAT_DEPTH32F) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (fmt == NVN_FORMAT_DEPTH24_STENCIL8 || fmt == NVN_FORMAT_DEPTH32F_STENCIL8) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (fmt == NVN_FORMAT_STENCIL8) return VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}
// The channel swizzle a sampled view reads through: the view's own, else the
// texture builder's, as on Metal (treg_swizzle). NVN names the source channel
// each output component takes (ZERO/ONE/R/G/B/A).
static int nvn_view_swz(const tex_ent *e, const view_t *v, uint8_t out[4]) {
    if (v && v->set_swizzle) { memcpy(out, v->swz, 4); return 1; }
    if (e->t.set_swz) { for (int c = 0; c < 4; c++) out[c] = (uint8_t)(e->t.swz >> (3 * c) & 7); return 1; }
    return 0;
}
static VkComponentSwizzle nvn_swz_vk(uint8_t s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}
// A view's format/levels/layers as a cached VkImageView. Without a view, the
// texture's base view. A sampled view also carries its channel swizzle: Vulkan
// allows a component mapping on sampled image views only, so render targets and
// storage images pass sampled = 0 and keep the identity.
// One render pass spans consecutive draws with the same attachments. Beginning
// and ending a pass per draw (as this replay did) loaded and stored every
// attachment for each of a frame's ~500 draws: on a tile-based Apple GPU that
// was the frame's whole GPU cost. The pass closes when the attachments or the
// command buffer change, before any copy/clear/dispatch, and at submission.
static struct { int on; VkCommandBuffer cb; VkRenderPass rp; VkFramebuffer fb; uint32_t w, h, nv; VkImageView views[9]; } g_rp;
// The pipeline, viewport, scissor and stencil reference of the open pass: a
// draw that repeats them skips the command (Mesa records each as a command and
// processes it at submit).
static VkPipeline g_cur_pipe;
static VkViewport g_cur_vp; static VkRect2D g_cur_sc; static int g_cur_vp_valid, g_cur_sc_valid;
static uint32_t g_cur_stencil; static int g_cur_stencil_valid;
static VkBuffer g_cur_vbuf[16]; static int g_cur_vbuf_valid[16];
static VkBuffer g_cur_ibuf; static VkIndexType g_cur_itype; static int g_cur_ibuf_valid;
void nvkd_rp_close(void) {
    if (!g_rp.on) return;
    vkCmdEndRenderPass(g_rp.cb);
    sub_free_fb(g_rp.fb);
    memset(&g_rp, 0, sizeof g_rp);
    g_cur_pipe = VK_NULL_HANDLE; g_cur_vp_valid = g_cur_sc_valid = g_cur_stencil_valid = 0;
    memset(g_cur_vbuf_valid, 0, sizeof g_cur_vbuf_valid); g_cur_ibuf_valid = 0;
}

// A finalized texture's cached views are unreachable (serials are never
// reused); drop them so no view outlives its image. Called under the replay lock.
void nvn_views_drop_serial(uint64_t serial) {
    if (g_tv_memo.serial == serial) memset(&g_tv_memo, 0, sizeof g_tv_memo);
    int removed = 0;
    for (int i = 0; i < g_ntv; i++) if (g_tviews[i].serial == serial) {
        if (g_tviews[i].view) sub_free_iv(g_tviews[i].view);
        g_tviews[i] = g_tviews[--g_ntv];
        i--; removed = 1;
    }
    if (removed) tv_hash_rebuild();
}

static VkImageView cached_view(tex_ent *e, const view_t *v, VkFormat *fmt_out, int sampled) {
    uint64_t _tv = nx_monotonic_ns();
    uint8_t swz[4] = {0};
    int has_swz = sampled && nvn_view_swz(e, v, swz);
    if (has_swz && swz[0] == NVN_TEXTURE_SWIZZLE_R && swz[1] == NVN_TEXTURE_SWIZZLE_G &&
        swz[2] == NVN_TEXTURE_SWIZZLE_B && swz[3] == NVN_TEXTURE_SWIZZLE_A) has_swz = 0;
    int plain = !v || (!v->set_format && !v->set_levels && !v->set_layers);
    int fmt = v && v->set_format ? v->format : 0, base = v && v->set_levels ? v->base_level : 0;
    int levels = v && v->set_levels ? v->nlevels : 1;
    int minl = v && v->set_layers ? v->min_layer : 0, nlayers = v && v->set_layers ? v->nlayers : 1;
    VkFormat vf = fmt ? vkfmt(fmt) : e->fmt;
    if (vf == VK_FORMAT_UNDEFINED) vf = e->fmt;
    if (fmt_out) *fmt_out = vf;
    // A format with no alpha channel (an RGBX texture, read through the RGBA
    // format of the same size) reads alpha as 1. An explicit swizzle wins.
    int one_alpha = sampled && !has_swz && nvn_fmt_no_alpha(fmt ? fmt : e->t.d.format);
    if (plain && !has_swz && !one_alpha) { g_prof[PROF_VIEW] += nx_monotonic_ns() - _tv; return e->view; }
    if (g_tv_memo.serial == e->serial && g_tv_memo.fmt == fmt && g_tv_memo.base == base &&
        g_tv_memo.levels == levels && g_tv_memo.minl == minl && g_tv_memo.nlayers == nlayers &&
        g_tv_memo.has_swz == (uint8_t)has_swz && g_tv_memo.one_alpha == (uint8_t)one_alpha &&
        (!has_swz || !memcmp(g_tv_memo.swz, swz, 4)))
        { g_prof[PROF_VIEW] += nx_monotonic_ns() - _tv; return g_tv_memo.view; }
    if (g_tv_hash) {
        int h = tv_hash_of(e->serial, fmt, base, levels, minl, nlayers, has_swz, swz, one_alpha);
        for (int i = g_tv_hash[h]; i; i = g_tviews[i - 1].hnext) {
            tex_view_ent *x = &g_tviews[i - 1];
            if (x->serial != e->serial || x->fmt != fmt || x->base != base || x->levels != levels ||
                x->minl != minl || x->nlayers != nlayers || x->has_swz != (uint8_t)has_swz ||
                x->one_alpha != (uint8_t)one_alpha || (has_swz && memcmp(x->swz, swz, 4))) continue;
            x->used = ++g_tv_clock;
            g_tv_memo.serial = e->serial; g_tv_memo.fmt = fmt; g_tv_memo.base = base;
            g_tv_memo.levels = levels; g_tv_memo.minl = minl; g_tv_memo.nlayers = nlayers;
            g_tv_memo.has_swz = (uint8_t)has_swz; memcpy(g_tv_memo.swz, swz, 4);
            g_tv_memo.one_alpha = (uint8_t)one_alpha; g_tv_memo.view = x->view;
            g_prof[PROF_VIEW] += nx_monotonic_ns() - _tv; return x->view;
        }
    }
    // The cache is bounded: a game with thousands of textures and swizzled or
    // sRGB views needs more than any fixed count. Evict the least recently used
    // entry; its view is destroyed when the batch that may still use it completes.
    tex_view_ent *slot = NULL;
    if (g_ntv >= (int)(sizeof g_tviews / sizeof *g_tviews)) {
        slot = &g_tviews[0];
        for (int i = 1; i < g_ntv; i++) if (g_tviews[i].used < slot->used) slot = &g_tviews[i];
        tv_hash_del((int)(slot - g_tviews));
        sub_free_iv(slot->view);   // in use by this batch, freed with it
        memset(slot, 0, sizeof *slot);
    }
    VkImageViewCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = e->img; ci.format = vf;
    if (plain) {
        // A swizzle on the texture's own view: the whole texture, as e->view.
        ci.viewType = e->view_type;
        ci.subresourceRange = tex_subresources(e);
    } else {
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.subresourceRange = (VkImageSubresourceRange){nvn_view_aspect(fmt ? fmt : e->t.d.format),
            (uint32_t)base, (uint32_t)levels, (uint32_t)minl, (uint32_t)nlayers};
    }
    if (has_swz) {
        ci.components.r = nvn_swz_vk(swz[0]); ci.components.g = nvn_swz_vk(swz[1]);
        ci.components.b = nvn_swz_vk(swz[2]); ci.components.a = nvn_swz_vk(swz[3]);
    } else if (one_alpha) ci.components.a = VK_COMPONENT_SWIZZLE_ONE;
    VkImageView iv = VK_NULL_HANDLE;
    if (vkCreateImageView(g_vk.device, &ci, NULL, &iv) != VK_SUCCESS) { g_prof[PROF_VIEW] += nx_monotonic_ns() - _tv; return e->view; }
    if (!slot) tv_hash_ensure();
    tex_view_ent *x = slot ? slot : &g_tviews[g_ntv++];
    *x = (tex_view_ent){e->serial, 0, fmt, base, levels, minl, nlayers, (uint8_t)has_swz, {0}, (uint8_t)one_alpha, iv};
    if (has_swz) memcpy(x->swz, swz, 4);
    x->used = ++g_tv_clock;
    tv_hash_add((int)(x - g_tviews));
    g_tv_memo.serial = e->serial; g_tv_memo.fmt = fmt; g_tv_memo.base = base;
    g_tv_memo.levels = levels; g_tv_memo.minl = minl; g_tv_memo.nlayers = nlayers;
    g_tv_memo.has_swz = (uint8_t)has_swz; memcpy(g_tv_memo.swz, swz, 4);
    g_tv_memo.one_alpha = (uint8_t)one_alpha; g_tv_memo.view = iv;
    { g_prof[PROF_VIEW] += nx_monotonic_ns() - _tv; return iv; }
}

// One guest texture id -> sampled image view. Uploads the guest storage when the
// GPU has not written it since the last CPU write.
static tex_ent *texid_image(int id, VkImageView *view) {
    tex_id_ent *t = tid_find(id);
    if (!t || t->gone) return NULL;
    tex_ent *e = tex_ensure(&t->t);
    if (!e) return NULL;
    if (tex_stale(e, &t->t) && upload_guest_texture(&t->t, e) != 0) return NULL;
    *view = cached_view(e, t->has_view ? &t->v : NULL, NULL, 1);
    return e;
}

typedef struct { tex_ent *texture; int writable; } nvn_image_use;

static int nvn_image_use_add(nvn_image_use *uses, size_t *count, size_t capacity,
                             tex_ent *texture, int writable) {
    if (!texture) return 0;
    for (size_t i = 0; i < *count; i++) if (uses[i].texture == texture) {
        uses[i].writable |= writable;
        return 0;
    }
    if (*count >= capacity) return -1;
    uses[*count] = (nvn_image_use){texture, writable};
    (*count)++;
    return 0;
}

static void nvn_image_shader_barrier(VkCommandBuffer cb, tex_ent *texture,
                                     VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = texture->layout;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = texture->img;
    b.subresourceRange = tex_subresources(texture);
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    // Emit the dependency when a write is not yet ordered for reads, or the
    // layout changes. A texture read by consecutive draws was already ordered
    // by the first read's barrier; a barrier per sampled image per draw was
    // thousands of commands a frame (2 ms of the submit's CPU).
    if (texture->layout == VK_IMAGE_LAYOUT_GENERAL && texture->rgen == texture->wgen) return;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_stage,
                         0, 0, NULL, 0, NULL, 1, &b);
    texture->rgen = texture->wgen;
    texture->layout = VK_IMAGE_LAYOUT_GENERAL;
}

// NVN barriers order earlier command-stream writes before later primitive,
// fragment, cache-invalidate, or indirect-data consumers. The renderer submits
// replay operations on one Vulkan queue, so a global memory dependency is the
// conservative Vulkan equivalent for every defined NVN barrier bit.
static void nvkd_barrier(int32_t bits) {
    const uint32_t known = NVN_BARRIER_ORDER_PRIMITIVES_BIT |
                           NVN_BARRIER_ORDER_FRAGMENTS_BIT |
                           NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT |
                           NVN_BARRIER_ORDER_INDIRECT_DATA_BIT |
                           NVN_BARRIER_INVALIDATE_TEXTURE_BIT |
                           NVN_BARRIER_INVALIDATE_SHADER_BIT |
                           NVN_BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR_BIT |
                           NVN_BARRIER_INVALIDATE_ZCULL_BIT;
    uint32_t requested = (uint32_t)bits;
    if (!requested) return;
    if (requested & ~known)
        LOG("[RED] [nvn/vk] barrier contains unknown bits %#x; applying a full Vulkan memory dependency", requested & ~known);
    VkCommandBuffer cb = rp_cb();
    if (!cb) {
        LOG("[RED] [nvn/vk] could not record NVN barrier bits %#x", requested);
        return;
    }
    VkMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b,
                         0, NULL, 0, NULL);
    rp_done(cb);
    atomic_fetch_add_explicit(&g_vk_barriers, 1, memory_order_relaxed);
}

// A bindless handle the game stores in one of its own uniform buffers
// (dmsh_reflect.py KIND_TEX_UBO / KIND_SAMP_UBO: reg = c[3..16] slot, off =
// byte offset). The value is read at draw time, as the game may rewrite it
// between draws.
static int ubo_handle(int stage, int reg, int off, uint32_t *out) {
    if (reg < 0 || reg >= 16 || off < 0) return -1;
    const p_bind_buf *b = &g_st.ubo[stage][reg];
    if (!b->addr || b->size < (uint32_t)off + 4) return -1;
    memcpy(out, (const void *)(uintptr_t)(b->addr + (uint64_t)off), 4);
    return 0;
}

// A bindless sampler's other half (dmsh_reflect.py decode_ubo_tex type 1):
// sreg = a uniform slot (3..16), 'c2' (2) or 'id' (-2); soff = byte offset or
// the constant sampler ID.
static int bindless_sampler(int stage, int sreg, int soff, int *sid) {
    if (sreg == -2) { *sid = soff; return 0; }
    if (sreg >= 3 && sreg <= 16) {
        uint32_t w = 0;
        if (ubo_handle(stage, sreg - 3, soff, &w) != 0) return -1;
        *sid = (int)(w & 0xfffff) + g_st.samp_base;
        return 0;
    }
    if (sreg == 2 && soff >= DMSH_C2_SAMP) {
        unsigned idx = (unsigned)(soff - DMSH_C2_SAMP) / 8;
        if (idx < 128) { *sid = (int)(g_st.samp[stage][idx] & 0xfffff) + g_st.samp_base; return 0; }
    }
    return -1;
}

static int add_write(VkWriteDescriptorSet *w, VkDescriptorBufferInfo *bi, VkDescriptorImageInfo *ii, VkBufferView *bv,
                     int *nw, int write_cap, size_t info_cap, VkDescriptorSet set, const nvn_desc_bind *d,
                     size_t *buffer_info_at, size_t *image_info_at,
                     VkBuffer buf, VkDeviceSize range, VkImageView img, VkSampler samp, VkBufferView tbuf,
                     const VkDescriptorImageInfo *image_array) {
    if (*nw < 0 || *nw >= write_cap) { LOG("[RED] [nvn/vk] descriptor write storage exhausted (%d entries)", write_cap); return -1; }
    int wi = *nw;
    VkWriteDescriptorSet *x = &w[wi];
    memset(x, 0, sizeof *x);
    x->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    x->dstSet = set; x->dstBinding = d->binding; x->descriptorCount = d->count ? d->count : 1;
    x->descriptorType = d->type;
    if (tbuf) {
        bv[wi] = tbuf;
        x->pTexelBufferView = &bv[wi];
    } else if (d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
        if (*buffer_info_at > info_cap || x->descriptorCount > info_cap - *buffer_info_at) return -1;
        for (uint32_t i = 0; i < x->descriptorCount; i++) { bi[*buffer_info_at + i].buffer = buf; bi[*buffer_info_at + i].offset = 0; bi[*buffer_info_at + i].range = range; }
        x->pBufferInfo = &bi[*buffer_info_at];
        *buffer_info_at += x->descriptorCount;
    } else {
        if (*image_info_at > info_cap || x->descriptorCount > info_cap - *image_info_at) return -1;
        if (image_array) x->pImageInfo = image_array;
        else {
            for (uint32_t i = 0; i < x->descriptorCount; i++) {
                ii[*image_info_at + i].sampler = samp; ii[*image_info_at + i].imageView = img;
                ii[*image_info_at + i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
            x->pImageInfo = &ii[*image_info_at];
        }
        *image_info_at += x->descriptorCount;
    }
    (*nw)++;
    return 0;
}

// Resolve one element of a fixed direct NVN texture/sampler array. NVN's
// BindTextures / BindSeparateTextures and BindSamplers commands bind a range
// beginning at `reg`, so element k is the resource at reg + k. Bindless UBO,
// c2-table and pool arrays have different index sources and remain separate
// reflection kinds; do not infer their semantics from this rule.
static int nvn_direct_array_image(int stage, const nvn_refl_bind *r, VkDescriptorType type,
                                  uint32_t elem, VkDescriptorImageInfo *out, tex_ent **texture_out) {
    memset(out, 0, sizeof *out);
    if (texture_out) *texture_out = NULL;
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!strcmp(r->kind, "tex")) {
        int slot = r->reg + (int)elem;
        if (slot < 0 || slot >= 256) return -1;
        uint64_t h = g_st.tex[stage][slot];
        if (!h) return 0;
        int tid = (int)(h & 0xfffff) + g_st.tex_base;
        int sid = (int)(h >> 20) + (slot >= NVN_SEPTEX_SLOT0 ? 0 : g_st.samp_base);
        if ((h >> 20) == 0) sid = 0;
        tex_ent *te = texid_image(tid, &out->imageView);
        if (!te) {
            LOG("[RED] [nvn/vk] array texture id %d (slot %d) is not registered", tid, slot);
            return -1;
        }
        if (texture_out) *texture_out = te;
        if (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) out->sampler = sampler_get(sid);
        else if (type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) return -1;
        return 0;
    }
    if (!strcmp(r->kind, "samp_tex")) {
        int slot = r->reg + (int)elem;
        if (slot < 0 || slot >= 256 || type != VK_DESCRIPTOR_TYPE_SAMPLER) return -1;
        uint64_t h = g_st.tex[stage][slot];
        if (h) {
            int sid = (int)(h >> 20) + (slot >= NVN_SEPTEX_SLOT0 ? 0 : g_st.samp_base);
            if ((h >> 20) == 0) sid = 0;
            out->sampler = sampler_get(sid);
        }
        return 0;
    }
    if (!strcmp(r->kind, "samp")) {
        int slot = r->reg + (int)elem;
        if (slot < 0 || slot >= 128 || type != VK_DESCRIPTOR_TYPE_SAMPLER) return -1;
        uint64_t h = g_st.samp[stage][slot];
        out->sampler = sampler_get((int)(h & 0xfffff) + g_st.samp_base);
        return 0;
    }
    if (!strcmp(r->kind, "img")) {
        int slot = r->reg + (int)elem;
        if (slot < 0 || slot >= 32 || type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) return -1;
        uint64_t h = g_st.img[stage][slot];
        if (!h) return -1;
        img_id_ent *im = imgid_find((int)(h & 0xfffff) + g_st.tex_base);
        if (!im) {
            LOG("[RED] [nvn/vk] storage image id %u (slot %d) is not registered", (unsigned)(h & 0xfffff), slot);
            return -1;
        }
        tex_ent *te = tex_ensure(&im->t);
        if (!te) return -1;
        if (tex_stale(te, &im->t)) upload_guest_texture(&im->t, te);
        out->imageView = te->view;
        if (texture_out) *texture_out = te;
        return 0;
    }
    return -1;
}

// Build and submit one draw. Indexed draws take their indices from the guest.
static int nvkd_indirect_count_live(uint64_t count_addr, uint32_t index, int *live) {
    if (nvn_range_gpu_written(count_addr, 4)) sub_flush();   // a count the GPU wrote this submission
    uint32_t count = 0;
    if (!count_addr || !nvn_host_readable(count_addr, sizeof count)) {
        LOG("[RED] [nvn/vk] indirect-count value at %#llx is not host-readable",
            (unsigned long long)count_addr);
        return -1;
    }
    memcpy(&count, (const void *)(uintptr_t)count_addr, sizeof count);
    *live = index < count;
    return 0;
}

static int nvkd_indirect_args(uint64_t addr, void *out, size_t size) {
    if (nvn_range_gpu_written(addr, size)) sub_flush();   // arguments the GPU may have written this submission
    if (!addr || !nvn_host_readable(addr, size)) {
        LOG("[RED] [nvn/vk] indirect arguments at %#llx (%zu bytes) are not host-readable",
            (unsigned long long)addr, size);
        return -1;
    }
    memcpy(out, (const void *)(uintptr_t)addr, size);
    return 0;
}

// The translator's support buffer (route B): a 64 KB zero block whose render
// scale is 1, at the same layout the Metal host fills (Ryujinx's SupportBuffer:
// alpha_test u32, is_bgra u32[8], viewport_inverse f4, viewport_size f4,
// frag_scale_count i32, then render_scale float4[73] from byte 192). A shader
// that names it gets it at its own binding, which shifts every other binding
// by one when the JSON reflection omits the support member.
static VkBuffer g_support_buf; static VkDeviceMemory g_support_mem; static int g_support_ready;
static VkBuffer nvn_support_buffer(void) {
    if (g_support_ready) return g_support_buf;
    g_support_ready = 1;
    void *map = NULL;
    if (nx_vk_create_buffer(&g_vk, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &g_support_buf, &g_support_mem, &map) != 0 || !map) {
        LOG("[RED] [nvn/vk] cannot create the shader support buffer");
        g_support_buf = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    for (int i = 0; i < 73; i++) *(float *)((uint8_t *)map + 192 + 16 * i) = 1.0f;
    return g_support_buf;
}

static void nvkd_draw_common(const prog_t *const stg[6], int indexed, const p_draw *da, const p_draw_idx *di) {
    p_draw resolved_da;
    p_draw_idx resolved_di;
    if (!indexed && (da->ind || da->mdi_cnt)) {
        resolved_da = *da;
        if (da->mdi_cnt) {
            int live = 0;
            if (da->mdi_i < 0 || nvkd_indirect_count_live(da->mdi_cnt, (uint32_t)da->mdi_i, &live) != 0 || !live) return;
        }
        struct { uint32_t count, instances, first, first_instance; } args;
        if (nvkd_indirect_args(da->ind, &args, sizeof args) != 0 || args.count > INT32_MAX ||
            args.instances > INT32_MAX || args.first > INT32_MAX || args.first_instance > INT32_MAX) return;
        if (!args.count || !args.instances) return;
        resolved_da.first = (int32_t)args.first;
        resolved_da.count = (int32_t)args.count;
        resolved_da.instances = (int32_t)args.instances;
        resolved_da.base_instance = (int32_t)args.first_instance;
        resolved_da.ind = resolved_da.mdi_cnt = 0;
        da = &resolved_da;
    } else if (indexed && (di->ind || di->mdi_cnt)) {
        resolved_di = *di;
        if (di->mdi_cnt) {
            int live = 0;
            if (di->mdi_i < 0 || nvkd_indirect_count_live(di->mdi_cnt, (uint32_t)di->mdi_i, &live) != 0 || !live) return;
        }
        struct { uint32_t count, instances, first_index; int32_t vertex_offset; uint32_t first_instance; } args;
        if (nvkd_indirect_args(di->ind, &args, sizeof args) != 0 || args.count > INT32_MAX ||
            args.instances > INT32_MAX || args.first_index > INT32_MAX || args.first_instance > INT32_MAX ||
            (di->itype != NVN_INDEX_TYPE_UNSIGNED_BYTE && di->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT && di->itype != NVN_INDEX_TYPE_UNSIGNED_INT)) return;
        if (!args.count || !args.instances) return;
        uint64_t index_size = di->itype == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 1;
        if (args.first_index > (UINT64_MAX - di->ibuf) / index_size) return;
        resolved_di.count = (int32_t)args.count;
        resolved_di.instances = (int32_t)args.instances;
        resolved_di.base_vertex = args.vertex_offset;
        resolved_di.base_instance = (int32_t)args.first_instance;
        resolved_di.ibuf = di->ibuf + (uint64_t)args.first_index * index_size;
        resolved_di.ind = resolved_di.mdi_cnt = 0;
        di = &resolved_di;
    }
    uint64_t _tp = nx_monotonic_ns();
    prog_vk *pv = pvk_get_stages(stg, 0x1fu);   // the draw stages, not a bound compute program
    g_prof[PROF_STAGES] += nx_monotonic_ns() - _tp;
    if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] draw: program %llu pv %p rt %d count %d", pv ? (unsigned long long)pv->serial : 0ull, (void *)pv, g_rt.n, indexed ? di->count : da->count);
    if (!pv) return;
    if ((g_rt.n <= 0 || !g_rt.color[0]) && !g_rt.depth) { warn_once(&g_draw_warned, "NVN draw without a colour or depth target"); return; }
    int ncolor = g_rt.n > 8 ? 8 : g_rt.n;
    if (ncolor > 0 && !g_rt.color[0]) ncolor = 0;
    VkFormat color[8]; tex_ent *ce[8] = {0};
    uint32_t dww = 1, dhh = 1;
    for (int i = 0; i < ncolor; i++) {
        if (!g_rt.color[i]) { ncolor = i; break; }
        ce[i] = tex_ensure(g_rt.color[i]);
        if (!ce[i]) return;
        if (!ce[i]->can_attach) {
            static int warned;
            if (!warned++) LOG("[RED] [nvn/vk] colour target format %d cannot be a render target on this device; the draw is dropped",
                               g_rt.color[i]->d.format);
            return;
        }
        if (i == 0) { dww = g_rt.color[i]->d.width ? g_rt.color[i]->d.width : 1; dhh = g_rt.color[i]->d.height ? g_rt.color[i]->d.height : 1; }
        color[i] = ce[i]->fmt;
        if (g_rt.has_cview[i]) cached_view(ce[i], &g_rt.cview[i], &color[i], 0);
    }
    if (!ncolor && g_rt.depth) { dww = g_rt.depth->d.width ? g_rt.depth->d.width : 1; dhh = g_rt.depth->d.height ? g_rt.depth->d.height : 1; }
    if (!ncolor && !g_rt.depth) return;
    uint32_t samples = ncolor ? ce[0]->samples : 1;
    tex_ent *de = NULL; VkFormat depth_fmt = VK_FORMAT_UNDEFINED; VkImageView depth_view = VK_NULL_HANDLE;
    if (g_rt.depth) { de = tex_ensure(g_rt.depth); if (de && !de->can_attach) {
        static int warned;
        if (!warned++) LOG("[RED] [nvn/vk] depth target format %d cannot be a render target on this device; the draw is dropped",
                           g_rt.depth->d.format);
        return;
    } else if (de) { depth_fmt = de->fmt; depth_view = g_rt.has_dview ? cached_view(de, &g_rt.dview, &depth_fmt, 0) : de->view; if (!ncolor) samples = de->samples; } }
    VkRenderPass rp = rp_get(ncolor, color, depth_fmt, samples);
    if (!rp) return;
    int prim = da ? da->prim : di->prim;
    int expand_prim = prim == NVN_DRAW_PRIMITIVE_QUADS || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP ||
                      prim == NVN_DRAW_PRIMITIVE_POLYGON || prim == NVN_DRAW_PRIMITIVE_LINE_LOOP;
    g_draw_topo = expand_prim ? (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                              : vktopo(prim);
    uint64_t _tpipe = nx_monotonic_ns();
    VkPipeline pipe = pvk_pipeline(pv, rp, ncolor, color, depth_fmt, samples);
    g_prof[PROF_PIPELINE] += nx_monotonic_ns() - _tpipe;
    if (!pipe) return;
    if (nx_env_bool("NX_VK_DEBUG", 0)) {
        LOG("[nvn/vk] draw: pipe %p rp %p ncolor %d fmt0 %d depth %d vbuf0 %#llx+%llu attr0 fmt %u off %u stream %u stride %u target serial %llu %ux%u depth serial %llu %ux%u",
            (void *)pipe, (void *)rp, ncolor, (int)color[0], (int)depth_fmt,
            (unsigned long long)g_st.vbuf[0].address, (unsigned long long)g_st.vbuf[0].size,
            g_st.attr[0].fmt, g_st.attr[0].offset, g_st.attr[0].stream, g_st.stream[0].stride,
            ncolor ? (unsigned long long)g_rt.color[0]->serial : 0ull, ncolor ? g_rt.color[0]->d.width : 0u, ncolor ? g_rt.color[0]->d.height : 0u,
            g_rt.depth ? (unsigned long long)g_rt.depth->serial : 0ull, g_rt.depth ? g_rt.depth->d.width : 0u, g_rt.depth ? g_rt.depth->d.height : 0u);
        const float *fv = (const float *)(uintptr_t)g_st.vbuf[0].address;
        if (fv) LOG("[nvn/vk] draw: first vertices %g %g, %g %g, %g %g", fv[0], fv[1], fv[2], fv[3], fv[4], fv[5]);
    }
    VkCommandBuffer cb = rp_cb();
    if (!cb) return;

    // Every attachment the pass reads or writes is in GENERAL.
    for (int i = 0; i < ncolor; i++) if (ce[i]->layout != VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier b = {0};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = ce[i]->layout; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = ce[i]->img; b.subresourceRange = tex_subresources(ce[i]);
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        ce[i]->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (de && de->layout != VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier b = {0};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = de->layout; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = de->img; b.subresourceRange = tex_subresources(de);
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        de->layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkImageView views[9]; uint32_t nv = 0;
    for (int i = 0; i < ncolor; i++) views[nv++] = g_rt.has_cview[i] ? cached_view(ce[i], &g_rt.cview[i], NULL, 0) : ce[i]->view;
    if (de) views[nv++] = depth_view;
    int fresh = !g_rp.on || g_rp.cb != cb || g_rp.rp != rp || g_rp.w != dww || g_rp.h != dhh || g_rp.nv != nv;
    for (uint32_t i = 0; i < nv && !fresh; i++) if (g_rp.views[i] != views[i]) fresh = 1;

    // Descriptors: one set for all stages, built from each stage's reflection.
    uint32_t total = 0, write_cap = 0;
    for (int st = 0; st < 5; st++) if (pv->have[st])
        for (int i = 0; i < pv->sh[st].ndesc; i++) {
            total += pv->sh[st].desc[i].count ? pv->sh[st].desc[i].count : 1;
            write_cap++;
        }
    VkDescriptorPool pool = batch_desc_pool();
    if (!pool) { rp_done(cb); return; }
    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &pv->setl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) != VK_SUCCESS) { rp_done(cb); return; }

    // Grow-only scratch: one replay thread, and a calloc/free pair per draw
    // (seven of them) was part of the frame's CPU.
    static void *scr_wr, *scr_bvs, *scr_hb, *scr_tbufs, *scr_bi, *scr_ii, *scr_images;
    static size_t cap_wr, cap_bvs, cap_hb, cap_tbufs, cap_bi, cap_ii, cap_images;
    size_t write_slots = write_cap ? write_cap : 1;
    size_t info_slots = total ? total : 1;
    #define SCRATCH(var, cap, type, n) ((cap) >= (size_t)(n) ? (type *)(var) : ((void)((var) = realloc((var), ((cap) = (n)) * sizeof(type))), (type *)(var)))
    VkWriteDescriptorSet *wr = SCRATCH(scr_wr, cap_wr, VkWriteDescriptorSet, write_slots);
    VkBufferView *bvs = SCRATCH(scr_bvs, cap_bvs, VkBufferView, write_slots);
    hbuf *hb = SCRATCH(scr_hb, cap_hb, hbuf, write_slots);
    VkBufferView *tbufs = SCRATCH(scr_tbufs, cap_tbufs, VkBufferView, write_slots);
    VkDescriptorBufferInfo *bi = SCRATCH(scr_bi, cap_bi, VkDescriptorBufferInfo, info_slots);
    VkDescriptorImageInfo *ii = SCRATCH(scr_ii, cap_ii, VkDescriptorImageInfo, info_slots);
    nvn_image_use *image_uses = SCRATCH(scr_images, cap_images, nvn_image_use, info_slots);
    size_t nimage_uses = 0;
    if (!wr || !bvs || !hb || !tbufs || !bi || !ii || !image_uses) {
        rp_done(cb); return;
    }
    memset(wr, 0, write_slots * sizeof *wr);
    memset(hb, 0, write_slots * sizeof *hb);
    memset(tbufs, 0, write_slots * sizeof *tbufs);
    g_sub_draws++;
    { extern uint64_t g_last_window_draw; if (ncolor && g_rt.color[0] && g_rt.color[0]->serial >= 2 && g_rt.color[0]->serial <= 4) g_last_window_draw = g_rt.color[0]->serial; }
    {   // NX_VK_TEXDUMP=<WxH>:<dir>: read the pass of that size when it ends
        const char *ds = nx_env("NX_VK_TEXDUMP");
        if (ds && *ds && ds[0] >= '0' && ds[0] <= '9') {
            static tex_t keep; static int have; static uint64_t last;
            static uint32_t dw, dh;
            if (!dw) sscanf(ds, "%ux%u", &dw, &dh);
            uint32_t cw = (ncolor && g_rt.color[0]) ? g_rt.color[0]->d.width : 0;
            uint32_t ch = (ncolor && g_rt.color[0]) ? g_rt.color[0]->d.height : 0;
            uint64_t cur = (ncolor && g_rt.color[0]) ? g_rt.color[0]->serial : 0;
            const char *at = nx_env("NX_VK_DRAWAT");
            int want = (at && (uint64_t)atoll(at) == (uint64_t)g_present_count) || (!at && g_present_count >= 1500 && g_present_count <= 1620);
            if (want && have && cur != last) {
                static int seen; const char *sk = nx_env("NX_VK_TEXSKIP"); int skip = sk ? atoi(sk) : 0;
                if (seen++ >= skip) tex_dump_now(&keep, (uint32_t)keep.serial);
            }
            if (want && cw == dw && ch == dh) {
                static int nlog; if (nlog++ < 3) LOG("[nvn/vk] texdump: pass %llx %ux%u (want %ux%u), present %llu", (unsigned long long)cur, cw, ch, dw, dh, (unsigned long long)g_present_count);
                keep = *g_rt.color[0]; last = cur; have = 1;
            }
        }
    }
    {
        const char *at = nx_env("NX_VK_DRAWAT");
        if (at && (uint64_t)atoll(at) == (uint64_t)g_present_count)
            LOG("[nvn/vk] desc prog %llu: have %d%d%d%d%d keys %s %s ndesc %d %d %d %d %d", (unsigned long long)pv->serial,
                pv->have[0], pv->have[1], pv->have[2], pv->have[3], pv->have[4],
                pv->have[0] ? pv->sh[0].key : "-", pv->have[1] ? pv->sh[1].key : "-",
                pv->sh[0].ndesc, pv->sh[1].ndesc, pv->sh[2].ndesc, pv->sh[3].ndesc, pv->sh[4].ndesc);
        if (at && (uint64_t)atoll(at) == (uint64_t)g_present_count)
            LOG("[nvn/vk] draw #%d prog %llu color0 %llx %ux%u fmt %d depth %llx %ux%u verts %d inst %d indexed %d",
                g_sub_draws, (unsigned long long)pv->serial,
                (unsigned long long)(g_rt.color[0] ? g_rt.color[0]->serial : 0), dww, dhh, (int)color[0],
                (unsigned long long)(g_rt.depth ? g_rt.depth->serial : 0),
                g_rt.depth ? g_rt.depth->d.width : 0, g_rt.depth ? g_rt.depth->d.height : 0,
                indexed ? di->count : da->count, indexed ? di->instances : da->instances, indexed);
        if (at && (uint64_t)atoll(at) == (uint64_t)g_present_count)
            LOG("[nvn/vk] samples prog %llu color0 %d depth %d", (unsigned long long)pv->serial,
                ncolor ? (int)ce[0]->samples : 0, de ? (int)de->samples : 0);
    }
    int ntbuf = 0;
    int nw = 0, nhb = 0; size_t buffer_info_at = 0, image_info_at = 0;
    int ok = 1;
    uint64_t _td = nx_monotonic_ns();
    for (int st = 0; st < 5 && ok; st++) if (pv->have[st]) {
        nvn_shader *sh = &pv->sh[st];
        for (int k = 0; k < sh->ndesc && ok; k++) {
            nvn_desc_bind *d = &sh->desc[k];
            const nvn_refl_bind *r = d->refl >= 0 ? &sh->rb[d->refl] : NULL;
            { const char *at = nx_env("NX_VK_DRAWAT");
              if (at && (uint64_t)atoll(at) == (uint64_t)g_present_count && (pv->serial == 264 || pv->serial == 37))
                  LOG("[nvn/vk] bind prog %llu st%d k%d binding %u name %s kind %s reg %d refl %d count %u type %d",
                      (unsigned long long)pv->serial, st, k, d->binding, d->name, r ? r->kind : "-", r ? r->reg : -99, d->refl, d->count, (int)d->type); }
            VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize range = VK_WHOLE_SIZE; VkImageView iv = VK_NULL_HANDLE; VkSampler sm = VK_NULL_HANDLE;
            int support = r ? !strcmp(r->kind, DMSH_KIND_SUPPORT) : !strcmp(d->name, "support_buffer");
            if (!r && !support) { LOG("[RED] [nvn/vk] shader %s binding %u (%s) has no reflection entry", sh->key, d->binding, d->name); ok = 0; break; }
            if (support) {
                if (d->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d->count > 1) {
                    LOG("[RED] [nvn/vk] shader %s support buffer binding %u is not a single uniform buffer", sh->key, d->binding); ok = 0; break;
                }
                buf = nvn_support_buffer(); range = 65536;
                if (!buf || add_write(wr, bi, ii, bvs, &nw, (int)write_cap, total, set, d, &buffer_info_at, &image_info_at,
                                      buf, range, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, NULL) != 0) { ok = 0; break; }
                continue;
            }
            if (d->count > 1) {
                if (nw >= (int)write_cap) { LOG("[RED] [nvn/vk] descriptor write storage exhausted (%u entries)", write_cap); ok = 0; break; }
                VkDescriptorImageInfo *array = &ii[image_info_at];
                for (uint32_t e = 0; e < d->count; e++) {
                    tex_ent *te = NULL;
                    if (nvn_direct_array_image(st, r, d->type, e, &array[e], &te) != 0) {
                        LOG("[RED] [nvn/vk] shader %s binding %u array kind '%s' is not a fixed direct texture/sampler array", sh->key, d->binding, r->kind);
                        ok = 0; break;
                    }
                    if (te && nvn_image_use_add(image_uses, &nimage_uses, total, te,
                                                d->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) != 0) { ok = 0; break; }
                }
                if (!ok) break;
                if (add_write(wr, bi, ii, bvs, &nw, (int)write_cap, total, set, d, &buffer_info_at, &image_info_at, buf, range, iv, sm, VK_NULL_HANDLE, array) != 0) { ok = 0; break; }
                continue;
            }
            const char *kind = r->kind;
            if (!strcmp(kind, "c1")) {
                size_t n = sh->c1_len ? sh->c1_len : 16;
                hbuf *b = &hb[nhb];
                uint8_t zero[16] = {0};
                if (hbuf_make(b, sh->c1_len ? sh->c1 : zero, n) != 0) { ok = 0; break; }
                nhb++; buf = b->buf;
            } else if (!strcmp(kind, "drvcb")) {
                hbuf *b = &hb[nhb];
                uint8_t *tmp = malloc(DMSH_DRV_SIZE);
                if (!tmp) { ok = 0; break; }
                drv_fill(tmp, r->bank, st, sh->key);
                int hr = hbuf_make(b, tmp, DMSH_DRV_SIZE); free(tmp);
                if (hr != 0) { ok = 0; break; }
                nhb++; buf = b->buf;
            } else if (!strcmp(kind, "cbuf")) {
                { const char *at = nx_env("NX_VK_DRAWAT");
                  if (at && (uint64_t)atoll(at) == (uint64_t)g_present_count && (pv->serial == 14061 || pv->serial == 28158) && r->reg >= 0 && r->reg < 16 && g_st.ubo[st][r->reg].addr) {
                      const float *g = (const float *)(uintptr_t)g_st.ubo[st][r->reg].addr;
                      LOG("[nvn/vk] cbuf tail prog %llu st%d slot%d: f32..39 %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f | f40..47 %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f | f48..55 %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
                          (unsigned long long)pv->serial, st, r->reg, g[32],g[33],g[34],g[35],g[36],g[37],g[38],g[39],
                          g[40],g[41],g[42],g[43],g[44],g[45],g[46],g[47], g[48],g[49],g[50],g[51],g[52],g[53],g[54],g[55]);
                  } }
                { const char *at = nx_env("NX_VK_DRAWAT");
                  if (at && (uint64_t)atoll(at) == (uint64_t)g_present_count && r->reg >= 0 && r->reg < 16 && g_st.ubo[st][r->reg].addr) {
                      const float *f = (const float *)(uintptr_t)g_st.ubo[st][r->reg].addr;
                      LOG("[nvn/vk] cbuf prog %llu st%d slot %d size %u: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                          (unsigned long long)pv->serial, st, r->reg, (unsigned)g_st.ubo[st][r->reg].size, f[0],f[1],f[2],f[3], f[4],f[5],f[6],f[7], f[8],f[9],f[10],f[11], f[12],f[13],f[14],f[15]);
                  } }
                // dmsh_reflect's reg is the NVN uniform slot (c[3] is slot 0),
                // the index BindUniformBuffer takes and g_st.ubo stores.
                int slot = r->reg;
                if ((pv->serial == 264 || pv->serial == 37) && nx_env("NX_VK_DRAWAT") && (uint64_t)atoll(nx_env("NX_VK_DRAWAT")) == (uint64_t)g_present_count)
                    LOG("[nvn/vk] cbuf prog %llu st%d slot %d is %s", (unsigned long long)pv->serial, st, slot, (slot >= 0 && slot < 16 && g_st.ubo[st][slot].addr) ? "bound" : "NULL");
                if (slot < 0 || slot >= 16 || !g_st.ubo[st][slot].addr) {
                    uint8_t zero[16] = {0}; hbuf *b = &hb[nhb]; if (hbuf_make(b, zero, 16) != 0) { ok = 0; break; } nhb++; buf = b->buf;
                } else {
                    size_t n = (size_t)g_st.ubo[st][slot].size;
                    if (n < 16) n = 16;
                    if (nx_env_bool("NX_VK_DEBUG", 0)) {
                        const float *f = (const float *)(uintptr_t)g_st.ubo[st][slot].addr;
                        LOG("[nvn/vk] cbuf stage %d slot %d: %zu bytes at %#llx, first %g %g %g %g", st, slot, n,
                            (unsigned long long)g_st.ubo[st][slot].addr, f[0], f[1], f[2], f[3]);
                    }
                    hbuf *b = &hb[nhb];
                    size_t padded = r->fixed > 0 && (size_t)r->fixed > n ? (size_t)r->fixed : n;
                    if (hbuf_make(b, NULL, padded) != 0) { ok = 0; break; }
                    memcpy(b->map, (const void *)(uintptr_t)g_st.ubo[st][slot].addr, n);
                    nhb++; buf = b->buf;
                }
            } else if (!strcmp(kind, "sbuf") || !strcmp(kind, "sbuf_push")) {
                int slot = r->reg;
                if (slot < 0 || slot >= 16 || !g_st.ssbo[st][slot].addr) {
                    uint8_t zero[16] = {0}; hbuf *b = &hb[nhb]; if (hbuf_make(b, zero, 16) != 0) { ok = 0; break; } nhb++; buf = b->buf;
                } else {
                    size_t n = (size_t)g_st.ssbo[st][slot].size;
                    hbuf *b = &hb[nhb];
                    if (hbuf_make(b, (const void *)(uintptr_t)g_st.ssbo[st][slot].addr, n) != 0) { ok = 0; break; }
                    nhb++; buf = b->buf;
                }
            } else if (!strcmp(kind, "cbsize")) {
                uint32_t sz[DMSH_CBSIZE_SLOTS] = {0};
                for (int q = 0; q < DMSH_CBSIZE_SLOTS; q++) sz[q] = (uint32_t)g_st.ubo[st][q].size;
                hbuf *b = &hb[nhb];
                if (hbuf_make(b, sz, sizeof sz) != 0) { ok = 0; break; }
                nhb++; buf = b->buf;
            } else if (!strcmp(kind, "sblen")) {
                uint32_t sz[DMSH_SBLEN_SLOTS * 2] = {0};
                for (int q = 0; q < DMSH_SBLEN_SLOTS && q < 16; q++) { sz[2*q] = (uint32_t)(g_st.ssbo[st][q].size / 4); sz[2*q+1] = sz[2*q]; }
                hbuf *b = &hb[nhb];
                if (hbuf_make(b, sz, sizeof sz) != 0) { ok = 0; break; }
                nhb++; buf = b->buf;
            } else if (!strcmp(kind, "tbias") || !strcmp(kind, "support") || !strcmp(kind, "gmem_map") || !strcmp(kind, "gmem_log") || !strcmp(kind, "border") || !strcmp(kind, "geometry")) {
                uint8_t zero[2048] = {0};
                hbuf *b = &hb[nhb];
                if (hbuf_make(b, zero, sizeof zero) != 0) { ok = 0; break; }
                nhb++; buf = b->buf;
            } else if (!strcmp(kind, "tbuf")) {
                if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] tbuf binding %u type %d reg %d", d->binding, (int)d->type, r->reg);
                uint64_t h = (r->reg >= 0 && r->reg < 256) ? g_st.tex[st][r->reg] : 0;
                if (!h) { ok = 0; break; }
                int tid = (int)(h & 0xfffff) + g_st.tex_base;
                tex_id_ent *ti = tid_find(tid);
                if (!ti) { LOG("[RED] [nvn/vk] buffer texture id %d is not registered", tid); ok = 0; break; }
                size_t bpp = 0; VkFormat tf = VK_FORMAT_UNDEFINED;
                if (copy_format(&ti->t, &tf, &bpp) != 0) { LOG("[RED] [nvn/vk] buffer texture format %d is not mapped", ti->t.d.format); ok = 0; break; }
                size_t bytes = (size_t)ti->t.d.width * bpp;
                hbuf *b = &hb[nhb];
                if (hbuf_make(b, ti->t.mem, bytes) != 0) { ok = 0; break; }
                nhb++; buf = b->buf;
                VkBufferViewCreateInfo bvi = {0};
                bvi.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
                bvi.buffer = b->buf; bvi.format = tf; bvi.offset = 0; bvi.range = bytes;
                VkBufferView tv = VK_NULL_HANDLE;
                if (vkCreateBufferView(g_vk.device, &bvi, NULL, &tv) != VK_SUCCESS) { ok = 0; break; }
                if (ntbuf < (int)write_cap) tbufs[ntbuf++] = tv;
                else { sub_free_bv(tv); ok = 0; break; }
            } else if (!strcmp(kind, "img_ubo")) {
                uint32_t h = 0;
                if (ubo_handle(st, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] bindless image handle at stage %d c[%d]+%#x is not in a bound uniform buffer", st, r->reg, r->off); ok = 0; break; }
                int tid = (int)(h & 0xfffff) + g_st.tex_base;
                img_id_ent *im = imgid_find(tid);
                if (!im) { LOG("[RED] [nvn/vk] bindless image id %d is not registered", tid); ok = 0; break; }
                tex_ent *te = tex_ensure(&im->t);
                if (!te) { ok = 0; break; }
                if (tex_stale(te, &im->t)) upload_guest_texture(&im->t, te);
                iv = te->view;
                if (nvn_image_use_add(image_uses, &nimage_uses, total, te,
                                      d->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) != 0) { ok = 0; break; }
            } else if (!strcmp(kind, "tbuf_ubo")) {
                uint32_t h = 0;
                if (ubo_handle(st, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] bindless buffer texture handle at stage %d c[%d]+%#x is not in a bound uniform buffer", st, r->reg, r->off); ok = 0; break; }
                int tid = (int)(h & 0xfffff) + g_st.tex_base;
                tex_id_ent *ti = tid_find(tid);
                if (!ti) { LOG("[RED] [nvn/vk] bindless buffer texture id %d is not registered", tid); ok = 0; break; }
                size_t bpp = 0; VkFormat tf = VK_FORMAT_UNDEFINED;
                if (copy_format(&ti->t, &tf, &bpp) != 0) { LOG("[RED] [nvn/vk] buffer texture format %d is not mapped", ti->t.d.format); ok = 0; break; }
                size_t bytes = (size_t)ti->t.d.width * bpp;
                hbuf *b = &hb[nhb];
                if (hbuf_make(b, ti->t.mem, bytes) != 0) { ok = 0; break; }
                nhb++; buf = b->buf;
                VkBufferViewCreateInfo bvi = {0};
                bvi.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
                bvi.buffer = b->buf; bvi.format = tf; bvi.offset = 0; bvi.range = bytes;
                VkBufferView tv = VK_NULL_HANDLE;
                if (vkCreateBufferView(g_vk.device, &bvi, NULL, &tv) != VK_SUCCESS) { ok = 0; break; }
                if (ntbuf < (int)write_cap) tbufs[ntbuf++] = tv; else { sub_free_bv(tv); ok = 0; break; }
            } else if (!strcmp(kind, "tex_ubo")) {
                uint32_t h = 0;
                if (ubo_handle(st, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] bindless texture handle at stage %d c[%d]+%#x is not in a bound uniform buffer", st, r->reg, r->off); ok = 0; break; }
                int tid = (int)(h & 0xfffff) + g_st.tex_base;
                int sid = 0;
                if (r->sreg != -1) { if (bindless_sampler(st, r->sreg, r->soff, &sid) != 0) { LOG("[RED] [nvn/vk] bindless sampler beside texture id %d is not readable", tid); ok = 0; break; } }
                else sid = (int)(h >> 20) + g_st.samp_base;
                tex_ent *te = texid_image(tid, &iv);
                if (!te) { LOG("[RED] [nvn/vk] bindless texture id %d is not registered", tid); ok = 0; break; }
                if (nvn_image_use_add(image_uses, &nimage_uses, total, te, 0) != 0) { ok = 0; break; }
                sm = sampler_get(sid);
                if (d->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) sm = VK_NULL_HANDLE;
            } else if (!strcmp(kind, "samp_ubo")) {
                uint32_t h = 0;
                if (ubo_handle(st, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] bindless sampler handle at stage %d c[%d]+%#x is not in a bound uniform buffer", st, r->reg, r->off); ok = 0; break; }
                int sid = r->raw_id ? (int)h : (int)(h & 0xfffff) + g_st.samp_base;
                sm = sampler_get(sid);
            } else if (!strcmp(kind, "tex") || !strcmp(kind, "samp_tex")) {
                uint64_t h = (r->reg >= 0 && r->reg < 256) ? g_st.tex[st][r->reg] : 0;
                if (!h) { iv = NULL; sm = NULL; }
                else {
                    int tid = (int)(h & 0xfffff) + g_st.tex_base;
                    int sid = (int)(h >> 20) + (r->reg >= NVN_SEPTEX_SLOT0 ? 0 : g_st.samp_base);
                    if ((h >> 20) == 0) sid = 0;
                    tex_ent *te = texid_image(tid, &iv);
                    if (!te) { LOG("[RED] [nvn/vk] texture id %d (slot %d) is not registered", tid, r->reg); ok = 0; break; }
                    { const char *at = nx_env("NX_VK_DRAWAT");
                      if (at && (uint64_t)atoll(at) == (uint64_t)g_present_count && ncolor && g_rt.color[0]) {
                          samp_par sp = {0}; int have_sp = 0;
                          for (int q = g_nsamp - 1; q >= 0; q--) if (g_samps[q].id == sid) { sp = g_samps[q].p; have_sp = 1; break; }
                          tex_id_ent *tg = tid_find(tid);
                          int hv = tg ? tg->has_view : 0;
                          LOG("[nvn/vk] graph: %llx -> %llx (reg %d st %d, %ux%u fmt %#x lv %u, view %d base %d nlv %d, samp %d bias %.3f lod %.2f..%.2f filt %d/%d)",
                              (unsigned long long)g_rt.color[0]->serial, (unsigned long long)te->serial, r->reg, st,
                              te->t.d.width, te->t.d.height, te->t.d.format, te->t.d.levels,
                              hv, hv && tg->v.set_levels ? tg->v.base_level : 0, hv && tg->v.set_levels ? tg->v.nlevels : 0, sid,
                              have_sp ? sp.lod_bias : 0.0f, have_sp ? sp.lod_min : 0.0f, have_sp ? sp.lod_max : 0.0f,
                              have_sp ? sp.magf : -1, have_sp ? sp.minf : -1);
                      } }
                    const char *tb = nx_env("NX_VK_TEXBIND");
                    if (tb && *tb && ncolor && g_rt.color[0] && g_rt.color[0]->serial == strtoull(tb, NULL, 0))
                        LOG("[nvn/vk] bind target %llx: tex id %d slot %d -> serial %llx %ux%u fmt %#x gpu_dirty %d", (unsigned long long)g_rt.color[0]->serial, tid, r->reg,
                            (unsigned long long)te->serial, te->t.d.width, te->t.d.height, te->t.d.format, te->gpu_dirty);
                    if (nvn_image_use_add(image_uses, &nimage_uses, total, te, 0) != 0) { ok = 0; break; }
                    if (nx_env_bool("NX_VK_DEBUG", 0)) {
                        tex_id_ent *ti = tid_find(tid);
                        LOG("[nvn/vk] tex id %d: serial %llu %ux%u fmt %d mem %p off %lld L.size %llu layer %llu dirty %d layout %d",
                            tid, (unsigned long long)ti->t.serial, ti->t.d.width, ti->t.d.height, ti->t.d.format,
                            (void *)ti->t.mem, (long long)ti->t.off, (unsigned long long)ti->t.L.size,
                            (unsigned long long)ti->t.L.layer_size, te->gpu_dirty, (int)te->layout);
                    }
                    if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] tex slot %d id %d sampler %d -> view %p", r->reg, tid, sid, (void *)iv);
                    sm = sampler_get(sid);
                    if (d->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) sm = VK_NULL_HANDLE;
                }
            } else if (!strcmp(kind, "samp")) {
                uint64_t h = (r->reg >= 0 && r->reg < 128) ? g_st.samp[st][r->reg] : 0;
                sm = sampler_get((int)(h & 0xfffff) + g_st.samp_base);
            } else if (!strcmp(kind, "img")) {
                uint64_t h = (r->reg >= 0 && r->reg < 32) ? g_st.img[st][r->reg] : 0;
                if (!h) { ok = 0; break; }
                img_id_ent *im = imgid_find((int)(h & 0xfffff) + g_st.tex_base);
                if (!im) { ok = 0; break; }
                tex_ent *te = tex_ensure(&im->t);
                if (!te) { ok = 0; break; }
                if (tex_stale(te, &im->t)) upload_guest_texture(&im->t, te);
                iv = te->view;
                if (nvn_image_use_add(image_uses, &nimage_uses, total, te,
                                      d->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) != 0) { ok = 0; break; }
            } else if (!strcmp(kind, "samp_id")) {
                sm = sampler_get(r->reg);
            } else {
                LOG("[RED] [nvn/vk] binding kind '%s' is not replayed yet (program %llu)", kind, (unsigned long long)pv->serial);
                ok = 0; break;
            }
            VkBufferView tv = (ntbuf && !strcmp(kind, "tbuf")) ? tbufs[ntbuf - 1] : VK_NULL_HANDLE;
            if (add_write(wr, bi, ii, bvs, &nw, (int)write_cap, total, set, d, &buffer_info_at, &image_info_at, buf, range, iv, sm, tv, NULL) != 0) { ok = 0; break; }
        }
    }
    if (ok && nw && nx_env_bool("NX_VK_DEBUG", 0)) {
        for (int q = 0; q < nw; q++)
            LOG("[nvn/vk] desc %d: set %p binding %u type %d count %u img %p buf %p tbuf %p", q, (void *)wr[q].dstSet, wr[q].dstBinding,
                (int)wr[q].descriptorType, wr[q].descriptorCount, (void *)(wr[q].pImageInfo ? wr[q].pImageInfo->imageView : NULL),
                (void *)(wr[q].pBufferInfo ? wr[q].pBufferInfo->buffer : NULL), (void *)wr[q].pTexelBufferView);
    }
    g_prof[PROF_DESC] += nx_monotonic_ns() - _td;
    uint64_t _tu = nx_monotonic_ns();
    if (ok && nw) vkUpdateDescriptorSets(g_vk.device, (uint32_t)nw, wr, 0, NULL);
    g_prof[PROF_UPD] += nx_monotonic_ns() - _tu;
    uint64_t _tc = nx_monotonic_ns();

    // Synchronize every sampled or storage image, even when its layout is
    // already GENERAL. Keep write ownership only for storage descriptors.
    if (ok) for (size_t i = 0; i < nimage_uses; i++)
        nvn_image_shader_barrier(cb, image_uses[i].texture, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);

    // Vertex and index buffers.
    hbuf vb[16]; int nvb = 0;
    int used_stream[16] = {0};
    for (int i = 0; i < pv->sh[0].nvin; i++) {
        int loc = pv->sh[0].vin[i].location;
        if (loc >= 0 && loc < 32 && g_st.attr[loc].fmt && g_st.attr[loc].stream < 16) used_stream[g_st.attr[loc].stream] = 1;
    }
    VkBuffer vbufs[16] = {0};
    if (ok) for (int s = 0; s < 16; s++) if (used_stream[s]) {
        if (!g_st.vbuf[s].address || !g_st.vbuf[s].size) { LOG("[RED] [nvn/vk] vertex stream %d is used but not bound", s); ok = 0; break; }
        if (hbuf_make(&vb[nvb], (const void *)(uintptr_t)g_st.vbuf[s].address, (size_t)g_st.vbuf[s].size) != 0) { ok = 0; break; }
        vbufs[s] = vb[nvb].buf; nvb++;
    }
    hbuf ib; int has_ib = 0;
    int draw_indexed = indexed || expand_prim;
    uint32_t draw_count = indexed ? (uint32_t)di->count : (uint32_t)da->count;
    int32_t draw_vertex_offset = indexed ? di->base_vertex : 0;
    VkIndexType draw_index_type = indexed && di->itype == NVN_INDEX_TYPE_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (ok && expand_prim) {
        int bad_count = indexed ? di->count < 0 : (da->count < 0 || da->first < 0);
        size_t count = (size_t)(indexed ? di->count : da->count);
        if (bad_count || count > SIZE_MAX / sizeof(uint32_t) || (indexed && (!di->ibuf || !count))) {
            LOG("[RED] [nvn/vk] invalid input for primitive expansion (primitive %d, count %zu)", prim, count);
            ok = 0;
        } else if (!count) {
            ok = 0;
        } else {
            uint32_t *source = malloc(count * sizeof *source), *expanded = NULL;
            if (!source) ok = 0;
            else {
                if (indexed) {
                    const uint8_t *raw = (const uint8_t *)(uintptr_t)di->ibuf;
                    size_t stride = di->itype == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 1;
                    if (di->itype != NVN_INDEX_TYPE_UNSIGNED_BYTE && di->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT && di->itype != NVN_INDEX_TYPE_UNSIGNED_INT) ok = 0;
                    else for (size_t i = 0; i < count; i++) {
                        if (stride == 4) memcpy(&source[i], raw + i * stride, sizeof(uint32_t));
                        else if (stride == 2) { uint16_t v; memcpy(&v, raw + i * stride, sizeof v); source[i] = v; }
                        else source[i] = raw[i];
                    }
                } else for (size_t i = 0; i < count; i++) source[i] = (uint32_t)da->first + (uint32_t)i;
                uint32_t restart = indexed ? g_st.restart.index : 0;
                if (indexed && g_st.restart.on) {
                    uint32_t mask = di->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE ? 0xffu : di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 0xffffu : UINT32_MAX;
                    restart &= mask;
                }
                if (ok && nvkd_expand_primitive(prim, source, count, indexed && g_st.restart.on, restart,
                                                &expanded, &draw_count) != 0) {
                    LOG("[RED] [nvn/vk] failed to expand primitive %d with %zu indices", prim, count);
                    ok = 0;
                }
                free(source);
                if (ok && draw_count) {
                    ok = hbuf_make(&ib, expanded, (size_t)draw_count * sizeof *expanded) == 0;
                    has_ib = ok;
                    draw_index_type = VK_INDEX_TYPE_UINT32;
                } else if (ok) ok = 0;
                free(expanded);
            }
        }
    } else if (ok && indexed) {
        uint64_t count = (uint64_t)di->count, isz = di->itype == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 1;
        if (!di->ibuf || !count || di->count < 0) { warn_once(&g_draw_warned, "NVN indexed draw without indices"); ok = 0; }
        else if (di->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) {
            // Vulkan has no core uint8 index type; widen to uint16.
            uint16_t *tmp = malloc((size_t)count * 2);
            if (!tmp) ok = 0;
            else {
                const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
                for (uint64_t i = 0; i < count; i++) tmp[i] = src[i];
                ok = hbuf_make(&ib, tmp, (size_t)count * 2) == 0;
                free(tmp);
                has_ib = ok;
            }
        } else if (hbuf_make(&ib, (const void *)(uintptr_t)di->ibuf, (size_t)(count * isz)) != 0) ok = 0;
        else has_ib = 1;
    }
    if (ok) {
        // A texture upload during the descriptor build may have closed the
        // pass: any open pass that is not this one closes now, and this one
        // begins with a framebuffer made here (a closed one is already on the
        // batch's free list).
        if (g_rp.on && (g_rp.cb != cb || g_rp.rp != rp || g_rp.w != dww || g_rp.h != dhh || g_rp.nv != nv)) nvkd_rp_close();
        for (uint32_t i = 0; i < nv && g_rp.on; i++) if (g_rp.views[i] != views[i]) { nvkd_rp_close(); break; }
        if (!g_rp.on) {
            uint64_t _tfb = nx_monotonic_ns();
            VkFramebufferCreateInfo fci = {0};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = rp; fci.attachmentCount = nv; fci.pAttachments = views;
            fci.width = dww; fci.height = dhh; fci.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(g_vk.device, &fci, NULL, &fb) != VK_SUCCESS) { g_prof[PROF_FB] += nx_monotonic_ns() - _tfb; rp_done(cb); return; }
            VkRenderPassBeginInfo rbi = {0};
            rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rbi.renderPass = rp; rbi.framebuffer = fb;
            rbi.renderArea = (VkRect2D){{0, 0}, {dww, dhh}};
            atomic_fetch_add_explicit(&g_stat_passes, 1, memory_order_relaxed);
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            g_rp.on = 1; g_rp.cb = cb; g_rp.rp = rp; g_rp.fb = fb;
            g_rp.w = dww; g_rp.h = dhh; g_rp.nv = nv;
            memcpy(g_rp.views, views, nv * sizeof *views);
            g_prof[PROF_FB] += nx_monotonic_ns() - _tfb;
        }
    }
    if (ok) {
        if (pipe != g_cur_pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); g_cur_pipe = pipe; }
        if (nw) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pv->pl, 0, 1, &set, 0, NULL);
        if (pv->sh[0].push_size || pv->sh[1].push_size || pv->sh[2].push_size || pv->sh[3].push_size || pv->sh[4].push_size)
            vkCmdPushConstants(cb, pv->pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 256, g_st.push[0]);
        float vx = 0, vy = 0, vw = (float)dww, vh = (float)dhh;
        if (g_st.has_vp) { vx = g_st.vp[0][0]; vy = g_st.vp[0][1]; vw = g_st.vp[0][2]; vh = g_st.vp[0][3]; }
        float vpy = g_origin_ll ? vy : vy + vh, vph = g_origin_ll ? vh : -vh;
        // SetViewportSwizzles: NEGATIVE_Y negates clip y, which is the same as
        // flipping the viewport about its centre (nvn_mtl.m). Other source
        // swizzles exchange or negate other axes and have no viewport-only
        // equivalent; they are reported and drawn unswizzled, as on Metal.
        { uint32_t sw; memcpy(&sw, g_st.vp_sw[0], sizeof sw);
          if (sw == 0x100) { vpy += vph; vph = -vph; }
          else if (sw) {
              static int warned_sw;
              if (!warned_sw++) LOG("[RED] [nvn/vk] viewport swizzle %d,%d,%d,%d is not a viewport flip; the draw is unswizzled",
                                    g_st.vp_sw[0][0], g_st.vp_sw[0][1] ^ 2, g_st.vp_sw[0][2] ^ 4, g_st.vp_sw[0][3] ^ 6);
          } }
        VkViewport vp = {vx, vpy, vw, vph,
                         g_st.has_vp ? g_st.dr[0] : 0.0f, g_st.has_vp ? g_st.dr[1] : 1.0f};
        VkRect2D sc = g_st.has_scissor ? (VkRect2D){{g_st.sc_x, g_st.sc_y}, {(uint32_t)g_st.sc_w, (uint32_t)g_st.sc_h}}
                                        : (VkRect2D){{0, 0}, {dww, dhh}};
        if (sc.extent.width > dww) sc.extent.width = dww;
        if (sc.extent.height > dhh) sc.extent.height = dhh;
        if (!g_cur_vp_valid || memcmp(&g_cur_vp, &vp, sizeof vp)) { vkCmdSetViewport(cb, 0, 1, &vp); g_cur_vp = vp; g_cur_vp_valid = 1; }
        if (!g_cur_sc_valid || g_cur_sc.offset.x != sc.offset.x || g_cur_sc.offset.y != sc.offset.y ||
            g_cur_sc.extent.width != sc.extent.width || g_cur_sc.extent.height != sc.extent.height) {
            vkCmdSetScissor(cb, 0, 1, &sc); g_cur_sc = sc; g_cur_sc_valid = 1;
        }
        if (g_st.ds.st && (!g_cur_stencil_valid || g_cur_stencil != g_st.st_ref[0])) {
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_st.st_ref[0]);
            g_cur_stencil = g_st.st_ref[0]; g_cur_stencil_valid = 1;
        }
        for (int s = 0; s < 16; s++) if (used_stream[s] && (!g_cur_vbuf_valid[s] || g_cur_vbuf[s] != vbufs[s])) {
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &vbufs[s], &off);
            g_cur_vbuf[s] = vbufs[s]; g_cur_vbuf_valid[s] = 1;
        }
        atomic_fetch_add_explicit(&g_stat_draws, 1, memory_order_relaxed);
        if (draw_indexed) {
            if (has_ib && (!g_cur_ibuf_valid || g_cur_ibuf != ib.buf || g_cur_itype != draw_index_type)) {
                vkCmdBindIndexBuffer(cb, ib.buf, 0, draw_index_type);
                g_cur_ibuf = ib.buf; g_cur_itype = draw_index_type; g_cur_ibuf_valid = 1;
            }
            uint32_t inst = indexed ? (di->instances ? (uint32_t)di->instances : 1)
                                    : (da->instances ? (uint32_t)da->instances : 1);
            if (expand_prim) {
                vkCmdDrawIndexed(cb, draw_count, inst, 0, draw_vertex_offset,
                                 (uint32_t)(indexed ? di->base_instance : da->base_instance));
            } else vkCmdDrawIndexed(cb, draw_count, inst, 0, draw_vertex_offset, (uint32_t)di->base_instance);
        } else {
            uint32_t inst = da->instances ? (uint32_t)da->instances : 1;
            vkCmdDraw(cb, draw_count, inst, (uint32_t)da->first, (uint32_t)da->base_instance);
        }
        for (int i = 0; i < ncolor; i++) { ce[i]->gpu_dirty = 1; ce[i]->wgen++; }
        if (de) de->wgen++;
        for (size_t i = 0; i < nimage_uses; i++)
            if (image_uses[i].writable) { image_uses[i].texture->gpu_dirty = 1; image_uses[i].texture->wgen++; }
    }
    rp_done(cb);
    for (int i = 0; i < ntbuf; i++) sub_free_bv(tbufs[i]);
    for (int i = 0; i < nhb; i++) sub_free_hbuf(&hb[i]);
    for (int i = 0; i < nvb; i++) sub_free_hbuf(&vb[i]);
    if (has_ib) sub_free_hbuf(&ib);
    g_prof[PROF_CMD2] += nx_monotonic_ns() - _tc;
}

// Large NVN indirect-count batches are CPU-expanded here because this replay
// backend submits each preceding operation synchronously. The count and
// argument reads therefore observe earlier compute writes in command order.
static void nvkd_multi_draw(const p_multi_draw *m) {
    // The count and records may be GPU-written by earlier commands in this
    // submission: wait for them only when the GPU owns those bytes. A list the
    // CPU built (the usual case) is read where it is, without a stall.
    if (m && m->max > 0 && m->cnt && m->ind) {
        uint64_t end = m->ind + (uint64_t)m->max * (uint64_t)(m->stride > 0 ? m->stride : 1);
        if (nvn_range_gpu_written(m->cnt, 4) || nvn_range_gpu_written(m->ind, end - m->ind)) sub_flush();
    }
    if (!m || m->max <= 0 || !m->ind || !m->cnt || m->stride < 0) {
        LOG("[RED] [nvn/vk] invalid NVN multi-draw range");
        return;
    }
    uint32_t count = 0;
    if (!nvn_host_readable(m->cnt, sizeof count)) {
        LOG("[RED] [nvn/vk] multi-draw count at %#llx is not host-readable", (unsigned long long)m->cnt);
        return;
    }
    memcpy(&count, (const void *)(uintptr_t)m->cnt, sizeof count);
    uint32_t n = count < (uint32_t)m->max ? count : (uint32_t)m->max;
    if (m->itype == -1) {
        const size_t arg_size = 4 * sizeof(uint32_t);
        if ((uint64_t)m->stride < arg_size) { LOG("[RED] [nvn/vk] arrays multi-draw stride %lld is smaller than %zu", (long long)m->stride, arg_size); return; }
        for (uint32_t i = 0; i < n; i++) {
            if ((uint64_t)i > (UINT64_MAX - m->ind) / (uint64_t)m->stride) break;
            struct { uint32_t count, instances, first, first_instance; } args;
            uint64_t addr = m->ind + (uint64_t)i * (uint64_t)m->stride;
            if (nvkd_indirect_args(addr, &args, sizeof args) != 0) break;
            if (args.count > INT32_MAX || args.instances > INT32_MAX || args.first > INT32_MAX || args.first_instance > INT32_MAX) {
                LOG("[RED] [nvn/vk] arrays multi-draw argument %u exceeds supported integer range", i); break;
            }
            if (!args.count || !args.instances) continue;
            p_draw d = {m->prim, (int32_t)args.first, (int32_t)args.count, (int32_t)args.instances, (int32_t)args.first_instance, 0, 0, 0, 0};
            if (g_bound_stage[0]) nvkd_draw_common(g_bound_stage, 0, &d, NULL);
        }
    } else {
        const size_t arg_size = 5 * sizeof(uint32_t);
        if (m->itype < NVN_INDEX_TYPE_UNSIGNED_BYTE || m->itype > NVN_INDEX_TYPE_UNSIGNED_INT ||
            (uint64_t)m->stride < arg_size || !m->ibuf) {
            LOG("[RED] [nvn/vk] invalid indexed multi-draw type, stride or index buffer"); return;
        }
        uint64_t index_size = m->itype == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : m->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 1;
        for (uint32_t i = 0; i < n; i++) {
            if ((uint64_t)i > (UINT64_MAX - m->ind) / (uint64_t)m->stride) break;
            struct { uint32_t count, instances, first_index; int32_t vertex_offset; uint32_t first_instance; } args;
            uint64_t addr = m->ind + (uint64_t)i * (uint64_t)m->stride;
            if (nvkd_indirect_args(addr, &args, sizeof args) != 0) break;
            if (args.count > INT32_MAX || args.instances > INT32_MAX || args.first_index > INT32_MAX || args.first_instance > INT32_MAX ||
                args.first_index > (UINT64_MAX - m->ibuf) / index_size) {
                LOG("[RED] [nvn/vk] indexed multi-draw argument %u exceeds supported range", i); break;
            }
            if (!args.count || !args.instances) continue;
            p_draw_idx d = {m->prim, m->itype, (int32_t)args.count, args.vertex_offset,
                            (int32_t)args.first_instance, (int32_t)args.instances,
                            m->ibuf + (uint64_t)args.first_index * index_size, 0, 0, 0, 0};
            if (g_bound_stage[0]) nvkd_draw_common(g_bound_stage, 1, NULL, &d);
        }
    }
}

// Compute dispatch: a compute pipeline from the program's stage 5, the same
// descriptor mapping as a draw, and a copy-back of the storage buffers the
// shader wrote so the guest sees the results.
typedef struct { hbuf *b; uint64_t addr; size_t size; } ssbo_out;
static void nvkd_dispatch(const p_dispatch *d) {
    nvkd_rp_close();   // these record outside a render pass
    g_sub_dispatches++;
    prog_vk *pv = pvk_get_stages(g_bound_stage, 0x20u);   // the compute stage alone
    if (!pv) return;
    if (!pv->have[5]) { warn_once(&g_dispatch_warned, "NVN compute dispatch replay"); return; }
    uint32_t groups[3];
    if (d->ind) {
        if (d->ind & 3) { LOG("[RED] [nvn/vk] indirect compute argument address %#llx is not 4-byte aligned", (unsigned long long)d->ind); return; }
        if (!nvn_host_readable(d->ind, sizeof groups)) {
            LOG("[RED] [nvn/vk] indirect compute arguments at %#llx are not host-readable", (unsigned long long)d->ind); return;
        }
        // Previous replay operations submit and wait before returning, and
        // storage-buffer writes are copied back before the next command. Read
        // the three guest counts here so this indirect dispatch stays in
        // command-stream order and preserves its group counts in driver state.
        memcpy(groups, (const void *)(uintptr_t)d->ind, sizeof groups);
    } else {
        if (d->x < 0 || d->y < 0 || d->z < 0) {
            LOG("[RED] [nvn/vk] negative compute group count %d %d %d", d->x, d->y, d->z); return;
        }
        groups[0] = (uint32_t)d->x; groups[1] = (uint32_t)d->y; groups[2] = (uint32_t)d->z;
    }
    for (int i = 0; i < 3; i++) if (groups[i] > g_vk.caps.props.limits.maxComputeWorkGroupCount[i]) {
        LOG("[RED] [nvn/vk] compute group count %u on axis %d exceeds device limit %u", groups[i], i,
            g_vk.caps.props.limits.maxComputeWorkGroupCount[i]); return;
    }
    g_st.dispatch[0] = groups[0]; g_st.dispatch[1] = groups[1]; g_st.dispatch[2] = groups[2];
    if (!groups[0] || !groups[1] || !groups[2]) return;
    if (!pv->cpipe) {
        VkComputePipelineCreateInfo ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = pv->mod[5];
        ci.stage.pName = pv->sh[5].entry;
        ci.layout = pv->pl;
        if (vkCreateComputePipelines(g_vk.device, g_vk.pipeline_cache, 1, &ci, NULL, &pv->cpipe) != VK_SUCCESS) {
            LOG("[RED] [nvn/vk] compute pipeline creation failed for program %llu", (unsigned long long)pv->serial);
            pv->cpipe = VK_NULL_HANDLE; return;
        }
    }
    nvn_shader *sh = &pv->sh[5];
    uint32_t desc_total = 0;
    for (int k = 0; k < sh->ndesc; k++) desc_total += sh->desc[k].count ? sh->desc[k].count : 1;
    size_t write_slots = sh->ndesc ? (size_t)sh->ndesc : 1;
    VkDescriptorPool pool = batch_desc_pool();
    if (!pool) return;
    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &pv->setl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) != VK_SUCCESS) return;
    VkCommandBuffer cb = rp_cb();
    if (!cb) return;
    VkWriteDescriptorSet *wr = calloc(write_slots, sizeof *wr);
    VkBufferView *bvs = calloc(write_slots, sizeof *bvs);
    hbuf *hb = calloc(write_slots, sizeof *hb);
    VkBufferView *tbufs = calloc(write_slots, sizeof *tbufs);
    VkDescriptorBufferInfo *bi = calloc(desc_total ? desc_total : 1, sizeof *bi);
    VkDescriptorImageInfo *ii = calloc(desc_total ? desc_total : 1, sizeof *ii);
    nvn_image_use *images = calloc(desc_total ? desc_total : 1, sizeof *images);
    if (!wr || !bvs || !hb || !tbufs || !bi || !ii || !images) {
        free(wr); free(bvs); free(hb); free(tbufs); free(bi); free(ii); free(images);
        rp_done(cb); return;
    }
    int ntbuf = 0;
    int nhb = 0, nw = 0, ok = 1; size_t buffer_info_at = 0, image_info_at = 0;
    ssbo_out outs[64]; int nout = 0;
    size_t nimg = 0;
    for (int k = 0; k < sh->ndesc && ok; k++) {
        nvn_desc_bind *db = &sh->desc[k];
        const nvn_refl_bind *r = db->refl >= 0 ? &sh->rb[db->refl] : NULL;
        VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize range = VK_WHOLE_SIZE; VkImageView iv = VK_NULL_HANDLE; VkSampler sm = VK_NULL_HANDLE;
        int support = r ? !strcmp(r->kind, DMSH_KIND_SUPPORT) : !strcmp(db->name, "support_buffer");
        if (!r && !support) { LOG("[RED] [nvn/vk] compute binding %u (%s) has no reflection entry", db->binding, db->name); ok = 0; break; }
        if (support) {
            if (db->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || db->count > 1) {
                LOG("[RED] [nvn/vk] compute support buffer binding %u is not a single uniform buffer", db->binding); ok = 0; break;
            }
            buf = nvn_support_buffer(); range = 65536;
            if (!buf || add_write(wr, bi, ii, bvs, &nw, (int)write_slots, desc_total, set, db, &buffer_info_at, &image_info_at,
                                  buf, range, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, NULL) != 0) { ok = 0; break; }
            continue;
        }
        if (db->count > 1) {
            if (nw >= (int)write_slots) { LOG("[RED] [nvn/vk] descriptor write storage exhausted (%zu entries)", write_slots); ok = 0; break; }
            VkDescriptorImageInfo *array = &ii[image_info_at];
            for (uint32_t e = 0; e < db->count; e++) {
                tex_ent *te = NULL;
                if (nvn_direct_array_image(5, r, db->type, e, &array[e], &te) != 0) {
                    LOG("[RED] [nvn/vk] compute binding %u array kind '%s' is not a fixed direct texture/sampler array", db->binding, r->kind);
                    ok = 0; break;
                }
                if (te && nvn_image_use_add(images, &nimg, desc_total, te,
                                            db->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) != 0) {
                    LOG("[RED] [nvn/vk] compute image tracking capacity %u exceeded", desc_total);
                    ok = 0; break;
                }
            }
            if (!ok) break;
            if (add_write(wr, bi, ii, bvs, &nw, (int)write_slots, desc_total, set, db, &buffer_info_at, &image_info_at, buf, range, iv, sm, VK_NULL_HANDLE, array) != 0) { ok = 0; break; }
            continue;
        }
        const char *kind = r->kind;
        if (!strcmp(kind, "c1")) {
            uint8_t zero[16] = {0};
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, sh->c1_len ? sh->c1 : zero, sh->c1_len ? sh->c1_len : 16) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
        } else if (!strcmp(kind, "drvcb")) {
            uint8_t *tmp = malloc(DMSH_DRV_SIZE);
            if (!tmp) { ok = 0; break; }
            drv_fill(tmp, r->bank, 5, sh->key);
            hbuf *b = &hb[nhb];
            int hr = hbuf_make(b, tmp, DMSH_DRV_SIZE); free(tmp);
            if (hr) { ok = 0; break; }
            nhb++; buf = b->buf;
        } else if (!strcmp(kind, "cbuf")) {
            int slot = r->reg;
            uint8_t zero[16] = {0};
            const void *src = (slot >= 0 && slot < 16 && g_st.ubo[5][slot].addr) ? (const void *)(uintptr_t)g_st.ubo[5][slot].addr : zero;
            size_t n = (slot >= 0 && slot < 16 && g_st.ubo[5][slot].size > 16) ? (size_t)g_st.ubo[5][slot].size : 16;
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, src, n) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
        } else if (!strcmp(kind, "ubo_ptr")) {
            // The compute stage reads a uniform slot through its device pointer
            // (Maxwell's c0 address table): the shader's binding is that buffer.
            int slot = r->reg;
            if (slot < 0 || slot >= 16 || !g_st.ubo[5][slot].addr) { ok = 0; break; }
            size_t n = (size_t)(g_st.ubo[5][slot].size ? g_st.ubo[5][slot].size : 16);
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, (const void *)(uintptr_t)g_st.ubo[5][slot].addr, n) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
        } else if (!strcmp(kind, "sbuf") || !strcmp(kind, "sbuf_push")) {
            int slot = r->reg;
            if (slot < 0 || slot >= 16 || !g_st.ssbo[5][slot].addr) { ok = 0; break; }
            size_t n = (size_t)g_st.ssbo[5][slot].size;
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, (const void *)(uintptr_t)g_st.ssbo[5][slot].addr, n) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
            if (nout >= (int)(sizeof outs / sizeof *outs)) {
                LOG("[RED] [nvn/vk] compute storage-buffer copyback capacity exceeded");
                ok = 0; break;
            }
            outs[nout++] = (ssbo_out){b, g_st.ssbo[5][slot].addr, n};
        } else if (!strcmp(kind, "cbsize")) {
            uint32_t sz[DMSH_CBSIZE_SLOTS] = {0};
            for (int q = 0; q < DMSH_CBSIZE_SLOTS; q++) sz[q] = (uint32_t)g_st.ubo[5][q].size;
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, sz, sizeof sz) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
        } else if (!strcmp(kind, "sblen")) {
            uint32_t sz[DMSH_SBLEN_SLOTS * 2] = {0};
            for (int q = 0; q < DMSH_SBLEN_SLOTS && q < 16; q++) { sz[2*q] = (uint32_t)(g_st.ssbo[5][q].size / 4); sz[2*q+1] = sz[2*q]; }
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, sz, sizeof sz) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
        } else if (!strcmp(kind, "tbias") || !strcmp(kind, "support") || !strcmp(kind, "gmem_map") || !strcmp(kind, "gmem_log") || !strcmp(kind, "border") || !strcmp(kind, "geometry")) {
            uint8_t zero[2048] = {0};
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, zero, sizeof zero) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
        } else if (!strcmp(kind, "tbuf")) {
            uint64_t h = (r->reg >= 0 && r->reg < 256) ? g_st.tex[5][r->reg] : 0;
            if (!h) { ok = 0; break; }
            int tid = (int)(h & 0xfffff) + g_st.tex_base;
            tex_id_ent *ti = tid_find(tid);
            if (!ti) { LOG("[RED] [nvn/vk] compute buffer texture id %d is not registered", tid); ok = 0; break; }
            size_t bpp = 0; VkFormat tf = VK_FORMAT_UNDEFINED;
            if (copy_format(&ti->t, &tf, &bpp) != 0) { ok = 0; break; }
            size_t bytes = (size_t)ti->t.d.width * bpp;
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, ti->t.mem, bytes) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
            VkBufferViewCreateInfo bvi = {0};
            bvi.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
            bvi.buffer = b->buf; bvi.format = tf; bvi.offset = 0; bvi.range = bytes;
            VkBufferView tv = VK_NULL_HANDLE;
            if (vkCreateBufferView(g_vk.device, &bvi, NULL, &tv) != VK_SUCCESS) { ok = 0; break; }
            if (ntbuf < (int)write_slots) tbufs[ntbuf++] = tv; else { sub_free_bv(tv); ok = 0; break; }
        } else if (!strcmp(kind, "img_ubo")) {
            uint32_t h = 0;
            if (ubo_handle(5, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] compute bindless image handle at c[%d]+%#x is not in a bound uniform buffer", r->reg, r->off); ok = 0; break; }
            int tid = (int)(h & 0xfffff) + g_st.tex_base;
            img_id_ent *im = imgid_find(tid);
            if (!im) { LOG("[RED] [nvn/vk] compute bindless image id %d is not registered", tid); ok = 0; break; }
            tex_ent *te = tex_ensure(&im->t);
            if (!te) { ok = 0; break; }
            if (tex_stale(te, &im->t)) upload_guest_texture(&im->t, te);
            iv = te->view;
            if (nvn_image_use_add(images, &nimg, desc_total, te,
                                  db->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) != 0) { ok = 0; break; }
        } else if (!strcmp(kind, "tbuf_ubo")) {
            uint32_t h = 0;
            if (ubo_handle(5, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] compute bindless buffer texture handle at c[%d]+%#x is not in a bound uniform buffer", r->reg, r->off); ok = 0; break; }
            int tid = (int)(h & 0xfffff) + g_st.tex_base;
            tex_id_ent *ti = tid_find(tid);
            if (!ti) { LOG("[RED] [nvn/vk] compute bindless buffer texture id %d is not registered", tid); ok = 0; break; }
            size_t bpp = 0; VkFormat tf = VK_FORMAT_UNDEFINED;
            if (copy_format(&ti->t, &tf, &bpp) != 0) { ok = 0; break; }
            size_t bytes = (size_t)ti->t.d.width * bpp;
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, ti->t.mem, bytes) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
            VkBufferViewCreateInfo bvi = {0};
            bvi.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
            bvi.buffer = b->buf; bvi.format = tf; bvi.offset = 0; bvi.range = bytes;
            VkBufferView tv = VK_NULL_HANDLE;
            if (vkCreateBufferView(g_vk.device, &bvi, NULL, &tv) != VK_SUCCESS) { ok = 0; break; }
            if (ntbuf < (int)write_slots) tbufs[ntbuf++] = tv; else { sub_free_bv(tv); ok = 0; break; }
        } else if (!strcmp(kind, "tex_ubo")) {
            uint32_t h = 0;
            if (ubo_handle(5, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] compute bindless texture handle at c[%d]+%#x is not in a bound uniform buffer", r->reg, r->off); ok = 0; break; }
            int tid = (int)(h & 0xfffff) + g_st.tex_base;
            int sid = 0;
            if (r->sreg != -1) { if (bindless_sampler(5, r->sreg, r->soff, &sid) != 0) { LOG("[RED] [nvn/vk] compute bindless sampler beside texture id %d is not readable", tid); ok = 0; break; } }
            else sid = (int)(h >> 20) + g_st.samp_base;
            tex_ent *te = texid_image(tid, &iv);
            if (!te) { LOG("[RED] [nvn/vk] compute bindless texture id %d is not registered", tid); ok = 0; break; }
            if (nvn_image_use_add(images, &nimg, desc_total, te, 0) != 0) { ok = 0; break; }
            sm = sampler_get(sid);
            if (db->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) sm = VK_NULL_HANDLE;
        } else if (!strcmp(kind, "samp_ubo")) {
            uint32_t h = 0;
            if (ubo_handle(5, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] compute bindless sampler handle at c[%d]+%#x is not in a bound uniform buffer", r->reg, r->off); ok = 0; break; }
            int sid = r->raw_id ? (int)h : (int)(h & 0xfffff) + g_st.samp_base;
            sm = sampler_get(sid);
        } else if (!strcmp(kind, "tex") || !strcmp(kind, "samp_tex")) {
            uint64_t h = (r->reg >= 0 && r->reg < 256) ? g_st.tex[5][r->reg] : 0;
            if (h) {
                int tid = (int)(h & 0xfffff) + g_st.tex_base;
                int sid = (int)(h >> 20) + ((h >> 20) ? g_st.samp_base : 0);
                tex_ent *te = texid_image(tid, &iv);
                if (!te) { LOG("[RED] [nvn/vk] compute texture id %d is not registered", tid); ok = 0; break; }
                sm = sampler_get(sid);
                if (db->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) sm = VK_NULL_HANDLE;
                if (nvn_image_use_add(images, &nimg, desc_total, te,
                                      db->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) != 0) {
                    LOG("[RED] [nvn/vk] compute image tracking capacity %u exceeded", desc_total);
                    ok = 0; break;
                }
            }
        } else if (!strcmp(kind, "samp")) {
            uint64_t h = (r->reg >= 0 && r->reg < 128) ? g_st.samp[5][r->reg] : 0;
            sm = sampler_get((int)(h & 0xfffff) + g_st.samp_base);
        } else if (!strcmp(kind, "img")) {
            uint64_t h = (r->reg >= 0 && r->reg < 32) ? g_st.img[5][r->reg] : 0;
            if (!h) { ok = 0; break; }
            img_id_ent *im = imgid_find((int)(h & 0xfffff) + g_st.tex_base);
            if (!im) { ok = 0; break; }
            tex_ent *te = tex_ensure(&im->t);
            if (!te) { ok = 0; break; }
            if (tex_stale(te, &im->t)) upload_guest_texture(&im->t, te);
            iv = te->view;
            if (nvn_image_use_add(images, &nimg, desc_total, te,
                                  db->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) != 0) {
                LOG("[RED] [nvn/vk] compute image tracking capacity %u exceeded", desc_total);
                ok = 0; break;
            }
        } else if (!strcmp(kind, "samp_id")) {
            sm = sampler_get(r->reg);
        } else {
            LOG("[RED] [nvn/vk] compute binding kind '%s' is not replayed yet", kind);
            ok = 0; break;
        }
        VkBufferView tv = (ntbuf && !strcmp(kind, "tbuf")) ? tbufs[ntbuf - 1] : VK_NULL_HANDLE;
        if (add_write(wr, bi, ii, bvs, &nw, (int)write_slots, desc_total, set, db, &buffer_info_at, &image_info_at, buf, range, iv, sm, tv, NULL) != 0) { ok = 0; break; }
    }
    if (ok) for (size_t i = 0; i < nimg; i++)
        nvn_image_shader_barrier(cb, images[i].texture, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (ok && nw && nx_env_bool("NX_VK_DEBUG", 0)) {
        for (int q = 0; q < nw; q++)
            LOG("[nvn/vk] desc %d: set %p binding %u type %d count %u img %p buf %p tbuf %p", q, (void *)wr[q].dstSet, wr[q].dstBinding,
                (int)wr[q].descriptorType, wr[q].descriptorCount, (void *)(wr[q].pImageInfo ? wr[q].pImageInfo->imageView : NULL),
                (void *)(wr[q].pBufferInfo ? wr[q].pBufferInfo->buffer : NULL), (void *)wr[q].pTexelBufferView);
    }
    if (ok && nw) vkUpdateDescriptorSets(g_vk.device, (uint32_t)nw, wr, 0, NULL);
    if (ok) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pv->cpipe);
        if (nw) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pv->pl, 0, 1, &set, 0, NULL);
        if (sh->push_size) vkCmdPushConstants(cb, pv->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 256, g_st.push[1]);
        vkCmdDispatch(cb, groups[0], groups[1], groups[2]);
        if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] %sdispatch %u %u %u, program %llu",
            d->ind ? "indirect " : "", groups[0], groups[1], groups[2], (unsigned long long)pv->serial);
    }
    rp_done(cb);
    if (nout) sub_flush();   // the CPU copies back what the dispatch wrote
    for (int i = 0; i < nout; i++) if (outs[i].b->map) memcpy((void *)(uintptr_t)outs[i].addr, outs[i].b->map, outs[i].size);
    if (ok) for (size_t i = 0; i < nimg; i++)
        if (images[i].writable) { images[i].texture->gpu_dirty = 1; images[i].texture->wgen++; }
    for (int i = 0; i < ntbuf; i++) sub_free_bv(tbufs[i]);
    for (int i = 0; i < nhb; i++) sub_free_hbuf(&hb[i]);
    free(wr); free(bvs); free(hb); free(tbufs); free(bi); free(ii); free(images);
}

// ---------------------------------------------------------------------------
// Texture copies (buffer→texture, texture→buffer, texture→texture)
// ---------------------------------------------------------------------------
// The texture's Vulkan format with its texel block size and extent. A 1x1
// block is an ordinary texel; a larger one is a block-compressed format, whose
// transfers are counted in whole blocks.
static int copy_format_blocks(const tex_t *t, VkFormat *vf, size_t *bpp,
                              uint32_t *block_w, uint32_t *block_h) {
    int bw = 0, bh = 0, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (bpb <= 0 || bw <= 0 || bh <= 0) return -1;
    *vf = vkfmt(t->d.format);
    if (*vf == VK_FORMAT_UNDEFINED) return -1;
    *bpp = (size_t)bpb;
    if (block_w) *block_w = (uint32_t)bw;
    if (block_h) *block_h = (uint32_t)bh;
    return 0;
}
// Only the blit path needs plain texels: Vulkan blits cannot express a
// block-compressed format change, scaling or mirroring here.
static int copy_format(const tex_t *t, VkFormat *vf, size_t *bpp) {
    uint32_t bw = 0, bh = 0;
    if (copy_format_blocks(t, vf, bpp, &bw, &bh) != 0 || bw != 1 || bh != 1) return -1;
    return 0;
}
// Vulkan blits convert normalized / floating color formats through float and
// allow integer conversion only when both sides have the same signedness.
// Depth/stencil and block-compressed copies use separate semantics.
static int nvn_copy_numeric_class(int f) {
    switch (f) {
    case NVN_FORMAT_R8UI: case NVN_FORMAT_R16UI: case NVN_FORMAT_R32UI:
    case NVN_FORMAT_RG8UI: case NVN_FORMAT_RG16UI: case NVN_FORMAT_RG32UI:
    case NVN_FORMAT_RGB8UI: case NVN_FORMAT_RGB16UI: case NVN_FORMAT_RGB32UI:
    case NVN_FORMAT_RGBA8UI: case NVN_FORMAT_RGBA16UI: case NVN_FORMAT_RGBA32UI:
    case NVN_FORMAT_RGB10A2UI: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX16UI:
    case NVN_FORMAT_RGBX32UI: return 2;
    case NVN_FORMAT_R8I: case NVN_FORMAT_R16I: case NVN_FORMAT_R32I:
    case NVN_FORMAT_RG8I: case NVN_FORMAT_RG16I: case NVN_FORMAT_RG32I:
    case NVN_FORMAT_RGB8I: case NVN_FORMAT_RGB16I: case NVN_FORMAT_RGB32I:
    case NVN_FORMAT_RGBA8I: case NVN_FORMAT_RGBA16I: case NVN_FORMAT_RGBA32I:
    case NVN_FORMAT_RGB10A2I: case NVN_FORMAT_RGBX8I: case NVN_FORMAT_RGBX16I:
    case NVN_FORMAT_RGBX32I: return 3;
    case NVN_FORMAT_STENCIL8: case NVN_FORMAT_DEPTH16: case NVN_FORMAT_DEPTH24:
    case NVN_FORMAT_DEPTH32F: case NVN_FORMAT_DEPTH24_STENCIL8:
    case NVN_FORMAT_DEPTH32F_STENCIL8: return 0;
    default: return 1;
    }
}
static void copy_transition(VkCommandBuffer cb, tex_ent *e, VkImageLayout to, VkPipelineStageFlags stage, VkAccessFlags access) {
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = e->layout; b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = e->img; b.subresourceRange = tex_subresources(e);
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = access;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, stage, 0, 0, NULL, 0, NULL, 1, &b);
    e->layout = to;
}

typedef struct {
    VkImageSubresourceLayers subresource;
    VkOffset3D offset;
    VkExtent3D extent;            // in texels
    uint32_t slices, rows;        // stage rows: texel rows, or blocks of a compressed format
    size_t row_bytes, tight_image_bytes, staging_bytes;
} nvn_copy_region;

static int nvn_copy_view_compatible(const tex_t *t, const view_t *view) {
    if (!view) return 1;
    // Transfer commands use the image's storage format and texture target;
    // they do not operate through a VkImageView. Reject reinterpretations
    // until NVN's view-format copy rules are implemented explicitly.
    if (view->set_format && view->format != t->d.format) return 0;
    if (view->set_target && view->target != t->d.target) return 0;
    return 1;
}

static int nvn_copy_region_make(const tex_t *t, tex_ent *e, const view_t *view,
                                const NVNcopyRegion *r, size_t bpp,
                                nvn_copy_region *out) {
    if (!t || !e || !r || !out || !bpp || e->aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
        r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0) return -1;
    uint32_t level = view && view->set_levels ? view->base_level : 0;
    uint32_t nlevels = view && view->set_levels ? view->nlevels : e->levels - level;
    uint32_t base_layer = view && view->set_layers ? view->min_layer : 0;
    uint32_t nlayers = view && view->set_layers ? view->nlayers : e->layers - base_layer;
    if (level >= e->levels || !nlevels || nlevels > e->levels - level ||
        base_layer >= e->layers || !nlayers || nlayers > e->layers - base_layer) return -1;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    if (is3d && view && view->set_layers) return -1;
    uint32_t tw = t->d.width >> level, th = t->d.height >> level;
    uint32_t td = is3d ? t->d.depth >> level : 1;
    if (!tw) tw = 1;
    if (!th) th = 1;
    if (!td) td = 1;
    uint32_t x = (uint32_t)r->xoffset, y = (uint32_t)r->yoffset, z = (uint32_t)r->zoffset;
    if (x >= tw || y >= th) return -1;
    uint32_t w = r->width > 0 ? (uint32_t)r->width : tw - x;
    uint32_t h = r->height > 0 ? (uint32_t)r->height : th - y;
    uint32_t depth = r->depth > 0 ? (uint32_t)r->depth : 1;
    if (!w || !h || !depth || w > tw - x || h > th - y) return -1;
    uint32_t first_layer = 0, layer_count = 1, image_depth = 1;
    if (is3d) {
        if (z >= td || depth > td - z) return -1;
        image_depth = depth;
    } else {
        if (z >= nlayers || depth > nlayers - z) return -1;
        first_layer = base_layer + z;
        layer_count = depth;
        if (t->d.target != NVN_TEXTURE_TARGET_2D_ARRAY &&
            t->d.target != NVN_TEXTURE_TARGET_CUBEMAP &&
            t->d.target != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && (z || depth != 1)) return -1;
    }
    int fbw = 0, fbh = 0;
    int fbpb = nvn_fmt_bpb(t->d.format, &fbw, &fbh);
    if (fbpb <= 0 || fbw <= 0 || fbh <= 0 || (size_t)fbpb != bpp) return -1;
    // A compressed copy moves whole blocks: its rows are block rows and every
    // block that the region touches is copied.
    if ((fbw > 1 || fbh > 1) && ((uint32_t)fbw > w || (uint32_t)fbh > h)) return -1;
    uint32_t blocks_x = (w + (uint32_t)fbw - 1) / (uint32_t)fbw;
    uint32_t rows = (h + (uint32_t)fbh - 1) / (uint32_t)fbh;
    if (blocks_x > SIZE_MAX / bpp) return -1;
    size_t row_bytes = (size_t)blocks_x * bpp;
    if ((size_t)rows > SIZE_MAX / row_bytes) return -1;
    size_t tight_image = row_bytes * (size_t)rows;
    uint32_t slices = is3d ? image_depth : layer_count;
    if ((size_t)slices > SIZE_MAX / tight_image) return -1;
    *out = (nvn_copy_region){
        .subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, first_layer, layer_count},
        .offset = {(int32_t)x, (int32_t)y, is3d ? (int32_t)z : 0},
        .extent = {w, h, image_depth}, .slices = slices, .rows = rows,
        .row_bytes = row_bytes, .tight_image_bytes = tight_image,
        .staging_bytes = tight_image * (size_t)slices,
    };
    return 0;
}

static int nvn_copy_strides_get(const p_copy_strides *cs, const nvn_copy_region *r,
                                size_t *row_pitch, size_t *image_pitch) {
    if (cs && (cs->row < 0 || cs->image < 0)) return -1;
    *row_pitch = cs && cs->row > 0 ? (size_t)cs->row : r->row_bytes;
    if (!r->rows || *row_pitch < r->row_bytes || *row_pitch > SIZE_MAX / r->rows) return -1;
    size_t minimum_image = *row_pitch * r->rows;
    *image_pitch = cs && cs->image > 0 ? (size_t)cs->image : minimum_image;
    if (*image_pitch < minimum_image) return -1;
    return 0;
}

static int nvn_copy_staging_span(const nvn_copy_region *r, size_t row_pitch,
                                 size_t image_pitch, size_t *span) {
    if (!r || !span || !r->slices || !r->rows ||
        row_pitch < r->row_bytes || row_pitch > SIZE_MAX / r->rows)
        return -1;
    size_t minimum_image = row_pitch * (size_t)r->rows;
    if (image_pitch < minimum_image) return -1;
    size_t last_image = 0, last_row = 0;
    if ((size_t)(r->slices - 1) > SIZE_MAX / image_pitch ||
        (size_t)(r->rows - 1) > SIZE_MAX / row_pitch) return -1;
    last_image = (size_t)(r->slices - 1) * image_pitch;
    last_row = (size_t)(r->rows - 1) * row_pitch;
    if (last_image > SIZE_MAX - last_row || last_image + last_row > SIZE_MAX - r->row_bytes) return -1;
    *span = last_image + last_row + r->row_bytes;
    return 0;
}

static void copy_b2t(const p_copy_b2t *c, uint32_t rec_size) {
    nvkd_rp_close();   // these record outside a render pass
    if (!c || !c->tex || !c->src) return;
    size_t bpp = 0; VkFormat vf = VK_FORMAT_UNDEFINED;
    if (copy_format_blocks(c->tex, &vf, &bpp, NULL, NULL) != 0) { warn_once(&g_draw_warned, "NVN buffer-to-texture copy of an unmapped format"); return; }
    tex_ent *e = tex_ensure(c->tex);
    if (!e) return;
    nvn_copy_region region;
    const view_t *view = c->has_view ? &c->view : NULL;
    if (e->samples != 1 || !nvn_copy_view_compatible(c->tex, view) ||
        nvn_copy_region_make(c->tex, e, view, &c->r, bpp, &region) != 0) {
        LOG("[RED] [nvn/vk] buffer-to-texture copy has unsupported view, region, or sample count"); return;
    }
    if ((c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT) != 0) {
        warn_once(&g_draw_warned, "NVN buffer-to-texture copy flags"); return;
    }
    // A partial upload must preserve texels outside the copied region.
    if (tex_stale(e, c->tex) && upload_guest_texture(c->tex, e) != 0) return;
    const p_copy_strides *cs = copy_strides(c, rec_size);
    size_t row_pitch = 0, image_pitch = 0;
    if (nvn_copy_strides_get(cs, &region, &row_pitch, &image_pitch) != 0) {
        LOG("[RED] [nvn/vk] invalid buffer-to-texture copy strides"); return;
    }
    size_t guest_span = 0;
    if (c->src > UINTPTR_MAX || nvn_copy_staging_span(&region, row_pitch, image_pitch, &guest_span) != 0 ||
        guest_span > UINTPTR_MAX - (uintptr_t)c->src || !nvn_host_readable(c->src, guest_span)) {
        LOG("[RED] [nvn/vk] buffer-to-texture source range is not host-readable"); return;
    }
    if (nvn_range_gpu_written(c->src, guest_span)) sub_flush();   // its source may be a buffer the GPU wrote
    hbuf staging;
    if (hbuf_make(&staging, NULL, region.staging_bytes) != 0) return;
    for (uint32_t z = 0; z < region.slices; z++) for (uint32_t y = 0; y < region.rows; y++)
        memcpy((uint8_t *)staging.map + (size_t)z * region.tight_image_bytes + (size_t)y * region.row_bytes,
               (const uint8_t *)(uintptr_t)c->src + (size_t)z * image_pitch + (size_t)y * row_pitch,
               region.row_bytes);
    VkCommandBuffer cb = rp_cb();
    if (!cb) { sub_free_hbuf(&staging); return; }
    copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r = {0};
    r.imageSubresource = region.subresource;
    r.imageOffset = region.offset;
    r.imageExtent = region.extent;
    vkCmdCopyBufferToImage(cb, staging.buf, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    rp_done(cb);
    sub_free_hbuf(&staging);
    e->gpu_dirty = 1; e->wgen++;
}

// ---------- Clears a load action cannot express: one triangle over the target ----------
// An NVN clear writes only the components its own mask names: a colour clear's
// channel mask and a depth-stencil clear's stencil write mask. A Vulkan load
// action, vkCmdClearColorImage and vkCmdClearAttachments all cover every
// component or bit, so those clears are drawn as one full-target triangle with
// the requested write mask (the same mechanism as the Metal host's
// dm/gfx/msl/clear.metal). Scissor rectangles and ClearTexture regions use the
// triangle too: its scissor is the clear's rectangle.
typedef struct {
    VkFormat color, depth;      // color for a colour clear, depth for a depth/stencil clear
    uint32_t samples, mask;     // colour write mask
    uint32_t stencil_mask;      // 0: no stencil test
    int has_depth, depth_write;
    int int_type;               // colour fragment variant: 0 float/normalized, 1 uint, 2 int
} clr_key;

static struct { clr_key k; VkRenderPass rp; VkPipeline pipe; } g_clr_pipes[48];
static int g_nclr_pipes;
static VkShaderModule g_clr_vs_mod, g_clr_fs_mod[3], g_clr_empty_mod;
static VkPipelineLayout g_clr_layout;
static int g_clr_ready;

static int clear_ready(void) {
    if (g_clr_ready) return g_clr_ready > 0;
    g_clr_vs_mod = nx_vk_shader_module(&g_vk, k_clear_vs_spv, sizeof k_clear_vs_spv);
    g_clr_fs_mod[0] = nx_vk_shader_module(&g_vk, k_clear_fs_float_spv, sizeof k_clear_fs_float_spv);
    g_clr_fs_mod[1] = nx_vk_shader_module(&g_vk, k_clear_fs_uint_spv, sizeof k_clear_fs_uint_spv);
    g_clr_fs_mod[2] = nx_vk_shader_module(&g_vk, k_clear_fs_int_spv, sizeof k_clear_fs_int_spv);
    g_clr_empty_mod = nx_vk_shader_module(&g_vk, k_clear_fs_empty_spv, sizeof k_clear_fs_empty_spv);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
    if (!g_clr_vs_mod || !g_clr_fs_mod[0] || !g_clr_fs_mod[1] || !g_clr_fs_mod[2] || !g_clr_empty_mod ||
        vkCreatePipelineLayout(g_vk.device, &pl, NULL, &g_clr_layout) != VK_SUCCESS) {
        LOG("[RED] [nvn/vk] could not create the clear-triangle shaders or pipeline layout; masked clears are skipped");
        g_clr_ready = -1;
        return 0;
    }
    g_clr_ready = 1;
    return 1;
}

static VkPipeline clear_pipeline(const clr_key *k, VkRenderPass rp) {
    for (int i = 0; i < g_nclr_pipes; i++)
        if (g_clr_pipes[i].rp == rp && !memcmp(&g_clr_pipes[i].k, k, sizeof *k)) return g_clr_pipes[i].pipe;
    if (!clear_ready()) return VK_NULL_HANDLE;
    if (g_nclr_pipes == (int)(sizeof g_clr_pipes / sizeof *g_clr_pipes)) {
        LOG("[RED] [nvn/vk] clear-triangle pipeline cache is full");
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {{0}};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_clr_vs_mod; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = k->color != VK_FORMAT_UNDEFINED ? g_clr_fs_mod[k->int_type] : g_clr_empty_mod;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1);
    VkPipelineDepthStencilStateCreateInfo dss = {0};
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = (VkBool32)k->has_depth;
    dss.depthWriteEnable = (VkBool32)(k->depth_write && k->has_depth);
    dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    if (k->stencil_mask) {
        dss.stencilTestEnable = VK_TRUE;
        dss.front.compareOp = dss.back.compareOp = VK_COMPARE_OP_ALWAYS;
        dss.front.passOp = dss.back.passOp = VK_STENCIL_OP_REPLACE;
        dss.front.failOp = dss.back.failOp = VK_STENCIL_OP_KEEP;
        dss.front.depthFailOp = dss.back.depthFailOp = VK_STENCIL_OP_KEEP;
        dss.front.compareMask = dss.back.compareMask = 0xff;
        dss.front.writeMask = dss.back.writeMask = k->stencil_mask;
    }
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = (VkColorComponentFlags)k->mask;
    VkPipelineColorBlendStateCreateInfo cbs = {0};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = k->color != VK_FORMAT_UNDEFINED ? 1u : 0u;
    cbs.pAttachments = cbs.attachmentCount ? &cba : NULL;
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 3; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2; ci.pStages = st;
    ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps; ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms; ci.pDepthStencilState = &dss;
    ci.pColorBlendState = &cbs; ci.pDynamicState = &ds;
    ci.layout = g_clr_layout; ci.renderPass = rp; ci.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.device, g_vk.pipeline_cache, 1, &ci, NULL, &pipe) != VK_SUCCESS) {
        LOG("[RED] [nvn/vk] could not create a clear-triangle pipeline (colour %d depth %d samples %u mask %#x stencil %#x)",
            (int)k->color, (int)k->depth, k->samples, k->mask, k->stencil_mask);
        return VK_NULL_HANDLE;
    }
    g_clr_pipes[g_nclr_pipes].k = *k; g_clr_pipes[g_nclr_pipes].rp = rp; g_clr_pipes[g_nclr_pipes].pipe = pipe;
    g_nclr_pipes++;
    return pipe;
}

static void clear_tri_draw(VkCommandBuffer cb, VkPipeline pipe, uint32_t w, uint32_t h,
                           int32_t x, int32_t y, uint32_t rw, uint32_t rh,
                           const uint32_t value[4], float depth, uint32_t stencil_ref) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D sc = {{x, y}, {rw, rh}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref);
    struct { uint32_t c[4]; float z; } pc;
    memcpy(pc.c, value, sizeof pc.c);
    pc.z = depth;
    vkCmdPushConstants(cb, g_clr_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof pc, &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// NVN's float clear value converted to the attachment's numeric type, as a load
// action does it: unsigned saturates to [0, UINT32_MAX], signed rounds to
// [INT32_MIN, INT32_MAX] and NaN becomes 0.
static uint32_t clear_value_bits(float f, int cls) {
    uint32_t out;
    if (cls == 2) out = f > 0.0f ? (f < 4294967040.0f ? (uint32_t)f : 0xffffffffu) : 0u;
    else if (cls == 3) {
        int32_t si = !(f == f) ? 0 : f >= 2147483520.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
        memcpy(&out, &si, sizeof out);
    } else memcpy(&out, &f, sizeof out);
    return out;
}

static void clear_color_bits(const float rgba[4], int cls, uint32_t out[4]) {
    for (int i = 0; i < 4; i++) out[i] = clear_value_bits(rgba[i], cls);
}

// A ClearTexture value is either a float vector or integer components. An
// integer vector on an integer format keeps its bits; on a float format each
// component converts to float, as Metal's MTLClearColorMake receives it.
static void clear_tex_bits(const p_clear_tex *c, int cls, uint32_t out[4]) {
    for (int i = 0; i < 4; i++) {
        if (c->is_uint && (cls == 2 || cls == 3)) out[i] = c->c.u[i];
        else out[i] = clear_value_bits(c->is_uint ? (float)c->c.u[i] : c->c.f[i], cls);
    }
}

static void clear_color_record(const p_clear_color *c) {
    nvkd_rp_close();   // these record outside a render pass
    if (!c || c->index < 0 || c->index >= g_rt.n || !g_rt.color[c->index]) return;
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if ((c->mask & ~0xf) != 0) {
        static int warned;
        if (!warned++) LOG("[nvn/vk] color clear mask %#x has bits outside NVN's four channels; ignoring them", c->mask);
    }
    tex_t *t = (tex_t *)g_rt.color[c->index];
    tex_ent *e = tex_ensure(t);
    if (!e || e->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return;
    const view_t *view = g_rt.has_cview[c->index] ? &g_rt.cview[c->index] : NULL;
    uint32_t mip = view && view->set_levels ? view->base_level : 0;
    uint32_t base_layer = view && view->set_layers ? view->min_layer : 0;
    uint32_t layers = view && view->set_layers ? view->nlayers : e->layers;
    if (mip >= e->levels || base_layer >= e->layers || !layers || layers > e->layers - base_layer) {
        static int warned;
        if (warned++ < 4) LOG("[RED] [nvn/vk] color clear attachment view is out of range: mip %u/%u layers %u+%u/%u texture %ux%u target %d view %d",
                              mip, e->levels, base_layer, layers, e->layers, t->d.width, t->d.height, t->d.target, view != NULL);
        return;
    }
    uint32_t width = t->d.width >> mip, height = t->d.height >> mip;
    if (!width) width = 1;
    if (!height) height = 1;
    int64_t x0 = g_st.has_scissor ? g_st.sc_x : 0;
    int64_t y0 = g_st.has_scissor ? g_st.sc_y : 0;
    int64_t x1 = g_st.has_scissor ? x0 + g_st.sc_w : width;
    int64_t y1 = g_st.has_scissor ? y0 + g_st.sc_h : height;
    if (g_st.has_scissor && (g_st.sc_w < 0 || g_st.sc_h < 0)) {
        LOG("[RED] [nvn/vk] color clear has a negative scissor extent"); return;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x1 <= x0 || y1 <= y0) return;
    int rectangular = x0 != 0 || y0 != 0 || x1 != width || y1 != height;
    int cls = nvn_copy_numeric_class(t->d.format);
    int int_type = cls == 2 ? 1 : cls == 3 ? 2 : 0;
    int integer = int_type != 0;
    uint32_t bits[4];
    clear_color_bits(c->rgba, cls, bits);
    VkClearColorValue value = {0};
    if (integer) memcpy(value.uint32, bits, sizeof value.uint32);
    else memcpy(value.float32, c->rgba, sizeof value.float32);
    VkCommandBuffer cb = rp_cb();
    if (!cb) return;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, base_layer, layers};
    if (mask == 0xf && !rectangular) {
        copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        rp_done(cb);
        e->gpu_dirty = 1; e->wgen++;
        return;
    }
    int is_3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    if (is_3d && !g_vk.caps.image_2d_view_of_3d) {
        rp_done(cb);
        LOG("[RED] [nvn/vk] scissored color clear of a 3D texture requires image2DViewOf3D"); return;
    }
    if (!e->can_attach) {
        static int warned;
        if (!warned++) LOG("[RED] [nvn/vk] colour clear target format %d cannot be a render target on this device; the clear is dropped", t->d.format);
        rp_done(cb);
        return;
    }
    VkFormat fmt = view && view->set_format ? vkfmt(view->format) : e->fmt;
    if (fmt == VK_FORMAT_UNDEFINED) fmt = e->fmt;
    VkRenderPass rp = rp_get(1, &fmt, VK_FORMAT_UNDEFINED, e->samples);
    if (!rp) { rp_done(cb); LOG("[RED] [nvn/vk] could not create color-clear render pass"); return; }
    int partial_mask = mask != 0xf;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (partial_mask) {
        clr_key k = {0};
        k.color = fmt; k.samples = e->samples; k.mask = mask; k.int_type = int_type;
        pipe = clear_pipeline(&k, rp);
        if (!pipe) {
            rp_done(cb);
            LOG("[RED] [nvn/vk] masked color clear has no clear-triangle pipeline"); return;
        }
    }
    copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    uint32_t slices = is_3d ? (t->d.depth >> mip) : (partial_mask ? layers : 1);
    if (!slices) slices = 1;
    for (uint32_t slice = 0; slice < slices; slice++) {
        // A clear triangle draws every fragment to layer 0: a partial mask over
        // an array target gets one single-layer pass per layer.
        uint32_t view_layers = is_3d ? 1 : (partial_mask ? 1 : layers);
        uint32_t view_layer = is_3d ? slice : (partial_mask ? base_layer + slice : base_layer);
        VkImageViewCreateInfo vi = {0};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = e->img;
        vi.viewType = view_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, mip, 1,
            view_layer, view_layers};
        VkImageView iv = VK_NULL_HANDLE;
        if (vkCreateImageView(g_vk.device, &vi, NULL, &iv) != VK_SUCCESS) {
            rp_done(cb); LOG("[RED] [nvn/vk] could not create color-clear image view"); return;
        }
        VkFramebuffer fb = VK_NULL_HANDLE;
        VkFramebufferCreateInfo fi = {0};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass = rp;
        fi.attachmentCount = 1; fi.pAttachments = &iv; fi.width = width; fi.height = height; fi.layers = view_layers;
        if (vkCreateFramebuffer(g_vk.device, &fi, NULL, &fb) != VK_SUCCESS) {
            sub_free_iv(iv); rp_done(cb);
            LOG("[RED] [nvn/vk] could not create color-clear framebuffer"); return;
        }
        VkRenderPassBeginInfo bi = {0};
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = rp; bi.framebuffer = fb;
        bi.renderArea = (VkRect2D){{0, 0}, {width, height}};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        if (partial_mask) {
            clear_tri_draw(cb, pipe, width, height, (int32_t)x0, (int32_t)y0,
                           (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), bits, 0.0f, 0);
        } else {
            VkClearAttachment ca = {0}; ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            if (integer) memcpy(ca.clearValue.color.uint32, value.uint32, sizeof value.uint32);
            else memcpy(ca.clearValue.color.float32, value.float32, sizeof value.float32);
            VkClearRect cr = {{{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, view_layers};
            vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        }
        vkCmdEndRenderPass(cb);
        sub_free_fb(fb); sub_free_iv(iv);
    }
    e->layout = VK_IMAGE_LAYOUT_GENERAL;
    rp_done(cb);
    e->gpu_dirty = 1; e->wgen++;
}
static void copy_t2b(const p_copy_b2t *c, uint32_t rec_size) {
    nvkd_rp_close();   // these record outside a render pass
    if (!c || !c->tex || !c->src) return;
    size_t bpp = 0; VkFormat vf = VK_FORMAT_UNDEFINED;
    if (copy_format_blocks(c->tex, &vf, &bpp, NULL, NULL) != 0) { warn_once(&g_draw_warned, "NVN texture-to-buffer copy of an unmapped format"); return; }
    tex_ent *e = tex_ensure(c->tex);
    if (!e) return;
    if (tex_stale(e, c->tex) && upload_guest_texture(c->tex, e) != 0) return;
    nvn_copy_region region;
    const view_t *view = c->has_view ? &c->view : NULL;
    if (e->samples != 1 || !nvn_copy_view_compatible(c->tex, view) ||
        nvn_copy_region_make(c->tex, e, view, &c->r, bpp, &region) != 0) {
        LOG("[RED] [nvn/vk] texture-to-buffer copy has unsupported view, region, or sample count"); return;
    }
    if ((c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT) != 0) {
        warn_once(&g_draw_warned, "NVN texture-to-buffer copy flags"); return;
    }
    const p_copy_strides *cs = copy_strides(c, rec_size);
    size_t row_pitch = 0, image_pitch = 0;
    if (nvn_copy_strides_get(cs, &region, &row_pitch, &image_pitch) != 0) {
        LOG("[RED] [nvn/vk] invalid texture-to-buffer copy strides"); return;
    }
    size_t guest_span = 0;
    if (c->src > UINTPTR_MAX || nvn_copy_staging_span(&region, row_pitch, image_pitch, &guest_span) != 0 ||
        guest_span > UINTPTR_MAX - (uintptr_t)c->src || !nvn_host_readable(c->src, guest_span)) {
        LOG("[RED] [nvn/vk] texture-to-buffer destination range is not host-accessible"); return;
    }
    hbuf staging;
    if (hbuf_make(&staging, NULL, region.staging_bytes) != 0) return;
    VkCommandBuffer cb = rp_cb();
    if (!cb) { sub_free_hbuf(&staging); return; }
    copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy r = {0};
    r.imageSubresource = region.subresource;
    r.imageOffset = region.offset;
    r.imageExtent = region.extent;
    vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buf, 1, &r);
    VkBufferMemoryBarrier bb = {0};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = staging.buf; bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
    copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    rp_done(cb);
    if (staging.map) {
        sub_flush();   // the CPU reads what this copy wrote
        for (uint32_t z = 0; z < region.slices; z++) for (uint32_t y = 0; y < region.rows; y++)
            memcpy((uint8_t *)(uintptr_t)c->src + (size_t)z * image_pitch + (size_t)y * row_pitch,
                   (uint8_t *)staging.map + (size_t)z * region.tight_image_bytes + (size_t)y * region.row_bytes,
                   region.row_bytes);
    }
    sub_free_hbuf(&staging);
}
static void copy_t2t(const p_copy_t2t *c) {
    if (!c || !c->src || !c->dst) return;
    size_t sbpp = 0, dbpp = 0; VkFormat sf = VK_FORMAT_UNDEFINED, df = VK_FORMAT_UNDEFINED;
    uint32_t sbw = 0, sbh = 0, dbw = 0, dbh = 0;
    if (copy_format_blocks(c->src, &sf, &sbpp, &sbw, &sbh) != 0 ||
        copy_format_blocks(c->dst, &df, &dbpp, &dbw, &dbh) != 0) {
        warn_once(&g_draw_warned, "NVN texture-to-texture copy of unmapped formats"); return;
    }
    int compressed = sbw > 1 || sbh > 1;
    if (compressed && (sf != df || sbw != dbw || sbh != dbh)) {
        warn_once(&g_draw_warned, "NVN texture-to-texture copy between different block-compressed formats"); return;
    }
    int format_convert = sf != df;
    int src_class = nvn_copy_numeric_class(c->src->d.format);
    int dst_class = nvn_copy_numeric_class(c->dst->d.format);
    if ((format_convert && (!src_class || src_class != dst_class)) ||
        (format_convert && (c->flags & DM_COPY_RESOLVE))) {
        warn_once(&g_draw_warned, "NVN texture-to-texture copy of incompatible numeric formats"); return;
    }
    tex_ent *s = tex_ensure(c->src), *d = tex_ensure(c->dst);
    if (!s || !d) return;
    int resolve = (c->flags & DM_COPY_RESOLVE) != 0;
    // NVN allows a copy whose regions overlap, including one texture onto
    // itself (a scroll). Vulkan copy commands forbid that, so it goes through
    // the staging snapshot below.
    int in_place = s == d;
    if (resolve ? (s->samples == 1 || d->samples != 1) : (s->samples != 1 || d->samples != 1)) {
        warn_once(&g_draw_warned, "NVN copy with unsupported multisample counts"); return;
    }
    int32_t allowed_flags = NVN_COPY_FLAGS_ENGINE_2D_BIT | NVN_COPY_FLAGS_LINEAR_FILTER_BIT |
        NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT;
    if ((c->flags & ~(allowed_flags | DM_COPY_RESOLVE)) != 0) {
        warn_once(&g_draw_warned, "NVN texture copy mirroring or unknown flags"); return;
    }
    const view_t *sv = c->has_sv ? &c->sv : NULL;
    const view_t *dv = c->has_dv ? &c->dv : NULL;
    nvn_copy_region sr, dr;
    if (!nvn_copy_view_compatible(c->src, sv) || !nvn_copy_view_compatible(c->dst, dv) ||
        nvn_copy_region_make(c->src, s, sv, &c->sr, sbpp, &sr) != 0 ||
        nvn_copy_region_make(c->dst, d, dv, &c->dr, dbpp, &dr) != 0 ||
        sr.subresource.layerCount != dr.subresource.layerCount ||
        (c->src->d.target == NVN_TEXTURE_TARGET_3D) != (c->dst->d.target == NVN_TEXTURE_TARGET_3D)) {
        LOG("[RED] [nvn/vk] texture-to-texture source/destination views or regions are incompatible"); return;
    }
    int blit = sr.extent.width != dr.extent.width || sr.extent.height != dr.extent.height ||
        sr.extent.depth != dr.extent.depth ||
        format_convert ||
        (c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT |
                     NVN_COPY_FLAGS_MIRROR_Z_BIT)) != 0 ||
        (!resolve && (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT));
    int mirror_array_z = (c->flags & NVN_COPY_FLAGS_MIRROR_Z_BIT) &&
        c->src->d.target != NVN_TEXTURE_TARGET_3D;
    if (compressed && blit) {
        LOG("[RED] [nvn/vk] block-compressed texture copy cannot scale, mirror or change format"); return;
    }
    if (compressed && (sr.extent.width % sbw || sr.extent.height % sbh || sr.offset.x % (int32_t)sbw ||
                       sr.offset.y % (int32_t)sbh || dr.offset.x % (int32_t)dbw || dr.offset.y % (int32_t)dbh)) {
        LOG("[RED] [nvn/vk] block-compressed texture copy region is not block-aligned"); return;
    }
    if (resolve && blit) {
        LOG("[RED] [nvn/vk] multisample resolve cannot scale or mirror"); return;
    }
    if (in_place && (blit || resolve)) {
        LOG("[RED] [nvn/vk] in-place texture copy cannot scale, mirror, convert or resolve"); return;
    }
    if (blit && !resolve) {
        VkFormatProperties src_props, dst_props;
        vkGetPhysicalDeviceFormatProperties(g_vk.physical, sf, &src_props);
        vkGetPhysicalDeviceFormatProperties(g_vk.physical, df, &dst_props);
        if (!(src_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            !(dst_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
            ((c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) &&
             (!(src_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) || src_class != 1))) {
            LOG("[RED] [nvn/vk] format conversion/blit %d to %d lacks compatible Vulkan format features", sf, df); return;
        }
    }
    if (tex_stale(s, c->src) && upload_guest_texture(c->src, s) != 0) return;
    if (!d->gpu_dirty && upload_guest_texture(c->dst, d) != 0) return;
    if (in_place) {
        // Snapshot the source region into a staging buffer, then write it to
        // the destination region: the guest's overlap semantics (the whole
        // source is read before the destination is written).
        if (sr.row_bytes > SIZE_MAX / sr.rows || sr.row_bytes * sr.rows > SIZE_MAX / sr.slices) return;
        size_t bytes = sr.row_bytes * sr.rows * sr.slices;
        VkBuffer snap = VK_NULL_HANDLE; VkDeviceMemory smem = VK_NULL_HANDLE;
        if (nx_vk_create_buffer(&g_vk, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &snap, &smem, NULL) != 0) {
            LOG("[RED] [nvn/vk] in-place texture copy has no staging buffer"); return;
        }
        VkCommandBuffer cb = rp_cb();
        if (!cb) { nx_vk_destroy_buffer(&g_vk, snap, smem); return; }
        copy_transition(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy out = {0};
        out.imageSubresource = sr.subresource; out.imageOffset = sr.offset; out.imageExtent = sr.extent;
        vkCmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap, 1, &out);
        VkBufferMemoryBarrier bb = {0};
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = snap; bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
        copy_transition(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy in = {0};
        in.imageSubresource = dr.subresource; in.imageOffset = dr.offset; in.imageExtent = dr.extent;
        vkCmdCopyBufferToImage(cb, snap, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &in);
        copy_transition(cb, d, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        rp_done(cb);
        nx_vk_destroy_buffer(&g_vk, snap, smem);
        d->gpu_dirty = 1; d->wgen++;
        return;
    }
    VkCommandBuffer cb = rp_cb();
    if (!cb) return;
    copy_transition(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    copy_transition(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (resolve) {
        VkImageResolve rr = {0};
        rr.srcSubresource = sr.subresource; rr.srcOffset = sr.offset;
        rr.dstSubresource = dr.subresource; rr.dstOffset = dr.offset;
        rr.extent = sr.extent;
        vkCmdResolveImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
    } else if (blit) {
        VkFilter filter = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        if (mirror_array_z) {
            // Array layers are not the Z extent of a Vulkan 2D image. Reverse
            // source-layer selection explicitly while blitting each matching
            // destination layer, preserving X/Y transforms and scaling.
            for (uint32_t layer = 0; layer < sr.subresource.layerCount; layer++) {
                VkImageBlit br = {0};
                br.srcSubresource = sr.subresource; br.dstSubresource = dr.subresource;
                br.srcSubresource.baseArrayLayer += sr.subresource.layerCount - 1 - layer;
                br.srcSubresource.layerCount = 1;
                br.dstSubresource.baseArrayLayer += layer;
                br.dstSubresource.layerCount = 1;
                br.srcOffsets[0] = sr.offset;
                br.srcOffsets[1] = (VkOffset3D){sr.offset.x + (int32_t)sr.extent.width,
                                                 sr.offset.y + (int32_t)sr.extent.height, 1};
                br.dstOffsets[0] = dr.offset;
                br.dstOffsets[1] = (VkOffset3D){dr.offset.x + (int32_t)dr.extent.width,
                                                 dr.offset.y + (int32_t)dr.extent.height, 1};
                if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int32_t x = br.srcOffsets[0].x; br.srcOffsets[0].x = br.srcOffsets[1].x; br.srcOffsets[1].x = x; }
                if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int32_t y = br.srcOffsets[0].y; br.srcOffsets[0].y = br.srcOffsets[1].y; br.srcOffsets[1].y = y; }
                vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &br, filter);
            }
        } else {
            VkImageBlit br = {0};
            br.srcSubresource = sr.subresource; br.dstSubresource = dr.subresource;
            br.srcOffsets[0] = sr.offset;
            br.srcOffsets[1] = (VkOffset3D){sr.offset.x + (int32_t)sr.extent.width,
                                             sr.offset.y + (int32_t)sr.extent.height,
                                             sr.offset.z + (int32_t)sr.extent.depth};
            br.dstOffsets[0] = dr.offset;
            br.dstOffsets[1] = (VkOffset3D){dr.offset.x + (int32_t)dr.extent.width,
                                             dr.offset.y + (int32_t)dr.extent.height,
                                             dr.offset.z + (int32_t)dr.extent.depth};
            if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int32_t x = br.srcOffsets[0].x; br.srcOffsets[0].x = br.srcOffsets[1].x; br.srcOffsets[1].x = x; }
            if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int32_t y = br.srcOffsets[0].y; br.srcOffsets[0].y = br.srcOffsets[1].y; br.srcOffsets[1].y = y; }
            if (c->flags & NVN_COPY_FLAGS_MIRROR_Z_BIT) { int32_t z = br.srcOffsets[0].z; br.srcOffsets[0].z = br.srcOffsets[1].z; br.srcOffsets[1].z = z; }
            vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &br, filter);
        }
    } else {
        VkImageCopy r = {0};
        r.srcSubresource = sr.subresource; r.srcOffset = sr.offset;
        r.dstSubresource = dr.subresource; r.dstOffset = dr.offset;
        r.extent = sr.extent;
        vkCmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    copy_transition(cb, s, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    copy_transition(cb, d, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    rp_done(cb);
    d->gpu_dirty = 1; d->wgen++;
}

// nvnCommandBufferClearTexture: use transfer clear for complete subresources
// and a small clear-only render pass for rectangular color regions. The latter
// preserves pixels outside the region; vkCmdClearColorImage cannot express a
// rectangle. A partial component mask is drawn as the same clear triangle a
// masked attachment clear uses. Depth/stencil formats still take the transfer
// clear and need a distinct implementation for regions or masks.
static void clear_tex_record(const p_clear_tex *c) {
    nvkd_rp_close();   // these record their own passes
    if (!c || !c->tex) return;
    tex_ent *e = tex_ensure(c->tex);
    if (!e) return;
    size_t bpp = 0; VkFormat fmt = VK_FORMAT_UNDEFINED;
    if (copy_format(c->tex, &fmt, &bpp) != 0 || e->samples != 1) {
        LOG("[RED] [nvn/vk] texture clear format or sample count is unsupported"); return;
    }
    const view_t *view = c->has_view ? &c->view : NULL;
    nvn_copy_region region;
    if (!nvn_copy_view_compatible(c->tex, view) ||
        nvn_copy_region_make(c->tex, e, view, &c->r, bpp, &region) != 0) {
        LOG("[RED] [nvn/vk] texture clear has an unsupported view or region"); return;
    }
    int mask = c->mask & 0xf;
    if (!mask) return;
    if ((c->mask & ~0xf) != 0) {
        static int warned;
        if (!warned++) LOG("[nvn/vk] texture clear mask %#x has bits outside NVN's four channels; ignoring them", c->mask);
    }
    int full_mask = mask == 0xf;
    int cls = nvn_copy_numeric_class(c->tex->d.format);
    int int_type = cls == 2 ? 1 : cls == 3 ? 2 : 0;
    uint32_t bits[4];
    clear_tex_bits(c, cls, bits);
    int partial = region.offset.x || region.offset.y || region.offset.z ||
        region.extent.width != (c->tex->d.width >> region.subresource.mipLevel) ||
        region.extent.height != (c->tex->d.height >> region.subresource.mipLevel) ||
        (c->tex->d.target == NVN_TEXTURE_TARGET_3D &&
         (region.offset.z || region.extent.depth != (c->tex->d.depth >> region.subresource.mipLevel)));
    VkCommandBuffer cb = rp_cb();
    if (!cb) return;
    if (full_mask && !partial && region.offset.z == 0 && region.subresource.baseArrayLayer == 0 &&
        region.subresource.layerCount == e->layers && region.subresource.mipLevel == 0 &&
        (c->tex->d.target != NVN_TEXTURE_TARGET_3D || region.extent.depth == c->tex->d.depth)) {
        copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e->levels, 0, e->layers};
        VkClearColorValue v = {0};
        if (int_type) memcpy(v.uint32, bits, sizeof v.uint32);
        else memcpy(v.float32, bits, sizeof v.float32);
        vkCmdClearColorImage(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    } else {
        if (!e->can_attach) {
            static int warned;
            if (!warned++) LOG("[RED] [nvn/vk] texture clear target format %d cannot be a render target on this device", c->tex->d.format);
            rp_done(cb);
            return;
        }
        const int is_3d = c->tex->d.target == NVN_TEXTURE_TARGET_3D;
        if (is_3d && !g_vk.caps.image_2d_view_of_3d) {
            rp_done(cb);
            LOG("[RED] [nvn/vk] partial 3D clear requires Vulkan image2DViewOf3D support"); return;
        }
        VkRenderPass rp = rp_get(1, &fmt, VK_FORMAT_UNDEFINED, 1);
        uint32_t mip_w = c->tex->d.width >> region.subresource.mipLevel;
        uint32_t mip_h = c->tex->d.height >> region.subresource.mipLevel;
        if (!mip_w) mip_w = 1;
        if (!mip_h) mip_h = 1;
        if (rp == VK_NULL_HANDLE) {
            rp_done(cb);
            LOG("[RED] [nvn/vk] could not create partial-clear render pass"); return;
        }
        VkPipeline pipe = VK_NULL_HANDLE;
        if (!full_mask) {
            clr_key k = {0};
            k.color = fmt; k.samples = e->samples; k.mask = mask; k.int_type = int_type;
            pipe = clear_pipeline(&k, rp);
            if (!pipe) {
                rp_done(cb);
                LOG("[RED] [nvn/vk] masked texture clear has no clear-triangle pipeline"); return;
            }
        }
        copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        uint32_t slices = is_3d ? region.extent.depth
                                : (full_mask ? 1 : region.subresource.layerCount);
        for (uint32_t slice = 0; slice < slices; slice++) {
            // A masked clear is a triangle, which reaches only layer 0 of a
            // layered framebuffer: use one single-layer pass per array layer.
            uint32_t layers = is_3d ? 1 : (full_mask ? region.subresource.layerCount : 1);
            uint32_t base_layer = is_3d ? region.offset.z + slice
                                        : (full_mask ? region.subresource.baseArrayLayer
                                                     : region.subresource.baseArrayLayer + slice);
            VkImageViewCreateInfo vi = {0};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = e->img;
            vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            vi.format = fmt;
            vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,
                region.subresource.mipLevel, 1, base_layer, layers};
            VkImageView iv = VK_NULL_HANDLE;
            if (vkCreateImageView(g_vk.device, &vi, NULL, &iv) != VK_SUCCESS) {
                rp_done(cb);
                LOG("[RED] [nvn/vk] could not create partial-clear image view"); return;
            }
            VkFramebuffer fb = VK_NULL_HANDLE;
            VkFramebufferCreateInfo fi = {0};
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass = rp;
            fi.attachmentCount = 1; fi.pAttachments = &iv; fi.width = mip_w; fi.height = mip_h;
            fi.layers = layers;
            if (vkCreateFramebuffer(g_vk.device, &fi, NULL, &fb) != VK_SUCCESS) {
                sub_free_iv(iv); rp_done(cb);
                LOG("[RED] [nvn/vk] could not create partial-clear framebuffer"); return;
            }
            VkRenderPassBeginInfo bi = {0};
            bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = rp; bi.framebuffer = fb;
            bi.renderArea = (VkRect2D){{0, 0}, {mip_w, mip_h}};
            vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
            if (full_mask) {
                VkClearAttachment ca = {0}; ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                if (int_type) memcpy(ca.clearValue.color.uint32, bits, sizeof bits);
                else memcpy(ca.clearValue.color.float32, bits, sizeof bits);
                VkClearRect cr = {{{region.offset.x, region.offset.y}, {region.extent.width, region.extent.height}}, 0, layers};
                vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
            } else {
                clear_tri_draw(cb, pipe, mip_w, mip_h, region.offset.x, region.offset.y,
                               region.extent.width, region.extent.height, bits, 0.0f, 0);
            }
            vkCmdEndRenderPass(cb);
            sub_free_fb(fb);
            sub_free_iv(iv);
        }
        e->layout = VK_IMAGE_LAYOUT_GENERAL;
        rp_done(cb);
    }
    e->gpu_dirty = 1; e->wgen++;
}

// ---------- nvnCommandBufferDrawTexture: a textured rectangle ----------
// The source region of a texture (texels, through the handle's sampler) drawn
// into the destination region of each bound colour target (pixels), the same
// operation dm/gfx/msl/draw_tex.metal draws on the Metal host. The triangle
// covers the target and the fragment stage maps gl_FragCoord through the
// destination origin and scale, so a swapped destination rectangle mirrors
// through negative scale; the scissor is the destination rectangle clamped to
// the target, and the channel mask of each target is its pipeline's write mask.
// NVN's blend state is not applied (logged once when it is on), as on Metal.
typedef struct { VkRenderPass rp; uint32_t mask, samples; VkPipeline pipe; } dt_pipe_ent;
static dt_pipe_ent g_dt_pipes[32];
static int g_ndt_pipes;
static VkShaderModule g_dt_vs, g_dt_fs;
static VkDescriptorSetLayout g_dt_setl;
static VkPipelineLayout g_dt_pl;
static int g_dt_ready;

static int draw_tex_ready(void) {
    if (g_dt_ready) return g_dt_ready > 0;
    g_dt_vs = nx_vk_shader_module(&g_vk, k_drawtex_vs_spv, sizeof k_drawtex_vs_spv);
    g_dt_fs = nx_vk_shader_module(&g_vk, k_drawtex_fs_spv, sizeof k_drawtex_fs_spv);
    VkDescriptorSetLayoutBinding b = {0};
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo sl = {0};
    sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sl.bindingCount = 1; sl.pBindings = &b;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1; pl.pSetLayouts = &g_dt_setl;
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
    if (!g_dt_vs || !g_dt_fs ||
        vkCreateDescriptorSetLayout(g_vk.device, &sl, NULL, &g_dt_setl) != VK_SUCCESS ||
        vkCreatePipelineLayout(g_vk.device, &pl, NULL, &g_dt_pl) != VK_SUCCESS) {
        LOG("[RED] [nvn/vk] could not create the DrawTexture shaders or pipeline layout; DrawTexture is dropped");
        g_dt_ready = -1;
        return 0;
    }
    g_dt_ready = 1;
    return 1;
}

static VkPipeline draw_tex_pipeline(VkRenderPass rp, uint32_t mask, uint32_t samples) {
    for (int i = 0; i < g_ndt_pipes; i++)
        if (g_dt_pipes[i].rp == rp && g_dt_pipes[i].mask == mask &&
            g_dt_pipes[i].samples == samples) return g_dt_pipes[i].pipe;
    if (!draw_tex_ready()) return VK_NULL_HANDLE;
    if (g_ndt_pipes == (int)(sizeof g_dt_pipes / sizeof *g_dt_pipes)) {
        LOG("[RED] [nvn/vk] DrawTexture pipeline cache is full");
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {{0}};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_dt_vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_dt_fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(samples ? samples : 1);
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = (VkColorComponentFlags)mask;
    VkPipelineColorBlendStateCreateInfo cbs = {0};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2; ci.pStages = st;
    ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps; ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms; ci.pColorBlendState = &cbs; ci.pDynamicState = &ds;
    ci.layout = g_dt_pl; ci.renderPass = rp; ci.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.device, g_vk.pipeline_cache, 1, &ci, NULL, &pipe) != VK_SUCCESS) {
        LOG("[RED] [nvn/vk] could not create a DrawTexture pipeline (render pass %p mask %#x)", (void *)rp, mask);
        return VK_NULL_HANDLE;
    }
    g_dt_pipes[g_ndt_pipes].rp = rp; g_dt_pipes[g_ndt_pipes].mask = mask;
    g_dt_pipes[g_ndt_pipes].samples = samples; g_dt_pipes[g_ndt_pipes].pipe = pipe;
    g_ndt_pipes++;
    return pipe;
}

// ceil(v - 0.5) for a non-negative-or-small coordinate, without libm: the
// pixels whose centres lie at or after v (Metal's draw_tex uses the same rule).
static int64_t dt_ceil_half(double v) {
    double t = v - 0.5;
    int64_t i = (int64_t)t;
    if ((double)i < t) i++;
    return i;
}

static void draw_tex_record(const p_draw_tex *c) {
    nvkd_rp_close();   // these record their own passes
    if (!c) return;
    if (g_rt.n <= 0 || !g_rt.color[0]) { warn_once(&g_draw_warned, "NVN DrawTexture without a colour target"); return; }
    float dw = c->d.x1 - c->d.x0, dh = c->d.y1 - c->d.y0;
    if (dw == 0.0f || dh == 0.0f) return;
    int tid = (int)(c->h & 0xfffff) + g_st.tex_base;
    int sid = (c->h >> 20) ? (int)(c->h >> 20) + g_st.samp_base : 0;
    VkImageView src_view = VK_NULL_HANDLE;
    tex_ent *src = texid_image(tid, &src_view);
    if (!src) {
        LOG("[RED] [nvn/vk] DrawTexture: texture id %d (handle %#llx) is not registered",
            tid, (unsigned long long)c->h);
        return;
    }
    if (src->t.d.target != NVN_TEXTURE_TARGET_2D || nvn_copy_numeric_class(src->t.d.format) != 1 ||
        src->samples > 1 || !src_view) {
        static int warned;
        if (!warned++) LOG("[RED] [nvn/vk] DrawTexture: source id %d (target %d, format %d, %u samples) is not a single-sample 2D float/unorm texture; the command is dropped",
                           tid, src->t.d.target, src->t.d.format, src->samples);
        return;
    }
    // The source region is in texels of the level the handle's texture entry
    // samples from (a registered view may select a mip), and the shader
    // normalises by that level's size.
    tex_id_ent *sid_ent = tid_find(tid);
    uint32_t sbase = sid_ent && sid_ent->has_view && sid_ent->v.set_levels ? sid_ent->v.base_level : 0;
    if (sbase >= src->levels) sbase = 0;
    struct { float dst[4]; float src[4]; } pc;
    pc.dst[2] = (c->s.x1 - c->s.x0) / dw;
    pc.dst[3] = (c->s.y1 - c->s.y0) / dh;
    pc.src[0] = c->s.x0; pc.src[1] = c->s.y0;
    pc.src[2] = (float)((src->t.d.width >> sbase) ? (src->t.d.width >> sbase) : 1);
    pc.src[3] = (float)((src->t.d.height >> sbase) ? (src->t.d.height >> sbase) : 1);
    for (int k = 0; k < g_rt.n && k < 8; k++) {
        if (!g_rt.color[k]) break;
        uint32_t wm = (g_st.chmask.mask >> (4 * k)) & 0xf;
        if (!wm) continue;
        tex_ent *dst = tex_ensure(g_rt.color[k]);
        if (!dst) continue;
        if (!dst->can_attach) { static int warned; if (!warned++) LOG("[RED] [nvn/vk] DrawTexture target format %d cannot be a render target on this device", g_rt.color[k]->d.format); continue; }
        int fmtn = g_rt.color[k]->d.format;
        if (g_rt.has_cview[k] && g_rt.cview[k].set_format) fmtn = g_rt.cview[k].format;
        if (nvn_copy_numeric_class(fmtn) != 1 || dst->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            static int warned;
            if (!warned++) LOG("[RED] [nvn/vk] DrawTexture: target %d (format %d) is not a float/unorm colour target; it is skipped",
                               k, fmtn);
            continue;
        }
        if ((g_st.color.blend_en >> k) & 1) {
            static int warned;
            if (!warned++) LOG("[RED] [nvn/vk] DrawTexture with blending enabled on target %d: drawn without it", k);
        }
        uint32_t lv = g_rt.has_cview[k] && g_rt.cview[k].set_levels ? g_rt.cview[k].base_level : 0;
        uint32_t tw = g_rt.color[k]->d.width >> lv, th = g_rt.color[k]->d.height >> lv;
        if (!tw) tw = 1;
        if (!th) th = 1;
        uint32_t samples = dst->samples ? dst->samples : 1;
        VkFormat fmt = dst->fmt;
        view_t dv = {0};
        dv.set_levels = 1; dv.base_level = (uint8_t)lv; dv.nlevels = 1;
        dv.set_layers = 1; dv.nlayers = 1;
        dv.min_layer = g_rt.has_cview[k] && g_rt.cview[k].set_layers ? g_rt.cview[k].min_layer : 0;
        if (g_rt.has_cview[k] && g_rt.cview[k].set_format) { dv.set_format = 1; dv.format = g_rt.cview[k].format; }
        VkImageView dst_view = cached_view(dst, &dv, &fmt, 0);
        if (dst_view == VK_NULL_HANDLE) continue;
        VkRenderPass rp = rp_get(1, &fmt, VK_FORMAT_UNDEFINED, samples);
        if (!rp) { LOG("[RED] [nvn/vk] could not create a DrawTexture render pass"); continue; }
        VkPipeline pipe = draw_tex_pipeline(rp, wm, samples);
        if (!pipe) continue;

        // The destination rectangle (either corner order) within the target
        // and the scissor: the pixels whose centres it contains.
        double x0 = c->d.x0 < c->d.x1 ? c->d.x0 : c->d.x1, x1 = c->d.x0 < c->d.x1 ? c->d.x1 : c->d.x0;
        double y0 = c->d.y0 < c->d.y1 ? c->d.y0 : c->d.y1, y1 = c->d.y0 < c->d.y1 ? c->d.y1 : c->d.y0;
        if (g_st.has_scissor) {
            if (x0 < (double)g_st.sc_x) x0 = g_st.sc_x;
            if (y0 < (double)g_st.sc_y) y0 = g_st.sc_y;
            if (x1 > (double)g_st.sc_x + g_st.sc_w) x1 = (double)g_st.sc_x + g_st.sc_w;
            if (y1 > (double)g_st.sc_y + g_st.sc_h) y1 = (double)g_st.sc_y + g_st.sc_h;
        }
        if (x0 < 0.0) x0 = 0.0;
        if (y0 < 0.0) y0 = 0.0;
        if (x1 > (double)tw) x1 = tw;
        if (y1 > (double)th) y1 = th;
        int64_t sx = dt_ceil_half(x0), sy = dt_ceil_half(y0);
        int64_t ex = dt_ceil_half(x1), ey = dt_ceil_half(y1);
        if (ex <= sx || ey <= sy) continue;
        pc.dst[0] = c->d.x0;
        pc.dst[1] = c->d.y0;

        VkCommandBuffer cb = rp_cb();
        if (!cb) return;
        if (dst->layout != VK_IMAGE_LAYOUT_GENERAL) {
            VkImageMemoryBarrier b = {0};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = dst->layout; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dst->img; b.subresourceRange = tex_subresources(dst);
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                 0, 0, NULL, 0, NULL, 1, &b);
            dst->layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        nvn_image_shader_barrier(cb, src, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pci = {0};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        VkDescriptorPool dpool = VK_NULL_HANDLE;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
        VkFramebufferCreateInfo fci = {0};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &dst_view;
        fci.width = tw; fci.height = th; fci.layers = 1;
        int started = 1;
        if (vkCreateDescriptorPool(g_vk.device, &pci, NULL, &dpool) != VK_SUCCESS) started = 0;
        if (started) {
            VkDescriptorSetAllocateInfo ai = {0};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_dt_setl;
            if (vkAllocateDescriptorSets(g_vk.device, &ai, &dset) != VK_SUCCESS) started = 0;
        }
        if (started && vkCreateFramebuffer(g_vk.device, &fci, NULL, &fb) != VK_SUCCESS) started = 0;
        if (!started) {
            if (fb) sub_free_fb(fb);
            if (dpool) sub_free_pool(dpool);
            rp_done(cb);
            LOG("[RED] [nvn/vk] could not create DrawTexture descriptors or its framebuffer");
            return;
        }
        VkDescriptorImageInfo di = {sampler_get(sid), src_view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr = {0};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = dset; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &di;
        vkUpdateDescriptorSets(g_vk.device, 1, &wr, 0, NULL);

        VkRenderPassBeginInfo bi = {0};
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass = rp; bi.framebuffer = fb;
        bi.renderArea = (VkRect2D){{0, 0}, {tw, th}};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dt_pl, 0, 1, &dset, 0, NULL);
        VkViewport vp = {0.0f, 0.0f, (float)tw, (float)th, 0.0f, 1.0f};
        VkRect2D sc = {{(int32_t)sx, (int32_t)sy}, {(uint32_t)(ex - sx), (uint32_t)(ey - sy)}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdPushConstants(cb, g_dt_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        rp_done(cb);
        sub_free_fb(fb);
        sub_free_pool(dpool);
        dst->gpu_dirty = 1; dst->wgen++;
        if (nx_env_bool("NX_VK_DEBUG", 0))
            LOG("[nvn/vk] DrawTexture: texture id %d (sampler %d) → target %d region %g,%g..%g,%g from %g,%g..%g,%g (%gx%g texels)",
                tid, sid, k, c->d.x0, c->d.y0, c->d.x1, c->d.y1, c->s.x0, c->s.y0, c->s.x1, c->s.y1,
                pc.src[2], pc.src[3]);
        { static int once; if (!once++)
            LOG("[nvn/vk] DrawTexture: first case %gx%g texels → %gx%g pixels of target %d (%ux%u)",
                c->s.x1 - c->s.x0, c->s.y1 - c->s.y0, dw, dh, k, tw, th); }
    }
}
