// Shader-cache loader test: compile a vertex/fragment pair with shaderc at test
// time, write it in dmsh_build's output shape (<key>_b.spv + <key>_b.json), then
// load it through the same code the NVN replay uses.
#include "nvn_spv.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef void *shaderc_compiler_t;
typedef void *shaderc_compile_options_t;
typedef void *shaderc_compilation_result_t;
typedef struct { void *p; } shaderc_shim;

int main(void) {
    void *lib = dlopen("libshaderc_shared.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("SKIP: libshaderc is not installed\n"); return 0; }
    shaderc_compiler_t (*compiler_init)(void) = (void *)dlsym(lib, "shaderc_compiler_initialize");
    shaderc_compile_options_t (*opts_init)(void) = (void *)dlsym(lib, "shaderc_compile_options_initialize");
    shaderc_compilation_result_t (*compile)(shaderc_compiler_t, const char *, size_t, int, const char *, const char *, shaderc_compile_options_t) = (void *)dlsym(lib, "shaderc_compile_into_spv");
    const uint32_t *(*bytes)(shaderc_compilation_result_t) = (void *)dlsym(lib, "shaderc_result_get_bytes");
    size_t (*length)(shaderc_compilation_result_t) = (void *)dlsym(lib, "shaderc_result_get_length");
    int (*status)(shaderc_compilation_result_t) = (void *)dlsym(lib, "shaderc_result_get_compilation_status");
    const char *(*errmsg)(shaderc_compilation_result_t) = (void *)dlsym(lib, "shaderc_result_get_error_message");
    if (!compiler_init || !opts_init || !compile || !bytes || !length || !status) { printf("SKIP: shaderc API incomplete\n"); return 0; }
    shaderc_compiler_t c = compiler_init();
    shaderc_compile_options_t o = opts_init();
    const char *vs_src =
        "#version 450\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(set=0,binding=1) uniform UBO { vec4 tint; } ubo;\n"
        "layout(location=0) out vec2 uv;\n"
        "void main(){ uv=pos; gl_Position=vec4(pos,0,1); }\n";
    const char *fs_src =
        "#version 450\n"
        "layout(location=0) in vec2 uv;\n"
        "layout(set=0,binding=2) uniform sampler2D tex;\n"
        "layout(set=0,binding=3) uniform CB { vec4 color; } cb;\n"
        "layout(location=0) out vec4 outColor;\n"
        "void main(){ outColor=texture(tex,uv)*cb.color; }\n";
    shaderc_compilation_result_t rv = compile(c, vs_src, strlen(vs_src), 0, "t.vert", "main", o);   // shaderc_vertex_shader
    shaderc_compilation_result_t rf = compile(c, fs_src, strlen(fs_src), 1, "t.frag", "main", o);   // shaderc_fragment_shader
    if (!rv || status(rv) != 0 || !rf || status(rf) != 0) {
        printf("FAIL: shaderc: %s\n", (rf && status(rf)) ? errmsg(rf) : (rv ? errmsg(rv) : "no result"));
        return 1;
    }
    char dir[] = "/tmp/opencode/nvn-shader-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    const char *key = "0123456789abcdef";
    char path[512];
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, key);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(bytes(rv), 1, length(rv), f) != length(rv)) { printf("FAIL: write vertex SPIR-V\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_b.json", dir, key);
    f = fopen(path, "w");
    if (!f) { printf("FAIL: write json\n"); return 1; }
    fprintf(f, "{\"stage\":\"vertex\",\"entry\":\"main0\",\"c1\":\"0102ff\",\"bindings\":["
               "{\"arg_id\":1,\"kind\":\"cbuf\",\"reg\":7,\"msl_type\":\"constant cb7*\"},"
               "{\"arg_id\":2,\"kind\":\"tex\",\"reg\":3,\"msl_type\":\"texture2d<float>\"},"
               "{\"arg_id\":3,\"kind\":\"cbuf\",\"reg\":8}],"
               "\"vertex_inputs\":[{\"location\":0,\"msl_type\":\"float2\"}],"
               "\"raster\":{\"colors\":[0]}}\n");
    fclose(f);
    nvn_shader vs;
    if (nvn_shader_load(dir, key, &vs) != 0) { printf("FAIL: load vertex shader: %s\n", vs.load_error); return 1; }
    if (strcmp(vs.entry, "main")) { printf("FAIL: the pipeline entry is '%s', not the SPIR-V entry 'main'\n", vs.entry); return 1; }
    if (strcmp(vs.stage, "vertex") || vs.ndesc != 1 || vs.desc[0].type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
        printf("FAIL: vertex reflection: stage '%s', %d descriptors, first type %d\n", vs.stage, vs.ndesc, vs.ndesc ? (int)vs.desc[0].type : -1);
        return 1;
    }
    const nvn_refl_bind *b = nvn_shader_bind(&vs, 1);
    if (!b || strcmp(b->kind, "cbuf") || b->reg != 7) { printf("FAIL: vertex binding 1 is not cbuf 7\n"); return 1; }
    if (vs.c1_len != 3 || vs.c1[0] != 1 || vs.c1[1] != 2 || vs.c1[2] != 0xff) { printf("FAIL: c1 is not 0102ff\n"); return 1; }
    if (vs.nvin != 1 || vs.vin[0].location != 0 || strcmp(vs.vin[0].type, "float2")) { printf("FAIL: vertex input reflection\n"); return 1; }
    if (vs.color_mask != 1) { printf("FAIL: color mask %d\n", vs.color_mask); return 1; }
    nvn_shader_free(&vs);

    snprintf(path, sizeof path, "%s/%s_b.spv", dir, key);
    f = fopen(path, "wb");
    if (!f || fwrite(bytes(rf), 1, length(rf), f) != length(rf)) { printf("FAIL: write fragment SPIR-V\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s_b.json", dir, key);
    f = fopen(path, "w");
    fprintf(f, "{\"stage\":\"pixel\",\"bindings\":["
               "{\"arg_id\":2,\"kind\":\"tex\",\"reg\":4},"
               "{\"arg_id\":3,\"kind\":\"cbuf\",\"reg\":2}],\"vertex_inputs\":[]}\n");
    fclose(f);
    nvn_shader fs;
    if (nvn_shader_load(dir, key, &fs) != 0) { printf("FAIL: load fragment shader: %s\n", fs.load_error); return 1; }
    if (strcmp(fs.stage, "pixel") || fs.ndesc != 2) { printf("FAIL: fragment reflection: stage '%s', %d descriptors\n", fs.stage, fs.ndesc); return 1; }
    int saw_tex = 0, saw_cb = 0;
    for (int i = 0; i < fs.ndesc; i++) {
        if (fs.desc[i].binding == 2 && fs.desc[i].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) saw_tex = 1;
        if (fs.desc[i].binding == 3 && fs.desc[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) saw_cb = 1;
    }
    if (!saw_tex || !saw_cb) { printf("FAIL: fragment descriptor types (tex %d, cb %d)\n", saw_tex, saw_cb); return 1; }
    nvn_shader_free(&fs);

    // Keep the distinction between a fixed descriptor array and an unsized
    // runtime array. The NVN replay can lay out the former in Vulkan, but does
    // not yet populate array elements; runtime arrays additionally require
    // descriptor-indexing features and layout flags. In particular, zero
    // must not be rewritten to one by the layout path.
    const char *array_src =
        "#version 450\n"
        "layout(set=0,binding=4) uniform sampler2D tex[2];\n"
        "layout(location=0) out vec4 outColor;\n"
        "void main(){ outColor=texture(tex[1],vec2(0.5)); }\n";
    const char *runtime_src =
        "#version 450\n"
        "#extension GL_EXT_nonuniform_qualifier : require\n"
        "layout(set=0,binding=5) uniform sampler2D tex[];\n"
        "layout(location=0) out vec4 outColor;\n"
        "void main(){ outColor=texture(tex[nonuniformEXT(0)],vec2(0.5)); }\n";
    shaderc_compilation_result_t fa = compile(c, array_src, strlen(array_src), 1, "t.array.frag", "main", o);
    shaderc_compilation_result_t fr = compile(c, runtime_src, strlen(runtime_src), 1, "t.runtime.frag", "main", o);
    if (!fa || status(fa) != 0 || !fr || status(fr) != 0) {
        printf("FAIL: shaderc descriptor-array compile: %s\n", (fr && status(fr)) ? errmsg(fr) : (fa ? errmsg(fa) : "no result"));
        return 1;
    }
    const char *array_key = "1111111111111111", *runtime_key = "2222222222222222";
    const char *keys[] = {array_key, runtime_key};
    shaderc_compilation_result_t results[] = {fa, fr};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/%s_b.spv", dir, keys[i]);
        f = fopen(path, "wb");
        if (!f || fwrite(bytes(results[i]), 1, length(results[i]), f) != length(results[i])) { printf("FAIL: write descriptor-array SPIR-V\n"); return 1; }
        fclose(f);
        snprintf(path, sizeof path, "%s/%s_b.json", dir, keys[i]);
        f = fopen(path, "w");
        if (!f) { printf("FAIL: write descriptor-array JSON\n"); return 1; }
        fprintf(f, "{\"stage\":\"pixel\",\"bindings\":[{\"arg_id\":1,\"kind\":\"tex\",\"reg\":4}]}\n");
        fclose(f);
    }
    nvn_shader arr;
    if (nvn_shader_load(dir, array_key, &arr) != 0 || arr.ndesc != 1 || arr.desc[0].count != 2) {
        printf("FAIL: fixed descriptor array reflection (count %u)\n", arr.ndesc ? arr.desc[0].count : 0);
        return 1;
    }
    nvn_shader_free(&arr);
    nvn_shader runtime;
    if (nvn_shader_load(dir, runtime_key, &runtime) != 0 || runtime.ndesc != 1 || runtime.desc[0].count != 0) {
        printf("FAIL: runtime descriptor array reflection (count %u)\n", runtime.ndesc ? runtime.desc[0].count : 1);
        return 1;
    }
    nvn_shader_free(&runtime);

    // Cache files can be interrupted or damaged. Reject malformed instruction
    // streams and IDs outside the module's declared bound before reflection
    // indexes any parser tables.
    snprintf(path, sizeof path, "%s/%s_b.spv", dir, key);
    size_t spv_size = length(rv);
    uint32_t *bad_spv = malloc(spv_size + sizeof(uint32_t));
    if (!bad_spv) { printf("FAIL: allocate malformed SPIR-V fixture\n"); return 1; }
    memcpy(bad_spv, bytes(rv), spv_size);
    bad_spv[3] = 1; // The module contains IDs outside this declared bound.
    f = fopen(path, "wb");
    int wrote = f && fwrite(bad_spv, 1, spv_size, f) == spv_size;
    if (f) fclose(f);
    nvn_shader malformed;
    if (!wrote || nvn_shader_load(dir, key, &malformed) == 0) {
        free(bad_spv);
        printf("FAIL: out-of-bound SPIR-V IDs were accepted\n");
        return 1;
    }
    memcpy(bad_spv, bytes(rv), spv_size);
    bad_spv[spv_size / sizeof(uint32_t)] = 1; // zero-word-count trailing instruction
    f = fopen(path, "wb");
    wrote = f && fwrite(bad_spv, 1, spv_size + sizeof(uint32_t), f) == spv_size + sizeof(uint32_t);
    if (f) fclose(f);
    free(bad_spv);
    if (!wrote || nvn_shader_load(dir, key, &malformed) == 0) {
        printf("FAIL: malformed SPIR-V instruction stream was accepted\n");
        return 1;
    }
    printf("SHADER CACHE TEST PASSED\n");
    return 0;
}
