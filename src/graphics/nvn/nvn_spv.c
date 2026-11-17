// SPIR-V shader-cache loader and descriptor reflection.
//
// dmsh_build.py already produces the only shader form this port needs: real
// Vulkan SPIR-V translated from the game's NVN shaders by dmsh_xlate. This file
// reads the module, walks its binding decorations to build the Vulkan
// descriptor set layout, and joins each binding with the replay reflection JSON
// (which names the NVN slot and kind the descriptor comes from).
#include "nvn_spv.h"
#include "nvn_json.h"
#include "platform/linux/nx_platform.h"

#include <stdlib.h>
#include <string.h>

// ---- SPIR-V constants (only the ones used here) ----
enum {
    SPV_OP_ENTRY_POINT = 15, SPV_OP_TYPE_IMAGE = 25, SPV_OP_TYPE_SAMPLER = 26,
    SPV_OP_TYPE_SAMPLED_IMAGE = 27, SPV_OP_TYPE_ARRAY = 28, SPV_OP_TYPE_RUNTIME_ARRAY = 29,
    SPV_OP_TYPE_STRUCT = 30, SPV_OP_TYPE_POINTER = 32, SPV_OP_CONSTANT = 43,
    SPV_OP_VARIABLE = 59, SPV_OP_DECORATE = 71,
    SPV_DEC_BLOCK = 2, SPV_DEC_BUFFER_BLOCK = 3, SPV_DEC_BINDING = 33, SPV_DEC_DESCRIPTOR_SET = 34,
    SPV_STORAGE_UNIFORM_CONSTANT = 0, SPV_STORAGE_UNIFORM = 2, SPV_STORAGE_PUSH_CONSTANT = 9,
    SPV_STORAGE_STORAGE_BUFFER = 12
};

typedef struct {
    int kind;
    uint32_t elem;
    uint32_t count;
    uint32_t sampled, dim;
    int block;
} spv_type;

typedef struct {
    uint32_t *words; size_t n;
    uint32_t bound;
    uint32_t *type_of;      // type id -> kind (0 unknown)
    spv_type *types;
    int32_t *binding, *set, *location;
    uint8_t *block;          // struct id is a Block (1) / BufferBlock (2)
    int32_t *array_len;      // array type id -> length type id
} spv_mod;

static void spv_free(spv_mod *m);

static int spv_id_valid(const spv_mod *m, uint32_t id) {
    return id > 0 && id < m->bound;
}

