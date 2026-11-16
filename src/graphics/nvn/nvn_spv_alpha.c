// Alpha-test injection for NVN pixel shaders.
//
// The translated SPIR-V has no alpha-test entry point (the Metal build cuts a
// separate _bx library). This pass injects the test before every return of the
// fragment entry:
//
//   pass = mask & (1 << (alpha<ref) | (alpha==ref)<<1 | (alpha>ref)<<2)
//   pass ? return : discard
//
// `ref` and `mask` are specialization constants, set from the NVN color state
// and alpha reference at pipeline creation. The mask bits are:
//   bit 0: alpha < ref, bit 1: alpha == ref, bit 2: alpha > ref.
#include "nvn_spv.h"

#include <stdlib.h>
#include <string.h>

enum {
    SPV_OP_ENTRY_POINT = 15, SPV_OP_TYPE_BOOL = 20, SPV_OP_TYPE_INT = 21,
    SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23, SPV_OP_TYPE_POINTER = 32,
    SPV_OP_CONSTANT = 43, SPV_OP_SPEC_CONSTANT = 50, SPV_OP_FUNCTION = 54,
    SPV_OP_FUNCTION_END = 56, SPV_OP_VARIABLE = 59, SPV_OP_LOAD = 61,
    SPV_OP_ACCESS_CHAIN = 65, SPV_OP_DECORATE = 71,
    SPV_OP_SELECT = 169, SPV_OP_INOT_EQUAL = 171, SPV_OP_FORD_EQUAL = 180,
    SPV_OP_FORD_LESS = 184, SPV_OP_FORD_GREATER = 186,
    SPV_OP_IADD = 128, SPV_OP_IMUL = 132,
    SPV_OP_SHIFT_LEFT = 196, SPV_OP_BITWISE_AND = 199,
    SPV_OP_SELECTION_MERGE = 247, SPV_OP_LABEL = 248,
    SPV_OP_BRANCH_CONDITIONAL = 250, SPV_OP_KILL = 252, SPV_OP_RETURN = 253,
    SPV_DEC_SPEC_ID = 1, SPV_DEC_LOCATION = 30,
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
static uint32_t find_ptr(const uint32_t *w, size_t n, uint32_t storage, uint32_t pointee) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_TYPE_POINTER && wc >= 4 && w[i + 2] == storage && w[i + 3] == pointee) return w[i + 1];
        i += wc;
    }
    return 0;
}
static uint32_t find_const(const uint32_t *w, size_t n, uint32_t type, uint32_t value) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_CONSTANT && wc >= 4 && w[i + 1] == type && w[i + 3] == value) return w[i + 2];
        i += wc;
    }
    return 0;
}
static uint32_t find_type(const uint32_t *w, size_t n, uint32_t kind) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == kind) return w[i + 1];
        i += wc;
    }
    return 0;
}
static uint32_t find_float(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_TYPE_FLOAT && wc >= 3 && w[i + 2] == 32) return w[i + 1];
        i += wc;
    }
    return 0;
}
static uint32_t find_uint(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_TYPE_INT && wc >= 4 && w[i + 2] == 32 && w[i + 3] == 0) return w[i + 1];
        i += wc;
    }
    return 0;
}
static uint32_t find_vec4(const uint32_t *w, size_t n, uint32_t flt) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_TYPE_VECTOR && wc >= 4 && w[i + 2] == flt && w[i + 3] == 4) return w[i + 1];
        i += wc;
    }
    return 0;
}
static int spec_id_used(const uint32_t *w, size_t n, uint32_t id) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_DECORATE && wc >= 4 && w[i + 2] == SPV_DEC_SPEC_ID && w[i + 3] == id) return 1;
        i += wc;
    }
    return 0;
}
static uint32_t find_pointee(const uint32_t *w, size_t n, uint32_t ptr) {
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_TYPE_POINTER && wc >= 4 && w[i + 1] == ptr) return w[i + 3];
        i += wc;
    }
    return 0;
}

