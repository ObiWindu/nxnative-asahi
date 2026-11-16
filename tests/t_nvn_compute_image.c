// NVN compute writes a storage image; the host reads the image back. This
// covers STORAGE_IMAGE descriptors (imageStore) on the compute path.
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

int main(void) {
    void *lib = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("SKIP: libshaderc is not installed\n"); return 0; }
    char dir[] = "/tmp/opencode/nvn-compute-img-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("NX_NVN_SHADER_CACHE", dir, 1);
    const char *src =
        "#version 450\n"
        "layout(local_size_x=1,local_size_y=1,local_size_z=1) in;\n"
        "layout(set=0,binding=1,rgba8) uniform image2D img;\n"
        "void main(){ imageStore(img, ivec2(0,0), vec4(0,1,0,1)); }\n";
    void *(*ci)(void) = (void *)dlsym(lib, "shaderc_compiler_initialize");
    void *(*oi)(void) = (void *)dlsym(lib, "shaderc_compile_options_initialize");
    void *(*co)(void *, const char *, size_t, int, const char *, const char *, void *) = (void *)dlsym(lib, "shaderc_compile_into_spv");
    const uint32_t *(*by)(void *) = (void *)dlsym(lib, "shaderc_result_get_bytes");
    size_t (*le)(void *) = (void *)dlsym(lib, "shaderc_result_get_length");
    int (*st)(void *) = (void *)dlsym(lib, "shaderc_result_get_compilation_status");
    if (!ci || !oi || !co || !by || !le || !st) { printf("SKIP: shaderc API incomplete\n"); return 0; }
    void *res = co(ci(), src, strlen(src), 2, "t.comp", "main", oi());   // shaderc_compute_shader
    if (!res || st(res) != 0) { printf("FAIL: shaderc compile\n"); return 1; }
    const char *key = "0f825ad6733bf669";
    char path[512]; snprintf(path, sizeof path, "%s/%s_b.spv", dir, key);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(by(res), 1, le(res), f) != le(res)) { printf("FAIL: write spv\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_b.json", dir, key);
    f = fopen(path, "w");
    fprintf(f, "{\"stage\":\"cs\",\"entry\":\"main\",\"bindings\":[{\"arg_id\":1,\"kind\":\"img\",\"reg\":0}],\"vertex_inputs\":[]}\n");
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
    NVNtexturePool tpool;
    CHECK(NVNF(nvnTexturePoolInitialize)(&tpool, &pool, 0x20000, 8) == 1, "texture pool initializes");
    NVNqueueBuilder qb; NVNqueue q;
    static uint8_t qmem[1 << 20] __attribute__((aligned(4096)));
    NVNF(nvnQueueBuilderSetDefaults)(&qb); NVNF(nvnQueueBuilderSetDevice)(&qb, &dev);
    size_t qsz = NVNF(nvnQueueBuilderGetQueueMemorySize)(&qb);
    NVNF(nvnQueueBuilderSetQueueMemory)(&qb, qmem, qsz);
    CHECK(NVNF(nvnQueueInitialize)(&q, &qb) == 1, "queue initializes");
    NVNtextureBuilder tb; NVNtexture tex;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 1, 1);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x40000);
    CHECK(NVNF(nvnTextureInitialize)(&tex, &tb) == 1, "image texture initializes");
    NVNF(nvnTexturePoolRegisterImage)(&tpool, 1, &tex, NULL);
    static uint8_t ctl[0x880]; ctl[0] = 4;
    NVNprogram prog;
    CHECK(NVNF(nvnProgramInitialize)(&prog, &dev) == 1, "program initializes");
    NVNshaderData sd[1] = {{0, ctl}};
    CHECK(NVNF(nvnProgramSetShaders)(&prog, 1, sd) == 1, "program takes the compute stage");
    NVNcommandBuffer cb;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb, &dev) == 1, "command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0x10000, 0x10000);
    NVNF(nvnCommandBufferSetTexturePool)(&cb, &tpool);
    NVNF(nvnCommandBufferBeginRecording)(&cb);
    NVNF(nvnCommandBufferBindProgram)(&cb, &prog, 0);
    NVNF(nvnCommandBufferBindImage)(&cb, NVN_SHADER_STAGE_COMPUTE, 0, (NVNimageHandle)1);
    NVNF(nvnCommandBufferDispatchCompute)(&cb, 1, 1, 1);
    NVNcommandHandle h = NVNF(nvnCommandBufferEndRecording)(&cb);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &h);
    NVNF(nvnQueueFinish)(&q);
    uint8_t px[4]; NVNcopyRegion all = {0, 0, 0, 1, 1, 1};
    mtl_read_texels(&tex, 0, 0, &all, px, 4, 4);
    CHECK(px[1] > 200 && px[0] < 40 && px[2] < 40, "imageStore reached the guest texture: %u %u %u %u", px[0], px[1], px[2], px[3]);
    printf("\n%s (%d failure%s)\n", g_fail ? "NVN COMPUTE IMAGE TEST FAILED" : "NVN COMPUTE IMAGE TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
