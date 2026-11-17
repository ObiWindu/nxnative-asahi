// NVN -> Vulkan advanced-blend test: Porter-Duff and complex blend equations
// must override the state's ordinary blend functions.
// A full-screen triangle writes the case's source colour over the case's clear
// colour; the expected centre is the fixed-function result. A driver rounds a
// blend result to its own precision, so each channel allows one step.
// The shaders are compiled with the system's shaderc at test time and written
// under the keys the zeroed control sections hash to.
#include "dm.h"
#include "nvn.h"
#include "nvn_protos.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern void *dm_nvn_test_lookup(const char *name);
void mtl_read_texels(const void *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
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

int main(void) {
    void *lib = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("SKIP: libshaderc is not installed\n"); return 0; }
    char dir[] = "/tmp/opencode/nvn-advblend-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("NX_NVN_SHADER_CACHE", dir, 1);
    const char *vs_src =
        "#version 450\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(location=1) in vec4 col;\n"
        "layout(location=0) out vec4 vcol;\n"
        "void main(){ gl_Position=vec4(pos,0.0,1.0); vcol=col; }\n";
    const char *fs_src =
        "#version 450\n"
        "layout(location=0) in vec4 vcol;\n"
        "layout(location=0) out vec4 outColor;\n"
        "void main(){ outColor=vcol; }\n";
    uint8_t *vsp = NULL, *fsp = NULL; size_t vsn = 0, fsn = 0;
    if (compile_shader(lib, vs_src, 0, "t.vert", &vsp, &vsn) || compile_shader(lib, fs_src, 1, "t.frag", &fsp, &fsn)) {
        printf("FAIL: shaderc compile\n"); return 1;
    }
    // SHA-256 of 0x880 zero bytes, and of the same with byte 0 = 1: the keys
    // prog_key gives two zeroed control sections that differ in that byte.
    const char *vkey = "5626d9a8f1d7364b", *fkey = "b92a56c3b25472de";
    char path[512];
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, vkey);
    if (put(path, vsp, vsn)) { printf("FAIL: write vs\n"); return 1; }
    snprintf(path, sizeof path, "%s/%s_b.json", dir, vkey);
    const char *vsjson = "{\"stage\":\"vs\",\"entry\":\"main\",\"bindings\":[],\"vertex_inputs\":[{\"location\":0,\"msl_type\":\"float2\"},{\"location\":1,\"msl_type\":\"float4\"}],\"raster\":{\"colors\":[0]}}\n";
    if (put(path, vsjson, strlen(vsjson))) { printf("FAIL: write vs json\n"); return 1; }
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, fkey);
    if (put(path, fsp, fsn)) { printf("FAIL: write fs\n"); return 1; }
    snprintf(path, sizeof path, "%s/%s_b.json", dir, fkey);
    const char *fsjson = "{\"stage\":\"ps\",\"entry\":\"main\",\"bindings\":[],\"vertex_inputs\":[],\"raster\":{\"colors\":[0]}}\n";
    if (put(path, fsjson, strlen(fsjson))) { printf("FAIL: write fs json\n"); return 1; }

    int g_fail = 0;
    NVNdeviceBuilder db; NVNdevice dev;
    NVNF(nvnDeviceBuilderSetDefaults)(&db); NVNF(nvnDeviceBuilderSetFlags)(&db, 0);
    CHECK(NVNF(nvnDeviceInitialize)(&dev, &db) == 1, "device initializes");
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    static uint8_t storage[1 << 20] __attribute__((aligned(4096)));
    NVNF(nvnMemoryPoolBuilderSetDefaults)(&pb); NVNF(nvnMemoryPoolBuilderSetDevice)(&pb, &dev);
    NVNF(nvnMemoryPoolBuilderSetStorage)(&pb, storage, sizeof storage);
    CHECK(NVNF(nvnMemoryPoolInitialize)(&pool, &pb) == 1, "memory pool initializes");
    NVNqueueBuilder qb; NVNqueue q;
    static uint8_t qmem[1 << 20] __attribute__((aligned(4096)));
    NVNF(nvnQueueBuilderSetDefaults)(&qb); NVNF(nvnQueueBuilderSetDevice)(&qb, &dev);
    size_t qsz = NVNF(nvnQueueBuilderGetQueueMemorySize)(&qb);
    NVNF(nvnQueueBuilderSetQueueMemory)(&qb, qmem, qsz);
    CHECK(NVNF(nvnQueueInitialize)(&q, &qb) == 1, "queue initializes");

    NVNtextureBuilder tb; NVNtexture tex;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 64, 64);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
    CHECK(NVNF(nvnTextureInitialize)(&tex, &tb) == 1, "render target texture initializes");

    static uint8_t ctl_vs[0x880], ctl_ps[0x880];
    ctl_ps[0] = 1;
    NVNprogram prog;
    CHECK(NVNF(nvnProgramInitialize)(&prog, &dev) == 1, "program initializes");
    NVNshaderData sd[2] = {{0, ctl_vs}, {0, ctl_ps}};
    CHECK(NVNF(nvnProgramSetShaders)(&prog, 2, sd) == 1, "program takes the two stages");

    struct {
        NVNblendAdvancedMode mode; const char *name; int npm;
        float src[4], clear[4]; uint8_t r, g, b, a;
    } cases[] = {
        {NVN_BLEND_ADVANCED_MODE_ZERO, "ZERO", 0, {1,0,0,1}, {0,0,1,1}, 0, 0, 0, 0},
        {NVN_BLEND_ADVANCED_MODE_SRC, "SRC", 0, {1,0,0,1}, {0,0,1,1}, 255, 0, 0, 255},
        {NVN_BLEND_ADVANCED_MODE_DST, "DST", 0, {1,0,0,1}, {0,0,1,1}, 0, 0, 255, 255},
        {NVN_BLEND_ADVANCED_MODE_SRC_OVER, "SRC_OVER premultiplied", 0, {0.5f,0,0,0.5f}, {0,0,1,1}, 128, 0, 128, 255},
        {NVN_BLEND_ADVANCED_MODE_SRC_OVER, "SRC_OVER not premultiplied", 1, {1,0,0,0.5f}, {0,0,1,1}, 128, 0, 128, 255},
        {NVN_BLEND_ADVANCED_MODE_PLUS, "PLUS", 0, {1,0,0,1}, {0,0,1,1}, 255, 0, 255, 255},
        {NVN_BLEND_ADVANCED_MODE_SRC_IN, "SRC_IN", 0, {1,0,0,1}, {0,0,1,0.5f}, 128, 0, 0, 128},
        {NVN_BLEND_ADVANCED_MODE_MULTIPLY, "MULTIPLY", 0, {1,0,0,1}, {0,0,1,1}, 0, 0, 0, 255},
        {NVN_BLEND_ADVANCED_MODE_SCREEN, "SCREEN", 0, {1,0,0,1}, {0,0,1,1}, 255, 0, 255, 255},
        {NVN_BLEND_ADVANCED_MODE_OVERLAY, "OVERLAY", 0, {1,0,0,1}, {0,0,1,1}, 0, 0, 255, 255},
        {NVN_BLEND_ADVANCED_MODE_DIFFERENCE, "DIFFERENCE", 0, {1,0,0,1}, {0,0,1,1}, 255, 0, 255, 255},
    };
    const int ncase = (int)(sizeof cases / sizeof *cases);
    for (int c = 0; c < ncase; c++) {
        float *verts = (float *)(storage + 0x8000);
        const float pos[3][2] = {{-1,-1}, {1,-1}, {0,1}};
        for (int i = 0; i < 3; i++) {
            verts[i * 6 + 0] = pos[i][0]; verts[i * 6 + 1] = pos[i][1];
            memcpy(verts + i * 6 + 2, cases[c].src, sizeof cases[c].src);
        }
        NVNcommandBuffer cb;
        CHECK(NVNF(nvnCommandBufferInitialize)(&cb, &dev) == 1, "%s: command buffer initializes", cases[c].name);
        ptrdiff_t cmd_off = (ptrdiff_t)(0x10000 + (size_t)c * 0x10000);
        NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, cmd_off, 0x10000);
        NVNF(nvnCommandBufferBeginRecording)(&cb);
        const NVNtexture *color[1] = {&tex};
        NVNF(nvnCommandBufferSetRenderTargets)(&cb, 1, color, NULL, NULL, NULL);
        NVNF(nvnCommandBufferClearColor)(&cb, 0, cases[c].clear, 0xf);
        NVNF(nvnCommandBufferBindProgram)(&cb, &prog, 3);
        NVNblendState bs;
        NVNF(nvnBlendStateSetDefaults)(&bs);
        NVNF(nvnBlendStateSetAdvancedMode)(&bs, cases[c].mode);
        if (cases[c].npm) NVNF(nvnBlendStateSetAdvancedPremultipliedSrc)(&bs, 0);
        NVNF(nvnCommandBufferBindBlendState)(&cb, &bs);
        NVNcolorState cs;
        NVNF(nvnColorStateSetDefaults)(&cs);
        NVNF(nvnColorStateSetBlendEnable)(&cs, 0, 1);
        NVNF(nvnCommandBufferBindColorState)(&cb, &cs);
        NVNvertexAttribState va[2];
        NVNF(nvnVertexAttribStateSetDefaults)(&va[0]);
        NVNF(nvnVertexAttribStateSetFormat)(&va[0], NVN_FORMAT_RG32F, 0);
        NVNF(nvnVertexAttribStateSetStreamIndex)(&va[0], 0);
        NVNF(nvnVertexAttribStateSetDefaults)(&va[1]);
        NVNF(nvnVertexAttribStateSetFormat)(&va[1], NVN_FORMAT_RGBA32F, 8);
        NVNF(nvnVertexAttribStateSetStreamIndex)(&va[1], 0);
        NVNF(nvnCommandBufferBindVertexAttribState)(&cb, 2, va);
        NVNvertexStreamState vs;
        NVNF(nvnVertexStreamStateSetDefaults)(&vs);
        NVNF(nvnVertexStreamStateSetStride)(&vs, 24);
        NVNF(nvnVertexStreamStateSetDivisor)(&vs, 0);
        NVNF(nvnCommandBufferBindVertexStreamState)(&cb, 1, &vs);
        NVNF(nvnCommandBufferBindVertexBuffer)(&cb, 0, (NVNbufferAddress)(uintptr_t)(storage + 0x8000), 72);
        NVNF(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        NVNcommandHandle h = NVNF(nvnCommandBufferEndRecording)(&cb);
        NVNF(nvnQueueSubmitCommands)(&q, 1, &h);
        NVNF(nvnQueueFinish)(&q);

        uint8_t px[64 * 64 * 4];
        NVNcopyRegion all = {0, 0, 0, 64, 64, 1};
        mtl_read_texels(&tex, 0, 0, &all, px, 64 * 4, 64 * 4);
        const uint8_t *mid = px + (32 * 64 + 32) * 4;
        const uint8_t got[4] = {mid[0], mid[1], mid[2], mid[3]};
        const uint8_t want[4] = {cases[c].r, cases[c].g, cases[c].b, cases[c].a};
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            int d = got[i] > want[i] ? got[i] - want[i] : want[i] - got[i];
            ok = ok && d <= 1;
        }
        CHECK(ok, "%s gives %u %u %u %u (expected %u %u %u %u, each within 1)", cases[c].name, mid[0], mid[1], mid[2], mid[3],
              cases[c].r, cases[c].g, cases[c].b, cases[c].a);
    }
    printf("\n%s (%d failure%s)\n", g_fail ? "NVN ADVANCED BLEND TEST FAILED" : "NVN ADVANCED BLEND TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