int nvn_spv_alpha_test(nvn_shader *s) {
    if (!s || !s->spv || s->spv_words < 6 || s->spv[0] != 0x07230203u) return -1;
    const uint32_t *w = s->spv; size_t n = s->spv_words;
    uint32_t entry = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_ENTRY_POINT && wc >= 4 && w[i + 1] == 4) entry = w[i + 2];   // Fragment
        i += wc;
    }
    if (!entry) return -1;
    uint32_t flt = find_float(w, n), uint_t = find_uint(w, n), bool_t = find_type(w, n, SPV_OP_TYPE_BOOL);
    if (!flt) return -1;
    if (!uint_t) uint_t = 0;
    if (!bool_t) bool_t = 0;
    uint32_t vec4 = find_vec4(w, n, flt);
    if (!vec4) return -1;
    // Output variable with Location 0 (not the input at the same location).
    uint32_t var = 0, var_type = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (o == SPV_OP_VARIABLE && wc >= 4 && w[i + 3] == SPV_STORAGE_OUTPUT && find_pointee(w, n, w[i + 1]) == vec4) {
            uint32_t id = w[i + 2];
            for (size_t j = 5; j < n;) {
                uint32_t wc2 = w[j] >> 16, o2 = w[j] & 0xffff;
                if (!wc2 || j + wc2 > n) break;
                if (o2 == SPV_OP_DECORATE && wc2 >= 4 && w[j + 1] == id && w[j + 2] == SPV_DEC_LOCATION && w[j + 3] == 0) { var = id; var_type = w[i + 1]; break; }
                j += wc2;
            }
            if (var) break;
        }
        i += wc;
    }
    if (!var || !var_type) return -1;
    // Types/pointers/constants we need.
    uint32_t next = w[3];
    uint32_t ptr_float = find_ptr(w, n, SPV_STORAGE_OUTPUT, flt);
    if (!ptr_float) ptr_float = next++;
    if (!uint_t) uint_t = next++;
    if (!bool_t) bool_t = next++;
    uint32_t c0 = find_const(w, n, uint_t, 0), c1 = find_const(w, n, uint_t, 1),
             c2 = find_const(w, n, uint_t, 2), c4 = find_const(w, n, uint_t, 4),
             c3 = find_const(w, n, uint_t, 3);
    if (!c0) c0 = next++;
    if (!c1) c1 = next++;
    if (!c2) c2 = next++;
    if (!c3) c3 = next++;
    if (!c4) c4 = next++;
    // SpecIds: two unused values, distinct from each other and from every
    // SpecId the translator already declared.
    uint32_t spec_ref = next++, spec_mask = next++, spec_ref_id = 1000, spec_mask_id = 1001;
    while (spec_id_used(w, n, spec_ref_id)) spec_ref_id++;
    while (spec_mask_id == spec_ref_id || spec_id_used(w, n, spec_mask_id)) spec_mask_id++;
    words out = {0};
    if (push(&out, w, 5) != 0) return -1;
    int decls = 0, decorated = 0, in_entry = 0, injected = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, o = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (!decorated && !decls && o >= 19 && o <= 59) {
            uint32_t a[3];
            a[0] = spec_ref; a[1] = SPV_DEC_SPEC_ID; a[2] = spec_ref_id; op(&out, SPV_OP_DECORATE, a, 3);
            a[0] = spec_mask; a[1] = SPV_DEC_SPEC_ID; a[2] = spec_mask_id; op(&out, SPV_OP_DECORATE, a, 3);
            decorated = 1;
        }
        if (o == SPV_OP_FUNCTION && !decls) {
            decls = 1;
            uint32_t a[6];
            if (!find_type(w, n, SPV_OP_TYPE_BOOL)) { a[0] = bool_t; op(&out, SPV_OP_TYPE_BOOL, a, 1); }
            if (!find_uint(w, n)) { a[0] = uint_t; a[1] = 32; a[2] = 0; op(&out, SPV_OP_TYPE_INT, a, 3); }
            if (!find_ptr(w, n, SPV_STORAGE_OUTPUT, flt)) { a[0] = ptr_float; a[1] = SPV_STORAGE_OUTPUT; a[2] = flt; op(&out, SPV_OP_TYPE_POINTER, a, 3); }
            if (!find_const(w, n, uint_t, 0)) { a[0] = uint_t; a[1] = c0; a[2] = 0; op(&out, SPV_OP_CONSTANT, a, 3); }
            if (!find_const(w, n, uint_t, 1)) { a[0] = uint_t; a[1] = c1; a[2] = 1; op(&out, SPV_OP_CONSTANT, a, 3); }
            if (!find_const(w, n, uint_t, 2)) { a[0] = uint_t; a[1] = c2; a[2] = 2; op(&out, SPV_OP_CONSTANT, a, 3); }
            if (!find_const(w, n, uint_t, 3)) { a[0] = uint_t; a[1] = c3; a[2] = 3; op(&out, SPV_OP_CONSTANT, a, 3); }
            if (!find_const(w, n, uint_t, 4)) { a[0] = uint_t; a[1] = c4; a[2] = 4; op(&out, SPV_OP_CONSTANT, a, 3); }
            a[0] = flt; a[1] = spec_ref; a[2] = 0; op(&out, SPV_OP_SPEC_CONSTANT, a, 3);
            a[0] = uint_t; a[1] = spec_mask; a[2] = 7; op(&out, SPV_OP_SPEC_CONSTANT, a, 3);
        }
        if (o == SPV_OP_FUNCTION) in_entry = (wc >= 3 && w[i + 2] == entry);
        if (o == SPV_OP_FUNCTION_END) in_entry = 0;
        if (o == SPV_OP_RETURN && in_entry) {
            uint32_t pa = next++, alpha = next++, lt = next++, eq = next++, gt = next++;
            uint32_t lti = next++, eqi = next++, gti = next++, eq2 = next++, gt4 = next++;
            uint32_t bits = next++, one = next++, andv = next++, pass = next++;
            uint32_t kill = next++, merge = next++;
            uint32_t a[6];
            a[0] = ptr_float; a[1] = pa; a[2] = var; a[3] = c3;
            op(&out, SPV_OP_ACCESS_CHAIN, a, 4);
            a[0] = flt; a[1] = alpha; a[2] = pa;
            op(&out, SPV_OP_LOAD, a, 3);
            a[0] = bool_t; a[1] = lt; a[2] = alpha; a[3] = spec_ref;
            op(&out, SPV_OP_FORD_LESS, a, 4);
            a[0] = bool_t; a[1] = eq; a[2] = alpha; a[3] = spec_ref;
            op(&out, SPV_OP_FORD_EQUAL, a, 4);
            a[0] = bool_t; a[1] = gt; a[2] = alpha; a[3] = spec_ref;
            op(&out, SPV_OP_FORD_GREATER, a, 4);
            a[0] = uint_t; a[1] = lti; a[2] = lt; a[3] = c1; a[4] = c0;
            op(&out, SPV_OP_SELECT, a, 5);
            a[0] = uint_t; a[1] = eqi; a[2] = eq; a[3] = c1; a[4] = c0;
            op(&out, SPV_OP_SELECT, a, 5);
            a[0] = uint_t; a[1] = gti; a[2] = gt; a[3] = c1; a[4] = c0;
            op(&out, SPV_OP_SELECT, a, 5);
            // Bit position: 0 for <, 1 for ==, 2 for >.
            a[0] = uint_t; a[1] = eq2; a[2] = eqi; a[3] = c1;
            op(&out, SPV_OP_IMUL, a, 4);
            a[0] = uint_t; a[1] = gt4; a[2] = gti; a[3] = c2;
            op(&out, SPV_OP_IMUL, a, 4);
            a[0] = uint_t; a[1] = bits; a[2] = eq2; a[3] = gt4;
            op(&out, SPV_OP_IADD, a, 4);
            a[0] = uint_t; a[1] = one; // c1 used below as the value; placeholder-free
            a[0] = uint_t; a[1] = one; a[2] = c1; a[3] = bits;
            op(&out, SPV_OP_SHIFT_LEFT, a, 4);
            a[0] = uint_t; a[1] = andv; a[2] = one; a[3] = spec_mask;
            op(&out, SPV_OP_BITWISE_AND, a, 4);
            a[0] = bool_t; a[1] = pass; a[2] = andv; a[3] = c0;
            op(&out, SPV_OP_INOT_EQUAL, a, 4);
            a[0] = merge; a[1] = 0;
            op(&out, SPV_OP_SELECTION_MERGE, a, 2);
            a[0] = pass; a[1] = merge; a[2] = kill;
            op(&out, SPV_OP_BRANCH_CONDITIONAL, a, 3);
            a[0] = kill; op(&out, SPV_OP_LABEL, a, 1);
            op(&out, SPV_OP_KILL, NULL, 0);
            a[0] = merge; op(&out, SPV_OP_LABEL, a, 1);
            op(&out, SPV_OP_RETURN, NULL, 0);
            injected = 1;
            i += wc;
            continue;
        }
        if (push(&out, w + i, wc) != 0) { free(out.w); return -1; }
        i += wc;
    }
    if (!injected) { free(out.w); return -1; }
    out.w[3] = next;
    free(s->spv);
    s->spv = out.w; s->spv_words = out.n;
    s->alpha_ref_id = (int)spec_ref_id;
    s->alpha_mask_id = (int)spec_mask_id;
    return 0;
}
