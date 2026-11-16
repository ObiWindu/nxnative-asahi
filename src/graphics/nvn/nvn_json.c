#include "nvn_json.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p, *end; nj_arena *a; int depth; } nj_parser;

static void *nj_alloc(nj_arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (a->used + n > a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 65536;
        while (cap < a->used + n) cap *= 2;
        char *m = realloc(a->mem, cap);
        if (!m) return NULL;
        a->mem = m; a->cap = cap;
    }
    void *p = a->mem + a->used; a->used += n; memset(p, 0, n);
    return p;
}
static void skip(nj_parser *s) {
    for (;;) {
        while (s->p < s->end && (unsigned char)*s->p <= ' ') s->p++;
        if (s->p + 1 < s->end && s->p[0] == '/' && s->p[1] == '/') { while (s->p < s->end && *s->p != '\n') s->p++; continue; }
        if (s->p + 1 < s->end && s->p[0] == '/' && s->p[1] == '*') { s->p += 2; while (s->p + 1 < s->end && !(s->p[0] == '*' && s->p[1] == '/')) s->p++; s->p = s->p + 1 < s->end ? s->p + 2 : s->end; continue; }
        break;
    }
}
static char *string(nj_parser *s) {
    if (s->p >= s->end || *s->p != '"') return NULL;
    s->p++;
    const char *start = s->p;
    size_t cap = 32, n = 0;
    char *out = nj_alloc(s->a, cap);
    if (!out) return NULL;
    while (s->p < s->end && *s->p != '"') {
        unsigned char c = (unsigned char)*s->p++;
        if (c == '\\' && s->p < s->end) {
            char e = *s->p++;
            switch (e) {
            case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0; int k = 0;
                while (k < 4 && s->p < s->end) { char h = *s->p++; v <<= 4; v |= h >= '0' && h <= '9' ? (unsigned)(h - '0') : h >= 'a' && h <= 'f' ? (unsigned)(h - 'a' + 10) : h >= 'A' && h <= 'F' ? (unsigned)(h - 'A' + 10) : 0; k++; }
                c = v < 0x80 ? (unsigned char)v : '?';
                break;
            }
            default: c = (unsigned char)e;
            }
        }
        if (n + 1 >= cap) {
            char *m = nj_alloc(s->a, cap * 2);
            if (!m) return NULL;
            memcpy(m, out, n); out = m; cap *= 2;
        }
        out[n++] = (char)c;
    }
    (void)start;
    if (s->p < s->end) s->p++;
    out[n] = 0;
    return out;
}
static nj_val *value(nj_parser *s);
static nj_val *array(nj_parser *s) {
    nj_val *v = nj_alloc(s->a, sizeof *v);
    if (!v) return NULL;
    v->type = NJ_ARRAY;
    s->p++;
    size_t cap = 8;
    v->items = nj_alloc(s->a, cap * sizeof *v->items);
    while (s->p < s->end) {
        skip(s);
        if (s->p < s->end && *s->p == ']') { s->p++; return v; }
        nj_val *e = value(s);
        if (!e) return v;
        if (v->count == cap) {
            nj_val **m = nj_alloc(s->a, cap * 2 * sizeof *m);
            if (!m) return v;
            memcpy(m, v->items, v->count * sizeof *m); v->items = m; cap *= 2;
        }
        v->items[v->count++] = e;
        skip(s);
        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
    }
    return v;
}
static nj_val *object(nj_parser *s) {
    nj_val *v = nj_alloc(s->a, sizeof *v);
    if (!v) return NULL;
    v->type = NJ_OBJECT;
    s->p++;
    size_t cap = 16;
    v->items = nj_alloc(s->a, cap * sizeof *v->items);
    v->keys = nj_alloc(s->a, cap * sizeof *v->keys);
    while (s->p < s->end) {
        skip(s);
        if (s->p < s->end && *s->p == '}') { s->p++; return v; }
        char *k = string(s);
        if (!k) return v;
        skip(s);
        if (s->p < s->end && *s->p == ':') s->p++;
        nj_val *e = value(s);
        if (!e) return v;
        if (v->count == cap) {
            nj_val **mi = nj_alloc(s->a, cap * 2 * sizeof *mi);
            char **mk = nj_alloc(s->a, cap * 2 * sizeof *mk);
            if (!mi || !mk) return v;
            memcpy(mi, v->items, v->count * sizeof *mi); v->items = mi;
            memcpy(mk, v->keys, v->count * sizeof *mk); v->keys = mk;
            cap *= 2;
        }
        v->keys[v->count] = k;
        v->items[v->count++] = e;
        skip(s);
        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
    }
    return v;
}
static nj_val *value(nj_parser *s) {
    skip(s);
    if (s->p >= s->end || s->depth > 64) return NULL;
    if (*s->p == '{') { s->depth++; nj_val *v = object(s); s->depth--; return v; }
    if (*s->p == '[') { s->depth++; nj_val *v = array(s); s->depth--; return v; }
    if (*s->p == '"') {
        nj_val *v = nj_alloc(s->a, sizeof *v);
        if (!v) return NULL;
        v->type = NJ_STRING; v->text = string(s); return v;
    }
    nj_val *v = nj_alloc(s->a, sizeof *v);
    if (!v) return NULL;
    const char *start = s->p;
    if (!strncmp(s->p, "true", 4)) { v->type = NJ_BOOL; v->boolean = 1; s->p += 4; return v; }
    if (!strncmp(s->p, "false", 5)) { v->type = NJ_BOOL; s->p += 5; return v; }
    if (!strncmp(s->p, "null", 4)) { v->type = NJ_NULL; s->p += 4; return v; }
    v->type = NJ_NUMBER;
    while (s->p < s->end && (strchr("+-0123456789.eE", *s->p))) s->p++;
    size_t n = (size_t)(s->p - start);
    char *t = nj_alloc(s->a, n + 1);
    if (!t) return NULL;
    memcpy(t, start, n); t[n] = 0; v->text = t;
    if (s->p == start) s->p++;
    return v;
}
nj_val *nj_parse(const char *text, size_t len, nj_arena *arena) {
    memset(arena, 0, sizeof *arena);
    nj_parser s = {text, text + len, arena, 0};
    nj_val *v = value(&s);
    if (!v) nj_arena_free(arena);
    return v;
}
void nj_arena_free(nj_arena *a) { free(a->mem); a->mem = NULL; a->used = a->cap = 0; }
const nj_val *nj_get(const nj_val *o, const char *key) {
    if (!o || o->type != NJ_OBJECT || !key) return NULL;
    for (size_t i = 0; i < o->count; i++) if (o->keys[i] && !strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}
const nj_val *nj_at(const nj_val *a, size_t i) { return a && a->type == NJ_ARRAY && i < a->count ? a->items[i] : NULL; }
int nj_int(const nj_val *v, int def) {
    if (!v) return def;
    if (v->type == NJ_NUMBER && v->text) return (int)strtol(v->text, NULL, 10);
    if (v->type == NJ_BOOL) return v->boolean;
    return def;
}
const char *nj_str(const nj_val *v, const char *def) { return v && v->type == NJ_STRING && v->text ? v->text : def; }
