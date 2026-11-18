// nvnCommandBufferDrawTexture: a texture's source region drawn into a colour
// target's destination rectangle (the Vulkan counterpart of the Metal host's
// draw_tex). The replay draws it as one full-target triangle, so the tests check
// the texel-to-pixel mapping, a source sub-region, a scissored destination, a
// mirrored destination rectangle and the channel write mask.
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

static int g_fail;

// The 4x4 source pattern: red rises along x, green along y, blue and alpha are
// constants, so a readback says which source texel a destination pixel took.
static void src_px(int x, int y, uint8_t out[4]) {
    out[0] = (uint8_t)(40 + x * 50);
    out[1] = (uint8_t)(40 + y * 50);
    out[2] = 200;
    out[3] = 255;
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
    NVNtexturePool tpool; NVNsamplerPool spool;
    CHECK(NVNF(nvnTexturePoolInitialize)(&tpool, &pool, 0x30000, 32) == 1, "texture pool initializes");
    CHECK(NVNF(nvnSamplerPoolInitialize)(&spool, &pool, 0x38000, 8) == 1, "sampler pool initializes");
    NVNqueueBuilder qb; NVNqueue q;
    static uint8_t qmem[1 << 20] __attribute__((aligned(4096)));
    NVNF(nvnQueueBuilderSetDefaults)(&qb); NVNF(nvnQueueBuilderSetDevice)(&qb, &dev);
    size_t qsz = NVNF(nvnQueueBuilderGetQueueMemorySize)(&qb);
    NVNF(nvnQueueBuilderSetQueueMemory)(&qb, qmem, qsz);
    CHECK(NVNF(nvnQueueInitialize)(&q, &qb) == 1, "queue initializes");

    NVNtextureBuilder tb; NVNtexture target, source;
    NVNF(nvnTextureBuilderSetDefaults)(&tb); NVNF(nvnTextureBuilderSetDevice)(&tb, &dev);
    NVNF(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    NVNF(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNF(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 8, 8);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
    CHECK(NVNF(nvnTextureInitialize)(&target, &tb) == 1, "render target initializes");
    NVNF(nvnTextureBuilderSetSize2D)(&tb, 4, 4);
    NVNF(nvnTextureBuilderSetStorage)(&tb, &pool, 0x1000);
    CHECK(NVNF(nvnTextureInitialize)(&source, &tb) == 1, "source texture initializes");
    NVNF(nvnTexturePoolRegisterTexture)(&tpool, 1, &source, NULL);

    // Fill the source through a buffer copy, which writes the texture's own
    // (padded) linear layout; a tight CPU write would ignore its row pitch.
    uint8_t *src_texels = storage + 0x8000;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        uint8_t p[4]; src_px(x, y, p);
        memcpy(src_texels + (y * 4 + x) * 4, p, 4);
    }
    NVNcopyRegion src_all = {0, 0, 0, 4, 4, 1};
    NVNcommandBuffer cb_src;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb_src, &dev) == 1, "source-fill command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_src, &pool, 0x20000, 0x10000);
    NVNF(nvnCommandBufferBeginRecording)(&cb_src);
    NVNF(nvnCommandBufferCopyBufferToTexture)(&cb_src, (NVNbufferAddress)(uintptr_t)src_texels, &source, NULL, &src_all, 0);
    NVNcommandHandle hsrc = NVNF(nvnCommandBufferEndRecording)(&cb_src);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hsrc);
    NVNF(nvnQueueFinish)(&q);

    NVNsamplerBuilder sb; NVNsampler smp;
    NVNF(nvnSamplerBuilderSetDefaults)(&sb); NVNF(nvnSamplerBuilderSetDevice)(&sb, &dev);
    // NVN's defaults filter (mag linear, min nearest-mipmap-linear). The test
    // needs the exact texel each destination pixel takes, so sample nearest
    // with no mip selection.
    NVNF(nvnSamplerBuilderSetMinMagFilter)(&sb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    NVNF(nvnSamplerBuilderSetLodClamp)(&sb, 0.0f, 0.0f);
    CHECK(NVNF(nvnSamplerInitialize)(&smp, &sb) == 1, "sampler initializes");
    NVNF(nvnSamplerPoolRegisterSampler)(&spool, 1, &smp);
    NVNtextureHandle handle = NVNF(nvnDeviceGetTextureHandle)(&dev, 1, 1);

    NVNchannelMaskState cmask, cfull;
    NVNF(nvnChannelMaskStateSetDefaults)(&cfull);
    NVNF(nvnChannelMaskStateSetDefaults)(&cmask);
    NVNF(nvnChannelMaskStateSetChannelMask)(&cmask, 0, 1, 0, 0, 0);

    NVNcopyRegion all = {0, 0, 0, 8, 8, 1};
    uint8_t px[8 * 8 * 4];
    const NVNtexture *colors[1] = {&target};
    float black[4] = {0, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    NVNdrawTextureRegion full_dst = {0, 0, 8, 8}, full_src = {0, 0, 4, 4};

    // 1. A 4x4 source stretched over the whole 8x8 target: destination pixel
    // (x, y) samples source texel (x/2, y/2).
    NVNcommandBuffer cb_full;
    CHECK(NVNF(nvnCommandBufferInitialize)(&cb_full, &dev) == 1, "stretch command buffer initializes");
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_full, &pool, 0x40000, 0x10000);
    NVNF(nvnCommandBufferSetTexturePool)(&cb_full, &tpool);
    NVNF(nvnCommandBufferSetSamplerPool)(&cb_full, &spool);
    NVNF(nvnCommandBufferBeginRecording)(&cb_full);
    NVNF(nvnCommandBufferSetRenderTargets)(&cb_full, 1, colors, NULL, NULL, NULL);
    NVNF(nvnCommandBufferBindChannelMaskState)(&cb_full, &cfull);
    NVNF(nvnCommandBufferSetScissor)(&cb_full, 0, 0, 8, 8);
    NVNF(nvnCommandBufferClearColor)(&cb_full, 0, black, 0xf);
    NVNF(nvnCommandBufferDrawTexture)(&cb_full, handle, &full_dst, &full_src);
    NVNcommandHandle hf = NVNF(nvnCommandBufferEndRecording)(&cb_full);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hf);
    NVNF(nvnQueueFinish)(&q);
    mtl_read_texels(&target, 0, 0, &all, px, 8 * 4, 8 * 8 * 4);
    int stretch_ok = 1;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        uint8_t want[4]; src_px(x / 2, y / 2, want);
        stretch_ok &= memcmp(px + (y * 8 + x) * 4, want, 4) == 0;
    }
    CHECK(stretch_ok, "DrawTexture stretches its source region over the destination: (%u %u %u) at 7,7",
          px[(7 * 8 + 7) * 4], px[(7 * 8 + 7) * 4 + 1], px[(7 * 8 + 7) * 4 + 2]);

    // 2. A sub-region of the destination, clamped by the scissor: pixels
    // outside the destination rectangle keep the clear's blue.
    NVNcommandBuffer cb_region;
    NVNF(nvnCommandBufferInitialize)(&cb_region, &dev);
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_region, &pool, 0x48000, 0x10000);
    NVNF(nvnCommandBufferSetTexturePool)(&cb_region, &tpool);
    NVNF(nvnCommandBufferSetSamplerPool)(&cb_region, &spool);
    NVNF(nvnCommandBufferBeginRecording)(&cb_region);
    NVNF(nvnCommandBufferSetRenderTargets)(&cb_region, 1, colors, NULL, NULL, NULL);
    NVNF(nvnCommandBufferBindChannelMaskState)(&cb_region, &cfull);
    // The clear covers the whole target; the smaller scissor applies to the
    // draw only, so the pixels outside its destination rectangle keep blue.
    NVNF(nvnCommandBufferSetScissor)(&cb_region, 0, 0, 8, 8);
    NVNF(nvnCommandBufferClearColor)(&cb_region, 0, blue, 0xf);
    NVNF(nvnCommandBufferSetScissor)(&cb_region, 2, 2, 4, 4);
    NVNF(nvnCommandBufferDrawTexture)(&cb_region, handle, &full_dst, &full_src);
    NVNcommandHandle hr = NVNF(nvnCommandBufferEndRecording)(&cb_region);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hr);
    NVNF(nvnQueueFinish)(&q);
    mtl_read_texels(&target, 0, 0, &all, px, 8 * 4, 8 * 8 * 4);
    int region_ok = 1;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        const uint8_t *p = px + (y * 8 + x) * 4;
        if (x >= 2 && x < 6 && y >= 2 && y < 6) {
            uint8_t want[4]; src_px(x / 2, y / 2, want);
            region_ok &= memcmp(p, want, 4) == 0;
        } else region_ok &= p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255;
    }
    CHECK(region_ok, "DrawTexture writes only its scissored destination rectangle");

    // 3. A destination rectangle given in the other corner order mirrors the
    // source: destination pixel x samples source texel 3 - x/2.
    NVNdrawTextureRegion mirror_dst = {8, 0, 0, 8};
    NVNcommandBuffer cb_mirror;
    NVNF(nvnCommandBufferInitialize)(&cb_mirror, &dev);
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_mirror, &pool, 0x50000, 0x10000);
    NVNF(nvnCommandBufferSetTexturePool)(&cb_mirror, &tpool);
    NVNF(nvnCommandBufferSetSamplerPool)(&cb_mirror, &spool);
    NVNF(nvnCommandBufferBeginRecording)(&cb_mirror);
    NVNF(nvnCommandBufferSetRenderTargets)(&cb_mirror, 1, colors, NULL, NULL, NULL);
    NVNF(nvnCommandBufferBindChannelMaskState)(&cb_mirror, &cfull);
    NVNF(nvnCommandBufferSetScissor)(&cb_mirror, 0, 0, 8, 8);
    NVNF(nvnCommandBufferClearColor)(&cb_mirror, 0, black, 0xf);
    NVNF(nvnCommandBufferDrawTexture)(&cb_mirror, handle, &mirror_dst, &full_src);
    NVNcommandHandle hm = NVNF(nvnCommandBufferEndRecording)(&cb_mirror);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hm);
    NVNF(nvnQueueFinish)(&q);
    mtl_read_texels(&target, 0, 0, &all, px, 8 * 4, 8 * 8 * 4);
    int mirror_ok = 1;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        uint8_t want[4]; src_px(3 - x / 2, y / 2, want);
        mirror_ok &= memcmp(px + (y * 8 + x) * 4, want, 4) == 0;
    }
    CHECK(mirror_ok, "a swapped destination rectangle mirrors the source: red %u at 0,0", px[0]);

    // 4. The channel mask selects which components the draw writes: red only
    // leaves the clear's green, blue and alpha.
    NVNcommandBuffer cb_mask;
    NVNF(nvnCommandBufferInitialize)(&cb_mask, &dev);
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_mask, &pool, 0x58000, 0x10000);
    NVNF(nvnCommandBufferSetTexturePool)(&cb_mask, &tpool);
    NVNF(nvnCommandBufferSetSamplerPool)(&cb_mask, &spool);
    NVNF(nvnCommandBufferBeginRecording)(&cb_mask);
    NVNF(nvnCommandBufferSetRenderTargets)(&cb_mask, 1, colors, NULL, NULL, NULL);
    NVNF(nvnCommandBufferBindChannelMaskState)(&cb_mask, &cmask);
    NVNF(nvnCommandBufferSetScissor)(&cb_mask, 0, 0, 8, 8);
    NVNF(nvnCommandBufferClearColor)(&cb_mask, 0, blue, 0xf);
    NVNF(nvnCommandBufferDrawTexture)(&cb_mask, handle, &full_dst, &full_src);
    NVNcommandHandle hk = NVNF(nvnCommandBufferEndRecording)(&cb_mask);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hk);
    NVNF(nvnQueueFinish)(&q);
    mtl_read_texels(&target, 0, 0, &all, px, 8 * 4, 8 * 8 * 4);
    int mask_ok = 1;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        const uint8_t *p = px + (y * 8 + x) * 4;
        uint8_t want[4]; src_px(x / 2, y / 2, want);
        mask_ok &= p[0] == want[0] && p[1] == 0 && p[2] == 255 && p[3] == 255;
    }
    CHECK(mask_ok, "the channel mask writes red only, keeping the clear's other components: %u %u %u %u",
          px[0], px[1], px[2], px[3]);

    // 5. A source sub-region: the 2x2 texels at (1,1) fill a 4x4 destination
    // rectangle, so destination pixel (x, y) takes source texel (1 + x/2,
    // 1 + y/2) and the source-origin offset shows.
    NVNdrawTextureRegion sub_dst = {0, 0, 4, 4}, sub_src = {1, 1, 3, 3};
    NVNcommandBuffer cb_sub;
    NVNF(nvnCommandBufferInitialize)(&cb_sub, &dev);
    NVNF(nvnCommandBufferAddCommandMemory)(&cb_sub, &pool, 0x60000, 0x10000);
    NVNF(nvnCommandBufferSetTexturePool)(&cb_sub, &tpool);
    NVNF(nvnCommandBufferSetSamplerPool)(&cb_sub, &spool);
    NVNF(nvnCommandBufferBeginRecording)(&cb_sub);
    NVNF(nvnCommandBufferSetRenderTargets)(&cb_sub, 1, colors, NULL, NULL, NULL);
    NVNF(nvnCommandBufferBindChannelMaskState)(&cb_sub, &cfull);
    NVNF(nvnCommandBufferSetScissor)(&cb_sub, 0, 0, 8, 8);
    NVNF(nvnCommandBufferClearColor)(&cb_sub, 0, black, 0xf);
    NVNF(nvnCommandBufferDrawTexture)(&cb_sub, handle, &sub_dst, &sub_src);
    NVNcommandHandle hs = NVNF(nvnCommandBufferEndRecording)(&cb_sub);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &hs);
    NVNF(nvnQueueFinish)(&q);
    mtl_read_texels(&target, 0, 0, &all, px, 8 * 4, 8 * 8 * 4);
    int sub_ok = 1;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        const uint8_t *p = px + (y * 8 + x) * 4;
        if (x < 4 && y < 4) {
            uint8_t want[4]; src_px(1 + x / 2, 1 + y / 2, want);
            sub_ok &= memcmp(p, want, 4) == 0;
        } else sub_ok &= p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255;
    }
    CHECK(sub_ok, "a source sub-region fills its destination rectangle: %u %u %u at 3,3",
          px[(3 * 8 + 3) * 4], px[(3 * 8 + 3) * 4 + 1], px[(3 * 8 + 3) * 4 + 2]);

    printf("\n%s (%d failure%s)\n", g_fail ? "NVN DRAW TEXTURE TEST FAILED" : "NVN DRAW TEXTURE TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
