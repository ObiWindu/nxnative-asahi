// Depth-convention rewrite for NVN vertex-processing stages.
//
// The translator emits SPIR-V with NVN's near=0 convention: gl_Position.z is
// left unchanged. NVN's default depth mode is the GL convention (near = -w),
// where clip z ranges over [-w, w]. This pass injects the same operation the
// Metal host's main0 entry point applies:
//
//     gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5
//
// Vertex and tessellation-evaluation stages consume the position after their
// return, so it is injected before every return. A geometry stage consumes it
// at every EmitVertex, so there it is injected around the emit (the NVN value
// is restored afterwards). It handles the translator's gl_PerVertex block
// (member 0, BuiltIn Position) and a standalone vec4 Position variable; the
// caller chooses the last vertex-processing stage.
#include "nvn_spv.h"

#include <stdlib.h>
#include <string.h>

enum {
    SPV_OP_NAME = 5, SPV_OP_MEMBER_NAME = 6, SPV_OP_ENTRY_POINT = 15,
    SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23,
    SPV_OP_TYPE_POINTER = 32, SPV_OP_CONSTANT = 43, SPV_OP_FUNCTION = 54,
    SPV_OP_FUNCTION_END = 56, SPV_OP_VARIABLE = 59, SPV_OP_LOAD = 61,
    SPV_OP_STORE = 62, SPV_OP_ACCESS_CHAIN = 65, SPV_OP_FMUL = 133,
    SPV_OP_FADD = 129, SPV_OP_EMIT_VERTEX = 218, SPV_OP_EMIT_STREAM_VERTEX = 220,
    SPV_OP_RETURN = 253, SPV_OP_DECORATE = 71,
    SPV_OP_MEMBER_DECORATE = 72,
    SPV_DEC_BLOCK = 2, SPV_DEC_BUILTIN = 11,
    SPV_STORAGE_OUTPUT = 3
};

typedef struct { uint32_t *w; size_t n, cap; } words;

static int push(words *b, const uint32_t *w, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n) cap *= 2;
        uint32_t *p = realloc(b->w, cap * sizeof *p);
        if (!p) return -1;
        b->w = p; b->cap = cap;
    }
    memcpy(b->w + b->n, w, n * sizeof *w); b->n += n;
    return 0;
}
static void op(words *b, uint32_t opcode, const uint32_t *args, size_t n) {
    uint32_t h = ((uint32_t)(n + 1) << 16) | opcode;
    push(b, &h, 1); push(b, args, n);
}

// A parsed type/constant lookup result.
typedef struct {
    uint32_t struct_id, member, vec4_type, float_type, var_id, var_ptr_type;
    uint32_t ptr_vec4, ptr_float, uint_type, c0, c2, c3, c_half;
    uint32_t entry;
    int standalone;
    uint32_t exec_model;   // Vertex, TessellationEvaluation or Geometry
} found;

static uint32_t find_pointer(const uint32_t *w, size_t n, uint32_t storage, uint32_t pointee) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_TYPE_POINTER && wc >= 4 && w[i + 2] == storage && w[i + 3] == pointee) return w[i + 1];
        i += wc;
    }
    return 0;
}
static uint32_t find_constant(const uint32_t *w, size_t n, uint32_t type, uint32_t value) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_CONSTANT && wc >= 4 && w[i + 1] == type && w[i + 3] == value) return w[i + 2];
        i += wc;
    }
    return 0;
}
static uint32_t find_type_int(const uint32_t *w, size_t n, uint32_t width, int sign) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_TYPE_INT && wc >= 4 && w[i + 2] == width && w[i + 3] == (uint32_t)sign) return w[i + 1];
        i += wc;
    }
    return 0;
}
static uint32_t find_type_float(const uint32_t *w, size_t n, uint32_t width) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_TYPE_FLOAT && wc >= 3 && w[i + 2] == width) return w[i + 1];
        i += wc;
    }
    return 0;
}
static uint32_t find_vec4_float(const uint32_t *w, size_t n, uint32_t flt) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_TYPE_VECTOR && wc >= 4 && w[i + 2] == flt && w[i + 3] == 4) return w[i + 1];
        i += wc;
    }
    return 0;
}

