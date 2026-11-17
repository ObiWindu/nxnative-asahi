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
    case NVN_FORMAT_R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case NVN_FORMAT_RGB565: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case NVN_FORMAT_RGB5A1: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case NVN_FORMAT_RGBA4: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case NVN_FORMAT_DEPTH16: return VK_FORMAT_D16_UNORM;
    case NVN_FORMAT_DEPTH24: return VK_FORMAT_X8_D24_UNORM_PACK32;
    case NVN_FORMAT_DEPTH32F: return VK_FORMAT_D32_SFLOAT;
    case NVN_FORMAT_DEPTH24_STENCIL8: return VK_FORMAT_D24_UNORM_S8_UINT;
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
    case NVN_FORMAT_RGBA_ASTC_8x8_SRGB: return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
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
    nvn_shader sh[6]; int have[6];
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

static prog_vk *pvk_find(uint64_t serial) {
    for (int i = 0; i < g_npvk; i++) if (g_pvks[i].serial == serial) return &g_pvks[i];
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
static prog_vk *pvk_get(const prog_t *p) {
    if (!p || !p->serial) return NULL;
    prog_vk *pv = pvk_find(p->serial);
    if (pv) return pv->state == 1 ? pv : NULL;
    if (g_npvk == g_cpvk) { g_cpvk = g_cpvk ? g_cpvk * 2 : 32; g_pvks = realloc(g_pvks, (size_t)g_cpvk * sizeof *g_pvks); }
    pv = &g_pvks[g_npvk++]; memset(pv, 0, sizeof *pv); pv->serial = p->serial;
    char dir[1024];
    shader_cache_dir(dir, sizeof dir);
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
            return NULL;
        }
        int st = stage_index(sh.stage);
        if (st < 0) st = (int)i;
        if (st >= 6) { nvn_shader_free(&sh); continue; }
        if (st == 3 || st == 4) {
            // Tessellation stages are accepted together; a program with only one
            // of them is rejected at pipeline creation.
        }
        pv->sh[st] = sh; pv->have[st] = 1;
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
    if (pv->have[0] || pv->have[1]) {
        if (!pv->have[0]) {
            LOG("[RED] [nvn/vk] program %llu has no vertex stage; the draws are dropped", (unsigned long long)p->serial);
            pv->state = -1; return NULL;
        }
    } else if (!pv->have[5]) {
        LOG("[RED] [nvn/vk] program %llu has no graphics or compute stage", (unsigned long long)p->serial);
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
                LOG("[RED] [nvn/vk] program %llu has more than %zu descriptor bindings", (unsigned long long)p->serial, sizeof b / sizeof *b);
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
        if (pv->have[0]) LOG("[nvn/vk] program %llu: %d Vulkan shader bindings (%s + %s)", (unsigned long long)p->serial, nb, pv->sh[0].key, pv->sh[1].key);
        else LOG("[nvn/vk] program %llu: %d Vulkan compute bindings (%s)", (unsigned long long)p->serial, nb, pv->sh[5].key);
    }
    return pv;
}

static void nvkd_prefetch(const prog_t *p) { vk_ready(); (void)pvk_get(p); }

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
    rs.frontFace = g_st.poly.front == NVN_FRONT_FACE_CW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
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

typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; size_t size; } hbuf;
static int hbuf_make(hbuf *b, const void *src, size_t n) {
    memset(b, 0, sizeof *b);
    if (!n) n = 4;
    if (nx_vk_create_buffer(&g_vk, n, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &b->buf, &b->mem, &b->map) != 0) return -1;
    b->size = n;
    if (src) memcpy(b->map, src, n);
    return 0;
}
static void hbuf_free(hbuf *b) { if (b->buf || b->mem) nx_vk_destroy_buffer(&g_vk, b->buf, b->mem); memset(b, 0, sizeof *b); }

