// NVN copy records: buffer→texture, texture→texture and texture→buffer. The
// round trip through the Vulkan replay must preserve every byte.
#include "dm.h"
#include "nvn.h"
#include "nvn_protos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *dm_nvn_test_lookup(const char *name);
extern void mtl_read_texels(const void *texture, unsigned level, unsigned layer,
                            const NVNcopyRegion *region, uint8_t *dst,
                            size_t row_stride, size_t image_stride);
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

    NVNcommandBuffer cb_mirror;
    NVNF(nvnCommandBufferInitialize)(&cb_mirror, &dev);
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_mirror, &pool, 0x11000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb_mirror);
    NVNF(nvnCommandBufferCopyTextureToTexture)(&cb_mirror, &a, NULL, &r, &b, NULL, &r, NVN_COPY_FLAGS_MIRROR_X_BIT);
    NVNF(nvnCommandBufferCopyTextureToBuffer)(&cb_mirror, &b, NULL, &r, (NVNbufferAddress)(uintptr_t)out, 0);
    NVNcommandHandle hm = NVNF(nvnCommandBufferEndRecording)(&cb_mirror);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hm);
    NVNF(nvnQueueFinish)(&q);
    int mirror_match = 1;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        mirror_match &= memcmp(out + (y * 8 + x) * 4, in + (y * 8 + (7 - x)) * 4, 4) == 0;
    CHECK(mirror_match, "texture copy mirror-X reverses each row");

    // Exercise every selection field used by image copies: mip 1, array layer
    // 1, two-layer regions, row padding and per-image padding.
    NVNtexture aa, ab;
    NVNtextureBuilder array_builder;
    NVNF(nvnTextureBuilderSetDefaults)(&array_builder); NVNF(nvnTextureBuilderSetDevice)(&array_builder, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&array_builder, NVN_TEXTURE_TARGET_2D_ARRAY);
    NVNF(nvnTextureBuilderSetSize2D)(&array_builder, 4, 4);
    NVNF(nvnTextureBuilderSetDepth)(&array_builder, 2);
    NVNF(nvnTextureBuilderSetLevels)(&array_builder, 2);
    NVNF(nvnTextureBuilderSetFormat)(&array_builder, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetFlags)(&array_builder, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetStorage)(&array_builder, &pool, 0x20000);
    CHECK(NVNF(nvnTextureInitialize)(&aa, &array_builder) == 1, "array source texture initializes");
    NVNF(nvnTextureBuilderSetStorage)(&array_builder, &pool, 0x40000);
    CHECK(NVNF(nvnTextureInitialize)(&ab, &array_builder) == 1, "array destination texture initializes");
    uint8_t *layered_in = storage + 0xa000;
    uint8_t *layered_out = storage + 0xa100;
    memset(layered_in, 0xee, 64); memset(layered_out, 0, 64);
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++)
        for (int x = 0; x < 8; x++) layered_in[z * 32 + y * 12 + x] = (uint8_t)(1 + z * 80 + y * 24 + x);
    NVNcopyRegion two_layers = {0, 0, 0, 2, 2, 2};
    NVNcommandBuffer cb_array;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb_array, &dev) == 1, "array copy command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_array, &pool, 0x12000, 0x10000);
    NVNF(nvnCommandBufferSetCopyRowStride)(&cb_array, 12);
    NVNF(nvnCommandBufferSetCopyImageStride)(&cb_array, 32);
    NVNF(nvnCommandBufferBeginRecording)(&cb_array);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cb_array, (NVNbufferAddress)(uintptr_t)layered_in, &aa, NULL, &two_layers, 0);
    NVNF(nvnCommandBufferCopyTextureToTexture)(&cb_array, &aa, NULL, &two_layers, &ab, NULL, &two_layers, 0);
    NVNF(nvnCommandBufferCopyTextureToBuffer)(&cb_array, &ab, NULL, &two_layers, (NVNbufferAddress)(uintptr_t)layered_out, 0);
    NVNcommandHandle ha = NVNF(nvnCommandBufferEndRecording)(&cb_array);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &ha);
    NVNF(nvnQueueFinish)(&q);
    int rows_match = 1;
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++)
        rows_match &= memcmp(layered_in + z * 32 + y * 12, layered_out + z * 32 + y * 12, 8) == 0;
    CHECK(rows_match, "two array layers copy with row and image strides");
    uint8_t untouched[4] = {0xff, 0xff, 0xff, 0xff};
    NVNcopyRegion corner = {3, 3, 0, 1, 1, 1};
    mtl_read_texels(&aa, 0, 0, &corner, untouched, 4, 4);
    CHECK(untouched[0] == 0 && untouched[1] == 0 && untouched[2] == 0 && untouched[3] == 0,
          "partial buffer-to-texture copy preserves untouched texels");
    memset(untouched, 0xff, sizeof untouched);
    mtl_read_texels(&ab, 0, 0, &corner, untouched, 4, 4);
    CHECK(untouched[0] == 0 && untouched[1] == 0 && untouched[2] == 0 && untouched[3] == 0,
          "partial texture-to-texture copy preserves destination texels");

    NVNtextureView av, bv;
    NVNF(nvnTextureViewSetDefaults)(&av); NVNF(nvnTextureViewSetLevels)(&av, 1, 1); NVNF(nvnTextureViewSetLayers)(&av, 1, 1);
    NVNF(nvnTextureViewSetDefaults)(&bv); NVNF(nvnTextureViewSetLevels)(&bv, 1, 1); NVNF(nvnTextureViewSetLayers)(&bv, 0, 1);
    uint8_t *mip_in = storage + 0xa200, *mip_out = storage + 0xa300;
    for (int i = 0; i < 16; i++) mip_in[i] = (uint8_t)(0xb0 + i);
    memset(mip_out, 0, 16);
    NVNcopyRegion mip_layer = {0, 0, 0, 2, 2, 1};
    NVNcommandBuffer cb_view;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb_view, &dev) == 1, "view copy command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_view, &pool, 0x13000, 0x10000);
    NVNF(nvnCommandBufferSetCopyRowStride)(&cb_view, 0);
    NVNF(nvnCommandBufferSetCopyImageStride)(&cb_view, 0);
    NVNF(nvnCommandBufferBeginRecording)(&cb_view);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cb_view, (NVNbufferAddress)(uintptr_t)mip_in, &aa, &av, &mip_layer, 0);
    NVNF(nvnCommandBufferCopyTextureToTexture)(&cb_view, &aa, &av, &mip_layer, &ab, &bv, &mip_layer, 0);
    NVNF(nvnCommandBufferCopyTextureToBuffer)(&cb_view, &ab, &bv, &mip_layer, (NVNbufferAddress)(uintptr_t)mip_out, 0);
    NVNcommandHandle hv = NVNF(nvnCommandBufferEndRecording)(&cb_view);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hv);
    NVNF(nvnQueueFinish)(&q);
    CHECK(memcmp(mip_in, mip_out, 16) == 0, "selected mip 1 and array layer 1 copy through texture views");

    NVNtextureView bv0;
    NVNF(nvnTextureViewSetDefaults)(&bv0); NVNF(nvnTextureViewSetLevels)(&bv0, 0, 1); NVNF(nvnTextureViewSetLayers)(&bv0, 0, 1);
    uint8_t scaled_out[64] = {0};
    NVNcopyRegion src_small = {0, 0, 0, 2, 2, 1}, dst_large = {0, 0, 0, 4, 4, 1};
    NVNcommandBuffer cb_scale;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb_scale, &dev) == 1, "scale copy command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_scale, &pool, 0x15000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb_scale);
    NVNF(nvnCommandBufferCopyTextureToTexture)(&cb_scale, &aa, &av, &src_small, &ab, &bv0, &dst_large, 0);
    NVNF(nvnCommandBufferCopyTextureToBuffer)(&cb_scale, &ab, &bv0, &dst_large, (NVNbufferAddress)(uintptr_t)scaled_out, 0);
    NVNcommandHandle hs = NVNF(nvnCommandBufferEndRecording)(&cb_scale);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hs);
    NVNF(nvnQueueFinish)(&q);
    int scale_match = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        scale_match &= memcmp(scaled_out + (y * 4 + x) * 4,
                              mip_in + ((y / 2) * 2 + x / 2) * 4, 4) == 0;
    CHECK(scale_match, "nearest texture blit scales a selected 2x2 mip region to 4x4");

    NVNtexture packed_float, rgba_float;
    NVNtextureBuilder convert_builder;
    NVNF(nvnTextureBuilderSetDefaults)(&convert_builder); NVNF(nvnTextureBuilderSetDevice)(&convert_builder, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&convert_builder, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&convert_builder, 1, 1);
    NVNF(nvnTextureBuilderSetFormat)(&convert_builder, NVN_FORMAT_R11G11B10F);
    NVNF(nvnTextureBuilderSetFlags)(&convert_builder, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetStorage)(&convert_builder, &pool, 0x80000);
    CHECK(NVNF(nvnTextureInitialize)(&packed_float, &convert_builder) == 1, "packed float source texture initializes");
    NVNF(nvnTextureBuilderSetFormat)(&convert_builder, NVN_FORMAT_RGBA16F);
    NVNF(nvnTextureBuilderSetStorage)(&convert_builder, &pool, 0x90000);
    CHECK(NVNF(nvnTextureInitialize)(&rgba_float, &convert_builder) == 1, "RGBA16F conversion target initializes");
    uint32_t one_rgb11 = 0x781e03c0;
    uint16_t rgba16_out[4] = {0};
    NVNcopyRegion one_pixel = {0, 0, 0, 1, 1, 1};
    NVNcommandBuffer cb_convert;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb_convert, &dev) == 1, "format conversion command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_convert, &pool, 0x16000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb_convert);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cb_convert, (NVNbufferAddress)(uintptr_t)&one_rgb11, &packed_float, NULL, &one_pixel, 0);
    NVNF(nvnCommandBufferCopyTextureToTexture)(&cb_convert, &packed_float, NULL, &one_pixel, &rgba_float, NULL, &one_pixel, 0);
    NVNF(nvnCommandBufferCopyTextureToBuffer)(&cb_convert, &rgba_float, NULL, &one_pixel, (NVNbufferAddress)(uintptr_t)rgba16_out, 0);
    NVNcommandHandle hc = NVNF(nvnCommandBufferEndRecording)(&cb_convert);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hc);
    NVNF(nvnQueueFinish)(&q);
    CHECK(rgba16_out[0] == 0x3c00 && rgba16_out[1] == 0x3c00 && rgba16_out[2] == 0x3c00 && rgba16_out[3] == 0x3c00,
          "packed R11G11B10 float blit converts 1.0 RGB to RGBA16F");

    NVNtexture volume;
    NVNtextureBuilder volume_builder;
    NVNF(nvnTextureBuilderSetDefaults)(&volume_builder); NVNF(nvnTextureBuilderSetDevice)(&volume_builder, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&volume_builder, NVN_TEXTURE_TARGET_3D);
    NVNF(nvnTextureBuilderSetSize3D)(&volume_builder, 4, 4, 4);
    NVNF(nvnTextureBuilderSetFormat)(&volume_builder, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetFlags)(&volume_builder, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetStorage)(&volume_builder, &pool, 0x60000);
    CHECK(NVNF(nvnTextureInitialize)(&volume, &volume_builder) == 1, "3D texture initializes");
    uint8_t *volume_in = storage + 0xa400, *volume_out = storage + 0xa500;
    memset(volume_in, 0xdd, 64); memset(volume_out, 0, 64);
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++)
        for (int x = 0; x < 8; x++) volume_in[z * 32 + y * 12 + x] = (uint8_t)(0x30 + z * 60 + y * 20 + x);
    NVNcopyRegion volume_region = {0, 0, 1, 2, 2, 2};
    NVNcommandBuffer cb_volume;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb_volume, &dev) == 1, "3D copy command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_volume, &pool, 0x14000, 0x10000);
    NVNF(nvnCommandBufferSetCopyRowStride)(&cb_volume, 12);
    NVNF(nvnCommandBufferSetCopyImageStride)(&cb_volume, 32);
    NVNF(nvnCommandBufferBeginRecording)(&cb_volume);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cb_volume, (NVNbufferAddress)(uintptr_t)volume_in, &volume, NULL, &volume_region, 0);
    NVNF(nvnCommandBufferCopyTextureToBuffer)(&cb_volume, &volume, NULL, &volume_region, (NVNbufferAddress)(uintptr_t)volume_out, 0);
    NVNcommandHandle hz = NVNF(nvnCommandBufferEndRecording)(&cb_volume);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hz);
    NVNF(nvnQueueFinish)(&q);
    int volume_match = 1;
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++)
        volume_match &= memcmp(volume_in + z * 32 + y * 12, volume_out + z * 32 + y * 12, 8) == 0;
    CHECK(volume_match, "3D buffer copy preserves selected depth slices and strides");
    printf("\n%s (%d failure%s)\n", g_fail ? "NVN COPY TEST FAILED" : "NVN COPY TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
