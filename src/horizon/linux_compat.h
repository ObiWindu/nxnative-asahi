// Compatibility shims for the proven host sources. Only facilities that have a
// direct Linux equivalent live here; Apple frameworks are replaced by real
// platform modules, never by success-returning stubs.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/prctl.h>

#ifndef __APPLE__
#define TARGET_OS_IPHONE 0
#define TARGET_OS_OSX 0
#define TARGET_OS_MAC 0
#endif

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef __APPLE__
#define st_mtimespec st_mtim
#endif

#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_UPTIME_RAW_APPROX
#define CLOCK_UPTIME_RAW_APPROX CLOCK_MONOTONIC
#endif

static inline uint64_t clock_gettime_nsec_np(clockid_t id) {
    struct timespec ts;
    if (clock_gettime(id, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void sys_icache_invalidate(void *addr, size_t len) {
    __builtin___clear_cache((char *)addr, (char *)addr + len);
}

// Linux names the thread it is called on; the host sources use the Darwin
// one-argument form.
#define pthread_setname_np(name) prctl(PR_SET_NAME, (name), 0, 0, 0)


static inline int nx_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += rel->tv_sec;
    abs.tv_nsec += rel->tv_nsec;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &abs);
}
#define pthread_cond_timedwait_relative_np(c, m, rel) nx_cond_timedwait_relative_np((c), (m), (rel))

// Stable sort used by the HID layer. glibc has qsort but not mergesort.
int mergesort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *));

// CommonCrypto SHA-256 replacement used by the loader, save sync and RO layer.
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} NxSha256Ctx;
void nx_sha256_init(NxSha256Ctx *ctx);
void nx_sha256_update(NxSha256Ctx *ctx, const void *data, size_t len);
void nx_sha256_final(NxSha256Ctx *ctx, uint8_t out[32]);
void nx_sha256(const void *data, size_t len, uint8_t out[32]);

// fcntl(F_GETPATH) replacement.
int nx_fd_path(int fd, char *out, size_t n);

// Protection of the mapping containing addr, from /proc/self/maps. Returns 0
// with *prot as PROT_* bits and *shared set for a shared mapping, or -1.
int nx_range_prot(uintptr_t addr, int *prot, int *shared);

static inline uint64_t nx_rotl64(uint64_t x, unsigned n) { return (x << n) | (x >> (64 - n)); }

// Save transactions: recursive clone (copy-on-write per file where the
// filesystem supports FICLONE) and the atomic directory swap used by Commit.
#define RENAME_SWAP 0x2
int clonefile(const char *src, const char *dst, int flags);
int renamex_np(const char *from, const char *to, unsigned flags);