static VkSampler sampler_get(int sid) {
    for (int i = g_nsamp - 1; i >= 0; i--) if (g_samps[i].id == sid) {
        if (g_samps[i].vk) return g_samps[i].vk;
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
        if (vkCreateSampler(g_vk.device, &ci, NULL, &g_samps[i].vk) != VK_SUCCESS) return VK_NULL_HANDLE;
        return g_samps[i].vk;
    }
    // A handle with no registered sampler: NVN's default (nearest, clamp)
    VkSamplerCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_NEAREST; ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(g_vk.device, &ci, NULL, &s);
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
typedef struct { uint64_t serial; int fmt, base, levels, minl, nlayers; VkImageView view; } tex_view_ent;
static tex_view_ent g_tviews[256]; static int g_ntv;

static VkImageAspectFlags nvn_view_aspect(int fmt) {
    if (fmt == NVN_FORMAT_DEPTH16 || fmt == NVN_FORMAT_DEPTH24 || fmt == NVN_FORMAT_DEPTH32F) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (fmt == NVN_FORMAT_DEPTH24_STENCIL8 || fmt == NVN_FORMAT_DEPTH32F_STENCIL8) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (fmt == NVN_FORMAT_STENCIL8) return VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}
// A view's format/levels/layers as a cached VkImageView. Without a view, the
// texture's base view. Used by sampled descriptors and render targets.
static VkImageView cached_view(tex_ent *e, const view_t *v, VkFormat *fmt_out) {
    if (!v || (!v->set_format && !v->set_levels && !v->set_layers)) { if (fmt_out) *fmt_out = e->fmt; return e->view; }
    int fmt = v->set_format ? v->format : 0, base = v->set_levels ? v->base_level : 0;
    int levels = v->set_levels ? v->nlevels : 1;
    int minl = v->set_layers ? v->min_layer : 0, nlayers = v->set_layers ? v->nlayers : 1;
    VkFormat vf = fmt ? vkfmt(fmt) : e->fmt;
    if (vf == VK_FORMAT_UNDEFINED) { if (fmt_out) *fmt_out = e->fmt; return e->view; }
    if (fmt_out) *fmt_out = vf;
    for (int i = 0; i < g_ntv; i++) {
        tex_view_ent *x = &g_tviews[i];
        if (x->serial == e->serial && x->fmt == fmt && x->base == base && x->levels == levels && x->minl == minl && x->nlayers == nlayers) return x->view;
    }
    if (g_ntv >= (int)(sizeof g_tviews / sizeof *g_tviews)) return e->view;
    VkImageViewCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = e->img; ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.format = vf;
    ci.subresourceRange = (VkImageSubresourceRange){nvn_view_aspect(fmt ? fmt : e->t.d.format), (uint32_t)base, (uint32_t)levels, (uint32_t)minl, (uint32_t)nlayers};
    VkImageView iv = VK_NULL_HANDLE;
    if (vkCreateImageView(g_vk.device, &ci, NULL, &iv) != VK_SUCCESS) return e->view;
    g_tviews[g_ntv++] = (tex_view_ent){e->serial, fmt, base, levels, minl, nlayers, iv};
    return iv;
}

// One guest texture id -> sampled image view. Uploads the guest storage when the
// GPU has not written it since the last CPU write.
static tex_ent *texid_image(int id, VkImageView *view) {
    tex_id_ent *t = tid_find(id);
    if (!t || t->gone) return NULL;
    tex_ent *e = tex_ensure(&t->t);
    if (!e) return NULL;
    if (!e->gpu_dirty && upload_guest_texture(&t->t, e) != 0) return NULL;
    *view = t->has_view ? cached_view(e, &t->v, NULL) : e->view;
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
    // Emit the dependency even when GENERAL is already the current layout:
    // layout equality does not make earlier shader writes visible.
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_stage,
                         0, 0, NULL, 0, NULL, 1, &b);
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
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
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
    nx_vk_oneshot_end(&g_vk, cb);
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
        if (!te->gpu_dirty) upload_guest_texture(&im->t, te);
        out->imageView = te->view;
        if (texture_out) *texture_out = te;
        return 0;
    }
    return -1;
}

// Build and submit one draw. Indexed draws take their indices from the guest.
static int nvkd_indirect_count_live(uint64_t count_addr, uint32_t index, int *live) {
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
    if (!addr || !nvn_host_readable(addr, size)) {
        LOG("[RED] [nvn/vk] indirect arguments at %#llx (%zu bytes) are not host-readable",
            (unsigned long long)addr, size);
        return -1;
    }
    memcpy(out, (const void *)(uintptr_t)addr, size);
    return 0;
}

