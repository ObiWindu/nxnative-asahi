// NVN -> Vulkan viewport test: NVN's window origin, declared front face,
// viewport swizzles and line width all change what reaches the framebuffer.
// The test renders a clip-space counter-clockwise triangle and checks both
// the picture's direction and which face survives each cull mode. It also
// checks that a wide line covers more than one pixel row.
//
// NVN's facing rule is the same one the Metal host uses: a triangle is front
// facing when its winding after the viewport swizzle is the declared front
// face, independently of the window origin. The shaders are compiled with the
// system shaderc at test time under the keys the zeroed control sections hash
// to.
#include "dm.h"
#include "nvn.h"
#include "nvn_obj.h"
#include "nvn_protos.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern void *dm_nvn_test_lookup(const char *name);
#define NVNF(name) ((NVNT_##name)dm_nvn_test_lookup(#name))
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int compile_shader(void *lib, const char *src, int kind, const char *name, uint8_t **out, size_t *n) {
    void *(*ci)(void) = (void *)dlsym(lib, "shaderc_compiler_initialize");
    void *(*oi)(void) = (void *)dlsym(lib, "shaderc_compile_options_initialize");
    void *(*co)(void *, const char *, size_t, int, const char *, const char *, void *) = (void *)dlsym(lib, "shaderc_compile_into_spv");
    const uint32_t *(*by)(void *) = (void *)dlsym(lib, "shaderc_result_get_bytes");
    size_t (*le)(void *) = (void *)dlsym(lib, "shaderc_result_get_length");
    int (*st)(void *) = (void *)dlsym(lib, "shaderc_result_get_compilation_status");
    if (!ci || !oi || !co || !by || !le || !st) return -1;
    void *r = co(ci(), src, strlen(src), kind, name, "main", oi());
    if (!r || st(r) != 0) return -1;
    *n = le(r); *out = malloc(*n);
    if (!*out) return -1;
    memcpy(*out, by(r), *n);
    return 0;
}
static int put(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    int ok = fwrite(p, 1, n, f) == n; fclose(f); return ok ? 0 : -1;
}

static NVNdevice dev;
static NVNmemoryPool pool;
static NVNtexture tex;
static NVNprogram prog;
static float *verts;
static int g_fail;

typedef struct {
    int front_cw;       // declared front face: 0 = CCW, 1 = CW
    int cull;           // 0 none, 1 front, 2 back
    int swizzle_neg_y;  // viewport swizzle (x, -y, z, w)
    int origin_ll;      // window origin lower left
    int line_width;     // > 0 draws a line instead of the triangle
} case_t;

