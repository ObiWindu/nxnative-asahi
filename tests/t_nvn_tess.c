// NVN -> Vulkan tessellation test: vertex, hull (tess control), domain (tess
// evaluation) and pixel stages from the shader cache. The domain shader emits
// a centre triangle; the vertex shader only covers a corner, so the centre is
// red only when tessellation ran.
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

static void *lib;
static int compile(const char *src, int kind, const char *name, uint8_t **out, size_t *n) {
    void *(*ci)(void) = (void *)dlsym(lib, "shaderc_compiler_initialize");
    void *(*oi)(void) = (void *)dlsym(lib, "shaderc_compile_options_initialize");
    void *(*co)(void *, const char *, size_t, int, const char *, const char *, void *) = (void *)dlsym(lib, "shaderc_compile_into_spv");
    const uint32_t *(*by)(void *) = (void *)dlsym(lib, "shaderc_result_get_bytes");
    size_t (*le)(void *) = (void *)dlsym(lib, "shaderc_result_get_length");
    int (*st)(void *) = (void *)dlsym(lib, "shaderc_result_get_compilation_status");
    if (!ci || !oi || !co || !by || !le || !st) return -1;
    void *r = co(ci(), src, strlen(src), kind, name, "main", oi());
    if (!r || st(r) != 0) return -1;
    *n = le(r); *out = malloc(*n); if (!*out) return -1;
    memcpy(*out, by(r), *n); return 0;
}
static int put(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    int ok = fwrite(p, 1, n, f) == n; fclose(f); return ok ? 0 : -1;
}
static int put_shader(const char *dir, const char *key, const char *stage, const char *entry, const uint8_t *spv, size_t n) {
    char path[512]; snprintf(path, sizeof path, "%s/%s_b.spv", dir, key);
    if (put(path, spv, n)) return -1;
    snprintf(path, sizeof path, "%s/%s_b.json", dir, key);
    char json[256]; snprintf(json, sizeof json, "{\"stage\":\"%s\",\"entry\":\"%s\",\"bindings\":[],\"vertex_inputs\":[]}\n", stage, entry);
    return put(path, json, strlen(json));
}
int main(void) {
    lib = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("SKIP: libshaderc is not installed\n"); return 0; }
    char dir[] = "/tmp/opencode/nvn-tess-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("NX_NVN_SHADER_CACHE", dir, 1);
    const char *vs_src =
        "#version 450\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(location=0) out vec2 uv;\n"
        "void main(){ uv=pos; gl_Position=vec4(pos,0,1); }\n";
    const char *tcs_src =
        "#version 450\n"
        "layout(vertices=3) out;\n"
        "layout(location=0) in vec2 uv[];\n"
        "layout(location=0) out vec2 uvo[];\n"
        "void main(){ uvo[gl_InvocationID]=uv[gl_InvocationID];\n"
        "  gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;\n"
        "  if(gl_InvocationID==0){ gl_TessLevelOuter[0]=1; gl_TessLevelOuter[1]=1; gl_TessLevelOuter[2]=1; gl_TessLevelInner[0]=1; } }\n";
    const char *tes_src =
        "#version 450\n"
        "layout(triangles, equal_spacing, ccw) in;\n"
        "layout(location=0) in vec2 uvo[];\n"
        "layout(location=0) out vec2 uvf;\n"
        "void main(){ vec3 b=gl_TessCoord; vec2 p=vec2(-0.5,-0.5)*b.x+vec2(0.5,-0.5)*b.y+vec2(0,0.5)*b.z;\n"
        "  gl_Position=vec4(p,0,1); uvf=p; }\n";
    const char *fs_src =
        "#version 450\n"
        "layout(location=0) in vec2 uvf;\n"
        "layout(location=0) out vec4 o;\n"
        "void main(){ o=vec4(1,0,0,1); }\n";
    uint8_t *vsp=NULL,*tp=NULL,*ep=NULL,*fsp=NULL; size_t vsn=0,tn=0,en=0,fsn=0;
    if (compile(vs_src, 0, "t.vert", &vsp, &vsn) || compile(tcs_src, 4, "t.tesc", &tp, &tn) ||
        compile(tes_src, 5, "t.tese", &ep, &en) || compile(fs_src, 1, "t.frag", &fsp, &fsn)) {
        printf("FAIL: shaderc compile\n"); return 1;
    }
    const char *keys[4] = {"5626d9a8f1d7364b", "b92a56c3b25472de", "4764cc257d24a328", "f38e17e3ce81e218"};
    const char *stages[4] = {"vertex", "hull", "domain", "pixel"};
    const uint8_t *spvs[4] = {vsp, tp, ep, fsp}; const size_t ns[4] = {vsn, tn, en, fsn};
    for (int i = 0; i < 4; i++) if (put_shader(dir, keys[i], stages[i], "main", spvs[i], ns[i])) { printf("FAIL: write stage %d\n", i); return 1; }

    int g_fail = 0;
    NVNdeviceBuilder db; NVNdevice dev;
    NVNF(nvnDeviceBuilderSetDefaults)(&db); NVNF(nvnDeviceBuilderSetFlags)(&db, 0);
    CHECK(NVNF(nvnDeviceInitialize)(&dev, &db) == 1, "device initializes");
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    static uint8_t storage[1 << 20] __attribute__((aligned(4096)));
    NVNF(nvnMemoryPoolBuilderSetDefaults)(&pb); NVNF(nvnMemoryPoolBuilderSetDevice)(&pb, &dev);
    NVNF(nvnMemoryPoolBuilderSetStorage)(&pb, storage, sizeof storage);
    CHECK(NVNF(nvnMemoryPoolInitialize)(&pool, &pb) == 1, "memory pool initializes");
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
    CHECK(NVNF(nvnTextureInitialize)(&tex, &tb) == 1, "render target initializes");
    static uint8_t ctl[4][0x880];
    for (int i = 0; i < 4; i++) ctl[i][0] = (uint8_t)i;
    NVNprogram prog;
    CHECK(NVNF(nvnProgramInitialize)(&prog, &dev) == 1, "program initializes");
    NVNshaderData sd[4] = {{0, ctl[0]}, {0, ctl[1]}, {0, ctl[2]}, {0, ctl[3]}};
    CHECK(NVNF(nvnProgramSetShaders)(&prog, 4, sd) == 1, "program takes four stages");
    float *verts = (float *)(storage + 0x8000);
    verts[0] = -0.9f; verts[1] = -0.9f; verts[2] = -0.7f; verts[3] = -0.9f; verts[4] = -0.8f; verts[5] = -0.7f;
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
    NVNF(nvnCommandBufferSetPatchSize)(&cb, 3);
    NVNF(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_PATCHES, 0, 3);
    NVNcommandHandle h = NVNF(nvnCommandBufferEndRecording)(&cb);
    NVNF(nvnQueueSubmitCommands)(&q, 1, &h);
    NVNF(nvnQueueFinish)(&q);
    uint8_t px[64 * 64 * 4];
    NVNcopyRegion all = {0, 0, 0, 64, 64, 1};
    extern void mtl_read_texels(const void *, unsigned, unsigned, const NVNcopyRegion *, uint8_t *, size_t, size_t);
    mtl_read_texels(&tex, 0, 0, &all, px, 64 * 4, 64 * 4);
    const uint8_t *corner = px, *mid = px + (32 * 64 + 32) * 4;
    CHECK(corner[2] > 200 && corner[0] < 40, "clear reached the corner: %u %u %u", corner[0], corner[1], corner[2]);
    CHECK(mid[0] > 200 && mid[1] < 40 && mid[2] < 40, "the tessellated triangle reached the centre: %u %u %u", mid[0], mid[1], mid[2]);
    printf("\n%s (%d failure%s)\n", g_fail ? "NVN TESSELLATION TEST FAILED" : "NVN TESSELLATION TEST PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
