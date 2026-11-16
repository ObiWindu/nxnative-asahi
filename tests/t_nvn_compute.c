// NVN -> Vulkan compute test: a compute shader from the shader cache writes an
// SSBO, the NVN dispatch record runs it, and the guest's storage buffer holds
// the results afterwards.
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
#define NVNF(name) ((NVNT_##name)dm_nvn_test_lookup(#name))
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    void *lib = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("SKIP: libshaderc is not installed\n"); return 0; }
    char dir[] = "/tmp/opencode/nvn-compute-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("NX_NVN_SHADER_CACHE", dir, 1);
    const char *src =
        "#version 450\n"
        "layout(local_size_x=8,local_size_y=1,local_size_z=1) in;\n"
        "layout(set=0,binding=1) buffer B { uint v[]; } b;\n"
        "void main(){ uint i=gl_GlobalInvocationID.x; if(i<64u) b.v[i]=i*3u+1u; }\n";
    void *(*ci)(void) = (void *)dlsym(lib, "shaderc_compiler_initialize");
    void *(*oi)(void) = (void *)dlsym(lib, "shaderc_compile_options_initialize");
    void *(*co)(void *, const char *, size_t, int, const char *, const char *, void *) = (void *)dlsym(lib, "shaderc_compile_into_spv");
    const uint32_t *(*by)(void *) = (void *)dlsym(lib, "shaderc_result_get_bytes");
    size_t (*le)(void *) = (void *)dlsym(lib, "shaderc_result_get_length");
    int (*st)(void *) = (void *)dlsym(lib, "shaderc_result_get_compilation_status");
    if (!ci || !oi || !co || !by || !le || !st) { printf("SKIP: shaderc API incomplete\n"); return 0; }
    void *res = co(ci(), src, strlen(src), 5 /* compute */, "t.comp", "main", oi());   // shaderc_compute_shader == 2 in current shaderc; try both below
    if (!res || st(res) != 0) res = co(ci(), src, strlen(src), 2, "t.comp", "main", oi());
    if (!res || st(res) != 0) { printf("FAIL: shaderc compute compile\n"); return 1; }
    const char *key = "4764cc257d24a328";
    char path[512]; snprintf(path, sizeof path, "%s/%s_b.spv", dir, key);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(by(res), 1, le(res), f) != le(res)) { printf("FAIL: write compute spv\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_b.json", dir, key);
    f = fopen(path, "w");
    fprintf(f, "{\"stage\":\"cs\",\"entry\":\"main\",\"bindings\":[{\"arg_id\":1,\"kind\":\"sbuf\",\"reg\":0}],\"vertex_inputs\":[]}\n");
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

    static uint8_t ctl[0x880];
    ctl[0] = 2;
    NVNprogram prog;
    CHECK(NVNF(nvnProgramInitialize)(&prog, &dev) == 1, "program initializes");
    NVNshaderData sd[1] = {{0, ctl}};
    CHECK(NVNF(nvnProgramSetShaders)(&prog, 1, sd) == 1, "program takes the compute stage");

    uint32_t *out = (uint32_t *)(storage + 0x8000);
    NVNcommandBuffer cb;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb, &dev) == 1, "command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0x10000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb);
    NVNF(nvnCommandBufferBindProgram)(&cb, &prog, 0);
    NVNF(nvnCommandBufferBindStorageBuffer)(&cb, NVN_SHADER_STAGE_COMPUTE, 0, (NVNbufferAddress)(uintptr_t)out, 64 * 4);
    NVNF(nvnCommandBufferDispatchCompute)(&cb, 16, 1, 1);
    NVNcommandHandle h = NVNF(nvnCommandBufferEndRecording)(&cb);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &h);
    NVNF(nvnQueueFinish)(&q);
    int bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (out[i] != i * 3 + 1) { if (!bad) printf("first mismatch at %u: %u\n", i, out[i]); bad++; }
    CHECK(bad == 0, "compute wrote all 64 words (first %u, last %u)", out[0], out[63]);
    printf("\n%s (%d failure%s)\n", g_fail ? "NVN COMPUTE TEST FAILED" : "NVN COMPUTE TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