// Render one case and read the target back.
static int run_case(const case_t *c, uint8_t px[64 * 64 * 4]) {
    static uint8_t qmem[16][1 << 20] __attribute__((aligned(4096)));
    static int qslot;
    if (qslot >= 16) return -1;
    NVNF(nvnDeviceSetWindowOriginMode)(&dev,
        c->origin_ll ? NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT : NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    NVNqueueBuilder qb; NVNqueue q;
    NVNF(nvnQueueBuilderSetDefaults)(&qb); NVNF(nvnQueueBuilderSetDevice)(&qb, &dev);
    size_t qsz = NVNF(nvnQueueBuilderGetQueueMemorySize)(&qb);
    if (qsz > sizeof qmem[0]) return -1;
    NVNF(nvnQueueBuilderSetQueueMemory)(&qb, qmem[qslot], qsz);
    if (NVNF(nvnQueueInitialize)(&q, &qb) != 1) return -1;
    qslot++;

    NVNcommandBuffer cb;
    if (NVNF(nvnCommandBufferInitialize)(&cb, &dev) != 1) return -1;
    NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0x10000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb);
    const NVNtexture *color[1] = {&tex};
    NVNF(nvnCommandBufferSetRenderTargets)(&cb, 1, color, NULL, NULL, NULL);
    float blue[4] = {0, 0, 1, 1};
    NVNF(nvnCommandBufferClearColor)(&cb, 0, blue, 0xf);
    NVNF(nvnCommandBufferSetViewport)(&cb, 0, 0, 64, 64);
    NVNF(nvnCommandBufferBindProgram)(&cb, &prog, 3);
    NVNvertexAttribState va;
    NVNF(nvnVertexAttribStateSetDefaults)(&va);
    NVNF(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RG32F, 0);
    NVNF(nvnVertexAttribStateSetStreamIndex)(&va, 0);
    NVNF(nvnCommandBufferBindVertexAttribState)(&cb, 1, &va);
    NVNvertexStreamState vs;
    NVNF(nvnVertexStreamStateSetDefaults)(&vs);
    NVNF(nvnVertexStreamStateSetStride)(&vs, 8);
    NVNF(nvnVertexStreamStateSetDivisor)(&vs, 0);
    NVNF(nvnCommandBufferBindVertexStreamState)(&cb, 1, &vs);
    NVNF(nvnCommandBufferBindVertexBuffer)(&cb, 0, (NVNbufferAddress)(uintptr_t)verts, 24);
    NVNpolygonState poly;
    NVNF(nvnPolygonStateSetDefaults)(&poly);
    NVNF(nvnPolygonStateSetFrontFace)(&poly, c->front_cw ? NVN_FRONT_FACE_CW : NVN_FRONT_FACE_CCW);
    NVNF(nvnPolygonStateSetCullFace)(&poly, c->cull == 1 ? NVN_FACE_FRONT : c->cull == 2 ? NVN_FACE_BACK : NVN_FACE_NONE);
    NVNF(nvnCommandBufferBindPolygonState)(&cb, &poly);
    if (c->swizzle_neg_y) {
        NVNviewportSwizzle sw[4] = {NVN_VIEWPORT_SWIZZLE_POSITIVE_X, NVN_VIEWPORT_SWIZZLE_NEGATIVE_Y,
                                    NVN_VIEWPORT_SWIZZLE_POSITIVE_Z, NVN_VIEWPORT_SWIZZLE_POSITIVE_W};
        NVNF(nvnCommandBufferSetViewportSwizzles)(&cb, 0, 1, sw);
    }
    if (c->line_width > 0) {
        NVNF(nvnCommandBufferSetLineWidth)(&cb, (float)c->line_width);
        NVNF(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_LINES, 0, 2);
    } else {
        NVNF(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    }
    NVNcommandHandle h = NVNF(nvnCommandBufferEndRecording)(&cb);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &h);
    NVNF(nvnQueueFinish)(&q);
    NVNcopyRegion all = {0, 0, 0, 64, 64, 1};
    mtl_read_texels((const tex_t *)&tex, 0, 0, &all, px, 64 * 4, 64 * 4);
    return 0;
}

static int green(const uint8_t *p) { return p[1] > 160 && p[0] < 80 && p[2] < 80; }
static int blue(const uint8_t *p) { return p[2] > 160 && p[0] < 80 && p[1] < 80; }
static const uint8_t *at(const uint8_t *px, int x, int y) { return px + ((size_t)y * 64 + (size_t)x) * 4; }