static int scan(const uint32_t *w, size_t n, found *f) {
    memset(f, 0, sizeof *f);
    // Entry point of a stage whose output position is consumed after the
    // shader returns (vertex 0, tessellation evaluation 2) or at EmitVertex
    // (geometry 3).
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_ENTRY_POINT && wc >= 4 && (w[i + 1] == 0 || w[i + 1] == 2 || w[i + 1] == 3)) {
            f->entry = w[i + 2]; f->exec_model = w[i + 1];
        }
        i += wc;
    }
    // Struct with member BuiltIn Position, and its member index.
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_MEMBER_DECORATE && wc >= 5 && w[i + 3] == SPV_DEC_BUILTIN && w[i + 4] == 0) {
            f->struct_id = w[i + 1]; f->member = w[i + 2];
            break;
        }
        i += wc;
    }
    f->float_type = find_type_float(w, n, 32);
    f->uint_type = find_type_int(w, n, 32, 0);
    if (!f->float_type) return -1;
    if (f->struct_id) {
        // The struct's first member is the position vec4.
        for (size_t i = 5; i < n;) {
            uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
            if (!wc || i + wc > n) break;
            if (o == 30 && wc >= 3 && w[i + 1] == f->struct_id) {
                uint32_t idx = f->member + 2;
                if (idx < wc) f->vec4_type = w[i + idx];
                break;
            }
            i += wc;
        }
        if (!f->vec4_type) f->vec4_type = find_vec4_float(w, n, f->float_type);
    } else {
        f->vec4_type = find_vec4_float(w, n, f->float_type);
    }
    if (!f->vec4_type) return -1;
    // Output variable: pointer to the struct, or a standalone Position variable.
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_VARIABLE && wc >= 4 && w[i + 3] == SPV_STORAGE_OUTPUT) {
            uint32_t ptr = w[i + 1], pointee = 0;
            for (size_t j = 5; j < n;) {
                uint32_t wc2 = w[j] >> 16, o2 = w[j] & 0xffff;
                if (!wc2 || j + wc2 > n) break;
                if (o2 == SPV_OP_TYPE_POINTER && wc2 >= 4 && w[j + 1] == ptr) { pointee = w[j + 3]; break; }
                j += wc2;
            }
            if (f->struct_id && pointee == f->struct_id) {
                f->var_id = w[i + 2]; f->var_ptr_type = ptr; f->standalone = 0; break;
            }
            if (!f->struct_id && pointee == f->vec4_type) {
                f->var_id = w[i + 2]; f->var_ptr_type = ptr; f->standalone = 1; break;
            }
        }
        i += wc;
    }
    if (!f->var_id) return -1;
    f->ptr_vec4 = find_pointer(w, n, SPV_STORAGE_OUTPUT, f->vec4_type);
    f->ptr_float = find_pointer(w, n, SPV_STORAGE_OUTPUT, f->float_type);
    f->c0 = find_constant(w, n, f->uint_type, 0);
    f->c2 = find_constant(w, n, f->uint_type, 2);
    f->c3 = find_constant(w, n, f->uint_type, 3);
    f->c_half = find_constant(w, n, f->float_type, 0x3f000000u);
    return 0;
}

typedef struct { uint32_t pz, z; } pos_rewrite;

// Emit z' = z/2 + w/2 into the position output. Returns the pointer and the
// original z so a geometry stage can restore them after EmitVertex.
static void emit_position_rewrite(words *out, const found *f, uint32_t *next,
                                  uint32_t ptr_vec4, uint32_t ptr_float, uint32_t float_type,
                                  uint32_t c0, uint32_t c2, uint32_t c3, uint32_t c_half,
                                  pos_rewrite *r) {
    uint32_t pvec = 0, pz, pw, z, wv, zh, wh, zn;
    uint32_t a[8];
    if (!f->standalone) {
        pvec = (*next)++;
        a[0] = ptr_vec4; a[1] = pvec; a[2] = f->var_id; a[3] = c0;
        op(out, SPV_OP_ACCESS_CHAIN, a, 4);
    }
    pz = (*next)++;
    a[0] = ptr_float; a[1] = pz; a[2] = f->standalone ? f->var_id : pvec; a[3] = c2;
    op(out, SPV_OP_ACCESS_CHAIN, a, 4);
    pw = (*next)++;
    a[0] = ptr_float; a[1] = pw; a[2] = f->standalone ? f->var_id : pvec; a[3] = c3;
    op(out, SPV_OP_ACCESS_CHAIN, a, 4);
    z = (*next)++;
    a[0] = float_type; a[1] = z; a[2] = pz;
    op(out, SPV_OP_LOAD, a, 3);
    wv = (*next)++;
    a[0] = float_type; a[1] = wv; a[2] = pw;
    op(out, SPV_OP_LOAD, a, 3);
    zh = (*next)++;
    a[0] = float_type; a[1] = zh; a[2] = z; a[3] = c_half;
    op(out, SPV_OP_FMUL, a, 4);
    wh = (*next)++;
    a[0] = float_type; a[1] = wh; a[2] = wv; a[3] = c_half;
    op(out, SPV_OP_FMUL, a, 4);
    zn = (*next)++;
    a[0] = float_type; a[1] = zn; a[2] = zh; a[3] = wh;
    op(out, SPV_OP_FADD, a, 4);
    a[0] = pz; a[1] = zn;
    op(out, SPV_OP_STORE, a, 2);
    r->pz = pz; r->z = z;
}

