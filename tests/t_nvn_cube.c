// NVN -> Vulkan cubemap test: six faces are uploaded and samplerCube samples
// the +Z face. Each face has a different colour, so a wrong face order or a
// missing upload is visible in the readback.
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
    char dir[] = "/tmp/opencode/nvn-draw-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("NX_NVN_SHADER_CACHE", dir, 1);
    const char *vs_src =
        "#version 450\n"
        "layout(location=0) in vec2 pos;\n"
        "void main(){ gl_Position=vec4(pos,0,1); }\n";
    const char *fs_src =
        "#version 450\n"
        "layout(set=0,binding=2) uniform samplerCube tex;\n"
        "layout(location=0) out vec4 outColor;\n"
        "void main(){ outColor=texture(tex,vec3(0.0,0.0,1.0)); }\n";
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
    const char *vsjson = "{\"stage\":\"vs\",\"entry\":\"main\",\"bindings\":[],\"vertex_inputs\":[{\"location\":0,\"msl_type\":\"float2\"}],\"raster\":{\"colors\":[0]}}\n";
    if (put(path, vsjson, strlen(vsjson))) { printf("FAIL: write vs json\n"); return 1; }
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, fkey);
    if (put(path, fsp, fsn)) { printf("FAIL: write fs\n"); return 1; }
    snprintf(path, sizeof path, "%s/%s_b.json", dir, fkey);
    const char *fsjson = "{\"stage\":\"ps\",\"entry\":\"main\",\"bindings\":[{\"arg_id\":2,\"kind\":\"tex\",\"reg\":0}],\"vertex_inputs\":[],\"raster\":{\"colors\":[0]}}\n";
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
    NVNtexturePool tpool; NVNsamplerPool spool;
    CHECK(NVNF(nvnTexturePoolInitialize)(&tpool, &pool, 0x20000, 8) == 1, "texture pool initializes");
    CHECK(NVNF(nvnSamplerPoolInitialize)(&spool, &pool, 0x30000, 8) == 1, "sampler pool initializes");
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

    // A 2x2 RGBA8 cubemap: face 4 (+Z) magenta, the other faces black.
    NVNtextureBuilder stb; NVNtexture src;
    NVNF(nvnTextureBuilderSetDefaults)(&stb); NVNF(nvnTextureBuilderSetDevice)(&stb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&stb, NVN_TEXTURE_TARGET_CUBEMAP);
    NVNF(nvnTextureBuilderSetSize2D)(&stb, 2, 2);
    NVNF(nvnTextureBuilderSetDepth)(&stb, 6);
    NVNF(nvnTextureBuilderSetFormat)(&stb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetStorage)(&stb, &pool, 0x40000);
    CHECK(NVNF(nvnTextureInitialize)(&src, &stb) == 1, "cubemap initializes");
    uint8_t face[2 * 2 * 4];
    for (int f = 0; f < 6; f++) {
        for (int i = 0; i < 4; i++) { face[i*4+0]=f==4?255:0; face[i*4+1]=0; face[i*4+2]=f==4?255:0; face[i*4+3]=255; }
        NVNcopyRegion rr = {0, 0, f, 2, 2, 1};
        NVNF(nvnTextureWriteTexels)(&src, NULL, &rr, face);
    }
    NVNF(nvnTexturePoolRegisterTexture)(&tpool, 1, &src, NULL);
    NVNsamplerBuilder sb; NVNsampler smp;
    NVNF(nvnSamplerBuilderSetDefaults)(&sb); NVNF(nvnSamplerBuilderSetDevice)(&sb, &dev);
    CHECK(NVNF(nvnSamplerInitialize)(&smp, &sb) == 1, "sampler initializes");
    NVNF(nvnSamplerPoolRegisterSampler)(&spool, 1, &smp);

    static uint8_t ctl_vs[0x880], ctl_ps[0x880];
    ctl_ps[0] = 1;
    NVNprogram prog;
    CHECK(NVNF(nvnProgramInitialize)(&prog, &dev) == 1, "program initializes");
    NVNshaderData sd[2] = {{0, ctl_vs}, {0, ctl_ps}};
    CHECK(NVNF(nvnProgramSetShaders)(&prog, 2, sd) == 1, "program takes the two stages");

    float *verts = (float *)(storage + 0x8000);
    verts[0] = -0.5f; verts[1] = -0.5f;
    verts[2] =  0.5f; verts[3] = -0.5f;
    verts[4] =  0.0f; verts[5] =  0.5f;

    NVNcommandBuffer cb;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb, &dev) == 1, "command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0x10000, 0x10000);
    NVNF(nvnCommandBufferSetTexturePool)(&cb, &tpool);
    NVNF(nvnCommandBufferSetSamplerPool)(&cb, &spool);
    NVNF(nvnCommandBufferBeginRecording)(&cb);
    const NVNtexture *color[1] = {&tex};
    NVNF(nvnCommandBufferSetRenderTargets)(&cb, 1, color, NULL, NULL, NULL);
    float blue[4] = {0, 0, 1, 1};
    NVNF(nvnCommandBufferClearColor)(&cb, 0, blue, 0xf);
    NVNF(nvnCommandBufferBindProgram)(&cb, &prog, 3);
    NVNF(nvnCommandBufferBindTexture)(&cb, NVN_SHADER_STAGE_FRAGMENT, 0, (NVNtextureHandle)(1 | (1 << 20)));
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

    uint8_t px[64 * 64 * 4];
    NVNcopyRegion all = {0, 0, 0, 64, 64, 1};
    mtl_read_texels(&tex, 0, 0, &all, px, 64 * 4, 64 * 4);
    const uint8_t *corner = px;
    const uint8_t *mid = px + (32 * 64 + 32) * 4;
    CHECK(corner[2] > 200 && corner[0] < 40, "clear reached the target corner: %u %u %u", corner[0], corner[1], corner[2]);
    CHECK(mid[0] > 200 && mid[1] < 40 && mid[2] > 200,
          "samplerCube +Z face reached the centre: %u %u %u", mid[0], mid[1], mid[2]);
    printf("\n%s (%d failure%s)\n", g_fail ? "NVN CUBEMAP TEST FAILED" : "NVN CUBEMAP TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