int main(void) {
    void *lib = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("SKIP: libshaderc is not installed\n"); return 0; }
    char dir[] = "/tmp/opencode/nvn-viewport-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("NX_NVN_SHADER_CACHE", dir, 1);
    const char *vs_src =
        "#version 450\n"
        "layout(location=0) in vec2 pos;\n"
        "void main(){ gl_Position=vec4(pos,0,1); }\n";
    const char *fs_src =
        "#version 450\n"
        "layout(location=0) out vec4 outColor;\n"
        "void main(){ outColor=vec4(0,1,0,1); }\n";
    uint8_t *vsp = NULL, *fsp = NULL; size_t vsn = 0, fsn = 0;
    if (compile_shader(lib, vs_src, 0, "t.vert", &vsp, &vsn) || compile_shader(lib, fs_src, 1, "t.frag", &fsp, &fsn)) {
        printf("FAIL: shaderc compile\n"); return 1;
    }
    const char *vkey = "5626d9a8f1d7364b", *fkey = "b92a56c3b25472de";
    char path[512];
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, vkey);
    if (put(path, vsp, vsn)) { printf("FAIL: write vs\n"); return 1; }
    snprintf(path, sizeof path, "%s/%s_b.json", dir, vkey);
    const char *vsjson = "{\"stage\":\"vs\",\"entry\":\"main\",\"bindings\":[],\"vertex_inputs\":[{\"location\":0,\"msl_type\":\"float2\"}],\"raster\":{\"colors\":[0]}}\n";
    if (put(path, vsjson, strlen(vsjson))) { printf("FAIL: write vs json\n"); return 1; }
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, fkey);
    if (put(path, fsp, fsn)) { printf("FAIL: write fs\n"); return 1; }
    snprintf(path, sizeof path, "%s/%s_b.json", dir, fkey);
    const char *fsjson = "{\"stage\":\"ps\",\"entry\":\"main\",\"bindings\":[],\"vertex_inputs\":[],\"raster\":{\"colors\":[0]}}\n";
    if (put(path, fsjson, strlen(fsjson))) { printf("FAIL: write fs json\n"); return 1; }

    NVNdeviceBuilder db;
    NVNF(nvnDeviceBuilderSetDefaults)(&db); NVNF(nvnDeviceBuilderSetFlags)(&db, 0);
    CHECK(NVNF(nvnDeviceInitialize)(&dev, &db) == 1, "device initializes");
    NVNmemoryPoolBuilder pb;
    static uint8_t storage[4 << 20] __attribute__((aligned(4096)));
    NVNF(nvnMemoryPoolBuilderSetDefaults)(&pb); NVNF(nvnMemoryPoolBuilderSetDevice)(&pb, &dev);
    NVNF(nvnMemoryPoolBuilderSetStorage)(&pb, storage, sizeof storage);
    CHECK(NVNF(nvnMemoryPoolInitialize)(&pool, &pb) == 1, "memory pool initializes");

    NVNtextureBuilder tb;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 64, 64);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
    CHECK(NVNF(nvnTextureInitialize)(&tex, &tb) == 1, "render target texture initializes");

    static uint8_t ctl_vs[0x880], ctl_ps[0x880];
    ctl_ps[0] = 1;
    CHECK(NVNF(nvnProgramInitialize)(&prog, &dev) == 1, "program initializes");
    NVNshaderData sd[2] = {{0, ctl_vs}, {0, ctl_ps}};
    CHECK(NVNF(nvnProgramSetShaders)(&prog, 2, sd) == 1, "program takes the two stages");

    // Clip-space CCW in the y-up convention NVN uses: bottom left, bottom
    // right, top left. The last two vertices are a horizontal line.
    verts = (float *)(storage + 0x8000);
    verts[0] = -0.8f; verts[1] = -0.8f;
    verts[2] =  0.8f; verts[3] = -0.8f;
    verts[4] = -0.8f; verts[5] =  0.8f;
    verts[6] = -0.8f; verts[7] =  0.0f;
    verts[8] =  0.8f; verts[9] =  0.0f;

    uint8_t px[64 * 64 * 4];

    // 1. Declared CCW, culled back: the triangle is front facing and visible.
    case_t c = {0, 2, 0, 0, 0};
    CHECK(run_case(&c, px) == 0, "upper-left CCW/back case renders");
    CHECK(green(at(px, 8, 56)) && blue(at(px, 56, 8)),
          "upper-left origin puts NVN's -y (clip bottom) at the bottom rows: %u %u %u",
          at(px, 8, 56)[0], at(px, 8, 56)[1], at(px, 8, 56)[2]);

    // 2. Declared CCW, culled front: the same triangle is a back face.
    c.cull = 1;
    CHECK(run_case(&c, px) == 0, "upper-left CCW/front case renders");
    CHECK(blue(at(px, 8, 56)) && blue(at(px, 56, 8)),
          "a CCW triangle is culled as a back face when front is CCW: %u %u %u",
          at(px, 8, 56)[0], at(px, 8, 56)[1], at(px, 8, 56)[2]);

    // 3. Declared CW, culled front: now the triangle is front facing.
    c.front_cw = 1;
    CHECK(run_case(&c, px) == 0, "upper-left CW/front case renders");
    CHECK(green(at(px, 8, 56)), "a CW triangle is front facing when front is CW: %u %u %u",
          at(px, 8, 56)[0], at(px, 8, 56)[1], at(px, 8, 56)[2]);

    // 4. Declared CW, culled back: culled.
    c.cull = 2;
    CHECK(run_case(&c, px) == 0, "upper-left CW/back case renders");
    CHECK(blue(at(px, 8, 56)), "a CW triangle is culled as a back face when front is CW");

    // 5. The same facing rule under NVN's lower-left origin, where NDC y=-1
    // lands on the first memory row: the clip-bottom half appears at the top.
    c.front_cw = 0; c.cull = 2; c.origin_ll = 1;
    CHECK(run_case(&c, px) == 0, "lower-left CCW/back case renders");
    CHECK(green(at(px, 8, 8)) && blue(at(px, 56, 56)),
          "lower-left origin keeps the CCW clip triangle front facing: %u %u %u",
          at(px, 8, 8)[0], at(px, 8, 8)[1], at(px, 8, 8)[2]);

    // 6. ... and culling front removes it.
    c.cull = 1;
    CHECK(run_case(&c, px) == 0, "lower-left CCW/front case renders");
    CHECK(blue(at(px, 8, 8)), "lower-left origin culls the CCW triangle as a back face");

    // 7. NEGATIVE_Y negates clip y: the picture flips about the viewport
    // centre and the triangle's post-swizzle winding becomes CW.
    c.origin_ll = 0; c.cull = 0; c.front_cw = 0; c.swizzle_neg_y = 1;
    CHECK(run_case(&c, px) == 0, "negative-Y swizzle case renders");
    CHECK(green(at(px, 8, 8)) && blue(at(px, 8, 56)),
          "negative-Y swizzle mirrors the triangle about the viewport centre: top %u %u %u bottom %u %u %u",
          at(px, 8, 8)[0], at(px, 8, 8)[1], at(px, 8, 8)[2],
          at(px, 8, 56)[0], at(px, 8, 56)[1], at(px, 8, 56)[2]);
    c.cull = 2;
    CHECK(run_case(&c, px) == 0, "negative-Y swizzle with culling renders");
    CHECK(blue(at(px, 8, 8)),
          "facing follows the swizzled winding: a clip-CCW triangle is CW after -y and culled when front is CCW");
    c.front_cw = 1;
    CHECK(run_case(&c, px) == 0, "negative-Y swizzle with CW front renders");
    CHECK(green(at(px, 8, 8)),
          "the swizzled CW triangle is front facing when front is CW: %u %u %u",
          at(px, 8, 8)[0], at(px, 8, 8)[1], at(px, 8, 8)[2]);

    // 8. A wide line covers more than one row; a one-pixel line does not.
    case_t lw = {0, 0, 0, 0, 1};
    CHECK(run_case(&lw, px) == 0, "one-pixel line case renders");
    int thin = 0;
    for (int y = 0; y < 64; y++) if (green(at(px, 32, y))) thin++;
    lw.line_width = 8;
    CHECK(run_case(&lw, px) == 0, "eight-pixel line case renders");
    int wide = 0;
    for (int y = 0; y < 64; y++) if (green(at(px, 32, y))) wide++;
    CHECK(thin >= 1 && thin <= 3 && wide >= 6 && wide <= 12,
          "NVN line width reaches the Vulkan pipeline: 1 px covers %d row(s), 8 px covers %d",
          thin, wide);

    printf("\n%s (%d failure%s)\n", g_fail ? "NVN VIEWPORT TEST FAILED" : "NVN VIEWPORT TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
