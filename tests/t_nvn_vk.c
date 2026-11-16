// Minimal NVN interface + Vulkan replay smoke test: bootstrap lookup, device,
// memory pool, command buffer recording, submit and fence. It is deliberately
// smaller than dm/gfx/tests/t_nvn.c; the latter remains the comprehensive NVN
// interface test once the draw path is implemented.
#include "dm.h"
#include "nvn.h"
#include "nvn_protos.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern void *dm_nvn_test_lookup(const char *name);
#define NVNF(name) ((NVNT_##name)dm_nvn_test_lookup(#name))
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    setenv("NX_VK_TEST_SHADERS", "1", 1);
    int g_fail = 0;
    for (unsigned i = 0;; i++) {
        const char *n = i == 0 ? "nvnDeviceGetProcAddress" : i == 1 ? "nvnDeviceBuilderSetDefaults" : i == 2 ? "nvnDeviceInitialize" : NULL;
        if (!n) break;
        CHECK(dm_nvn_test_lookup(n) != NULL, "bootstrap resolves %s", n);
    }
    NVNdeviceBuilder db;
    NVNdevice dev;
    NVNF(nvnDeviceBuilderSetDefaults)(&db);
    NVNF(nvnDeviceBuilderSetFlags)(&db, 0);
    CHECK(NVNF(nvnDeviceInitialize)(&dev, &db) == 1, "device initializes");
    int align = 0;
    NVNF(nvnDeviceGetInteger)(&dev, NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT, &align);
    CHECK(align == 256, "uniform buffer alignment is 256 (got %d)", align);

    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool;
    static uint8_t storage[1 << 20] __attribute__((aligned(4096)));
    NVNF(nvnMemoryPoolBuilderSetDefaults)(&pb);
    NVNF(nvnMemoryPoolBuilderSetDevice)(&pb, &dev);
    NVNF(nvnMemoryPoolBuilderSetStorage)(&pb, storage, sizeof storage);
    CHECK(NVNF(nvnMemoryPoolInitialize)(&pool, &pb) == 1, "memory pool initializes");

    NVNqueueBuilder qb;
    NVNqueue q;
    static uint8_t qmem[1 << 20] __attribute__((aligned(4096)));
    NVNF(nvnQueueBuilderSetDefaults)(&qb);
    NVNF(nvnQueueBuilderSetDevice)(&qb, &dev);
    size_t qsz = NVNF(nvnQueueBuilderGetQueueMemorySize)(&qb);
    NVNF(nvnQueueBuilderSetQueueMemory)(&qb, qmem, qsz);
    CHECK(NVNF(nvnQueueInitialize)(&q, &qb) == 1, "queue initializes");

    NVNtextureBuilder tb;
    NVNtexture tex;
    NVNF(nvnTextureBuilderSetDefaults)(&tb);
    NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 64, 64);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
    size_t tsz = NVNF(nvnTextureBuilderGetStorageSize)(&tb);
    CHECK(tsz > 0 && tsz <= 0x10000, "linear RGBA8 64x64 storage size fits the pool (%zu)", tsz);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
    CHECK(NVNF(nvnTextureInitialize)(&tex, &tb) == 1, "texture initializes");

    NVNprogram prog;
    CHECK(NVNF(nvnProgramInitialize)(&prog, &dev) == 1, "program initializes");
    static uint8_t control[0x880];
    NVNshaderData sd[2] = {{0, control}, {0, control}};
    CHECK(NVNF(nvnProgramSetShaders)(&prog, 2, sd) == 1, "program takes two shader stages");

    float *verts = (float *)(storage + 0x8000);
    verts[0] = -0.5f; verts[1] = -0.5f;
    verts[2] =  0.5f; verts[3] = -0.5f;
    verts[4] =  0.0f; verts[5] =  0.5f;

    NVNcommandBuffer cb;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb, &dev) == 1, "command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0x10000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb);
    const NVNtexture *color[1] = {&tex};
    NVNF(nvnCommandBufferSetRenderTargets)(&cb, 1, color, NULL, NULL, NULL);
    float blue[4] = {0, 0, 1, 1};
    NVNF(nvnCommandBufferClearColor)(&cb, 0, blue, 0xf);
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
    NVNF(nvnCommandBufferBindVertexBuffer)(&cb, 0, (NVNbufferAddress)(uintptr_t)(storage + 0x8000), 24);
    NVNF(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    NVNcommandHandle h = NVNF(nvnCommandBufferEndRecording)(&cb);
    CHECK(h != 0, "recording ends with a handle");
    NVNF(nvnQueueSubmitCommands)(&q, 1, &h);
    NVNF(nvnQueueFinish)(&q);
    CHECK(1, "submit and finish complete");
    const uint8_t *px = storage;
    const uint8_t *mid = storage + (32 * 64 + 32) * 4;
    CHECK(px[0] < 16 && px[1] < 16 && px[2] > 240 && px[3] > 240,
          "NVN clear reached the guest texture corner: %u %u %u %u", px[0], px[1], px[2], px[3]);
    CHECK(mid[0] > 240 && mid[1] < 16 && mid[2] < 16 && mid[3] > 240,
          "NVN draw reached the guest texture center: %u %u %u %u", mid[0], mid[1], mid[2], mid[3]);
    printf("\n%s (%d failure%s)\n", g_fail ? "NVN VK TEST FAILED" : "NVN VK TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