static void nvkd_draw_common(const prog_t *progp, int indexed, const p_draw *da, const p_draw_idx *di) {
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
    prog_vk *pv = pvk_get(progp);
    if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] draw: program %llu pv %p rt %d count %d", (unsigned long long)progp->serial, (void *)pv, g_rt.n, indexed ? di->count : da->count);
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
        if (i == 0) { dww = g_rt.color[i]->d.width ? g_rt.color[i]->d.width : 1; dhh = g_rt.color[i]->d.height ? g_rt.color[i]->d.height : 1; }
        color[i] = ce[i]->fmt;
        if (g_rt.has_cview[i]) cached_view(ce[i], &g_rt.cview[i], &color[i]);
    }
    if (!ncolor && g_rt.depth) { dww = g_rt.depth->d.width ? g_rt.depth->d.width : 1; dhh = g_rt.depth->d.height ? g_rt.depth->d.height : 1; }
    if (!ncolor && !g_rt.depth) return;
    uint32_t samples = ncolor ? ce[0]->samples : 1;
    tex_ent *de = NULL; VkFormat depth_fmt = VK_FORMAT_UNDEFINED; VkImageView depth_view = VK_NULL_HANDLE;
    if (g_rt.depth) { de = tex_ensure(g_rt.depth); if (de) { depth_fmt = de->fmt; depth_view = g_rt.has_dview ? cached_view(de, &g_rt.dview, &depth_fmt) : de->view; if (!ncolor) samples = de->samples; } }
    VkRenderPass rp = rp_get(ncolor, color, depth_fmt, samples);
    if (!rp) return;
    int prim = da ? da->prim : di->prim;
    int expand_prim = prim == NVN_DRAW_PRIMITIVE_QUADS || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP ||
                      prim == NVN_DRAW_PRIMITIVE_POLYGON || prim == NVN_DRAW_PRIMITIVE_LINE_LOOP;
    g_draw_topo = expand_prim ? (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                              : vktopo(prim);
    VkPipeline pipe = pvk_pipeline(pv, rp, ncolor, color, depth_fmt, samples);
    if (!pipe) return;
    if (nx_env_bool("NX_VK_DEBUG", 0)) {
        LOG("[nvn/vk] draw: pipe %p rp %p ncolor %d fmt0 %d depth %d vbuf0 %#llx+%llu attr0 fmt %u off %u stream %u stride %u",
            (void *)pipe, (void *)rp, ncolor, (int)color[0], (int)depth_fmt,
            (unsigned long long)g_st.vbuf[0].address, (unsigned long long)g_st.vbuf[0].size,
            g_st.attr[0].fmt, g_st.attr[0].offset, g_st.attr[0].stream, g_st.stream[0].stride);
        const float *fv = (const float *)(uintptr_t)g_st.vbuf[0].address;
        if (fv) LOG("[nvn/vk] draw: first vertices %g %g, %g %g, %g %g", fv[0], fv[1], fv[2], fv[3], fv[4], fv[5]);
    }
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
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
    for (int i = 0; i < ncolor; i++) views[nv++] = g_rt.has_cview[i] ? cached_view(ce[i], &g_rt.cview[i], NULL) : ce[i]->view;
    if (de) views[nv++] = depth_view;
    VkFramebufferCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = rp; fci.attachmentCount = nv; fci.pAttachments = views;
    fci.width = dww; fci.height = dhh; fci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(g_vk.device, &fci, NULL, &fb) != VK_SUCCESS) { nx_vk_oneshot_end(&g_vk, cb); return; }

    // Descriptors: one set for all stages, built from each stage's reflection.
    uint32_t total = 0, write_cap = 0;
    for (int st = 0; st < 6; st++) if (pv->have[st])
        for (int i = 0; i < pv->sh[st].ndesc; i++) {
            total += pv->sh[st].desc[i].count ? pv->sh[st].desc[i].count : 1;
            write_cap++;
        }
    VkDescriptorPoolSize ps[8] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, total + 8}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, total + 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, total + 8}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, total + 8},
        {VK_DESCRIPTOR_TYPE_SAMPLER, total + 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, total + 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, total + 8}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, total + 8},
    };
    VkDescriptorPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1; pci.poolSizeCount = 8; pci.pPoolSizes = ps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(g_vk.device, &pci, NULL, &pool) != VK_SUCCESS) { vkDestroyFramebuffer(g_vk.device, fb, NULL); nx_vk_oneshot_end(&g_vk, cb); return; }
    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &pv->setl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) != VK_SUCCESS) { vkDestroyDescriptorPool(g_vk.device, pool, NULL); vkDestroyFramebuffer(g_vk.device, fb, NULL); nx_vk_oneshot_end(&g_vk, cb); return; }

    size_t write_slots = write_cap ? write_cap : 1;
    VkWriteDescriptorSet *wr = calloc(write_slots, sizeof *wr);
    VkBufferView *bvs = calloc(write_slots, sizeof *bvs);
    hbuf *hb = calloc(write_slots, sizeof *hb);
    VkBufferView *tbufs = calloc(write_slots, sizeof *tbufs);
    VkDescriptorBufferInfo *bi = calloc(total ? total : 1, sizeof *bi);
    VkDescriptorImageInfo *ii = calloc(total ? total : 1, sizeof *ii);
    nvn_image_use *image_uses = calloc(total ? total : 1, sizeof *image_uses);
    size_t nimage_uses = 0;
    if (!wr || !bvs || !hb || !tbufs || !bi || !ii || !image_uses) {
        free(wr); free(bvs); free(hb); free(tbufs); free(bi); free(ii); free(image_uses);
        vkDestroyDescriptorPool(g_vk.device, pool, NULL); vkDestroyFramebuffer(g_vk.device, fb, NULL); nx_vk_oneshot_end(&g_vk, cb); return;
    }
    int ntbuf = 0;
    int nw = 0, nhb = 0; size_t buffer_info_at = 0, image_info_at = 0;
    int ok = 1;
    for (int st = 0; st < 6 && ok; st++) if (pv->have[st]) {
        nvn_shader *sh = &pv->sh[st];
        for (int k = 0; k < sh->ndesc && ok; k++) {
            nvn_desc_bind *d = &sh->desc[k];
            const nvn_refl_bind *r = d->refl >= 0 ? &sh->rb[d->refl] : NULL;
            VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize range = VK_WHOLE_SIZE; VkImageView iv = VK_NULL_HANDLE; VkSampler sm = VK_NULL_HANDLE;
            if (!r) { LOG("[RED] [nvn/vk] shader %s binding %u has no reflection entry", sh->key, d->binding); ok = 0; break; }
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
                int slot = r->reg - 3;
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
                else { vkDestroyBufferView(g_vk.device, tv, NULL); ok = 0; break; }
            } else if (!strcmp(kind, "img_ubo")) {
                uint32_t h = 0;
                if (ubo_handle(st, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] bindless image handle at stage %d c[%d]+%#x is not in a bound uniform buffer", st, r->reg, r->off); ok = 0; break; }
                int tid = (int)(h & 0xfffff) + g_st.tex_base;
                img_id_ent *im = imgid_find(tid);
                if (!im) { LOG("[RED] [nvn/vk] bindless image id %d is not registered", tid); ok = 0; break; }
                tex_ent *te = tex_ensure(&im->t);
                if (!te) { ok = 0; break; }
                if (!te->gpu_dirty) upload_guest_texture(&im->t, te);
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
                if (ntbuf < (int)write_cap) tbufs[ntbuf++] = tv; else { vkDestroyBufferView(g_vk.device, tv, NULL); ok = 0; break; }
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
                if (!te->gpu_dirty) upload_guest_texture(&im->t, te);
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
    if (ok && nw) vkUpdateDescriptorSets(g_vk.device, (uint32_t)nw, wr, 0, NULL);

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
        VkRenderPassBeginInfo rbi = {0};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = rp; rbi.framebuffer = fb;
        rbi.renderArea = (VkRect2D){{0, 0}, {dww, dhh}};
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        if (nw) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pv->pl, 0, 1, &set, 0, NULL);
        if (pv->sh[0].push_size || pv->sh[1].push_size || pv->sh[2].push_size || pv->sh[3].push_size || pv->sh[4].push_size)
            vkCmdPushConstants(cb, pv->pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 256, g_st.push[0]);
        float vx = 0, vy = 0, vw = (float)dww, vh = (float)dhh;
        if (g_st.has_vp) { vx = g_st.vp[0][0]; vy = g_st.vp[0][1]; vw = g_st.vp[0][2]; vh = g_st.vp[0][3]; }
        VkViewport vp = {vx, g_origin_ll ? vy : vy + vh, vw, g_origin_ll ? vh : -vh,
                         g_st.has_vp ? g_st.dr[0] : 0.0f, g_st.has_vp ? g_st.dr[1] : 1.0f};
        VkRect2D sc = g_st.has_scissor ? (VkRect2D){{g_st.sc_x, g_st.sc_y}, {(uint32_t)g_st.sc_w, (uint32_t)g_st.sc_h}}
                                        : (VkRect2D){{0, 0}, {dww, dhh}};
        if (sc.extent.width > dww) sc.extent.width = dww;
        if (sc.extent.height > dhh) sc.extent.height = dhh;
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        if (g_st.ds.st) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_st.st_ref[0]);
        for (int s = 0; s < 16; s++) if (used_stream[s]) {
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &vbufs[s], &off);
        }
        if (draw_indexed) {
            if (has_ib) vkCmdBindIndexBuffer(cb, ib.buf, 0, draw_index_type);
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
        vkCmdEndRenderPass(cb);
        for (int i = 0; i < ncolor; i++) ce[i]->gpu_dirty = 1;
        for (size_t i = 0; i < nimage_uses; i++)
            if (image_uses[i].writable) image_uses[i].texture->gpu_dirty = 1;
    }
    nx_vk_oneshot_end(&g_vk, cb);
    for (int i = 0; i < ntbuf; i++) vkDestroyBufferView(g_vk.device, tbufs[i], NULL);
    for (int i = 0; i < nhb; i++) hbuf_free(&hb[i]);
    free(wr); free(bvs); free(hb); free(tbufs); free(bi); free(ii); free(image_uses);
    for (int i = 0; i < nvb; i++) hbuf_free(&vb[i]);
    if (has_ib) hbuf_free(&ib);
    if (pool) vkDestroyDescriptorPool(g_vk.device, pool, NULL);
    vkDestroyFramebuffer(g_vk.device, fb, NULL);
}

