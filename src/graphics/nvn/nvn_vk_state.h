// State shared by the NVN Vulkan replay's recording block (nvn_vk.c) and its
// draw block (nvn_vk_draw.inc). One command stream at a time, as on NVN.
#pragma once
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include <vulkan/vulkan.h>

typedef struct {
    uint64_t tex[6][256], samp[6][128], img[6][32];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint16_t ubo_set[6];
    va_st attr[32]; int nattr;
    vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    float vp[16][4]; int has_vp;
    uint8_t vp_sw[16][4];              // SetViewportSwizzles XOR the identity; 0 = unchanged
    float line_width;                  // SetLineWidth; 0 until the game sets one
    int sc_x, sc_y, sc_w, sc_h, has_scissor;
    float dr[2];
    blend_st blend; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    int depth_clamp, rast_discard;
    int patch_size;
    int depth_zero;                    // NVN depth mode: 1 = near=0, 0 = near=-w (default)
    p_poly_offset po; int has_po;
    p_prim_restart restart;
    uint8_t push[2][DMSH_PUSH_BLOCKS * DMSH_PUSH_SIZE];
    float blend_color[4], alpha_ref;
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    int tex_base, samp_base;
    uint32_t dispatch[3];
} vk_state;

typedef struct { int id; tex_t t; view_t v; int has_view, gone; } tex_id_ent;
typedef struct { int id; tex_t t; view_t v; int has_view, gone; } img_id_ent;

static vk_state g_st;
static tex_id_ent *g_tids; static int g_ntid, g_ctid;
static img_id_ent *g_imgids; static int g_nimg, g_cimg;
static VkPrimitiveTopology g_draw_topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
static int g_origin_ll = 1;          // device window origin: NVN's default is lower left
static int g_queue_origin_ll = 1;    // raster flip captured when the submitting queue was created

static tex_id_ent *tid_find(int id) {
    for (int i = 0; i < g_ntid; i++) if (g_tids[i].id == id) return &g_tids[i];
    return NULL;
}
static img_id_ent *imgid_find(int id) {
    for (int i = 0; i < g_nimg; i++) if (g_imgids[i].id == id) return &g_imgids[i];
    return NULL;
}
