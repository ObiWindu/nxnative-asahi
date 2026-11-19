// Linux SPIR-V shader cache: translated NVN shaders built by dm/gfx/tools/
// dmsh_build.py, loadable directly as Vulkan modules. The offline build writes
// <key>_b.spv beside <key>_b.json (the replay reflection); this loads both.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    int arg_id;                 // MSL argument-buffer id (matches the SPIR-V binding only when no support buffer shifts it)
    char msl_name[40];          // the MSL argument's name, which the SPIR-V variable carries too
    char kind[24];              // DMSH_KIND_* ("cbuf", "tex", "samp", "sbuf", ...)
    int reg;                    // NVN slot or bank the kind uses
    int count;                  // array length, 0 when not an array
    char msl_type[64];
    int elem;                   // tex_pool / tex_htab element type
    int bank;                   // drvcb: 0 or 2
    int off, soff, sreg;        // bindless / buffer-array fields
    int tbx;                    // buffer image / texture offset table entry, 0 none
    int rw;                     // storage buffer written by the shader
    int raw_id;                 // a sampler ID word, not a handle
    int fixed;                  // cbuf bytes read at fixed offsets
} nvn_refl_bind;

typedef struct {
    uint32_t binding, set;
    VkDescriptorType type;
    uint32_t count;             // 1 for non-arrays, 0 for runtime arrays
    int refl;                   // index into rb[], -1 when the JSON has no entry
    char name[40];              // the SPIR-V variable name (route B names it after its MSL argument)
} nvn_desc_bind;

typedef struct {
    char key[17];
    char stage[16];
    char entry[32];
    uint32_t *spv;
    size_t spv_words;
    nvn_desc_bind desc[64];
    int ndesc;
    nvn_refl_bind rb[96];
    int nrb;
    uint8_t *c1;
    size_t c1_len;
    struct { int location; char type[16]; } vin[16];
    int nvin;
    int pos_attr, discard, color_mask;
    int push_size;
    int alpha_ref_id, alpha_mask_id;   // specialization constants injected by nvn_spv_alpha.c
    char load_error[160];
} nvn_shader;

// Load one shader from `dir`; key is 16 hex digits. Returns 0 on success.
int nvn_shader_load(const char *dir, const char *key, nvn_shader *out);
// Load a geometry shader's <key>_bg<topology> variant (0 points, 1 lines,
// 2 lines with adjacency, 3 triangles, 4 triangles with adjacency).
int nvn_shader_load_geo(const char *dir, const char *key, int topology, nvn_shader *out);
// Move every descriptor binding by `add` (used to give each program stage a
// disjoint binding range in the one set the linked Vulkan pipeline has).
void nvn_spv_rebind(nvn_shader *s, uint32_t add);
// Apply the GL depth convention (z' = z/2 + w/2) at every return of the
// shader's vertex-stage entry point. Returns 0 when the shader was rewritten,
// -1 when there is no position output to rewrite.
int nvn_spv_depth_gl(nvn_shader *s);
// Inject a fragment alpha test with specialization constants for the reference
// and comparison mask. Returns 0 when the shader was rewritten.
int nvn_spv_alpha_test(nvn_shader *s);
void nvn_shader_free(nvn_shader *s);
void nvn_shader_key_hex(const uint8_t key[8], char out[17]);
const nvn_refl_bind *nvn_shader_bind(const nvn_shader *s, uint32_t binding);