// Large NVN indirect-count batches are CPU-expanded here because this replay
// backend submits each preceding operation synchronously. The count and
// argument reads therefore observe earlier compute writes in command order.
static void nvkd_multi_draw(const p_multi_draw *m) {
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
            if (g_bound_prog) nvkd_draw_common(g_bound_prog, 0, &d, NULL);
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
            if (g_bound_prog) nvkd_draw_common(g_bound_prog, 1, NULL, &d);
        }
    }
}

// Compute dispatch: a compute pipeline from the program's stage 5, the same
// descriptor mapping as a draw, and a copy-back of the storage buffers the
// shader wrote so the guest sees the results.
typedef struct { hbuf *b; uint64_t addr; size_t size; } ssbo_out;
static void nvkd_dispatch(const p_dispatch *d) {
    prog_vk *pv = pvk_get(g_bound_prog);
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
    VkDescriptorPoolSize ps[8] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, desc_total + 8}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, desc_total + 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, desc_total + 8}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, desc_total + 8},
        {VK_DESCRIPTOR_TYPE_SAMPLER, desc_total + 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, desc_total + 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, desc_total + 8}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, desc_total + 8},
    };
    VkDescriptorPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1; pci.poolSizeCount = 8; pci.pPoolSizes = ps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(g_vk.device, &pci, NULL, &pool) != VK_SUCCESS) return;
    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &pv->setl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) != VK_SUCCESS) { vkDestroyDescriptorPool(g_vk.device, pool, NULL); return; }
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) { vkDestroyDescriptorPool(g_vk.device, pool, NULL); return; }
    VkWriteDescriptorSet *wr = calloc(write_slots, sizeof *wr);
    VkBufferView *bvs = calloc(write_slots, sizeof *bvs);
    hbuf *hb = calloc(write_slots, sizeof *hb);
    VkBufferView *tbufs = calloc(write_slots, sizeof *tbufs);
    VkDescriptorBufferInfo *bi = calloc(desc_total ? desc_total : 1, sizeof *bi);
    VkDescriptorImageInfo *ii = calloc(desc_total ? desc_total : 1, sizeof *ii);
    nvn_image_use *images = calloc(desc_total ? desc_total : 1, sizeof *images);
    if (!wr || !bvs || !hb || !tbufs || !bi || !ii || !images) {
        free(wr); free(bvs); free(hb); free(tbufs); free(bi); free(ii); free(images);
        nx_vk_oneshot_end(&g_vk, cb); vkDestroyDescriptorPool(g_vk.device, pool, NULL); return;
    }
    int ntbuf = 0;
    int nhb = 0, nw = 0, ok = 1; size_t buffer_info_at = 0, image_info_at = 0;
    ssbo_out outs[64]; int nout = 0;
    size_t nimg = 0;
    for (int k = 0; k < sh->ndesc && ok; k++) {
        nvn_desc_bind *db = &sh->desc[k];
        const nvn_refl_bind *r = db->refl >= 0 ? &sh->rb[db->refl] : NULL;
        VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize range = VK_WHOLE_SIZE; VkImageView iv = VK_NULL_HANDLE; VkSampler sm = VK_NULL_HANDLE;
        if (!r) { LOG("[RED] [nvn/vk] compute binding %u has no reflection entry", db->binding); ok = 0; break; }
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
            int slot = r->reg - 3;
            uint8_t zero[16] = {0};
            const void *src = (slot >= 0 && slot < 16 && g_st.ubo[5][slot].addr) ? (const void *)(uintptr_t)g_st.ubo[5][slot].addr : zero;
            size_t n = (slot >= 0 && slot < 16 && g_st.ubo[5][slot].size > 16) ? (size_t)g_st.ubo[5][slot].size : 16;
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, src, n) != 0) { ok = 0; break; }
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
            if (ntbuf < (int)write_slots) tbufs[ntbuf++] = tv; else { vkDestroyBufferView(g_vk.device, tv, NULL); ok = 0; break; }
        } else if (!strcmp(kind, "img_ubo")) {
            uint32_t h = 0;
            if (ubo_handle(5, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] compute bindless image handle at c[%d]+%#x is not in a bound uniform buffer", r->reg, r->off); ok = 0; break; }
            int tid = (int)(h & 0xfffff) + g_st.tex_base;
            img_id_ent *im = imgid_find(tid);
            if (!im) { LOG("[RED] [nvn/vk] compute bindless image id %d is not registered", tid); ok = 0; break; }
            tex_ent *te = tex_ensure(&im->t);
            if (!te) { ok = 0; break; }
            if (!te->gpu_dirty) upload_guest_texture(&im->t, te);
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
            if (ntbuf < (int)write_slots) tbufs[ntbuf++] = tv; else { vkDestroyBufferView(g_vk.device, tv, NULL); ok = 0; break; }
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
            if (!te->gpu_dirty) upload_guest_texture(&im->t, te);
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
    nx_vk_oneshot_end(&g_vk, cb);
    for (int i = 0; i < nout; i++) if (outs[i].b->map) memcpy((void *)(uintptr_t)outs[i].addr, outs[i].b->map, outs[i].size);
    if (ok) for (size_t i = 0; i < nimg; i++)
        if (images[i].writable) images[i].texture->gpu_dirty = 1;
    for (int i = 0; i < ntbuf; i++) vkDestroyBufferView(g_vk.device, tbufs[i], NULL);
    for (int i = 0; i < nhb; i++) hbuf_free(&hb[i]);
    vkDestroyDescriptorPool(g_vk.device, pool, NULL);
    free(wr); free(bvs); free(hb); free(tbufs); free(bi); free(ii); free(images);
}

// ---------------------------------------------------------------------------
// Texture copies (buffer→texture, texture→buffer, texture→texture)
// ---------------------------------------------------------------------------
static int copy_format(const tex_t *t, VkFormat *vf, size_t *bpp) {
    int bw = 0, bh = 0, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (bpb <= 0 || bw != 1 || bh != 1) return -1;
    *vf = vkfmt(t->d.format);
    if (*vf == VK_FORMAT_UNDEFINED) return -1;
    *bpp = (size_t)bpb;
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
    VkExtent3D extent;
    uint32_t slices;
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
    if ((size_t)w > SIZE_MAX / bpp) return -1;
    size_t row_bytes = (size_t)w * bpp;
    if ((size_t)h > SIZE_MAX / row_bytes) return -1;
    size_t tight_image = row_bytes * h;
    uint32_t slices = is3d ? image_depth : layer_count;
    if ((size_t)slices > SIZE_MAX / tight_image) return -1;
    *out = (nvn_copy_region){
        .subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, first_layer, layer_count},
        .offset = {(int32_t)x, (int32_t)y, is3d ? (int32_t)z : 0},
        .extent = {w, h, image_depth}, .slices = slices,
        .row_bytes = row_bytes, .tight_image_bytes = tight_image,
        .staging_bytes = tight_image * (size_t)slices,
    };
    return 0;
}

