// NVN texture channel swizzles: a sampled texture reads through the swizzle of
// its registered view, or of its builder when it has no view, as NVN's
// SetSwizzle / SetTextureViewSwizzle ask. The Vulkan replay applies it as the
// sampled image view's component mapping.
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

static int g_fail;

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

static void put_texture(uint8_t *storage, ptrdiff_t off, const uint8_t rgba[4]) {
    memcpy(storage + off, rgba, 4);
}

int main(void) {
    void *lib = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("SKIP: libshaderc is not installed\n"); return 0; }
    char dir[] = "/tmp/opencode/nvn-swz-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("NX_NVN_SHADER_CACHE", dir, 1);
    const char *vs_src =
        "#version 450\n"
        "layout(location=0) in vec2 pos;\n"
        "void main(){ gl_Position=vec4(pos,0,1); }\n";
    const char *fs_src =
        "#version 450\n"
        "layout(set=0,binding=2) uniform sampler2D tex;\n"
        "layout(location=0) out vec4 outColor;\n"
        "void main(){ outColor=texture(tex, vec2(0.5)); }\n";
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
    const char *fsjson = "{\"stage\":\"ps\",\"entry\":\"main\",\"bindings\":[{\"arg_id\":2,\"kind\":\"tex\",\"reg\":0}],\"vertex_inputs\":[],\"raster\":{\"colors\":[0]}}\n";
    if (put(path, fsjson, strlen(fsjson))) { printf("FAIL: write fs json\n"); return 1; }

    NVNdeviceBuilder db; NVNdevice dev;
    NVNF(nvnDeviceBuilderSetDefaults)(&db); NVNF(nvnDeviceBuilderSetFlags)(&db, 0);
    CHECK(NVNF(nvnDeviceInitialize)(&dev, &db) == 1, "device initializes");
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    static uint8_t storage[1 << 20] __attribute__((aligned(4096)));
    memset(storage, 0, sizeof storage);
    NVNF(nvnMemoryPoolBuilderSetDefaults)(&pb); NVNF(nvnMemoryPoolBuilderSetDevice)(&pb, &dev);
    NVNF(nvnMemoryPoolBuilderSetStorage)(&pb, storage, sizeof storage);
    CHECK(NVNF(nvnMemoryPoolInitialize)(&pool, &pb) == 1, "memory pool initializes");
    NVNtexturePool tpool; NVNsamplerPool spool;
    CHECK(NVNF(nvnTexturePoolInitialize)(&tpool, &pool, 0x30000, 32) == 1, "texture pool initializes");
    CHECK(NVNF(nvnSamplerPoolInitialize)(&spool, &pool, 0x38000, 8) == 1, "sampler pool initializes");
    NVNqueueBuilder qb; NVNqueue q;
    static uint8_t qmem[1 << 20] __attribute__((aligned(4096)));
    NVNF(nvnQueueBuilderSetDefaults)(&qb); NVNF(nvnQueueBuilderSetDevice)(&qb, &dev);
    size_t qsz = NVNF(nvnQueueBuilderGetQueueMemorySize)(&qb);
    NVNF(nvnQueueBuilderSetQueueMemory)(&qb, qmem, qsz);
    CHECK(NVNF(nvnQueueInitialize)(&q, &qb) == 1, "queue initializes");

    // Two 1x1 source textures with distinct texels: one registers a view that
    // swaps red and blue, the other carries a builder swizzle.
    const uint8_t texel1[4] = {10, 20, 30, 40}, texel2[4] = {50, 60, 70, 80};
    const uint8_t texel3[4] = {128, 128, 128, 255};
    const uint8_t texel4[4] = {11, 22, 33, 99};   // RGBX8: the X byte is spare
    // Packed 16-bit texels with only the top five bits set: blue in BGR565,
    // red in RGB5 (whose spare alpha bit stays 0 and must read as 1).
    const uint8_t packed_high[2] = {0x00, 0xF8}; const uint8_t packed_all[2] = {0xFF, 0xFF};
    put_texture(storage, 0x1000, texel1);
    put_texture(storage, 0x2000, texel2);
    put_texture(storage, 0x9000, texel3);
    put_texture(storage, 0xa000, texel4);
    memcpy(storage + 0xb000, packed_high, sizeof packed_high);
    NVNtextureBuilder tb; NVNtexture src1, src2, src3, src4, src5;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 1, 1);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x1000);
    CHECK(NVNF(nvnTextureInitialize)(&src1, &tb) == 1, "first source texture initializes");
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x2000);
    NVNF(nvnTextureBuilderSetSwizzle)(&tb, NVN_TEXTURE_SWIZZLE_ZERO, NVN_TEXTURE_SWIZZLE_ONE,
                                      NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_A);
    CHECK(NVNF(nvnTextureInitialize)(&src2, &tb) == 1, "second source texture initializes");
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 1, 1);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x9000);
    CHECK(NVNF(nvnTextureInitialize)(&src3, &tb) == 1, "sRGB-twin source texture initializes");
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 1, 1);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBX8);
    NVNF(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0xa000);
    CHECK(NVNF(nvnTextureInitialize)(&src4, &tb) == 1, "RGBX8 source texture initializes");
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 1, 1);
    NVNF(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_BGR565);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0xb000);
    CHECK(NVNF(nvnTextureInitialize)(&src5, &tb) == 1, "BGR565 source texture initializes");
    NVNtextureView swz_view;
    NVNF(nvnTextureViewSetDefaults)(&swz_view);
    NVNF(nvnTextureViewSetSwizzle)(&swz_view, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_G,
                                   NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_A);
    // The same linear RGBA8 storage read through an sRGB view is decoded.
    NVNtextureView srgb_view;
    NVNF(nvnTextureViewSetDefaults)(&srgb_view);
    NVNF(nvnTextureViewSetFormat)(&srgb_view, NVN_FORMAT_RGBA8_SRGB);
    NVNF(nvnTexturePoolRegisterTexture)(&tpool, 1, &src1, &swz_view);
    NVNF(nvnTexturePoolRegisterTexture)(&tpool, 2, &src2, NULL);
    NVNF(nvnTexturePoolRegisterTexture)(&tpool, 3, &src3, &srgb_view);
    NVNF(nvnTexturePoolRegisterTexture)(&tpool, 4, &src4, NULL);
    NVNF(nvnTexturePoolRegisterTexture)(&tpool, 5, &src5, NULL);

    NVNsamplerBuilder sb; NVNsampler smp;
    NVNF(nvnSamplerBuilderSetDefaults)(&sb); NVNF(nvnSamplerBuilderSetDevice)(&sb, &dev);
    NVNF(nvnSamplerBuilderSetMinMagFilter)(&sb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    NVNF(nvnSamplerBuilderSetLodClamp)(&sb, 0.0f, 0.0f);
    CHECK(NVNF(nvnSamplerInitialize)(&smp, &sb) == 1, "sampler initializes");
    NVNF(nvnSamplerPoolRegisterSampler)(&spool, 1, &smp);

    NVNtextureBuilder rtb; NVNtexture target;
    NVNF(nvnTextureBuilderSetDefaults)(&rtb); NVNF(nvnTextureBuilderSetDevice)(&rtb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&rtb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&rtb, 8, 8);
    NVNF(nvnTextureBuilderSetFormat)(&rtb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetStorage)(&rtb, &pool, 0x3000);
    CHECK(NVNF(nvnTextureInitialize)(&target, &rtb) == 1, "render target initializes");

    static uint8_t ctl_vs[0x880], ctl_ps[0x880];
    ctl_ps[0] = 1;
    NVNprogram prog;
    CHECK(NVNF(nvnProgramInitialize)(&prog, &dev) == 1, "program initializes");
    NVNshaderData sd[2] = {{0, ctl_vs}, {0, ctl_ps}};
    CHECK(NVNF(nvnProgramSetShaders)(&prog, 2, sd) == 1, "program takes the two stages");

    float *verts = (float *)(storage + 0x8000);
    verts[0] = -1.0f; verts[1] = -1.0f;
    verts[2] =  3.0f; verts[3] = -1.0f;
    verts[4] = -1.0f; verts[5] =  3.0f;

    const NVNtexture *colors[1] = {&target};
    float black[4] = {0, 0, 0, 0};
    NVNcopyRegion all = {0, 0, 0, 8, 8, 1};
    uint8_t px[8 * 8 * 4];

    for (int pass = 0; pass < 5; pass++) {
        int id = pass + 1;
        NVNcommandBuffer cb;
        CHECK(NVNF(nvnCommandBufferInitialize)(&cb, &dev) == 1, "swizzle command buffer %d initializes", pass);
        NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0x50000 + pass * 0x10000, 0x10000);
        NVNF(nvnCommandBufferSetTexturePool)(&cb, &tpool);
        NVNF(nvnCommandBufferSetSamplerPool)(&cb, &spool);
        NVNF(nvnCommandBufferBeginRecording)(&cb);
        NVNF(nvnCommandBufferSetRenderTargets)(&cb, 1, colors, NULL, NULL, NULL);
        NVNF(nvnCommandBufferSetScissor)(&cb, 0, 0, 8, 8);
        NVNF(nvnCommandBufferClearColor)(&cb, 0, black, 0xf);
        NVNF(nvnCommandBufferBindProgram)(&cb, &prog, 3);
        NVNF(nvnCommandBufferBindTexture)(&cb, NVN_SHADER_STAGE_FRAGMENT, 0,
                                          (NVNtextureHandle)((uint64_t)id | (UINT64_C(1) << 20)));
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
        mtl_read_texels(&target, 0, 0, &all, px, 8 * 4, 8 * 8 * 4);
        const uint8_t *mid = px + (4 * 8 + 4) * 4;
        if (pass == 0)
            CHECK(mid[0] == 30 && mid[1] == 20 && mid[2] == 10 && mid[3] == 40,
                  "a view swizzled B,G,R,A swaps red and blue: %u %u %u %u", mid[0], mid[1], mid[2], mid[3]);
        else if (pass == 1)
            CHECK(mid[0] == 0 && mid[1] == 255 && mid[2] == 50 && mid[3] == 80,
                  "a builder swizzle ZERO,ONE,R,A reads constant channels: %u %u %u %u", mid[0], mid[1], mid[2], mid[3]);
        else if (pass == 2)
            CHECK(mid[0] >= 50 && mid[0] <= 60 && mid[1] == mid[0] && mid[2] == mid[0] && mid[3] == 255,
                  "an sRGB view decodes the linear texel 128 to about 55: %u %u %u %u", mid[0], mid[1], mid[2], mid[3]);
        else if (pass == 3)
            CHECK(mid[0] == 11 && mid[1] == 22 && mid[2] == 33 && mid[3] == 255,
                  "an RGBX8 texture reads its spare byte as alpha 1: %u %u %u %u", mid[0], mid[1], mid[2], mid[3]);
        else
            CHECK(mid[0] == 0 && mid[1] == 0 && mid[2] == 255 && mid[3] == 255,
                  "BGR565 maps blue to the high bits: %u %u %u %u", mid[0], mid[1], mid[2], mid[3]);
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "NVN VIEW SWIZZLE TEST FAILED" : "NVN VIEW SWIZZLE TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