static int spv_parse(const uint32_t *w, size_t n, spv_mod *m) {
    memset(m, 0, sizeof *m);
    if (n < 5 || w[0] != 0x07230203u || (w[1] >> 16) != 0x0001u || w[4] != 0) return -1;
    m->words = (uint32_t *)w; m->n = n; m->bound = w[3];
    if (m->bound < 1 || m->bound > 200000) return -1;
    m->type_of = calloc(m->bound, sizeof *m->type_of);
    m->types = calloc(m->bound, sizeof *m->types);
    m->binding = malloc(m->bound * sizeof *m->binding);
    m->set = malloc(m->bound * sizeof *m->set);
    m->location = malloc(m->bound * sizeof *m->location);
    m->block = calloc(m->bound, 1);   // Block = 1, BufferBlock = 2
    m->array_len = malloc(m->bound * sizeof *m->array_len);
    if (!m->type_of || !m->types || !m->binding || !m->set || !m->location || !m->block || !m->array_len) {
        spv_free(m);
        return -1;
    }
    for (uint32_t i = 0; i < m->bound; i++) { m->binding[i] = -1; m->set[i] = -1; m->location[i] = -1; m->array_len[i] = -1; }
    size_t i = 5;
    while (i < n) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) { spv_free(m); return -1; }
        const uint32_t *o = w + i + 1;
        uint32_t nops = wc - 1;
        switch (op) {
        case SPV_OP_TYPE_IMAGE:
            if (nops >= 8) {
                if (!spv_id_valid(m, o[0])) { spv_free(m); return -1; }
                m->type_of[o[0]] = SPV_OP_TYPE_IMAGE; m->types[o[0]].sampled = o[6]; m->types[o[0]].dim = o[2];
            } else { spv_free(m); return -1; }
            break;
        case SPV_OP_TYPE_SAMPLER:
            if (nops >= 1) {
                if (!spv_id_valid(m, o[0])) { spv_free(m); return -1; }
                m->type_of[o[0]] = SPV_OP_TYPE_SAMPLER;
            } else { spv_free(m); return -1; }
            break;
        case SPV_OP_TYPE_SAMPLED_IMAGE:
            if (nops >= 2) {
                if (!spv_id_valid(m, o[0]) || !spv_id_valid(m, o[1])) { spv_free(m); return -1; }
                m->type_of[o[0]] = SPV_OP_TYPE_SAMPLED_IMAGE; m->types[o[0]].elem = o[1];
            } else { spv_free(m); return -1; }
            break;
        case SPV_OP_TYPE_ARRAY:
            if (nops >= 3) {
                if (!spv_id_valid(m, o[0]) || !spv_id_valid(m, o[1]) || !spv_id_valid(m, o[2])) { spv_free(m); return -1; }
                m->type_of[o[0]] = SPV_OP_TYPE_ARRAY; m->types[o[0]].elem = o[1]; m->array_len[o[0]] = (int32_t)o[2];
            } else { spv_free(m); return -1; }
            break;
        case SPV_OP_TYPE_RUNTIME_ARRAY:
            if (nops >= 2) {
                if (!spv_id_valid(m, o[0]) || !spv_id_valid(m, o[1])) { spv_free(m); return -1; }
                m->type_of[o[0]] = SPV_OP_TYPE_RUNTIME_ARRAY; m->types[o[0]].elem = o[1];
            } else { spv_free(m); return -1; }
            break;
        case SPV_OP_TYPE_STRUCT:
            if (nops >= 1) {
                if (!spv_id_valid(m, o[0])) { spv_free(m); return -1; }
                for (uint32_t j = 1; j < nops; j++) if (!spv_id_valid(m, o[j])) { spv_free(m); return -1; }
                m->type_of[o[0]] = SPV_OP_TYPE_STRUCT;
            } else { spv_free(m); return -1; }
            break;
        case SPV_OP_TYPE_POINTER:
            if (nops < 3 || !spv_id_valid(m, o[0]) || !spv_id_valid(m, o[2])) { spv_free(m); return -1; }
            break;
        case SPV_OP_CONSTANT:
            if (nops < 3 || !spv_id_valid(m, o[0]) || !spv_id_valid(m, o[1])) { spv_free(m); return -1; }
            break;
        case SPV_OP_VARIABLE:
            if (nops < 3 || !spv_id_valid(m, o[0]) || !spv_id_valid(m, o[1])) { spv_free(m); return -1; }
            break;
        case SPV_OP_DECORATE:
            if (nops >= 2) {
                uint32_t id = o[0], dec = o[1];
                if (!spv_id_valid(m, id)) { spv_free(m); return -1; }
                if (dec == SPV_DEC_BINDING && nops >= 3) m->binding[id] = (int32_t)o[2];
                else if (dec == SPV_DEC_DESCRIPTOR_SET && nops >= 3) m->set[id] = (int32_t)o[2];
                else if (dec == SPV_DEC_BLOCK) m->block[id] = 1;
                else if (dec == SPV_DEC_BUFFER_BLOCK) m->block[id] = 2;
                else if (dec == 30 && nops >= 3) m->location[id] = (int32_t)o[2];   // Location
            }
            break;
        default: break;
        }
        i += wc;
    }
    return 0;
}

static void spv_free(spv_mod *m) {
    free(m->type_of); free(m->types); free(m->binding); free(m->set); free(m->location); free(m->block); free(m->array_len);
    memset(m, 0, sizeof *m);
}

static uint32_t spv_const_value(const spv_mod *m, uint32_t type_id) {
    if (!spv_id_valid(m, type_id)) return 0;
    size_t i = 5;
    while (i + 3 < m->n) {
        uint32_t wc = m->words[i] >> 16, op = m->words[i] & 0xffff;
        if (!wc || i + wc > m->n) break;
        if (op == SPV_OP_CONSTANT && wc >= 4 && m->words[i + 2] == type_id) return m->words[i + 3];
        i += wc;
    }
    return 0;
}