int nvn_spv_depth_gl(nvn_shader *s) {
    if (!s || !s->spv || s->spv_words < 6) return -1;
    const uint32_t *w = s->spv; size_t n = s->spv_words;
    if (w[0] != 0x07230203u) return -1;
    found f;
    if (scan(w, n, &f) != 0 || !f.entry) return -1;
    uint32_t next = w[3];
    uint32_t ptr_vec4 = f.ptr_vec4, ptr_float = f.ptr_float;
    uint32_t c0 = f.c0, c2 = f.c2, c3 = f.c3, chalf = f.c_half;
    int need_ptr_vec4 = 0, need_ptr_float = 0;
    if (!ptr_vec4) { ptr_vec4 = next++; need_ptr_vec4 = 1; }
    if (!ptr_float) { ptr_float = next++; need_ptr_float = 1; }
    if (!c0) { c0 = next++; }
    if (!c2) { c2 = next++; }
    if (!c3) { c3 = next++; }
    if (!chalf) { chalf = next++; }
    uint32_t uint_type = f.uint_type ? f.uint_type : next++;
    uint32_t float_type = f.float_type ? f.float_type : next++;
    int need_uint = !f.uint_type, need_float = !f.float_type;

    words out = {0};
    if (push(&out, w, 5) != 0) return -1;
    int emitted_decls = 0, in_entry = 0, injected = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_FUNCTION && !emitted_decls) {
            emitted_decls = 1;
            uint32_t a[5];
            if (need_uint) { a[0] = uint_type; a[1] = 32; a[2] = 0; op(&out, SPV_OP_TYPE_INT, a, 3); }
            if (need_float) { a[0] = float_type; a[1] = 32; op(&out, SPV_OP_TYPE_FLOAT, a, 2); }
            if (!f.c0) { a[0] = uint_type; a[1] = c0; a[2] = 0; op(&out, SPV_OP_CONSTANT, a, 3); }
            if (!f.c2) { a[0] = uint_type; a[1] = c2; a[2] = 2; op(&out, SPV_OP_CONSTANT, a, 3); }
            if (!f.c3) { a[0] = uint_type; a[1] = c3; a[2] = 3; op(&out, SPV_OP_CONSTANT, a, 3); }
            if (!f.c_half) { a[0] = float_type; a[1] = chalf; a[2] = 0x3f000000u; op(&out, SPV_OP_CONSTANT, a, 3); }
            if (need_ptr_vec4) { a[0] = ptr_vec4; a[1] = SPV_STORAGE_OUTPUT; a[2] = f.vec4_type; op(&out, SPV_OP_TYPE_POINTER, a, 3); }
            if (need_ptr_float) { a[0] = ptr_float; a[1] = SPV_STORAGE_OUTPUT; a[2] = float_type; op(&out, SPV_OP_TYPE_POINTER, a, 3); }
        }
        if (o == SPV_OP_FUNCTION) in_entry = (wc >= 3 && w[i + 2] == f.entry);
        if (o == SPV_OP_FUNCTION_END) in_entry = 0;
        int rewrite_here = in_entry &&
            ((f.exec_model == 3) ? (o == SPV_OP_EMIT_VERTEX || o == SPV_OP_EMIT_STREAM_VERTEX)
                                 : (o == SPV_OP_RETURN));
        if (rewrite_here) {
            pos_rewrite r;
            emit_position_rewrite(&out, &f, &next, ptr_vec4, ptr_float, float_type, c0, c2, c3, chalf, &r);
            injected = 1;
            if (f.exec_model == 3) {
                // The position is consumed by EmitVertex, not at return. Put
                // the NVN value back so later code (and a repeated emit) sees
                // what the shader wrote.
                if (push(&out, w + i, wc) != 0) { free(out.w); return -1; }
                uint32_t a[2] = {r.pz, r.z};
                op(&out, SPV_OP_STORE, a, 2);
                i += wc;
                continue;
            }
        }
        if (push(&out, w + i, wc) != 0) { free(out.w); return -1; }
        i += wc;
    }
    if (!injected) { free(out.w); return -1; }
    out.w[3] = next;
    free(s->spv);
    s->spv = out.w;
    s->spv_words = out.n;
    return 0;
}
