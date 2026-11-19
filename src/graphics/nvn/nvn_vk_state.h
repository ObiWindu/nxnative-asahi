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

typedef struct { int id; tex_t t; view_t v; int has_view, gone; int hnext; } tex_id_ent;
typedef struct { int id; tex_t t; view_t v; int has_view, gone; int hnext; } img_id_ent;

static vk_state g_st;
static tex_id_ent *g_tids; static int g_ntid, g_ctid;
static img_id_ent *g_imgids; static int g_nimg, g_cimg;
// Registered textures and images are found by id on every draw; a game with
// thousands of them made the linear scan part of a frame's cost. The maps
// store entry index + 1, so a realloc of the entry array keeps them valid.
static int *g_tid_hash; static int g_tid_hash_n;
static int *g_img_hash; static int g_img_hash_n;
static int id_hash_bucket(int id, int n) { return (int)(((unsigned)id * 2654435761u) & (unsigned)(n - 1)); }
static void tid_hash_rebuild(int n) {
    int *b = calloc((size_t)n, sizeof *b); if (!b) return;
    free(g_tid_hash); g_tid_hash = b; g_tid_hash_n = n;
    for (int i = 0; i < g_ntid; i++) { int h = id_hash_bucket(g_tids[i].id, n); g_tids[i].hnext = b[h]; b[h] = i + 1; }
}
static void img_hash_rebuild(int n) {
    int *b = calloc((size_t)n, sizeof *b); if (!b) return;
    free(g_img_hash); g_img_hash = b; g_img_hash_n = n;
    for (int i = 0; i < g_nimg; i++) { int h = id_hash_bucket(g_imgids[i].id, n); g_imgids[i].hnext = b[h]; b[h] = i + 1; }
}
static tex_id_ent *tid_find(int id);
static img_id_ent *imgid_find(int id);

// Call before marking a new entry live (a rebuild walks the array).
static void tid_hash_ensure(void) { if (!g_tid_hash) tid_hash_rebuild(64); else if (g_ntid > g_tid_hash_n * 2) tid_hash_rebuild(g_tid_hash_n * 2); }
static void img_hash_ensure(void) { if (!g_img_hash) img_hash_rebuild(64); else if (g_nimg > g_img_hash_n * 2) img_hash_rebuild(g_img_hash_n * 2); }
static void tid_hash_put(int ix) { int id = g_tids[ix].id, h = id_hash_bucket(id, g_tid_hash_n); g_tids[ix].hnext = g_tid_hash[h]; g_tid_hash[h] = ix + 1; }
static void img_hash_put(int ix) { int id = g_imgids[ix].id, h = id_hash_bucket(id, g_img_hash_n); g_imgids[ix].hnext = g_img_hash[h]; g_img_hash[h] = ix + 1; }
static VkPrimitiveTopology g_draw_topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
static int g_origin_ll = 1;          // device window origin: NVN's default is lower left
static int g_queue_origin_ll = 1;    // raster flip captured when the submitting queue was created

