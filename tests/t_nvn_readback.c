// NVN CPU texture reads (nvnCommandBuffer ReadTexels through nvnTextureReadTexels):
// a 3D texture's slices with their image stride, a 2D array's layers, and a
// block-compressed texture's blocks, which the replay returns as the guest
// stores them (no decompression).
#include "dm.h"
#include "nvn.h"
#include "nvn_protos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *dm_nvn_test_lookup(const char *name);
#define NVNF(name) ((NVNT_##name)dm_nvn_test_lookup(#name))
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int g_fail;

static void fill_pattern(uint8_t *dst, uint8_t seed, int w, int h) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t *p = dst + (y * w + x) * 4;
        p[0] = (uint8_t)(seed + x * 17);
        p[1] = (uint8_t)(seed + y * 29);
        p[2] = (uint8_t)(seed * 3 + x + y);
        p[3] = 255;
    }
}

int main(void) {
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

    // A 4x4x2 3D texture: slice 1 gets a known pattern, slice 0 stays zero.
    NVNtextureBuilder tb; NVNtexture tex3d;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_3D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 4, 4);
    NVNF(nvnTextureBuilderSetDepth)(&tb, 2);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
    CHECK(NVNF(nvnTextureInitialize)(&tex3d, &tb) == 1, "3D texture initializes");
    uint8_t slice_px[4 * 4 * 4];
    fill_pattern(slice_px, 11, 4, 4);
    memcpy(storage + 0x8000, slice_px, sizeof slice_px);

    NVNcopyRegion slice1 = {0, 0, 1, 4, 4, 1};
    NVNcommandBuffer cb3d;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb3d, &dev) == 1, "3D command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb3d, &pool, 0x10000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb3d);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cb3d, (NVNbufferAddress)(uintptr_t)(storage + 0x8000),
                                              &tex3d, NULL, &slice1, 0);
    NVNcommandHandle h3d = NVNF(nvnCommandBufferEndRecording)(&cb3d);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &h3d);
    NVNF(nvnQueueFinish)(&q);

    uint8_t got_slice[4 * 4 * 4];
    NVNF(nvnTextureReadTexels)(&tex3d, NULL, &slice1, got_slice);
    CHECK(memcmp(got_slice, slice_px, sizeof slice_px) == 0,
          "ReadTexels returns the 3D texture's selected slice (first texel %u %u %u)",
          got_slice[0], got_slice[1], got_slice[2]);

    // The whole volume with the guest's image stride: slice 0 then slice 1.
    NVNcopyRegion volume = {0, 0, 0, 4, 4, 2};
    uint8_t got_volume[2][4 * 4 * 4];
    memset(got_volume, 0xAA, sizeof got_volume);
    NVNF(nvnTextureReadTexels)(&tex3d, NULL, &volume, got_volume);
    int volume_ok = 1;
    for (int i = 0; i < 4 * 4 * 4; i++) volume_ok &= got_volume[0][i] == 0;
    volume_ok &= memcmp(got_volume[1], slice_px, sizeof slice_px) == 0;
    CHECK(volume_ok, "ReadTexels returns every 3D slice at the guest's image stride");

    // A 4x4 2-layer array: each layer's copy selects its layer, and one read
    // returns both with the image stride.
    NVNtexture array;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_ARRAY);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 4, 4);
    NVNF(nvnTextureBuilderSetDepth)(&tb, 2);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x1000);
    CHECK(NVNF(nvnTextureInitialize)(&array, &tb) == 1, "2-layer array initializes");
    uint8_t layer0[4 * 4 * 4], layer1[4 * 4 * 4];
    fill_pattern(layer0, 3, 4, 4);
    fill_pattern(layer1, 200, 4, 4);
    memcpy(storage + 0x8100, layer0, sizeof layer0);
    memcpy(storage + 0x8200, layer1, sizeof layer1);
    NVNcopyRegion arr0 = {0, 0, 0, 4, 4, 1}, arr1 = {0, 0, 1, 4, 4, 1}, arr2 = {0, 0, 0, 4, 4, 2};
    NVNcommandBuffer cba;
    NVNF(nvnCommandBufferInitialize)(&cba, &dev);
    NVNF(nvnCommandBufferAddCommandMemory)(&cba, &pool, 0x20000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cba);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cba, (NVNbufferAddress)(uintptr_t)(storage + 0x8100), &array, NULL, &arr0, 0);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cba, (NVNbufferAddress)(uintptr_t)(storage + 0x8200), &array, NULL, &arr1, 0);
    NVNcommandHandle ha = NVNF(nvnCommandBufferEndRecording)(&cba);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &ha);
    NVNF(nvnQueueFinish)(&q);
    uint8_t got_layers[2][4 * 4 * 4];
    NVNF(nvnTextureReadTexels)(&array, NULL, &arr2, got_layers);
    CHECK(memcmp(got_layers[0], layer0, sizeof layer0) == 0 &&
          memcmp(got_layers[1], layer1, sizeof layer1) == 0,
          "ReadTexels returns both layers of a 2D array");

    // An 8x8 BC1 texture: 2x2 blocks of 8 bytes. The replay copies the blocks
    // through the transfer path and reads them back unchanged.
    uint8_t blocks[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) blocks[i][j] = (uint8_t)(0x10 * i + j * 5);
    memcpy(storage + 0x8400, blocks, sizeof blocks);
    NVNtexture bc_src, bc_dst;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 8, 8);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA_DXT1);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x2000);
    CHECK(NVNF(nvnTextureInitialize)(&bc_src, &tb) == 1, "BC1 source texture initializes");
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x3000);
    CHECK(NVNF(nvnTextureInitialize)(&bc_dst, &tb) == 1, "BC1 destination texture initializes");

    NVNcopyRegion bc_all = {0, 0, 0, 8, 8, 1}, bc_block = {0, 0, 0, 4, 4, 1}, bc_block_11 = {4, 4, 0, 4, 4, 1};
    NVNcommandBuffer cbc;
    NVNF(nvnCommandBufferInitialize)(&cbc, &dev);
    NVNF(nvnCommandBufferAddCommandMemory)(&cbc, &pool, 0x30000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cbc);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cbc, (NVNbufferAddress)(uintptr_t)(storage + 0x8400), &bc_src, NULL, &bc_all, 0);
    NVNF(nvnCommandBufferCopyTextureToTexture)(&cbc, &bc_src, NULL, &bc_all, &bc_dst, NULL, &bc_all, 0);
    NVNcommandHandle hbc = NVNF(nvnCommandBufferEndRecording)(&cbc);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hbc);
    NVNF(nvnQueueFinish)(&q);
    uint8_t got_blocks[32];
    NVNF(nvnTextureReadTexels)(&bc_src, NULL, &bc_all, got_blocks);
    CHECK(memcmp(got_blocks, blocks, sizeof blocks) == 0,
          "buffer to BC1 texture and ReadTexels round-trip its blocks (first block %02x %02x)",
          got_blocks[0], got_blocks[1]);
    memset(got_blocks, 0xAA, sizeof got_blocks);
    NVNF(nvnTextureReadTexels)(&bc_dst, NULL, &bc_all, got_blocks);
    CHECK(memcmp(got_blocks, blocks, sizeof blocks) == 0, "BC1 texture to texture copy keeps every block");

    // A single block copied to the destination's block (1,1) changes only it.
    NVNcommandBuffer cbblk;
    NVNF(nvnCommandBufferInitialize)(&cbblk, &dev);
    NVNF(nvnCommandBufferAddCommandMemory)(&cbblk, &pool, 0x40000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cbblk);
    NVNF(nvnCommandBufferCopyTextureToTexture)(&cbblk, &bc_src, NULL, &bc_block, &bc_dst, NULL, &bc_block_11, 0);
    NVNcommandHandle hblk = NVNF(nvnCommandBufferEndRecording)(&cbblk);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hblk);
    NVNF(nvnQueueFinish)(&q);
    uint8_t want_blocks[32];
    memcpy(want_blocks, blocks, sizeof blocks);
    memcpy(want_blocks + 3 * 8, blocks + 0 * 8, 8);
    memset(got_blocks, 0xAA, sizeof got_blocks);
    NVNF(nvnTextureReadTexels)(&bc_dst, NULL, &bc_all, got_blocks);
    CHECK(memcmp(got_blocks, want_blocks, sizeof blocks) == 0,
          "a block-region copy writes the destination block at its offset only");

    printf("\n%s (%d failure%s)\n", g_fail ? "NVN READBACK TEST FAILED" : "NVN READBACK TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