// Resolve a pointee type into a Vulkan descriptor type and array length.
static VkDescriptorType spv_descriptor(const spv_mod *m, uint32_t type_id, uint32_t *count, int storage_class) {
    *count = 1;
    for (int depth = 0; depth < 8 && type_id && type_id < m->bound; depth++) {
        uint32_t k = m->type_of[type_id];
        if (k == SPV_OP_TYPE_ARRAY || k == SPV_OP_TYPE_RUNTIME_ARRAY) {
            if (k == SPV_OP_TYPE_RUNTIME_ARRAY) *count = 0;
            else {
                uint32_t len = m->array_len[type_id] >= 0 ? spv_const_value(m, (uint32_t)m->array_len[type_id]) : 0;
                if (!len || (*count && *count > UINT32_MAX / len)) return VK_DESCRIPTOR_TYPE_MAX_ENUM;
                *count *= len;
            }
            type_id = m->types[type_id].elem;
            continue;
        }
        if (k == SPV_OP_TYPE_SAMPLED_IMAGE) {
            uint32_t img = m->types[type_id].elem;
            if (img < m->bound && m->type_of[img] == SPV_OP_TYPE_IMAGE && m->types[img].dim == 5)
                return m->types[img].sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        if (k == SPV_OP_TYPE_SAMPLER) return VK_DESCRIPTOR_TYPE_SAMPLER;
        if (k == SPV_OP_TYPE_IMAGE) {
            if (m->types[type_id].dim == 5) return m->types[type_id].sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            return m->types[type_id].sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        if (k == SPV_OP_TYPE_STRUCT) {
            if (storage_class == SPV_STORAGE_STORAGE_BUFFER) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (storage_class == SPV_STORAGE_UNIFORM)
                return m->block[type_id] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
        return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

static int spv_reflect(const spv_mod *m, nvn_desc_bind *out, int max) {
    int n = 0;
    size_t i = 5;
    while (i < m->n) {
        uint32_t wc = m->words[i] >> 16, op = m->words[i] & 0xffff;
        if (!wc || i + wc > m->n) break;
        if (op == SPV_OP_VARIABLE && wc >= 4) {
            uint32_t type_id = m->words[i + 1], id = m->words[i + 2], sc = m->words[i + 3];
            if (!spv_id_valid(m, type_id) || !spv_id_valid(m, id)) return n;
            int32_t binding = m->binding[id], set = m->set[id];
            if (binding >= 0 && (sc == SPV_STORAGE_UNIFORM_CONSTANT || sc == SPV_STORAGE_UNIFORM || sc == SPV_STORAGE_STORAGE_BUFFER)) {
                // type_id is a pointer: its pointee is the variable's type
                uint32_t pointee = (type_id < m->bound && m->type_of[type_id] == SPV_OP_TYPE_POINTER) ? 0 : 0;
                // The pointer's pointee is word 3 of OpTypePointer; index it from the instruction table.
                size_t j = 5;
                while (j < m->n) {
                    uint32_t w2 = m->words[j] >> 16, o2 = m->words[j] & 0xffff;
                    if (!w2 || j + w2 > m->n) break;
                    if (o2 == SPV_OP_TYPE_POINTER && w2 >= 4 && m->words[j + 1] == type_id) { pointee = m->words[j + 3]; break; }
                    j += w2;
                }
                uint32_t count = 1;
                VkDescriptorType dt = spv_descriptor(m, pointee, &count, sc);
                if (dt != VK_DESCRIPTOR_TYPE_MAX_ENUM && n < max) {
                    out[n++] = (nvn_desc_bind){(uint32_t)binding, set >= 0 ? (uint32_t)set : 0, dt, count, -1};
                }
            }
        }
        i += wc;
    }
    return n;
}

// The module's own entry point name (SPIR-V OpEntryPoint), which is what
// vkCreateShaderModule/pipeline needs. The JSON's "entry" is the Metal name
// (main0) and must not be used here.
static void spv_entry_name(const spv_mod *m, char *out, size_t n) {
    out[0] = 0;
    size_t i = 5;
    while (i < m->n) {
        uint32_t wc = m->words[i] >> 16, op = m->words[i] & 0xffff;
        if (!wc || i + wc > m->n) break;
        if (op == SPV_OP_ENTRY_POINT && wc >= 4) {
            const char *name = (const char *)&m->words[i + 3];
            size_t max = (size_t)(wc - 3) * 4;
            size_t len = strnlen(name, max);
            if (len && len < n) { memcpy(out, name, len); out[len] = 0; return; }
        }
        i += wc;
    }
}

static int spv_push_size(const spv_mod *m) {
    size_t i = 5;
    while (i < m->n) {
        uint32_t wc = m->words[i] >> 16, op = m->words[i] & 0xffff;
        if (!wc || i + wc > m->n) break;
        if (op == SPV_OP_VARIABLE && wc >= 4 && m->words[i + 3] == SPV_STORAGE_PUSH_CONSTANT) {
            uint32_t type_id = m->words[i + 1];
            size_t j = 5;
            while (j < m->n) {
                uint32_t w2 = m->words[j] >> 16, o2 = m->words[j] & 0xffff;
                if (!w2 || j + w2 > m->n) break;
                if (o2 == SPV_OP_TYPE_POINTER && w2 >= 4 && m->words[j + 1] == type_id) {
                    uint32_t pointee = m->words[j + 3];
                    // The translated NVN shaders reserve one 256-byte block.
                    (void)pointee;
                    return 256;
                }
                j += w2;
            }
        }
        i += wc;
    }
    return 0;
}

void nvn_shader_key_hex(const uint8_t key[8], char out[17]) {
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", key[i]);
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)sz);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return p;
}

static int shader_load_impl(const char *dir, const char *key, int geo, nvn_shader *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->key, sizeof out->key, "%s", key);
    snprintf(out->entry, sizeof out->entry, "main");
    out->pos_attr = -1;
    if (!dir || !key) { snprintf(out->load_error, sizeof out->load_error, "no shader cache directory"); return -1; }
    char path[1200], suffix[16] = "_b";
    size_t n = 0;
    uint8_t *spv = NULL;
    if (geo >= 0) {
        // A geometry shader has one SPIR-V module per input topology, named as
        // its JSON is (dmsh_build's geometry_finish).
        snprintf(suffix, sizeof suffix, "_bg%d", geo);
        snprintf(path, sizeof path, "%s/%s%s.spv", dir, key, suffix);
        spv = read_file(path, &n);
    } else {
        snprintf(path, sizeof path, "%s/%s_b.spv", dir, key);
        spv = read_file(path, &n);
        if (!spv) {
            snprintf(path, sizeof path, "%s/%s.spv", dir, key);
            spv = read_file(path, &n);
        }
        if (!spv) {
            // A geometry shader's base variant is <key>_bg0; the JSON has the
            // same suffix. Draws pick the topology-specific variant later.
            snprintf(path, sizeof path, "%s/%s_bg0.spv", dir, key);
            spv = read_file(path, &n);
            if (spv) snprintf(suffix, sizeof suffix, "%s", "_bg0");
        }
    }
    if (!spv || n < 20 || (n % 4)) {
        free(spv);
        snprintf(out->load_error, sizeof out->load_error, "%s%s.spv is missing in %s", key, suffix, dir);
        return -1;
    }
    out->spv = (uint32_t *)spv;
    out->spv_words = n / 4;
    spv_mod m;
    if (spv_parse(out->spv, out->spv_words, &m) != 0) {
        snprintf(out->load_error, sizeof out->load_error, "%s_b.spv is not SPIR-V", key);
        nvn_shader_free(out);
        return -1;
    }
    out->ndesc = spv_reflect(&m, out->desc, (int)(sizeof out->desc / sizeof *out->desc));
    out->push_size = spv_push_size(&m);
    spv_entry_name(&m, out->entry, sizeof out->entry);
    spv_free(&m);
    if (!out->entry[0]) snprintf(out->entry, sizeof out->entry, "main");

    snprintf(path, sizeof path, "%s/%s%s.json", dir, key, suffix);
    size_t jn = 0;
    uint8_t *jt = read_file(path, &jn);
    if (jt) {
        nj_arena arena;
        nj_val *root = nj_parse((const char *)jt, jn, &arena);
        if (root) {
            snprintf(out->stage, sizeof out->stage, "%s", nj_str(nj_get(root, "stage"), ""));
            // The JSON "entry" is the Metal name (main0); the Vulkan entry point
            // is the SPIR-V OpEntryPoint read above and is not overwritten.
            const nj_val *c1 = nj_get(root, "c1");
            if (c1 && c1->type == NJ_STRING && c1->text) {
                size_t hn = strlen(c1->text);
                if (!(hn & 1) && hn / 2 <= 0xffff) {
                    out->c1_len = hn / 2;
                    out->c1 = malloc(out->c1_len ? out->c1_len : 1);
                    if (out->c1) for (size_t i = 0; i < out->c1_len; i++) {
                        char b[3] = {c1->text[2 * i], c1->text[2 * i + 1], 0};
                        out->c1[i] = (uint8_t)strtoul(b, NULL, 16);
                    }
                }
            }
            const nj_val *bs = nj_get(root, "bindings");
            if (bs && bs->type == NJ_ARRAY) {
                for (size_t i = 0; i < bs->count && out->nrb < (int)(sizeof out->rb / sizeof *out->rb); i++) {
                    const nj_val *b = nj_at(bs, i);
                    nvn_refl_bind *r = &out->rb[out->nrb];
                    r->arg_id = nj_int(nj_get(b, "arg_id"), -1);
                    snprintf(r->kind, sizeof r->kind, "%s", nj_str(nj_get(b, "kind"), "?"));
                    r->reg = nj_int(nj_get(b, "reg"), -1);
                    r->count = nj_int(nj_get(b, "count"), 0);
                    snprintf(r->msl_type, sizeof r->msl_type, "%s", nj_str(nj_get(b, "msl_type"), ""));
                    r->elem = nj_int(nj_get(b, "elem"), 0);
                    r->bank = nj_int(nj_get(b, "bank"), 0);
                    r->off = nj_int(nj_get(b, "off"), 0);
                    r->soff = nj_int(nj_get(b, "soff"), -1);
                    const nj_val *sr = nj_get(b, "sreg");
                    r->sreg = (sr && sr->type == NJ_STRING && !strcmp(nj_str(sr, ""), "c2")) ? 2 :
                              (sr && sr->type == NJ_STRING && !strcmp(nj_str(sr, ""), "id")) ? -2 : nj_int(sr, -1);
                    r->tbx = nj_int(nj_get(b, "tbx"), 0);
                    r->rw = nj_int(nj_get(b, "rw"), 0);
                    r->raw_id = nj_int(nj_get(b, "raw_id"), 0);
                    r->fixed = nj_int(nj_get(b, "fixed"), 0);
                    out->nrb++;
                }
            }
            const nj_val *vs = nj_get(root, "vertex_inputs");
            if (vs && vs->type == NJ_ARRAY) {
                for (size_t i = 0; i < vs->count && out->nvin < (int)(sizeof out->vin / sizeof *out->vin); i++) {
                    const nj_val *v = nj_at(vs, i);
                    out->vin[out->nvin].location = nj_int(nj_get(v, "location"), -1);
                    snprintf(out->vin[out->nvin].type, sizeof out->vin[out->nvin].type, "%s", nj_str(nj_get(v, "msl_type"), ""));
                    if (out->vin[out->nvin].location >= 0) out->nvin++;
                }
            }
            const nj_val *ra = nj_get(root, "raster");
            if (ra) {
                out->pos_attr = nj_int(nj_get(ra, "pos_attr"), -1);
                out->discard = nj_int(nj_get(ra, "discard"), 0);
                const nj_val *cols = nj_get(ra, "colors");
                if (cols && cols->type == NJ_ARRAY) for (size_t i = 0; i < cols->count; i++) {
                    int c = nj_int(nj_at(cols, i), -1);
                    if (c >= 0 && c < 32) out->color_mask |= 1 << c;
                }
            }
        }
        nj_arena_free(&arena);
        free(jt);
    }
    for (int i = 0; i < out->ndesc; i++)
        for (int j = 0; j < out->nrb; j++)
            if (out->rb[j].arg_id == (int)out->desc[i].binding) { out->desc[i].refl = j; break; }
    return 0;
}

int nvn_shader_load(const char *dir, const char *key, nvn_shader *out) {
    return shader_load_impl(dir, key, -1, out);
}

int nvn_shader_load_geo(const char *dir, const char *key, int topology, nvn_shader *out) {
    if (topology < 0 || topology > 4) { memset(out, 0, sizeof *out); snprintf(out->load_error, sizeof out->load_error, "geometry topology %d", topology); return -1; }
    return shader_load_impl(dir, key, topology, out);
}

void nvn_spv_rebind(nvn_shader *s, uint32_t add) {
    if (!s || !add || !s->spv) return;
    size_t i = 5;
    while (i < s->spv_words) {
        uint32_t wc = s->spv[i] >> 16, op = s->spv[i] & 0xffff;
        if (!wc || i + wc > s->spv_words) break;
        if (op == SPV_OP_DECORATE && wc >= 4 && s->spv[i + 2] == SPV_DEC_BINDING)
            s->spv[i + 3] += add;
        i += wc;
    }
    for (int k = 0; k < s->ndesc; k++) s->desc[k].binding += add;
}

void nvn_shader_free(nvn_shader *s) {
    if (!s) return;
    free(s->spv); free(s->c1);
    memset(s, 0, sizeof *s);
}

const nvn_refl_bind *nvn_shader_bind(const nvn_shader *s, uint32_t binding) {
    if (!s) return NULL;
    for (int i = 0; i < s->ndesc; i++) if (s->desc[i].binding == binding) return s->desc[i].refl >= 0 ? &s->rb[s->desc[i].refl] : NULL;
    return NULL;
}
