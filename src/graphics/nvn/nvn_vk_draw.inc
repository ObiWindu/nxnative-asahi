// ---------------------------------------------------------------------------
// General NVN draw path on Vulkan.
//
// The recording layer has already resolved the game's NVN state and the
// offline build has translated its shaders to SPIR-V (nvn_spv.c). This block
// links the two: it builds a Vulkan pipeline and descriptor set from the
// shader reflection, uploads the guest's vertex/uniform/storage data, binds
// the guest textures and samplers, and replays the draw.
//
// It deliberately refuses what it cannot do correctly (bindless pool arrays,
// descriptor arrays, mesh/task stages and unknown reflection kinds) with a red
// line instead of drawing a wrong picture; those are the next blocks, not
// silent fallbacks. Geometry, tessellation, compute, buffer textures and
// bindless handles in the game's uniform buffers are replayed.
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
// An advanced blend mode (blend_st.adv, hle_nvn.c) replaces the blend
// functions. The Porter-Duff modes with uncorrelated overlap are sums of the
// source and destination weighted by 1, 0, As, Ad and their complements, which
// fixed-function factors give exactly; the result uses the ADD equation. A
// source that is not premultiplied is multiplied by As first, which only a
// source factor of ONE can take. The other modes (multiply, screen, overlay,
// HSL, ...) need the destination colour in the shader and keep the state's own
// functions, with a red line. This is the same mapping as the Metal host's
// adv_blend.
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
    // One descriptor set for all stages, each stage's bindings in its own range.
    VkDescriptorSetLayoutBinding b[128]; int nb = 0;
    for (int st = 0; st < 6; st++) if (pv->have[st]) {
        VkShaderStageFlags flags = st == 0 ? VK_SHADER_STAGE_VERTEX_BIT : st == 2 ? VK_SHADER_STAGE_GEOMETRY_BIT :
                                    st == 3 ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : st == 4 ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT :
                                    st == 5 ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        for (int i = 0; i < pv->sh[st].ndesc; i++) {
            if (pv->sh[st].desc[i].set != 0) {
                LOG("[RED] [nvn/vk] shader %s uses descriptor set %u; the replay binds set 0 only", pv->sh[st].key, pv->sh[st].desc[i].set);
                pv->state = -1; return NULL;
            }
            VkDescriptorSetLayoutBinding *x = &b[nb++];
            memset(x, 0, sizeof *x);
            x->binding = pv->sh[st].desc[i].binding;
            x->descriptorType = pv->sh[st].desc[i].type;
            x->descriptorCount = pv->sh[st].desc[i].count ? pv->sh[st].desc[i].count : 1;
            x->stageFlags = flags;
            if (nb == (int)(sizeof b / sizeof *b)) break;
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
    blend_st bl = adv_blend(g_st.blend);
    VkPipelineColorBlendAttachmentState cba[8] = {{0}};
    for (int i = 0; i < ncolor; i++) {
        int on = (g_st.color.blend_en >> i) & 1;
        cba[i].blendEnable = (VkBool32)(!logic && on);   // a logic operation takes the place of blending
        cba[i].srcColorBlendFactor = vkbfac(bl.src_rgb);
        cba[i].dstColorBlendFactor = vkbfac(bl.dst_rgb);
        cba[i].colorBlendOp = vkbeq(bl.eq_rgb);
        cba[i].srcAlphaBlendFactor = vkbfac(bl.src_a);
        cba[i].dstAlphaBlendFactor = vkbfac(bl.dst_a);
        cba[i].alphaBlendOp = vkbeq(bl.eq_a);
        cba[i].colorWriteMask = (VkColorComponentFlags)((g_st.chmask.mask >> (4 * i)) & 0xf);
    }
    VkPipelineColorBlendStateCreateInfo cbs = {0};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = (uint32_t)ncolor; cbs.pAttachments = ncolor ? cba : NULL;
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

static void drv_fill(uint8_t *d, int bank, int st) {
    memset(d, 0, DMSH_DRV_SIZE);
    if (bank == 0) {
        static const unsigned bases[6] = {DMSH_C0_SSBO_VS, DMSH_C0_SSBO_CS, DMSH_C0_SSBO_GS, 0, 0, DMSH_C0_SSBO_CS};
        unsigned base = st < 6 ? bases[st] : 0;
        if (st == 5) {
            memcpy(d + DMSH_C0_DISPATCH, g_st.dispatch, sizeof g_st.dispatch);
            for (int r = 0; r < DMSH_C0_CS_UBO_SLOTS; r++) {
                uint64_t a = g_st.ubo[5][r].addr; uint32_t n = (uint32_t)g_st.ubo[5][r].size;
                memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r, &a, 8);
                memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r + 8, &n, 4);
            }
        }
        if (base) for (int r = 0; r < DMSH_C0_SSBO_SLOTS; r++) {
            uint64_t a = g_st.ssbo[st][r].addr; uint32_t n = (uint32_t)g_st.ssbo[st][r].size;
            memcpy(d + base + DMSH_C0_ROW * r, &a, 8);
            memcpy(d + base + DMSH_C0_ROW * r + 8, &n, 4);
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
        }
        for (int r = 0; r < 8; r++) {
            uint64_t h = g_st.img[st][r];
            if (!h) continue;
            uint64_t v = (h & 0xfffff) + (uint64_t)g_st.tex_base;
            unsigned off = DMSH_C2_IMG + DMSH_C2_ROW * (unsigned)r;
            if (off + 8 <= DMSH_DRV_SIZE) memcpy(d + off, &v, 4);
        }
        for (int r = 0; r < 128; r++) {
            uint64_t h = g_st.samp[st][r];
            if (!h) continue;
            uint64_t v = (h & 0xfffff) + (uint64_t)g_st.samp_base;
            unsigned off = DMSH_C2_SAMP + DMSH_C2_ROW * (unsigned)r;
            if (off + 8 <= DMSH_DRV_SIZE) memcpy(d + off, &v, 4);
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
                     int *nw, VkDescriptorSet set, const nvn_desc_bind *d, hbuf *buffers, int *nbuf,
                     VkBuffer buf, VkDeviceSize range, VkImageView img, VkSampler samp, VkBufferView tbuf) {
    if (d->count > 16) { LOG("[RED] [nvn/vk] descriptor array %u is too large", d->count); return -1; }
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
        for (uint32_t i = 0; i < x->descriptorCount; i++) { bi[wi * 16 + i].buffer = buf; bi[wi * 16 + i].offset = 0; bi[wi * 16 + i].range = range; }
        x->pBufferInfo = &bi[wi * 16];
    } else {
        for (uint32_t i = 0; i < x->descriptorCount; i++) {
            ii[wi * 16 + i].sampler = samp; ii[wi * 16 + i].imageView = img;
            ii[wi * 16 + i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        x->pImageInfo = &ii[wi * 16];
    }
    (void)buffers; (void)nbuf;
    (*nw)++;
    return 0;
}

// Build and submit one draw. Indexed draws take their indices from the guest.
static void nvkd_draw_common(const prog_t *progp, int indexed, const p_draw *da, const p_draw_idx *di) {
    if ((!indexed && (da->ind || da->mdi_cnt)) || (indexed && (di->ind || di->mdi_cnt))) {
        warn_once(&g_draw_warned, "NVN indirect or multi-draw arguments");
        return;
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
    g_draw_topo = vktopo(da ? da->prim : di->prim);
    int prim = da ? da->prim : di->prim;
    if (prim == NVN_DRAW_PRIMITIVE_QUADS || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP || prim == NVN_DRAW_PRIMITIVE_POLYGON ||
        prim == NVN_DRAW_PRIMITIVE_LINE_LOOP) {
        warn_once(&g_draw_warned, "NVN quad/polygon/line-loop primitive replay"); return;
    }
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
        b.image = ce[i]->img; b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        ce[i]->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (de && de->layout != VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier b = {0};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = de->layout; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = de->img; b.subresourceRange = (VkImageSubresourceRange){nvn_view_aspect(g_rt.depth->d.format), 0, 1, 0, 1};
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
    uint32_t total = 0;
    for (int st = 0; st < 6; st++) if (pv->have[st]) total += (uint32_t)pv->sh[st].ndesc;
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

    VkWriteDescriptorSet wr[160]; VkDescriptorBufferInfo bi[160 * 16]; VkDescriptorImageInfo ii[160 * 16]; VkBufferView bvs[160];
    VkBufferView tbufs[16]; int ntbuf = 0;
    int nw = 0;
    hbuf hb[160]; int nhb = 0;
    int ok = 1;
    for (int st = 0; st < 6 && ok; st++) if (pv->have[st]) {
        nvn_shader *sh = &pv->sh[st];
        for (int k = 0; k < sh->ndesc && ok; k++) {
            nvn_desc_bind *d = &sh->desc[k];
            const nvn_refl_bind *r = d->refl >= 0 ? &sh->rb[d->refl] : NULL;
            VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize range = VK_WHOLE_SIZE; VkImageView iv = VK_NULL_HANDLE; VkSampler sm = VK_NULL_HANDLE;
            if (!r) { LOG("[RED] [nvn/vk] shader %s binding %u has no reflection entry", sh->key, d->binding); ok = 0; break; }
            if (d->count > 1) { LOG("[RED] [nvn/vk] shader %s binding %u is an array of %u descriptors; descriptor arrays are not replayed yet", sh->key, d->binding, d->count); ok = 0; break; }
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
                drv_fill(tmp, r->bank, st);
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
                if (ntbuf < 16) tbufs[ntbuf++] = tv;
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
                if (ntbuf < 16) tbufs[ntbuf++] = tv; else { vkDestroyBufferView(g_vk.device, tv, NULL); ok = 0; break; }
            } else if (!strcmp(kind, "tex_ubo")) {
                uint32_t h = 0;
                if (ubo_handle(st, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] bindless texture handle at stage %d c[%d]+%#x is not in a bound uniform buffer", st, r->reg, r->off); ok = 0; break; }
                int tid = (int)(h & 0xfffff) + g_st.tex_base;
                int sid = 0;
                if (r->sreg != -1) { if (bindless_sampler(st, r->sreg, r->soff, &sid) != 0) { LOG("[RED] [nvn/vk] bindless sampler beside texture id %d is not readable", tid); ok = 0; break; } }
                else sid = (int)(h >> 20) + g_st.samp_base;
                tex_ent *te = texid_image(tid, &iv);
                if (!te) { LOG("[RED] [nvn/vk] bindless texture id %d is not registered", tid); ok = 0; break; }
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
            } else if (!strcmp(kind, "samp_id")) {
                sm = sampler_get(r->reg);
            } else {
                LOG("[RED] [nvn/vk] binding kind '%s' is not replayed yet (program %llu)", kind, (unsigned long long)pv->serial);
                ok = 0; break;
            }
            VkBufferView tv = (ntbuf && !strcmp(kind, "tbuf")) ? tbufs[ntbuf - 1] : VK_NULL_HANDLE;
            if (add_write(wr, bi, ii, bvs, &nw, set, d, hb, &nhb, buf, range, iv, sm, tv) != 0) { ok = 0; break; }
        }
    }
    if (ok && nw && nx_env_bool("NX_VK_DEBUG", 0)) {
        for (int q = 0; q < nw; q++)
            LOG("[nvn/vk] desc %d: set %p binding %u type %d count %u img %p buf %p tbuf %p", q, (void *)wr[q].dstSet, wr[q].dstBinding,
                (int)wr[q].descriptorType, wr[q].descriptorCount, (void *)(wr[q].pImageInfo ? wr[q].pImageInfo->imageView : NULL),
                (void *)(wr[q].pBufferInfo ? wr[q].pBufferInfo->buffer : NULL), (void *)wr[q].pTexelBufferView);
    }
    if (ok && nw) vkUpdateDescriptorSets(g_vk.device, (uint32_t)nw, wr, 0, NULL);

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
    if (ok && indexed) {
        uint64_t count = (uint64_t)di->count, isz = di->itype == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 1;
        if (!di->ibuf || !count) { warn_once(&g_draw_warned, "NVN indexed draw without indices"); ok = 0; }
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
        if (indexed) {
            VkIndexType it = di->itype == NVN_INDEX_TYPE_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
            vkCmdBindIndexBuffer(cb, ib.buf, 0, it);
            uint32_t inst = di->instances ? (uint32_t)di->instances : 1;
            vkCmdDrawIndexed(cb, (uint32_t)di->count, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        } else {
            uint32_t inst = da->instances ? (uint32_t)da->instances : 1;
            vkCmdDraw(cb, (uint32_t)da->count, inst, (uint32_t)da->first, (uint32_t)da->base_instance);
        }
        vkCmdEndRenderPass(cb);
        for (int i = 0; i < ncolor; i++) ce[i]->gpu_dirty = 1;
    }
    nx_vk_oneshot_end(&g_vk, cb);
    for (int i = 0; i < ntbuf; i++) vkDestroyBufferView(g_vk.device, tbufs[i], NULL);
    for (int i = 0; i < nhb; i++) hbuf_free(&hb[i]);
    for (int i = 0; i < nvb; i++) hbuf_free(&vb[i]);
    if (has_ib) hbuf_free(&ib);
    if (pool) vkDestroyDescriptorPool(g_vk.device, pool, NULL);
    vkDestroyFramebuffer(g_vk.device, fb, NULL);
}

// Compute dispatch: a compute pipeline from the program's stage 5, the same
// descriptor mapping as a draw, and a copy-back of the storage buffers the
// shader wrote so the guest sees the results.
typedef struct { hbuf *b; uint64_t addr; size_t size; } ssbo_out;
static void nvkd_dispatch(const p_dispatch *d) {
    prog_vk *pv = pvk_get(g_bound_prog);
    if (!pv) return;
    if (!pv->have[5]) { warn_once(&g_dispatch_warned, "NVN compute dispatch replay"); return; }
    if (d->ind) { warn_once(&g_dispatch_warned, "NVN indirect compute dispatch"); return; }
    if (!d->x || !d->y || !d->z) { warn_once(&g_dispatch_warned, "NVN compute dispatch with a zero group count"); return; }
    g_st.dispatch[0] = (uint32_t)d->x; g_st.dispatch[1] = (uint32_t)d->y; g_st.dispatch[2] = (uint32_t)d->z;
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
    VkDescriptorPoolSize ps[8] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)sh->ndesc + 8}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)sh->ndesc + 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)sh->ndesc + 8}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (uint32_t)sh->ndesc + 8},
        {VK_DESCRIPTOR_TYPE_SAMPLER, (uint32_t)sh->ndesc + 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)sh->ndesc + 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, (uint32_t)sh->ndesc + 8}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, (uint32_t)sh->ndesc + 8},
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
    VkWriteDescriptorSet wr[128]; VkDescriptorBufferInfo bi[128 * 16]; VkDescriptorImageInfo ii[128 * 16]; VkBufferView bvs[128];
    VkBufferView tbufs[16]; int ntbuf = 0;
    hbuf hb[128]; int nhb = 0, nw = 0, ok = 1;
    ssbo_out outs[32]; int nout = 0;
    tex_ent *images[32]; int nimg = 0;
    for (int k = 0; k < sh->ndesc && ok; k++) {
        nvn_desc_bind *db = &sh->desc[k];
        const nvn_refl_bind *r = db->refl >= 0 ? &sh->rb[db->refl] : NULL;
        if (!r) { LOG("[RED] [nvn/vk] compute binding %u has no reflection entry", db->binding); ok = 0; break; }
        if (db->count > 1) { LOG("[RED] [nvn/vk] compute binding %u is an array of %u descriptors; descriptor arrays are not replayed yet", db->binding, db->count); ok = 0; break; }
        const char *kind = r->kind;
        VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize range = VK_WHOLE_SIZE; VkImageView iv = VK_NULL_HANDLE; VkSampler sm = VK_NULL_HANDLE;
        if (!strcmp(kind, "c1")) {
            uint8_t zero[16] = {0};
            hbuf *b = &hb[nhb];
            if (hbuf_make(b, sh->c1_len ? sh->c1 : zero, sh->c1_len ? sh->c1_len : 16) != 0) { ok = 0; break; }
            nhb++; buf = b->buf;
        } else if (!strcmp(kind, "drvcb")) {
            uint8_t *tmp = malloc(DMSH_DRV_SIZE);
            if (!tmp) { ok = 0; break; }
            drv_fill(tmp, r->bank, 5);
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
            if (nout < 32) outs[nout++] = (ssbo_out){b, g_st.ssbo[5][slot].addr, n};
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
            if (ntbuf < 16) tbufs[ntbuf++] = tv; else { vkDestroyBufferView(g_vk.device, tv, NULL); ok = 0; break; }
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
            if (ntbuf < 16) tbufs[ntbuf++] = tv; else { vkDestroyBufferView(g_vk.device, tv, NULL); ok = 0; break; }
        } else if (!strcmp(kind, "tex_ubo")) {
            uint32_t h = 0;
            if (ubo_handle(5, r->reg, r->off, &h) != 0) { LOG("[RED] [nvn/vk] compute bindless texture handle at c[%d]+%#x is not in a bound uniform buffer", r->reg, r->off); ok = 0; break; }
            int tid = (int)(h & 0xfffff) + g_st.tex_base;
            int sid = 0;
            if (r->sreg != -1) { if (bindless_sampler(5, r->sreg, r->soff, &sid) != 0) { LOG("[RED] [nvn/vk] compute bindless sampler beside texture id %d is not readable", tid); ok = 0; break; } }
            else sid = (int)(h >> 20) + g_st.samp_base;
            tex_ent *te = texid_image(tid, &iv);
            if (!te) { LOG("[RED] [nvn/vk] compute bindless texture id %d is not registered", tid); ok = 0; break; }
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
                if (nimg < 32) images[nimg++] = te;
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
            if (nimg < 32) images[nimg++] = te;
        } else if (!strcmp(kind, "samp_id")) {
            sm = sampler_get(r->reg);
        } else {
            LOG("[RED] [nvn/vk] compute binding kind '%s' is not replayed yet", kind);
            ok = 0; break;
        }
        VkBufferView tv = (ntbuf && !strcmp(kind, "tbuf")) ? tbufs[ntbuf - 1] : VK_NULL_HANDLE;
        if (add_write(wr, bi, ii, bvs, &nw, set, db, hb, &nhb, buf, range, iv, sm, tv) != 0) { ok = 0; break; }
    }
    for (int i = 0; i < nimg; i++) if (images[i]->layout != VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier b = {0};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = images[i]->layout; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[i]->img;
        b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        images[i]->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
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
        vkCmdDispatch(cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
        if (nx_env_bool("NX_VK_DEBUG", 0)) LOG("[nvn/vk] dispatch %d %d %d, program %llu", d->x, d->y, d->z, (unsigned long long)pv->serial);
    }
    nx_vk_oneshot_end(&g_vk, cb);
    for (int i = 0; i < nout; i++) if (outs[i].b->map) memcpy((void *)(uintptr_t)outs[i].addr, outs[i].b->map, outs[i].size);
    for (int i = 0; i < nimg; i++) images[i]->gpu_dirty = 1;
    for (int i = 0; i < ntbuf; i++) vkDestroyBufferView(g_vk.device, tbufs[i], NULL);
    for (int i = 0; i < nhb; i++) hbuf_free(&hb[i]);
    vkDestroyDescriptorPool(g_vk.device, pool, NULL);
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
static void copy_transition(VkCommandBuffer cb, tex_ent *e, VkImageLayout to, VkPipelineStageFlags stage, VkAccessFlags access) {
    if (e->layout == to) return;
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = e->layout; b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = e->img; b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = access;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, stage, 0, 0, NULL, 0, NULL, 1, &b);
    e->layout = to;
}
static void copy_b2t(const p_copy_b2t *c, uint32_t rec_size) {
    if (!c || !c->tex || !c->src) return;
    size_t bpp = 0; VkFormat vf = VK_FORMAT_UNDEFINED;
    if (copy_format(c->tex, &vf, &bpp) != 0) { warn_once(&g_draw_warned, "NVN buffer-to-texture copy of a block-compressed format"); return; }
    tex_ent *e = tex_ensure(c->tex);
    if (!e) return;
    uint32_t w = (uint32_t)(c->r.width > 0 ? c->r.width : (int)c->tex->d.width);
    uint32_t h = (uint32_t)(c->r.height > 0 ? c->r.height : (int)c->tex->d.height);
    size_t row = (size_t)w * bpp, bytes = row * h;
    if (!bytes) return;
    const p_copy_strides *cs = copy_strides(c, rec_size);
    size_t pitch = cs && cs->row > 0 ? (size_t)cs->row : row;
    hbuf staging;
    if (hbuf_make(&staging, NULL, bytes) != 0) return;
    if (pitch == row) memcpy(staging.map, (const void *)(uintptr_t)c->src, bytes);
    else for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)staging.map + (size_t)y * row, (const uint8_t *)(uintptr_t)c->src + (size_t)y * pitch, row);
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) { hbuf_free(&staging); return; }
    copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r = {0};
    r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageOffset = (VkOffset3D){c->r.xoffset > 0 ? c->r.xoffset : 0, c->r.yoffset > 0 ? c->r.yoffset : 0, 0};
    r.imageExtent = (VkExtent3D){w, h, 1};
    vkCmdCopyBufferToImage(cb, staging.buf, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    nx_vk_oneshot_end(&g_vk, cb);
    hbuf_free(&staging);
    e->gpu_dirty = 1;
}
static void copy_t2b(const p_copy_b2t *c, uint32_t rec_size) {
    if (!c || !c->tex || !c->src) return;
    size_t bpp = 0; VkFormat vf = VK_FORMAT_UNDEFINED;
    if (copy_format(c->tex, &vf, &bpp) != 0) { warn_once(&g_draw_warned, "NVN texture-to-buffer copy of a block-compressed format"); return; }
    tex_ent *e = tex_ensure(c->tex);
    if (!e) return;
    if (!e->gpu_dirty && upload_guest_texture(c->tex, e) != 0) return;
    uint32_t w = (uint32_t)(c->r.width > 0 ? c->r.width : (int)c->tex->d.width);
    uint32_t h = (uint32_t)(c->r.height > 0 ? c->r.height : (int)c->tex->d.height);
    size_t row = (size_t)w * bpp, bytes = row * h;
    hbuf staging;
    if (hbuf_make(&staging, NULL, bytes) != 0) return;
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) { hbuf_free(&staging); return; }
    copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy r = {0};
    r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageOffset = (VkOffset3D){c->r.xoffset > 0 ? c->r.xoffset : 0, c->r.yoffset > 0 ? c->r.yoffset : 0, 0};
    r.imageExtent = (VkExtent3D){w, h, 1};
    vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buf, 1, &r);
    VkBufferMemoryBarrier bb = {0};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = staging.buf; bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
    copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    nx_vk_oneshot_end(&g_vk, cb);
    const p_copy_strides *cs = copy_strides(c, rec_size);
    size_t pitch = cs && cs->row > 0 ? (size_t)cs->row : row;
    if (staging.map) {
        if (pitch == row) memcpy((void *)(uintptr_t)c->src, staging.map, bytes);
        else for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)(uintptr_t)c->src + (size_t)y * pitch, (uint8_t *)staging.map + (size_t)y * row, row);
    }
    hbuf_free(&staging);
}
static void copy_t2t(const p_copy_t2t *c) {
    if (!c || !c->src || !c->dst) return;
    size_t sbpp = 0, dbpp = 0; VkFormat sf = VK_FORMAT_UNDEFINED, df = VK_FORMAT_UNDEFINED;
    if (copy_format(c->src, &sf, &sbpp) != 0 || copy_format(c->dst, &df, &dbpp) != 0 || sbpp != dbpp) {
        warn_once(&g_draw_warned, "NVN texture-to-texture copy of differing or block-compressed formats"); return;
    }
    tex_ent *s = tex_ensure(c->src), *d = tex_ensure(c->dst);
    if (!s || !d) return;
    int resolve = (c->flags & DM_COPY_RESOLVE) != 0;
    if (!resolve && (s->samples > 1 || d->samples > 1)) { warn_once(&g_draw_warned, "NVN copy involving a multisampled texture"); return; }
    if (!resolve && !s->gpu_dirty && upload_guest_texture(c->src, s) != 0) return;
    uint32_t w = (uint32_t)(c->sr.width > 0 ? c->sr.width : (int)c->src->d.width);
    uint32_t h = (uint32_t)(c->sr.height > 0 ? c->sr.height : (int)c->src->d.height);
    uint32_t dw = (uint32_t)(c->dr.width > 0 ? c->dr.width : (int)c->dst->d.width);
    uint32_t dh = (uint32_t)(c->dr.height > 0 ? c->dr.height : (int)c->dst->d.height);
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) return;
    copy_transition(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    copy_transition(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy r = {0};
    r.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.srcOffset = (VkOffset3D){c->sr.xoffset > 0 ? c->sr.xoffset : 0, c->sr.yoffset > 0 ? c->sr.yoffset : 0, 0};
    r.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstOffset = (VkOffset3D){c->dr.xoffset > 0 ? c->dr.xoffset : 0, c->dr.yoffset > 0 ? c->dr.yoffset : 0, 0};
    r.extent = (VkExtent3D){w < dw ? w : dw, h < dh ? h : dh, 1};
    if (resolve) {
        VkImageResolve rr = {0};
        rr.srcSubresource = r.srcSubresource; rr.srcOffset = r.srcOffset;
        rr.dstSubresource = r.dstSubresource; rr.dstOffset = r.dstOffset;
        rr.extent = r.extent;
        vkCmdResolveImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
    } else {
        vkCmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    copy_transition(cb, s, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    copy_transition(cb, d, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    nx_vk_oneshot_end(&g_vk, cb);
    d->gpu_dirty = 1;
}

// nvnCommandBufferClearTexture: a full-level clear goes through Vulkan's
// transfer clear. A partial clear needs a render pass or shader; it reports
// rather than clearing the whole target.
static void clear_tex_record(const p_clear_tex *c) {
    if (!c || !c->tex) return;
    tex_ent *e = tex_ensure(c->tex);
    if (!e) return;
    uint32_t w = c->tex->d.width ? c->tex->d.width : 1, h = c->tex->d.height ? c->tex->d.height : 1;
    if (c->r.xoffset || c->r.yoffset || c->r.zoffset || (uint32_t)c->r.width != w || (uint32_t)c->r.height != h) {
        warn_once(&g_draw_warned, "NVN partial texture clear"); return;
    }
    VkCommandBuffer cb = nx_vk_oneshot_begin(&g_vk);
    if (!cb) return;
    copy_transition(cb, e, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c->tex->d.levels ? c->tex->d.levels : 1, 0, 1};
    if (c->is_uint) {
        VkClearColorValue v; memcpy(v.uint32, c->c.u, sizeof v.uint32);
        vkCmdClearColorImage(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    } else {
        VkClearColorValue v; memcpy(v.float32, c->c.f, sizeof v.float32);
        vkCmdClearColorImage(cb, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    }
    copy_transition(cb, e, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    nx_vk_oneshot_end(&g_vk, cb);
    e->gpu_dirty = 1;
}
