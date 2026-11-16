#pragma once
// Apple's libcompression (LZFSE) is not available on Linux. NVN capture files
// are stored raw; the field walk (cap_rec_fields) is unaffected.
#include <stddef.h>
#include <stdint.h>
#define COMPRESSION_LZFSE 0
static inline size_t compression_encode_buffer(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size, void *scratch, int algorithm) {
    (void)dst; (void)dst_size; (void)src; (void)src_size; (void)scratch; (void)algorithm; return 0;
}
static inline size_t compression_decode_buffer(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size, void *scratch, int algorithm) {
    (void)dst; (void)dst_size; (void)src; (void)src_size; (void)scratch; (void)algorithm; return 0;
}
