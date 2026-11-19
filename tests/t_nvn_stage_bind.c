// NVN binds programs per stage: BindProgram(program, stageBits) sets only the
// stages in its mask, and a game may keep the vertex and fragment shaders in
// separate NVNprogram objects (Unity does). The replay must combine the bound
// per-stage programs for a draw; this test binds one program with only a
// vertex shader and another with only a fragment shader.
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
extern void mtl_read_texels(const void *texture, unsigned level, unsigned layer,
                            const NVNcopyRegion *region, uint8_t *dst,
                            size_t row_stride, size_t image_stride);
#define NVNF(name) ((NVNT_##name)dm_nvn_test_lookup(#name))
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    void *lib = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("SKIP: libshaderc is not installed\n"); return 0; }
    void *(*ci)(void) = (void *)dlsym(lib, "shaderc_compiler_initialize");
    void *(*oi)(void) = (void *)dlsym(lib, "shaderc_compile_options_initialize");
    void *(*co)(void *, const char *, size_t, int, const char *, const char *, void *) = (void *)dlsym(lib, "shaderc_compile_into_spv");
    const uint32_t *(*by)(void *) = (void *)dlsym(lib, "shaderc_result_get_bytes");
    size_t (*le)(void *) = (void *)dlsym(lib, "shaderc_result_get_length");
    int (*st)(void *) = (void *)dlsym(lib, "shaderc_result_get_compilation_status");
    if (!ci || !oi || !co || !by || !le || !st) { printf("SKIP: shaderc entry points are missing\n"); return 0; }

    char dir[] = "/tmp/opencode/nvn-stage-XXXXXX";
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
    void *vr = co(ci(), vs_src, strlen(vs_src), 0, "t.vert", "main", oi());
    void *fr = co(ci(), fs_src, strlen(fs_src), 1, "t.frag", "main", oi());
    if (!vr || !fr || st(vr) != 0 || st(fr) != 0) { printf("FAIL: shaderc compile\n"); return 1; }
    vsn = le(vr); vsp = malloc(vsn); memcpy(vsp, by(vr), vsn);
    fsn = le(fr); fsp = malloc(fsn); memcpy(fsp, by(fr), fsn);
    // SHA-256 of 0x880 zero bytes, and of the same with byte 0 = 1: the keys
    // prog_key gives the zeroed control sections.
    const char *vkey = "5626d9a8f1d7364b", *fkey = "b92a56c3b25472de";
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, vkey);
    if (!(f = fopen(path, "wb")) || fwrite(vsp, 1, vsn, f) != vsn) { printf("FAIL: write vs\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_b.json", dir, vkey);
    const char *vsjson = "{\"stage\":\"vs\",\"entry\":\"main\",\"bindings\":[],\"vertex_inputs\":[{\"location\":0,\"msl_type\":\"float2\"}],\"raster\":{\"colors\":[0]}}\n";
    if (!(f = fopen(path, "wb")) || fwrite(vsjson, 1, strlen(vsjson), f) != strlen(vsjson)) { printf("FAIL: write vs json\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, fkey);
    if (!(f = fopen(path, "wb")) || fwrite(fsp, 1, fsn, f) != fsn) { printf("FAIL: write fs\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_b.json", dir, fkey);
    const char *fsjson = "{\"stage\":\"ps\",\"entry\":\"main\",\"bindings\":[],\"vertex_inputs\":[],\"raster\":{\"colors\":[0]}}\n";
    if (!(f = fopen(path, "wb")) || fwrite(fsjson, 1, strlen(fsjson), f) != strlen(fsjson)) { printf("FAIL: write fs json\n"); return 1; }
    fclose(f);

    int g_fail = 0;
    NVNdeviceBuilder db; NVNdevice dev;
    NVNF(nvnDeviceBuilderSetDefaults)(&db); NVNF(nvnDeviceBuilderSetFlags)(&db, 0);
    CHECK(NVNF(nvnDeviceInitialize)(&dev, &db) == 1, "device initializes");
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    static uint8_t storage[1 << 20] __attribute__((aligned(4096)));
    memset(storage, 0, sizeof storage);
    NVNF(nvnMemoryPoolBuilderSetDefaults)(&pb); NVNF(nvnMemoryPoolBuilderSetDevice)(&pb, &dev);
    NVNF(nvnMemoryPoolBuilderSetStorage)(&pb, storage, sizeof storage);
    CHECK(NVNF(nvnMemoryPoolInitialize)(&pool, &pb) == 1, "memory pool initializes");
    NVNqueueBuilder qb; NVNqueue q;
    static uint8_t qmem[1 << 20] __attribute__((aligned(4096)));
    NVNF(nvnQueueBuilderSetDefaults)(&qb); NVNF(nvnQueueBuilderSetDevice)(&qb, &dev);
    size_t qsz = NVNF(nvnQueueBuilderGetQueueMemorySize)(&qb);
    NVNF(nvnQueueBuilderSetQueueMemory)(&qb, qmem, qsz);
    CHECK(NVNF(nvnQueueInitialize)(&q, &qb) == 1, "queue initializes");

    NVNtextureBuilder tb; NVNtexture target;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 32, 32);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
    CHECK(NVNF(nvnTextureInitialize)(&target, &tb) == 1, "render target initializes");

    // One program with only a vertex shader, one with only a fragment shader.
    static uint8_t ctl_vs[0x880], ctl_ps[0x880];
    ctl_ps[0] = 1;
    NVNprogram prog_vs, prog_ps;
    CHECK(NVNF(nvnProgramInitialize)(&prog_vs, &dev) == 1, "vertex program initializes");
    NVNshaderData vsd = {0, ctl_vs};
    CHECK(NVNF(nvnProgramSetShaders)(&prog_vs, 1, &vsd) == 1, "vertex program takes one stage");
    CHECK(NVNF(nvnProgramInitialize)(&prog_ps, &dev) == 1, "fragment program initializes");
    NVNshaderData fsd = {0, ctl_ps};
    CHECK(NVNF(nvnProgramSetShaders)(&prog_ps, 1, &fsd) == 1, "fragment program takes one stage");

    float *verts = (float *)(storage + 0x8000);
    verts[0] = -0.5f; verts[1] = -0.5f;
    verts[2] =  0.5f; verts[3] = -0.5f;
    verts[4] =  0.0f; verts[5] =  0.5f;

    const NVNtexture *colors[1] = {&target};
    float black[4] = {0, 0, 0, 1};
    NVNcommandBuffer cb;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb, &dev) == 1, "command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0x10000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb);
    NVNF(nvnCommandBufferSetRenderTargets)(&cb, 1, colors, NULL, NULL, NULL);
    NVNF(nvnCommandBufferSetScissor)(&cb, 0, 0, 32, 32);
    NVNF(nvnCommandBufferClearColor)(&cb, 0, black, 0xf);
    NVNF(nvnCommandBufferBindProgram)(&cb, &prog_vs, NVN_SHADER_STAGE_VERTEX_BIT);
    NVNF(nvnCommandBufferBindProgram)(&cb, &prog_ps, NVN_SHADER_STAGE_FRAGMENT_BIT);
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
    NVNF(nvnCommandBufferBindVertexBuffer)(&cb, 0, (NVNbufferAddress)(uintptr_t)(storage + 0x8000), 24);
    NVNF(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    NVNcommandHandle h = NVNF(nvnCommandBufferEndRecording)(&cb);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &h);
    NVNF(nvnQueueFinish)(&q);

    uint8_t px[32 * 32 * 4];
    NVNcopyRegion all = {0, 0, 0, 32, 32, 1};
    mtl_read_texels(&target, 0, 0, &all, px, 32 * 4, 32 * 32 * 4);
    const uint8_t *corner = px, *mid = px + (16 * 32 + 16) * 4;
    CHECK(mid[0] < 40 && mid[1] > 200 && mid[2] < 40,
          "a vertex-only and a fragment-only program combine for a draw: centre %u %u %u", mid[0], mid[1], mid[2]);
    CHECK(corner[0] < 40 && corner[1] < 40 && corner[2] < 40,
          "the clear outside the triangle stays: corner %u %u %u", corner[0], corner[1], corner[2]);

    printf("\n%s (%d failure%s)\n", g_fail ? "NVN STAGE BIND TEST FAILED" : "NVN STAGE BIND TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