static int nvn_copy_strides_get(const p_copy_strides *cs, const nvn_copy_region *r,
                                size_t *row_pitch, size_t *image_pitch) {
    if (cs && (cs->row < 0 || cs->image < 0)) return -1;
    *row_pitch = cs && cs->row > 0 ? (size_t)cs->row : r->row_bytes;
    if (*row_pitch < r->row_bytes || *row_pitch > SIZE_MAX / r->extent.height) return -1;
    size_t minimum_image = *row_pitch * r->extent.height;
    *image_pitch = cs && cs->image > 0 ? (size_t)cs->image : minimum_image;
    if (*image_pitch < minimum_image) return -1;
    return 0;
}

static int nvn_copy_staging_span(const nvn_copy_region *r, size_t row_pitch,
                                 size_t image_pitch, size_t *span) {
    if (!r || !span || !r->slices || !r->extent.height ||
        row_pitch < r->row_bytes || row_pitch > SIZE_MAX / r->extent.height)
        return -1;
    size_t minimum_image = row_pitch * (size_t)r->extent.height;
    if (image_pitch < minimum_image) return -1;
    size_t last_image = 0, last_row = 0;
    if ((size_t)(r->slices - 1) > SIZE_MAX / image_pitch ||
        (size_t)(r->extent.height - 1) > SIZE_MAX / row_pitch) return -1;
    last_image = (size_t)(r->slices - 1) * image_pitch;
    last_row = (size_t)(r->extent.height - 1) * row_pitch;
    if (last_image > SIZE_MAX - last_row || last_image + last_row > SIZE_MAX - r->row_bytes) return -1;
    *span = last_image + last_row + r->row_bytes;
    return 0;
}

