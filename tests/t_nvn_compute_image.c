// NVN compute dynamically indexes a storage-image descriptor array, writing
// both mipmapped array images at layer 1; the host reads those subresources
// back. This covers STORAGE_IMAGE array binding and whole-image transitions.
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
extern void mtl_barrier_stats(uint64_t *skip, uint64_t *full);
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
        "layout(set=0,binding=1,rgba8) uniform image2DArray imgs[2];\n"
        "void main(){ uint i=gl_GlobalInvocationID.x; imageStore(imgs[i], ivec3(0,0,1), vec4(float(i==0u),float(i==1u),0,1)); }\n";
    const char *read_src =
        "#version 450\n"
        "layout(local_size_x=1,local_size_y=1,local_size_z=1) in;\n"
        "layout(set=0,binding=1,rgba8) readonly uniform image2DArray imgs[2];\n"
        "layout(set=0,binding=2,std430) buffer Results { uint values[]; } result;\n"
        "void main(){ uint i=gl_GlobalInvocationID.x; vec4 v=imageLoad(imgs[i],ivec3(0,0,1)); result.values[i]=uint(v.r*255.0+0.5)|(uint(v.g*255.0+0.5)<<8); }\n";
    void *(*ci)(void) = (void *)dlsym(lib, "shaderc_compiler_initialize");
    void *(*oi)(void) = (void *)dlsym(lib, "shaderc_compile_options_initialize");
    void *(*co)(void *, const char *, size_t, int, const char *, const char *, void *) = (void *)dlsym(lib, "shaderc_compile_into_spv");
    const uint32_t *(*by)(void *) = (void *)dlsym(lib, "shaderc_result_get_bytes");
    size_t (*le)(void *) = (void *)dlsym(lib, "shaderc_result_get_length");
    int (*st)(void *) = (void *)dlsym(lib, "shaderc_result_get_compilation_status");
    if (!ci || !oi || !co || !by || !le || !st) { printf("SKIP: shaderc API incomplete\n"); return 0; }
    void *res = co(ci(), src, strlen(src), 2, "t.comp", "main", oi());   // shaderc_compute_shader
    if (!res || st(res) != 0) { printf("FAIL: shaderc compile\n"); return 1; }
    void *read_res = co(ci(), read_src, strlen(read_src), 2, "t-read.comp", "main", oi());
    if (!read_res || st(read_res) != 0) { printf("FAIL: shaderc readback compile\n"); return 1; }
    const char *key = "0f825ad6733bf669";
    char path[512]; snprintf(path, sizeof path, "%s/%s_b.spv", dir, key);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(by(res), 1, le(res), f) != le(res)) { printf("FAIL: write spv\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_b.json", dir, key);
    f = fopen(path, "w");
    fprintf(f, "{\"stage\":\"cs\",\"entry\":\"main\",\"bindings\":[{\"arg_id\":1,\"kind\":\"img\",\"reg\":0}],\"vertex_inputs\":[]}\n");
    fclose(f);
    const char *read_key = "04e80bf6139612c8";
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, read_key);
    f = fopen(path, "wb");
    if (!f || fwrite(by(read_res), 1, le(read_res), f) != le(read_res)) { printf("FAIL: write readback spv\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_b.json", dir, read_key);
    f = fopen(path, "w");
    fprintf(f, "{\"stage\":\"cs\",\"entry\":\"main\",\"bindings\":[{\"arg_id\":1,\"kind\":\"img\",\"reg\":0},{\"arg_id\":2,\"kind\":\"sbuf\",\"reg\":0}],\"vertex_inputs\":[]}\n");
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
    NVNtextureBuilder tb; NVNtexture tex[2];
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_ARRAY);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 2, 2);
    NVNF(nvnTextureBuilderSetDepth)(&tb, 2);
    NVNF(nvnTextureBuilderSetLevels)(&tb, 2);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x40000);
    CHECK(NVNF(nvnTextureInitialize)(&tex[0], &tb) == 1, "first image texture initializes");
    NVNF(nvnTexturePoolRegisterImage)(&tpool, 1, &tex[0], NULL);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x60000);
    CHECK(NVNF(nvnTextureInitialize)(&tex[1], &tb) == 1, "second image texture initializes");
    NVNF(nvnTexturePoolRegisterImage)(&tpool, 2, &tex[1], NULL);
    static uint8_t ctl_write[0x880], ctl_read[0x880]; ctl_write[0] = 4; ctl_read[0] = 5;
    NVNprogram prog_write, prog_read;
    CHECK(NVNF(nvnProgramInitialize)(&prog_write, &dev) == 1, "write program initializes");
    CHECK(NVNF(nvnProgramInitialize)(&prog_read, &dev) == 1, "read program initializes");
    NVNshaderData sd_write[1] = {{0, ctl_write}}, sd_read[1] = {{0, ctl_read}};
    CHECK(NVNF(nvnProgramSetShaders)(&prog_write, 1, sd_write) == 1, "write program takes the compute stage");
    CHECK(NVNF(nvnProgramSetShaders)(&prog_read, 1, sd_read) == 1, "read program takes the compute stage");
    NVNcommandBuffer cb;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb, &dev) == 1, "command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0x10000, 0x10000);
    NVNF(nvnCommandBufferSetTexturePool)(&cb, &tpool);
    uint64_t barriers_before = 0;
    mtl_barrier_stats(NULL, &barriers_before);
    NVNF(nvnCommandBufferBeginRecording)(&cb);
    NVNF(nvnCommandBufferBindProgram)(&cb, &prog_write, 0);
    NVNF(nvnCommandBufferBindImage)(&cb, NVN_SHADER_STAGE_COMPUTE, 0, (NVNimageHandle)1);
    NVNF(nvnCommandBufferBindImage)(&cb, NVN_SHADER_STAGE_COMPUTE, 1, (NVNimageHandle)2);
    NVNF(nvnCommandBufferDispatchCompute)(&cb, 2, 1, 1);
    NVNF(nvnCommandBufferBarrier)(&cb, NVN_BARRIER_ORDER_PRIMITIVES_BIT |
        NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT |
        NVN_BARRIER_ORDER_INDIRECT_DATA_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT |
        NVN_BARRIER_INVALIDATE_SHADER_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR_BIT |
        NVN_BARRIER_INVALIDATE_ZCULL_BIT);
    uint32_t *results = (uint32_t *)(storage + 0x8000);
    results[0] = results[1] = 0;
    NVNF(nvnCommandBufferBindStorageBuffer)(&cb, NVN_SHADER_STAGE_COMPUTE, 0,
        (NVNbufferAddress)(uintptr_t)results, 2 * sizeof *results);
    NVNF(nvnCommandBufferBindProgram)(&cb, &prog_read, 0);
    NVNF(nvnCommandBufferDispatchCompute)(&cb, 2, 1, 1);
    NVNcommandHandle h = NVNF(nvnCommandBufferEndRecording)(&cb);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &h);
    NVNF(nvnQueueFinish)(&q);
    uint8_t px[2][16]; NVNcopyRegion all = {0, 0, 0, 2, 2, 1};
    mtl_read_texels(&tex[0], 0, 1, &all, px[0], 8, 16);
    mtl_read_texels(&tex[1], 0, 1, &all, px[1], 8, 16);
    CHECK(px[0][0] > 200 && px[0][1] < 40 && px[0][2] < 40, "storage-image array element 0 contains red: %u %u %u %u", px[0][0], px[0][1], px[0][2], px[0][3]);
    CHECK(px[1][1] > 200 && px[1][0] < 40 && px[1][2] < 40, "storage-image array element 1 contains green: %u %u %u %u", px[1][0], px[1][1], px[1][2], px[1][3]);
    CHECK(results[0] == 255 && results[1] == (255u << 8),
          "following compute dispatch reads prior image writes: %#x %#x", results[0], results[1]);
    uint64_t barriers_after = 0;
    mtl_barrier_stats(NULL, &barriers_after);
    CHECK(barriers_after == barriers_before + 1,
          "NVN barrier record emitted one Vulkan memory dependency: %llu -> %llu",
          (unsigned long long)barriers_before, (unsigned long long)barriers_after);
    printf("\n%s (%d failure%s)\n", g_fail ? "NVN COMPUTE IMAGE TEST FAILED" : "NVN COMPUTE IMAGE TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
