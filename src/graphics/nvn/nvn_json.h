// A small JSON reader for shader reflection files (nvn_spv.c). Objects keep
// their keys, arrays their elements; scalars keep a text view. The DOM allocates
// from one arena and is freed with the arena.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum { NJ_NULL, NJ_BOOL, NJ_NUMBER, NJ_STRING, NJ_ARRAY, NJ_OBJECT } nj_type;
typedef struct nj_val nj_val;
struct nj_val {
    nj_type type;
    int boolean;
    char *text;                 // string value (NUL-terminated) or number text
    size_t count;               // array elements / object members
    nj_val **items;
    char **keys;                // object only
};

typedef struct { char *mem; size_t used, cap; } nj_arena;

nj_val *nj_parse(const char *text, size_t len, nj_arena *arena);
void nj_arena_free(nj_arena *arena);
const nj_val *nj_get(const nj_val *obj, const char *key);
const nj_val *nj_at(const nj_val *arr, size_t i);
int nj_int(const nj_val *v, int def);
const char *nj_str(const nj_val *v, const char *def);