static void copy_b2t(const p_copy_b2t *c, uint32_t rec_size) {
    if (!c || !c->tex || !c->src) return;
    size_t bpp = 0; VkFormat vf = VK_FORMAT_UNDEFINED;
    if (copy_format(c->tex, &vf, &bpp) != 0) { warn_once(&g_draw_warned, "NVN buffer-to-texture copy of a block-compressed format"); return; }
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
    if (!e->gpu_dirty && upload_guest_texture(c->tex, e) != 0) return;
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
    hbuf staging;
    if (hbuf_make(&staging, NULL, region.staging_bytes) != 0) return;
    for (uint32_t z = 0; z < region.slices; z++) for (uint32_t y = 0; y < region.extent.height; y++)
        memcpy((uint8_t *)staging.map + (size_t)z * region.tight_image_bytes + (size_t)y * region.row_bytes,
               (const uint8_t *)(uintptr_t)c->src + (size_t)z * image_pitch + (size_t)y * row_pitch,
               region.row_bytes);
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) { hbuf_free(&staging); return; }
    copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r = {0};
    r.imageSubresource = region.subresource;
    r.imageOffset = region.offset;
    r.imageExtent = region.extent;
    vkCmdCopyBufferToImage(cb, staging.buf, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    nx_vk_oneshot_end(&g_vk, cb);
    hbuf_free(&staging);
    e->gpu_dirty = 1;
}

