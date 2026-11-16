// NVN copy records: buffer→texture, texture→texture and texture→buffer. The
// round trip through the Vulkan replay must preserve every byte.
#include "dm.h"
#include "nvn.h"
#include "nvn_protos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *dm_nvn_test_lookup(const char *name);
#define NVNF(name) ((NVNT_##name)dm_nvn_test_lookup(#name))
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
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
    NVNtextureBuilder tb;
    NVNtexture a, b;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 8, 8);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
    CHECK(NVNF(nvnTextureInitialize)(&a, &tb) == 1, "source texture initializes");
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x1000);
    CHECK(NVNF(nvnTextureInitialize)(&b, &tb) == 1, "destination texture initializes");
    uint8_t *in = storage + 0x8000;
    uint8_t *out = storage + 0x9000;
    for (int i = 0; i < 8 * 8 * 4; i++) in[i] = (uint8_t)(i * 7 + 3);
    NVNcopyRegion r = {0, 0, 0, 8, 8, 1};
    NVNcommandBuffer cb;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb, &dev) == 1, "command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0x10000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cb, (NVNbufferAddress)(uintptr_t)in, &a, NULL, &r, 0);
    NVNF(nvnCommandBufferCopyTextureToTexture)(&cb, &a, NULL, &r, &b, NULL, &r, 0);
    NVNF(nvnCommandBufferCopyTextureToBuffer)(&cb, &b, NULL, &r, (NVNbufferAddress)(uintptr_t)out, 0);
    NVNcommandHandle h = NVNF(nvnCommandBufferEndRecording)(&cb);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &h);
    NVNF(nvnQueueFinish)(&q);
    CHECK(memcmp(in, out, 8 * 8 * 4) == 0, "buffer→texture→texture→buffer round trip (%u %u %u ...)", out[0], out[1], out[2]);
    printf("\n%s (%d failure%s)\n", g_fail ? "NVN COPY TEST FAILED" : "NVN COPY TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