static void clear_color_record(const p_clear_color *c) {
    if (!c || c->index < 0 || c->index >= g_rt.n || !g_rt.color[c->index]) return;
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if (mask != 0xf || (c->mask & ~0xf) != 0) {
        LOG("[RED] [nvn/vk] partial attachment color clear mask %#x is unsupported", c->mask); return;
    }
    tex_t *t = (tex_t *)g_rt.color[c->index];
    tex_ent *e = tex_ensure(t);
    if (!e || e->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return;
    const view_t *view = g_rt.has_cview[c->index] ? &g_rt.cview[c->index] : NULL;
    uint32_t mip = view && view->set_levels ? view->base_level : 0;
    uint32_t base_layer = view && view->set_layers ? view->min_layer : 0;
    uint32_t layers = view && view->set_layers ? view->nlayers : e->layers;
    if (mip >= e->levels || base_layer >= e->layers || !layers || layers > e->layers - base_layer) {
        LOG("[RED] [nvn/vk] color clear attachment view is out of range"); return;
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
    int integer = nvn_copy_numeric_class(t->d.format) == 2 || nvn_copy_numeric_class(t->d.format) == 3;
    VkClearColorValue value = {0};
    if (integer) for (int i = 0; i < 4; i++) value.uint32[i] = (uint32_t)c->rgba[i];
    else memcpy(value.float32, c->rgba, sizeof value.float32);
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) return;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, base_layer, layers};
    if (!rectangular) {
        copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        nx_vk_oneshot_end(&g_vk, cb);
        e->gpu_dirty = 1;
        return;
    }
    int is_3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    if (is_3d && !g_vk.caps.image_2d_view_of_3d) {
        nx_vk_oneshot_end(&g_vk, cb);
        LOG("[RED] [nvn/vk] scissored color clear of a 3D texture requires image2DViewOf3D"); return;
    }
    VkFormat fmt = view && view->set_format ? vkfmt(view->format) : e->fmt;
    if (fmt == VK_FORMAT_UNDEFINED) fmt = e->fmt;
    VkRenderPass rp = rp_get(1, &fmt, VK_FORMAT_UNDEFINED, e->samples);
    if (!rp) { nx_vk_oneshot_end(&g_vk, cb); LOG("[RED] [nvn/vk] could not create color-clear render pass"); return; }
    copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    uint32_t slices = is_3d ? (t->d.depth >> mip) : 1;
    if (!slices) slices = 1;
    for (uint32_t slice = 0; slice < slices; slice++) {
        uint32_t view_layers = is_3d ? 1 : layers;
        VkImageViewCreateInfo vi = {0};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = e->img;
        vi.viewType = !is_3d && view_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, mip, 1,
            is_3d ? slice : base_layer, view_layers};
        VkImageView iv = VK_NULL_HANDLE;
        if (vkCreateImageView(g_vk.device, &vi, NULL, &iv) != VK_SUCCESS) {
            nx_vk_oneshot_end(&g_vk, cb); LOG("[RED] [nvn/vk] could not create color-clear image view"); return;
        }
        VkFramebuffer fb = VK_NULL_HANDLE;
        VkFramebufferCreateInfo fi = {0};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass = rp;
        fi.attachmentCount = 1; fi.pAttachments = &iv; fi.width = width; fi.height = height; fi.layers = view_layers;
        if (vkCreateFramebuffer(g_vk.device, &fi, NULL, &fb) != VK_SUCCESS) {
            vkDestroyImageView(g_vk.device, iv, NULL); nx_vk_oneshot_end(&g_vk, cb);
            LOG("[RED] [nvn/vk] could not create color-clear framebuffer"); return;
        }
        VkRenderPassBeginInfo bi = {0};
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = rp; bi.framebuffer = fb;
        bi.renderArea = (VkRect2D){{0, 0}, {width, height}};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment ca = {0}; ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        if (integer) memcpy(ca.clearValue.color.uint32, value.uint32, sizeof value.uint32);
        else memcpy(ca.clearValue.color.float32, value.float32, sizeof value.float32);
        VkClearRect cr = {{{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, view_layers};
        vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        vkCmdEndRenderPass(cb);
        vkDestroyFramebuffer(g_vk.device, fb, NULL); vkDestroyImageView(g_vk.device, iv, NULL);
    }
    e->layout = VK_IMAGE_LAYOUT_GENERAL;
    nx_vk_oneshot_end(&g_vk, cb);
    e->gpu_dirty = 1;
}
static void copy_t2b(const p_copy_b2t *c, uint32_t rec_size) {
    if (!c || !c->tex || !c->src) return;
    size_t bpp = 0; VkFormat vf = VK_FORMAT_UNDEFINED;
    if (copy_format(c->tex, &vf, &bpp) != 0) { warn_once(&g_draw_warned, "NVN texture-to-buffer copy of a block-compressed format"); return; }
    tex_ent *e = tex_ensure(c->tex);
    if (!e) return;
    if (!e->gpu_dirty && upload_guest_texture(c->tex, e) != 0) return;
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
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) { hbuf_free(&staging); return; }
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
    nx_vk_oneshot_end(&g_vk, cb);
    if (staging.map) {
        for (uint32_t z = 0; z < region.slices; z++) for (uint32_t y = 0; y < region.extent.height; y++)
            memcpy((uint8_t *)(uintptr_t)c->src + (size_t)z * image_pitch + (size_t)y * row_pitch,
                   (uint8_t *)staging.map + (size_t)z * region.tight_image_bytes + (size_t)y * region.row_bytes,
                   region.row_bytes);
    }
    hbuf_free(&staging);
}
static void copy_t2t(const p_copy_t2t *c) {
    if (!c || !c->src || !c->dst) return;
    size_t sbpp = 0, dbpp = 0; VkFormat sf = VK_FORMAT_UNDEFINED, df = VK_FORMAT_UNDEFINED;
    if (copy_format(c->src, &sf, &sbpp) != 0 || copy_format(c->dst, &df, &dbpp) != 0) {
        warn_once(&g_draw_warned, "NVN texture-to-texture copy of block-compressed or unmapped formats"); return;
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
    if (s == d) { warn_once(&g_draw_warned, "NVN in-place texture copy"); return; }
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
    if (resolve && blit) {
        LOG("[RED] [nvn/vk] multisample resolve cannot scale or mirror"); return;
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
    if (!s->gpu_dirty && upload_guest_texture(c->src, s) != 0) return;
    if (!d->gpu_dirty && upload_guest_texture(c->dst, d) != 0) return;
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
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
    nx_vk_oneshot_end(&g_vk, cb);
    d->gpu_dirty = 1;
}

// nvnCommandBufferClearTexture: use transfer clear for complete subresources
// and a small clear-only render pass for rectangular color regions. The latter
// preserves pixels outside the region; vkCmdClearColorImage cannot express a
// rectangle. Partial component masks and depth/stencil formats need a distinct
// implementation, so they remain explicit errors instead of being widened.
static void clear_tex_record(const p_clear_tex *c) {
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
    if ((c->mask & 0xf) != 0xf || (c->mask & ~0xf) != 0) {
        warn_once(&g_draw_warned, "NVN partial texture clear component mask"); return;
    }
    int partial = region.offset.x || region.offset.y || region.offset.z ||
        region.extent.width != (c->tex->d.width >> region.subresource.mipLevel) ||
        region.extent.height != (c->tex->d.height >> region.subresource.mipLevel) ||
        (c->tex->d.target == NVN_TEXTURE_TARGET_3D &&
         (region.offset.z || region.extent.depth != (c->tex->d.depth >> region.subresource.mipLevel)));
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) return;
    if (!partial && region.offset.z == 0 && region.subresource.baseArrayLayer == 0 &&
        region.subresource.layerCount == e->layers && region.subresource.mipLevel == 0 &&
        (c->tex->d.target != NVN_TEXTURE_TARGET_3D || region.extent.depth == c->tex->d.depth)) {
        copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e->levels, 0, e->layers};
        if (c->is_uint) {
            VkClearColorValue v; memcpy(v.uint32, c->c.u, sizeof v.uint32);
            vkCmdClearColorImage(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        } else {
            VkClearColorValue v; memcpy(v.float32, c->c.f, sizeof v.float32);
            vkCmdClearColorImage(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        }
        copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    } else {
        const int is_3d = c->tex->d.target == NVN_TEXTURE_TARGET_3D;
        if (is_3d && !g_vk.caps.image_2d_view_of_3d) {
            nx_vk_oneshot_end(&g_vk, cb);
            LOG("[RED] [nvn/vk] partial 3D clear requires Vulkan image2DViewOf3D support"); return;
        }
        VkRenderPass rp = rp_get(1, &fmt, VK_FORMAT_UNDEFINED, 1);
        uint32_t mip_w = c->tex->d.width >> region.subresource.mipLevel;
        uint32_t mip_h = c->tex->d.height >> region.subresource.mipLevel;
        if (!mip_w) mip_w = 1;
        if (!mip_h) mip_h = 1;
        if (rp == VK_NULL_HANDLE) {
            nx_vk_oneshot_end(&g_vk, cb);
            LOG("[RED] [nvn/vk] could not create partial-clear render pass"); return;
        }
        copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        uint32_t slices = is_3d ? region.extent.depth : 1;
        for (uint32_t slice = 0; slice < slices; slice++) {
            uint32_t base_layer = is_3d ? region.offset.z + slice : region.subresource.baseArrayLayer;
            uint32_t layers = is_3d ? 1 : region.subresource.layerCount;
            VkImageViewCreateInfo vi = {0};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = e->img;
            vi.viewType = !is_3d && layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            vi.format = fmt;
            vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,
                region.subresource.mipLevel, 1, base_layer, layers};
            VkImageView iv = VK_NULL_HANDLE;
            if (vkCreateImageView(g_vk.device, &vi, NULL, &iv) != VK_SUCCESS) {
                nx_vk_oneshot_end(&g_vk, cb);
                LOG("[RED] [nvn/vk] could not create partial-clear image view"); return;
            }
            VkFramebuffer fb = VK_NULL_HANDLE;
            VkFramebufferCreateInfo fi = {0};
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass = rp;
            fi.attachmentCount = 1; fi.pAttachments = &iv; fi.width = mip_w; fi.height = mip_h;
            fi.layers = layers;
            if (vkCreateFramebuffer(g_vk.device, &fi, NULL, &fb) != VK_SUCCESS) {
                vkDestroyImageView(g_vk.device, iv, NULL); nx_vk_oneshot_end(&g_vk, cb);
                LOG("[RED] [nvn/vk] could not create partial-clear framebuffer"); return;
            }
            VkRenderPassBeginInfo bi = {0};
            bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = rp; bi.framebuffer = fb;
            bi.renderArea = (VkRect2D){{0, 0}, {mip_w, mip_h}};
            vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
            VkClearAttachment ca = {0}; ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            if (c->is_uint) memcpy(ca.clearValue.color.uint32, c->c.u, sizeof c->c.u);
            else memcpy(ca.clearValue.color.float32, c->c.f, sizeof c->c.f);
            VkClearRect cr = {{{region.offset.x, region.offset.y}, {region.extent.width, region.extent.height}}, 0, layers};
            vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
            vkCmdEndRenderPass(cb);
            vkDestroyFramebuffer(g_vk.device, fb, NULL);
            vkDestroyImageView(g_vk.device, iv, NULL);
        }
        e->layout = VK_IMAGE_LAYOUT_GENERAL;
        nx_vk_oneshot_end(&g_vk, cb);
    }
    e->gpu_dirty = 1;
}
